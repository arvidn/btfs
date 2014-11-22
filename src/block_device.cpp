/*

Copyright (c) 2014, Arvid Norberg
All rights reserved.

btfs Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

For details, see LICENSE

*/

#include "block_device.hpp"
#include "pool_allocator.hpp"

#include "libtorrent/io.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/lazy_entry.hpp"
#include "libtorrent/file_storage.hpp"
#include "libtorrent/allocator.hpp" // for page_aligned_allocator
#include "libtorrent/storage_defs.hpp" // for storage_params
#include "libtorrent/torrent_info.hpp" // for torrent_info

#include <boost/tuple/tuple.hpp>
#include <boost/bind.hpp>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

using boost::system::generic_category;
using libtorrent::bufs_size;
using libtorrent::aligned_holder;

#if defined TORRENT_WINDOWS
#include "windows.h"
#include "winioctl.h"
#endif

#ifdef TORRENT_BSD
#include <sys/ioctl.h>
#include <sys/disk.h>
#endif

#ifdef TORRENT_LINUX
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

#define DISK_ACCESS_LOG 1

#if DISK_ACCESS_LOG
#include "libtorrent/time.hpp"
#include "libtorrent/thread.hpp"
#include <boost/atomic.hpp>
using libtorrent::ptime;
using libtorrent::min_time;
using libtorrent::time_now_hires;
using libtorrent::total_microseconds;
using boost::atomic;
#endif

using libtorrent::file;
using libtorrent::file_storage;
using libtorrent::lazy_entry;
using libtorrent::entry;


// TODO: write unit test for block device

/*
	the block device implement an extremely simple (and bittorrent
	taylored) filesystem, with the following layout:

	The first 4 MiB block is the root block.
	The root block primarily consists of an array of
	torrent entries. The header looks like this:
*/

// this is in block 0
struct fs_root_block_t
{
	// the string 'BitTorrent filesystem\0\0\0'
	boost::uint8_t fs_identifier[24];

	// the block size for the filesystem. Should
	// be in the range of 1 MiB to 8 MiB. inode
	// extents cannot be chained, so smaller
	// blocks means smaller max file size
	// default is 4 MiB. The block size MUST
	// be an even multiple of the underlying device's
	// block size (which typically is 512 bytes)
	boost::uint32_t block_size;

	// the number of files, i.e. the number of
	// items in the files array
	boost::uint32_t num_files;

	// reserved for future extensions and to
	// pad the header to an even 512 byte block
	boost::uint8_t reserved[480];

	// this is an array of inode pointers.
	sub_block_ref files[];
};

/*
	inode indices are sequence numbers of 4 MiB blocks on the device.
	They refer to a block which contains the mapping of offsets in
	the file. Each inode has an inode header and a block map. The
	inode header is:
*/

// on-disk representation of an inode
struct inode_block_t
{
	// the string 'inod'
	boost::uint8_t inode_identifier[4];

	// info-hash for this file
	sha1_hash info_hash;

	// the size of the block_map
	boost::uint32_t num_blocks;
	
	// self reference. Contains size
	sub_block_ref inode_ref;

	// reserved for future use
	boost::uint8_t reserved[28];

	// array of num_blocks. Mapping file blocks
	// to device blocks. an entry with 'unallocated_block'
	// (i.e. -1) is not allocated on the device. Reading
	// such block should result in zeroes, writing to
	// it requires allocating a new block
	boost::uint32_t block_map[];
};

/*
	immediately following the inode header, is the block allocation
	map. This is an array of 32 bit unsigned block indices. The length
	of the array is `num_blocks` as specified in the inode header.

	Each entry represents 4 MiB of the space in the file and the index
	refers to a block where this payload is stored. If this range does
	not have any data, the block index is 0, which means unallocated.
*/

// this is the in-memory representation of an inode
struct inode_block
{
	inode_block(sha1_hash const& ih, sub_block_ref in)
		: info_hash(ih)
		, block_index(in)
		, dirty(0)
		, marked_for_deletion(0)
		, references(0)
		, blocks_in_use(0)
		, max_size((in.node_size() - sizeof(inode_block_t)) / 4)
	{}

	sha1_hash info_hash;

	// this mutex must be held when manipulating the inode_block
	mutex inode_mutex;

	// the block this inode is stored in
	// inode blocks are special. The low bits
	// indicate a sub block of the filesystem block
	sub_block_ref block_index;
	
	// set to true if the block map holds information
	// that has not been flushed to disk
	boost::uint32_t dirty:1;

	// if this is set, the inode and all its blocks are returned
	// to the global freelist once the reference count reach 0
	boost::uint32_t marked_for_deletion:1;

	// the number of times this inode has been opened
	// but not yet closed
	boost::uint32_t references:30;

	// the number of allocated blocks in this file.
	// this determines how much disk space it uses
	boost::uint32_t blocks_in_use;

	// the max number of blocks this inode can hold.
	// this is initialized when the file is created
	boost::uint32_t max_size;

	// the block map, mapping file blocks to device blocks
	std::vector<boost::uint32_t> block_map;
};

// ============= block_device  ==================

#if DISK_ACCESS_LOG
enum access_log_flags
{
	start_read, start_write, complete_read, complete_write
};

int write_disk_log(FILE* f, boost::uint64_t offset, int id, int event, ptime timestamp)
{
	static atomic<int> global_id(0);

	if (event == start_read || event == start_write)
		id = ++global_id;

	// the event format in the log is:
	// uint64_t timestamp (microseconds)
	// uint64_t file offset
	// uint32_t event-id
	// uint8_t  event (0: start read, 1: start write, 2: complete read, 4: complete write)
	char buf[21];
	char* ptr = buf;

	using namespace libtorrent::detail;

	write_uint64(total_microseconds((timestamp - min_time())), ptr);
	write_uint64(offset, ptr);
	write_uint32(id, ptr);
	write_uint8(event, ptr);

	int ret = fwrite(buf, 1, sizeof(buf), f);
	if (ret != sizeof(buf))
	{
		fprintf(stderr, "ERROR writing to disk access log: (%d) %s\n"
			, errno, strerror(errno));
	}
	return id;
}
#endif

block_device::block_device(std::string const& device_path, error_code& ec)
	: m_dirty_root_block(false)
{
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	m_destructed = false;
#endif
#if DISK_ACCESS_LOG
	m_access_log = NULL;
#endif
	m_file.open(device_path, file::read_write | file::no_cache | file::direct_io, ec);
	if (ec) return;

	// TODO: this should be properly queried
	// assume 512 for windows for now
	m_media_block_size = 512;

#if defined TORRENT_WINDOWS

	PARTITION_INFORMATION pi;
	DISK_GEOMETRY gi;
	DWORD retbytes;
	LARGE_INTEGER size;

	if (DeviceIoControl(m_file.native_handle()
		, IOCTL_DISK_GET_PARTITION_INFO
		, &pi, sizeof(PARTITION_INFORMATION)
		, &pi, sizeof(PARTITION_INFORMATION)
		, &size, NULL))
	{
		m_max_size = pi.PartitionLength.QuadPart;
	}
	else if (DeviceIoControl(m_file.native_handle(), IOCTL_DISK_GET_DRIVE_GEOMETRY
		, &gi, sizeof(DISK_GEOMETRY)
		, &gi, sizeof(DISK_GEOMETRY)
		, &size, NULL))
	{
		m_max_size = gi.BytesPerSector *
			gi.SectorsPerTrack *
			gi.TracksPerCylinder *
			gi.Cylinders.QuadPart;
	}
	else if (GetFileSizeEx(m_file.native_handle(), &size))
	{
		m_max_size = size.QuadPart / blocksize;
	}
	else
	{
		ec.assign(GetLastError(), generic_category());
		return;
	}

#else // TORRENT_WINDOWS
	
#if defined DKIOCGETBLOCKCOUNT

	boost::uint64_t block_count = 0;
	int block_size = 0;
	if (ioctl(m_file.native_handle(), DKIOCGETBLOCKCOUNT, (char*)&block_count) >= 0)
	{
		if (ioctl(m_file.native_handle(), DKIOCGETBLOCKSIZE, (char*)&block_size) < 0)
		{
			ec.assign(errno, generic_category());
			return;
		}

		m_max_size = block_count * block_size;
		m_media_block_size = block_size;
	}
	else if (errno == ENOTTY)
	{
		// it appears to be a regular file
		struct stat st;
		if (fstat(m_file.native_handle(), &st) < 0)
		{
			ec.assign(errno, generic_category());
			return;
		}
		m_max_size = st.st_size;
	}
	else
	{
		ec.assign(errno, generic_category());
		return;
	}

#elif defined BLKGETSIZE64

	if (ioctl(m_file.native_handle(), BLKGETSIZE64, &m_max_size) < 0)
	{
		if (errno == ENOTTY)
		{
			// it appears to be a regular file
			struct stat st;
			if (fstat(m_file.native_handle(), &st) < 0)
			{
				ec.assign(errno, generic_category());
				return;
			}
			m_max_size = st.st_size;
		}
		else
		{
			ec.assign(errno, generic_category());
			return;
		}
	}

#elif defined BLKGETSIZE

	long num_blocks;
	if (ioctl(m_file.native_handle(), BLKGETSIZE, &num_blocks) < 0)
	{
		if (errno == ENOTTY)
		{
			// it appears to be a regular file
			struct stat st;
			if (fstat(m_file.native_handle(), &st) < 0)
			{
				ec.assign(errno, generic_category());
				return;
			}
			m_max_size = st.st_size;
		}
		else
		{
			ec.assign(errno, generic_category());
			return;
		}
	}

#else
	// TODO: in this case, just support flat files
#error do not know how to query the size of a block device!
#endif

#endif // TORRENT_WINDOWS

#if DISK_ACCESS_LOG
	m_start_time = time_now_hires();
	m_access_log = fopen("block_device_access.log", "w+");
#endif

}

block_device::~block_device()
{
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	TORRENT_ASSERT(!m_destructed);
	m_destructed = true;
#endif
#if DISK_ACCESS_LOG
	if (m_access_log) fclose(m_access_log);
#endif
	flush_inodes();
	flush_root_block();
}

void block_device::read_root_block(error_code& ec)
{
	TORRENT_ASSERT(!m_destructed);
	aligned_holder root_block(m_media_block_size);

	file::iovec_t b = { root_block.get(), m_media_block_size };
	int read = m_file.readv(0, &b, 1, ec);

	if (ec) return;

	if (read != m_media_block_size)
	{
		ec.assign(boost::system::errc::io_error, generic_category());
		return;
	}

	fs_root_block_t* root = (fs_root_block_t*)root_block.get();

	if (memcmp(root->fs_identifier, "BitTorrent filesystem\0\0\0", 24) != 0)
	{
		// this is not a BitTorrent filesystem
		ec.assign(boost::system::errc::no_such_file_or_directory, generic_category());
		return;
	}

	m_block_size = root->block_size;
	// verify that the block size is reasonable
	if ((m_block_size % m_media_block_size) != 0 || m_block_size < 4096)
	{
		// invalid block size
		ec.assign(boost::system::errc::no_such_file_or_directory, generic_category());
		return;
	}

	for (int i = 0; i < sizeof(m_node_allocator)/sizeof(m_node_allocator[0]); ++i)
		m_node_allocator[i].init(m_block_size / (1024 << i), i);

	boost::uint32_t max_blocks = m_max_size / m_block_size;

	int num_files = root->num_files;

	if (num_files > (m_block_size - sizeof(fs_root_block_t)) / sizeof(sub_block_ref))
	{
		ec.assign(boost::system::errc::no_such_file_or_directory, generic_category());
		return;
	}

	int block_size = num_files * sizeof(sub_block_ref);
	// round up to an even multiple of media block size
	block_size = (block_size + m_media_block_size-1) & ~(m_media_block_size-1);
	TORRENT_ASSERT(block_size % m_media_block_size == 0);
	std::vector<sub_block_ref> file_list(block_size / sizeof(sub_block_ref));

	TORRENT_ASSERT((file_list.size()*sizeof(sub_block_ref)) % m_media_block_size == 0);

	b.iov_base = &file_list[0];
	b.iov_len = file_list.size() * sizeof(sub_block_ref);
	read = m_file.readv(sizeof(fs_root_block_t), &b, 1, ec);
	if (ec) return;
	if (read != file_list.size() * sizeof(sub_block_ref))
	{
		ec.assign(boost::system::errc::io_error, generic_category());
		return;
	}

	file_list.resize(num_files);

	// read inodes in increasing order to minimize seeking
	std::sort(file_list.begin(), file_list.end());

	// when reading the directory, also collect all blocks
	// used by files, in order to build the freelist
	bitfield used_blocks(max_blocks, false);

	// the root block is in use
	used_blocks.set_bit(0);

	for (std::vector<sub_block_ref>::iterator i = file_list.begin()
		, end(file_list.end()); i != end; ++i)
	{
		read_inode(used_blocks, *i, ec);
		if (ec) return;
	}

	m_free_blocks.init(max_blocks, used_blocks);
}

void block_device::read_inode(bitfield& used_blocks, sub_block_ref iblock, error_code& ec)
{
	std::vector<boost::uint64_t> inode_buffer;
	int inode_size = iblock.node_size();
	inode_buffer.resize(inode_size / 8);
	file::iovec_t b = { &inode_buffer[0], inode_buffer.size() * 8};
	int read = m_file.readv(iblock.device_offset(m_block_size), &b, 1, ec);

	if (ec) return;

	if (read != inode_buffer.size() * 8)
	{
		ec.assign(boost::system::errc::io_error, generic_category());
		return;
	}

	inode_block_t* inode = (inode_block_t*)&inode_buffer[0];

	// if this is not an inode, just ignore it
	TORRENT_ASSERT(memcmp(inode->inode_identifier, "inod", 4) == 0);
	if (memcmp(inode->inode_identifier, "inod", 4) != 0)
		return;

	if (iblock.subblock_size >= sizeof(m_node_allocator)/sizeof(m_node_allocator[0]))
	{
		// invalid inode size
		TORRENT_ASSERT(false);
		return;
	}

	if ((m_block_size >> iblock.subblock_size) < 1024)
	{
		// invalid inode size
		TORRENT_ASSERT(false);
		return;
	}

	// this inode block is in use
	used_blocks.set_bit(iblock.block);

	m_node_allocator[iblock.subblock_size].mark_in_use(iblock);

	inode_block* blk = new (std::nothrow) inode_block(inode->info_hash, iblock);
	if (blk == 0)
	{
		ec.assign(boost::system::errc::not_enough_memory, generic_category());
		return;
	}
	m_inodes.insert(std::make_pair(inode->info_hash, blk));
	m_dirty_root_block = true;

	blk->block_map.resize(inode->num_blocks, block_allocator::unallocated_block);

	// first, copy all the blocks we read. We may have to read more from disk
	// in case this is a big file
	int min_blocks = (std::min)(inode->num_blocks
		, boost::uint32_t((inode_buffer.size()*8 - sizeof(inode_block_t)) / 4));
	std::copy(inode->block_map, inode->block_map + min_blocks, &blk->block_map[0]);
	int blocks_in_use = 0;
	for (int i = 0; i < min_blocks; ++i)
	{
		boost::uint32_t data_block = inode->block_map[i];
		if (data_block == block_allocator::unallocated_block) continue;
		TORRENT_ASSERT(!used_blocks[data_block]);
		used_blocks.set_bit(data_block);
		++blocks_in_use;
	}

	blk->blocks_in_use = blocks_in_use;

	if (inode->num_blocks > min_blocks)
	{
		// ok, the first 64 kiB that we read did not cover the whole
		// block list. This file is pretty big (> 64 GB)
		// read the rest of the block list
		TORRENT_ASSERT(false && "not implemented");
	}
}

void block_device::flush_inodes()
{
	mutex::scoped_lock l(m_dirty_blocks_mutex);
	std::vector<inode_block*> dirty_blocks;
	m_dirty_blocks.swap(dirty_blocks);
	l.unlock();

	std::sort(dirty_blocks.begin(), dirty_blocks.end()
		, boost::bind(&inode_block::block_index, _1)
		< boost::bind(&inode_block::block_index, _2));

	std::vector<boost::uint32_t> buffer;

	for (std::vector<inode_block*>::iterator i = dirty_blocks.begin()
		, end(dirty_blocks.end()); i != end; ++i)
	{
		inode_block* blk = *i;
		mutex::scoped_lock l2(blk->inode_mutex);
		blk->dirty = false;
		int block_size = sizeof(inode_block_t) + blk->block_map.size() * 4;
		// round up to even media_block_size
		block_size = (block_size + m_media_block_size - 1) & ~(m_media_block_size-1);
		buffer.resize(block_size / 4);
		inode_block_t* block = (inode_block_t*)&buffer[0];

		memcpy(block->inode_identifier, "inod", 4);
		block->info_hash = blk->info_hash;
		block->num_blocks = blk->block_map.size();
		block->inode_ref = blk->block_index;

		std::copy(blk->block_map.begin(), blk->block_map.end(), block->block_map);
		l2.unlock();

		size_type dev_offset = blk->block_index.device_offset(m_block_size);
		file::iovec_t b = { &buffer[0], buffer.size()*4 };
		error_code ec;
		m_file.writev(dev_offset, &b, 1, ec);

		// decrement the references, to allow the file
		// to be closed. Whenever the dirty flag was
		// set, the reference count was incremented.
		// calling close here primarily decrements that back
		// down, but also, if we reached zero, checks to
		// see if this node was marked for deletion
		close_impl(blk, l2);
	}
}

void block_device::flush_root_block()
{
	mutex::scoped_lock l(m_inode_mutex);
	if (!m_dirty_root_block) return;

	int num_files = m_inodes.size();

	std::vector<boost::uint32_t> buffer;
	int block_size = sizeof(fs_root_block_t) + num_files * 4;

	// round up to m_media_block_size
	block_size = (block_size + m_media_block_size-1) & ~(m_media_block_size-1);

	buffer.resize(block_size / 4);
	fs_root_block_t* root = (fs_root_block_t*)&buffer[0];
	memcpy(root->fs_identifier, "BitTorrent filesystem\0\0\0", 24);
	root->block_size = m_block_size;
	root->num_files = num_files;

	int file_index = 0;
	for (boost::unordered_map<sha1_hash, inode_block*>::iterator i = m_inodes.begin()
		, end(m_inodes.end()); i != end; ++i, ++file_index)
	{
		root->files[file_index] = i->second->block_index;
	}
	m_dirty_root_block = false;
	l.unlock();

	error_code ec;
	file::iovec_t b = { &buffer[0], buffer.size()*4 };
	m_file.writev(0, &b, 1, ec);
}

void block_device::format(error_code& ec, int block_size)
{
	if (block_size < 4096)
	{
		ec.assign(boost::system::errc::invalid_argument, generic_category());
		return;
	}

	if ((block_size % m_media_block_size) != 0)
	{
		ec.assign(boost::system::errc::invalid_argument, generic_category());
		return;
	}

	aligned_holder root_block((std::max)(boost::uint32_t(4096), m_media_block_size));

	using namespace libtorrent::detail;

	fs_root_block_t* root = (fs_root_block_t*)root_block.get();
	memcpy(root->fs_identifier, "BitTorrent filesystem\0\0\0", 24);

	root->block_size = block_size;
	root->num_files = 0;
	memset(root->reserved, 0, sizeof(root->reserved));

	file::iovec_t b = { root_block.get(), m_media_block_size };
	int written = m_file.writev(0, &b, 1, ec);

	if (ec) return;

	if (written != 512)
	{
		ec.assign(boost::system::errc::io_error, generic_category());
		return;
	}
	m_block_size = block_size;
	boost::uint32_t max_blocks = m_max_size / m_block_size;
	m_free_blocks.init(max_blocks);

	for (int i = 0; i < sizeof(m_node_allocator)/sizeof(m_node_allocator[0]); ++i)
		m_node_allocator[i].init(m_block_size / (1024 << i), i);

	for (boost::unordered_map<sha1_hash, inode_block*>::iterator i = m_inodes.begin()
		, end(m_inodes.end()); i != end; ++i)
	{
		delete i->second;
	}
	m_inodes.clear();
}

// returns true if there is an entry for this info-hash
bool block_device::exists(sha1_hash const& info_hash) const
{
	mutex::scoped_lock l(m_inode_mutex);
	return m_inodes.find(info_hash) != m_inodes.end();
}

// returns a file handle
void* block_device::open(sha1_hash const& info_hash, boost::uint64_t max_size, error_code& ec)
{
	mutex::scoped_lock l(m_inode_mutex);
	boost::unordered_map<sha1_hash, inode_block*>::iterator i = m_inodes.find(info_hash);

	if (i == m_inodes.end())
	{
		// create a new file
		sub_block_ref inode_block_index = allocate_inode(max_size, ec);
		if (inode_block_index == sub_block_ref::invalid)
			return NULL;

		inode_block* blk = new (std::nothrow) inode_block(info_hash, inode_block_index);
		if (blk == NULL)
		{
			// if this allocation fails, we need to return the block we just
			// allocated for it as well
			free_inode(inode_block_index);
			ec.assign(boost::system::errc::not_enough_memory, generic_category());
			return NULL;
		}
		bool inserted;
		boost::tie(i, inserted) = m_inodes.insert(std::make_pair(info_hash, blk));
		TORRENT_ASSERT(inserted);
		TORRENT_ASSERT(i->second == blk);

		m_dirty_root_block = true;

		// schedule this inode for flushing
		mutex::scoped_lock l2(m_dirty_blocks_mutex);
		m_dirty_blocks.push_back(blk);
		blk->dirty = true;
		// keep the inode block alive until we've had a chance
		// to flush it. When it's flushed it will be decremented
		++blk->references;
	}

	inode_block* blk = i->second;
	mutex::scoped_lock l2(blk->inode_mutex);
	TORRENT_ASSERT(!blk->marked_for_deletion);
	if (blk->references == INT_MAX)
	{
		ec.assign(boost::system::errc::too_many_files_open, generic_category());
		return NULL;
	}
	++blk->references;
	return blk;
}

// close this file and make sure it's flushed
void block_device::close(void* inode)
{
	inode_block* blk = (inode_block*)inode;
	mutex::scoped_lock l(blk->inode_mutex);

	close_impl(blk, l);
}

void block_device::close_impl(inode_block* blk, mutex::scoped_lock& l)
{
	TORRENT_ASSERT(blk->references > 0);
	--blk->references;

	if (blk->references > 0) return;

	// this was the last reference to this file. is it marked
	// for deletion?
	if (!blk->marked_for_deletion) return;

	std::vector<boost::uint32_t> used_blocks;

	// return the inode block itself
	free_inode(blk->block_index);

	// return all the allocated data blocks to the free-list
	for (std::vector<boost::uint32_t>::iterator i = blk->block_map.begin()
		, end(blk->block_map.end()); i != end; ++i)
	{
		if (*i == block_allocator::unallocated_block) continue;
		used_blocks.push_back(*i);
	}

	trim_blocks(&used_blocks[0], used_blocks.size());

	m_free_blocks.free_blocks(&used_blocks[0], used_blocks.size());

	// we have to unlock before deleting the block
	// since the mutex is a member
	l.unlock();
	delete blk;
}

#ifdef __linux__
#ifndef FITRIM
struct fstrim_range {
	uint64_t start;
	uint64_t len;
	uint64_t minlen;
};
#define FITRIM		_IOWR('X', 121, struct fstrim_range)
#endif
#endif // __linux__

void block_device::trim_blocks(boost::uint32_t* b, int num_blocks)
{
#ifdef FITRIM
	for (int i = 0; i < num_blocks; ++i)
	{
		fstrim_range rng = { boost::uint64_t(b[i]) * m_block_size, m_block_size, 0};
		ioctl(m_file.native_handle(), FITRIM, &rng);
	}
#endif
#ifdef FSCTL_FILE_LEVEL_TRIM
	FILE_LEVEL_TRIM* fstrim = TORRENT_ALLOCA(char, sizeof(FILE_LEVEL_TRIM)
		+ sizeof(EXTENT_PAIR) * num_blocks);
	fstrim.PairCount = num_blocks;
	for (int i = 0; i < num_blocks; ++i)
	{
		fstrim.Pairs[i].Offset = boost:uint64_t(b[i]) * m_block_size;
		fstrim.Pairs[i].Length = m_block_size;
	}

	DeviceIoControl(m_file.native_handle(), FSCTL_FILE_LEVEL_TRIM
		, fstrim, sizeof(FILE_LEVEL_TRIM) + sizeof(EXTENT_PAIR) * num_blocks
		, NULL, 0, NULL, NULL);
#endif
}

// return all blocks belonging to this file back to the free-list
// and remove this entry from the file allocation table.
void block_device::unlink(void* inode)
{
	TORRENT_ASSERT(!m_destructed);
	inode_block* blk = (inode_block*)inode;

	mutex::scoped_lock l(m_inode_mutex);
	mutex::scoped_lock l2(blk->inode_mutex);

	// we'll return the blocks once all handles
	// are closed
	TORRENT_ASSERT(blk->references > 0);

	if (blk->marked_for_deletion) return;

	blk->marked_for_deletion = true;

	boost::unordered_map<sha1_hash, inode_block*>::iterator i
		= m_inodes.find(blk->info_hash);

	// this file has not been unlinked, since marked_for_deletion
	// was false. that means this file must exist in the map
	TORRENT_ASSERT(i != m_inodes.end());

	// in production, it's probably better to just abort if
	// this happens though
	if (i == m_inodes.end()) return;

	m_inodes.erase(i);
	m_dirty_root_block = true;

	if (!blk->dirty) return;

	mutex::scoped_lock dl(m_dirty_blocks_mutex);
	std::vector<inode_block*>::iterator dirty
		= std::find(m_dirty_blocks.begin(), m_dirty_blocks.end(), blk);
	if (dirty != m_dirty_blocks.end())
	{
		m_dirty_blocks.erase(dirty);
		blk->dirty = false;
		close_impl(blk, l2);
	}
}

// returns a rough estimate of how much free space there is on
// the device
size_type block_device::free_space() const
{
	return size_type(m_free_blocks.num_free()) * m_block_size;
}

void block_device::stat(void* inode, fstatus* st) const
{
	inode_block* blk = (inode_block*)inode;

	mutex::scoped_lock l(blk->inode_mutex);

	TORRENT_ASSERT(blk->references > 0);

	st->file_size = size_type(blk->block_map.size()) * m_block_size;
	st->allocated_size = size_type(blk->blocks_in_use) * m_block_size;
	st->info_hash = blk->info_hash;
}

void block_device::readdir(std::vector<fstatus>* dir) const
{
	mutex::scoped_lock l(m_inode_mutex);
	dir->resize(m_inodes.size());

	int index = 0;
	for (boost::unordered_map<sha1_hash, inode_block*>::const_iterator i =
		m_inodes.begin(), end(m_inodes.end()); i != end; ++i, ++index)
	{
		fstatus& fs = (*dir)[index];
		inode_block* blk = i->second;

		fs.info_hash = blk->info_hash;
		fs.file_size = size_type(blk->block_map.size()) * m_block_size;
		fs.allocated_size = size_type(blk->blocks_in_use) * m_block_size;
	}
}

void block_device::extent_map(void* inode, std::vector<boost::uint32_t>* map) const
{
	inode_block* blk = (inode_block*)inode;
	mutex::scoped_lock l(blk->inode_mutex);
	TORRENT_ASSERT(blk->references > 0);
	*map = blk->block_map;
}

void block_device::allocator_stats(std::vector<std::pair<int, int> >* st) const
{
	const int num_allocators = sizeof(m_node_allocator)/sizeof(m_node_allocator[0]);
	st->resize(num_allocators);
	for (int i = 0; i < num_allocators; ++i)
		(*st)[i] = m_node_allocator[i].usage();
}

sub_block_ref block_device::allocate_inode(boost::uint64_t size, error_code& ec)
{
	int target_blocks = (size + m_block_size - 1) / m_block_size;
	int target_inode_size = 0;
	while (target_blocks > ((1024 << target_inode_size) - sizeof(inode_block_t)) / 4)
		++target_inode_size;

	if (1024 > (m_block_size >> target_inode_size))
	{
		ec.assign(boost::system::errc::file_too_large, generic_category());
		return sub_block_ref::invalid;
	}

	sub_block_ref ret = m_node_allocator[target_inode_size].allocate_node(m_free_blocks);
	if (ret == sub_block_ref::invalid)
		ec.assign(boost::system::errc::no_space_on_device, generic_category());
	
	return ret; 
}

void block_device::free_inode(sub_block_ref iblock)
{
	if (m_node_allocator[iblock.subblock_size].free_node(iblock, m_free_blocks))
		trim_blocks(&iblock.block, 1);
}

bool block_device::check_iop(inode_block* inode, file::iovec_t const* iov, int nvec
	, size_type offset, error_code& ec) const
{
	// negative number of iovecs is clearly invalid
	if (nvec < 0)
	{
		ec.assign(boost::system::errc::invalid_argument, generic_category());
		return true;
	}

	int iop_size = bufs_size(iov, nvec);

	// negative offsets are invalid
	if (offset < 0)
	{
		ec.assign(boost::system::errc::invalid_argument, generic_category());
		return true;
	}

	// an IOP is not allowed to span multiple blocks
	if ((offset % m_block_size) + iop_size > m_block_size)
	{
		ec.assign(boost::system::errc::invalid_argument, generic_category());
		return true;
	}

	// files cannot be larger than block_size * block_map_size
	// where block_map_size depends on how many 32 bit words
	// fit in the inode_block, which is block_size - inode_header_size
	if (offset + iop_size > size_type(inode->max_size) * m_block_size)
	{
		ec.assign(boost::system::errc::file_too_large, generic_category());
		return true;
	}

	return false;
}

int block_device::preadv(void* inode, file::iovec_t const* iov, int nvec
	, size_type offset, error_code& ec)
{
	TORRENT_ASSERT(!m_destructed);
	inode_block* blk = (inode_block*)inode;

	// whoever is making this call must hold a reference to the inode
	TORRENT_ASSERT(blk->references > 0);

	if (check_iop(blk, iov, nvec, offset, ec)) return -1;

	// the block within the file address space
	int file_block = offset / m_block_size;

	// the block index in the device address space
	int device_block = block_allocator::unallocated_block;

	// look up the device block to read from
	mutex::scoped_lock l(blk->inode_mutex);
	if (blk->block_map.size() > file_block)
		device_block = blk->block_map[file_block];
	l.unlock();

	if (device_block == block_allocator::unallocated_block)
	{
		// we're reading unallocated space. return zeroes
		int ret = 0;
		for (int i = 0; i < nvec; ++i)
		{
			memset(iov[i].iov_base, 0, iov[i].iov_len);
			ret += iov[i].iov_len;
		}
		return ret;
	}

	size_type dev_offset = size_type(device_block) * m_block_size
		+ (offset % m_block_size);

#if DISK_ACCESS_LOG
	int id = write_disk_log(m_access_log, dev_offset, 0, start_read, time_now_hires());
#endif
	int ret = m_file.readv(dev_offset, iov, nvec, ec);
#if DISK_ACCESS_LOG
	write_disk_log(m_access_log, dev_offset + bufs_size(iov, nvec), id, complete_read, time_now_hires());
#endif
	return ret;
}

int block_device::pwritev(void* inode, file::iovec_t const* iov, int nvec
	, size_type offset, error_code& ec)
{
	TORRENT_ASSERT(!m_destructed);
	inode_block* blk = (inode_block*)inode;

	// whoever is making this call must hold a reference to the inode
	TORRENT_ASSERT(blk->references > 0);

	if (check_iop(blk, iov, nvec, offset, ec)) return -1;

	// the block within the file address space
	int file_block = offset / m_block_size;

	// the block index in the device address space
	int device_block = block_allocator::unallocated_block;

	// look up the device block to write to
	mutex::scoped_lock l(blk->inode_mutex);
	if (blk->block_map.size() <= file_block)
	{
		if (file_block >= blk->max_size)
		{
			ec.assign(boost::system::errc::file_too_large, generic_category());
			return -1;
		}
		// no allocated space for this block, extend the
		// block map and fill the new entries with unallocated_block
		blk->block_map.resize(file_block + 1, block_allocator::unallocated_block);
	}
	else
	{
		// this slot exists in the block map, now
		// let's see if there's already a block allocated
		// on the device for this file block
		device_block = blk->block_map[file_block];
	}

	boost::optional<mutex::scoped_lock> seq;

	if (device_block == block_allocator::unallocated_block)
	{
		// we're allocating a block. Make sure any other thread that's also
		// allocating a new block to write to is serialized with this thread,
		// to force full sequenctial writes to the disk
		seq = boost::in_place(std::ref(m_sequential_write_mutex));

		// we need to allocae a new block on the device
		// this is a cheap O(1) operation that doesn't
		// have to touch the disk, it's OK to do this
		// while still holding the lock. In fact, we need
		// to hold the lock to avoid another thread allocating
		// the same block
		device_block = m_free_blocks.allocate_block(false);
		if (device_block == block_allocator::unallocated_block)
		{
			// we failed to allocate a new block
			// we're out of space on the device!
			ec.assign(boost::system::errc::no_space_on_device, generic_category());
			return -1;
		}

		// allright, let's add our newly allocated block
		// to the block map
		blk->block_map[file_block] = device_block;
		++blk->blocks_in_use;
		if (!blk->dirty)
		{
			mutex::scoped_lock l2(m_dirty_blocks_mutex);
			m_dirty_blocks.push_back(blk);
			blk->dirty = true;
			// keep the inode block alive until we've had a chance
			// to flush it. When it's flushed it will be decremented
			++blk->references;
		}
		if (bufs_size(iov, nvec) != m_block_size)
		{
			printf("performance warning: writing less than block size: %d B "
				"( block-size: %d B) block: %d\n"
				, bufs_size(iov, nvec), m_block_size, device_block);
		}
	}
	else
	{
		printf("performance warning: writing to existing block %d\n", device_block);
	}

	l.unlock();

	size_type dev_offset = size_type(device_block) * m_block_size
		+ (offset % m_block_size);

#if DISK_ACCESS_LOG
	int id = write_disk_log(m_access_log, dev_offset, 0, start_write, time_now_hires());
#endif
	int ret = m_file.writev(dev_offset, iov, nvec, ec);
#if DISK_ACCESS_LOG
	write_disk_log(m_access_log, dev_offset + bufs_size(iov, nvec), id, complete_write, time_now_hires());
#endif
	return ret;
}

// this implements the libtorrent storage interface, to store torrents in
// the block_device
struct block_device_storage : libtorrent::storage_interface
{
	block_device_storage(boost::shared_ptr<block_device> dev, file_storage const& fs, sha1_hash const& ih)
		: m_device(dev)
		, m_info_hash(ih)
		, m_inode(0)
		, m_piece_size(fs.piece_length())
		, m_total_size(fs.total_size())
	{}

	virtual bool tick()
	{
		fprintf(stderr, "flushing inodes and root block\n");
		m_device->flush_inodes();
		m_device->flush_root_block();
		return false;
	}

	// create directories and set file sizes
	virtual void initialize(storage_error& ec)
	{
	}

	virtual int readv(file::iovec_t const* bufs, int num_bufs
		, int piece, int offset, int flags, storage_error& ec)
	{
		if (m_inode == NULL)
		{
			m_inode = m_device->open(m_info_hash, m_total_size, ec.ec);
			if (ec) return -1;
		}

		size_type toffset = size_type(piece) * m_piece_size + offset;
		return m_device->preadv(m_inode, bufs, num_bufs, toffset, ec.ec);
	}

	virtual int writev(file::iovec_t const* bufs, int num_bufs
		, int piece, int offset, int flags, storage_error& ec)
	{
		if (m_inode == NULL)
		{
			m_inode = m_device->open(m_info_hash, m_total_size, ec.ec);
			if (ec) return -1;
		}

		size_type toffset = size_type(piece) * m_piece_size + offset;
		return m_device->pwritev(m_inode, bufs, num_bufs, toffset, ec.ec);
	}

	virtual bool has_any_file(storage_error& ec)
	{
		return m_device->exists(m_info_hash);
	}

	// change the priorities of files. This is a fenced job and is
	// guaranteed to be the only running function on this storage
	virtual void set_file_priority(std::vector<boost::uint8_t> const& prio, storage_error& ec) {}

	// non-zero return value indicates an error
	virtual int move_storage(std::string const& save_path, int flags
		, storage_error& ec) { return libtorrent::piece_manager::no_error; }

	// verify storage dependent fast resume entries
	virtual bool verify_resume_data(lazy_entry const& rd, storage_error& ec) { return true; }

	// write storage dependent fast resume entries
	virtual void write_resume_data(entry& rd, storage_error& ec) const {}

	// this will close all open files that are opened for
	// writing. This is called when a torrent has finished
	// downloading.
	// non-zero return value indicates an error
	virtual void release_files(storage_error& ec) {}

	// this will rename the file specified by index.
	virtual void rename_file(int index, std::string const& new_filenamem, storage_error& ec) {}

	// this will close all open files and delete them
	// non-zero return value indicates an error
	virtual void delete_files(storage_error& ec)
	{
		if (m_inode == NULL) return;
		m_device->unlink(m_inode);
		m_device->close(m_inode);
		m_inode = NULL;
	}

	virtual ~block_device_storage()
	{
		if (m_inode)
			m_device->close(m_inode);
	}

private:
	boost::shared_ptr<block_device> m_device;
	sha1_hash m_info_hash;

	// refers to the storage for this specific torrent file
	void* m_inode;

	int m_piece_size;
	boost::uint64_t m_total_size;
};

libtorrent::storage_interface* block_device_storage_constructor(
	boost::shared_ptr<block_device> dev, storage_params const& params)
{
	return new block_device_storage(dev, *params.files, params.info->info_hash());
}


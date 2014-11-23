/*

Copyright (c) 2014, Arvid Norberg
All rights reserved.

btfs Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

For details, see LICENSE

*/

#ifndef TORRENT_STORAGE_DEVICE
#define TORRENT_STORAGE_DEVICE

#include <boost/cstdint.hpp>
#include <boost/unordered_map.hpp>
#include <string>
#include <vector>
#include <set>
#include "libtorrent/config.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/peer_id.hpp" // for sha1_hash
#include "libtorrent/bitfield.hpp"
#include "libtorrent/thread.hpp"
#include "libtorrent/storage.hpp"
#include "block_allocator.hpp"
#include "pool_allocator.hpp"

namespace libtorrent
{
	struct storage_params;
}

using libtorrent::error_code;
using libtorrent::file;
using libtorrent::sha1_hash;
using libtorrent::bitfield;
using libtorrent::mutex;
using libtorrent::storage_error;
using libtorrent::storage_params;

struct inode_block;
struct block_device;

#define DISK_ACCESS_LOG 1

#if DISK_ACCESS_LOG
#include "libtorrent/time.hpp"
using libtorrent::ptime;
#endif

TORRENT_EXPORT libtorrent::storage_interface* block_device_storage_constructor(
	boost::shared_ptr<block_device> dev, storage_params const& params);

// this is the singleton all torrent storages uses to
// actually write to to the device
struct TORRENT_EXPORT block_device
{
	// the blocks that are allocated on the
	// raw block device are 4 MB.
	enum { default_block_size = 4 * 1024 * 1024 };

	block_device(std::string const& device_path, error_code& ec);
	~block_device();

	int preadv(void* inode, file::iovec_t const* iov, int nvec, boost::int64_t offset, error_code& ec);
	int pwritev(void* inode, file::iovec_t const* iov, int nvec, boost::int64_t offset, error_code& ec);

	// returns true if there is an entry for this info-hash
	bool exists(sha1_hash const& info_hash) const;

	// returns a file handle
	// max_size is required if the file doesn't exist
	void* open(sha1_hash const& info_hash, boost::uint64_t max_size, error_code& ec);

	// close this file and make sure it's flushed
	void close(void* inode);

	// return all blocks belonging to this file back to the free-list
	// and remove this entry from the file allocation table.
	void unlink(void* inode);

	// returns a rough estimate of how much free space there is on
	// the device
	boost::int64_t free_space() const;

	struct fstatus
	{
		sha1_hash info_hash;
		boost::int64_t file_size;
		boost::int64_t allocated_size;
	};

	void stat(void* inode, fstatus* st) const;

	void readdir(std::vector<fstatus>* dir) const;

	void extent_map(void* inode, std::vector<boost::uint32_t>* map) const;

	void allocator_stats(std::vector<std::pair<int, int> >* st) const;

	// -------- initialization ------------
	// you either need to call read_root_block() to initialize
	// the block device, assuming there is a formatted file
	// system on there already. If not, and you wish to initialize
	// the device, call format().

	// initialize device
	void format(error_code& ec, int block_size = default_block_size);

	// read an existing filesystem from drive
	void read_root_block(error_code& ec);

	int block_size() const { return m_block_size; }

	void flush_inodes();
	void flush_root_block();

private:

	int preadv_impl(inode_block* blk, file::iovec_t const* iov, int nvec
		, int file_block, int block_offset, error_code& ec);
	int pwritev_impl(inode_block* blk, file::iovec_t const* iov, int nvec
		, int file_block, int block_offset, error_code& ec);

	void close_impl(inode_block* blk, mutex::scoped_lock& l);
	void trim_blocks(boost::uint32_t* b, int num_blocks);

	bool check_iop(inode_block* inode, file::iovec_t const* iov, int nvec
		, boost::int64_t offset, error_code& ec) const;
	void read_inode(bitfield& used_blocks, sub_block_ref iblock, error_code& ec);

	sub_block_ref allocate_inode(boost::uint64_t max_size, error_code& ec);
	void free_inode(sub_block_ref iblock);

	// this is the max size (in bytes) of the device
	boost::uint64_t m_max_size;

	// this is the filesystem block size (typically 4 MiB)
	boost::uint32_t m_block_size;

	// this is the underlying media block size (typically 512 B)
	boost::uint32_t m_media_block_size;
	
	// the file referring to the block device or flat file
	file m_file;

	mutable mutex m_dirty_blocks_mutex;
	std::vector<inode_block*> m_dirty_blocks;
	
	// this is the file list
	mutable mutex m_inode_mutex;
	boost::unordered_map<sha1_hash, inode_block*> m_inodes;
	bool m_dirty_root_block;

	// this mutex is used to serialize all writes to newly allocated blocks.
	// this forces accurate sequential writes
	mutable mutex m_sequential_write_mutex;

	// keeps track of free filesystem blocks
	block_allocator m_free_blocks;

	// keeps track of allocated but unused inode
	// slots of varying sizes
	pool_allocator m_node_allocator[13];

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	bool m_destructed;
#endif

#if DISK_ACCESS_LOG
	FILE* m_access_log;
	ptime m_start_time;
#endif

};

#endif


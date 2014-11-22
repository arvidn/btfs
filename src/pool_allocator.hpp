/*

Copyright (c) 2014, Arvid Norberg
All rights reserved.

btfs Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

For details, see LICENSE

*/

#ifndef TORRENT_POOL_ALLOCATOR_HPP_INCLUDED
#define TORRENT_POOL_ALLOCATOR_HPP_INCLUDED

// this is the pool allocator used to allocate inodes
// each instantiation of a pool allocator allocates
// inodes of a certain size. Typical sizes are:
// 1 kiB, 2 kiB, 4 kiB, 8 kiB, 16 kiB, 32kiB, 64 kiB
// 128 kiB, 256 kiB, 512 kiB, 1 MiB, 2 MiB, 4 MiB.

// the maximum file size each of those inodes can hold
// are:
// 960 MiB, 1984 MiB, 4032 MiB, 8128 MiB, 16320 MiB, 32704 MiB
// and so on. i.e. (size of inode - 64) MiB

#include <boost/cstdint.hpp>
#include <set>

#include "libtorrent/thread.hpp"
#include "block_allocator.hpp"

using libtorrent::mutex;

struct sub_block_ref
{
	// this is the filesystem block
	boost::uint32_t block;
	// this is the sub-block within the
	// filesystem block
	boost::uint16_t subblock;
	// this is the size expressed in how many
	// times to shift 1024 to the left.
	boost::uint8_t subblock_size;

	// unused
	boost::uint8_t padding;

	boost::uint64_t device_offset(boost::uint32_t block_size) const
	{ return boost::uint64_t(block_size) * block + subblock * (1024 << subblock_size); }

	int node_size() const { return 1024 << subblock_size; }

	bool operator!=(sub_block_ref rhs) const
	{
		return block != rhs.block || subblock != rhs.subblock;
	}

	bool operator==(sub_block_ref rhs) const
	{
		return block == rhs.block && subblock == rhs.subblock;
	}

	bool operator<(sub_block_ref rhs) const
	{
		if (block < rhs.block) return true;
		if (block > rhs.block) return false;
		return subblock < rhs.subblock;
	}

	const static sub_block_ref invalid;
};

struct pool_allocator
{
	pool_allocator();
	void init(int nodes_per_block, int size);
	void mark_in_use(sub_block_ref blk);
	sub_block_ref allocate_node(block_allocator& alloc);
	bool free_node(sub_block_ref, block_allocator& alloc);
	std::pair<int, int> usage() const;
private:
	mutable mutex m_mutex;
	int m_nodes_per_block;
	int m_node_size;

	// the number of nodes that are in use
	int m_in_use;
	std::set<sub_block_ref> m_free_nodes;
};

#endif


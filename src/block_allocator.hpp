/*

Copyright (c) 2014, Arvid Norberg
All rights reserved.

btfs Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

For details, see LICENSE

*/

#ifndef BLOCK_ALLOCATOR_HPP
#define BLOCK_ALLOCATOR_HPP

#include <boost/cstdint.hpp>
#include <deque>
#include "libtorrent/thread.hpp"
#include "libtorrent/bitfield.hpp"

using libtorrent::bitfield;
using libtorrent::mutex;

// the logic manipulating the free-list of
// filesystem blocks

// this allocator always allocates the lowest index block number
struct ordered_block_allocator
{
	// used when initializing an empty device
	void init(boost::uint32_t total_blocks);

	// used when opening a device
	void init(boost::uint32_t total_blocks, bitfield const& used_blocks);

	boost::uint32_t num_free() const;

	boost::uint32_t allocate_block(bool inode);
	void free_block(boost::uint32_t block);
	void free_blocks(boost::uint32_t* blocks, int num);

	enum constants_t
	{
		// this is what is put in the block_map
		// for an block that's not allocated. block 0
		// is the filesystem root block, it can never
		// be allocated for data or anything else
		unallocated_block = 0,
	};

private:

	// the number of blocks available on media
	boost::uint32_t m_max_blocks;

	// this mutex must be held when reading or writing
	// m_free_blocks.
	mutable mutex m_mutex;

	// free blocks are pushed and popped at the end
	// of this vector. It's essentially used as a stack.
	// this means blocks are likely to be reused immediately
	// after they are freed. This may improve cache hits
	// when using a cached device or a flat file
	// inode blocks are allocated from the front and data
	// blocks are allocated from the back
	std::deque<boost::uint32_t> m_free_blocks;
};

// this allocator always allocates the next available
// block from the cursor of the last block allocated
struct sequential_block_allocator
{
	// used when initializing an empty device
	void init(boost::uint32_t total_blocks);

	// used when opening a device
	void init(boost::uint32_t total_blocks, bitfield const& used_blocks);

	boost::uint32_t num_free() const;

	boost::uint32_t allocate_block(bool inode);
	void free_block(boost::uint32_t block);
	void free_blocks(boost::uint32_t* blocks, int num);

	enum constants_t
	{
		// this is what is put in the block_map
		// for an block that's not allocated. block 0
		// is the filesystem root block, it can never
		// be allocated for data or anything else
		unallocated_block = 0,
	};

private:

	// the number of blocks available on media
	boost::uint32_t m_max_blocks;

	// number of blocks not in use
	int m_free_blocks;

	// this mutex must be held when reading or writing
	// m_free_blocks.
	mutable mutex m_mutex;

	bitfield m_used_blocks;

	// the next block to allocate
	int m_cursor;
};

//typedef ordered_block_allocator block_allocator;
typedef sequential_block_allocator block_allocator;

#endif


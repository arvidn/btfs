/*

Copyright (c) 2014, Arvid Norberg
All rights reserved.

btfs Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

For details, see LICENSE

*/

#include "block_allocator.hpp"
#include "libtorrent/assert.hpp"
#include <algorithm>

// used when initializing an empty device
void ordered_block_allocator::init(boost::uint32_t total_blocks)
{
	m_max_blocks = total_blocks;
	m_free_blocks.resize(m_max_blocks-1);
	for (int i = 0; i < m_max_blocks - 1; ++i)
		m_free_blocks[i] = i + 1;
}

// used when opening a device
void ordered_block_allocator::init(boost::uint32_t total_blocks, bitfield const& used_blocks)
{
	m_max_blocks = total_blocks;

	// build the free-list for filesystem blocks
	for (boost::uint32_t i = 0; i < m_max_blocks; ++i)
	{
		if (used_blocks[i]) continue;
		m_free_blocks.push_back(i);
	}
}

boost::uint32_t ordered_block_allocator::num_free() const
{ return m_free_blocks.size(); }

void ordered_block_allocator::free_block(boost::uint32_t block)
{
	mutex::scoped_lock l(m_mutex);
	// insert the block ordered
	std::deque<boost::uint32_t>::iterator i = std::lower_bound(
		m_free_blocks.begin(), m_free_blocks.end(), block);
	m_free_blocks.insert(i, block);
}

void ordered_block_allocator::free_blocks(boost::uint32_t* blocks, int num)
{
	TORRENT_ASSERT(num > 0);
	if (num <= 0) return;

	// this will speed up in-order insertion
	std::sort(blocks, blocks + num);

	mutex::scoped_lock l(m_mutex);

	std::deque<boost::uint32_t>::iterator i = m_free_blocks.begin();
	while (num > 0)
	{
		i = std::lower_bound(i, m_free_blocks.end(), *blocks);
		// insert the block ordered
		m_free_blocks.insert(i, *blocks);
		++blocks;
		--num;
	}
}

boost::uint32_t ordered_block_allocator::allocate_block(bool inode)
{
	mutex::scoped_lock l(m_mutex);

	// no free blocks left!
	if (m_free_blocks.empty()) return unallocated_block;

	boost::uint32_t blk;
	if (inode)
	{
		blk = m_free_blocks.back();
		m_free_blocks.pop_back();
	}
	else
	{
		blk = m_free_blocks.front();
		m_free_blocks.pop_front();
	}
	return blk;
}



// ========== sequential block allocator ============



// used when initializing an empty device
void sequential_block_allocator::init(boost::uint32_t total_blocks)
{
	m_max_blocks = total_blocks;
	m_used_blocks = total_blocks;
	m_cursor = 0;
	m_free_blocks = total_blocks;

	TORRENT_ASSERT(m_free_blocks == m_used_blocks.size() - m_used_blocks.count());
}

// used when opening a device
void sequential_block_allocator::init(boost::uint32_t total_blocks, bitfield const& used_blocks)
{
	m_max_blocks = total_blocks;
	m_used_blocks = used_blocks;
	m_cursor = 0;
	m_free_blocks = total_blocks;
	for (int i = 0; i < total_blocks; ++i)
	{
		if (m_used_blocks[i]) --m_free_blocks;
	}

	TORRENT_ASSERT(m_free_blocks == m_used_blocks.size() - m_used_blocks.count());
}

boost::uint32_t sequential_block_allocator::num_free() const
{ return m_free_blocks; }

void sequential_block_allocator::free_block(boost::uint32_t block)
{
	mutex::scoped_lock l(m_mutex);
	
	TORRENT_ASSERT(block < m_used_blocks.size());
	TORRENT_ASSERT(m_used_blocks[block]);
	m_used_blocks.clear_bit(block);
	++m_free_blocks;

	TORRENT_ASSERT(m_free_blocks == m_used_blocks.size() - m_used_blocks.count());
}

void sequential_block_allocator::free_blocks(boost::uint32_t* blocks, int num)
{
	TORRENT_ASSERT(num > 0);
	if (num <= 0) return;

	// this will speed up in-order insertion
	std::sort(blocks, blocks + num);

	mutex::scoped_lock l(m_mutex);

	int i = 0;
	while (num > 0)
	{
		TORRENT_ASSERT(*blocks < m_used_blocks.size());
		TORRENT_ASSERT(m_used_blocks[*blocks]);
		m_used_blocks.clear_bit(*blocks);
		++blocks;
		++i;
		--num;
		++m_free_blocks;
	}
	TORRENT_ASSERT(m_free_blocks == m_used_blocks.size() - m_used_blocks.count());
}

boost::uint32_t sequential_block_allocator::allocate_block(bool inode)
{
	mutex::scoped_lock l(m_mutex);

	if (m_free_blocks == 0)
		return unallocated_block;

	while (m_used_blocks[m_cursor])
	{
		++m_cursor;
		if (m_cursor >= m_max_blocks) m_cursor = 0;
	}

	m_used_blocks.set_bit(m_cursor);
	--m_free_blocks;
	TORRENT_ASSERT(m_free_blocks == m_used_blocks.size() - m_used_blocks.count());
	return m_cursor;
}


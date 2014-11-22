/*

Copyright (c) 2014, Arvid Norberg
All rights reserved.

btfs Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

For details, see LICENSE

*/

#include "pool_allocator.hpp"
#include "block_allocator.hpp"

pool_allocator::pool_allocator()
	: m_nodes_per_block(0)
	, m_node_size(0)
	, m_in_use(0)
{}

void pool_allocator::init(int nodes_per_block, int size)
{
	m_nodes_per_block = nodes_per_block;
	m_node_size = size;
}

void pool_allocator::mark_in_use(sub_block_ref blk)
{
	TORRENT_ASSERT(blk.subblock_size == m_node_size);

	++m_in_use;

	// we're not locking here because this is only used
	// when initializing the allocator, which is single-
	// threaded
	std::set<sub_block_ref>::iterator i = m_free_nodes.find(blk);
	if (i != m_free_nodes.end())
	{
		m_free_nodes.erase(i);
		return;
	}

	for (int i = 0; i < m_nodes_per_block; ++i)
	{
		if (i == blk.subblock) continue;
		sub_block_ref r = {blk.block, boost::uint16_t(i), boost::uint8_t(m_node_size)};
		m_free_nodes.insert(r);
	}
}

sub_block_ref pool_allocator::allocate_node(block_allocator& alloc)
{
	mutex::scoped_lock l(m_mutex);
	if (!m_free_nodes.empty())
	{
		sub_block_ref ret = *m_free_nodes.begin();
		m_free_nodes.erase(m_free_nodes.begin());
		++m_in_use;
		return ret;
	}

	boost::uint32_t block = alloc.allocate_block(true);
	if (block == block_allocator::unallocated_block) return sub_block_ref::invalid;
	sub_block_ref ret = { block, 0, boost::uint8_t(m_node_size) };
	for (int i = 1; i < m_nodes_per_block; ++i)
	{
		sub_block_ref r = {block, boost::uint16_t(i), boost::uint8_t(m_node_size)};
		m_free_nodes.insert(r);
	}
	++m_in_use;
	return ret;
}

bool pool_allocator::free_node(sub_block_ref blk, block_allocator& alloc)
{
	TORRENT_ASSERT(blk.subblock_size == m_node_size);

	mutex::scoped_lock l(m_mutex);

	m_free_nodes.insert(blk);

	// now, see if we have freed an entire filesystem block
	// if so, we need to return it to the block allocator
	typedef std::set<sub_block_ref>::iterator iter;
	sub_block_ref r = {blk.block, 0, blk.subblock_size};
		m_free_nodes.insert(r);
	iter first = std::lower_bound(m_free_nodes.begin(), m_free_nodes.end(), r);
	iter end = m_free_nodes.end();
	int num_free_in_block = 0;
	iter i;
	for (i = first; i != end && i->block == blk.block; ++i)
		++num_free_in_block;

	if (num_free_in_block < m_nodes_per_block) return false;

	alloc.free_block(blk.block);
	m_free_nodes.erase(first, i);
	return true;
}

std::pair<int, int> pool_allocator::usage() const
{
	mutex::scoped_lock l(m_mutex);
	return std::pair<int, int>(m_in_use, m_free_nodes.size());
}

const sub_block_ref sub_block_ref::invalid = {0,0,0};


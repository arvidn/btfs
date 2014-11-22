/*

Copyright (c) 2014, Arvid Norberg
All rights reserved.

btfs Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

For details, see LICENSE

*/

#include "block_affinity.hpp"
#include "libtorrent/alert.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/peer_connection.hpp"
#include <boost/cstdint.hpp>

using libtorrent::alert;
using libtorrent::block_downloading_alert;
using libtorrent::alert_cast;
using libtorrent::piece_picker;

// this is an alert observer that creates an affinity
// to download pieces adjacent to some other pieces
// in order to more efficiently fill up the blocks
// on the block device filesystem
block_affinity_plugin::block_affinity_plugin(torrent& t, int block_size)
	: m_torrent(t)
	, m_block_size(block_size)
	, m_peer_plugin(new peer_block_affinity(*this))
{}

boost::shared_ptr<peer_plugin> block_affinity_plugin::new_connection(peer_connection* p)
{
	// this will make peers request whole aligned piece ranges
	// to match the block size on disk
	p->prefer_whole_pieces(m_block_size / m_torrent.torrent_file().piece_length());
	p->picker_options(piece_picker::align_expanded_pieces);
	return m_peer_plugin;
}

void block_affinity_plugin::sending_request(int piece)
{
	int piece_size = m_torrent.torrent_file().piece_length();

	// there's no point in doing this optimization if a piece
	// is as big as a block, or bigger
	if (piece_size == 0 || piece_size >= m_block_size) return;

	std::vector<std::pair<int, int> > pieces;

	int range_start = ((boost::uint64_t(piece) * piece_size) & ~(m_block_size-1)) / piece_size;
	int range_end = (std::min)(range_start + m_block_size / piece_size, m_torrent.torrent_file().num_pieces());

	for (; range_start < range_end; ++range_start)
		pieces.push_back(std::make_pair(range_start, 7));

	// increase the priority of all pieces that would end up
	// in this block on the device, in order to minimize wasted
	// space by unused block area
	m_torrent.prioritize_piece_list(pieces);
}

boost::shared_ptr<torrent_plugin> block_affinity(torrent* t, int block_size)
{
	return boost::shared_ptr<torrent_plugin>(new block_affinity_plugin(*t, block_size));
}


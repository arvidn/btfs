/*

Copyright (c) 2014, Arvid Norberg
All rights reserved.

btfs Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

For details, see LICENSE

*/

#ifndef TORRENT_BLOCK_AFFINITY_HPP_INCLUDED
#define TORRENT_BLOCK_AFFINITY_HPP_INCLUDED

#include "libtorrent/extensions.hpp"
#include "libtorrent/torrent.hpp"
#include <boost/function.hpp>

using libtorrent::torrent_plugin;
using libtorrent::peer_plugin;
using libtorrent::torrent;
using libtorrent::peer_connection;
using libtorrent::peer_request;

// this is an plugin that creates an affinity
// to download pieces adjacent to some other pieces
// in order to more efficiently fill up the blocks
// on the block device filesystem
struct block_affinity_plugin : torrent_plugin
{
	block_affinity_plugin(torrent& t, int block_size);
	boost::shared_ptr<peer_plugin> new_connection(peer_connection*);
	void sending_request(int piece);

private:
	torrent& m_torrent;
	int m_block_size;
	boost::shared_ptr<peer_plugin> m_peer_plugin;
};

struct peer_block_affinity : peer_plugin
{
	peer_block_affinity(block_affinity_plugin& ba) : m_ba(ba) {}
	bool write_request(peer_request const& req)
	{
		m_ba.sending_request(req.piece);
		return false;
	}

	block_affinity_plugin& m_ba;
};

boost::shared_ptr<torrent_plugin> TORRENT_EXPORT
	block_affinity(torrent* t, int block_size);

#endif // TORRENT_BLOCK_AFFINITY_HPP_INCLUDED


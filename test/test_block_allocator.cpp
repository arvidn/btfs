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
#include <vector>

using namespace libtorrent;

const int num_blocks = 45000;
const int num_torrents = 500;

struct test_torrent
{
	int size;
	std::vector<boost::uint32_t> blocks;
};

// there are 100 torrents, each with its own
// list of blocks that it has allocated
test_torrent torrents[num_torrents];

int get_torrent_size()
{
	// average torrent size is 300 MiB
	// the return value is the size in 4 MiB blocks
	// 300 MiB = 75 blocks.
	// randomize the torrent sizes +- 200 MiB / 50 blocks

	int size1 = (rand() % 100) + 75;

	// create a slight bias towards the center (75 blocks)
	int size2 = (rand() % 100) + 75;

	return size1 * 2 / 3 + size2 / 3;
}

int main()
{
	int ret = 0;

	// the random numbers should be predictable
	srand(0x1337);

	for (int i = 0; i < num_torrents; ++i)
		torrents[i].size = get_torrent_size();

	block_allocator blk;
	bitfield bf;
	bf.resize(num_blocks, false);
	bf.set_bit(0);
	blk.init(num_blocks, bf);

	int last_write = 0;

	FILE* f = fopen("stats.dat", "w+");
	fprintf(f, "#%-14s %-15s %-15s\n", "index", "free blocks", "write block");

	for (int loops = 0; loops < num_blocks * 4; ++loops)
	{
		int t = rand() % num_torrents;
		test_torrent& tor = torrents[t];
		boost::uint32_t b = blk.allocate_block(false);
		if (b == 0)
		{
			fprintf(stderr, "allocation failed\n");
			return 1;
		}
		tor.blocks.push_back(b);
		fprintf(f, "%-15d %-15d %-15d\n", loops, blk.num_free(), b);
		last_write = b;
		if (tor.blocks.size() >= tor.size)
		{
			// this torrent is complete, delete all its
			// blocks and replace it with a new torrent
			blk.free_blocks(&tor.blocks[0], tor.blocks.size());
			tor.blocks.clear();
			tor.size = get_torrent_size();
		}
	}

	fclose(f);

	return ret;
}


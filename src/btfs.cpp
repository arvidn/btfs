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
#include "libtorrent/escape_string.hpp"

#include <sys/stat.h>

using libtorrent::from_hex;
using libtorrent::to_hex;
using libtorrent::file;

void print_usage()
{
	char const* usage_string = "usage: btfs command [arg] device\n\n"
		"command must be one of:\n"
		"  list                    - list file contents on device\n"
		"  initialize <blk-size>   - format device as a bittorrent filesystem\n"
		"                            block size is specified in bytes.\n"
		"                            recommended setting is 4 MB, 4194304 bytes\n"
		"  unlink <file>           - unlink (remove) the specifed file. This\n"
		"                            command takes an additional argument.\n"
		"                            the file is specified as a 40 digit hex string.\n"
		"  read <file>             - reads the entire file specified and prints it\n"
		"                            to stdout. This command takes an additional\n"
		"                            argument which is the file to print.\n"
		"  write <file> <path>     - reads from stdin and writes to the specified\n"
		"                            file. This command takes an additional\n"
		"                            argument which is the file to write to.\n"
		"  visualize                 prints a graphical representation of the\n"
		"                            filesystem to stdout.\n"
		"  stats                     prints i-node allocation statistics\n"
		"\n"
		"filenames are always 40 hex digit sha-1 digests.\n"
		"\n"
		"the device may be a file, which must have been pre-allocated\n"
		"to the desired size\n";

	fputs(usage_string, stderr);
	exit(1);
}

enum command_t
{
	cmd_list, cmd_initialize, cmd_unlink, cmd_read, cmd_write, cmd_visualize, cmd_stats
};

int parse_command(int argc, char* argv[], char const* argument[], char const*& device)
{
	// skip executable filename
	++argv;
	--argc;
	int ret;
	int num_args = 0;
	argument[0] = NULL;
	argument[1] = NULL;
	if (strcmp(argv[0], "list") == 0)
	{
		ret = cmd_list;
		num_args = 0;
	}
	else if (strcmp(argv[0], "visualize") == 0)
	{
		ret = cmd_visualize;
		num_args = 0;
	}
	else if (strcmp(argv[0], "stats") == 0)
	{
		ret = cmd_stats;
		num_args = 0;
	}
	else if (strcmp(argv[0], "initialize") == 0)
	{
		ret = cmd_initialize;
		num_args = 1;
	}
	else if (strcmp(argv[0], "unlink") == 0)
	{
		ret = cmd_unlink;
		num_args = 1;
	}
	else if (strcmp(argv[0], "read") == 0)
	{
		ret = cmd_read;
		num_args = 1;
	}
	else if (strcmp(argv[0], "write") == 0)
	{
		ret = cmd_write;
		num_args = 2;
	}
	else
	{
		fprintf(stderr, "unknown command: \"%s\"\n", argv[0]);
		print_usage();
		return 0;
	}
	++argv;
	--argc;

	if (argc <= 0)
	{
		fputs("too few arguments\n", stderr);
		print_usage();
		return 0;
	}

	for (int i = 0; i < num_args; ++i)
	{
		argument[i] = argv[0];
		++argv;
		--argc;

		if (argc <= 0)
		{
			fputs("too few arguments\n", stderr);
			print_usage();
			return 0;
		}
	}

	if (argc > 1)
	{
		fputs("too many arguments\n", stderr);
		print_usage();
		return 0;
	}

	device = argv[0];
	return ret;
}

int main(int argc, char* argv[])
{
	if (argc < 3) print_usage();

	char const* argument[5];
	char const* device = NULL;
	int command = parse_command(argc, argv, argument, device);

	error_code ec;
	block_device dev(device, ec);
	if (ec)
	{
		fprintf(stderr, "Error opening device or file: %s\n", ec.message().c_str());
		return 1;
	}

	sha1_hash info_hash;
	if (command != cmd_initialize)
	{
		// the first argument is always a sha1-hash
		if (argument[0])
		{
			if (strlen(argument[0]) != 40)
			{
				fprintf(stderr, "invalid filename argument; \"%s\". Expected 40 hex digits"
					" len=%d\n"
					, argument[0], int(strlen(argument[0])));
				return 1;
			}
			if (!from_hex(argument[0], 40, (char*)&info_hash[0]))
			{
				fprintf(stderr, "invalid filename argument; \"%s\". Expected 40 hex digits\n", argument[0]);
				return 1;
			}
		}

		dev.read_root_block(ec);
		if (ec)
		{
			fprintf(stderr, "failed to read filesystem: %s\n", ec.message().c_str());
			return 1;
		}
	}
	switch (command)
	{
		case cmd_list:
		{
			std::vector<block_device::fstatus> dir;
			dev.readdir(&dir);
			for (std::vector<block_device::fstatus>::iterator i = dir.begin()
				, end(dir.end()); i != end; ++i)
			{
				fprintf(stderr, "%s  s: %10" PRId64 " a: %10" PRId64 "\n"
					, to_hex(i->info_hash.to_string()).c_str(), i->file_size, i->allocated_size);
			}
			fprintf(stderr, "%10" PRId64 " bytes free\n", dev.free_space());
			break;
		}
		case cmd_initialize:
		{
			int block_size = atoi(argument[0]);
			dev.format(ec, block_size);
			if (ec)
			{
				fprintf(stderr, "failed to initialize device '%s': %s\n", device, ec.message().c_str());
				return 1;
			}
			fprintf(stderr, "device '%s' successfully initialized. %" PRId64 " bytes free\n"
				, device, dev.free_space());
			break;
		}
		case cmd_unlink:
			{
				if (!dev.exists(info_hash))
				{
					fprintf(stderr, "file \"%s\" does not exist\n", argument[0]);
					return 1;
				}
				void* inode = dev.open(info_hash, 0, ec);
				if (ec)
				{
					fprintf(stderr, "failed to open file \"%s\": %s\n", argument[0], ec.message().c_str());
					return 1;
				}
				dev.unlink(inode);
				dev.close(inode);
				break;
			}
		case cmd_read:
			{
				if (!dev.exists(info_hash))
				{
					fprintf(stderr, "file \"%s\" does not exist\n", argument[0]);
					return 1;
				}
				void* inode = dev.open(info_hash, 0, ec);
				if (ec)
				{
					fprintf(stderr, "failed to open file \"%s\": %s\n", argument[0], ec.message().c_str());
					return 1;
				}
				char filebuf[4096];
				memset(filebuf, 0, sizeof(filebuf));
				file::iovec_t b = { filebuf, sizeof(filebuf) };
				boost::int64_t offset = 0;
				block_device::fstatus st;
				dev.stat(inode, &st);
				while (offset < st.file_size)
				{
					dev.preadv(inode, &b, 1, offset, ec);
					if (ec)
					{
						fprintf(stderr, "error reading from file: \"%s\": %s\n"
							, argument[0], ec.message().c_str());
						break;
					}
					offset += sizeof(filebuf);
					fwrite(filebuf, 1, sizeof(filebuf), stdout);
				}
				fprintf(stderr, "read %" PRId64 " bytes from \"%s\"\n", offset, argument[0]);
				dev.close(inode);
				break;
			}
		case cmd_write:
			{
				if (!dev.exists(info_hash))
					fprintf(stderr, "creating new file \"%s\"\n", argument[0]);
				FILE* input = fopen(argument[1], "rb");
				if (input == NULL)
				{
					fprintf(stderr, "failed to open input file \"%s\": (%d) %s\n"
						, argument[1], errno, strerror(errno));
					return 1;
				}
				struct stat st;
				if (stat(argument[1], &st) < 0)
				{
					fprintf(stderr, "failed to stat input file: \"%s\": (%d) %s\n"
						, argument[1], errno, strerror(errno));
				}
				void* inode = dev.open(info_hash, st.st_size, ec);
				if (ec)
				{
					fprintf(stderr, "failed to open file \"%s\": %s\n"
						, argument[0], ec.message().c_str());
					return 1;
				}
				std::vector<char> filebuf(4 * 1024 * 1024);
				int len = 0;
				memset(&filebuf[0], 0, filebuf.size());
				file::iovec_t b[2] = { { &filebuf[0], filebuf.size()/2}
					, { &filebuf[filebuf.size()/2], filebuf.size()/2 } };

				boost::int64_t offset = 0;
				while ((len = fread(&filebuf[0], 1, filebuf.size(), input)) > 0)
				{
					if (len < filebuf.size()/2)
					{
						b[0].iov_len = len;
						b[1].iov_len = 0;
					}
					else
					{
						b[1].iov_len = len - filebuf.size() / 2;
					}

					dev.pwritev(inode, b, 2, offset, ec);
					if (ec)
					{
						fprintf(stderr, "error writing to file: \"%s\": %s\n"
							, argument[0], ec.message().c_str());
						break;
					}
					offset += filebuf.size();
				}
				fclose(input);
				fprintf(stderr, "wrote %" PRId64 " bytes to \"%s\"\n", offset, argument[0]);
				dev.close(inode);
				break;
			}
		case cmd_visualize:
			{
				std::vector<block_device::fstatus> dir;
				dev.readdir(&dir);
				std::vector<boost::uint32_t> blocks;
				for (std::vector<block_device::fstatus>::iterator i = dir.begin()
					, end(dir.end()); i != end; ++i)
				{
					printf("%s  s: %10" PRId64 " a: %10" PRId64 "\n"
						, to_hex(i->info_hash.to_string()).c_str(), i->file_size, i->allocated_size);
					error_code ec;
					void* inode = dev.open(i->info_hash, 0, ec);
					if (ec)
					{
						fprintf(stderr, "error opening file \"%s\": %s\n"
							, to_hex(i->info_hash.to_string()).c_str(), ec.message().c_str());
						break;
					}
					dev.extent_map(inode, &blocks);
					for (int i = 0; i < int(blocks.size()); ++i)
					{
						if (blocks[i] == 0xffffffff) printf(".");
						else printf("%u ", blocks[i]);
					}
					printf("\n");
					dev.close(inode);
				}
				fprintf(stderr, "%10" PRId64 " bytes free\n", dev.free_space());
				break;
			}
		case cmd_stats:
			{
				std::vector<std::pair<int, int> > st;
				dev.allocator_stats(&st);
				int k = 0;
				fprintf(stderr, "i-node allocator stats:\n");
				for (std::vector<std::pair<int, int> >::iterator i = st.begin()
					, end(st.end()); i != end; ++i, ++k)
				{
					fprintf(stderr, "%4d kiB [ in-use: %-5d allocated: %-5d ]\n"
						, 1 << k, i->first, i->second);
				}
			}
			break;
	}
};

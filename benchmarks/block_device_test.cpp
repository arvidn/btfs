/*

Copyright (c) 2014, Arvid Norberg
All rights reserved.

btfs Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

For details, see LICENSE

*/

#include "libtorrent/session.hpp"
#include "libtorrent/torrent_handle.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/alert_types.hpp"

#include "block_device.hpp"
#include "block_affinity.hpp"

#include <boost/bind.hpp>
#include <map>

using namespace libtorrent;

int main(int argc, char *const argv[])
{
	if (argc > 3 || argc < 2)
	{
		fprintf(stderr, "usage: block_device_test listen-port [device-path]\n");
		return 1;
	}

	// we want this to destruct after the session, so the
	// pointer must be declared before ses
	boost::shared_ptr<block_device> dev;

	int port = atoi(argv[1]);
	session ses(fingerprint("LT", 0, 1, 0, 0)
		, std::make_pair(port, port + 1));
	ses.set_alert_mask(~alert::progress_notification);
	settings_pack s;
	high_performance_seed(s);
	s.set_int(settings_pack::cache_size, 65536); // 1 GB
	s.set_int(settings_pack::listen_queue_size, 500);
	s.set_int(settings_pack::alert_queue_size, 50000);
//	s.set_bool(settings_pack::contiguous_recv_buffer, false);
	s.set_bool(settings_pack::contiguous_recv_buffer, true);
	s.set_bool(settings_pack::allow_multiple_connections_per_ip, true);

	if (argc > 2)
	{
		char const* device_path = argv[2];

		error_code ec;
		dev.reset(new block_device(device_path, ec));
		if (ec)
		{
			fprintf(stderr, "FATAL: failed to open device \"%s\": (%d) %s\n"
				, device_path, ec.value(), ec.message().c_str());
			return 1;
		}
		dev->read_root_block(ec);
		if (ec)
		{
			fprintf(stderr, "FATAL: failed read device \"%s\": (%d) %s\n"
				, device_path, ec.value(), ec.message().c_str());
			return 1;
		}

		// with too many threads, we risk getting our writes out of order
		// breaking sequentiality
		s.set_int(settings_pack::aio_threads, 4);
	}

	ses.add_extension(boost::bind(&block_affinity, _1, 4 * 1024 * 1024));

	// these two settings have a significant impact on performance
	// it would be interesting to run multiple tests on a regular
	// filesystem with varying cache line sizes
	s.set_bool(settings_pack::allow_partial_disk_writes, false);

	// try to flush 4 MB at a time to the disk
	s.set_int(settings_pack::write_cache_line_size, 256);

	ses.apply_settings(s);

	// (filename, torrent_info)
	std::vector<std::pair<std::string, boost::shared_ptr<torrent_info> > > test_torrents;
	// (handle, cmd line for connection_tester)
	std::map<torrent_handle, std::string> handles;

	error_code ec;
	std::string path("test_torrents");
	for (directory dir(path, ec); !ec && !dir.done(); dir.next(ec))
	{
		if (extension(dir.file()) != ".torrent") continue;
		std::string file_path = combine_path(path, dir.file());
		error_code tec;
		boost::shared_ptr<torrent_info> ti(new torrent_info(file_path, tec));

		// assume the file isn't fully written yet.
		if (tec)
		{
			fprintf(stderr, "error loading \"%s\": %s\n", file_path.c_str(), tec.message().c_str());
			continue;
		}

		test_torrents.push_back(std::make_pair(file_path, ti));
	}

	std::deque<alert*> alert_queue;
	time_t last_added = 0;
	do
	{
		// space out adding new torrents by 2 second
		// TODO: it should really be spaced out by number of bytes downloaded...
		if (!test_torrents.empty() && handles.size() < 10 && time(NULL) - 1 > last_added)
		{
			add_torrent_params p;
			p.flags = add_torrent_params::flag_update_subscribe | add_torrent_params::flag_pinned;
			p.save_path = "torrent_storage";
			p.ti = test_torrents.back().second;
			if (dev)
				p.storage = boost::bind(&block_device_storage_constructor, dev, _1);
			std::string path = test_torrents.back().first;
			printf("adding \"%s\"\n", path.c_str());
			test_torrents.pop_back();
			torrent_handle h = ses.add_torrent(p);
			char cmd_buf[200];
			snprintf(cmd_buf, sizeof(cmd_buf), "connection_tester upload -c 10 -d 127.0.0.1 -p %d -t %s >logs/tester_%s.log 2>1 &"
				, port, path.c_str(), filename(path).c_str());
			handles.insert(std::make_pair(h, std::string(cmd_buf)));
			last_added = time(NULL);
		}
		
		usleep(100000);

		ses.pop_alerts(&alert_queue);
		for (std::deque<alert*>::iterator i = alert_queue.begin()
			, end(alert_queue.end()); i != end; ++i)
		{
			std::auto_ptr<alert> a(*i);
//			printf("  %s\n", a->message().c_str());

			torrent_deleted_alert* td = alert_cast<torrent_deleted_alert>(a.get());
			torrent_delete_failed_alert* tdf = alert_cast<torrent_delete_failed_alert>(a.get());

			if (td || tdf)
			{
				torrent_alert* tf = (torrent_alert*)td;
				if (tf == NULL) tf = (torrent_alert*)tdf;

				std::map<torrent_handle, std::string>::iterator hi = handles.find(tf->handle);
				if (hi == handles.end())
				{
					// delete the first invalid handle we can find instead
					for (std::map<torrent_handle, std::string>::iterator i = handles.begin()
						, end(handles.end()); i != end; ++i)
					{
						if (i->first.is_valid()) continue;
						hi = i;
						break;
					}
				}

				handles.erase(hi);

				printf("still running: ");
				for (std::map<torrent_handle, std::string>::iterator i = handles.begin()
					, end(handles.end()); i != end; ++i)
				{
					int str_start = i->second.find("test_torrents/");
					if (str_start == std::string::npos) str_start = 0;
					else str_start += 14;

					int str_end = i->second.find(" ", str_start);

					printf("\"%s\" ", i->second.substr(str_start, str_end - str_start).c_str());
				}
				printf("\n");
			}
			else if (torrent_finished_alert* tf = alert_cast<torrent_finished_alert>(a.get()))
			{
				std::map<torrent_handle, std::string>::iterator hi = handles.find(tf->handle);
				if (hi == handles.end()) continue;
				printf("completed: \"%s\"\n", tf->handle.name().c_str());
				ses.remove_torrent(tf->handle, session::delete_files);
			}
			else if (state_changed_alert* sc = alert_cast<state_changed_alert>(a.get()))
			{
				if (sc->prev_state == torrent_status::checking_resume_data)
				{
					std::map<torrent_handle, std::string>::iterator hi = handles.find(sc->handle);
					printf("running: \"%s\"\n", hi->second.c_str());
					system(hi->second.c_str());
				}
			}
			else if (torrent_error_alert* ea = alert_cast<torrent_error_alert>(a.get()))
			{
				printf("ERROR: \"%s\": %s (%s)\n", ea->handle.name().c_str(), ea->error.message().c_str(), ea->error_file.c_str());
			}
		}
		alert_queue.clear();
//		printf("running: %d (%d)\n", int(handles.size()), handles.empty());
	}
	while (!handles.empty() || !test_torrents.empty());
}


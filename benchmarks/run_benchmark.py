# this test requires connection_tester (from libtorrent/examples) to be installed
# and available in $PATH, as well as parse_access_log (from libtorrent/tools).

# btfs from the parent directory needs to be built and available at ../btfs
# (this script attempts to do that)
# the test binary itself, block_device_test, should be copied from the build
# directory to the same directory this script lives in, benchmarks.

# right now the path to the block device where the custom filesystem is
# initialized and tested is hard coded in this script.

import os
import time
import shutil
import sys

num_torrents = 25

port = (int(time.time()) % 50000) + 2000

try: os.mkdir('test_torrents')
except: pass

# build the test
ret = os.system('b2 boost=source link=static debug-symbols=on release -j4 stage')
if ret != 0:
	print 'ERROR: building block_device_test failed: %d' % ret
	sys.exit(1)

# build the btfs tool
ret = os.system('(cd .. && b2 boost=source link=static -j4 stage)')
if ret != 0:
	print 'ERROR: building block_device_test failed: %d' % ret
	sys.exit(1)

if not os.path.exists('torrent_storage.img'):
	ret = os.system('dd if=/dev/zero count=20971520 of=torrent_storage.img')
	if ret != 0:
		print 'ERROR: dd failed: %d' % ret
		sys.exit(1)

ret = os.system('../btfs initialize 1048576 torrent_storage.img')
if ret != 0:
	print 'ERROR: btfs failed: %d' % ret
	sys.exit(1)

try: os.mkdir('logs')
except: pass

for i in range(num_torrents):
	if os.path.exists('test_torrents/%d.torrent' % i): continue
	ret = os.system('connection_tester gen-torrent -s 1000 -n %d -t test_torrents/%d.torrent' % (i+1, i))
	if ret != 0:
		print 'ERROR: connection_tester failed: %d' % ret
		sys.exit(1)

try: shutil.rmtree('torrent_storage')
except: pass

try: shutil.rmtree('session_stats')
except: pass
try: shutil.rmtree('session_stats_btfs')
except: pass
try: shutil.rmtree('session_stats_ext4')
except: pass

start = time.time();
#cmd = 'gdb --args ./block_device_test %d torrent_storage.img' % port
cmd = './block_device_test %d torrent_storage.img' % port
print cmd
ret = os.system(cmd)

if ret != 0:
	print 'ERROR: ./block_device_test failed: %d' % ret
	sys.exit(1)

end = time.time();

print 'runtime (custom filesystem): %d seconds' % (end - start)

os.rename('session_stats', 'session_stats_btfs')
os.system('python ../../libtorrent/tools/parse_session_stats.py session_stats_btfs/*.log')
try: os.rename('session_stats_report', 'session_stats_report_btfs')
except: pass
ret = os.system('parse_access_log block_device_access.log')
if ret != 0:
	print 'ERROR: parse_access_log failed: %d' % ret
	sys.exit(1)

start = time.time();
#cmd = 'gdb --args ./block_device_test %d' % port
cmd = './block_device_test %d' % port
print cmd
ret = os.system(cmd)
if ret != 0:
	print 'ERROR: ./block_device_test failed: %d' % ret
	sys.exit(1)

end = time.time();

print 'runtime (regular filesystem): %d seconds' % (end - start)

os.rename('session_stats', 'session_stats_ext4')
os.system('python ../../libtorrent/tools/parse_session_stats.py session_stats_ext4/*.log')
try: os.rename('session_stats_report', 'session_stats_report_ext4')
except: pass


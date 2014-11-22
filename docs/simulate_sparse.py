import os
import random

allocated_extents = {}

pieces = range(10000)
random.shuffle(pieces)

pieces_per_extent = 4
cursor = 0

f = open('sparse_access.dat', 'w+')

for i in xrange(10000):
	piece = pieces[0]
	pieces = pieces[1:]
	ext = piece / pieces_per_extent
	if ext in allocated_extents:
		pos = allocated_extents[ext] + piece % pieces_per_extent
	else:
		pos = cursor + piece % pieces_per_extent
		allocated_extents[ext] = cursor
		cursor += pieces_per_extent

	print >>f, '%d\t%d' % (i, pos)

f.close()


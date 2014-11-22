set term png size 800,400 giant
set output "torrent_size_distribution.png"
set title "torrent sizes in random sample"
set ylabel "fraction of torrents"
set xlabel "torrent size in MiB"
set style fill solid border -1 pattern 2
plot "sizes.dat" using 1:2 title "torrent size" with boxes

set terminal postscript
set output "torrent_size_distribution.ps"
replot

set term png size 800,400 giant
set output "piece_size_distribution.png"
set title "piece sizes in random sample"
set ylabel "fraction of torrents"
set xlabel "piece size in kiB"
set logscale x
set style fill solid border -1 pattern 2
plot "piece_sizes.dat" using 1:2 title "piece size" with boxes

set terminal postscript
set output "piece_size_distribution.ps"
replot

set term png size 800,400 giant
set nologscale x
set output "torrent_size_cdf.png"
set title "torrent sizes CDF in random sample"
set ylabel "torrents (%)"
set xlabel "torrent size in MiB"
set style fill solid border -1 pattern 2
plot "sizes_cdf.dat" using 1:2 title "torrent size" with lines

set terminal postscript
set output "torrent_size_cdf.ps"
replot


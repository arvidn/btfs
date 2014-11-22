set term png size 800,400 giant
set output "access.png"
set title "block writes"
set ylabel "block"
set xlabel "write"
set key off
plot "stats.dat" using 1:3 title "disk write" with dots

set terminal postscript
set output "access.ps"
replot

set term png size 800,400 giant
set output "free_blocks.png"

set title "free blocks"
set ylabel "free blocks"
set xlabel "write"
plot "stats.dat" using 1:2 title "blocks free" with steps

set terminal postscript
set output "free_blocks.ps"
replot


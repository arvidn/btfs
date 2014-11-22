set term png size 800,400 giant
set output "sparse_access.png"
set title "block writes (sparse files, 4 pieces per extent)"
set ylabel "block"
set xlabel "write"
set key off
plot "sparse_access.dat" using 1:2 title "disk write" with dots

set terminal postscript
set output "sparse_access.ps"
replot



set terminal pdf enhanced font 'Verdana,24’ persist
set output 'fct_homa_aeolus_many_to_many_testbed.pdf'
    
set boxwidth 0.9
set grid ytics
set yrange [0:300]
#set xrange [-1:4]
set auto x
set style data histogram
set style histogram cluster gap 1 errorbars
set style fill border -1
set xlabel "Flow Size (KB)"
set ylabel "FCT (ms)"
set key at graph 0.9, 0.96

plot "many_to_many.txt" using 2:3:4:xtic(1) lc rgb 'blue' fillstyle pattern 3 title "Homa" , \
            '' using 5:6:7 lc rgb 'orange' fillstyle pattern 2 title "Homa+Aeolus" 
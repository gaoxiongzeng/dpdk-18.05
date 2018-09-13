set terminal pdf enhanced font 'Verdana,24’ persist
set output 'incast_result.pdf'
    
set boxwidth 0.9
set grid ytics
set yrange [0:300]
#set xrange [-1:4]
set auto x
set style data histogram
set style histogram cluster gap 1 errorbars
set style fill pattern 0 border -1
set xlabel "Flow Size (KB)"
set ylabel "FCT (ms)"
set key at graph 0.9, 0.96

plot "raw.txt" using 2:3:4:xtic(1) title "Homa w/o Aeolus" , \
            '' using 5:6:7 title "Homa with Aeolus" 
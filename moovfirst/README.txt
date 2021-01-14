Use the .pro file if you already have qtcreator installed, otherwise you can compile by command line (no dependencies).

g++  -o moovfirst main.cpp file.cpp atom.cpp log.cpp



Moovfirst changes the order of the atoms in an mp4 or a mov such that
the moov atom (the index of the video) is at the beginning.

Moovfirst takes two parameters, the input file and the output file (must be different).

This program has not been extensively tested, especially on large files, so TEST the output video before removing the original one.


for m in *.mp4;
do
./moovfirst "$m" "${m%.mp4}"_stream.mp4;
done;


If you feel confident add a mv to replace the original:

for m in *.mp4;
do
./moovfirst "$m" tmp.mp4;
mv tmp.mp4 "${m%.mp4}"_stream.mp4;
done;


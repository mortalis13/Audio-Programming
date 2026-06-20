@echo off
path=d:\progs\mingw64-5.3.0\bin;..\shared

set FFMPEG=..\shared\ffmpeg-6.0

echo Building miniaudio
if not exist miniaudio.o ( g++ miniaudio.c -o miniaudio.o -c )

echo Building sample player
g++ -std=c++11 -o ma-player.exe miniaudio-player.cpp miniaudio.o

echo Building raw PCM player
g++ -std=c++11 -o ma-raw.exe miniaudio-raw.cpp miniaudio.o

echo Building Player with ffmpeg decoding and resampling
g++ miniaudio-ffmpeg.cpp miniaudio.o -o ma-ffmpeg.exe ^
  -std=c++11 ^
  -lws2_32 -lavformat -lavutil -lavcodec -lswresample ^
  -I %FFMPEG%/include -L %FFMPEG%/lib -Wall

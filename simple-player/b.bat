@echo off
path=d:\progs\mingw64-5.3.0\bin
rem Comment sections that are not needed

rem == Player from miniaudio examples (http://miniaud.io)
g++ -std=c++11 -o player-example.exe miniaudio-player.cpp
rem Run: player-example.exe audio.wav


rem == Raw PCM data player
g++ -std=c++11 -o ma-raw.exe miniaudio-raw.cpp


rem == Player with ffmpeg decoding and resampling
set FFMPEG=d:/progs/ffmpeg-6.0-full_build-shared
path=%path%;%FFMPEG%

if not exist miniaudio.o ( g++ miniaudio.c -o miniaudio.o -c )
g++ miniaudio-fm.cpp -o miniaudio-fm.o -std=c++11 -I %FFMPEG%/include -c
g++ miniaudio-fm.o miniaudio.o -o ma-fm.exe -lws2_32 -lavformat -lavutil -lavcodec -lswresample -L %FFMPEG%/lib

rem [Full build]
rem g++ miniaudio-fm.cpp miniaudio.c -o ma-fm.exe -std=c++11 -lws2_32 -lavformat -lavutil -lavcodec -lswresample -I %FFMPEG%/include -L %FFMPEG%/lib -Wall

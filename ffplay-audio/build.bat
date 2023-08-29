@echo off
path=d:\progs\mingw64-5.3.0\bin

gcc -o ffplay.exe ffplay.c ^
  -lavformat -lavutil -lavcodec -lswresample -I ./ffmpeg-6.0/include -L ./ffmpeg-6.0/lib ^
  -lSDL2main -lSDL2 -I ./SDL2/include/SDL2 -L ./SDL2/lib

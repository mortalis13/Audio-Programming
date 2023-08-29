@echo off
path=d:\progs\mingw64-5.3.0\bin

gcc -o ffplay.exe ffplay.c ^
  -lavformat -lavutil -lavcodec -lswresample -I ./ffmpeg-6.0/include -L ./ffmpeg-6.0/lib ^
  -lSDL2main -lSDL2 -I ./SDL2/include/SDL2 -L ./SDL2/lib


rem == Static build tests
rem gcc -o ffplay.exe ffplay.c -lavformat -lavutil -lavcodec -lswresample -I %FFMPEG%/include -L %FFMPEG%/lib ^
rem   -lSDL2main -lSDL2 -I SDL2/include/SDL2 ^
rem   -lmingw32 -mwindows  -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va ^
rem   -lm -ldinput8 -ldxguid -ldxerr8 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid -static

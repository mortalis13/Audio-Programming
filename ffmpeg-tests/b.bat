@echo off
path=d:\progs\mingw64-5.3.0\bin;..\shared

set FFMPEG=..\shared\ffmpeg-6.0
set SDL=..\shared\SDL2

@echo on
g++ -o decoder.exe decode-audio.cpp -std=c++11 ^
  -lws2_32 -lavcodec -lavformat -lavutil -lswresample ^
  -I %FFMPEG%/include -L %FFMPEG%/lib

g++ -o waveform.exe waveform.cpp -std=c++11 ^
  -lws2_32 -lavcodec -lavformat -lavutil -lSDL2main -lSDL2 ^
  -I %FFMPEG%/include -I %SDL%/include/SDL2 ^
  -L %FFMPEG%/lib -L %SDL%/lib

g++ -o fm-tests.exe fm-tests.cpp -std=c++11 ^
  -lws2_32 -lavcodec -lavformat -lavutil -lswresample ^
  -I %FFMPEG%/include -L %FFMPEG%/lib

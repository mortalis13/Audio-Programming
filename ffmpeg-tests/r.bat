@echo off
path=..\shared

@echo on
echo ==Decoder test==
decoder.exe ..\shared\serenade.mp3 serenade.wav

echo ==Waveform test==
waveform.exe ..\shared\serenade.mp3

echo ==ffmpeg test==
fm-tests.exe ..\shared\serenade.mp3

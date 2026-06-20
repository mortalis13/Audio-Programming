@echo off
path=d:\progs\mingw64-5.3.0\bin;..\shared

@echo on
g++ -std=c++11 -o tester.exe tester.cpp filter_fx.cpp

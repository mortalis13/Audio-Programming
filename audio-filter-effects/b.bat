@echo off
path=d:\progs\mingw64-5.3.0\bin

rem g++ -std=c++11 -c filter_fx.cpp
@echo on
g++ -std=c++11 -o q.exe tester.cpp filter_fx.cpp

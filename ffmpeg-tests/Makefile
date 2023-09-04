FFMPEG := d:/progs/ffmpeg-6.0-full_build-shared

CPPFLAGS:=-std=c++11
LDFLAGS:=-std=c++11 -lws2_32 -lavcodec -lavformat -lavutil
INCLUDES:=$(FFMPEG)/include
LDINCLUDES:=$(FFMPEG)/lib

SRC := $(wildcard *.cpp)
OBJ := $(notdir $(SRC:.cpp=.o))
EXE := $(notdir $(SRC:.cpp=.exe))

# g++ test.cpp -o test.exe -std=c++11 -lws2_32 -lavcodec -lavformat -lavutil -I %FFMPEG%/include -L %FFMPEG%/lib

all: $(EXE) $(OBJ)

%.exe: %.o
	g++ -g $^ -o $@ $(LDFLAGS) -L $(LDINCLUDES)

%.o: %.cpp
	g++ -o $@ $(CPPFLAGS) -I $(INCLUDES) -c $^

clean:
	del /Q /S *.o *.exe > nul 2>&1

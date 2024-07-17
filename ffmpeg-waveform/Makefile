PR := main.exe

BASE := ../shared
FFMPEG := $(BASE)/ffmpeg-6.0
SDL := $(BASE)/SDL2

CPPFLAGS:=-std=c++11
LDFLAGS:=-std=c++11 -lws2_32 -lavcodec -lavformat -lavutil -lswresample -lSDL2main -lSDL2
INCLUDES:=-I $(FFMPEG)/include -I $(SDL)/include/SDL2
LDINCLUDES:=-L $(FFMPEG)/lib -L $(SDL)/lib

HEADERS := $(wildcard *.h)

SRC := $(wildcard *.cpp)
OBJ := $(notdir $(SRC:.cpp=.o))

all: $(PR)

$(PR): $(OBJ)
	g++ -g $^ -o $@ $(LDFLAGS) $(LDINCLUDES)

%.o: %.cpp $(HEADERS)
	g++ -o $@ $(CPPFLAGS) $(INCLUDES) -c $<

clean:
	del /Q /S *.o *.exe > nul 2>&1

#include <string>

#include <SDL.h>

#include "AudioDecoder.h"

using namespace std;

// Override main from SDL2
#undef main

string filePath = "../shared/serenade.mp3";

int view_width = 1080;
int view_height = 120;

int data_size = view_width * 2;

short* waveformData = new short[data_size];


void onDraw(SDL_Renderer* renderer) {
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
  
  float center = (float) view_height / 2;
  int x = 0;
  
  for (int i = 0; i < data_size; i+=2, x++) {
    float h1 = waveformData[i];
    float h2 = waveformData[i+1];
    float y0 = center - h1;
    float y1 = center - h2;
    
    SDL_RenderDrawLine(renderer, x, y0, x, y1);
  }
  
  SDL_RenderPresent(renderer);
}

void buildWaveform(string audioPath) {
  float* pixel_data = new float[data_size];
  memset(pixel_data, 0, data_size * sizeof(float));
  
  AudioDecoder audioDecoder;
  int result = audioDecoder.compressSamples(audioPath, pixel_data, view_width);
  
  if (result != 0) {
    printf("\ncompressSamples: %d\n\n", result);
    return;
  }
  
  for (size_t i = 0; i < data_size; i++) {
    short value = (short) (pixel_data[i] * view_height / 2);
    waveformData[i] = value;
  }
  delete[] pixel_data;
}


int main() {
  printf("ffmpeg-waveform\n");
  
  buildWaveform(filePath);
  
  bool quit = false;
  SDL_Event event;

  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window* window = SDL_CreateWindow("SDL2 line drawing", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, view_width, view_height, 0);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);

  while (!quit) {
    SDL_Delay(10);
    SDL_PollEvent(&event);
    switch (event.type) {
      case SDL_QUIT:
        quit = true;
        break;
    }
    
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    
    onDraw(renderer);
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  
  return 0;
}

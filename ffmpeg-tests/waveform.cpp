extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <string>
#include <vector>
#include <cmath>

#include <SDL.h>

using namespace std;

// Override main from SDL2
#undef main

// Refer to ffmpeg avf_showwaves.c for waveform generation algorithm

AVFormatContext* formatContext = NULL;
AVCodecContext* codecContext = NULL;
AVStream* audioStream = NULL;

int view_width = 1080;
int view_height = 120;

int data_size = view_width;

short* waveformData = new short[data_size];


void log(const string& msg, ...) {
  va_list ap;
  va_start(ap, msg);
  string format = msg + "\n";
  vfprintf(stdout, format.c_str(), ap);
  fflush(stdout);
  va_end(ap);
}

av_always_inline char* _av_err2str(int errnum) {
  char str[AV_ERROR_MAX_STRING_SIZE];
  return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}
#ifdef av_err2str
#undef av_err2str
#endif
#define av_err2str(errnum) _av_err2str(errnum)


int loadCodec(string filePath) {
  int result = -1;
  
  result = avformat_open_input(&formatContext, filePath.c_str(), NULL, NULL);
  if (result < 0) {
    log("Failed to open file: (%d) %s", result, av_err2str(result));
    return result;
  }
  log("AV: input opened");
  
  result = avformat_find_stream_info(formatContext, NULL);
  if (result < 0) {
    log("Failed to find stream info: (%d) %s", result, av_err2str(result));
    return result;
  }
  log("AV: stream info found");
  
  int audioStreamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (audioStreamIndex >= 0) {
    audioStream = formatContext->streams[audioStreamIndex];
  }
  
  if (audioStream == NULL || audioStream->codecpar == NULL) {
    log("Could not find a suitable audio stream to decode");
    return -1;
  }
  log("AV: audio stream found");

  const AVCodec* audioCodec = avcodec_find_decoder(audioStream->codecpar->codec_id);
  if (!audioCodec){
    log("Could not find codec with ID: %d (%s)", audioStream->codecpar->codec_id, avcodec_get_name(audioStream->codecpar->codec_id));
    return -1;
  }
  log("AV: codec found");
  
  codecContext = avcodec_alloc_context3(audioCodec);
  if (!codecContext){
    log("Failed to allocate codec context");
    return -1;
  }
  log("AV: codec context allocated");

  codecContext->pkt_timebase = audioStream->time_base;
  
  result = avcodec_parameters_to_context(codecContext, audioStream->codecpar);
  if (result < 0) {
    log("Failed to copy codec parameters to codec context: (%d) %s", result, av_err2str(result));
    return result;
  }
  log("AV: codec parameters copied");

  result = avcodec_open2(codecContext, audioCodec, NULL);
  if (result < 0) {
    log("Could not open codec: (%d) %s", result, av_err2str(result));
    return result;
  }
  log("AV: codec opened");
  
  return result;
}

int compressSamples(string filePath, float* compressed_data, int dest_size) {
  int result;
  
  result = loadCodec(filePath);
  if (result != 0) return result;
  
  AVPacket* audioPacket;
  AVFrame* audioFrame;
  
  audioPacket = av_packet_alloc();
  if (!audioPacket) {
    log("[compress] Could not allocate audio packet");
    return -1;
  }
  
  audioFrame = av_frame_alloc();
  if (!audioFrame) {
    log("[compress] Could not allocate audio frame");
    av_packet_free(&audioPacket);
    return -1;
  }
  
  // estimate if it's enough to group each frame or a custom block size should be used
  int64_t estimated_samples = (int64_t) ((double) formatContext->duration / AV_TIME_BASE * codecContext->sample_rate);
  int64_t estimated_frames = (codecContext->frame_size != 0) ? estimated_samples / codecContext->frame_size: 0;
  
  int block_size = 10;
  if (estimated_samples < dest_size * block_size) block_size = 1;
  bool group_frames = (estimated_frames > dest_size);
  
  log("[input] channels: %d", audioStream->codecpar->ch_layout.nb_channels);
  log("[input] format: %s", av_get_sample_fmt_name((AVSampleFormat) audioStream->codecpar->format));
  log("[input] duration: %f", (double) formatContext->duration / AV_TIME_BASE);
  log("[input] frame_size: %d", codecContext->frame_size);
  log("[compress] estimated_samples: %ld", estimated_samples);
  log("[compress] estimated_frames: %ld", estimated_frames);
  log("[compress] block_size: %s", (group_frames) ? "auto": to_string(block_size).c_str());
  
  int size = FFMAX(estimated_frames + 100, (int64_t) dest_size);
  vector<float> packed_buffer;
  packed_buffer.reserve(size);
  
  int sample_id = 0;
  float max_value = 0;
  
  while (true) {
    result = av_read_frame(formatContext, audioPacket);
    
    if (result < 0) {
      if (result == AVERROR_EOF || avio_feof(formatContext->pb)) {
        avcodec_send_packet(codecContext, audioPacket);
        av_packet_unref(audioPacket);
        break;
      }
      
      log("[ERROR] av_read_frame: %s", av_err2str(result));
      continue;
    }
    
    if (audioPacket->stream_index != audioStream->index) {
      av_packet_unref(audioPacket);
      continue;
    }
    
    result = avcodec_send_packet(codecContext, audioPacket);
    if (result < 0) {
      log("[compress] avcodec_send_packet: %d: %s", result, av_err2str(result));
      break;
    }
    av_packet_unref(audioPacket);
    
    while (result >= 0) {
      result = avcodec_receive_frame(codecContext, audioFrame);
      if (result == AVERROR_EOF) {
        log("[compress] avcodec_receive_frame EOF");
        avcodec_flush_buffers(codecContext);
        break;
      }
      if (result == AVERROR(EAGAIN)) break;
      if (result < 0) {
        log("[compress] avcodec_receive_frame: %s", av_err2str(result));
        break;
      }
      
      if (group_frames) block_size = audioFrame->nb_samples;
      int dataChannels = audioStream->codecpar->ch_layout.nb_channels;
      
      for (int sid = 0; sid < audioFrame->nb_samples; sid++) {
        float* channel = (float*) audioFrame->data[0];
        float sample = FFABS(channel[sid]);
        
        if (dataChannels == 2) {
          float* channel = (float*) audioFrame->data[1];
          sample = FFMAX(sample, FFABS(channel[sid]));
        }
        
        if (sample > max_value) max_value = sample;
        sample_id++;
       
        if (sample_id >= block_size) {
          packed_buffer.push_back(max_value);
          max_value = 0;
          sample_id = 0;
        }
      }
      
      av_frame_unref(audioFrame);
    }
  }
  
  int total_buf_size = packed_buffer.size();
  log("[compress] total_buf_size: %d", total_buf_size);
  
  // Fill result buffer
  if (total_buf_size > dest_size) {
    // normalize data to fit in dest_size
    int unit_size = total_buf_size / dest_size;
    int over_size = total_buf_size % dest_size;
    
    // step for resizing arrays: (old_len - 1) / (new_len - 1)
    // only elements that are 'step' away will be taken
    float step = (float) (total_buf_size - 1) / (total_buf_size - over_size - 1);
    
    log("[compress] unit_size: %d", unit_size);
    log("[compress] over_size: %d", over_size);
    log("[compress] step: %f", step);
    
    int data_id = 0;
    float block_sum = 0;
    int block_counter = 0;
    max_value = 0;
    
    for (int i = 0; i < total_buf_size - over_size; ++i) {
      int id = round(i * step);
      block_sum += packed_buffer[id];
      block_counter++;
      
      if (block_counter == unit_size) {
        float unit_max = block_sum / unit_size;
        compressed_data[data_id++] = unit_max;
        block_sum = 0;
        block_counter = 0;
      }
    }
  }
  else {
    log("Compressed size is <= then requested");
    for (int i = 0; i < total_buf_size; ++i) {
      compressed_data[i] = packed_buffer[i];
    }
  }
  
  av_frame_free(&audioFrame);
  av_packet_free(&audioPacket);
  
  return 0;
}


void onDraw(SDL_Renderer* renderer) {
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
  
  float center = (float) view_height / 2;
  int x = 0;
  
  for (int i = 0; i < data_size; i++, x++) {
    float h1 = waveformData[i];
    float y0 = center - h1;
    float y1 = center + h1;
    
    SDL_RenderDrawLine(renderer, x, y0, x, y1);
  }
  
  SDL_RenderPresent(renderer);
}

void buildWaveform(string audioPath) {
  float* pixel_data = new float[data_size];
  memset(pixel_data, 0, data_size * sizeof(float));
  
  int result = compressSamples(audioPath, pixel_data, data_size);
  
  if (result != 0) {
    log("[ERROR] compressSamples: %d\n", result);
    return;
  }
  
  for (size_t i = 0; i < data_size; i++) {
    waveformData[i] = (short) (pixel_data[i] * view_height / 2);
  }
  delete[] pixel_data;
}


int main(int argc, char** argv) {
  string filePath = string(argv[1]);
  buildWaveform(filePath);
  
  bool quit = false;
  SDL_Event event;

  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window* window = SDL_CreateWindow("Waveform", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, view_width, view_height, 0);
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

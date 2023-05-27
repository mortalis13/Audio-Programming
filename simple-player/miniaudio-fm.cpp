#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <conio.h>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

using namespace std;


string filePath = "Italian Serenade.mp3";

const int ENGINE_SAMPLE_RATE = 44100;
const int ENGINE_CHANNELS = 2;
  

AVFormatContext* formatContext = NULL;
AVCodecContext* codecContext = NULL;
SwrContext* swrContext = NULL;

AVStream* audioStream = NULL;
const AVCodec* audioCodec = NULL;

AVPacket* audioPacket = NULL;
AVFrame* audioFrame = NULL;

short* globalBuffer = NULL;
int bytesWritten = 0;
int64_t bytesToWrite = 0;

bool isPlaying = true;


av_always_inline char* _av_err2str(int errnum) {
  char str[AV_ERROR_MAX_STRING_SIZE];
  return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}

void cleanup() {
  avformat_close_input(&formatContext);
  avcodec_free_context(&codecContext);
  swr_free(&swrContext);
  av_frame_free(&audioFrame);
  av_packet_free(&audioPacket);
}

void printResamplerParameters(AVStream* audioStream, AVChannelLayout outChannelLayout, int32_t outSampleRate, AVSampleFormat outSampleFormat) {
  printf("===Resampler params===\n");
  printf("Channels: %d => %d\n", audioStream->codecpar->ch_layout.nb_channels, outChannelLayout.nb_channels);
  printf("Sample rate: %d => %d\n", audioStream->codecpar->sample_rate, outSampleRate);
  printf("Sample format: %s => %s\n", av_get_sample_fmt_name((AVSampleFormat) audioStream->codecpar->format), av_get_sample_fmt_name(outSampleFormat));
  printf("===END Resampler params===\n\n");
}

void printCodecParameters(AVCodecParameters* codecParams) {
  printf("===Codec params===\n");
  printf("Channels: %d\n", codecParams->ch_layout.nb_channels);
  printf("Channel layout: order %d, mask %d\n", codecParams->ch_layout.order, (int) codecParams->ch_layout.u.mask);
  printf("Sample rate: %d\n", codecParams->sample_rate);
  printf("Frame size: %d\n", codecParams->frame_size);
  printf("Format: %s\n", av_get_sample_fmt_name((AVSampleFormat) codecParams->format));
  printf("Bytes per sample %d\n", av_get_bytes_per_sample((AVSampleFormat) codecParams->format));
  printf("===END Codec params===\n\n");
}

int getNextFrame() {
  int result = -1;
  av_freep(&globalBuffer);
  
  result = av_read_frame(formatContext, audioPacket);
  if (result != 0) {
    printf("Cannot read frame: %s\n", _av_err2str(result));
    cleanup();
    return result;
  }
  
  if (audioPacket->stream_index == audioStream->index && audioPacket->size > 0) {
    result = avcodec_send_packet(codecContext, audioPacket);
    if (result != 0) {
      printf("avcodec_send_packet error: %s\n", _av_err2str(result));
      return result;
    }

    result = avcodec_receive_frame(codecContext, audioFrame);
    if (result == AVERROR(EAGAIN)) {
      printf("avcodec_receive_frame returned EAGAIN\n");
      av_packet_unref(audioPacket);
      cleanup();
      // --End stream
      return 1;
    }
    else if (result != 0) {
      printf("avcodec_receive_frame error: %s\n", _av_err2str(result));
      cleanup();
      return result;
    }

    // Resample
    int64_t swr_delay = swr_get_delay(swrContext, audioFrame->sample_rate);
    int32_t dst_nb_samples = (int32_t) av_rescale_rnd(swr_delay + audioFrame->nb_samples, ENGINE_SAMPLE_RATE, audioFrame->sample_rate, AV_ROUND_UP);
    
    av_samples_alloc((uint8_t **) &globalBuffer, nullptr, ENGINE_CHANNELS, dst_nb_samples, AV_SAMPLE_FMT_FLT, 0);
    int frame_count = swr_convert(swrContext, (uint8_t **) &globalBuffer, dst_nb_samples, (const uint8_t **) audioFrame->data, audioFrame->nb_samples);

    bytesToWrite = frame_count * sizeof(float) * ENGINE_CHANNELS;

    av_frame_unref(audioFrame);
  }
  
  av_packet_unref(audioPacket);
  return result;
}


int loadFile(string filePath) {
  int result = 0;

  result = avformat_open_input(&formatContext, filePath.c_str(), NULL, NULL);
  if (result < 0) {
    printf("Failed to open file. Error code: %s\n", _av_err2str(result));
    cleanup();
    return result;
  }
  
  result = avformat_find_stream_info(formatContext, NULL);
  if (result < 0) {
    printf("Failed to find stream info. Error code: %s\n", _av_err2str(result));
    cleanup();
    return result;
  }
  
  int streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  audioStream = nullptr;
  if (streamIndex >= 0) {
    audioStream = formatContext->streams[streamIndex];
  }
  
  if (audioStream == nullptr || audioStream->codecpar == nullptr) {
    printf("Could not find a suitable audio stream to decode\n");
    cleanup();
    return -1;
  }

  printCodecParameters(audioStream->codecpar);

  audioCodec = avcodec_find_decoder(audioStream->codecpar->codec_id);
  if (!audioCodec){
    printf("Could not find codec with ID: %d\n", audioStream->codecpar->codec_id);
    cleanup();
    return -1;
  }
  
  codecContext = avcodec_alloc_context3(audioCodec);
  if (!codecContext){
    printf("Failed to allocate codec context\n");
    cleanup();
    return -1;
  }

  result = avcodec_parameters_to_context(codecContext, audioStream->codecpar);
  if (result < 0) {
    printf("Failed to copy codec parameters to codec context\n");
    cleanup();
    return result;
  }

  result = avcodec_open2(codecContext, audioCodec, nullptr);
  if (result < 0) {
    printf("Could not open codec\n");
    cleanup();
    return result;
  }

  // Fix warning "Could not update timestamps for skipped samples"
  codecContext->pkt_timebase = audioStream->time_base;

  swrContext = swr_alloc();
  if (!swrContext) {
    printf("Could not allocate resampler context\n");
    cleanup();
    return -1;
  }
  
  AVChannelLayout outChannelLayout;
  av_channel_layout_default(&outChannelLayout, ENGINE_CHANNELS);
  int32_t outSampleRate = ENGINE_SAMPLE_RATE;
  AVSampleFormat outSampleFormat = AV_SAMPLE_FMT_FLT;
  
  printResamplerParameters(audioStream, outChannelLayout, outSampleRate, outSampleFormat);
  
  av_opt_set_chlayout(swrContext, "in_chlayout", &audioStream->codecpar->ch_layout, 0);
  av_opt_set_int(swrContext, "in_sample_rate", audioStream->codecpar->sample_rate, 0);
  av_opt_set_sample_fmt(swrContext, "in_sample_fmt", (AVSampleFormat) audioStream->codecpar->format, 0);
  
  av_opt_set_chlayout(swrContext, "out_chlayout", &outChannelLayout, 0);
  av_opt_set_int(swrContext, "out_sample_rate", outSampleRate, 0);
  av_opt_set_sample_fmt(swrContext, "out_sample_fmt", outSampleFormat, 0);

  av_opt_set_int(swrContext, "force_resampling", 1, 0);

  result = swr_init(swrContext);
  if (result < 0){
    printf("Failed to initialize the resampling context. Error: %s\n", _av_err2str(result));
    cleanup();
    return result;
  }
  
  return result;
}


int initDecoder() {
  int result = 0;
  
  audioPacket = av_packet_alloc();
  if (!audioPacket) {
    printf("Could not allocate audio packet\n");
    cleanup();
  }
  
  audioFrame = av_frame_alloc();
  if (!audioFrame) {
    printf("Could not allocate audio frame\n");
    cleanup();
  }
  
  return result;
}


int seekToTime(int time_s) {
  // int64_t ts = (int64_t) (time_s * (double) audioStream->time_base.den / (double) audioStream->time_base.num);
  // int64_t ts = av_rescale_q(time_s * AV_TIME_BASE, AV_TIME_BASE_Q, audioStream->time_base);
  int64_t ts = av_rescale(time_s, audioStream->time_base.den, audioStream->time_base.num);
  
  printf("Seeking to %ds, timestamp: %d\n", time_s, (int) ts);
  
  int result = av_seek_frame(formatContext, audioStream->index, ts, AVSEEK_FLAG_FRAME);
  if (result < 0) {
    printf("Error when seeking to %d\n", time_s);
  }
  
  return result;
}


void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
  if (!isPlaying) return;
  float* stream = (float*) pOutput;
  
  for (uint32_t i = 0; i < frameCount * pDevice->playback.channels; i++) {
    if (bytesWritten >= bytesToWrite) {
      int result = getNextFrame();
      
      bytesWritten = 0;
      if (result != 0) {
        isPlaying = false;
        return;
      }
    }
    
    float sample;
    memcpy(&sample, (uint8_t*) globalBuffer + bytesWritten, sizeof(float));
    bytesWritten += sizeof(float);
    
    *stream++ = sample;
  }
}


int main() {
  // ffmpeg
  if (loadFile(filePath) != 0) return -1;
  if (initDecoder() != 0) return -1;
  seekToTime(0);

  // miniaudio
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = ENGINE_CHANNELS;
  config.sampleRate = ENGINE_SAMPLE_RATE;
  config.dataCallback = data_callback;

  ma_device device;
  if (ma_device_init(NULL, &config, &device) != MA_SUCCESS) {
    printf("Failed to initialize the device\n");
    return -1;
  }

  ma_device_start(&device);

  printf("Press ESC to quit...\n");
  char c = 0;
  while (c != 0x1b) {
    c = getch();
    printf("Char: %c 0x%x\n", c, c);
  }

  ma_device_uninit(&device);
  
  return 0;
}

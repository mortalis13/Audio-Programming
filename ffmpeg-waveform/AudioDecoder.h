#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

#include <string>

using namespace std;


class AudioDecoder {

public:
  AudioDecoder();
  ~AudioDecoder();
  
  int compressSamples(string filePath, float* samples, int dest_size);
  void stopCompression();

private:
  static void printCodecParameters(AVCodecParameters* codecParams);
  static void printResamplerParameters(AVCodecParameters* codecParams, int channels, int sample_rate, AVSampleFormat format);
  
  int loadCodec(string filePath);
  int loadResampler(int channels, int sample_rate, AVSampleFormat format);
  void cleanup();
  
private:
  bool compressing = false;
  
  int32_t dataChannels = 0;
  
  int audioStreamIndex = -1;
  
  AVFormatContext* formatContext = NULL;
  AVCodecContext* codecContext = NULL;
  SwrContext* swrContext = NULL;
  
};
#endif //AUDIO_DECODER_H
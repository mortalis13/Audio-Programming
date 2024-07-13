#include <string>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

using namespace std;


av_always_inline char* _av_err2str(int errnum) {
  char str[AV_ERROR_MAX_STRING_SIZE];
  return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}


AVFormatContext* formatContext = NULL;


int decoderInit(string filePath) {
  printf("decoderInit()\n");
  int result = -1;
  
  result = avformat_open_input(&formatContext, filePath.c_str(), NULL, NULL);
  if (result < 0) {
    printf("Failed to open file. Error code: %s\n", _av_err2str(result));
    return result;
  }
  
  printf("Input opened\n");
  
  return result;
}


int streamInit() {
  printf("streamInit()\n");
  int result = -1;
  
  // start stream with ffmpeg -re -i file.mp3 -c copy -f rtp rtp://127.0.0.1:1234
  string url = "rtp://127.0.0.1:1234";
  
  result = avformat_open_input(&formatContext, url.c_str(), NULL, NULL);
  if (result < 0) {
    printf("Failed to open input. Error code: %s\n", _av_err2str(result));
    return result;
  }
  
  printf("Input opened\n");
  
  return result;
}


int main() {
  printf("ffmpeg-tests\n");
  
  string filePath = "d:/file.mp3";
  decoderInit(filePath);
  
  // streamInit();
  return 0;
}

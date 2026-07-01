#include <string>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace std;

av_always_inline char* _av_err2str(int errnum) {
  char str[AV_ERROR_MAX_STRING_SIZE];
  return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}

int decoderInit(string filePath) {
  printf("decoderInit()\n");
  int result = -1;
  
  AVFormatContext* formatContext = NULL;
  
  result = avformat_open_input(&formatContext, filePath.c_str(), NULL, NULL);
  if (result < 0) {
    printf("Failed to open file. Error code: %s\n", _av_err2str(result));
    return result;
  }
  
  result = avformat_find_stream_info(formatContext, NULL);
  if (result < 0) {
    printf("Failed to load streams. Error code: %s\n", _av_err2str(result));
    return result;
  }
  
  printf("\nContext loaded\n\n");
  
  printf("nb_streams: %d\n", formatContext->nb_streams);
  printf("flags: 0x%x\n", formatContext->flags);
  
  printf("\n[Stream]\n");
  printf("time_base: %d\n", formatContext->streams[0]->time_base);
  printf("duration: %d\n", formatContext->streams[0]->duration);
  
  printf("\n[Codec]\n");
  printf("codec_type: %s\n", av_get_media_type_string(formatContext->streams[0]->codecpar->codec_type));
  printf("codec_id: %s\n", avcodec_get_name(formatContext->streams[0]->codecpar->codec_id));
  printf("format: %s\n", av_get_sample_fmt_name((AVSampleFormat) formatContext->streams[0]->codecpar->format));
  printf("bit_rate: %d\n", formatContext->streams[0]->codecpar->bit_rate);
  printf("width: %d\n", formatContext->streams[0]->codecpar->width);
  printf("height: %d\n", formatContext->streams[0]->codecpar->height);
  printf("sample_aspect_ratio: %d:%d\n", formatContext->streams[0]->codecpar->sample_aspect_ratio.num, formatContext->streams[0]->codecpar->sample_aspect_ratio.den);
  printf("sample_rate: %d\n", formatContext->streams[0]->codecpar->sample_rate);
  printf("frame_size: %d\n", formatContext->streams[0]->codecpar->frame_size);
  printf("nb_channels: %d\n", formatContext->streams[0]->codecpar->ch_layout.nb_channels);
  
  printf("\n[Metadata]\n");
  AVDictionaryEntry* tag = NULL;
  while ((tag = av_dict_get(formatContext->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
    printf("%s: %s\n", tag->key, tag->value);
  }
  printf("\n");
  
  printf("First entry:\n");
  AVDictionaryEntry* entry = av_dict_get(formatContext->metadata, "", NULL, AV_DICT_IGNORE_SUFFIX);
  if (entry) {
    AVDictionaryEntry *tagByKey = av_dict_get(formatContext->metadata, entry->key, NULL, 0);
    if (tagByKey) {
      printf("%s :: %s\n", entry->key, tagByKey->value);
    }
  }
  
  avformat_close_input(&formatContext);
  
  return result;
}

int main(int argc, char** argv) {
  av_log_set_level(AV_LOG_TRACE);
  
  // input: file path or stream URL like rtp://127.0.0.1:1234
  // to start a stream: ffmpeg -re -i file.mp3 -c copy -f rtp rtp://127.0.0.1:1234
  string input = string(argv[1]);
  
  decoderInit(input);

  return 0;
}

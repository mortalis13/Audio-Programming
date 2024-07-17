#include <string>
#include <vector>
#include <cmath>

extern "C" {
#include "libavutil/timestamp.h"
#include "libavutil/samplefmt.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
}

struct Status {
  int status;
  std::string error;
};


static AVFormatContext *format_context = NULL;
static AVCodecContext *codec_context;

static AVFrame *frame = NULL;
static AVPacket *pkt = NULL;

static int audio_stream_idx = -1;

static int frame_id = 0;
static float pixel_sum = 0;
static int pixel_index = 0;

static bool decoderUnloaded = false;


static int decode_packet(AVCodecContext *_codec_context, const AVPacket *pkt, std::vector<float> &buffer, int block_size, bool is_planar) {
  int ret = 0;
  int pixel_size = block_size;
  
  if (decoderUnloaded) {
    printf("decoder unloaded");
    return 0;
  }
  
  // submit the packet to the decoder
  ret = avcodec_send_packet(_codec_context, pkt);
  if (ret < 0) {
    printf("PACKET_SUBMITTING_ERROR: %d: %s", ret, av_err2str(ret));
    return ret;
  }
  
  // get all the available frames from the decoder
  while (ret >= 0) {
    ret = avcodec_receive_frame(_codec_context, frame);
    if (ret < 0) {
      if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) return 0;
      printf("DECODING_ERROR: %d: %s", ret, av_err2str(ret));
      return ret;
    }
    
    // if (frame_id++ % 2 != 0) continue;
    
    if (block_size == 0) pixel_size = frame->nb_samples;
    
    for (int sid = 0; sid < frame->nb_samples; sid++) {
      if (decoderUnloaded) {
        printf("decoder unloaded");
        av_frame_unref(frame);
        return 0;
      }
      
      float sample = 0;
      uint8_t* base;
      int offset;
      
      int num_channels = frame->ch_layout.nb_channels;
      
      for (int ch = 0; ch < num_channels; ch++) {
        float channelSample = 0;
        
        if (is_planar) {
          base = frame->extended_data[ch];
          offset = sid;
        }
        else {
          base = frame->data[0];
          offset = sid * num_channels + ch;
        }
        
        switch (_codec_context->sample_fmt) {
          case AV_SAMPLE_FMT_U8:
          case AV_SAMPLE_FMT_U8P:
            channelSample = static_cast<float>(reinterpret_cast<uint8_t*>(base)[offset]);
            break;
          
          case AV_SAMPLE_FMT_S16:
          case AV_SAMPLE_FMT_S16P:
            channelSample = static_cast<float>(reinterpret_cast<int16_t*>(base)[offset]);
            break;
          
          case AV_SAMPLE_FMT_S32:
          case AV_SAMPLE_FMT_S32P:
            channelSample = static_cast<float>(reinterpret_cast<int32_t*>(base)[offset]);
            break;
          
          case AV_SAMPLE_FMT_FLT:
          case AV_SAMPLE_FMT_FLTP:
            channelSample = reinterpret_cast<float*>(base)[offset];
            break;
          
          default:
            break;
        }
        
        sample += channelSample;
      }
      
      sample /= num_channels;
      pixel_sum += std::abs(sample);
      
      // pack samples
      pixel_index++;
      if (pixel_index >= pixel_size) {
        float pixel = pixel_sum / pixel_size;
        buffer.push_back(pixel);
        
        pixel_sum = 0;
        pixel_index = 0;
      }
    } // for nb_samples
    
    av_frame_unref(frame);
    if (ret < 0) return ret;
  }

  return 0;
}


static void cleanup() {
  avcodec_free_context(&codec_context);
  avformat_close_input(&format_context);
  av_packet_free(&pkt);
  av_frame_free(&frame);
}


static int init_codec(const char* audio_path, enum AVMediaType type) {
  int ret;
  AVStream *stream;
  const AVCodec *codec = NULL;
  AVDictionary *opts = NULL;
  
  // open input file, and allocate format context
  ret = avformat_open_input(&format_context, audio_path, NULL, NULL);
  if (ret < 0) {
    printf("FILE_OPEN_IO: %d: %s", ret, av_err2str(ret));
    return ret;
  }

  // retrieve stream information
  ret = avformat_find_stream_info(format_context, NULL);
  if (ret < 0) {
    printf("STREAM_INFO_NOT_FOUND_ERROR: %d: %s", ret, av_err2str(ret));
    return ret;
  }

  ret = av_find_best_stream(format_context, type, -1, -1, NULL, 0);
  if (ret < 0) {
    printf("STREAM_NOT_FOUND_ERROR: %d: %s", ret, av_err2str(ret));
    return ret;
  }
  
  audio_stream_idx = ret;
  stream = format_context->streams[audio_stream_idx];

  // find decoder for the stream
  codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    printf("CODEC_NOT_FOUND_ERROR: %x", stream->codecpar->codec_id);
    return AVERROR(EINVAL);
  }

  // Allocate a codec context for the decoder
  codec_context = avcodec_alloc_context3(codec);
  if (!codec_context) {
    ret = AVERROR(ENOMEM);
    printf("CODEC_CONTEXT_ALLOC_ERROR: %d: %s", ret, av_err2str(ret));
    return ret;
  }

  // Copy codec parameters from input stream to output codec context
  ret = avcodec_parameters_to_context(codec_context, stream->codecpar);
  if (ret < 0) {
    printf("CODEC_PARAMETERS_COPY_ERROR: %d: %s", ret, av_err2str(ret));
    return ret;
  }

  // Init the decoders
  ret = avcodec_open2(codec_context, codec, &opts);
  if (ret < 0) {
    printf("CODEC_OPEN_ERROR: %d: %s", ret, av_err2str(ret));
    return ret;
  }
  
  frame = av_frame_alloc();
  if (!frame) {
    ret = AVERROR(ENOMEM);
    printf("FRAME_ALLOC_ERROR: %d: %s", ret, av_err2str(ret));
    return ret;
  }

  pkt = av_packet_alloc();
  if (!pkt) {
    ret = AVERROR(ENOMEM);
    printf("PACKET_ALLOC_ERROR: %d: %s", ret, av_err2str(ret));
    return ret;
  }

  return 0;
}


void stopDecoding() {
  decoderUnloaded = true;
}


void decodeSamples(string audio_path, int view_width, int view_height) {
  int ret = 0;
  clock_t start_time = clock();
  printf("--> Decode Start");
  
  decoderUnloaded = false;
  
  frame_id = 0;
  pixel_sum = 0;
  pixel_index = 0;
  
  // input params
  const char* input_audio_path = audio_path.c_str();

  // prepare result containers
  std::vector<float> pixel_data;
  std::vector<float> pixel_buffer;

  ret = init_codec(input_audio_path, AVMEDIA_TYPE_AUDIO);
  if (ret != 0) {
    cleanup();
    return;
  }
    
  // estimate if it's enough to group each frame or a custom block size should be used
  int64_t total_samples = (int64_t) ((format_context->duration / (double) AV_TIME_BASE) * codec_context->sample_rate);
  int64_t estimated_frames = (codec_context->frame_size != 0) ? total_samples / codec_context->frame_size: 0;
  
  // printf("duration: %ld", format_context->duration);
  // printf("frame_size: %d", codec_context->frame_size);
  // printf("total_samples: %ld", total_samples);
  // printf("estimated_frames: %ld", estimated_frames);
  
  int block_size = 0;
  if (estimated_frames < view_width) {
    block_size = 10;
    if (total_samples < 0) {
      printf("total_samples < 0");
      block_size = 1000;
    }
    else {
      while (view_width * block_size * 10 < total_samples) block_size *= 10;
    }
  }
  
  printf("block_size: %d", block_size);
  
  bool is_planar = av_sample_fmt_is_planar(codec_context->sample_fmt);
  
  // read frames from the file
  while (av_read_frame(format_context, pkt) >= 0) {
    if (decoderUnloaded) {
      printf("decoder unloaded: %s", input_audio_path);
      cleanup();
      return;
    }
    
    bool is_audio_stream = pkt->stream_index == audio_stream_idx;
    
    if (is_audio_stream) {
      ret = decode_packet(codec_context, pkt, pixel_buffer, block_size, is_planar);
    }
    
    av_packet_unref(pkt);
    if (ret < 0) break;
  }

  // flush the decoders
  if (codec_context) {
    decode_packet(codec_context, NULL, pixel_buffer, block_size, is_planar);
  }
  
  // normalize data to fit in view_width
  int total_buf_size = pixel_buffer.size();
  int pixel_size = (int) ((float) total_buf_size / view_width);
  int over_size = total_buf_size % view_width;
  
  // printf("pixel_size: %d", pixel_size);
  printf("total_buf_size: %d", total_buf_size);
  // printf("over_size: %f", over_size);
  
  pixel_sum = 0;
  float max_pixel = 0;

  int block_counter = 0;
  
  // step for resizing arrays: (old_len - 1) / (new_len - 1)
  // only elements that are 'step' away will be taken
  float step = ((float) total_buf_size - 1) / (total_buf_size - over_size - 1);
  printf("step: %f", step);
  
  // [alt rescale index calc]
  // float scale_ratio = (float) total_buf_size / (total_buf_size - over_size);
  // float scale_ratio_2 = (float) total_buf_size / (2 * (total_buf_size - over_size));
  
  for (int i = 0; i < total_buf_size - over_size; ++i) {
    // [alt rescale index calc]
    // pixel_sum += pixel_buffer[std::round(i * scale_ratio + scale_ratio_2)];
    
    pixel_sum += pixel_buffer[std::round(i * step)];
    block_counter++;
    
    if (block_counter == pixel_size) {
      float pixel = pixel_sum / pixel_size;
      if (pixel > max_pixel) max_pixel = pixel;
      
      pixel_data.push_back(pixel);
      
      pixel_sum = 0;
      block_counter = 0;
    }
  }
  pixel_buffer.clear();
  
  cleanup();
  
  // Prepare result
  int total_pixels = pixel_data.size();
  printf("pixel_data: %d", total_pixels);
  
  // ----------
  short* waveform = new short[total_pixels]
  for (size_t i = 0; i < total_pixels; i++) {
    short value = (short) (pixel_data[i] * view_height / 2 / max_pixel);
    waveform[i] = value;
  }
  pixel_data.clear();

  printf("--> Decode End: %.2f", double(clock() - start_time) / CLOCKS_PER_SEC);

  return;
}

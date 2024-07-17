#include "AudioDecoder.h"

#include <vector>
#include <cmath>


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


AudioDecoder::AudioDecoder() {}

AudioDecoder::~AudioDecoder() {
  log("~AudioDecoder()");
  this->cleanup();
}

int AudioDecoder::loadCodec(string filePath) {
  log("loadCodec() -start- => %s", filePath.c_str());
  int result = -1;
  
  result = avformat_open_input(&formatContext, filePath.c_str(), NULL, NULL);
  if (result < 0) {
    log("Failed to open file: (%d) %s", result, _av_err2str(result));
    this->cleanup();
    return result;
  }
  log("AV: input opened");
  
  result = avformat_find_stream_info(formatContext, NULL);
  if (result < 0) {
    log("Failed to find stream info: (%d) %s", result, _av_err2str(result));
    this->cleanup();
    return result;
  }
  log("AV: stream info found");
  
  AVStream* audioStream = nullptr;
  this->audioStreamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (audioStreamIndex >= 0) {
    audioStream = formatContext->streams[audioStreamIndex];
  }
  
  if (audioStream == nullptr || audioStream->codecpar == nullptr) {
    log("Could not find a suitable audio stream to decode");
    this->cleanup();
    return -1;
  }
  log("AV: audio stream found");

  this->dataChannels = audioStream->codecpar->ch_layout.nb_channels;
  
  const AVCodec* audioCodec = avcodec_find_decoder(audioStream->codecpar->codec_id);
  if (!audioCodec){
    log("Could not find codec with ID: %d (%s)", audioStream->codecpar->codec_id, avcodec_get_name(audioStream->codecpar->codec_id));
    this->cleanup();
    return -1;
  }
  log("AV: codec found");
  
  codecContext = avcodec_alloc_context3(audioCodec);
  if (!codecContext){
    log("Failed to allocate codec context");
    this->cleanup();
    return -1;
  }
  log("AV: codec context allocated");

  codecContext->pkt_timebase = audioStream->time_base;
  
  result = avcodec_parameters_to_context(codecContext, audioStream->codecpar);
  if (result < 0) {
    log("Failed to copy codec parameters to codec context: (%d) %s", result, _av_err2str(result));
    this->cleanup();
    return result;
  }
  log("AV: codec parameters copied");

  result = avcodec_open2(codecContext, audioCodec, nullptr);
  if (result < 0) {
    log("Could not open codec: (%d) %s", result, _av_err2str(result));
    this->cleanup();
    return result;
  }
  log("AV: codec opened");
  
  log("loadCodec() -end- => %s", filePath.c_str());
  return result;
}

int AudioDecoder::loadResampler(int channels, int sample_rate, AVSampleFormat format) {
  int result = -1;
  
  AVCodecParameters* codecParams = formatContext->streams[this->audioStreamIndex]->codecpar;
  printResamplerParameters(codecParams, channels, sample_rate, format);
  
  swrContext = swr_alloc();
  if (!swrContext) {
    log("Could not allocate resampler context");
    this->cleanup();
    return -1;
  }
  log("AV: resampler context allocated");
  
  AVChannelLayout channel_layout;
  av_channel_layout_default(&channel_layout, channels);
  
  av_opt_set_chlayout(swrContext, "in_chlayout", &codecParams->ch_layout, 0);
  av_opt_set_int(swrContext, "in_sample_rate", codecParams->sample_rate, 0);
  av_opt_set_sample_fmt(swrContext, "in_sample_fmt", (AVSampleFormat) codecParams->format, 0);
  
  av_opt_set_chlayout(swrContext, "out_chlayout", &channel_layout, 0);
  av_opt_set_int(swrContext, "out_sample_rate", sample_rate, 0);
  av_opt_set_sample_fmt(swrContext, "out_sample_fmt", format, 0);

  result = swr_init(swrContext);
  if (result < 0) {
    log("Failed to initialize resampler context: (%d) %s", result, _av_err2str(result));
    this->cleanup();
    return result;
  }
  log("AV: resampler initialized");
  
  return 0;
}

void AudioDecoder::cleanup() {
  log("cleanup()");
  avformat_close_input(&formatContext);
  avcodec_free_context(&codecContext);
  swr_free(&swrContext);
}


void AudioDecoder::printResamplerParameters(AVCodecParameters* codecParams, int channels, int sample_rate, AVSampleFormat format) {
  log("===Resampler params===");
  log("Channels: %d => %d", codecParams->ch_layout.nb_channels, channels);
  log("Sample rate: %d => %d", codecParams->sample_rate, sample_rate);
  log("Sample format: %s => %s", av_get_sample_fmt_name((AVSampleFormat) codecParams->format), av_get_sample_fmt_name(format));
  log("===END Resampler params===");
  log("");
}

void AudioDecoder::printCodecParameters(AVCodecParameters* codecParams) {
  log("===Codec params===");
  log("Channels: %d", codecParams->ch_layout.nb_channels);
  log("Channel layout: order %d, mask %d", codecParams->ch_layout.order, (int) codecParams->ch_layout.u.mask);
  log("Sample rate: %d", codecParams->sample_rate);
  log("Frame size: %d", codecParams->frame_size);
  log("Sample format: %s", av_get_sample_fmt_name((AVSampleFormat) codecParams->format));
  log("Is planar: %d", av_sample_fmt_is_planar((AVSampleFormat) codecParams->format));
  log("Bytes per sample: %d", av_get_bytes_per_sample((AVSampleFormat) codecParams->format));
  log("Bitrate: %d", (int) (codecParams->bit_rate / 1000));
  log("Codec type: %s", av_get_media_type_string(codecParams->codec_type));
  log("Codec ID: %s", avcodec_get_name(codecParams->codec_id));
  log("===END Codec params===");
  log("");
}


int AudioDecoder::compressSamples(string filePath, float* compressed_data, int dest_size) {
  log("compressSamples() -start- -> %d", dest_size);
  compressing = true;
  int result;
  clock_t start_time = clock();
  
  result = loadCodec(filePath);
  if (result != 0) return result;
  
  AVCodecParameters* codecParams = formatContext->streams[this->audioStreamIndex]->codecpar;
  result = loadResampler(1, codecParams->sample_rate, AV_SAMPLE_FMT_FLTP);
  if (result < 0) return result;
  
  AVPacket* audioPacket;
  AVFrame* audioFrame;
  
  audioPacket = av_packet_alloc();
  if (!audioPacket) {
    log("[compress] Could not allocate audio packet");
    this->cleanup();
    return -1;
  }
  
  audioFrame = av_frame_alloc();
  if (!audioFrame) {
    log("[compress] Could not allocate audio frame");
    av_packet_free(&audioPacket);
    this->cleanup();
    return -1;
  }
  
  // estimate if it's enough to group each frame or a custom block size should be used
  int64_t estimated_samples = (int64_t) ((double) formatContext->duration / AV_TIME_BASE * codecContext->sample_rate);
  int64_t estimated_frames = (codecContext->frame_size != 0) ? estimated_samples / codecContext->frame_size: 0;
  
  int block_size = 10;
  if (estimated_samples < dest_size * block_size) block_size = 1;
  bool group_frames = (estimated_frames > dest_size);
  
  log("[compress] duration: %f", (double) formatContext->duration / AV_TIME_BASE);
  log("[compress] frame_size: %d", codecContext->frame_size);
  log("[compress] estimated_samples: %ld", estimated_samples);
  log("[compress] estimated_frames: %ld", estimated_frames);
  log("[compress] block_size: %s", (group_frames) ? "auto": to_string(block_size).c_str());
  
  // store separately max and min values for each block
  vector<float> packed_buffer_max;
  vector<float> packed_buffer_min;
  int size = max(estimated_frames + 100, (int64_t) dest_size);
  packed_buffer_max.reserve(size);
  packed_buffer_min.reserve(size);
  
  int sample_id = 0;
  float max_value = (float) INT_MIN;
  float min_value = (float) INT_MAX;
  
  while (this->compressing) {
    result = av_read_frame(formatContext, audioPacket);
    if (result < 0) {
      log("[compress] av_read_frame: %s", _av_err2str(result));
      if (result == AVERROR_EOF || avio_feof(formatContext->pb)) {
        avcodec_send_packet(codecContext, audioPacket);
        av_packet_unref(audioPacket);
        break;
      }
      continue;
    }
    
    if (audioPacket->stream_index != this->audioStreamIndex) {
      av_packet_unref(audioPacket);
      continue;
    }
    
    result = avcodec_send_packet(codecContext, audioPacket);
    if (result < 0) {
      log("[compress] avcodec_send_packet: %d: %s", result, _av_err2str(result));
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
        log("[compress] avcodec_receive_frame: %s", _av_err2str(result));
        break;
      }
      
      int num_samples = audioFrame->nb_samples;
      
      // Resample
      float* audio_buffer;
      av_samples_alloc((uint8_t**) &audio_buffer, nullptr, 1, num_samples, AV_SAMPLE_FMT_FLTP, 0);
      int frame_count = swr_convert(swrContext, (uint8_t**) &audio_buffer, num_samples, (const uint8_t**) audioFrame->data, num_samples);
      
      if (group_frames) block_size = num_samples;
      
      for (int sid = 0; sid < frame_count; sid++) {
        float sample = audio_buffer[sid];
        if (sample > max_value) max_value = sample;
        if (sample < min_value) min_value = sample;
        sample_id++;
       
        // pack samples
        if (sample_id >= block_size) {
          if (max_value > 0 && min_value > 0) min_value = 0;
          if (max_value < 0 && min_value < 0) max_value = 0;
          packed_buffer_max.push_back(max_value);
          packed_buffer_min.push_back(min_value);
          
          max_value = (float) INT_MIN;
          min_value = (float) INT_MAX;
          sample_id = 0;
        }
      }
      
      av_freep(&audio_buffer);
      av_frame_unref(audioFrame);
    }
  }
  
  if (!compressing) {
    log("Compression stopped");
    av_frame_free(&audioFrame);
    av_packet_free(&audioPacket);
    this->cleanup();
    return 1;
  }
  
  int total_buf_size = packed_buffer_max.size();
  log("[compress] total_buf_size: %d", total_buf_size);
  
  if (total_buf_size >= dest_size) {
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
    float block_sum_max = 0;
    float block_sum_min = 0;
    int block_counter = 0;
    max_value = 0;
    
    for (int i = 0; i < total_buf_size - over_size; ++i) {
      int id = round(i * step);
      block_sum_max += packed_buffer_max[id];
      block_sum_min += packed_buffer_min[id];
      block_counter++;
      
      if (block_counter == unit_size) {
        float unit_max = block_sum_max / unit_size;
        float unit_min = block_sum_min / unit_size;
        
        // put pairs of max and min values for each sample group
        compressed_data[data_id++] = unit_max;
        compressed_data[data_id++] = unit_min;
        
        block_sum_max = 0;
        block_sum_min = 0;
        block_counter = 0;
      }
    }
  }
  else {
    log("Compressed size is less then requested");
    for (int i = 0; i < total_buf_size; ++i) {
      compressed_data[i*2] = packed_buffer_max[i];
      compressed_data[i*2+1] = packed_buffer_min[i];
    }
  }
  
  float max_data = INT_MIN;
  for (int i = 0; i < dest_size * 2; ++i) {
    float value = abs(compressed_data[i]);
    if (value > max_data) max_data = value;
  }
  if (max_data == 0) max_data = 1;
  
  for (int i = 0; i < dest_size * 2; ++i) {
    compressed_data[i] /= max_data;
  }
  
  av_frame_free(&audioFrame);
  av_packet_free(&audioPacket);
  this->cleanup();
  
  compressing = false;
  log("compressSamples() -end- (%.2f s)", double(clock() - start_time) / CLOCKS_PER_SEC);
  return 0;
}


void AudioDecoder::stopCompression() {
  log("stopCompression()");
  this->compressing = false;
}

// Decodes audio to raw PCM frames
// Test the output.wav with ffplay
//
// ffplay -ch_layout stereo -ar 44100 -f f32le output.wav
//
// Check the PCM formats with ffplay -formats
// Possible values, depending on the audio encoding, could be: s16le, s32le, f32le, f64le, u8, u16le, u32le

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <string>
#include <fstream>

using namespace std;

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


AVFormatContext* formatContext = NULL;
AVCodecContext* codecContext = NULL;
AVStream* audioStream = NULL;

fstream outFile;


void printCodecParameters(AVCodecParameters* codecParams) {
  log("");
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


int loadCodec(string filePath) {
  int result = -1;

  result = avformat_open_input(&formatContext, filePath.c_str(), NULL, NULL);
  if (result < 0) {
    log("ERROR: Failed to open file: (%d) %s", result, av_err2str(result));
    return result;
  }
  log("AV: input opened");

  result = avformat_find_stream_info(formatContext, NULL);
  if (result < 0) {
    log("ERROR: Failed to find stream info: (%d) %s", result, av_err2str(result));
    return result;
  }
  log("AV: stream info found, total streams: %d", formatContext->nb_streams);

  int audioStreamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (audioStreamIndex >= 0) {
    audioStream = formatContext->streams[audioStreamIndex];
  }

  if (audioStream == NULL || audioStream->codecpar == NULL) {
    log("ERROR: Could not find a suitable audio stream to decode");
    return -1;
  }
  log("AV: audio stream found");

  const AVCodec* audioCodec = avcodec_find_decoder(audioStream->codecpar->codec_id);
  if (!audioCodec){
    log("ERROR: Could not find codec with ID: %d (%s)", audioStream->codecpar->codec_id, avcodec_get_name(audioStream->codecpar->codec_id));
    return -1;
  }
  log("AV: codec found");

  codecContext = avcodec_alloc_context3(audioCodec);
  if (!codecContext){
    log("ERROR: Failed to allocate codec context");
    return -1;
  }
  log("AV: codec context allocated");

  codecContext->pkt_timebase = audioStream->time_base;

  result = avcodec_parameters_to_context(codecContext, audioStream->codecpar);
  if (result < 0) {
    log("ERROR: Failed to copy codec parameters to codec context: (%d) %s", result, av_err2str(result));
    return result;
  }
  log("AV: codec parameters copied");

  result = avcodec_open2(codecContext, audioCodec, NULL);
  if (result < 0) {
    log("ERROR: Could not open codec: (%d) %s", result, av_err2str(result));
    return result;
  }
  log("AV: codec opened");

  printCodecParameters(audioStream->codecpar);

  return result;
}

void processFrame(AVFrame* audioFrame) {
  int sampleSize = av_get_bytes_per_sample(codecContext->sample_fmt);

  for (int i = 0; i < audioFrame->nb_samples; i++) {
    for (int ch = 0; ch < codecContext->ch_layout.nb_channels; ch++) {
      outFile.write((char *) audioFrame->data[ch] + i * sampleSize, sampleSize);
    }
  }
}

int decodeFrames() {
  int result = -1;

  AVPacket* audioPacket;
  AVFrame* audioFrame;

  audioPacket = av_packet_alloc();
  if (!audioPacket) {
    log("ERROR: Could not allocate audio packet");
    goto end;
  }

  audioFrame = av_frame_alloc();
  if (!audioFrame) {
    log("ERROR: Could not allocate audio frame");
    goto end;
  }

  log("Decode start");

  while (true) {
    result = av_read_frame(formatContext, audioPacket);

    if (result < 0) {
      if (result == AVERROR_EOF || avio_feof(formatContext->pb)) {
        avcodec_send_packet(codecContext, audioPacket);
        av_packet_unref(audioPacket);
        break;
      }

      log("ERROR: av_read_frame: %s", av_err2str(result));
      goto end;
    }
    
    if (audioPacket->stream_index != audioStream->index) {
      av_packet_unref(audioPacket);
      continue;
    }

    result = avcodec_send_packet(codecContext, audioPacket);
    if (result != 0) {
      log("ERROR: avcodec_send_packet: (%d) %s", result, av_err2str(result));
      goto end;
    }
    av_packet_unref(audioPacket);

    while (result >= 0) {
      result = avcodec_receive_frame(codecContext, audioFrame);

      if (result == AVERROR_EOF) {
        log("avcodec_receive_frame EOF");
        avcodec_flush_buffers(codecContext);
        goto end;
      }
      if (result == AVERROR(EAGAIN)) continue;
      if (result < 0) {
        log("ERROR: avcodec_receive_frame: (%d) %s", result, av_err2str(result));
        goto end;
      }

      processFrame(audioFrame);

      av_frame_unref(audioFrame);
    }
  }

  result = 0;
  log("Decode end");

end:
  av_frame_free(&audioFrame);
  av_packet_free(&audioPacket);

  return result;
}


int main(int argc, char** argv) {
  int result = 0;

  string filePath = string(argv[1]);

  string outFilePath = "output.wav";
  outFile.open(outFilePath.c_str(), ios_base::out | ios_base::binary);
  if (!outFile.is_open()) {
    log("ERROR: Could not create output file");
    return -1;
  }

  result = loadCodec(filePath);
  if (result < 0) return result;

  decodeFrames();
  if (result == 0) {
    log("Output written to: %s", outFilePath.c_str());
  }

  return result;
}

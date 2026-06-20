#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "filter_fx.h"

#include <stdio.h>

bool isPlaying = true;

AudioFilter* audioFilter0 = NULL;
AudioFilter* audioFilter1 = NULL;

void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
  if (!isPlaying) return;
  ma_float* stream = (ma_float*) pOutput;
  
  ma_decoder* pDecoder = (ma_decoder*) pDevice->pUserData;
  if (pDecoder == NULL) {
    return;
  }
  
  int dataSize = frameCount * pDevice->playback.channels;
  ma_float buf[dataSize];

  ma_uint64 framesRead;
  ma_decoder_read_pcm_frames(pDecoder, &buf, frameCount, &framesRead);
  
  for (int i = 0; i < frameCount; i++) {
    for (int ch = 0; ch < pDevice->playback.channels; ch++) {
      int id = i * pDevice->playback.channels + ch;
      ma_float _sample = buf[id];
      
      // Filter all channels separately
      AudioFilter* af = (ch == 0) ? audioFilter0 : audioFilter1;
      ma_float sample = af->processAudioSample(_sample);
      
      *stream++ = sample;
    }
  }
  
  if (framesRead < frameCount) {
    isPlaying = false;
    return;
  }
}

int main(int argc, char** argv) {
  ma_result result;
  ma_decoder decoder;
  ma_device_config deviceConfig;
  ma_device device;

  if (argc < 2) {
    printf("No input file.\n");
    return -1;
  }

  result = ma_decoder_init_file(argv[1], NULL, &decoder);
  if (result != MA_SUCCESS) {
    printf("Could not load file: %s\n", argv[1]);
    return -2;
  }
  
  audioFilter0 = new LowPassFilter();
  audioFilter0->setSampleRate(44100);
  audioFilter0->setFrequency(200);
  
  audioFilter1 = new LowPassFilter();
  audioFilter1->setSampleRate(44100);
  audioFilter1->setFrequency(200);
  
  deviceConfig = ma_device_config_init(ma_device_type_playback);
  deviceConfig.playback.format   = decoder.outputFormat;
  deviceConfig.playback.channels = decoder.outputChannels;
  deviceConfig.sampleRate        = decoder.outputSampleRate;
  deviceConfig.dataCallback      = data_callback;
  deviceConfig.pUserData         = &decoder;
  
  printf("deviceConfig.playback.format: %d\n", deviceConfig.playback.format);
  printf("deviceConfig.playback.channels: %d\n", deviceConfig.playback.channels);
  printf("deviceConfig.sampleRate: %d\n", deviceConfig.sampleRate);

  if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
    printf("Failed to open playback device.\n");
    ma_decoder_uninit(&decoder);
    return -3;
  }

  if (ma_device_start(&device) != MA_SUCCESS) {
    printf("Failed to start playback device.\n");
    ma_device_uninit(&device);
    ma_decoder_uninit(&decoder);
    return -4;
  }

  printf("Press Enter to quit...\n");
  getchar();

  ma_device_uninit(&device);
  ma_decoder_uninit(&decoder);

  return 0;
}

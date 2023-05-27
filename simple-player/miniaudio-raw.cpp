#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <fstream>
#include <string>

using namespace std;


int totalSamples = 0;
int nextSampleId = 0;


void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
  /* frameCount - max frames to read */
  
  ma_int16* stream = (ma_int16*) pOutput;
  ma_int16* data = (ma_int16*) pDevice->pUserData;
  
  for (int i = 0; i < frameCount; i++) {
    for (int ch = 0; ch < pDevice->playback.channels; ch++) {
      ma_int16 sample = data[nextSampleId++];
      if (nextSampleId >= totalSamples) return;

      *stream++ = sample;
    }
  }
}

int main() {
  // MONO
  string path = "sine_440_hz_raw.wav";
  
  // STEREO
  // string path = "Italian Serenade_raw.wav";
  
  // Raw 16-bit PCM data
  int sample_rate = 44100;
  int channels = 1;
  
  
  // Read all data
  ifstream file(path, ios::binary | ios::ate);
  
  int size = file.tellg();
  printf("File size: %d\n", size);
  file.seekg(0);
  
  totalSamples = size / sizeof(uint16_t);
  
  char* buf = new char[size];
  file.read((char*) buf, size);
  
  file.close();
  
  
  // miniaudio
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format   = ma_format_s16;   // Set to ma_format_unknown to use the device's native format.
  config.playback.channels = channels;               // Set to 0 to use the device's native channel count.
  config.sampleRate        = sample_rate;           // Set to 0 to use the device's native sample rate.
  config.dataCallback      = data_callback;   // This function will be called when miniaudio needs more data.
  config.pUserData         = buf;             // Can be accessed from the device object (device.pUserData).

  ma_device device;
  if (ma_device_init(NULL, &config, &device) != MA_SUCCESS) {
    printf("Failed to initialize the device\n");
    return -1;
  }

  ma_device_start(&device);     // The device is sleeping by default so you'll need to start it manually.

  printf("Press Enter to quit...");
  getchar();

  ma_device_uninit(&device);    // This will stop the device so no need to do that manually.
  delete[] buf;
  
  return 0;
}

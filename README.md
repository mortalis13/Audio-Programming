## audio-filter-effects
Based on the book "Designing Audio Effect Plugins in C++".

Build the tester binary, which uses a simple `miniaudio` player and applies low pass filter at 200 Hz to a sample audio.

## ffmpeg-tests
A set of tools and experiments based on `ffmpeg` libraries

- fm-tests - any tests with `ffmpeg` API
- decode-audio - extracts raw audio data (frames) from most audio formats
  - supports video formats, detecting an audio stream
  - the output are raw frames in the standard chain "input - packets - frames", that can be fed to filters for additional audio processing
  - to play the output raw data, detect the correct configuration (format, channels, sample rate) from the terminal output and form parameters for `ffplay`:
    - for an average mp3 file - `ffplay -ch_layout stereo -ar 44100 -f f32le output.wav`
    - for an amr audio - `ffplay -ch_layout mono -ar 8000 -f f32le output.wav`
- waveform - shows a waveform for an mp3 audio file
  - `ffmpeg` used for raw data decoding
  - `SDL2` for visualization

## ffplay-audio
Simplified version of `ffplay` from the `ffmpeg` tools, for audio playback.

## simple-player
Various options for a `miniaudio` based player:
- a basic sample from the docs
- a raw PCM data player (use with 16 bit PCM mono WAV input)
- an `ffmpeg` based decoder with direct audio data processing callback (for mp3 and other encoded audio formats)

---

## Building (Windows)
Use `b.bat` and `r.bat` in each module to build and test the applications.  
Set path to the `MinGW` build toolchain before building.  
The `shared/` folder is used for building and binary execution to load the libraries, reference include headers and sample files.

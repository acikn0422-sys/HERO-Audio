#include "hero_audio/fft_backend.hpp"
#include "hero_audio/wav_reader.hpp"

#include <exception>
#include <iomanip>
#include <iostream>

int main(int argc, char **argv) {
  if (argc > 2) {
    std::cerr << "Usage: hero-audio [input.wav]\n";
    return 2;
  }

  std::cout << "HERO-Audio v0.1 scaffold\nAvailable FFT backends:\n";
  for (const auto backend : hero_audio::available_fft_backends()) {
    std::cout << "  - " << hero_audio::backend_name(backend) << '\n';
  }

  if (argc == 2) {
    try {
      const auto audio = hero_audio::read_wav(argv[1]);
      std::cout << "WAV input:\n"
                << "  sample_rate_hz: " << audio.sample_rate_hz << '\n'
                << "  source_channels: " << audio.source_channels << '\n'
                << "  mono_samples: " << audio.mono_samples.size() << '\n'
                << "  duration_seconds: " << std::fixed << std::setprecision(6)
                << audio.duration_seconds() << '\n';
    } catch (const std::exception &error) {
      std::cerr << "Error: " << error.what() << '\n';
      return 1;
    }
  }
  return 0;
}

#include "hero_audio/fft_backend.hpp"

#include <iostream>

int main() {
  std::cout << "HERO-Audio v0.1 scaffold\nAvailable FFT backends:\n";
  for (const auto backend : hero_audio::available_fft_backends()) {
    std::cout << "  - " << hero_audio::backend_name(backend) << '\n';
  }
  return 0;
}

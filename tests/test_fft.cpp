#include "hero_audio/fft_backend.hpp"

#include <cmath>
#include <complex>
#include <iostream>
#include <numbers>
#include <vector>

namespace {

bool approximately_equal(std::complex<float> actual, std::complex<float> expected,
                         float tolerance = 1.0e-3F) {
  return std::abs(actual - expected) <= tolerance;
}

bool test_impulse(hero_audio::FFTBackendKind kind) {
  constexpr std::size_t size = 1024;
  auto backend = hero_audio::make_fft_backend(kind, size);
  std::vector<float> input(size, 0.0F);
  std::vector<std::complex<float>> output(size / 2 + 1);
  input[0] = 1.0F;
  backend->execute(input, output);
  for (const auto bin : output) {
    if (!approximately_equal(bin, {1.0F, 0.0F})) {
      return false;
    }
  }
  return true;
}

bool test_sine_peak(hero_audio::FFTBackendKind kind) {
  constexpr std::size_t size = 1024;
  constexpr std::size_t expected_bin = 37;
  auto backend = hero_audio::make_fft_backend(kind, size);
  std::vector<float> input(size);
  std::vector<std::complex<float>> output(size / 2 + 1);
  for (std::size_t index = 0; index < size; ++index) {
    input[index] = std::sin(2.0F * std::numbers::pi_v<float> *
                            static_cast<float>(expected_bin * index) /
                            static_cast<float>(size));
  }
  backend->execute(input, output);
  std::size_t largest_bin = 0;
  for (std::size_t bin = 1; bin < output.size(); ++bin) {
    if (std::abs(output[bin]) > std::abs(output[largest_bin])) {
      largest_bin = bin;
    }
  }
  return largest_bin == expected_bin;
}

} // namespace

int main() {
  for (const auto kind : hero_audio::available_fft_backends()) {
    if (!test_impulse(kind) || !test_sine_peak(kind)) {
      std::cerr << "FFT test failed for " << hero_audio::backend_name(kind) << '\n';
      return 1;
    }
    std::cout << "FFT tests passed for " << hero_audio::backend_name(kind) << '\n';
  }
  return 0;
}

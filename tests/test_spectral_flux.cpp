#include "hero_audio/fft_backend.hpp"
#include "hero_audio/spectral_flux.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool near(double actual, double expected, double tolerance = 1.0e-6) {
  return std::abs(actual - expected) <= tolerance;
}

bool test_hann_window() {
  const auto window = hero_audio::make_hann_window(1024);
  if (window.size() != 1024 || !near(window.front(), 0.0) || !near(window.back(), 0.0)) {
    return false;
  }
  for (std::size_t index = 0; index < window.size(); ++index) {
    if (!near(window[index], window[window.size() - 1 - index])) {
      return false;
    }
  }
  return true;
}

bool test_frame_count_and_timestamps(hero_audio::FFTBackendKind kind) {
  constexpr std::uint32_t sample_rate = 48000;
  auto fft = hero_audio::make_fft_backend(kind, 1024);
  const std::vector<float> silence(1536, 0.0F);
  const auto frames = hero_audio::compute_spectral_flux(silence, sample_rate, *fft);
  return frames.size() == 3 && frames[0].frame_index == 0 &&
         frames[0].frame_start_sample == 0 && near(frames[0].frame_start_seconds, 0.0) &&
         near(frames[0].frame_center_seconds, 512.0 / sample_rate) &&
         near(frames[0].available_seconds, 1024.0 / sample_rate) &&
         frames[2].frame_start_sample == 512 &&
         std::all_of(frames.begin(), frames.end(),
                     [](const auto &frame) { return near(frame.spectral_flux, 0.0); });
}

bool test_no_implicit_tail_padding(hero_audio::FFTBackendKind kind) {
  auto fft = hero_audio::make_fft_backend(kind, 1024);
  return hero_audio::compute_spectral_flux(std::vector<float>(1023), 48000, *fft).empty() &&
         hero_audio::compute_spectral_flux(std::vector<float>(1024), 48000, *fft).size() == 1 &&
         hero_audio::compute_spectral_flux(std::vector<float>(1279), 48000, *fft).size() == 1 &&
         hero_audio::compute_spectral_flux(std::vector<float>(1280), 48000, *fft).size() == 2;
}

bool test_impulse_produces_positive_flux(hero_audio::FFTBackendKind kind) {
  auto fft = hero_audio::make_fft_backend(kind, 1024);
  std::vector<float> samples(1536, 0.0F);
  samples[800] = 1.0F;
  const auto frames = hero_audio::compute_spectral_flux(samples, 48000, *fft);
  return frames.size() == 3 && near(frames[0].spectral_flux, 0.0) &&
         frames[1].spectral_flux > 0.0F;
}

bool test_backends_agree() {
  std::vector<float> samples(2048);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    samples[index] = static_cast<float>(
        0.6 * std::sin(2.0 * 3.14159265358979323846 * 37.0 *
                       static_cast<double>(index) / 1024.0));
  }
  samples[1200] += 1.0F;

  auto reference =
      hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, 1024);
  const auto expected = hero_audio::compute_spectral_flux(samples, 48000, *reference);
  for (const auto kind : hero_audio::available_fft_backends()) {
    auto backend = hero_audio::make_fft_backend(kind, 1024);
    const auto actual = hero_audio::compute_spectral_flux(samples, 48000, *backend);
    if (actual.size() != expected.size()) {
      return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
      const double scale = std::max(1.0, static_cast<double>(expected[index].spectral_flux));
      if (!near(actual[index].spectral_flux, expected[index].spectral_flux,
                2.0e-4 * scale)) {
        return false;
      }
    }
  }
  return true;
}

bool test_csv_output() {
  const std::vector<hero_audio::SpectralFluxFrame> frames{{
      .frame_index = 2,
      .frame_start_sample = 512,
      .frame_start_seconds = 0.0106666667,
      .frame_center_seconds = 0.0213333333,
      .available_seconds = 0.032,
      .spectral_flux = 12.5F,
  }};
  std::ostringstream output;
  hero_audio::write_spectral_flux_csv(output, frames);
  const std::string expected =
      "frame_index,frame_start_sample,frame_start_seconds,frame_center_seconds,"
      "available_seconds,spectral_flux\n"
      "2,512,0.010666667,0.021333333,0.032000000,12.500000000\n";
  return output.str() == expected;
}

bool test_rejects_invalid_input() {
  auto fft = hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, 1024);
  std::vector<float> samples(1024, 0.0F);
  samples[100] = std::numeric_limits<float>::quiet_NaN();
  try {
    static_cast<void>(hero_audio::compute_spectral_flux(samples, 48000, *fft));
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

} // namespace

int main() {
  if (!test_hann_window() || !test_backends_agree() || !test_csv_output() ||
      !test_rejects_invalid_input()) {
    std::cerr << "Spectral Flux shared test failed\n";
    return 1;
  }
  for (const auto kind : hero_audio::available_fft_backends()) {
    if (!test_frame_count_and_timestamps(kind) || !test_no_implicit_tail_padding(kind) ||
        !test_impulse_produces_positive_flux(kind)) {
      std::cerr << "Spectral Flux test failed for " << hero_audio::backend_name(kind) << '\n';
      return 1;
    }
  }
  std::cout << "Spectral Flux tests passed\n";
  return 0;
}

#include "hero_audio/spectral_flux.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace hero_audio {

std::vector<float> make_hann_window(std::size_t size) {
  if (size == 0) {
    throw std::invalid_argument("Hann window size must be non-zero");
  }
  if (size == 1) {
    return {1.0F};
  }

  std::vector<float> window(size);
  const auto denominator = static_cast<double>(size - 1);
  for (std::size_t index = 0; index < size; ++index) {
    const double phase = 2.0 * std::numbers::pi_v<double> *
                         static_cast<double>(index) / denominator;
    window[index] = static_cast<float>(0.5 * (1.0 - std::cos(phase)));
  }
  return window;
}

std::vector<SpectralFluxFrame>
compute_spectral_flux(std::span<const float> mono_samples, std::uint32_t sample_rate_hz,
                      FFTBackend &fft, SpectralFluxConfig config) {
  if (sample_rate_hz == 0) {
    throw std::invalid_argument("Sample rate must be non-zero");
  }
  if (config.frame_size_samples == 0 || config.hop_size_samples == 0) {
    throw std::invalid_argument("Frame size and hop size must be non-zero");
  }
  if (config.frame_size_samples != fft.fft_size()) {
    throw std::invalid_argument("Spectral Flux frame size must match FFT backend size");
  }
  if (mono_samples.size() < config.frame_size_samples) {
    return {};
  }
  if (std::any_of(mono_samples.begin(), mono_samples.end(),
                  [](float sample) { return !std::isfinite(sample); })) {
    throw std::invalid_argument("Spectral Flux input contains a non-finite sample");
  }

  const auto frame_count =
      1 + (mono_samples.size() - config.frame_size_samples) / config.hop_size_samples;
  const auto bin_count = config.frame_size_samples / 2 + 1;
  const auto window = make_hann_window(config.frame_size_samples);
  std::vector<float> windowed_frame(config.frame_size_samples);
  std::vector<std::complex<float>> spectrum(bin_count);
  std::vector<float> previous_magnitude(bin_count, 0.0F);
  std::vector<float> current_magnitude(bin_count, 0.0F);
  std::vector<SpectralFluxFrame> result;
  result.reserve(frame_count);

  for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
    const auto start_sample = frame_index * config.hop_size_samples;
    for (std::size_t sample = 0; sample < config.frame_size_samples; ++sample) {
      windowed_frame[sample] = mono_samples[start_sample + sample] * window[sample];
    }

    fft.execute(windowed_frame, spectrum);
    for (std::size_t bin = 0; bin < bin_count; ++bin) {
      current_magnitude[bin] = std::abs(spectrum[bin]);
      if (!std::isfinite(current_magnitude[bin])) {
        throw std::overflow_error("FFT magnitude is not finite");
      }
    }

    double flux = 0.0;
    if (frame_index != 0) {
      for (std::size_t bin = 0; bin < bin_count; ++bin) {
        flux += std::max(0.0F, current_magnitude[bin] - previous_magnitude[bin]);
      }
    }
    if (flux > static_cast<double>(std::numeric_limits<float>::max())) {
      throw std::overflow_error("Spectral Flux exceeds float32 range");
    }

    const auto start = static_cast<double>(start_sample);
    const auto frame_size = static_cast<double>(config.frame_size_samples);
    const auto sample_rate = static_cast<double>(sample_rate_hz);
    result.push_back(SpectralFluxFrame{
        .frame_index = frame_index,
        .frame_start_sample = start_sample,
        .frame_start_seconds = start / sample_rate,
        .frame_center_seconds = (start + frame_size / 2.0) / sample_rate,
        .available_seconds = (start + frame_size) / sample_rate,
        .spectral_flux = static_cast<float>(flux),
    });
    previous_magnitude.swap(current_magnitude);
  }
  return result;
}

void write_spectral_flux_csv(std::ostream &output,
                             std::span<const SpectralFluxFrame> frames) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  output << "frame_index,frame_start_sample,frame_start_seconds,frame_center_seconds,"
            "available_seconds,spectral_flux\n";
  output << std::fixed << std::setprecision(9);
  for (const auto &frame : frames) {
    output << frame.frame_index << ',' << frame.frame_start_sample << ','
           << frame.frame_start_seconds << ',' << frame.frame_center_seconds << ','
           << frame.available_seconds << ',' << frame.spectral_flux << '\n';
  }
  output.flags(old_flags);
  output.precision(old_precision);
  if (!output) {
    throw std::runtime_error("Unable to write Spectral Flux CSV");
  }
}

void write_spectral_flux_csv(const std::filesystem::path &path,
                             std::span<const SpectralFluxFrame> frames) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open Spectral Flux CSV: " + path.string());
  }
  write_spectral_flux_csv(output, frames);
}

} // namespace hero_audio

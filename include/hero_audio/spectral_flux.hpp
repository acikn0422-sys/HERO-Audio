#pragma once

#include "hero_audio/fft_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <vector>

namespace hero_audio {

struct SpectralFluxConfig {
  std::size_t frame_size_samples{1024};
  std::size_t hop_size_samples{256};
};

struct SpectralFluxFrame {
  std::size_t frame_index{};
  std::size_t frame_start_sample{};
  double frame_start_seconds{};
  double frame_center_seconds{};
  double available_seconds{};
  float spectral_flux{};
};

[[nodiscard]] std::vector<float> make_hann_window(std::size_t size);

// Frames start at frame_index * hop_size. Only complete frames are processed;
// the tail is not implicitly zero-padded. The first frame has zero flux because
// there is no previous spectrum.
[[nodiscard]] std::vector<SpectralFluxFrame>
compute_spectral_flux(std::span<const float> mono_samples, std::uint32_t sample_rate_hz,
                      FFTBackend &fft, SpectralFluxConfig config = {});

void write_spectral_flux_csv(std::ostream &output,
                             std::span<const SpectralFluxFrame> frames);
void write_spectral_flux_csv(const std::filesystem::path &path,
                             std::span<const SpectralFluxFrame> frames);

} // namespace hero_audio

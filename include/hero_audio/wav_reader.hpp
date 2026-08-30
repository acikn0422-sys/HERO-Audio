#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hero_audio {

enum class WavSampleEncoding { IntegerPcm, IeeeFloat };

struct AudioBuffer {
  std::uint32_t sample_rate_hz{};
  std::uint16_t source_channels{};
  WavSampleEncoding source_encoding{WavSampleEncoding::IntegerPcm};
  std::uint16_t source_bits_per_sample{};
  std::vector<float> mono_samples;

  [[nodiscard]] double duration_seconds() const noexcept;
};

// Reads little-endian RIFF/WAVE containing integer PCM (8/16/24/32-bit) or
// IEEE float32 samples. All channels are averaged into mono float32.
// Throws std::runtime_error when the file is malformed or unsupported.
[[nodiscard]] AudioBuffer read_wav(const std::filesystem::path &path);

} // namespace hero_audio

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>

namespace hero_audio {

// Streaming mono PCM16 WAV writer used by live capture. Samples are appended
// by the non-real-time consumer thread; finalize() patches RIFF sizes after the
// duration is known. Files larger than the classic RIFF 4 GiB limit are rejected.
class Pcm16WavWriter {
public:
  Pcm16WavWriter(const std::filesystem::path &path, std::uint32_t sample_rate_hz);
  ~Pcm16WavWriter();

  Pcm16WavWriter(const Pcm16WavWriter &) = delete;
  Pcm16WavWriter &operator=(const Pcm16WavWriter &) = delete;

  void append(std::span<const float> mono_samples);
  void append_silence(std::size_t sample_count);
  void finalize();

  [[nodiscard]] std::uint64_t sample_count() const noexcept { return sample_count_; }
  [[nodiscard]] bool finalized() const noexcept { return finalized_; }

private:
  std::ofstream output_;
  std::uint32_t sample_rate_hz_{};
  std::uint64_t sample_count_{};
  bool finalized_{};
};

} // namespace hero_audio

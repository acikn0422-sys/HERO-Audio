#include "hero_audio/wav_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hero_audio {
namespace {

void write_u16(std::ostream &output, std::uint16_t value) {
  const std::array<char, 2> bytes{
      static_cast<char>(value & 0xFFU),
      static_cast<char>((value >> 8U) & 0xFFU),
  };
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ostream &output, std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value & 0xFFU),
      static_cast<char>((value >> 8U) & 0xFFU),
      static_cast<char>((value >> 16U) & 0xFFU),
      static_cast<char>((value >> 24U) & 0xFFU),
  };
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::int16_t float_to_pcm16(float sample) {
  if (!std::isfinite(sample)) {
    throw std::invalid_argument("WAV output contains a non-finite sample");
  }
  const float clamped = std::clamp(sample, -1.0F, 1.0F);
  if (clamped <= -1.0F) {
    return std::numeric_limits<std::int16_t>::min();
  }
  return static_cast<std::int16_t>(std::lround(clamped * 32767.0F));
}

} // namespace

Pcm16WavWriter::Pcm16WavWriter(const std::filesystem::path &path,
                               std::uint32_t sample_rate_hz)
    : output_(path, std::ios::binary | std::ios::trunc), sample_rate_hz_(sample_rate_hz) {
  if (sample_rate_hz_ == 0) {
    throw std::invalid_argument("WAV output sample rate must be non-zero");
  }
  if (sample_rate_hz_ > std::numeric_limits<std::uint32_t>::max() / 2U) {
    throw std::invalid_argument("WAV output sample rate is too large for PCM16 byte rate");
  }
  if (!output_) {
    throw std::runtime_error("Unable to open WAV output: " + path.string());
  }

  // Classic 44-byte PCM RIFF header. Chunk sizes are patched in finalize().
  output_.write("RIFF", 4);
  write_u32(output_, 0);
  output_.write("WAVE", 4);
  output_.write("fmt ", 4);
  write_u32(output_, 16);
  write_u16(output_, 1); // Integer PCM.
  write_u16(output_, 1); // Mono.
  write_u32(output_, sample_rate_hz_);
  write_u32(output_, sample_rate_hz_ * 2U);
  write_u16(output_, 2);
  write_u16(output_, 16);
  output_.write("data", 4);
  write_u32(output_, 0);
  if (!output_) {
    throw std::runtime_error("Unable to write WAV header");
  }
}

Pcm16WavWriter::~Pcm16WavWriter() {
  if (!finalized_) {
    try {
      finalize();
    } catch (...) {
      // Destructors cannot report I/O errors; live_main calls finalize()
      // explicitly so normal execution still receives any failure.
    }
  }
}

void Pcm16WavWriter::append(std::span<const float> mono_samples) {
  if (finalized_) {
    throw std::logic_error("Cannot append to a finalized WAV file");
  }
  if (std::any_of(mono_samples.begin(), mono_samples.end(),
                  [](float sample) { return !std::isfinite(sample); })) {
    throw std::invalid_argument("WAV output contains a non-finite sample");
  }
  constexpr std::uint64_t maximum_data_bytes =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - 36U;
  if (mono_samples.size() > (maximum_data_bytes / 2U) - sample_count_) {
    throw std::overflow_error("PCM16 WAV exceeds the classic RIFF size limit");
  }

  for (const float sample : mono_samples) {
    write_u16(output_, static_cast<std::uint16_t>(float_to_pcm16(sample)));
  }
  sample_count_ += mono_samples.size();
  if (!output_) {
    throw std::runtime_error("Unable to append PCM16 WAV samples");
  }
}

void Pcm16WavWriter::append_silence(std::size_t sample_count) {
  constexpr std::array<float, 256> silence{};
  while (sample_count != 0) {
    const auto count = std::min(sample_count, silence.size());
    append(std::span<const float>(silence).first(count));
    sample_count -= count;
  }
}

void Pcm16WavWriter::finalize() {
  if (finalized_) {
    return;
  }
  const auto data_bytes = static_cast<std::uint32_t>(sample_count_ * 2U);
  output_.seekp(4, std::ios::beg);
  write_u32(output_, 36U + data_bytes);
  output_.seekp(40, std::ios::beg);
  write_u32(output_, data_bytes);
  output_.flush();
  if (!output_) {
    throw std::runtime_error("Unable to finalize PCM16 WAV output");
  }
  finalized_ = true;
}

} // namespace hero_audio

#include "hero_audio/wav_reader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hero_audio {
namespace {

constexpr std::uint16_t kPcmFormat = 1;
constexpr std::uint16_t kIeeeFloatFormat = 3;
constexpr std::uint16_t kExtensibleFormat = 0xFFFE;
constexpr std::uint32_t kMaximumFormatChunkBytes = 1024;

struct WavFormat {
  std::uint16_t encoding{};
  std::uint16_t channels{};
  std::uint32_t sample_rate_hz{};
  std::uint16_t block_align{};
  std::uint16_t bits_per_sample{};
};

[[nodiscard]] std::uint16_t read_u16(std::span<const unsigned char> bytes,
                                     std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32(std::span<const unsigned char> bytes,
                                     std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void read_exact(std::istream &stream, std::span<unsigned char> destination,
                std::string_view description) {
  stream.read(reinterpret_cast<char *>(destination.data()),
              static_cast<std::streamsize>(destination.size()));
  if (stream.gcount() != static_cast<std::streamsize>(destination.size())) {
    throw std::runtime_error("Unexpected end of WAV while reading " + std::string(description));
  }
}

[[nodiscard]] bool is_fourcc(std::span<const unsigned char> bytes, const char (&text)[5]) {
  return std::equal(bytes.begin(), bytes.end(), text);
}

[[nodiscard]] WavFormat parse_format(std::span<const unsigned char> payload) {
  if (payload.size() < 16) {
    throw std::runtime_error("WAV fmt chunk is shorter than 16 bytes");
  }

  WavFormat format{.encoding = read_u16(payload, 0),
                   .channels = read_u16(payload, 2),
                   .sample_rate_hz = read_u32(payload, 4),
                   .block_align = read_u16(payload, 12),
                   .bits_per_sample = read_u16(payload, 14)};

  if (format.encoding == kExtensibleFormat) {
    if (payload.size() < 40 || read_u16(payload, 16) < 22) {
      throw std::runtime_error("Invalid WAVE_FORMAT_EXTENSIBLE fmt chunk");
    }
    constexpr std::array<unsigned char, 12> standard_guid_suffix{
        0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    const auto subformat = read_u32(payload, 24);
    const bool has_standard_guid =
        std::equal(standard_guid_suffix.begin(), standard_guid_suffix.end(),
                   payload.begin() + 28);
    if (subformat > std::numeric_limits<std::uint16_t>::max() || !has_standard_guid) {
      throw std::runtime_error("Unsupported WAVE_FORMAT_EXTENSIBLE subformat GUID");
    }
    format.encoding = static_cast<std::uint16_t>(subformat);
  }

  if (format.channels == 0 || format.channels > 64) {
    throw std::runtime_error("WAV channel count must be between 1 and 64");
  }
  if (format.sample_rate_hz == 0) {
    throw std::runtime_error("WAV sample rate must be non-zero");
  }

  const bool integer_pcm = format.encoding == kPcmFormat &&
                           (format.bits_per_sample == 8 || format.bits_per_sample == 16 ||
                            format.bits_per_sample == 24 || format.bits_per_sample == 32);
  const bool float_pcm =
      format.encoding == kIeeeFloatFormat && format.bits_per_sample == 32;
  if (!integer_pcm && !float_pcm) {
    throw std::runtime_error("Unsupported WAV encoding or bit depth");
  }

  const auto bytes_per_sample = static_cast<std::uint16_t>(format.bits_per_sample / 8);
  const auto expected_alignment =
      static_cast<std::uint32_t>(format.channels) * bytes_per_sample;
  if (format.block_align != expected_alignment) {
    throw std::runtime_error("WAV block alignment does not match channels and bit depth");
  }
  return format;
}

[[nodiscard]] float decode_sample(std::span<const unsigned char> bytes,
                                  const WavFormat &format) {
  if (format.encoding == kIeeeFloatFormat) {
    const float value = std::bit_cast<float>(read_u32(bytes, 0));
    if (!std::isfinite(value)) {
      throw std::runtime_error("WAV contains a non-finite float sample");
    }
    return value;
  }

  switch (format.bits_per_sample) {
  case 8:
    return (static_cast<float>(bytes[0]) - 128.0F) / 128.0F;
  case 16: {
    const auto value = std::bit_cast<std::int16_t>(read_u16(bytes, 0));
    return static_cast<float>(value) / 32768.0F;
  }
  case 24: {
    std::uint32_t raw = static_cast<std::uint32_t>(bytes[0]) |
                        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                        (static_cast<std::uint32_t>(bytes[2]) << 16U);
    if ((raw & 0x00800000U) != 0U) {
      raw |= 0xFF000000U;
    }
    return static_cast<float>(std::bit_cast<std::int32_t>(raw)) / 8388608.0F;
  }
  case 32:
    return static_cast<float>(std::bit_cast<std::int32_t>(read_u32(bytes, 0))) /
           2147483648.0F;
  default:
    throw std::runtime_error("Internal error: unsupported PCM bit depth");
  }
}

} // namespace

double AudioBuffer::duration_seconds() const noexcept {
  if (sample_rate_hz == 0) {
    return 0.0;
  }
  return static_cast<double>(mono_samples.size()) / static_cast<double>(sample_rate_hz);
}

AudioBuffer read_wav(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Unable to open WAV file: " + path.string());
  }

  stream.seekg(0, std::ios::end);
  const auto file_size = stream.tellg();
  if (file_size < std::streamoff{12}) {
    throw std::runtime_error("File is too short to be a RIFF/WAVE file");
  }
  stream.seekg(0, std::ios::beg);

  std::array<unsigned char, 12> riff_header{};
  read_exact(stream, riff_header, "RIFF header");
  if (!is_fourcc(std::span<const unsigned char>{riff_header}.first<4>(), "RIFF") ||
      !is_fourcc(std::span<const unsigned char>{riff_header}.subspan<8, 4>(), "WAVE")) {
    throw std::runtime_error("File is not a little-endian RIFF/WAVE file");
  }

  WavFormat format{};
  bool has_format = false;
  std::streamoff data_offset = -1;
  std::uint32_t data_size = 0;

  while (stream.tellg() >= 0 && stream.tellg() + std::streamoff{8} <= file_size) {
    std::array<unsigned char, 8> chunk_header{};
    read_exact(stream, chunk_header, "chunk header");
    const auto chunk_size = read_u32(chunk_header, 4);
    const auto payload_offset = stream.tellg();
    const auto padded_size = static_cast<std::uint64_t>(chunk_size) + (chunk_size & 1U);
    if (padded_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        payload_offset + static_cast<std::streamoff>(padded_size) > file_size) {
      throw std::runtime_error("WAV chunk extends beyond the end of the file");
    }

    const auto chunk_id = std::span<const unsigned char>{chunk_header}.first<4>();
    if (is_fourcc(chunk_id, "fmt ")) {
      if (chunk_size > kMaximumFormatChunkBytes) {
        throw std::runtime_error("WAV fmt chunk is unreasonably large");
      }
      std::vector<unsigned char> payload(chunk_size);
      read_exact(stream, payload, "fmt chunk");
      format = parse_format(payload);
      has_format = true;
    } else if (is_fourcc(chunk_id, "data") && data_offset < 0) {
      data_offset = payload_offset;
      data_size = chunk_size;
    }

    stream.seekg(payload_offset + static_cast<std::streamoff>(padded_size));
    if (!stream) {
      throw std::runtime_error("Unable to seek to the next WAV chunk");
    }
  }

  if (!has_format) {
    throw std::runtime_error("WAV file has no fmt chunk");
  }
  if (data_offset < 0) {
    throw std::runtime_error("WAV file has no data chunk");
  }
  if (data_size % format.block_align != 0) {
    throw std::runtime_error("WAV data size is not a whole number of sample frames");
  }

  std::vector<unsigned char> encoded(data_size);
  stream.seekg(data_offset);
  read_exact(stream, encoded, "audio data");

  const auto frame_count = static_cast<std::size_t>(data_size / format.block_align);
  const auto bytes_per_sample = static_cast<std::size_t>(format.bits_per_sample / 8);
  AudioBuffer result{.sample_rate_hz = format.sample_rate_hz,
                     .source_channels = format.channels,
                     .source_encoding = format.encoding == kIeeeFloatFormat
                                            ? WavSampleEncoding::IeeeFloat
                                            : WavSampleEncoding::IntegerPcm,
                     .source_bits_per_sample = format.bits_per_sample,
                     .mono_samples = std::vector<float>(frame_count)};

  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    float sum = 0.0F;
    const auto frame_offset = frame * format.block_align;
    for (std::size_t channel = 0; channel < format.channels; ++channel) {
      const auto sample_offset = frame_offset + channel * bytes_per_sample;
      sum += decode_sample(std::span<const unsigned char>{encoded}.subspan(
                               sample_offset, bytes_per_sample),
                           format);
    }
    result.mono_samples[frame] = sum / static_cast<float>(format.channels);
  }
  return result;
}

} // namespace hero_audio

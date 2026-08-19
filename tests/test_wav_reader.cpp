#include "hero_audio/wav_reader.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void append_u16(std::vector<unsigned char> &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
  bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<unsigned char> &bytes, std::uint32_t value) {
  bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
  bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
  bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
  bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
}

void append_text(std::vector<unsigned char> &bytes, const char *text) {
  for (int index = 0; index < 4; ++index) {
    bytes.push_back(static_cast<unsigned char>(text[index]));
  }
}

std::vector<unsigned char> make_wav(std::uint16_t encoding, std::uint16_t channels,
                                    std::uint32_t sample_rate, std::uint16_t bits,
                                    const std::vector<unsigned char> &audio,
                                    bool include_odd_junk = false, bool extensible = false) {
  std::vector<unsigned char> body;
  append_text(body, "WAVE");
  if (include_odd_junk) {
    append_text(body, "JUNK");
    append_u32(body, 3);
    body.insert(body.end(), {1, 2, 3, 0});
  }
  append_text(body, "fmt ");
  append_u32(body, extensible ? 40U : 16U);
  append_u16(body, extensible ? 0xFFFEU : encoding);
  append_u16(body, channels);
  append_u32(body, sample_rate);
  const auto bytes_per_sample = static_cast<std::uint16_t>(bits / 8);
  const auto block_align = static_cast<std::uint16_t>(channels * bytes_per_sample);
  append_u32(body, sample_rate * block_align);
  append_u16(body, block_align);
  append_u16(body, bits);
  if (extensible) {
    append_u16(body, 22);
    append_u16(body, bits);
    append_u32(body, 0);
    append_u32(body, encoding);
    append_u16(body, 0);
    append_u16(body, 0x0010U);
    body.insert(body.end(), {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71});
  }
  append_text(body, "data");
  append_u32(body, static_cast<std::uint32_t>(audio.size()));
  body.insert(body.end(), audio.begin(), audio.end());
  if ((audio.size() & 1U) != 0U) {
    body.push_back(0);
  }

  std::vector<unsigned char> wav;
  append_text(wav, "RIFF");
  append_u32(wav, static_cast<std::uint32_t>(body.size()));
  wav.insert(wav.end(), body.begin(), body.end());
  return wav;
}

class TemporaryFile {
public:
  TemporaryFile(std::string name, const std::vector<unsigned char> &bytes)
      : path_(std::filesystem::temp_directory_path() / std::move(name)) {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
      throw std::runtime_error("Unable to create temporary WAV test file");
    }
  }

  ~TemporaryFile() {
    std::error_code ignored_error;
    std::filesystem::remove(path_, ignored_error);
  }
  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

bool near(float actual, float expected, float tolerance = 1.0e-6F) {
  return std::abs(actual - expected) <= tolerance;
}

bool test_pcm16_mono() {
  std::vector<unsigned char> audio;
  append_u16(audio, 0x8000U);
  append_u16(audio, 0x0000U);
  append_u16(audio, 0x7FFFU);
  TemporaryFile file("hero_audio_pcm16_mono.wav", make_wav(1, 1, 48000, 16, audio));
  const auto decoded = hero_audio::read_wav(file.path());
  return decoded.sample_rate_hz == 48000 && decoded.source_channels == 1 &&
         decoded.mono_samples.size() == 3 && near(decoded.mono_samples[0], -1.0F) &&
         near(decoded.mono_samples[1], 0.0F) &&
         near(decoded.mono_samples[2], 32767.0F / 32768.0F) &&
         near(static_cast<float>(decoded.duration_seconds()), 3.0F / 48000.0F);
}

bool test_stereo_downmix_and_unknown_chunk() {
  std::vector<unsigned char> audio;
  append_u16(audio, 0x4000U);
  append_u16(audio, 0x4000U);
  append_u16(audio, 0x7FFFU);
  append_u16(audio, 0x8000U);
  TemporaryFile file("hero_audio_stereo_junk.wav", make_wav(1, 2, 44100, 16, audio, true));
  const auto decoded = hero_audio::read_wav(file.path());
  return decoded.sample_rate_hz == 44100 && decoded.source_channels == 2 &&
         decoded.mono_samples.size() == 2 && near(decoded.mono_samples[0], 0.5F) &&
         near(decoded.mono_samples[1], -1.0F / 65536.0F);
}

bool test_supported_sample_formats() {
  TemporaryFile pcm8("hero_audio_pcm8.wav", make_wav(1, 1, 48000, 8, {255}));
  TemporaryFile pcm24("hero_audio_pcm24.wav",
                      make_wav(1, 1, 48000, 24, {0xFF, 0xFF, 0x7F}));
  TemporaryFile pcm32("hero_audio_pcm32.wav",
                      make_wav(1, 1, 48000, 32, {0xFF, 0xFF, 0xFF, 0x7F}));
  std::vector<unsigned char> float_bytes;
  append_u32(float_bytes, std::bit_cast<std::uint32_t>(0.25F));
  TemporaryFile float32("hero_audio_float32.wav", make_wav(3, 1, 48000, 32, float_bytes));

  return near(hero_audio::read_wav(pcm8.path()).mono_samples[0], 127.0F / 128.0F) &&
         near(hero_audio::read_wav(pcm24.path()).mono_samples[0], 8388607.0F / 8388608.0F) &&
         near(hero_audio::read_wav(pcm32.path()).mono_samples[0],
              2147483647.0F / 2147483648.0F) &&
         near(hero_audio::read_wav(float32.path()).mono_samples[0], 0.25F);
}

bool test_extensible_pcm() {
  std::vector<unsigned char> audio;
  append_u16(audio, 0x2000U);
  TemporaryFile file("hero_audio_extensible_pcm.wav",
                     make_wav(1, 1, 48000, 16, audio, false, true));
  const auto decoded = hero_audio::read_wav(file.path());
  return decoded.source_channels == 1 && decoded.sample_rate_hz == 48000 &&
         decoded.mono_samples.size() == 1 && near(decoded.mono_samples[0], 0.25F);
}

bool test_rejects_invalid_file() {
  TemporaryFile file("hero_audio_invalid.wav", {'N', 'O', 'T', 'W', 'A', 'V'});
  try {
    static_cast<void>(hero_audio::read_wav(file.path()));
  } catch (const std::runtime_error &) {
    return true;
  }
  return false;
}

} // namespace

int main() {
  if (!test_pcm16_mono() || !test_stereo_downmix_and_unknown_chunk() ||
      !test_supported_sample_formats() || !test_extensible_pcm() ||
      !test_rejects_invalid_file()) {
    std::cerr << "WAV reader test failed\n";
    return 1;
  }
  std::cout << "WAV reader tests passed\n";
  return 0;
}

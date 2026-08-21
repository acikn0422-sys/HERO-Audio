#include "hero_audio/fft_backend.hpp"
#include "hero_audio/offline_benchmark.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

void append_fourcc(std::vector<unsigned char> &bytes, const char *text) {
  for (int index = 0; index < 4; ++index) {
    bytes.push_back(static_cast<unsigned char>(text[index]));
  }
}

std::vector<unsigned char> make_pcm16_wav() {
  constexpr std::uint32_t sample_rate = 48000;
  std::vector<unsigned char> audio;
  audio.reserve(4096 * 2);
  for (std::size_t sample = 0; sample < 4096; ++sample) {
    const std::uint16_t value = sample == 1536 || sample == 3072 ? 0x7000U : 0U;
    append_u16(audio, value);
  }

  std::vector<unsigned char> body;
  append_fourcc(body, "WAVE");
  append_fourcc(body, "fmt ");
  append_u32(body, 16);
  append_u16(body, 1); // Integer PCM.
  append_u16(body, 1); // Mono.
  append_u32(body, sample_rate);
  append_u32(body, sample_rate * 2);
  append_u16(body, 2);
  append_u16(body, 16);
  append_fourcc(body, "data");
  append_u32(body, static_cast<std::uint32_t>(audio.size()));
  body.insert(body.end(), audio.begin(), audio.end());

  std::vector<unsigned char> wav;
  append_fourcc(wav, "RIFF");
  append_u32(wav, static_cast<std::uint32_t>(body.size()));
  wav.insert(wav.end(), body.begin(), body.end());
  return wav;
}

class TemporaryWav {
public:
  TemporaryWav()
      : path_(std::filesystem::temp_directory_path() / "hero_audio_offline_benchmark.wav") {
    const auto bytes = make_pcm16_wav();
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      throw std::runtime_error("Unable to create benchmark test WAV");
    }
  }

  ~TemporaryWav() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

bool near(double actual, double expected, double tolerance = 1.0e-9) {
  return std::abs(actual - expected) <= tolerance;
}

bool test_linear_percentiles() {
  const std::vector<double> values{5.0, 1.0, 4.0, 2.0, 3.0};
  return near(hero_audio::linear_percentile(values, 0.0), 1.0) &&
         near(hero_audio::linear_percentile(values, 0.5), 3.0) &&
         near(hero_audio::linear_percentile(values, 0.95), 4.8) &&
         near(hero_audio::linear_percentile(values, 0.99), 4.96) &&
         near(hero_audio::linear_percentile(values, 1.0), 5.0);
}

bool test_benchmark_and_serialization() {
  TemporaryWav wav;
  auto fft = hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, 1024);
  const auto result = hero_audio::benchmark_offline_wav(
      wav.path(), *fft, 0.125,
      hero_audio::OfflineBenchmarkConfig{.warmup_runs = 0, .measured_runs = 5});
  if (result.runs.size() != 5 || result.backend_name != "reference-radix2" ||
      !near(result.backend_initialization_ms, 0.125) || result.summary.median_elapsed_ms <= 0.0 ||
      result.summary.p95_elapsed_ms < result.summary.median_elapsed_ms ||
      result.summary.p99_elapsed_ms < result.summary.p95_elapsed_ms ||
      result.summary.median_real_time_factor <= 0.0) {
    return false;
  }
  for (std::size_t index = 0; index < result.runs.size(); ++index) {
    if (result.runs[index].run_index != index + 1 || result.runs[index].onset_count != 2 ||
        result.runs[index].serialized_onset_bytes == 0) {
      return false;
    }
  }

  std::ostringstream csv;
  hero_audio::write_offline_benchmark_runs_csv(csv, result.runs);
  std::ostringstream json;
  hero_audio::write_offline_benchmark_summary_json(json, result);
  return csv.str().starts_with("run_index,elapsed_ms,real_time_factor") &&
         json.str().find("\"metric\": \"wav_read_to_onset_csv_serialized\"") != std::string::npos &&
         json.str().find("\"measured_runs\": 5") != std::string::npos;
}

bool test_rejects_invalid_configuration() {
  auto fft = hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, 1024);
  try {
    static_cast<void>(hero_audio::benchmark_offline_wav(
        "unused.wav", *fft, 0.0,
        hero_audio::OfflineBenchmarkConfig{.warmup_runs = 0, .measured_runs = 4}));
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, bool (*)()>> tests{
      {"linear percentiles", test_linear_percentiles},
      {"benchmark and serialization", test_benchmark_and_serialization},
      {"invalid configuration", test_rejects_invalid_configuration},
  };
  for (const auto &[name, test] : tests) {
    if (!test()) {
      std::cerr << "Offline benchmark test failed: " << name << '\n';
      return 1;
    }
  }
  std::cout << "Offline benchmark tests passed\n";
  return 0;
}

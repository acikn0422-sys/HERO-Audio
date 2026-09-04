#include "hero_audio/causal_onset.hpp"
#include "hero_audio/fft_backend.hpp"
#include "hero_audio/spectral_flux.hpp"
#include "hero_audio/streaming_benchmark.hpp"
#include "hero_audio/streaming_processor.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kFrameSize = 1024;
constexpr std::size_t kHopSize = 256;

std::vector<float> make_samples(std::size_t size = 4096) {
  std::vector<float> samples(size, 0.0F);
  // Two short, spectrally broad events are intentionally aligned to samples,
  // making this fixture deterministic without depending on external files.
  if (size > 1536) {
    samples[1536] = 0.875F;
  }
  if (size > 3072) {
    samples[3072] = -0.75F;
  }
  return samples;
}

bool near(double actual, double expected, double tolerance = 1.0e-7) {
  return std::abs(actual - expected) <= tolerance;
}

bool same_frame(const hero_audio::SpectralFluxFrame &actual,
                const hero_audio::SpectralFluxFrame &expected) {
  return actual.frame_index == expected.frame_index &&
         actual.frame_start_sample == expected.frame_start_sample &&
         near(actual.frame_start_seconds, expected.frame_start_seconds, 1.0e-12) &&
         near(actual.frame_center_seconds, expected.frame_center_seconds, 1.0e-12) &&
         near(actual.available_seconds, expected.available_seconds, 1.0e-12) &&
         near(actual.spectral_flux, expected.spectral_flux, 1.0e-5);
}

bool same_onset(const hero_audio::OnsetEvent &actual,
                const hero_audio::OnsetEvent &expected) {
  return actual.frame_index == expected.frame_index &&
         near(actual.onset_time_seconds, expected.onset_time_seconds, 1.0e-12) &&
         near(actual.emitted_at_seconds, expected.emitted_at_seconds, 1.0e-12) &&
         near(actual.algorithm_delay_ms, expected.algorithm_delay_ms) &&
         near(actual.spectral_flux, expected.spectral_flux, 1.0e-5) &&
         near(actual.threshold, expected.threshold);
}

struct StreamingOutput {
  std::vector<hero_audio::SpectralFluxFrame> frames;
  std::vector<hero_audio::OnsetEvent> onsets;
};

StreamingOutput run_streaming(std::span<const float> samples,
                              hero_audio::StreamingProcessor &processor) {
  StreamingOutput output;
  const auto complete_hops = samples.size() / kHopSize;
  for (std::size_t hop = 0; hop < complete_hops; ++hop) {
    const auto result = processor.push_hop(samples.subspan(hop * kHopSize, kHopSize));
    if (result.flux_frame.has_value()) {
      output.frames.push_back(*result.flux_frame);
    }
    if (result.onset.has_value()) {
      output.onsets.push_back(*result.onset);
    }
  }
  return output;
}

bool test_matches_offline() {
  const auto samples = make_samples();
  auto streaming_fft =
      hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, kFrameSize);
  auto offline_fft =
      hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, kFrameSize);
  hero_audio::StreamingProcessor processor(
      *streaming_fft, hero_audio::StreamingProcessorConfig{.sample_rate_hz = kSampleRate});

  // The first three calls only fill 768 samples; the fourth completes the
  // first 1024-sample window and must emit offline frame zero.
  for (std::size_t hop = 0; hop < 3; ++hop) {
    const auto result = processor.push_hop(
        std::span<const float>(samples).subspan(hop * kHopSize, kHopSize));
    if (result.flux_frame.has_value() || result.onset.has_value()) {
      return false;
    }
  }
  const auto first = processor.push_hop(
      std::span<const float>(samples).subspan(3 * kHopSize, kHopSize));
  if (!first.flux_frame.has_value() || first.flux_frame->frame_index != 0 ||
      first.input_hop_index != 3) {
    return false;
  }

  processor.reset();
  const auto streaming = run_streaming(samples, processor);
  const auto offline_frames =
      hero_audio::compute_spectral_flux(samples, kSampleRate, *offline_fft);
  const auto offline_onsets = hero_audio::detect_causal_onsets(offline_frames);
  if (streaming.frames.size() != offline_frames.size() ||
      streaming.onsets.size() != offline_onsets.size()) {
    return false;
  }
  for (std::size_t index = 0; index < offline_frames.size(); ++index) {
    if (!same_frame(streaming.frames[index], offline_frames[index])) {
      return false;
    }
  }
  for (std::size_t index = 0; index < offline_onsets.size(); ++index) {
    if (!same_onset(streaming.onsets[index], offline_onsets[index])) {
      return false;
    }
  }
  return !streaming.onsets.empty();
}

bool test_rejects_bad_hop_without_state_change_and_resets() {
  auto fft = hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, kFrameSize);
  hero_audio::StreamingProcessor processor(
      *fft, hero_audio::StreamingProcessorConfig{.sample_rate_hz = kSampleRate});
  const std::vector<float> short_hop(kHopSize - 1, 0.0F);
  try {
    static_cast<void>(processor.push_hop(short_hop));
    return false;
  } catch (const std::invalid_argument &) {
  }
  auto invalid_hop = std::vector<float>(kHopSize, 0.0F);
  invalid_hop[4] = std::numeric_limits<float>::quiet_NaN();
  try {
    static_cast<void>(processor.push_hop(invalid_hop));
    return false;
  } catch (const std::invalid_argument &) {
  }
  if (processor.input_hops_received() != 0 || processor.frames_produced() != 0) {
    return false;
  }

  const auto samples = make_samples();
  const auto first_run = run_streaming(samples, processor);
  processor.reset();
  const auto second_run = run_streaming(samples, processor);
  if (first_run.frames.size() != second_run.frames.size() ||
      first_run.onsets.size() != second_run.onsets.size()) {
    return false;
  }
  for (std::size_t index = 0; index < first_run.frames.size(); ++index) {
    if (!same_frame(first_run.frames[index], second_run.frames[index])) {
      return false;
    }
  }
  return true;
}

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

std::vector<unsigned char> make_pcm16_wav_with_tail() {
  const auto samples = make_samples(4096 + 17);
  std::vector<unsigned char> audio;
  audio.reserve(samples.size() * 2);
  for (const float sample : samples) {
    const auto scaled = static_cast<std::int16_t>(sample * 32767.0F);
    append_u16(audio, static_cast<std::uint16_t>(scaled));
  }

  std::vector<unsigned char> body;
  append_fourcc(body, "WAVE");
  append_fourcc(body, "fmt ");
  append_u32(body, 16);
  append_u16(body, 1);
  append_u16(body, 1);
  append_u32(body, kSampleRate);
  append_u32(body, kSampleRate * 2);
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

class LocalWav {
public:
  LocalWav() : path_(std::filesystem::current_path() / "hero_audio_streaming_test.wav") {
    const auto bytes = make_pcm16_wav_with_tail();
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      throw std::runtime_error("Unable to create streaming test WAV");
    }
  }

  ~LocalWav() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

bool test_benchmark_and_serialization() {
  LocalWav wav;
  auto fft = hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, kFrameSize);
  const auto result = hero_audio::benchmark_streaming_wav(
      wav.path(), *fft, 0.125,
      hero_audio::StreamingBenchmarkConfig{.warmup_passes = 0, .measured_passes = 2});
  if (result.measurements.size() != 26 || result.first_pass_flux_frames.size() != 13 ||
      result.ignored_tail_sample_count != 17 || !result.consistency.overall_matches ||
      result.summary.measured_hop_count != 26 ||
      !near(result.summary.hop_period_ms, 1000.0 * 256.0 / 48000.0) ||
      result.summary.p50_compute_ms < 0.0 ||
      result.summary.p95_compute_ms < result.summary.p50_compute_ms ||
      result.summary.p99_compute_ms < result.summary.p95_compute_ms) {
    return false;
  }
  for (const auto &measurement : result.measurements) {
    if (measurement.compute_ms < 0.0 || measurement.deadline_ms <= 0.0) {
      return false;
    }
  }

  std::ostringstream csv;
  hero_audio::write_streaming_hop_measurements_csv(csv, result.measurements);
  std::ostringstream json;
  hero_audio::write_streaming_benchmark_summary_json(json, result);
  return csv.str().starts_with("pass_index,input_hop_index,frame_index,compute_ms") &&
         json.str().find("\"metric\": \"steady_state_hop_compute\"") !=
             std::string::npos &&
         json.str().find("\"ignored_tail_sample_count\": 17") != std::string::npos &&
         json.str().find("\"overall_matches\": true") != std::string::npos;
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, bool (*)()>> tests{
      {"streaming matches offline", test_matches_offline},
      {"invalid input and reset", test_rejects_bad_hop_without_state_change_and_resets},
      {"benchmark and serialization", test_benchmark_and_serialization},
  };
  for (const auto &[name, test] : tests) {
    if (!test()) {
      std::cerr << "Streaming processor test failed: " << name << '\n';
      return 1;
    }
  }
  std::cout << "Streaming processor tests passed\n";
  return 0;
}

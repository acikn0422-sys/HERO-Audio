#pragma once

#include "hero_audio/causal_onset.hpp"
#include "hero_audio/fft_backend.hpp"
#include "hero_audio/spectral_flux.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace hero_audio {

// A pass replays one decoded WAV through the stateful StreamingProcessor.
// FFT plan construction and WAV decoding are outside every per-hop timer.
struct StreamingBenchmarkConfig {
  std::size_t warmup_passes{1};
  std::size_t measured_passes{5};
  SpectralFluxConfig spectral_flux;
  CausalOnsetConfig onset;
};

// One raw steady-state sample. Startup calls that only fill the first FFT
// window are intentionally excluded; every retained row produced one complete
// analysis frame and includes hop insertion, Hann, FFT, Spectral Flux and
// causal onset detection.
struct StreamingHopMeasurement {
  std::size_t pass_index{};      // One-based measured-pass index.
  std::size_t input_hop_index{}; // Zero-based position within that pass.
  std::size_t frame_index{};
  double compute_ms{};
  double deadline_ms{};
  bool deadline_met{};
  bool onset_emitted{};
};

struct StreamingHopSummary {
  std::size_t measured_hop_count{};
  std::size_t deadline_miss_count{};
  double hop_period_ms{};
  double p50_compute_ms{};
  double p95_compute_ms{};
  double p99_compute_ms{};
  double maximum_compute_ms{};
  bool p95_within_hop_period{};
};

// The timed streaming pass is replayed by the existing offline functions
// outside the timed region. These fields make semantic drift visible.
struct StreamingConsistency {
  std::size_t streaming_frame_count{};
  std::size_t offline_frame_count{};
  std::size_t streaming_onset_count{};
  std::size_t offline_onset_count{};
  double maximum_flux_absolute_error{};
  bool frame_count_equal{};
  bool flux_frames_equal{};
  bool onset_events_equal{};
  bool overall_matches{};
};

struct StreamingBenchmarkResult {
  std::filesystem::path input_path;
  std::string backend_name;
  double backend_initialization_ms{};
  std::uint32_t sample_rate_hz{};
  std::size_t input_sample_count{};
  std::size_t processed_sample_count{};
  std::size_t ignored_tail_sample_count{};
  StreamingBenchmarkConfig config;
  std::vector<StreamingHopMeasurement> measurements;
  std::vector<SpectralFluxFrame> first_pass_flux_frames;
  std::vector<OnsetEvent> first_pass_onsets;
  StreamingHopSummary summary;
  StreamingConsistency consistency;
};

// read_wav() is performed once before any warm-up or measured per-hop timer.
// A final partial hop is ignored rather than padded, matching the offline
// complete-frame policy. At least one complete FFT frame is required.
[[nodiscard]] StreamingBenchmarkResult
benchmark_streaming_wav(const std::filesystem::path &input_path, FFTBackend &fft,
                        double backend_initialization_ms,
                        StreamingBenchmarkConfig config = {});

void write_streaming_hop_measurements_csv(
    std::ostream &output, std::span<const StreamingHopMeasurement> measurements);
void write_streaming_hop_measurements_csv(
    const std::filesystem::path &path,
    std::span<const StreamingHopMeasurement> measurements);
void write_streaming_benchmark_summary_json(std::ostream &output,
                                            const StreamingBenchmarkResult &result);
void write_streaming_benchmark_summary_json(const std::filesystem::path &path,
                                            const StreamingBenchmarkResult &result);

} // namespace hero_audio

#pragma once

#include "hero_audio/causal_onset.hpp"
#include "hero_audio/fft_backend.hpp"
#include "hero_audio/spectral_flux.hpp"

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace hero_audio {

// The official offline benchmark reuses an already-created FFT backend. This
// keeps FFT plan creation out of steady-state runs while reporting it
// separately as backend_initialization_ms.
struct OfflineBenchmarkConfig {
  std::size_t warmup_runs{3};
  std::size_t measured_runs{5};
  SpectralFluxConfig spectral_flux;
  CausalOnsetConfig onset;
};

// One row of raw benchmark evidence. Retaining every measured run is essential:
// summary percentiles must always be reproducible from committed raw data.
struct OfflineBenchmarkRun {
  std::size_t run_index{}; // One-based measured-run index.
  double elapsed_ms{};
  double real_time_factor{};
  double audio_duration_seconds{};
  std::size_t spectral_flux_frame_count{};
  std::size_t onset_count{};
  std::size_t serialized_onset_bytes{};
};

struct OfflineBenchmarkSummary {
  double median_elapsed_ms{};
  double p95_elapsed_ms{};
  double p99_elapsed_ms{};
  double median_real_time_factor{};
  double minimum_real_time_factor{};
};

struct OfflineBenchmarkResult {
  std::filesystem::path input_path;
  std::string backend_name;
  double backend_initialization_ms{};
  OfflineBenchmarkConfig config;
  std::vector<OfflineBenchmarkRun> runs;
  OfflineBenchmarkSummary summary;
};

// Linear-interpolation percentile (the common Type-7 definition): after
// sorting, rank = probability * (N - 1), then interpolate adjacent samples.
[[nodiscard]] double linear_percentile(std::span<const double> values, double probability);

// Each run starts immediately before read_wav() and stops after all detected
// onsets have been serialized as CSV in memory. File-system writes of benchmark
// evidence are deliberately outside the timed interval.
[[nodiscard]] OfflineBenchmarkResult benchmark_offline_wav(const std::filesystem::path &input_path,
                                                           FFTBackend &fft,
                                                           double backend_initialization_ms,
                                                           OfflineBenchmarkConfig config = {});

void write_offline_benchmark_runs_csv(std::ostream &output,
                                      std::span<const OfflineBenchmarkRun> runs);
void write_offline_benchmark_runs_csv(const std::filesystem::path &path,
                                      std::span<const OfflineBenchmarkRun> runs);
void write_offline_benchmark_summary_json(std::ostream &output,
                                          const OfflineBenchmarkResult &result);
void write_offline_benchmark_summary_json(const std::filesystem::path &path,
                                          const OfflineBenchmarkResult &result);

} // namespace hero_audio

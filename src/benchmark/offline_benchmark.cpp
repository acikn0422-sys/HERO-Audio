#include "hero_audio/offline_benchmark.hpp"

#include "hero_audio/wav_reader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hero_audio {
namespace {

using BenchmarkClock = std::chrono::steady_clock;

struct PipelineMeasurement {
  double elapsed_ms{};
  double real_time_factor{};
  double audio_duration_seconds{};
  std::size_t spectral_flux_frame_count{};
  std::size_t onset_count{};
  std::size_t serialized_onset_bytes{};
};

void validate_benchmark_config(const OfflineBenchmarkConfig &config,
                               double backend_initialization_ms) {
  if (config.measured_runs < 5) {
    throw std::invalid_argument("Offline benchmark requires at least five measured runs");
  }
  if (!std::isfinite(backend_initialization_ms) || backend_initialization_ms < 0.0) {
    throw std::invalid_argument("FFT backend initialization time must be finite and non-negative");
  }
}

[[nodiscard]] PipelineMeasurement measure_pipeline_once(const std::filesystem::path &input_path,
                                                        FFTBackend &fft,
                                                        const OfflineBenchmarkConfig &config) {
  // This boundary implements the locked definition:
  // start reading WAV -> every onset has been formatted for output.
  const auto start = BenchmarkClock::now();
  const auto audio = read_wav(input_path);
  const auto flux =
      compute_spectral_flux(audio.mono_samples, audio.sample_rate_hz, fft, config.spectral_flux);
  const auto onsets = detect_causal_onsets(flux, config.onset);

  // An in-memory stream includes the cost of producing the complete onset CSV
  // without mixing storage-device and file-system cache noise into CPU timing.
  std::ostringstream serialized_onsets;
  write_onsets_csv(serialized_onsets, onsets);
  const auto end = BenchmarkClock::now();

  const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
  if (!std::isfinite(elapsed_ms) || elapsed_ms <= 0.0) {
    throw std::runtime_error("Offline benchmark measured a non-positive duration");
  }
  const double audio_duration_seconds = audio.duration_seconds();
  const double real_time_factor =
      audio_duration_seconds == 0.0 ? 0.0 : audio_duration_seconds * 1000.0 / elapsed_ms;
  return PipelineMeasurement{
      .elapsed_ms = elapsed_ms,
      .real_time_factor = real_time_factor,
      .audio_duration_seconds = audio_duration_seconds,
      .spectral_flux_frame_count = flux.size(),
      .onset_count = onsets.size(),
      .serialized_onset_bytes = serialized_onsets.str().size(),
  };
}

[[nodiscard]] OfflineBenchmarkSummary summarize_runs(std::span<const OfflineBenchmarkRun> runs) {
  if (runs.empty()) {
    throw std::invalid_argument("Cannot summarize an empty benchmark result");
  }
  std::vector<double> elapsed;
  std::vector<double> real_time_factors;
  elapsed.reserve(runs.size());
  real_time_factors.reserve(runs.size());
  for (const auto &run : runs) {
    elapsed.push_back(run.elapsed_ms);
    real_time_factors.push_back(run.real_time_factor);
  }
  return OfflineBenchmarkSummary{
      .median_elapsed_ms = linear_percentile(elapsed, 0.50),
      .p95_elapsed_ms = linear_percentile(elapsed, 0.95),
      .p99_elapsed_ms = linear_percentile(elapsed, 0.99),
      .median_real_time_factor = linear_percentile(real_time_factors, 0.50),
      .minimum_real_time_factor =
          *std::min_element(real_time_factors.begin(), real_time_factors.end()),
  };
}

[[nodiscard]] std::string json_escape(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result.push_back(character);
      break;
    }
  }
  return result;
}

} // namespace

double linear_percentile(std::span<const double> values, double probability) {
  if (values.empty()) {
    throw std::invalid_argument("Percentile input cannot be empty");
  }
  if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0) {
    throw std::invalid_argument("Percentile probability must be in [0, 1]");
  }
  std::vector<double> sorted(values.begin(), values.end());
  for (const double value : sorted) {
    if (!std::isfinite(value)) {
      throw std::invalid_argument("Percentile input must contain only finite values");
    }
  }
  std::sort(sorted.begin(), sorted.end());
  const double rank = probability * static_cast<double>(sorted.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(rank));
  const auto upper = static_cast<std::size_t>(std::ceil(rank));
  const double fraction = rank - static_cast<double>(lower);
  return sorted[lower] + fraction * (sorted[upper] - sorted[lower]);
}

OfflineBenchmarkResult benchmark_offline_wav(const std::filesystem::path &input_path,
                                             FFTBackend &fft, double backend_initialization_ms,
                                             OfflineBenchmarkConfig config) {
  validate_benchmark_config(config, backend_initialization_ms);

  // Warm-ups execute the identical data path but never enter the raw sample set.
  for (std::size_t index = 0; index < config.warmup_runs; ++index) {
    static_cast<void>(measure_pipeline_once(input_path, fft, config));
  }

  std::vector<OfflineBenchmarkRun> runs;
  runs.reserve(config.measured_runs);
  for (std::size_t index = 0; index < config.measured_runs; ++index) {
    const auto measurement = measure_pipeline_once(input_path, fft, config);
    runs.push_back(OfflineBenchmarkRun{
        .run_index = index + 1,
        .elapsed_ms = measurement.elapsed_ms,
        .real_time_factor = measurement.real_time_factor,
        .audio_duration_seconds = measurement.audio_duration_seconds,
        .spectral_flux_frame_count = measurement.spectral_flux_frame_count,
        .onset_count = measurement.onset_count,
        .serialized_onset_bytes = measurement.serialized_onset_bytes,
    });
  }

  return OfflineBenchmarkResult{
      .input_path = input_path,
      .backend_name = std::string(fft.name()),
      .backend_initialization_ms = backend_initialization_ms,
      .config = config,
      .runs = runs,
      .summary = summarize_runs(runs),
  };
}

void write_offline_benchmark_runs_csv(std::ostream &output,
                                      std::span<const OfflineBenchmarkRun> runs) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  output << "run_index,elapsed_ms,real_time_factor,audio_duration_seconds,"
            "spectral_flux_frame_count,onset_count,serialized_onset_bytes\n"
         << std::fixed << std::setprecision(9);
  for (const auto &run : runs) {
    output << run.run_index << ',' << run.elapsed_ms << ',' << run.real_time_factor << ','
           << run.audio_duration_seconds << ',' << run.spectral_flux_frame_count << ','
           << run.onset_count << ',' << run.serialized_onset_bytes << '\n';
  }
  output.flags(old_flags);
  output.precision(old_precision);
  if (!output) {
    throw std::runtime_error("Unable to write offline benchmark CSV");
  }
}

void write_offline_benchmark_runs_csv(const std::filesystem::path &path,
                                      std::span<const OfflineBenchmarkRun> runs) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open offline benchmark CSV: " + path.string());
  }
  write_offline_benchmark_runs_csv(output, runs);
}

void write_offline_benchmark_summary_json(std::ostream &output,
                                          const OfflineBenchmarkResult &result) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  const auto &summary = result.summary;
  output << std::fixed << std::setprecision(9) << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"metric\": \"wav_read_to_onset_csv_serialized\",\n"
         << "  \"percentile_method\": \"linear_type_7\",\n"
         << "  \"input_path\": \"" << json_escape(result.input_path.string()) << "\",\n"
         << "  \"backend\": \"" << json_escape(result.backend_name) << "\",\n"
         << "  \"backend_initialization_ms\": " << result.backend_initialization_ms << ",\n"
         << "  \"warmup_runs\": " << result.config.warmup_runs << ",\n"
         << "  \"measured_runs\": " << result.config.measured_runs << ",\n"
         << "  \"median_elapsed_ms\": " << summary.median_elapsed_ms << ",\n"
         << "  \"p95_elapsed_ms\": " << summary.p95_elapsed_ms << ",\n"
         << "  \"p99_elapsed_ms\": " << summary.p99_elapsed_ms << ",\n"
         << "  \"median_real_time_factor\": " << summary.median_real_time_factor << ",\n"
         << "  \"minimum_real_time_factor\": " << summary.minimum_real_time_factor << "\n"
         << "}\n";
  output.flags(old_flags);
  output.precision(old_precision);
  if (!output) {
    throw std::runtime_error("Unable to write offline benchmark JSON");
  }
}

void write_offline_benchmark_summary_json(const std::filesystem::path &path,
                                          const OfflineBenchmarkResult &result) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open offline benchmark JSON: " + path.string());
  }
  write_offline_benchmark_summary_json(output, result);
}

} // namespace hero_audio

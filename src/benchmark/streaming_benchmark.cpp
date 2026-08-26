#include "hero_audio/streaming_benchmark.hpp"

#include "hero_audio/offline_benchmark.hpp"
#include "hero_audio/streaming_processor.hpp"
#include "hero_audio/wav_reader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hero_audio {
namespace {

using BenchmarkClock = std::chrono::steady_clock;

constexpr double kTimestampTolerance = 1.0e-12;
constexpr double kNumericTolerance = 1.0e-7;
constexpr double kFluxAbsoluteTolerance = 1.0e-5;
constexpr double kFluxRelativeTolerance = 1.0e-6;

void validate_config(const StreamingBenchmarkConfig &config, const AudioBuffer &audio,
                     const FFTBackend &fft, double backend_initialization_ms) {
  if (config.measured_passes == 0) {
    throw std::invalid_argument("Streaming benchmark requires at least one measured pass");
  }
  if (!std::isfinite(backend_initialization_ms) || backend_initialization_ms < 0.0) {
    throw std::invalid_argument("FFT backend initialization time must be finite and non-negative");
  }
  if (config.spectral_flux.frame_size_samples == 0 ||
      config.spectral_flux.hop_size_samples == 0) {
    throw std::invalid_argument("Streaming frame and hop sizes must be non-zero");
  }
  if (config.spectral_flux.frame_size_samples != fft.fft_size()) {
    throw std::invalid_argument("Streaming frame size must match FFT backend size");
  }
  if (audio.mono_samples.size() < config.spectral_flux.frame_size_samples) {
    throw std::invalid_argument("Streaming benchmark requires at least one complete FFT frame");
  }
}

[[nodiscard]] bool near(double left, double right, double tolerance) {
  return std::abs(left - right) <= tolerance;
}

[[nodiscard]] bool flux_near(float left, float right) {
  const double left_double = static_cast<double>(left);
  const double right_double = static_cast<double>(right);
  const double scale = std::max(std::abs(left_double), std::abs(right_double));
  return std::abs(left_double - right_double) <=
         kFluxAbsoluteTolerance + kFluxRelativeTolerance * scale;
}

[[nodiscard]] bool frames_equal(const SpectralFluxFrame &left,
                                const SpectralFluxFrame &right) {
  return left.frame_index == right.frame_index &&
         left.frame_start_sample == right.frame_start_sample &&
         near(left.frame_start_seconds, right.frame_start_seconds, kTimestampTolerance) &&
         near(left.frame_center_seconds, right.frame_center_seconds, kTimestampTolerance) &&
         near(left.available_seconds, right.available_seconds, kTimestampTolerance) &&
         flux_near(left.spectral_flux, right.spectral_flux);
}

[[nodiscard]] bool onsets_equal(const OnsetEvent &left, const OnsetEvent &right) {
  return left.frame_index == right.frame_index &&
         near(left.onset_time_seconds, right.onset_time_seconds, kTimestampTolerance) &&
         near(left.emitted_at_seconds, right.emitted_at_seconds, kTimestampTolerance) &&
         near(left.algorithm_delay_ms, right.algorithm_delay_ms, kNumericTolerance) &&
         flux_near(left.spectral_flux, right.spectral_flux) &&
         near(left.threshold, right.threshold, kNumericTolerance);
}

[[nodiscard]] StreamingConsistency compare_with_offline(
    std::span<const SpectralFluxFrame> streaming_frames,
    std::span<const OnsetEvent> streaming_onsets,
    std::span<const SpectralFluxFrame> offline_frames,
    std::span<const OnsetEvent> offline_onsets) {
  const bool frame_count_equal = streaming_frames.size() == offline_frames.size();
  const bool onset_count_equal = streaming_onsets.size() == offline_onsets.size();
  bool flux_frames_equal = frame_count_equal;
  double maximum_flux_absolute_error = 0.0;
  const auto common_frame_count = std::min(streaming_frames.size(), offline_frames.size());
  for (std::size_t index = 0; index < common_frame_count; ++index) {
    maximum_flux_absolute_error =
        std::max(maximum_flux_absolute_error,
                 std::abs(static_cast<double>(streaming_frames[index].spectral_flux) -
                          static_cast<double>(offline_frames[index].spectral_flux)));
    flux_frames_equal =
        flux_frames_equal && frames_equal(streaming_frames[index], offline_frames[index]);
  }

  bool onset_events_equal = onset_count_equal;
  const auto common_onset_count = std::min(streaming_onsets.size(), offline_onsets.size());
  for (std::size_t index = 0; index < common_onset_count; ++index) {
    onset_events_equal =
        onset_events_equal && onsets_equal(streaming_onsets[index], offline_onsets[index]);
  }

  return StreamingConsistency{
      .streaming_frame_count = streaming_frames.size(),
      .offline_frame_count = offline_frames.size(),
      .streaming_onset_count = streaming_onsets.size(),
      .offline_onset_count = offline_onsets.size(),
      .maximum_flux_absolute_error = maximum_flux_absolute_error,
      .frame_count_equal = frame_count_equal,
      .flux_frames_equal = flux_frames_equal,
      .onset_events_equal = onset_events_equal,
      .overall_matches = flux_frames_equal && onset_events_equal,
  };
}

void replay_warmup(std::span<const float> samples, StreamingProcessor &processor,
                   std::size_t complete_hop_count, std::size_t hop_size) {
  processor.reset();
  for (std::size_t hop = 0; hop < complete_hop_count; ++hop) {
    static_cast<void>(processor.push_hop(samples.subspan(hop * hop_size, hop_size)));
  }
}

[[nodiscard]] StreamingHopSummary
summarize_measurements(std::span<const StreamingHopMeasurement> measurements,
                       double hop_period_ms) {
  if (measurements.empty()) {
    throw std::invalid_argument("Cannot summarize an empty streaming benchmark");
  }
  std::vector<double> compute_times;
  compute_times.reserve(measurements.size());
  std::size_t deadline_misses = 0;
  for (const auto &measurement : measurements) {
    compute_times.push_back(measurement.compute_ms);
    if (!measurement.deadline_met) {
      ++deadline_misses;
    }
  }
  const double p95 = linear_percentile(compute_times, 0.95);
  return StreamingHopSummary{
      .measured_hop_count = measurements.size(),
      .deadline_miss_count = deadline_misses,
      .hop_period_ms = hop_period_ms,
      .p50_compute_ms = linear_percentile(compute_times, 0.50),
      .p95_compute_ms = p95,
      .p99_compute_ms = linear_percentile(compute_times, 0.99),
      .maximum_compute_ms = *std::max_element(compute_times.begin(), compute_times.end()),
      .p95_within_hop_period = p95 < hop_period_ms,
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

StreamingBenchmarkResult benchmark_streaming_wav(const std::filesystem::path &input_path,
                                                 FFTBackend &fft,
                                                 double backend_initialization_ms,
                                                 StreamingBenchmarkConfig config) {
  // Decoding is deliberately outside per-hop timing: this benchmark simulates
  // already-decoded float32 samples arriving from an audio callback.
  const auto audio = read_wav(input_path);
  validate_config(config, audio, fft, backend_initialization_ms);

  const auto hop_size = config.spectral_flux.hop_size_samples;
  const auto complete_hop_count = audio.mono_samples.size() / hop_size;
  const auto processed_sample_count = complete_hop_count * hop_size;
  const auto ignored_tail_sample_count = audio.mono_samples.size() - processed_sample_count;
  const double hop_period_ms = static_cast<double>(hop_size) * 1000.0 /
                               static_cast<double>(audio.sample_rate_hz);

  StreamingProcessor processor(
      fft, StreamingProcessorConfig{.sample_rate_hz = audio.sample_rate_hz,
                                    .spectral_flux = config.spectral_flux,
                                    .onset = config.onset});
  for (std::size_t pass = 0; pass < config.warmup_passes; ++pass) {
    replay_warmup(audio.mono_samples, processor, complete_hop_count, hop_size);
  }

  const auto frames_per_pass =
      1 + (processed_sample_count - config.spectral_flux.frame_size_samples) / hop_size;
  std::vector<StreamingHopMeasurement> measurements;
  measurements.reserve(frames_per_pass * config.measured_passes);
  std::vector<SpectralFluxFrame> first_pass_flux_frames;
  std::vector<OnsetEvent> first_pass_onsets;
  first_pass_flux_frames.reserve(frames_per_pass);

  for (std::size_t pass = 0; pass < config.measured_passes; ++pass) {
    processor.reset();
    for (std::size_t hop = 0; hop < complete_hop_count; ++hop) {
      const auto hop_samples =
          std::span<const float>(audio.mono_samples).subspan(hop * hop_size, hop_size);
      const auto start = BenchmarkClock::now();
      const auto result = processor.push_hop(hop_samples);
      const auto end = BenchmarkClock::now();

      // The first frame_size/hop_size-1 calls only fill startup state. Keeping
      // them out prevents trivial copies from biasing steady-state percentiles.
      if (!result.flux_frame.has_value()) {
        continue;
      }
      const double compute_ms =
          std::chrono::duration<double, std::milli>(end - start).count();
      if (!std::isfinite(compute_ms) || compute_ms < 0.0) {
        throw std::runtime_error("Streaming benchmark measured an invalid duration");
      }
      measurements.push_back(StreamingHopMeasurement{
          .pass_index = pass + 1,
          .input_hop_index = result.input_hop_index,
          .frame_index = result.flux_frame->frame_index,
          .compute_ms = compute_ms,
          .deadline_ms = hop_period_ms,
          .deadline_met = compute_ms < hop_period_ms,
          .onset_emitted = result.onset.has_value(),
      });
      if (pass == 0) {
        first_pass_flux_frames.push_back(*result.flux_frame);
        if (result.onset.has_value()) {
          first_pass_onsets.push_back(*result.onset);
        }
      }
    }
  }

  // Offline comparison intentionally happens after all timed passes.
  const auto offline_frames = compute_spectral_flux(audio.mono_samples, audio.sample_rate_hz,
                                                    fft, config.spectral_flux);
  const auto offline_onsets = detect_causal_onsets(offline_frames, config.onset);
  const auto consistency = compare_with_offline(first_pass_flux_frames, first_pass_onsets,
                                                offline_frames, offline_onsets);
  const auto summary = summarize_measurements(measurements, hop_period_ms);

  return StreamingBenchmarkResult{
      .input_path = input_path,
      .backend_name = std::string(fft.name()),
      .backend_initialization_ms = backend_initialization_ms,
      .sample_rate_hz = audio.sample_rate_hz,
      .input_sample_count = audio.mono_samples.size(),
      .processed_sample_count = processed_sample_count,
      .ignored_tail_sample_count = ignored_tail_sample_count,
      .config = config,
      .measurements = std::move(measurements),
      .first_pass_flux_frames = std::move(first_pass_flux_frames),
      .first_pass_onsets = std::move(first_pass_onsets),
      .summary = summary,
      .consistency = consistency,
  };
}

void write_streaming_hop_measurements_csv(
    std::ostream &output, std::span<const StreamingHopMeasurement> measurements) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  output << "pass_index,input_hop_index,frame_index,compute_ms,deadline_ms,deadline_met,"
            "onset_emitted\n"
         << std::fixed << std::setprecision(9);
  for (const auto &measurement : measurements) {
    output << measurement.pass_index << ',' << measurement.input_hop_index << ','
           << measurement.frame_index << ',' << measurement.compute_ms << ','
           << measurement.deadline_ms << ',' << (measurement.deadline_met ? 1 : 0) << ','
           << (measurement.onset_emitted ? 1 : 0) << '\n';
  }
  output.flags(old_flags);
  output.precision(old_precision);
  if (!output) {
    throw std::runtime_error("Unable to write streaming benchmark CSV");
  }
}

void write_streaming_hop_measurements_csv(
    const std::filesystem::path &path,
    std::span<const StreamingHopMeasurement> measurements) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open streaming benchmark CSV: " + path.string());
  }
  write_streaming_hop_measurements_csv(output, measurements);
}

void write_streaming_benchmark_summary_json(std::ostream &output,
                                            const StreamingBenchmarkResult &result) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  const auto &summary = result.summary;
  const auto &consistency = result.consistency;
  output << std::fixed << std::setprecision(9) << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"metric\": \"steady_state_hop_compute\",\n"
         << "  \"timed_scope\": \"hop_insert_hann_fft_flux_causal_detection\",\n"
         << "  \"excluded_scope\": \"wav_decode_backend_initialization_audio_device_driver\",\n"
         << "  \"percentile_method\": \"linear_type_7\",\n"
         << "  \"startup_fill_hops_excluded\": true,\n"
         << "  \"input_path\": \"" << json_escape(result.input_path.string()) << "\",\n"
         << "  \"backend\": \"" << json_escape(result.backend_name) << "\",\n"
         << "  \"backend_initialization_ms\": " << result.backend_initialization_ms << ",\n"
         << "  \"sample_rate_hz\": " << result.sample_rate_hz << ",\n"
         << "  \"frame_size_samples\": " << result.config.spectral_flux.frame_size_samples
         << ",\n"
         << "  \"hop_size_samples\": " << result.config.spectral_flux.hop_size_samples << ",\n"
         << "  \"input_sample_count\": " << result.input_sample_count << ",\n"
         << "  \"processed_sample_count\": " << result.processed_sample_count << ",\n"
         << "  \"ignored_tail_sample_count\": " << result.ignored_tail_sample_count << ",\n"
         << "  \"warmup_passes\": " << result.config.warmup_passes << ",\n"
         << "  \"measured_passes\": " << result.config.measured_passes << ",\n"
         << "  \"measured_hop_count\": " << summary.measured_hop_count << ",\n"
         << "  \"hop_period_ms\": " << summary.hop_period_ms << ",\n"
         << "  \"p50_compute_ms\": " << summary.p50_compute_ms << ",\n"
         << "  \"p95_compute_ms\": " << summary.p95_compute_ms << ",\n"
         << "  \"p99_compute_ms\": " << summary.p99_compute_ms << ",\n"
         << "  \"maximum_compute_ms\": " << summary.maximum_compute_ms << ",\n"
         << "  \"deadline_miss_count\": " << summary.deadline_miss_count << ",\n"
         << "  \"p95_within_hop_period\": "
         << (summary.p95_within_hop_period ? "true" : "false") << ",\n"
         << "  \"offline_consistency\": {\n"
         << "    \"streaming_frame_count\": " << consistency.streaming_frame_count << ",\n"
         << "    \"offline_frame_count\": " << consistency.offline_frame_count << ",\n"
         << "    \"streaming_onset_count\": " << consistency.streaming_onset_count << ",\n"
         << "    \"offline_onset_count\": " << consistency.offline_onset_count << ",\n"
         << "    \"maximum_flux_absolute_error\": "
         << consistency.maximum_flux_absolute_error << ",\n"
         << "    \"frame_count_equal\": "
         << (consistency.frame_count_equal ? "true" : "false") << ",\n"
         << "    \"flux_frames_equal\": "
         << (consistency.flux_frames_equal ? "true" : "false") << ",\n"
         << "    \"onset_events_equal\": "
         << (consistency.onset_events_equal ? "true" : "false") << ",\n"
         << "    \"overall_matches\": "
         << (consistency.overall_matches ? "true" : "false") << "\n"
         << "  }\n"
         << "}\n";
  output.flags(old_flags);
  output.precision(old_precision);
  if (!output) {
    throw std::runtime_error("Unable to write streaming benchmark JSON");
  }
}

void write_streaming_benchmark_summary_json(const std::filesystem::path &path,
                                            const StreamingBenchmarkResult &result) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open streaming benchmark JSON: " + path.string());
  }
  write_streaming_benchmark_summary_json(output, result);
}

} // namespace hero_audio

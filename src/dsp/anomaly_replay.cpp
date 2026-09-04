#include "hero_audio/anomaly_replay.hpp"

#include "hero_audio/anomaly_output.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace hero_audio {
namespace {

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

void write_stats(std::ostream &output, const RobustFeatureStats &stats) {
  output << "{\"median\": " << stats.median
         << ", \"median_absolute_deviation\": "
         << stats.median_absolute_deviation << ", \"scale\": " << stats.scale
         << '}';
}

} // namespace

AnomalyReplayResult replay_anomaly_audio(std::span<const float> mono_samples,
                                         std::uint32_t sample_rate_hz,
                                         FFTBackend &fft,
                                         AnomalyReplayConfig config) {
  if (sample_rate_hz == 0) {
    throw std::invalid_argument("Replay sample rate must be non-zero");
  }
  if (std::any_of(mono_samples.begin(), mono_samples.end(),
                  [](float value) { return !std::isfinite(value); })) {
    throw std::invalid_argument("Replay audio contains a non-finite sample");
  }

  config.streaming.sample_rate_hz = sample_rate_hz;
  config.anomaly.sample_rate_hz = sample_rate_hz;
  config.anomaly.hop_size_samples = config.streaming.spectral_flux.hop_size_samples;
  const auto frame_size = config.streaming.spectral_flux.frame_size_samples;
  const auto hop_size = config.streaming.spectral_flux.hop_size_samples;
  if (frame_size != fft.fft_size()) {
    throw std::invalid_argument("Replay frame size must match FFT backend size");
  }

  StreamingProcessor processor(fft, config.streaming);
  AcousticFeatureExtractor extractor(AcousticFeatureExtractorConfig{
      .frame_size_samples = frame_size,
      .hop_size_samples = hop_size,
  });
  TransientAnomalyDetector detector(config.anomaly);

  const std::size_t hop_count = mono_samples.size() / hop_size;
  if (hop_count > std::numeric_limits<std::size_t>::max() / hop_size) {
    throw std::overflow_error("Replay processed sample count overflows size_t");
  }
  AnomalyReplayResult result{
      .sample_rate_hz = sample_rate_hz,
      .input_sample_count = mono_samples.size(),
      .processed_sample_count = hop_count * hop_size,
      .ignored_tail_sample_count = mono_samples.size() % hop_size,
      .processed_hop_count = hop_count,
      .backend_name = std::string(fft.name()),
      .config = config,
  };
  result.frames.reserve(hop_count);

  for (std::size_t hop_index = 0; hop_index < hop_count; ++hop_index) {
    const auto hop = mono_samples.subspan(hop_index * hop_size, hop_size);
    const auto streaming = processor.push_hop(hop);
    const auto features = extractor.process_hop(hop, streaming);
    if (!features.has_value()) {
      continue;
    }
    auto decision = detector.process(*features, streaming.onset,
                                     config.operating_state);
    if (decision.scored_event.has_value() &&
        decision.scored_event->emitted_anomaly) {
      result.emitted_events.push_back(AnomalyReplayEvent{
          .input_hop_index = hop_index,
          .event = *decision.scored_event,
      });
    }
    result.frames.push_back(AnomalyReplayFrame{
        .input_hop_index = hop_index,
        .features = *features,
        .decision = std::move(decision),
    });
  }

  result.baseline = detector.baseline();
  result.baseline_frames_required = detector.baseline_frames_required();
  result.baseline_frames_collected = detector.baseline_frames_collected();
  result.scored_event_count = detector.scored_event_count();
  result.anomaly_candidate_count = detector.anomaly_candidate_count();
  return result;
}

void write_anomaly_replay_frames_csv(std::ostream &output,
                                     const AnomalyReplayResult &result) {
  write_anomaly_frame_csv_header(output);
  for (const auto &frame : result.frames) {
    // Zero is a deliberate deterministic placeholder. Replay has no meaningful
    // wall-clock anomaly_analysis_ms measurement.
    write_anomaly_frame_csv_row(output, frame.input_hop_index, 0, 0.0,
                                result.config.operating_state, frame.features,
                                frame.decision, 0.0);
  }
  if (!output) {
    throw std::runtime_error("Unable to write anomaly replay frame CSV");
  }
}

void write_anomaly_replay_frames_csv(const std::filesystem::path &path,
                                     const AnomalyReplayResult &result) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open anomaly replay frame CSV: " + path.string());
  }
  write_anomaly_replay_frames_csv(output, result);
}

void write_anomaly_replay_events_csv(std::ostream &output,
                                     const AnomalyReplayResult &result) {
  write_anomaly_event_csv_header(output);
  for (const auto &record : result.emitted_events) {
    const auto &event = record.event;
    const double algorithm_delay_ms =
        (event.emitted_at_seconds - event.onset_time_seconds) * 1000.0;
    write_anomaly_event_csv_row(output, record.input_hop_index, 0, 0.0,
                                result.config.operating_state, event,
                                "algorithm_only_replay", algorithm_delay_ms);
  }
  if (!output) {
    throw std::runtime_error("Unable to write anomaly replay event CSV");
  }
}

void write_anomaly_replay_events_csv(const std::filesystem::path &path,
                                     const AnomalyReplayResult &result) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open anomaly replay event CSV: " + path.string());
  }
  write_anomaly_replay_events_csv(output, result);
}

void write_anomaly_replay_summary_json(std::ostream &output,
                                       const AnomalyReplayResult &result,
                                       std::string_view input_path) {
  const auto &anomaly = result.config.anomaly;
  output << std::fixed << std::setprecision(9) << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"metric\": \"deterministic_anomaly_replay\",\n"
         << "  \"input_path\": \"" << json_escape(input_path) << "\",\n"
         << "  \"input_format_requirement\": \"mono_ieee_float32_for_bit_exact_live_replay\",\n"
         << "  \"sample_rate_hz\": " << result.sample_rate_hz << ",\n"
         << "  \"input_sample_count\": " << result.input_sample_count << ",\n"
         << "  \"processed_sample_count\": " << result.processed_sample_count << ",\n"
         << "  \"ignored_tail_sample_count\": "
         << result.ignored_tail_sample_count << ",\n"
         << "  \"processed_hop_count\": " << result.processed_hop_count << ",\n"
         << "  \"frame_size_samples\": "
         << result.config.streaming.spectral_flux.frame_size_samples << ",\n"
         << "  \"hop_size_samples\": "
         << result.config.streaming.spectral_flux.hop_size_samples << ",\n"
         << "  \"backend\": \"" << json_escape(result.backend_name) << "\",\n"
         << "  \"operating_state\": \""
         << to_string(result.config.operating_state) << "\",\n"
         << "  \"baseline_seconds_requested\": " << anomaly.baseline_seconds << ",\n"
         << "  \"baseline_frames_required\": "
         << result.baseline_frames_required << ",\n"
         << "  \"baseline_frames_collected\": "
         << result.baseline_frames_collected << ",\n"
         << "  \"baseline_complete\": "
         << (result.baseline.has_value() ? "true" : "false") << ",\n"
         << "  \"flux_positive_z_threshold\": " << anomaly.flux_z_threshold
         << ",\n"
         << "  \"rms_positive_z_threshold\": " << anomaly.rms_z_threshold
         << ",\n"
         << "  \"peak_positive_z_threshold\": " << anomaly.peak_z_threshold
         << ",\n"
         << "  \"anomaly_refractory_seconds\": "
         << anomaly.anomaly_refractory_seconds << ",\n"
         << "  \"capture_integrity_pass\": true,\n"
         << "  \"capture_integrity_source\": \"replay_input_only; validate original live summary separately\",\n"
         << "  \"deterministic_non_timing_output\": true,\n"
         << "  \"frame_analysis_ms_is_placeholder_zero\": true,\n"
         << "  \"event_delay_scope\": \"algorithm_only_replay\",\n"
         << "  \"scored_onset_count\": " << result.scored_event_count << ",\n"
         << "  \"above_threshold_candidate_count\": "
         << result.anomaly_candidate_count << ",\n"
         << "  \"emitted_anomaly_count\": " << result.emitted_events.size() << ",\n"
         << "  \"baseline_statistics\": ";
  if (!result.baseline.has_value()) {
    output << "null\n";
  } else {
    output << "{\n    \"spectral_flux\": ";
    write_stats(output, result.baseline->spectral_flux);
    output << ",\n    \"frame_rms\": ";
    write_stats(output, result.baseline->frame_rms);
    output << ",\n    \"peak_absolute\": ";
    write_stats(output, result.baseline->peak_absolute);
    output << ",\n    \"zero_crossing_rate\": ";
    write_stats(output, result.baseline->zero_crossing_rate);
    output << "\n  }\n";
  }
  output << "}\n";
  if (!output) {
    throw std::runtime_error("Unable to write anomaly replay summary JSON");
  }
}

void write_anomaly_replay_summary_json(const std::filesystem::path &path,
                                       const AnomalyReplayResult &result,
                                       std::string_view input_path) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open anomaly replay summary JSON: " + path.string());
  }
  write_anomaly_replay_summary_json(output, result, input_path);
}

} // namespace hero_audio

#include "hero_audio/acoustic_features.hpp"
#include "hero_audio/anomaly_detector.hpp"
#include "hero_audio/anomaly_output.hpp"
#include "hero_audio/coreaudio_input.hpp"
#include "hero_audio/fft_backend.hpp"
#include "hero_audio/offline_benchmark.hpp"
#include "hero_audio/streaming_processor.hpp"
#include "hero_audio/wav_writer.hpp"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t interrupted = 0;

void handle_interrupt(int) { interrupted = 1; }

struct Arguments {
  std::filesystem::path output_directory;
  double duration_seconds{10.0};
  std::string backend{"auto"};
  double anomaly_baseline_seconds{30.0};
  hero_audio::OperatingState operating_state{hero_audio::OperatingState::Steady};
};

struct LiveMeasurement {
  std::uint64_t sequence{};
  std::uint64_t first_sample_index{};
  double capture_seconds{};
  bool discontinuity_before{};
  bool steady_state{};
  std::optional<std::size_t> frame_index;
  double queue_wait_ms{};
  double compute_ms{};
  double deadline_ms{};
  bool deadline_met{};
  bool onset_emitted{};
};

struct LiveEvent {
  std::uint64_t sequence{};
  std::size_t segment_index{};
  std::size_t frame_index{};
  double onset_time_seconds{};
  double emitted_at_audio_seconds{};
  double algorithm_delay_ms{};
  double post_callback_processing_ms{};
  double estimated_software_detection_delay_ms{};
  float spectral_flux{};
  double threshold{};
};

[[nodiscard]] std::string usage() {
  return "Usage: hero-audio-live output-directory [--seconds 10] "
         "[--backend auto|fftw|reference] [--anomaly-baseline-seconds 30] "
         "[--operating-state idle|startup|steady|shutdown]\n"
         "\nCreates capture.wav, onset/measurement outputs, and separate transient "
         "anomaly audit outputs.\n";
}

[[nodiscard]] double parse_duration(std::string_view text) {
  std::string owned(text);
  char *end = nullptr;
  errno = 0;
  const double value = std::strtod(owned.c_str(), &end);
  if (errno != 0 || end != owned.c_str() + owned.size() || !std::isfinite(value) ||
      value < 0.25 || value > 3600.0) {
    throw std::invalid_argument("--seconds must be a finite number in [0.25, 3600]");
  }
  return value;
}

[[nodiscard]] double parse_baseline_duration(std::string_view text) {
  std::string owned(text);
  char *end = nullptr;
  errno = 0;
  const double value = std::strtod(owned.c_str(), &end);
  if (errno != 0 || end != owned.c_str() + owned.size() || !std::isfinite(value) ||
      value < 0.1 || value > 3600.0) {
    throw std::invalid_argument(
        "--anomaly-baseline-seconds must be a finite number in [0.1, 3600]");
  }
  return value;
}

[[nodiscard]] Arguments parse_arguments(int argc, char **argv) {
  if (argc < 2) {
    throw std::invalid_argument(usage());
  }
  Arguments result{.output_directory = argv[1]};
  for (int index = 2; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    const std::string_view value = argv[++index];
    if (option == "--seconds") {
      result.duration_seconds = parse_duration(value);
    } else if (option == "--backend") {
      result.backend = value;
      if (result.backend != "auto" && result.backend != "fftw" &&
          result.backend != "reference") {
        throw std::invalid_argument("--backend must be auto, fftw, or reference");
      }
    } else if (option == "--anomaly-baseline-seconds") {
      result.anomaly_baseline_seconds = parse_baseline_duration(value);
    } else if (option == "--operating-state") {
      result.operating_state = hero_audio::parse_operating_state(value);
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(option));
    }
  }
  return result;
}

[[nodiscard]] hero_audio::FFTBackendKind select_backend(std::string_view requested) {
  if (requested == "reference") {
    return hero_audio::FFTBackendKind::Reference;
  }
  if (requested == "fftw") {
    for (const auto kind : hero_audio::available_fft_backends()) {
      if (kind == hero_audio::FFTBackendKind::FFTW) {
        return kind;
      }
    }
    throw std::runtime_error("FFTW3f was requested but is unavailable in this build");
  }
  for (const auto kind : hero_audio::available_fft_backends()) {
    if (kind == hero_audio::FFTBackendKind::FFTW) {
      return kind;
    }
  }
  return hero_audio::FFTBackendKind::Reference;
}

[[nodiscard]] std::uint32_t integer_sample_rate(double sample_rate_hz) {
  const double rounded = std::round(sample_rate_hz);
  if (!std::isfinite(sample_rate_hz) || sample_rate_hz <= 0.0 ||
      std::abs(sample_rate_hz - rounded) > 1.0e-6 ||
      rounded > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("CoreAudio input sample rate is not a supported integer rate");
  }
  return static_cast<std::uint32_t>(rounded);
}

void prepare_output_directory(const std::filesystem::path &directory,
                              const std::vector<std::filesystem::path> &targets) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    throw std::runtime_error("Unable to create live output directory: " +
                             error.message());
  }
  for (const auto &target : targets) {
    if (std::filesystem::exists(target)) {
      throw std::runtime_error("Refusing to overwrite existing live output: " +
                               target.string());
    }
  }
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

void write_measurement_header(std::ostream &output) {
  output << "sequence,first_sample_index,capture_seconds,discontinuity_before,"
            "steady_state,frame_index,queue_wait_ms,compute_ms,deadline_ms,"
            "deadline_met,onset_emitted\n";
}

void write_measurement(std::ostream &output, const LiveMeasurement &measurement) {
  output << std::fixed << std::setprecision(9) << measurement.sequence << ','
         << measurement.first_sample_index << ',' << measurement.capture_seconds << ','
         << (measurement.discontinuity_before ? 1 : 0) << ','
         << (measurement.steady_state ? 1 : 0) << ',';
  if (measurement.frame_index.has_value()) {
    output << *measurement.frame_index;
  }
  output << ',' << measurement.queue_wait_ms << ',' << measurement.compute_ms << ','
         << measurement.deadline_ms << ',' << (measurement.deadline_met ? 1 : 0) << ','
         << (measurement.onset_emitted ? 1 : 0) << '\n';
}

void write_event_header(std::ostream &output) {
  output << "sequence,segment_index,frame_index,onset_time_seconds,"
            "emitted_at_audio_seconds,algorithm_delay_ms,"
            "post_callback_processing_ms,estimated_software_detection_delay_ms,"
            "spectral_flux,threshold\n";
}

void write_event(std::ostream &output, const LiveEvent &event) {
  output << std::fixed << std::setprecision(9) << event.sequence << ','
         << event.segment_index << ',' << event.frame_index << ','
         << event.onset_time_seconds << ',' << event.emitted_at_audio_seconds << ','
         << event.algorithm_delay_ms << ',' << event.post_callback_processing_ms << ','
         << event.estimated_software_detection_delay_ms << ',' << event.spectral_flux << ','
         << event.threshold << '\n';
}

[[nodiscard]] std::optional<double> percentile_or_none(std::span<const double> values,
                                                       double probability) {
  if (values.empty()) {
    return std::nullopt;
  }
  return hero_audio::linear_percentile(values, probability);
}

void write_optional_json_number(std::ostream &output, std::optional<double> value) {
  if (value.has_value()) {
    output << *value;
  } else {
    output << "null";
  }
}

void write_summary(const std::filesystem::path &path, const Arguments &arguments,
                   const hero_audio::CoreAudioInput &input,
                   const hero_audio::CoreAudioCaptureStats &capture,
                   std::string_view backend_name, double backend_initialization_ms,
                   double coreaudio_initialization_ms, double actual_wall_seconds,
                   double hop_period_ms, std::span<const double> steady_compute_times,
                   std::span<const double> queue_wait_times,
                   std::span<const double> detection_delays,
                   std::size_t deadline_miss_count, std::size_t discontinuity_count,
                   std::size_t processor_reset_count, std::size_t onset_count,
                   std::uint64_t written_sample_count) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open live summary JSON: " + path.string());
  }
  const auto p50_compute = percentile_or_none(steady_compute_times, 0.50);
  const auto p95_compute = percentile_or_none(steady_compute_times, 0.95);
  const auto p99_compute = percentile_or_none(steady_compute_times, 0.99);
  const auto p95_queue = percentile_or_none(queue_wait_times, 0.95);
  const auto p95_detection = percentile_or_none(detection_delays, 0.95);

  output << std::fixed << std::setprecision(9) << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"metric\": \"live_coreaudio_streaming_onset_detection\",\n"
         << "  \"input_scope\": \"default_input_device_mono_float32\",\n"
         << "  \"excluded_latency\": \"device_hardware_and_driver_before_callback\",\n"
         << "  \"output_directory\": \""
         << json_escape(arguments.output_directory.string()) << "\",\n"
         << "  \"device_id\": " << input.device_id() << ",\n"
         << "  \"device_name\": \"" << json_escape(input.device_name()) << "\",\n"
         << "  \"sample_rate_hz\": " << input.sample_rate_hz() << ",\n"
         << "  \"source_channel_count\": " << input.source_channel_count() << ",\n"
         << "  \"frame_size_samples\": 1024,\n"
         << "  \"hop_size_samples\": " << hero_audio::kLiveHopSize << ",\n"
         << "  \"queue_capacity_hops\": " << hero_audio::kLiveQueueCapacity << ",\n"
         << "  \"maximum_frames_per_slice\": " << input.maximum_frames_per_slice()
         << ",\n"
         << "  \"backend\": \"" << json_escape(backend_name) << "\",\n"
         << "  \"backend_initialization_ms\": " << backend_initialization_ms << ",\n"
         << "  \"coreaudio_initialization_ms\": " << coreaudio_initialization_ms << ",\n"
         << "  \"requested_duration_seconds\": " << arguments.duration_seconds << ",\n"
         << "  \"actual_wall_seconds\": " << actual_wall_seconds << ",\n"
         << "  \"written_sample_count\": " << written_sample_count << ",\n"
         << "  \"callback_count\": " << capture.callback_count << ",\n"
         << "  \"input_timeline_frame_count\": "
         << capture.input_timeline_frame_count << ",\n"
         << "  \"rendered_frame_count\": " << capture.rendered_frame_count << ",\n"
         << "  \"enqueued_hop_count\": " << capture.enqueued_hop_count << ",\n"
         << "  \"dropped_hop_count\": " << capture.dropped_hop_count << ",\n"
         << "  \"render_error_count\": " << capture.render_error_count << ",\n"
         << "  \"last_render_status\": " << capture.last_render_status << ",\n"
         << "  \"partial_sample_count\": " << capture.partial_sample_count << ",\n"
         << "  \"discontinuity_count\": " << discontinuity_count << ",\n"
         << "  \"processor_reset_count\": " << processor_reset_count << ",\n"
         << "  \"onset_count\": " << onset_count << ",\n"
         << "  \"steady_state_hop_count\": " << steady_compute_times.size() << ",\n"
         << "  \"hop_period_ms\": " << hop_period_ms << ",\n"
         << "  \"p50_compute_ms\": ";
  write_optional_json_number(output, p50_compute);
  output << ",\n  \"p95_compute_ms\": ";
  write_optional_json_number(output, p95_compute);
  output << ",\n  \"p99_compute_ms\": ";
  write_optional_json_number(output, p99_compute);
  output << ",\n  \"p95_callback_to_consumer_ms\": ";
  write_optional_json_number(output, p95_queue);
  output << ",\n  \"p95_estimated_software_detection_delay_ms\": ";
  write_optional_json_number(output, p95_detection);
  output << ",\n  \"deadline_miss_count\": " << deadline_miss_count
         << ",\n  \"p95_compute_within_hop_period\": ";
  if (p95_compute.has_value()) {
    output << (*p95_compute < hop_period_ms ? "true" : "false");
  } else {
    output << "null";
  }
  output << ",\n  \"p95_detection_delay_within_30_ms\": ";
  if (p95_detection.has_value()) {
    output << (*p95_detection < 30.0 ? "true" : "false");
  } else {
    output << "null";
  }
  output << ",\n  \"capture_integrity_pass\": "
         << (capture.dropped_hop_count == 0 && capture.render_error_count == 0
                 ? "true"
                 : "false")
         << "\n}\n";
  if (!output) {
    throw std::runtime_error("Unable to write live summary JSON");
  }
}

void write_robust_stats_json(std::ostream &output,
                             const hero_audio::RobustFeatureStats &stats) {
  output << "{\"median\": " << stats.median
         << ", \"median_absolute_deviation\": "
         << stats.median_absolute_deviation << ", \"scale\": " << stats.scale
         << '}';
}

void write_anomaly_summary(
    const std::filesystem::path &path, const Arguments &arguments,
    const hero_audio::TransientAnomalyDetector &detector,
    const hero_audio::CoreAudioCaptureStats &capture,
    std::span<const double> anomaly_analysis_times,
    std::size_t calibration_reset_count) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open anomaly summary JSON: " + path.string());
  }
  const auto &config = detector.config();
  const auto p95_analysis = percentile_or_none(anomaly_analysis_times, 0.95);
  const bool capture_integrity =
      capture.dropped_hop_count == 0 && capture.render_error_count == 0;

  output << std::fixed << std::setprecision(9) << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"model\": \"steady_state_transient_anomaly_v1\",\n"
         << "  \"status\": \"research_prototype_requires_labelled_validation\",\n"
         << "  \"scope\": \"unexpected_transient_events_during_declared_steady_operation\",\n"
         << "  \"not_a_claim_of\": \"universal_anomaly_or_fault_type_diagnosis\",\n"
         << "  \"operating_state\": \""
         << hero_audio::to_string(arguments.operating_state) << "\",\n"
         << "  \"baseline_human_assumption\": \"first uninterrupted steady interval is verified normal\",\n"
         << "  \"baseline_seconds_requested\": " << config.baseline_seconds << ",\n"
         << "  \"baseline_frames_required\": "
         << detector.baseline_frames_required() << ",\n"
         << "  \"baseline_frames_collected\": "
         << detector.baseline_frames_collected() << ",\n"
         << "  \"baseline_complete\": "
         << (detector.baseline().has_value() ? "true" : "false") << ",\n"
         << "  \"baseline_policy_after_completion\": \"frozen\",\n"
         << "  \"calibration_reset_count\": " << calibration_reset_count << ",\n"
         << "  \"requires_confirmed_causal_onset\": true,\n"
         << "  \"flux_positive_z_threshold\": " << config.flux_z_threshold << ",\n"
         << "  \"rms_positive_z_threshold\": " << config.rms_z_threshold << ",\n"
         << "  \"peak_positive_z_threshold\": " << config.peak_z_threshold << ",\n"
         << "  \"combined_score_threshold\": 1.000000000,\n"
         << "  \"anomaly_refractory_seconds\": "
         << config.anomaly_refractory_seconds << ",\n"
         << "  \"scored_onset_count\": " << detector.scored_event_count() << ",\n"
         << "  \"above_threshold_candidate_count\": "
         << detector.anomaly_candidate_count() << ",\n"
         << "  \"emitted_anomaly_count\": "
         << detector.emitted_anomaly_count() << ",\n"
         << "  \"p95_downstream_anomaly_analysis_ms\": ";
  write_optional_json_number(output, p95_analysis);
  output << ",\n  \"core_hop_compute_metric_includes_anomaly_layer\": false,\n"
         << "  \"capture_integrity_pass\": "
         << (capture_integrity ? "true" : "false") << ",\n"
         << "  \"labelled_validation_required_before_accuracy_claims\": true,\n"
         << "  \"baseline_statistics\": ";
  if (!detector.baseline().has_value()) {
    output << "null\n";
  } else {
    const auto &baseline = *detector.baseline();
    output << "{\n    \"spectral_flux\": ";
    write_robust_stats_json(output, baseline.spectral_flux);
    output << ",\n    \"frame_rms\": ";
    write_robust_stats_json(output, baseline.frame_rms);
    output << ",\n    \"peak_absolute\": ";
    write_robust_stats_json(output, baseline.peak_absolute);
    output << ",\n    \"zero_crossing_rate\": ";
    write_robust_stats_json(output, baseline.zero_crossing_rate);
    output << "\n  }\n";
  }
  output << "}\n";
  if (!output) {
    throw std::runtime_error("Unable to write anomaly summary JSON");
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << usage();
      return 0;
    }
    const auto arguments = parse_arguments(argc, argv);
    const auto backend_kind = select_backend(arguments.backend);

    const auto fft_start = Clock::now();
    auto fft = hero_audio::make_fft_backend(backend_kind, 1024);
    const auto fft_end = Clock::now();
    const double backend_initialization_ms =
        std::chrono::duration<double, std::milli>(fft_end - fft_start).count();

    hero_audio::LiveAudioHopQueue queue;
    const auto coreaudio_start = Clock::now();
    hero_audio::CoreAudioInput input(queue);
    const auto coreaudio_end = Clock::now();
    const double coreaudio_initialization_ms =
        std::chrono::duration<double, std::milli>(coreaudio_end - coreaudio_start).count();
    const std::uint32_t sample_rate_hz = integer_sample_rate(input.sample_rate_hz());
    const double hop_period_ms = static_cast<double>(hero_audio::kLiveHopSize) * 1000.0 /
                                 static_cast<double>(sample_rate_hz);
    hero_audio::StreamingProcessor processor(
        *fft, hero_audio::StreamingProcessorConfig{.sample_rate_hz = sample_rate_hz});
    hero_audio::AcousticFeatureExtractor feature_extractor(
        hero_audio::AcousticFeatureExtractorConfig{
            .frame_size_samples = 1024,
            .hop_size_samples = hero_audio::kLiveHopSize,
        });
    hero_audio::TransientAnomalyDetector anomaly_detector(
        hero_audio::TransientAnomalyConfig{
            .sample_rate_hz = sample_rate_hz,
            .hop_size_samples = hero_audio::kLiveHopSize,
            .baseline_seconds = arguments.anomaly_baseline_seconds,
        });

    const auto wav_path = arguments.output_directory / "capture.wav";
    const auto event_path = arguments.output_directory / "onsets.csv";
    const auto measurement_path = arguments.output_directory / "hop-measurements.csv";
    const auto summary_path = arguments.output_directory / "summary.json";
    const auto anomaly_frame_path =
        arguments.output_directory / "anomaly-frames.csv";
    const auto anomaly_event_path =
        arguments.output_directory / "anomaly-events.csv";
    const auto anomaly_summary_path =
        arguments.output_directory / "anomaly-summary.json";
    prepare_output_directory(
        arguments.output_directory,
        {wav_path, event_path, measurement_path, summary_path, anomaly_frame_path,
         anomaly_event_path, anomaly_summary_path});

    hero_audio::Pcm16WavWriter wav(wav_path, sample_rate_hz);
    std::ofstream events(event_path, std::ios::trunc);
    std::ofstream measurements(measurement_path, std::ios::trunc);
    std::ofstream anomaly_frames(anomaly_frame_path, std::ios::trunc);
    std::ofstream anomaly_events(anomaly_event_path, std::ios::trunc);
    if (!events || !measurements || !anomaly_frames || !anomaly_events) {
      throw std::runtime_error("Unable to open live CSV output files");
    }
    write_event_header(events);
    write_measurement_header(measurements);
    hero_audio::write_anomaly_frame_csv_header(anomaly_frames);
    hero_audio::write_anomaly_event_csv_header(anomaly_events);

    std::vector<double> steady_compute_times;
    std::vector<double> queue_wait_times;
    std::vector<double> detection_delays;
    std::vector<double> anomaly_analysis_times;
    steady_compute_times.reserve(
        static_cast<std::size_t>(arguments.duration_seconds * sample_rate_hz /
                                 hero_audio::kLiveHopSize));
    queue_wait_times.reserve(steady_compute_times.capacity());
    anomaly_analysis_times.reserve(steady_compute_times.capacity());

    std::uint64_t expected_sequence = 0;
    std::uint64_t expected_first_sample_index = 0;
    std::uint64_t segment_start_sample = 0;
    std::size_t segment_index = 0;
    std::size_t discontinuity_count = 0;
    std::size_t processor_reset_count = 0;
    std::size_t deadline_miss_count = 0;
    std::size_t onset_count = 0;
    std::size_t anomaly_calibration_reset_count = 0;
    bool anomaly_baseline_completion_announced = false;

    auto consume = [&](const hero_audio::LiveAudioHopBlock &block) {
      const bool discontinuity = block.sequence != expected_sequence ||
                                 block.first_sample_index != expected_first_sample_index;
      if (block.first_sample_index < wav.sample_count()) {
        throw std::runtime_error("CoreAudio hop timeline moved backwards");
      }
      if (block.first_sample_index > wav.sample_count()) {
        wav.append_silence(
            static_cast<std::size_t>(block.first_sample_index - wav.sample_count()));
      }
      wav.append(block.samples);

      if (discontinuity) {
        processor.reset();
        feature_extractor.reset();
        if (anomaly_detector.handle_discontinuity()) {
          ++anomaly_calibration_reset_count;
        }
        segment_start_sample = block.first_sample_index;
        ++segment_index;
        ++discontinuity_count;
        ++processor_reset_count;
      }
      expected_sequence = block.sequence + 1;
      expected_first_sample_index = block.first_sample_index + hero_audio::kLiveHopSize;

      const double queue_wait_ms = hero_audio::coreaudio_host_time_delta_ms(
          block.callback_host_time, hero_audio::coreaudio_current_host_time());
      const auto compute_start = Clock::now();
      const auto result = processor.push_hop(block.samples);
      const auto compute_end = Clock::now();
      const double compute_ms =
          std::chrono::duration<double, std::milli>(compute_end - compute_start).count();

      // Keep the established hop-compute metric above unchanged. Feature
      // extraction and anomaly scoring are a downstream measurement scope.
      const auto anomaly_start = Clock::now();
      const auto acoustic_features = feature_extractor.process_hop(block.samples, result);
      std::optional<hero_audio::TransientAnomalyFrameResult> anomaly_result;
      if (acoustic_features.has_value()) {
        anomaly_result = anomaly_detector.process(
            *acoustic_features, result.onset, arguments.operating_state);
      }
      const auto anomaly_end = Clock::now();
      const double anomaly_analysis_ms =
          std::chrono::duration<double, std::milli>(anomaly_end - anomaly_start).count();
      const bool steady_state = result.flux_frame.has_value();
      const bool deadline_met = compute_ms < hop_period_ms;
      if (steady_state) {
        steady_compute_times.push_back(compute_ms);
        if (!deadline_met) {
          ++deadline_miss_count;
        }
      }
      queue_wait_times.push_back(queue_wait_ms);
      if (acoustic_features.has_value()) {
        anomaly_analysis_times.push_back(anomaly_analysis_ms);
        const double segment_offset_seconds =
            static_cast<double>(segment_start_sample) / sample_rate_hz;
        hero_audio::write_anomaly_frame_csv_row(
            anomaly_frames, block.sequence, segment_index, segment_offset_seconds,
            arguments.operating_state, *acoustic_features, *anomaly_result,
            anomaly_analysis_ms);
        if (anomaly_result->scored_event.has_value() &&
            anomaly_result->scored_event->emitted_anomaly) {
          const auto &anomaly = *anomaly_result->scored_event;
          const double anomaly_delay_ms =
              (anomaly.emitted_at_seconds - anomaly.onset_time_seconds) * 1000.0 +
              queue_wait_ms + compute_ms + anomaly_analysis_ms;
          hero_audio::write_anomaly_event_csv_row(
              anomaly_events, block.sequence, segment_index,
              segment_offset_seconds, arguments.operating_state, anomaly,
              anomaly_delay_ms);
        }
        if (anomaly_detector.baseline().has_value() &&
            !anomaly_baseline_completion_announced) {
          std::cout << "anomaly_baseline_complete: monitoring has started\n";
          anomaly_baseline_completion_announced = true;
        }
      }

      write_measurement(
          measurements,
          LiveMeasurement{
              .sequence = block.sequence,
              .first_sample_index = block.first_sample_index,
              .capture_seconds =
                  static_cast<double>(block.first_sample_index) / sample_rate_hz,
              .discontinuity_before = discontinuity,
              .steady_state = steady_state,
              .frame_index = steady_state
                                 ? std::optional<std::size_t>(result.flux_frame->frame_index)
                                 : std::nullopt,
              .queue_wait_ms = queue_wait_ms,
              .compute_ms = compute_ms,
              .deadline_ms = hop_period_ms,
              .deadline_met = deadline_met,
              .onset_emitted = result.onset.has_value(),
          });

      if (result.onset.has_value()) {
        const double segment_offset_seconds =
            static_cast<double>(segment_start_sample) / sample_rate_hz;
        const double post_callback_processing_ms = queue_wait_ms + compute_ms;
        const double estimated_delay_ms =
            result.onset->algorithm_delay_ms + post_callback_processing_ms;
        detection_delays.push_back(estimated_delay_ms);
        ++onset_count;
        write_event(events,
                    LiveEvent{
                        .sequence = block.sequence,
                        .segment_index = segment_index,
                        .frame_index = result.onset->frame_index,
                        .onset_time_seconds =
                            segment_offset_seconds + result.onset->onset_time_seconds,
                        .emitted_at_audio_seconds =
                            segment_offset_seconds + result.onset->emitted_at_seconds,
                        .algorithm_delay_ms = result.onset->algorithm_delay_ms,
                        .post_callback_processing_ms = post_callback_processing_ms,
                        .estimated_software_detection_delay_ms = estimated_delay_ms,
                        .spectral_flux = result.onset->spectral_flux,
                        .threshold = result.onset->threshold,
                    });
      }
    };

    interrupted = 0;
    std::signal(SIGINT, handle_interrupt);
    std::cout << "input_device: " << input.device_name() << " (id " << input.device_id()
              << ")\n"
              << "sample_rate_hz: " << sample_rate_hz << '\n'
              << "source_channel_count: " << input.source_channel_count() << '\n'
              << "hop_period_ms: " << std::fixed << std::setprecision(6)
              << hop_period_ms << '\n'
              << "listening_seconds: " << arguments.duration_seconds
              << " (press Ctrl-C to stop early)\n"
              << "operating_state: "
              << hero_audio::to_string(arguments.operating_state) << '\n'
              << "verified_normal_baseline_seconds: "
              << arguments.anomaly_baseline_seconds << '\n';

    const auto wall_start = Clock::now();
    input.start();
    hero_audio::LiveAudioHopBlock block;
    while (interrupted == 0 &&
           std::chrono::duration<double>(Clock::now() - wall_start).count() <
               arguments.duration_seconds) {
      bool consumed_any = false;
      while (queue.try_pop(block)) {
        consume(block);
        consumed_any = true;
      }
      if (!consumed_any) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    input.stop();
    const auto wall_end = Clock::now();
    while (queue.try_pop(block)) {
      consume(block);
    }

    const auto capture = input.stats();
    const std::uint64_t complete_timeline_samples =
        capture.input_timeline_frame_count / hero_audio::kLiveHopSize *
        hero_audio::kLiveHopSize;
    if (complete_timeline_samples > wav.sample_count()) {
      wav.append_silence(
          static_cast<std::size_t>(complete_timeline_samples - wav.sample_count()));
    }
    wav.finalize();
    events.flush();
    measurements.flush();
    anomaly_frames.flush();
    anomaly_events.flush();
    if (!events || !measurements || !anomaly_frames || !anomaly_events) {
      throw std::runtime_error("Unable to finalize live CSV output files");
    }
    if (capture.rendered_frame_count == 0 || steady_compute_times.empty()) {
      throw std::runtime_error(
          "No analyzable microphone audio was received. Grant Terminal microphone "
          "permission in System Settings > Privacy & Security > Microphone, then use "
          "a new output directory.");
    }

    const double actual_wall_seconds =
        std::chrono::duration<double>(wall_end - wall_start).count();
    write_summary(summary_path, arguments, input, capture, fft->name(),
                  backend_initialization_ms, coreaudio_initialization_ms,
                  actual_wall_seconds, hop_period_ms, steady_compute_times,
                  queue_wait_times, detection_delays, deadline_miss_count,
                  discontinuity_count, processor_reset_count, onset_count,
                  wav.sample_count());
    write_anomaly_summary(anomaly_summary_path, arguments, anomaly_detector,
                          capture, anomaly_analysis_times,
                          anomaly_calibration_reset_count);

    const double p95_compute = hero_audio::linear_percentile(steady_compute_times, 0.95);
    std::cout << "capture_complete: true\n"
              << "rendered_frames: " << capture.rendered_frame_count << '\n'
              << "enqueued_hops: " << capture.enqueued_hop_count << '\n'
              << "dropped_hops: " << capture.dropped_hop_count << '\n'
              << "render_errors: " << capture.render_error_count << '\n'
              << "detected_onsets: " << onset_count << '\n'
              << "anomaly_baseline_complete: "
              << (anomaly_detector.baseline().has_value() ? "true" : "false")
              << '\n'
              << "scored_onsets: " << anomaly_detector.scored_event_count() << '\n'
              << "emitted_transient_anomalies: "
              << anomaly_detector.emitted_anomaly_count() << '\n'
              << "p95_hop_compute_ms: " << p95_compute << '\n'
              << "p95_within_hop_period: "
              << (p95_compute < hop_period_ms ? "true" : "false") << '\n'
              << "capture_integrity: "
              << (capture.dropped_hop_count == 0 && capture.render_error_count == 0
                      ? "PASS"
                      : "FAIL")
              << '\n'
              << "output_directory: " << arguments.output_directory.string() << '\n';
    if (!anomaly_detector.baseline().has_value()) {
      std::cout << "warning: anomaly baseline incomplete; no anomaly conclusion is valid\n";
    }
    if (backend_kind == hero_audio::FFTBackendKind::Reference) {
      std::cout << "warning: reference-radix2 is not the official CPU performance baseline\n";
    }
    return capture.dropped_hop_count == 0 && capture.render_error_count == 0 ? 0 : 3;
  } catch (const std::invalid_argument &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}

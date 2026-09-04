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
         "[--backend auto|fftw|reference]\n"
         "\nCreates capture.wav, onsets.csv, hop-measurements.csv and summary.json.\n";
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

    const auto wav_path = arguments.output_directory / "capture.wav";
    const auto event_path = arguments.output_directory / "onsets.csv";
    const auto measurement_path = arguments.output_directory / "hop-measurements.csv";
    const auto summary_path = arguments.output_directory / "summary.json";
    prepare_output_directory(arguments.output_directory,
                             {wav_path, event_path, measurement_path, summary_path});

    hero_audio::Pcm16WavWriter wav(wav_path, sample_rate_hz);
    std::ofstream events(event_path, std::ios::trunc);
    std::ofstream measurements(measurement_path, std::ios::trunc);
    if (!events || !measurements) {
      throw std::runtime_error("Unable to open live CSV output files");
    }
    write_event_header(events);
    write_measurement_header(measurements);

    std::vector<double> steady_compute_times;
    std::vector<double> queue_wait_times;
    std::vector<double> detection_delays;
    steady_compute_times.reserve(
        static_cast<std::size_t>(arguments.duration_seconds * sample_rate_hz /
                                 hero_audio::kLiveHopSize));
    queue_wait_times.reserve(steady_compute_times.capacity());

    std::uint64_t expected_sequence = 0;
    std::uint64_t expected_first_sample_index = 0;
    std::uint64_t segment_start_sample = 0;
    std::size_t segment_index = 0;
    std::size_t discontinuity_count = 0;
    std::size_t processor_reset_count = 0;
    std::size_t deadline_miss_count = 0;
    std::size_t onset_count = 0;

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
      const bool steady_state = result.flux_frame.has_value();
      const bool deadline_met = compute_ms < hop_period_ms;
      if (steady_state) {
        steady_compute_times.push_back(compute_ms);
        if (!deadline_met) {
          ++deadline_miss_count;
        }
      }
      queue_wait_times.push_back(queue_wait_ms);

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
              << " (press Ctrl-C to stop early)\n";

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
    if (!events || !measurements) {
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

    const double p95_compute = hero_audio::linear_percentile(steady_compute_times, 0.95);
    std::cout << "capture_complete: true\n"
              << "rendered_frames: " << capture.rendered_frame_count << '\n'
              << "enqueued_hops: " << capture.enqueued_hop_count << '\n'
              << "dropped_hops: " << capture.dropped_hop_count << '\n'
              << "render_errors: " << capture.render_error_count << '\n'
              << "detected_onsets: " << onset_count << '\n'
              << "p95_hop_compute_ms: " << p95_compute << '\n'
              << "p95_within_hop_period: "
              << (p95_compute < hop_period_ms ? "true" : "false") << '\n'
              << "capture_integrity: "
              << (capture.dropped_hop_count == 0 && capture.render_error_count == 0
                      ? "PASS"
                      : "FAIL")
              << '\n'
              << "output_directory: " << arguments.output_directory.string() << '\n';
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

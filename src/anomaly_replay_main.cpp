#include "hero_audio/anomaly_replay.hpp"
#include "hero_audio/fft_backend.hpp"
#include "hero_audio/wav_reader.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Arguments {
  std::filesystem::path input_wav;
  std::filesystem::path output_directory;
  std::string backend{"auto"};
  double baseline_seconds{30.0};
  double flux_z_threshold{6.0};
  double rms_z_threshold{6.0};
  double peak_z_threshold{6.0};
  double refractory_ms{250.0};
  hero_audio::OperatingState operating_state{hero_audio::OperatingState::Steady};
};

[[nodiscard]] std::string usage() {
  return "Usage: hero-audio-anomaly-replay input-float32.wav output-directory "
         "[--backend auto|fftw|reference] [--baseline-seconds 30] "
         "[--flux-z 6] [--rms-z 6] [--peak-z 6] [--refractory-ms 250] "
         "[--operating-state idle|startup|steady|shutdown]\n";
}

[[nodiscard]] double parse_number_in_range(std::string_view text,
                                           std::string_view option,
                                           double minimum, double maximum,
                                           bool minimum_inclusive) {
  std::string owned(text);
  char *end = nullptr;
  errno = 0;
  const double value = std::strtod(owned.c_str(), &end);
  if (errno != 0 || end != owned.c_str() + owned.size() || !std::isfinite(value) ||
      (minimum_inclusive ? value < minimum : value <= minimum) ||
      value > maximum) {
    throw std::invalid_argument(std::string(option) + " is outside its valid range");
  }
  return value;
}

[[nodiscard]] Arguments parse_arguments(int argc, char **argv) {
  if (argc < 3) {
    throw std::invalid_argument(usage());
  }
  Arguments result{.input_wav = argv[1], .output_directory = argv[2]};
  for (int index = 3; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    const std::string_view value = argv[++index];
    if (option == "--backend") {
      result.backend = value;
      if (result.backend != "auto" && result.backend != "fftw" &&
          result.backend != "reference") {
        throw std::invalid_argument("--backend must be auto, fftw, or reference");
      }
    } else if (option == "--baseline-seconds") {
      result.baseline_seconds =
          parse_number_in_range(value, option, 0.0, 3600.0, false);
    } else if (option == "--flux-z") {
      result.flux_z_threshold =
          parse_number_in_range(value, option, 0.0, 1000.0, false);
    } else if (option == "--rms-z") {
      result.rms_z_threshold =
          parse_number_in_range(value, option, 0.0, 1000.0, false);
    } else if (option == "--peak-z") {
      result.peak_z_threshold =
          parse_number_in_range(value, option, 0.0, 1000.0, false);
    } else if (option == "--refractory-ms") {
      result.refractory_ms =
          parse_number_in_range(value, option, 0.0, 60000.0, true);
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
  for (const auto kind : hero_audio::available_fft_backends()) {
    if (kind == hero_audio::FFTBackendKind::FFTW) {
      return kind;
    }
  }
  if (requested == "fftw") {
    throw std::runtime_error("FFTW3f was requested but is unavailable in this build");
  }
  return hero_audio::FFTBackendKind::Reference;
}

void prepare_output_directory(const std::filesystem::path &directory,
                              const std::vector<std::filesystem::path> &targets) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    throw std::runtime_error("Unable to create replay output directory: " +
                             error.message());
  }
  for (const auto &target : targets) {
    if (std::filesystem::exists(target)) {
      throw std::runtime_error("Refusing to overwrite replay output: " +
                               target.string());
    }
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
    const auto audio = hero_audio::read_wav(arguments.input_wav);
    if (audio.source_channels != 1 ||
        audio.source_encoding != hero_audio::WavSampleEncoding::IeeeFloat ||
        audio.source_bits_per_sample != 32) {
      throw std::invalid_argument(
          "Deterministic replay requires the mono IEEE float32 "
          "capture-analysis-f32.wav produced by hero-audio-live");
    }

    const auto backend_kind = select_backend(arguments.backend);
    auto fft = hero_audio::make_fft_backend(backend_kind, 1024);
    hero_audio::AnomalyReplayConfig config;
    config.anomaly.baseline_seconds = arguments.baseline_seconds;
    config.anomaly.flux_z_threshold = arguments.flux_z_threshold;
    config.anomaly.rms_z_threshold = arguments.rms_z_threshold;
    config.anomaly.peak_z_threshold = arguments.peak_z_threshold;
    config.anomaly.anomaly_refractory_seconds =
        arguments.refractory_ms / 1000.0;
    config.operating_state = arguments.operating_state;
    const auto result = hero_audio::replay_anomaly_audio(
        audio.mono_samples, audio.sample_rate_hz, *fft, config);

    const auto frames_path = arguments.output_directory / "anomaly-frames.csv";
    const auto events_path = arguments.output_directory / "anomaly-events.csv";
    const auto summary_path = arguments.output_directory / "anomaly-summary.json";
    prepare_output_directory(arguments.output_directory,
                             {frames_path, events_path, summary_path});
    hero_audio::write_anomaly_replay_frames_csv(frames_path, result);
    hero_audio::write_anomaly_replay_events_csv(events_path, result);
    hero_audio::write_anomaly_replay_summary_json(
        summary_path, result, arguments.input_wav.string());

    std::cout << "deterministic_replay_complete: true\n"
              << "backend: " << result.backend_name << '\n'
              << "processed_hops: " << result.processed_hop_count << '\n'
              << "ignored_tail_samples: " << result.ignored_tail_sample_count << '\n'
              << "baseline_complete: "
              << (result.baseline.has_value() ? "true" : "false") << '\n'
              << "flux_z_threshold: " << result.config.anomaly.flux_z_threshold
              << '\n'
              << "rms_z_threshold: " << result.config.anomaly.rms_z_threshold
              << '\n'
              << "peak_z_threshold: " << result.config.anomaly.peak_z_threshold
              << '\n'
              << "refractory_ms: "
              << result.config.anomaly.anomaly_refractory_seconds * 1000.0
              << '\n'
              << "emitted_anomalies: " << result.emitted_events.size() << '\n'
              << "output_directory: " << arguments.output_directory.string() << '\n';
    if (!result.baseline.has_value()) {
      std::cout << "warning: baseline incomplete; replay is not eligible for evaluation\n";
    }
    return 0;
  } catch (const std::invalid_argument &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}

#include "hero_audio/anomaly_evaluation.hpp"

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct Arguments {
  std::filesystem::path manifest;
  std::filesystem::path output_directory;
  std::optional<hero_audio::DatasetSplit> split;
  double tolerance_ms{50.0};
};

[[nodiscard]] std::string usage() {
  return "Usage: hero-audio-anomaly-eval manifest.csv output-directory "
         "--split development|held-out [--tolerance-ms 50]\n";
}

[[nodiscard]] double parse_nonnegative_number(std::string_view text,
                                              std::string_view option) {
  double value = 0.0;
  std::istringstream parser{std::string(text)};
  parser.imbue(std::locale::classic());
  parser >> std::noskipws >> value;
  if (!parser || !parser.eof() || !std::isfinite(value) || value < 0.0) {
    throw std::invalid_argument(std::string(option) +
                                " requires a non-negative number");
  }
  return value;
}

[[nodiscard]] Arguments parse_arguments(int argc, char **argv) {
  if (argc < 3) {
    throw std::invalid_argument(usage());
  }
  Arguments result{.manifest = argv[1], .output_directory = argv[2]};
  for (int index = 3; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    const std::string_view value = argv[++index];
    if (option == "--split") {
      if (result.split.has_value()) {
        throw std::invalid_argument("--split may be specified only once");
      }
      result.split = hero_audio::parse_dataset_split(value);
    } else if (option == "--tolerance-ms") {
      result.tolerance_ms = parse_nonnegative_number(value, option);
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(option));
    }
  }
  if (!result.split.has_value()) {
    throw std::invalid_argument("--split development|held-out is required");
  }
  return result;
}

void prepare_output_directory(const std::filesystem::path &directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    throw std::runtime_error("Unable to create evaluation output directory: " +
                             error.message());
  }
  for (const auto &name : {"matches.csv", "session-metrics.csv", "metrics.json"}) {
    const auto target = directory / name;
    if (std::filesystem::exists(target)) {
      throw std::runtime_error("Refusing to overwrite evaluation output: " +
                               target.string());
    }
  }
}

void print_optional_metric(std::string_view name,
                           const std::optional<double> &value) {
  std::cout << name << ": ";
  if (value.has_value()) {
    std::cout << *value;
  } else {
    std::cout << "not_available";
  }
  std::cout << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << usage();
      return 0;
    }
    const auto arguments = parse_arguments(argc, argv);
    const auto evaluation = hero_audio::evaluate_anomaly_manifest(
        arguments.manifest, *arguments.split, arguments.tolerance_ms / 1000.0);
    prepare_output_directory(arguments.output_directory);

    const auto matches_path = arguments.output_directory / "matches.csv";
    const auto sessions_path = arguments.output_directory / "session-metrics.csv";
    const auto metrics_path = arguments.output_directory / "metrics.json";
    hero_audio::write_anomaly_matches_csv(matches_path, evaluation);
    hero_audio::write_anomaly_session_metrics_csv(sessions_path, evaluation);
    hero_audio::write_anomaly_dataset_metrics_json(metrics_path, evaluation);

    const auto &metrics = evaluation.metrics;
    std::cout << std::fixed << std::setprecision(6)
              << "anomaly_evaluation_complete: true\n"
              << "split: " << hero_audio::to_string(evaluation.split) << '\n'
              << "sessions: " << evaluation.sessions.size() << '\n'
              << "tolerance_ms: " << arguments.tolerance_ms << '\n'
              << "true_positives: " << metrics.true_positives << '\n'
              << "false_positives: " << metrics.false_positives << '\n'
              << "false_negatives: " << metrics.false_negatives << '\n'
              << "precision: " << metrics.precision << '\n'
              << "recall: " << metrics.recall << '\n'
              << "f1: " << metrics.f1 << '\n'
              << "false_alarms_per_hour: " << metrics.false_alarms_per_hour
              << '\n';
    std::cout << "detection_delay_scope: ";
    if (evaluation.detection_delay_scope.has_value()) {
      std::cout << *evaluation.detection_delay_scope;
    } else {
      std::cout << "not_available";
    }
    std::cout << '\n';
    print_optional_metric("p50_detection_delay_ms",
                          metrics.p50_detection_delay_ms);
    print_optional_metric("p95_detection_delay_ms",
                          metrics.p95_detection_delay_ms);
    std::cout << "matches_csv: " << matches_path.string() << '\n'
              << "session_metrics_csv: " << sessions_path.string() << '\n'
              << "metrics_json: " << metrics_path.string() << '\n';
    return 0;
  } catch (const std::invalid_argument &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}

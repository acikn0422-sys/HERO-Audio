#include "hero_audio/onset_evaluation.hpp"

#include <charconv>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct Arguments {
  std::filesystem::path predictions;
  std::filesystem::path references;
  std::optional<std::filesystem::path> matches_output;
  std::optional<std::filesystem::path> metrics_output;
  double tolerance_ms{50.0};
};

double parse_nonnegative_number(std::string_view text, std::string_view option) {
  double value = 0.0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      !std::isfinite(value) || value < 0.0) {
    throw std::invalid_argument(std::string(option) + " requires a non-negative number");
  }
  return value;
}

Arguments parse_arguments(int argc, char **argv) {
  if (argc < 3) {
    throw std::invalid_argument(
        "Usage: hero-audio-eval predictions.csv references.csv "
        "[--matches matches.csv] [--metrics metrics.json] [--tolerance-ms 50]");
  }
  Arguments result{.predictions = argv[1], .references = argv[2]};
  for (int index = 3; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    const std::string_view value = argv[++index];
    if (option == "--matches") {
      result.matches_output = value;
    } else if (option == "--metrics") {
      result.metrics_output = value;
    } else if (option == "--tolerance-ms") {
      result.tolerance_ms = parse_nonnegative_number(value, option);
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(option));
    }
  }
  return result;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto predictions = hero_audio::read_onset_times_csv(arguments.predictions);
    const auto references = hero_audio::read_onset_times_csv(arguments.references);
    const auto evaluation =
        hero_audio::evaluate_onsets(predictions, references, arguments.tolerance_ms / 1000.0);
    const auto &metrics = evaluation.metrics;

    std::cout << std::fixed << std::setprecision(6) << "tolerance_ms: " << arguments.tolerance_ms
              << '\n'
              << "predictions: " << metrics.prediction_count << '\n'
              << "references: " << metrics.reference_count << '\n'
              << "true_positives: " << metrics.true_positives << '\n'
              << "false_positives: " << metrics.false_positives << '\n'
              << "false_negatives: " << metrics.false_negatives << '\n'
              << "precision: " << metrics.precision << '\n'
              << "recall: " << metrics.recall << '\n'
              << "f1: " << metrics.f1 << '\n';

    if (arguments.matches_output.has_value()) {
      hero_audio::write_onset_matches_csv(*arguments.matches_output, evaluation.matches);
      std::cout << "matches_csv: " << arguments.matches_output->string() << '\n';
    }
    if (arguments.metrics_output.has_value()) {
      hero_audio::write_evaluation_json(*arguments.metrics_output, evaluation);
      std::cout << "metrics_json: " << arguments.metrics_output->string() << '\n';
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

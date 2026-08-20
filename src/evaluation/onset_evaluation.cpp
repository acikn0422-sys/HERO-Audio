#include "hero_audio/onset_evaluation.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace hero_audio {
namespace {

struct IndexedTime {
  double seconds{};
  std::size_t original_index{};
};

enum class Choice : std::uint8_t { None, SkipPrediction, SkipReference, Match };

struct Cell {
  // Lexicographic objective for one DP prefix: maximise match_count first,
  // then minimise total_error. choice records the winning transition so the
  // actual pairs can be reconstructed after scoring is complete.
  std::size_t match_count{};
  long double total_error{};
  Choice choice{Choice::None};
};

[[nodiscard]] int choice_priority(Choice choice) noexcept {
  switch (choice) {
  case Choice::Match:
    return 3;
  case Choice::SkipPrediction:
    return 2;
  case Choice::SkipReference:
    return 1;
  case Choice::None:
    return 0;
  }
  return 0;
}

[[nodiscard]] bool is_better(const Cell &candidate, const Cell &current) noexcept {
  if (candidate.match_count != current.match_count) {
    return candidate.match_count > current.match_count;
  }
  if (candidate.total_error != current.total_error) {
    return candidate.total_error < current.total_error;
  }
  return choice_priority(candidate.choice) > choice_priority(current.choice);
}

[[nodiscard]] bool is_within_tolerance(double error, double prediction_seconds,
                                       double reference_seconds,
                                       double tolerance_seconds) noexcept {
  // Decimal timestamps such as 1.050 and 1.000 are not represented exactly by
  // binary floating point. Protect the documented inclusive boundary (<=) by
  // a few units of machine precision; this is many orders of magnitude below
  // any meaningful audio timestamp resolution.
  const double scale =
      std::max({1.0, std::abs(prediction_seconds), std::abs(reference_seconds), tolerance_seconds});
  const double roundoff_margin = 8.0 * std::numeric_limits<double>::epsilon() * scale;
  return error <= tolerance_seconds + roundoff_margin;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

[[nodiscard]] std::vector<std::string_view> split_csv_row(std::string_view row) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const auto comma = row.find(',', start);
    const auto end = comma == std::string_view::npos ? row.size() : comma;
    fields.push_back(trim(row.substr(start, end - start)));
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return fields;
}

[[nodiscard]] bool parse_number(std::string_view text, double &value) noexcept {
  text = trim(text);
  if (text.empty()) {
    return false;
  }
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         std::isfinite(value) && value >= 0.0;
}

void require_valid_times(std::span<const double> times, std::string_view name) {
  for (const double value : times) {
    if (!std::isfinite(value) || value < 0.0) {
      throw std::invalid_argument(std::string(name) +
                                  " onset times must be finite and non-negative");
    }
  }
}

[[nodiscard]] std::vector<IndexedTime> sorted_times(std::span<const double> times) {
  std::vector<IndexedTime> result;
  result.reserve(times.size());
  for (std::size_t index = 0; index < times.size(); ++index) {
    result.push_back(IndexedTime{.seconds = times[index], .original_index = index});
  }
  std::stable_sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
    if (left.seconds != right.seconds) {
      return left.seconds < right.seconds;
    }
    return left.original_index < right.original_index;
  });
  return result;
}

[[nodiscard]] EvaluationMetrics make_metrics(std::size_t prediction_count,
                                             std::size_t reference_count,
                                             std::size_t true_positives) noexcept {
  const auto false_positives = prediction_count - true_positives;
  const auto false_negatives = reference_count - true_positives;
  const double precision = prediction_count == 0 ? 0.0
                                                 : static_cast<double>(true_positives) /
                                                       static_cast<double>(prediction_count);
  const double recall = reference_count == 0 ? 0.0
                                             : static_cast<double>(true_positives) /
                                                   static_cast<double>(reference_count);
  const double f1 =
      precision + recall == 0.0 ? 0.0 : 2.0 * precision * recall / (precision + recall);
  return EvaluationMetrics{
      .prediction_count = prediction_count,
      .reference_count = reference_count,
      .true_positives = true_positives,
      .false_positives = false_positives,
      .false_negatives = false_negatives,
      .precision = precision,
      .recall = recall,
      .f1 = f1,
  };
}

} // namespace

OnsetEvaluation evaluate_onsets(std::span<const double> predictions_seconds,
                                std::span<const double> references_seconds,
                                double tolerance_seconds) {
  if (!std::isfinite(tolerance_seconds) || tolerance_seconds < 0.0) {
    throw std::invalid_argument("Onset matching tolerance must be finite and non-negative");
  }
  require_valid_times(predictions_seconds, "Predicted");
  require_valid_times(references_seconds, "Reference");
  const auto predictions = sorted_times(predictions_seconds);
  const auto references = sorted_times(references_seconds);

  const std::size_t rows = predictions.size() + 1;
  const std::size_t columns = references.size() + 1;
  if (rows > std::numeric_limits<std::size_t>::max() / columns) {
    throw std::length_error("Onset matching matrix dimensions overflow size_t");
  }
  std::vector<Cell> table(rows * columns);
  const auto cell = [&table, columns](std::size_t row, std::size_t column) -> Cell & {
    return table[row * columns + column];
  };

  for (std::size_t row = 1; row < rows; ++row) {
    cell(row, 0).choice = Choice::SkipPrediction;
  }
  for (std::size_t column = 1; column < columns; ++column) {
    cell(0, column).choice = Choice::SkipReference;
  }

  for (std::size_t row = 1; row < rows; ++row) {
    for (std::size_t column = 1; column < columns; ++column) {
      // A prefix can end by skipping either side, or by pairing its final two
      // elements when their error is within tolerance. Sorting makes a
      // non-crossing optimum sufficient for one-dimensional absolute error.
      Cell best = cell(row - 1, column);
      best.choice = Choice::SkipPrediction;

      Cell skip_reference = cell(row, column - 1);
      skip_reference.choice = Choice::SkipReference;
      if (is_better(skip_reference, best)) {
        best = skip_reference;
      }

      const double error = std::abs(predictions[row - 1].seconds - references[column - 1].seconds);
      if (is_within_tolerance(error, predictions[row - 1].seconds, references[column - 1].seconds,
                              tolerance_seconds)) {
        Cell match = cell(row - 1, column - 1);
        ++match.match_count;
        match.total_error += static_cast<long double>(error);
        match.choice = Choice::Match;
        if (is_better(match, best)) {
          best = match;
        }
      }
      cell(row, column) = best;
    }
  }

  std::vector<OnsetMatch> matches;
  matches.reserve(cell(rows - 1, columns - 1).match_count);
  std::size_t row = rows - 1;
  std::size_t column = columns - 1;
  // Follow the stored transition at each cell back to the empty prefixes.
  while (row > 0 || column > 0) {
    switch (cell(row, column).choice) {
    case Choice::Match: {
      const auto &prediction = predictions[row - 1];
      const auto &reference = references[column - 1];
      matches.push_back(OnsetMatch{
          .prediction_index = prediction.original_index,
          .reference_index = reference.original_index,
          .prediction_seconds = prediction.seconds,
          .reference_seconds = reference.seconds,
          .absolute_error_seconds = std::abs(prediction.seconds - reference.seconds),
      });
      --row;
      --column;
      break;
    }
    case Choice::SkipPrediction:
      --row;
      break;
    case Choice::SkipReference:
      --column;
      break;
    case Choice::None:
      throw std::logic_error("Invalid onset matching backtracking state");
    }
  }
  std::reverse(matches.begin(), matches.end());

  return OnsetEvaluation{
      .tolerance_seconds = tolerance_seconds,
      .matches = std::move(matches),
      .metrics = make_metrics(predictions.size(), references.size(),
                              cell(rows - 1, columns - 1).match_count),
  };
}

std::vector<double> read_onset_times_csv(std::istream &input, std::string_view column_name) {
  std::vector<double> result;
  std::string line;
  std::size_t line_number = 0;
  std::optional<std::size_t> selected_column;
  bool format_decided = false;
  bool headerless = false;

  while (std::getline(input, line)) {
    ++line_number;
    const auto row = trim(line);
    if (row.empty()) {
      continue;
    }
    const auto fields = split_csv_row(row);
    if (!format_decided) {
      double first_value = 0.0;
      if (fields.size() == 1 && parse_number(fields[0], first_value)) {
        headerless = true;
        format_decided = true;
        result.push_back(first_value);
        continue;
      }
      const auto found = std::find(fields.begin(), fields.end(), column_name);
      if (found == fields.end()) {
        throw std::runtime_error("CSV header has no '" + std::string(column_name) + "' column");
      }
      selected_column = static_cast<std::size_t>(std::distance(fields.begin(), found));
      format_decided = true;
      continue;
    }

    if (headerless && fields.size() != 1) {
      throw std::runtime_error("Headerless onset file must contain one value per line");
    }
    const std::size_t index = headerless ? 0 : *selected_column;
    if (index >= fields.size()) {
      throw std::runtime_error("CSV row " + std::to_string(line_number) + " has too few columns");
    }
    double value = 0.0;
    if (!parse_number(fields[index], value)) {
      throw std::runtime_error("CSV row " + std::to_string(line_number) +
                               " has an invalid onset time");
    }
    result.push_back(value);
  }
  if (!input.eof() && input.fail()) {
    throw std::runtime_error("Unable to read onset CSV stream");
  }
  return result;
}

std::vector<double> read_onset_times_csv(const std::filesystem::path &path,
                                         std::string_view column_name) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Unable to open onset CSV: " + path.string());
  }
  return read_onset_times_csv(input, column_name);
}

void write_onset_matches_csv(std::ostream &output, std::span<const OnsetMatch> matches) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  output << "prediction_index,reference_index,prediction_seconds,reference_seconds,"
            "absolute_error_ms\n";
  output << std::fixed << std::setprecision(9);
  for (const auto &match : matches) {
    output << match.prediction_index << ',' << match.reference_index << ','
           << match.prediction_seconds << ',' << match.reference_seconds << ','
           << match.absolute_error_seconds * 1000.0 << '\n';
  }
  output.flags(old_flags);
  output.precision(old_precision);
  if (!output) {
    throw std::runtime_error("Unable to write onset matches CSV");
  }
}

void write_onset_matches_csv(const std::filesystem::path &path,
                             std::span<const OnsetMatch> matches) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open onset matches CSV: " + path.string());
  }
  write_onset_matches_csv(output, matches);
}

void write_evaluation_json(std::ostream &output, const OnsetEvaluation &evaluation) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  const auto &metrics = evaluation.metrics;
  output << std::fixed << std::setprecision(9) << "{\n"
         << "  \"tolerance_ms\": " << evaluation.tolerance_seconds * 1000.0 << ",\n"
         << "  \"prediction_count\": " << metrics.prediction_count << ",\n"
         << "  \"reference_count\": " << metrics.reference_count << ",\n"
         << "  \"true_positives\": " << metrics.true_positives << ",\n"
         << "  \"false_positives\": " << metrics.false_positives << ",\n"
         << "  \"false_negatives\": " << metrics.false_negatives << ",\n"
         << "  \"precision\": " << metrics.precision << ",\n"
         << "  \"recall\": " << metrics.recall << ",\n"
         << "  \"f1\": " << metrics.f1 << "\n"
         << "}\n";
  output.flags(old_flags);
  output.precision(old_precision);
  if (!output) {
    throw std::runtime_error("Unable to write evaluation JSON");
  }
}

void write_evaluation_json(const std::filesystem::path &path, const OnsetEvaluation &evaluation) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open evaluation JSON: " + path.string());
  }
  write_evaluation_json(output, evaluation);
}

} // namespace hero_audio

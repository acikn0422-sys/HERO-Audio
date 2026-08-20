#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string_view>
#include <vector>

namespace hero_audio {

struct OnsetMatch {
  std::size_t prediction_index{};
  std::size_t reference_index{};
  double prediction_seconds{};
  double reference_seconds{};
  double absolute_error_seconds{};
};

struct EvaluationMetrics {
  std::size_t prediction_count{};
  std::size_t reference_count{};
  std::size_t true_positives{};
  std::size_t false_positives{};
  std::size_t false_negatives{};
  double precision{};
  double recall{};
  double f1{};
};

struct OnsetEvaluation {
  double tolerance_seconds{};
  std::vector<OnsetMatch> matches;
  EvaluationMetrics metrics;
};

// Inputs may be unsorted. Matching first maximises one-to-one match count and,
// among all maximum-cardinality solutions, minimises total absolute time error.
[[nodiscard]] OnsetEvaluation evaluate_onsets(std::span<const double> predictions_seconds,
                                              std::span<const double> references_seconds,
                                              double tolerance_seconds = 0.050);

// Reads either a headerless one-value-per-line file or an unquoted CSV with the
// requested numeric column. Empty lines are ignored.
[[nodiscard]] std::vector<double>
read_onset_times_csv(std::istream &input, std::string_view column_name = "onset_time_seconds");
[[nodiscard]] std::vector<double>
read_onset_times_csv(const std::filesystem::path &path,
                     std::string_view column_name = "onset_time_seconds");

void write_onset_matches_csv(std::ostream &output, std::span<const OnsetMatch> matches);
void write_onset_matches_csv(const std::filesystem::path &path,
                             std::span<const OnsetMatch> matches);
void write_evaluation_json(std::ostream &output, const OnsetEvaluation &evaluation);
void write_evaluation_json(const std::filesystem::path &path, const OnsetEvaluation &evaluation);

} // namespace hero_audio

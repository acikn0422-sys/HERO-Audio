#include "hero_audio/onset_evaluation.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

bool near(double actual, double expected, double tolerance = 1.0e-12) {
  return std::abs(actual - expected) <= tolerance;
}

bool test_maximises_cardinality_before_error() {
  const std::vector<double> predictions{0.040, 0.060};
  const std::vector<double> references{0.000, 0.050};
  const auto result = hero_audio::evaluate_onsets(predictions, references, 0.050);
  return result.matches.size() == 2 && result.matches[0].prediction_index == 0 &&
         result.matches[0].reference_index == 0 && result.matches[1].prediction_index == 1 &&
         result.matches[1].reference_index == 1 && result.metrics.true_positives == 2 &&
         near(result.metrics.f1, 1.0);
}

bool test_minimises_error_after_cardinality() {
  const std::vector<double> predictions{0.000, 0.090};
  const std::vector<double> references{0.040};
  const auto result = hero_audio::evaluate_onsets(predictions, references, 0.050);
  return result.matches.size() == 1 && result.matches[0].prediction_index == 0 &&
         near(result.matches[0].absolute_error_seconds, 0.040) &&
         result.metrics.false_positives == 1;
}

bool test_duplicate_predictions_count_as_false_positives() {
  const std::vector<double> predictions{0.990, 1.000, 1.010};
  const std::vector<double> references{1.000};
  const auto result = hero_audio::evaluate_onsets(predictions, references);
  return result.matches.size() == 1 && result.matches[0].prediction_index == 1 &&
         result.metrics.true_positives == 1 && result.metrics.false_positives == 2 &&
         result.metrics.false_negatives == 0 && near(result.metrics.precision, 1.0 / 3.0) &&
         near(result.metrics.recall, 1.0) && near(result.metrics.f1, 0.5);
}

bool test_unsorted_inputs_preserve_original_indices() {
  const std::vector<double> predictions{1.0, 0.0};
  const std::vector<double> references{0.0, 1.0};
  const auto result = hero_audio::evaluate_onsets(predictions, references, 0.0);
  return result.matches.size() == 2 && result.matches[0].prediction_index == 1 &&
         result.matches[0].reference_index == 0 && result.matches[1].prediction_index == 0 &&
         result.matches[1].reference_index == 1;
}

bool test_tolerance_boundary_is_inclusive() {
  const std::vector<double> predictions{1.050};
  const std::vector<double> references{1.000};
  return hero_audio::evaluate_onsets(predictions, references, 0.050).matches.size() == 1;
}

struct OracleResult {
  std::size_t match_count{};
  double total_error{std::numeric_limits<double>::infinity()};
};

OracleResult exhaustive_oracle(const std::vector<double> &predictions,
                               const std::vector<double> &references, double tolerance_seconds) {
  OracleResult best;
  std::vector<bool> reference_used(references.size(), false);
  std::function<void(std::size_t, std::size_t, double)> visit;
  visit = [&](std::size_t prediction_index, std::size_t match_count, double total_error) {
    if (prediction_index == predictions.size()) {
      if (match_count > best.match_count ||
          (match_count == best.match_count && total_error < best.total_error)) {
        best = OracleResult{.match_count = match_count, .total_error = total_error};
      }
      return;
    }

    visit(prediction_index + 1, match_count, total_error);
    for (std::size_t reference_index = 0; reference_index < references.size(); ++reference_index) {
      const double error = std::abs(predictions[prediction_index] - references[reference_index]);
      if (!reference_used[reference_index] && error <= tolerance_seconds) {
        reference_used[reference_index] = true;
        visit(prediction_index + 1, match_count + 1, total_error + error);
        reference_used[reference_index] = false;
      }
    }
  };
  visit(0, 0, 0.0);
  return best;
}

bool test_dynamic_programming_matches_exhaustive_oracle() {
  const std::vector<double> prediction_pool{0.011, 0.041, 0.091, 0.141};
  const std::vector<double> reference_pool{0.000, 0.050, 0.100, 0.150};
  constexpr double tolerance = 0.051;

  for (std::uint32_t prediction_mask = 0; prediction_mask < 16; ++prediction_mask) {
    std::vector<double> predictions;
    for (std::size_t index = 0; index < prediction_pool.size(); ++index) {
      if ((prediction_mask & (1U << index)) != 0U) {
        predictions.push_back(prediction_pool[index]);
      }
    }
    for (std::uint32_t reference_mask = 0; reference_mask < 16; ++reference_mask) {
      std::vector<double> references;
      for (std::size_t index = 0; index < reference_pool.size(); ++index) {
        if ((reference_mask & (1U << index)) != 0U) {
          references.push_back(reference_pool[index]);
        }
      }

      const auto expected = exhaustive_oracle(predictions, references, tolerance);
      const auto actual = hero_audio::evaluate_onsets(predictions, references, tolerance);
      double actual_total_error = 0.0;
      for (const auto &match : actual.matches) {
        actual_total_error += match.absolute_error_seconds;
      }
      if (actual.matches.size() != expected.match_count ||
          !near(actual_total_error, expected.total_error)) {
        return false;
      }
    }
  }
  return true;
}

bool test_zero_denominators_return_zero() {
  const std::vector<double> empty;
  const auto result = hero_audio::evaluate_onsets(empty, empty);
  return result.matches.empty() && near(result.metrics.precision, 0.0) &&
         near(result.metrics.recall, 0.0) && near(result.metrics.f1, 0.0);
}

bool test_csv_reader() {
  std::istringstream predictions(
      "frame_index,onset_time_seconds,emitted_at_seconds\n3,0.125,0.141\n7,0.500,0.516\n");
  const auto values = hero_audio::read_onset_times_csv(predictions);
  std::istringstream references("0.125\n\n0.500\n");
  const auto plain_values = hero_audio::read_onset_times_csv(references);
  return values.size() == 2 && plain_values.size() == 2 && near(values[0], 0.125) &&
         near(values[1], 0.500) && values == plain_values;
}

bool test_rejects_invalid_data() {
  try {
    const std::vector<double> invalid{std::numeric_limits<double>::quiet_NaN()};
    const std::vector<double> empty;
    static_cast<void>(hero_audio::evaluate_onsets(invalid, empty));
    return false;
  } catch (const std::invalid_argument &) {
  }
  try {
    std::istringstream malformed("onset_time_seconds\nnot-a-number\n");
    static_cast<void>(hero_audio::read_onset_times_csv(malformed));
  } catch (const std::runtime_error &) {
    return true;
  }
  return false;
}

bool test_serialisation() {
  const std::vector<double> predictions{1.010};
  const std::vector<double> references{1.000};
  const auto result = hero_audio::evaluate_onsets(predictions, references);
  std::ostringstream matches;
  hero_audio::write_onset_matches_csv(matches, result.matches);
  const std::string expected_matches =
      "prediction_index,reference_index,prediction_seconds,reference_seconds,"
      "absolute_error_ms\n"
      "0,0,1.010000000,1.000000000,10.000000000\n";
  if (matches.str() != expected_matches) {
    return false;
  }

  std::ostringstream metrics;
  hero_audio::write_evaluation_json(metrics, result);
  return metrics.str().find("\"true_positives\": 1") != std::string::npos &&
         metrics.str().find("\"f1\": 1.000000000") != std::string::npos;
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, bool (*)()>> tests{
      {"maximum cardinality", test_maximises_cardinality_before_error},
      {"minimum error", test_minimises_error_after_cardinality},
      {"duplicate predictions", test_duplicate_predictions_count_as_false_positives},
      {"unsorted inputs", test_unsorted_inputs_preserve_original_indices},
      {"inclusive tolerance", test_tolerance_boundary_is_inclusive},
      {"exhaustive oracle", test_dynamic_programming_matches_exhaustive_oracle},
      {"zero denominators", test_zero_denominators_return_zero},
      {"CSV reader", test_csv_reader},
      {"invalid data", test_rejects_invalid_data},
      {"serialisation", test_serialisation},
  };
  for (const auto &[name, test] : tests) {
    if (!test()) {
      std::cerr << "Onset evaluation test failed: " << name << '\n';
      return 1;
    }
  }
  std::cout << "Onset evaluation tests passed\n";
  return 0;
}

#include "hero_audio/anomaly_evaluation.hpp"

#include "hero_audio/offline_benchmark.hpp"
#include "hero_audio/wav_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace hero_audio {
namespace {

constexpr std::string_view kImpactClass = "anomaly_impact";
constexpr std::string_view kBurstClass = "anomaly_burst";
constexpr std::string_view kNormalBackgroundClass = "normal_background";
constexpr std::string_view kNormalTransitionClass = "normal_transition";

struct IndexedPrediction {
  const PredictedAnomalyEvent *event{};
};

struct IndexedReference {
  const AnomalyReferenceLabel *label{};
};

enum class Choice : std::uint8_t { None, SkipPrediction, SkipReference, Match };

struct Cell {
  std::size_t match_count{};
  long double total_error{};
  Choice choice{Choice::None};
};

struct ManifestRow {
  std::string session_id;
  DatasetSplit split{DatasetSplit::Development};
  std::filesystem::path audio_path;
  std::filesystem::path labels_path;
  std::filesystem::path frames_path;
  std::filesystem::path events_path;
  std::filesystem::path eligibility_summary_path;
};

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

[[nodiscard]] std::vector<std::string_view> split_csv(std::string_view row) {
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

[[nodiscard]] double parse_number(std::string_view text, std::string_view description) {
  double value = 0.0;
  std::istringstream parser{std::string(trim(text))};
  parser.imbue(std::locale::classic());
  parser >> std::noskipws >> value;
  if (!parser || !parser.eof() || !std::isfinite(value)) {
    throw std::runtime_error("Invalid " + std::string(description));
  }
  return value;
}

[[nodiscard]] std::map<std::string, std::size_t>
header_indices(std::string_view line, std::span<const std::string_view> required) {
  const auto fields = split_csv(line);
  std::map<std::string, std::size_t> result;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const std::string name(fields[index]);
    if (name.empty() || !result.emplace(name, index).second) {
      throw std::runtime_error("CSV header contains an empty or duplicate column");
    }
  }
  for (const auto name : required) {
    if (!result.contains(std::string(name))) {
      throw std::runtime_error("CSV header has no '" + std::string(name) + "' column");
    }
  }
  return result;
}

[[nodiscard]] std::string field_at(
    std::span<const std::string_view> fields,
    const std::map<std::string, std::size_t> &columns, std::string_view name,
    std::size_t line_number) {
  const auto index = columns.at(std::string(name));
  if (index >= fields.size()) {
    throw std::runtime_error("CSV row " + std::to_string(line_number) +
                             " has too few columns");
  }
  return std::string(fields[index]);
}

[[nodiscard]] bool is_anomaly_class(std::string_view class_name) noexcept {
  return class_name == kImpactClass || class_name == kBurstClass;
}

[[nodiscard]] bool is_allowed_class(std::string_view class_name) noexcept {
  return is_anomaly_class(class_name) || class_name == kNormalBackgroundClass ||
         class_name == kNormalTransitionClass;
}

[[nodiscard]] double interval_error(double point,
                                    const AnomalyReferenceLabel &label) noexcept {
  if (point < label.start_seconds) {
    return label.start_seconds - point;
  }
  if (point > label.end_seconds) {
    return point - label.end_seconds;
  }
  return 0.0;
}

[[nodiscard]] bool within(double value, double limit, double scale) noexcept {
  const double margin = 8.0 * std::numeric_limits<double>::epsilon() *
                        std::max(1.0, scale);
  return value <= limit + margin;
}

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

[[nodiscard]] bool better(const Cell &candidate, const Cell &current) noexcept {
  if (candidate.match_count != current.match_count) {
    return candidate.match_count > current.match_count;
  }
  if (candidate.total_error != current.total_error) {
    return candidate.total_error < current.total_error;
  }
  return choice_priority(candidate.choice) > choice_priority(current.choice);
}

[[nodiscard]] std::optional<double> percentile_or_none(
    std::span<const double> values, double probability) {
  if (values.empty()) {
    return std::nullopt;
  }
  return linear_percentile(values, probability);
}

[[nodiscard]] AnomalyMetrics make_metrics(
    std::size_t predictions, std::size_t references, std::size_t true_positives,
    std::size_t normal_transition_false_positives, double monitoring_seconds,
    std::span<const double> delays_ms) {
  const auto false_positives = predictions - true_positives;
  const auto false_negatives = references - true_positives;
  const double precision = predictions == 0
                               ? 0.0
                               : static_cast<double>(true_positives) /
                                     static_cast<double>(predictions);
  const double recall = references == 0
                            ? 0.0
                            : static_cast<double>(true_positives) /
                                  static_cast<double>(references);
  const double f1 = precision + recall == 0.0
                        ? 0.0
                        : 2.0 * precision * recall / (precision + recall);
  const double false_alarms_per_hour =
      monitoring_seconds == 0.0
          ? 0.0
          : static_cast<double>(false_positives) * 3600.0 / monitoring_seconds;
  return AnomalyMetrics{
      .prediction_count = predictions,
      .reference_count = references,
      .true_positives = true_positives,
      .false_positives = false_positives,
      .false_negatives = false_negatives,
      .false_positives_during_normal_transition =
          normal_transition_false_positives,
      .precision = precision,
      .recall = recall,
      .f1 = f1,
      .monitoring_seconds = monitoring_seconds,
      .false_alarms_per_hour = false_alarms_per_hour,
      .p50_detection_delay_ms = percentile_or_none(delays_ms, 0.50),
      .p95_detection_delay_ms = percentile_or_none(delays_ms, 0.95),
  };
}

[[nodiscard]] bool point_in_label(double point, const AnomalyReferenceLabel &label,
                                  double tolerance) noexcept {
  return point >= label.start_seconds - tolerance &&
         point <= label.end_seconds + tolerance;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Unable to open JSON summary: " + path.string());
  }
  std::ostringstream content;
  content << input.rdbuf();
  if (!input.eof() && input.fail()) {
    throw std::runtime_error("Unable to read JSON summary: " + path.string());
  }
  return content.str();
}

[[nodiscard]] std::size_t find_json_value(std::string_view json,
                                          std::string_view key) {
  const std::string token = "\"" + std::string(key) + "\"";
  const auto key_position = json.find(token);
  if (key_position == std::string_view::npos ||
      json.find(token, key_position + token.size()) != std::string_view::npos) {
    throw std::runtime_error("JSON summary must contain exactly one '" +
                             std::string(key) + "' field");
  }
  const auto colon = json.find(':', key_position + token.size());
  if (colon == std::string_view::npos) {
    throw std::runtime_error("Malformed JSON summary field '" + std::string(key) + "'");
  }
  const auto value = json.find_first_not_of(" \t\r\n", colon + 1);
  if (value == std::string_view::npos) {
    throw std::runtime_error("Missing JSON summary value for '" + std::string(key) + "'");
  }
  return value;
}

[[nodiscard]] bool json_bool(std::string_view json, std::string_view key) {
  const auto position = find_json_value(json, key);
  if (json.substr(position, 4) == "true") {
    return true;
  }
  if (json.substr(position, 5) == "false") {
    return false;
  }
  throw std::runtime_error("JSON field '" + std::string(key) + "' is not boolean");
}

[[nodiscard]] std::string json_string(std::string_view json, std::string_view key) {
  const auto position = find_json_value(json, key);
  if (json[position] != '"') {
    throw std::runtime_error("JSON field '" + std::string(key) + "' is not a string");
  }
  const auto end = json.find('"', position + 1);
  if (end == std::string_view::npos) {
    throw std::runtime_error("Unterminated JSON string for '" + std::string(key) + "'");
  }
  return std::string(json.substr(position + 1, end - position - 1));
}

void require_eligible_summary(const std::filesystem::path &path) {
  const auto json = read_text_file(path);
  if (!json_bool(json, "baseline_complete")) {
    throw std::runtime_error("Session baseline_complete is false: " + path.string());
  }
  if (!json_bool(json, "capture_integrity_pass")) {
    throw std::runtime_error("Session capture_integrity_pass is false: " + path.string());
  }
  if (json_string(json, "capture_integrity_source") != "live_coreaudio_stats") {
    throw std::runtime_error(
        "Eligibility summary must use live_coreaudio_stats, not a replay-only claim: " +
        path.string());
  }
}

[[nodiscard]] double read_monitoring_start(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Unable to open anomaly frame CSV: " + path.string());
  }
  std::string line;
  if (!std::getline(input, line)) {
    throw std::runtime_error("Anomaly frame CSV is empty: " + path.string());
  }
  constexpr std::array<std::string_view, 2> required{"phase", "frame_center_seconds"};
  const auto columns = header_indices(line, required);
  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (trim(line).empty()) {
      continue;
    }
    const auto fields = split_csv(line);
    if (field_at(fields, columns, "phase", line_number) == "monitoring") {
      const double start = parse_number(
          field_at(fields, columns, "frame_center_seconds", line_number),
          "monitoring frame center");
      if (start < 0.0) {
        throw std::runtime_error("Monitoring start must be non-negative");
      }
      return start;
    }
  }
  throw std::runtime_error("Anomaly frame CSV has no monitoring phase: " + path.string());
}

[[nodiscard]] std::filesystem::path resolve_path(
    const std::filesystem::path &base, std::string_view text) {
  std::filesystem::path path{std::string(text)};
  return path.is_absolute() ? path : (base / path).lexically_normal();
}

[[nodiscard]] std::vector<ManifestRow>
read_manifest(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Unable to open anomaly dataset manifest: " + path.string());
  }
  std::string line;
  if (!std::getline(input, line)) {
    throw std::runtime_error("Anomaly dataset manifest is empty");
  }
  constexpr std::array<std::string_view, 7> required{
      "session_id", "split", "audio_path", "labels_path", "frames_path",
      "events_path", "eligibility_summary_path"};
  const auto columns = header_indices(line, required);
  const auto base = path.parent_path();
  std::vector<ManifestRow> result;
  std::set<std::string> session_ids;
  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (trim(line).empty()) {
      continue;
    }
    const auto fields = split_csv(line);
    const auto session_id = field_at(fields, columns, "session_id", line_number);
    if (session_id.empty() || !session_ids.insert(session_id).second) {
      throw std::runtime_error("Manifest session_id is empty or duplicated");
    }
    result.push_back(ManifestRow{
        .session_id = session_id,
        .split = parse_dataset_split(field_at(fields, columns, "split", line_number)),
        .audio_path = resolve_path(
            base, field_at(fields, columns, "audio_path", line_number)),
        .labels_path = resolve_path(
            base, field_at(fields, columns, "labels_path", line_number)),
        .frames_path = resolve_path(
            base, field_at(fields, columns, "frames_path", line_number)),
        .events_path = resolve_path(
            base, field_at(fields, columns, "events_path", line_number)),
        .eligibility_summary_path = resolve_path(
            base, field_at(fields, columns, "eligibility_summary_path", line_number)),
    });
  }
  return result;
}

void write_optional_json(std::ostream &output, const std::optional<double> &value) {
  if (value.has_value()) {
    output << *value;
  } else {
    output << "null";
  }
}

} // namespace

DatasetSplit parse_dataset_split(std::string_view text) {
  if (text == "development") {
    return DatasetSplit::Development;
  }
  if (text == "held-out") {
    return DatasetSplit::HeldOut;
  }
  throw std::invalid_argument("Dataset split must be development or held-out");
}

std::string_view to_string(DatasetSplit split) noexcept {
  return split == DatasetSplit::Development ? "development" : "held-out";
}

std::vector<PredictedAnomalyEvent>
read_predicted_anomaly_events_csv(std::istream &input) {
  std::string line;
  if (!std::getline(input, line)) {
    throw std::runtime_error("Anomaly event CSV is empty");
  }
  constexpr std::array<std::string_view, 6> required{
      "class", "onset_time_seconds", "emitted_at_audio_seconds", "delay_scope",
      "estimated_software_detection_delay_ms", "anomaly_score"};
  const auto columns = header_indices(line, required);
  std::vector<PredictedAnomalyEvent> result;
  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (trim(line).empty()) {
      continue;
    }
    const auto fields = split_csv(line);
    const auto class_name = field_at(fields, columns, "class", line_number);
    const double onset = parse_number(
        field_at(fields, columns, "onset_time_seconds", line_number),
        "predicted onset time");
    const double emitted = parse_number(
        field_at(fields, columns, "emitted_at_audio_seconds", line_number),
        "predicted emitted time");
    const double delay_ms = parse_number(
        field_at(fields, columns, "estimated_software_detection_delay_ms", line_number),
        "estimated detection delay");
    const double score = parse_number(
        field_at(fields, columns, "anomaly_score", line_number), "anomaly score");
    const auto delay_scope =
        field_at(fields, columns, "delay_scope", line_number);
    if (class_name != "unexpected_transient" || onset < 0.0 || emitted < onset ||
        delay_scope.empty() || delay_ms < 0.0 || score < 0.0) {
      throw std::runtime_error("Invalid predicted anomaly event at CSV row " +
                               std::to_string(line_number));
    }
    result.push_back(PredictedAnomalyEvent{
        .original_index = result.size(),
        .class_name = class_name,
        .onset_time_seconds = onset,
        .emitted_at_audio_seconds = emitted,
        .delay_scope = delay_scope,
        .estimated_software_detection_delay_ms = delay_ms,
        .decision_time_seconds = onset + delay_ms / 1000.0,
        .anomaly_score = score,
    });
  }
  return result;
}

std::vector<PredictedAnomalyEvent>
read_predicted_anomaly_events_csv(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Unable to open anomaly event CSV: " + path.string());
  }
  return read_predicted_anomaly_events_csv(input);
}

std::vector<AnomalyReferenceLabel> read_anomaly_labels_csv(std::istream &input) {
  std::string line;
  if (!std::getline(input, line)) {
    throw std::runtime_error("Human label CSV is empty");
  }
  constexpr std::array<std::string_view, 6> required{
      "start_seconds", "end_seconds", "class", "operating_state", "confidence",
      "annotator"};
  const auto columns = header_indices(line, required);
  std::vector<AnomalyReferenceLabel> result;
  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (trim(line).empty()) {
      continue;
    }
    const auto fields = split_csv(line);
    const double start = parse_number(
        field_at(fields, columns, "start_seconds", line_number), "label start time");
    const double end = parse_number(
        field_at(fields, columns, "end_seconds", line_number), "label end time");
    const auto class_name = field_at(fields, columns, "class", line_number);
    const auto state = field_at(fields, columns, "operating_state", line_number);
    const auto confidence = field_at(fields, columns, "confidence", line_number);
    const auto annotator = field_at(fields, columns, "annotator", line_number);
    if (start < 0.0 || end < start || !is_allowed_class(class_name) ||
        state != "steady" ||
        (confidence != "high" && confidence != "medium" && confidence != "low") ||
        annotator.empty()) {
      throw std::runtime_error("Invalid human anomaly label at CSV row " +
                               std::to_string(line_number));
    }
    result.push_back(AnomalyReferenceLabel{
        .original_index = result.size(),
        .start_seconds = start,
        .end_seconds = end,
        .class_name = class_name,
        .operating_state = state,
        .confidence = confidence,
        .annotator = annotator,
    });
  }
  return result;
}

std::vector<AnomalyReferenceLabel>
read_anomaly_labels_csv(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Unable to open human label CSV: " + path.string());
  }
  return read_anomaly_labels_csv(input);
}

AnomalySessionEvaluation evaluate_anomaly_session(
    std::string session_id, DatasetSplit split,
    std::span<const PredictedAnomalyEvent> predictions,
    std::span<const AnomalyReferenceLabel> labels,
    double monitoring_start_seconds, double monitoring_end_seconds,
    double tolerance_seconds) {
  if (session_id.empty() || !std::isfinite(monitoring_start_seconds) ||
      !std::isfinite(monitoring_end_seconds) ||
      monitoring_start_seconds < 0.0 ||
      monitoring_end_seconds <= monitoring_start_seconds ||
      !std::isfinite(tolerance_seconds) || tolerance_seconds < 0.0) {
    throw std::invalid_argument("Invalid anomaly session evaluation interval");
  }

  std::vector<IndexedPrediction> sorted_predictions;
  sorted_predictions.reserve(predictions.size());
  std::optional<std::string> detection_delay_scope;
  std::set<std::size_t> prediction_indices;
  for (const auto &prediction : predictions) {
    if (!std::isfinite(prediction.onset_time_seconds) ||
        !std::isfinite(prediction.emitted_at_audio_seconds) ||
        !std::isfinite(prediction.estimated_software_detection_delay_ms) ||
        !std::isfinite(prediction.decision_time_seconds) ||
        !std::isfinite(prediction.anomaly_score) ||
        prediction.class_name != "unexpected_transient" ||
        prediction.delay_scope.empty() ||
        prediction.onset_time_seconds < 0.0 ||
        prediction.emitted_at_audio_seconds < prediction.onset_time_seconds ||
        prediction.estimated_software_detection_delay_ms < 0.0 ||
        prediction.anomaly_score < 0.0 ||
        !within(std::abs(prediction.decision_time_seconds -
                         (prediction.onset_time_seconds +
                          prediction.estimated_software_detection_delay_ms /
                              1000.0)),
                1.0e-9, std::abs(prediction.decision_time_seconds)) ||
        !prediction_indices.insert(prediction.original_index).second ||
        prediction.onset_time_seconds < monitoring_start_seconds ||
        prediction.onset_time_seconds > monitoring_end_seconds) {
      throw std::invalid_argument("Invalid prediction in anomaly evaluation");
    }
    if (!detection_delay_scope.has_value()) {
      detection_delay_scope = prediction.delay_scope;
    } else if (*detection_delay_scope != prediction.delay_scope) {
      throw std::invalid_argument(
          "A session may not mix different detection delay scopes");
    }
    sorted_predictions.push_back(IndexedPrediction{.event = &prediction});
  }
  std::stable_sort(sorted_predictions.begin(), sorted_predictions.end(),
                   [](const auto &left, const auto &right) {
                     if (left.event->onset_time_seconds != right.event->onset_time_seconds) {
                       return left.event->onset_time_seconds < right.event->onset_time_seconds;
                     }
                     return left.event->original_index < right.event->original_index;
                   });

  std::vector<IndexedReference> references;
  std::vector<const AnomalyReferenceLabel *> normal_transitions;
  std::set<std::size_t> label_indices;
  for (const auto &label : labels) {
    if (!std::isfinite(label.start_seconds) ||
        !std::isfinite(label.end_seconds) || label.start_seconds < 0.0 ||
        label.end_seconds < label.start_seconds ||
        !is_allowed_class(label.class_name) || label.operating_state != "steady" ||
        (label.confidence != "high" && label.confidence != "medium" &&
         label.confidence != "low") ||
        label.annotator.empty() ||
        !label_indices.insert(label.original_index).second ||
        label.end_seconds > monitoring_end_seconds) {
      throw std::invalid_argument("Invalid reference label in anomaly evaluation");
    }
    if (is_anomaly_class(label.class_name)) {
      if (label.start_seconds < monitoring_start_seconds) {
        throw std::invalid_argument("Anomaly label lies inside calibration");
      }
      references.push_back(IndexedReference{.label = &label});
    } else if (label.class_name == kNormalTransitionClass) {
      normal_transitions.push_back(&label);
    }
  }
  std::stable_sort(references.begin(), references.end(), [](const auto &left,
                                                            const auto &right) {
    if (left.label->start_seconds != right.label->start_seconds) {
      return left.label->start_seconds < right.label->start_seconds;
    }
    return left.label->original_index < right.label->original_index;
  });
  for (std::size_t index = 1; index < references.size(); ++index) {
    if (references[index].label->start_seconds <
        references[index - 1].label->end_seconds) {
      throw std::invalid_argument("Anomaly reference intervals must not overlap");
    }
  }

  const std::size_t rows = sorted_predictions.size() + 1;
  const std::size_t columns = references.size() + 1;
  if (rows > std::numeric_limits<std::size_t>::max() / columns) {
    throw std::length_error("Anomaly matching matrix dimensions overflow size_t");
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
      Cell best = cell(row - 1, column);
      best.choice = Choice::SkipPrediction;
      Cell skip_reference = cell(row, column - 1);
      skip_reference.choice = Choice::SkipReference;
      if (better(skip_reference, best)) {
        best = skip_reference;
      }
      const auto &prediction = *sorted_predictions[row - 1].event;
      const auto &reference = *references[column - 1].label;
      const double error = interval_error(prediction.onset_time_seconds, reference);
      const double scale = std::max({std::abs(prediction.onset_time_seconds),
                                     std::abs(reference.start_seconds),
                                     std::abs(reference.end_seconds), tolerance_seconds});
      if (within(error, tolerance_seconds, scale)) {
        Cell match = cell(row - 1, column - 1);
        ++match.match_count;
        match.total_error += static_cast<long double>(error);
        match.choice = Choice::Match;
        if (better(match, best)) {
          best = match;
        }
      }
      cell(row, column) = best;
    }
  }

  std::vector<AnomalyEventMatch> matches;
  std::set<std::size_t> matched_prediction_indices;
  std::size_t row = rows - 1;
  std::size_t column = columns - 1;
  while (row > 0 || column > 0) {
    switch (cell(row, column).choice) {
    case Choice::Match: {
      const auto &prediction = *sorted_predictions[row - 1].event;
      const auto &reference = *references[column - 1].label;
      matches.push_back(AnomalyEventMatch{
          .prediction_index = prediction.original_index,
          .reference_index = reference.original_index,
          .prediction_seconds = prediction.onset_time_seconds,
          .reference_start_seconds = reference.start_seconds,
          .reference_end_seconds = reference.end_seconds,
          .reference_class = reference.class_name,
          .interval_error_seconds =
              interval_error(prediction.onset_time_seconds, reference),
          .decision_time_seconds = prediction.decision_time_seconds,
          .detection_delay_seconds =
              prediction.decision_time_seconds - reference.start_seconds,
      });
      matched_prediction_indices.insert(prediction.original_index);
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
      throw std::logic_error("Invalid anomaly matching backtracking state");
    }
  }
  std::reverse(matches.begin(), matches.end());

  std::size_t transition_false_positives = 0;
  for (const auto &prediction : predictions) {
    if (matched_prediction_indices.contains(prediction.original_index)) {
      continue;
    }
    if (std::any_of(normal_transitions.begin(), normal_transitions.end(),
                    [&](const auto *label) {
                      return point_in_label(prediction.onset_time_seconds, *label,
                                            tolerance_seconds);
                    })) {
      ++transition_false_positives;
    }
  }

  std::vector<double> delays_ms;
  delays_ms.reserve(matches.size());
  std::map<std::string, std::pair<std::size_t, std::size_t>> class_counts{
      {std::string(kImpactClass), {0, 0}},
      {std::string(kBurstClass), {0, 0}},
  };
  for (const auto &reference : references) {
    ++class_counts[reference.label->class_name].first;
  }
  for (const auto &match : matches) {
    ++class_counts[match.reference_class].second;
    delays_ms.push_back(match.detection_delay_seconds * 1000.0);
  }
  std::vector<AnomalyClassMetrics> per_class;
  for (const auto &[class_name, counts] : class_counts) {
    per_class.push_back(AnomalyClassMetrics{
        .class_name = class_name,
        .reference_count = counts.first,
        .true_positives = counts.second,
        .recall = counts.first == 0
                      ? 0.0
                      : static_cast<double>(counts.second) /
                            static_cast<double>(counts.first),
    });
  }
  const double monitoring_seconds = monitoring_end_seconds - monitoring_start_seconds;
  return AnomalySessionEvaluation{
      .session_id = std::move(session_id),
      .split = split,
      .tolerance_seconds = tolerance_seconds,
      .monitoring_start_seconds = monitoring_start_seconds,
      .monitoring_end_seconds = monitoring_end_seconds,
      .detection_delay_scope = std::move(detection_delay_scope),
      .matches = std::move(matches),
      .per_class = std::move(per_class),
      .metrics = make_metrics(predictions.size(), references.size(),
                              cell(rows - 1, columns - 1).match_count,
                              transition_false_positives, monitoring_seconds,
                              delays_ms),
  };
}

AnomalyDatasetEvaluation evaluate_anomaly_manifest(
    const std::filesystem::path &manifest_path, DatasetSplit requested_split,
    double tolerance_seconds) {
  const auto rows = read_manifest(manifest_path);
  AnomalyDatasetEvaluation result{
      .split = requested_split,
      .tolerance_seconds = tolerance_seconds,
  };
  std::vector<double> pooled_delays_ms;
  std::map<std::string, std::pair<std::size_t, std::size_t>> class_counts{
      {std::string(kImpactClass), {0, 0}},
      {std::string(kBurstClass), {0, 0}},
  };
  std::size_t predictions = 0;
  std::size_t references = 0;
  std::size_t true_positives = 0;
  std::size_t transition_false_positives = 0;
  double monitoring_seconds = 0.0;

  for (const auto &row : rows) {
    if (row.split != requested_split) {
      continue;
    }
    require_eligible_summary(row.eligibility_summary_path);
    const auto audio = read_wav(row.audio_path);
    if (audio.source_channels != 1 ||
        audio.source_encoding != WavSampleEncoding::IeeeFloat ||
        audio.source_bits_per_sample != 32) {
      throw std::runtime_error("Evaluation requires mono float32 analysis audio: " +
                               row.audio_path.string());
    }
    const double monitoring_start = read_monitoring_start(row.frames_path);
    const double monitoring_end = audio.duration_seconds();
    const auto predicted_events =
        read_predicted_anomaly_events_csv(row.events_path);
    const auto human_labels = read_anomaly_labels_csv(row.labels_path);
    const auto session = evaluate_anomaly_session(
        row.session_id, row.split, predicted_events, human_labels,
        monitoring_start, monitoring_end, tolerance_seconds);

    if (session.detection_delay_scope.has_value()) {
      if (!result.detection_delay_scope.has_value()) {
        result.detection_delay_scope = session.detection_delay_scope;
      } else if (*result.detection_delay_scope !=
                 *session.detection_delay_scope) {
        throw std::runtime_error(
            "Requested split mixes incompatible detection delay scopes");
      }
    }

    predictions += session.metrics.prediction_count;
    references += session.metrics.reference_count;
    true_positives += session.metrics.true_positives;
    transition_false_positives +=
        session.metrics.false_positives_during_normal_transition;
    monitoring_seconds += session.metrics.monitoring_seconds;
    for (const auto &metrics : session.per_class) {
      class_counts[metrics.class_name].first += metrics.reference_count;
      class_counts[metrics.class_name].second += metrics.true_positives;
    }
    for (const auto &match : session.matches) {
      result.matches.push_back(match);
      result.match_session_ids.push_back(session.session_id);
      pooled_delays_ms.push_back(match.detection_delay_seconds * 1000.0);
    }
    result.sessions.push_back(session);
  }
  if (result.sessions.empty()) {
    throw std::runtime_error("Manifest contains no sessions for requested split '" +
                             std::string(to_string(requested_split)) + "'");
  }
  for (const auto &[class_name, counts] : class_counts) {
    result.per_class.push_back(AnomalyClassMetrics{
        .class_name = class_name,
        .reference_count = counts.first,
        .true_positives = counts.second,
        .recall = counts.first == 0
                      ? 0.0
                      : static_cast<double>(counts.second) /
                            static_cast<double>(counts.first),
    });
  }
  result.metrics = make_metrics(predictions, references, true_positives,
                                transition_false_positives, monitoring_seconds,
                                pooled_delays_ms);
  return result;
}

void write_anomaly_matches_csv(std::ostream &output,
                               const AnomalyDatasetEvaluation &evaluation) {
  output << "session_id,prediction_index,reference_index,prediction_seconds,"
            "reference_start_seconds,reference_end_seconds,reference_class,"
            "interval_error_ms,decision_time_seconds,detection_delay_ms\n"
         << std::fixed << std::setprecision(9);
  for (std::size_t index = 0; index < evaluation.matches.size(); ++index) {
    const auto &match = evaluation.matches[index];
    output << evaluation.match_session_ids[index] << ',' << match.prediction_index << ','
           << match.reference_index << ',' << match.prediction_seconds << ','
           << match.reference_start_seconds << ',' << match.reference_end_seconds << ','
           << match.reference_class << ',' << match.interval_error_seconds * 1000.0 << ','
           << match.decision_time_seconds << ',' << match.detection_delay_seconds * 1000.0
           << '\n';
  }
  if (!output) {
    throw std::runtime_error("Unable to write anomaly match CSV");
  }
}

void write_anomaly_matches_csv(const std::filesystem::path &path,
                               const AnomalyDatasetEvaluation &evaluation) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open anomaly match CSV: " + path.string());
  }
  write_anomaly_matches_csv(output, evaluation);
}

void write_anomaly_session_metrics_csv(
    std::ostream &output, const AnomalyDatasetEvaluation &evaluation) {
  output << "session_id,split,monitoring_start_seconds,monitoring_end_seconds,"
            "monitoring_seconds,predictions,references,tp,fp,fn,precision,recall,f1,"
            "false_alarms_per_hour,false_positives_during_normal_transition,"
            "detection_delay_scope,p50_detection_delay_ms,p95_detection_delay_ms\n"
         << std::fixed << std::setprecision(9);
  for (const auto &session : evaluation.sessions) {
    const auto &metrics = session.metrics;
    output << session.session_id << ',' << to_string(session.split) << ','
           << session.monitoring_start_seconds << ',' << session.monitoring_end_seconds << ','
           << metrics.monitoring_seconds << ',' << metrics.prediction_count << ','
           << metrics.reference_count << ',' << metrics.true_positives << ','
           << metrics.false_positives << ',' << metrics.false_negatives << ','
           << metrics.precision << ',' << metrics.recall << ',' << metrics.f1 << ','
           << metrics.false_alarms_per_hour << ','
           << metrics.false_positives_during_normal_transition << ',';
    if (session.detection_delay_scope.has_value()) {
      output << *session.detection_delay_scope;
    }
    output << ',';
    if (metrics.p50_detection_delay_ms.has_value()) {
      output << *metrics.p50_detection_delay_ms;
    }
    output << ',';
    if (metrics.p95_detection_delay_ms.has_value()) {
      output << *metrics.p95_detection_delay_ms;
    }
    output << '\n';
  }
  if (!output) {
    throw std::runtime_error("Unable to write anomaly session metrics CSV");
  }
}

void write_anomaly_session_metrics_csv(
    const std::filesystem::path &path,
    const AnomalyDatasetEvaluation &evaluation) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open anomaly session metrics CSV: " + path.string());
  }
  write_anomaly_session_metrics_csv(output, evaluation);
}

void write_anomaly_dataset_metrics_json(
    std::ostream &output, const AnomalyDatasetEvaluation &evaluation) {
  const auto &metrics = evaluation.metrics;
  output << std::fixed << std::setprecision(9) << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"split\": \"" << to_string(evaluation.split) << "\",\n"
         << "  \"session_count\": " << evaluation.sessions.size() << ",\n"
         << "  \"matching_rule\": \"max_cardinality_then_min_total_interval_error\",\n"
         << "  \"tolerance_ms\": " << evaluation.tolerance_seconds * 1000.0 << ",\n"
         << "  \"monitoring_seconds\": " << metrics.monitoring_seconds << ",\n"
         << "  \"prediction_count\": " << metrics.prediction_count << ",\n"
         << "  \"reference_count\": " << metrics.reference_count << ",\n"
         << "  \"true_positives\": " << metrics.true_positives << ",\n"
         << "  \"false_positives\": " << metrics.false_positives << ",\n"
         << "  \"false_negatives\": " << metrics.false_negatives << ",\n"
         << "  \"precision\": " << metrics.precision << ",\n"
         << "  \"recall\": " << metrics.recall << ",\n"
         << "  \"f1\": " << metrics.f1 << ",\n"
         << "  \"false_alarms_per_hour\": " << metrics.false_alarms_per_hour << ",\n"
         << "  \"false_positives_during_normal_transition\": "
         << metrics.false_positives_during_normal_transition << ",\n"
         << "  \"detection_delay_scope\": ";
  if (evaluation.detection_delay_scope.has_value()) {
    output << '"' << *evaluation.detection_delay_scope << '"';
  } else {
    output << "null";
  }
  output << ",\n"
         << "  \"p50_detection_delay_ms\": ";
  write_optional_json(output, metrics.p50_detection_delay_ms);
  output << ",\n  \"p95_detection_delay_ms\": ";
  write_optional_json(output, metrics.p95_detection_delay_ms);
  output << ",\n  \"per_class\": [\n";
  for (std::size_t index = 0; index < evaluation.per_class.size(); ++index) {
    const auto &class_metrics = evaluation.per_class[index];
    output << "    {\"class\": \"" << class_metrics.class_name
           << "\", \"references\": " << class_metrics.reference_count
           << ", \"true_positives\": " << class_metrics.true_positives
           << ", \"recall\": " << class_metrics.recall << '}';
    output << (index + 1 == evaluation.per_class.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  if (!output) {
    throw std::runtime_error("Unable to write anomaly dataset metrics JSON");
  }
}

void write_anomaly_dataset_metrics_json(
    const std::filesystem::path &path,
    const AnomalyDatasetEvaluation &evaluation) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open anomaly dataset metrics JSON: " + path.string());
  }
  write_anomaly_dataset_metrics_json(output, evaluation);
}

} // namespace hero_audio

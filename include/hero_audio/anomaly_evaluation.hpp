#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hero_audio {

enum class DatasetSplit { Development, HeldOut };

[[nodiscard]] DatasetSplit parse_dataset_split(std::string_view text);
[[nodiscard]] std::string_view to_string(DatasetSplit split) noexcept;

struct PredictedAnomalyEvent {
  std::size_t original_index{};
  std::string class_name;
  double onset_time_seconds{};
  double emitted_at_audio_seconds{};
  std::string delay_scope;
  double estimated_software_detection_delay_ms{};
  double decision_time_seconds{};
  double anomaly_score{};
};

struct AnomalyReferenceLabel {
  std::size_t original_index{};
  double start_seconds{};
  double end_seconds{};
  std::string class_name;
  std::string operating_state;
  std::string confidence;
  std::string annotator;
};

struct AnomalyEventMatch {
  std::size_t prediction_index{};
  std::size_t reference_index{};
  double prediction_seconds{};
  double reference_start_seconds{};
  double reference_end_seconds{};
  std::string reference_class;
  double interval_error_seconds{};
  double decision_time_seconds{};
  double detection_delay_seconds{};
};

struct AnomalyClassMetrics {
  std::string class_name;
  std::size_t reference_count{};
  std::size_t true_positives{};
  double recall{};
};

struct AnomalyMetrics {
  std::size_t prediction_count{};
  std::size_t reference_count{};
  std::size_t true_positives{};
  std::size_t false_positives{};
  std::size_t false_negatives{};
  std::size_t false_positives_during_normal_transition{};
  double precision{};
  double recall{};
  double f1{};
  double monitoring_seconds{};
  double false_alarms_per_hour{};
  std::optional<double> p50_detection_delay_ms;
  std::optional<double> p95_detection_delay_ms;
};

struct AnomalySessionEvaluation {
  std::string session_id;
  DatasetSplit split{DatasetSplit::Development};
  double tolerance_seconds{};
  double monitoring_start_seconds{};
  double monitoring_end_seconds{};
  std::optional<std::string> detection_delay_scope;
  std::vector<AnomalyEventMatch> matches;
  std::vector<AnomalyClassMetrics> per_class;
  AnomalyMetrics metrics;
};

struct AnomalyDatasetEvaluation {
  DatasetSplit split{DatasetSplit::Development};
  double tolerance_seconds{};
  std::optional<std::string> detection_delay_scope;
  std::vector<AnomalySessionEvaluation> sessions;
  std::vector<AnomalyEventMatch> matches;
  std::vector<std::string> match_session_ids;
  std::vector<AnomalyClassMetrics> per_class;
  AnomalyMetrics metrics;
};

[[nodiscard]] std::vector<PredictedAnomalyEvent>
read_predicted_anomaly_events_csv(std::istream &input);
[[nodiscard]] std::vector<PredictedAnomalyEvent>
read_predicted_anomaly_events_csv(const std::filesystem::path &path);
[[nodiscard]] std::vector<AnomalyReferenceLabel>
read_anomaly_labels_csv(std::istream &input);
[[nodiscard]] std::vector<AnomalyReferenceLabel>
read_anomaly_labels_csv(const std::filesystem::path &path);

// Predictions are time points. An anomaly reference is a labelled interval.
// A candidate pair exists when the point lies inside the interval or within
// tolerance of a boundary. Matching maximises cardinality, then minimises total
// distance to the intervals. Each prediction and reference is used at most once.
[[nodiscard]] AnomalySessionEvaluation evaluate_anomaly_session(
    std::string session_id, DatasetSplit split,
    std::span<const PredictedAnomalyEvent> predictions,
    std::span<const AnomalyReferenceLabel> labels,
    double monitoring_start_seconds, double monitoring_end_seconds,
    double tolerance_seconds = 0.050);

// Manifest columns: session_id,split,audio_path,labels_path,frames_path,
// events_path,eligibility_summary_path. Relative paths resolve from the
// manifest directory. Only the requested split is loaded and aggregated.
[[nodiscard]] AnomalyDatasetEvaluation evaluate_anomaly_manifest(
    const std::filesystem::path &manifest_path, DatasetSplit requested_split,
    double tolerance_seconds = 0.050);

void write_anomaly_matches_csv(std::ostream &output,
                               const AnomalyDatasetEvaluation &evaluation);
void write_anomaly_matches_csv(const std::filesystem::path &path,
                               const AnomalyDatasetEvaluation &evaluation);
void write_anomaly_session_metrics_csv(std::ostream &output,
                                       const AnomalyDatasetEvaluation &evaluation);
void write_anomaly_session_metrics_csv(const std::filesystem::path &path,
                                       const AnomalyDatasetEvaluation &evaluation);
void write_anomaly_dataset_metrics_json(std::ostream &output,
                                        const AnomalyDatasetEvaluation &evaluation);
void write_anomaly_dataset_metrics_json(const std::filesystem::path &path,
                                        const AnomalyDatasetEvaluation &evaluation);

} // namespace hero_audio

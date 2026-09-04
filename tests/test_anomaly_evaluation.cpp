#include "hero_audio/anomaly_evaluation.hpp"
#include "hero_audio/wav_writer.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] bool near(double actual, double expected,
                        double tolerance = 1.0e-9) {
  return std::abs(actual - expected) <= tolerance;
}

[[nodiscard]] hero_audio::PredictedAnomalyEvent prediction(
    std::size_t index, double onset, double delay_ms) {
  return hero_audio::PredictedAnomalyEvent{
      .original_index = index,
      .class_name = "unexpected_transient",
      .onset_time_seconds = onset,
      .emitted_at_audio_seconds = onset + delay_ms / 1000.0,
      .delay_scope = "algorithm_only_replay",
      .estimated_software_detection_delay_ms = delay_ms,
      .decision_time_seconds = onset + delay_ms / 1000.0,
      .anomaly_score = 1.5,
  };
}

[[nodiscard]] hero_audio::AnomalyReferenceLabel label(
    std::size_t index, double start, double end, std::string class_name) {
  return hero_audio::AnomalyReferenceLabel{
      .original_index = index,
      .start_seconds = start,
      .end_seconds = end,
      .class_name = std::move(class_name),
      .operating_state = "steady",
      .confidence = "high",
      .annotator = "independent-annotator-a",
  };
}

bool test_optimal_one_to_one_matching_and_metrics() {
  const std::vector<hero_audio::PredictedAnomalyEvent> predictions{
      prediction(0, 0.980, 40.0), prediction(1, 1.020, 20.0),
      prediction(2, 2.150, 10.0), prediction(3, 3.050, 5.0)};
  const std::vector<hero_audio::AnomalyReferenceLabel> labels{
      label(0, 1.000, 1.100, "anomaly_impact"),
      label(1, 2.000, 2.100, "anomaly_burst"),
      label(2, 3.000, 3.100, "normal_transition")};

  const auto result = hero_audio::evaluate_anomaly_session(
      "session-a", hero_audio::DatasetSplit::Development, predictions, labels,
      0.500, 4.000, 0.050);
  const auto &metrics = result.metrics;
  return result.matches.size() == 2 &&
         result.matches[0].prediction_index == 1 &&
         result.matches[0].reference_index == 0 &&
         near(result.matches[0].interval_error_seconds, 0.0) &&
         result.matches[1].prediction_index == 2 &&
         result.matches[1].reference_index == 1 &&
         near(result.matches[1].interval_error_seconds, 0.050) &&
         metrics.prediction_count == 4 && metrics.reference_count == 2 &&
         metrics.true_positives == 2 && metrics.false_positives == 2 &&
         metrics.false_negatives == 0 &&
         metrics.false_positives_during_normal_transition == 1 &&
         result.detection_delay_scope == "algorithm_only_replay" &&
         near(metrics.precision, 0.5) && near(metrics.recall, 1.0) &&
         near(metrics.f1, 2.0 / 3.0) &&
         near(metrics.false_alarms_per_hour, 2.0 * 3600.0 / 3.5) &&
         metrics.p50_detection_delay_ms.has_value() &&
         metrics.p95_detection_delay_ms.has_value() &&
         near(*metrics.p50_detection_delay_ms, 100.0) &&
         near(*metrics.p95_detection_delay_ms, 154.0);
}

bool test_csv_parsers_and_validation() {
  std::istringstream events(
      "class,onset_time_seconds,emitted_at_audio_seconds,delay_scope,"
      "estimated_software_detection_delay_ms,anomaly_score\n"
      "unexpected_transient,1.25,1.27,algorithm_only_replay,20,2.5\n");
  std::istringstream labels(
      "start_seconds,end_seconds,class,operating_state,confidence,annotator\n"
      "1.2,1.3,anomaly_impact,steady,high,annotator-a\n"
      "2.0,2.5,normal_background,steady,medium,annotator-a\n");
  const auto parsed_events =
      hero_audio::read_predicted_anomaly_events_csv(events);
  const auto parsed_labels = hero_audio::read_anomaly_labels_csv(labels);
  if (parsed_events.size() != 1 || parsed_labels.size() != 2 ||
      !near(parsed_events[0].decision_time_seconds, 1.27) ||
      parsed_labels[0].class_name != "anomaly_impact") {
    return false;
  }

  try {
    std::istringstream invalid_labels(
        "start_seconds,end_seconds,class,operating_state,confidence,annotator\n"
        "1,2,anomaly_impact,startup,high,annotator-a\n");
    static_cast<void>(hero_audio::read_anomaly_labels_csv(invalid_labels));
    return false;
  } catch (const std::runtime_error &) {
  }
  try {
    std::istringstream invalid_events(
        "class,onset_time_seconds,emitted_at_audio_seconds,delay_scope,"
        "estimated_software_detection_delay_ms,anomaly_score\n"
        "unexpected_transient,1,1.1,,100,2\n");
    static_cast<void>(
        hero_audio::read_predicted_anomaly_events_csv(invalid_events));
    return false;
  } catch (const std::runtime_error &) {
  }
  return true;
}

class LocalDirectory {
public:
  LocalDirectory()
      : path_(std::filesystem::current_path() /
              "hero_audio_anomaly_evaluation_test_workspace") {
    std::error_code error;
    const bool created = std::filesystem::create_directory(path_, error);
    if (!created || error) {
      throw std::runtime_error("Unable to create anomaly evaluation test directory");
    }
  }

  ~LocalDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_text(const std::filesystem::path &path, std::string_view content) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to create anomaly evaluation test file");
  }
  output << content;
  if (!output) {
    throw std::runtime_error("Unable to write anomaly evaluation test file");
  }
}

bool test_manifest_evaluation_and_live_eligibility_gate() {
  LocalDirectory workspace;
  const auto audio_path = workspace.path() / "analysis.wav";
  {
    hero_audio::Float32WavWriter writer(audio_path, 256);
    std::vector<float> samples(1024, 0.0F);
    writer.append(samples);
    writer.finalize();
  }
  write_text(workspace.path() / "frames.csv",
             "phase,frame_center_seconds\ncalibration,0.25\nmonitoring,0.5\n");
  write_text(
      workspace.path() / "events.csv",
      "class,onset_time_seconds,emitted_at_audio_seconds,delay_scope,"
      "estimated_software_detection_delay_ms,anomaly_score\n"
      "unexpected_transient,1.02,1.04,algorithm_only_replay,20,1.5\n");
  write_text(
      workspace.path() / "labels.csv",
      "start_seconds,end_seconds,class,operating_state,confidence,annotator\n"
      "1.0,1.1,anomaly_impact,steady,high,annotator-a\n");
  write_text(workspace.path() / "eligibility.json",
             "{\"baseline_complete\":true,\"capture_integrity_pass\":true,"
             "\"capture_integrity_source\":\"live_coreaudio_stats\"}\n");
  write_text(
      workspace.path() / "manifest.csv",
      "session_id,split,audio_path,labels_path,frames_path,events_path,"
      "eligibility_summary_path\n"
      "session-a,development,analysis.wav,labels.csv,frames.csv,events.csv,"
      "eligibility.json\n");

  const auto result = hero_audio::evaluate_anomaly_manifest(
      workspace.path() / "manifest.csv",
      hero_audio::DatasetSplit::Development, 0.050);
  if (result.sessions.size() != 1 || result.metrics.true_positives != 1 ||
      !near(result.metrics.f1, 1.0) ||
      result.detection_delay_scope != "algorithm_only_replay" ||
      !near(result.metrics.monitoring_seconds, 3.5)) {
    return false;
  }

  std::ostringstream matches;
  std::ostringstream sessions;
  std::ostringstream metrics;
  hero_audio::write_anomaly_matches_csv(matches, result);
  hero_audio::write_anomaly_session_metrics_csv(sessions, result);
  hero_audio::write_anomaly_dataset_metrics_json(metrics, result);
  if (matches.str().find("session-a,0,0") == std::string::npos ||
      sessions.str().find("session-a,development") == std::string::npos ||
      metrics.str().find("\"f1\": 1.000000000") == std::string::npos) {
    return false;
  }

  write_text(workspace.path() / "eligibility.json",
             "{\"baseline_complete\":true,\"capture_integrity_pass\":true,"
             "\"capture_integrity_source\":\"replay_input_only\"}\n");
  try {
    static_cast<void>(hero_audio::evaluate_anomaly_manifest(
        workspace.path() / "manifest.csv",
        hero_audio::DatasetSplit::Development, 0.050));
  } catch (const std::runtime_error &) {
    return true;
  }
  return false;
}

bool test_rejects_overlapping_anomaly_references() {
  const std::vector<hero_audio::PredictedAnomalyEvent> predictions;
  const std::vector<hero_audio::AnomalyReferenceLabel> labels{
      label(0, 1.0, 1.2, "anomaly_impact"),
      label(1, 1.1, 1.3, "anomaly_burst")};
  try {
    static_cast<void>(hero_audio::evaluate_anomaly_session(
        "session", hero_audio::DatasetSplit::Development, predictions, labels,
        0.5, 2.0, 0.05));
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

} // namespace

int main() {
  try {
    const std::array tests{
        std::pair{"optimal one-to-one matching and metrics",
                  &test_optimal_one_to_one_matching_and_metrics},
        std::pair{"CSV parsers and validation", &test_csv_parsers_and_validation},
        std::pair{"manifest and live eligibility gate",
                  &test_manifest_evaluation_and_live_eligibility_gate},
        std::pair{"overlapping anomaly references rejected",
                  &test_rejects_overlapping_anomaly_references},
    };
    for (const auto &[name, test] : tests) {
      if (!test()) {
        std::cerr << "FAILED: " << name << '\n';
        return 1;
      }
    }
    std::cout << "All anomaly evaluation tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAILED with exception: " << error.what() << '\n';
    return 1;
  }
}

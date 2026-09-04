#pragma once

#include "hero_audio/acoustic_features.hpp"
#include "hero_audio/causal_onset.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <vector>

namespace hero_audio {

enum class OperatingState { Idle, Startup, Steady, Shutdown };
enum class AnomalyPhase { Gated, Calibrating, Monitoring };
enum class AnomalyReason { SpectralFlux, FrameRms, PeakAbsolute };

[[nodiscard]] std::string_view to_string(OperatingState state) noexcept;
[[nodiscard]] std::string_view to_string(AnomalyPhase phase) noexcept;
[[nodiscard]] std::string_view to_string(AnomalyReason reason) noexcept;
[[nodiscard]] OperatingState parse_operating_state(std::string_view text);

struct TransientAnomalyConfig {
  std::uint32_t sample_rate_hz{48000};
  std::size_t hop_size_samples{256};
  double baseline_seconds{30.0};
  double flux_z_threshold{6.0};
  double rms_z_threshold{6.0};
  double peak_z_threshold{6.0};
  double anomaly_refractory_seconds{0.250};
  double robust_scale_relative_floor{0.01};
  double flux_scale_absolute_floor{1.0e-6};
  double rms_scale_absolute_floor{1.0e-6};
  double peak_scale_absolute_floor{1.0e-6};
  double zero_crossing_scale_absolute_floor{1.0e-4};
  std::size_t retained_feature_frames{8};
};

struct RobustFeatureStats {
  double median{};
  double median_absolute_deviation{};
  double scale{};
};

struct TransientNormalBaseline {
  RobustFeatureStats spectral_flux;
  RobustFeatureStats frame_rms;
  RobustFeatureStats peak_absolute;
  RobustFeatureStats zero_crossing_rate;
};

struct AcousticFeatureZScores {
  double spectral_flux{};
  double frame_rms{};
  double peak_absolute{};
  double zero_crossing_rate{};
};

// Every eligible onset receives a score. emitted_anomaly becomes true only
// when the score crosses 1.0 and is outside the anomaly refractory interval.
struct TransientEventScore {
  std::size_t frame_index{};
  double onset_time_seconds{};
  double emitted_at_seconds{};
  double score{};
  AnomalyReason primary_reason{AnomalyReason::SpectralFlux};
  bool above_threshold{};
  bool suppressed_by_refractory{};
  bool emitted_anomaly{};
  AcousticFeatureFrame features;
  AcousticFeatureZScores z_scores;
};

struct TransientAnomalyFrameResult {
  AnomalyPhase phase{AnomalyPhase::Gated};
  std::size_t baseline_frames_collected{};
  std::size_t baseline_frames_required{};
  double baseline_progress{};
  std::optional<AcousticFeatureZScores> current_z_scores;
  std::optional<TransientEventScore> scored_event;
};

// A deliberately narrow one-class detector for unexpected transient acoustic
// events during an explicitly declared steady operating state. The first
// baseline_seconds of steady frames are treated as verified normal data. The
// resulting median/MAD baseline is then frozen and never updated by detections.
class TransientAnomalyDetector {
public:
  explicit TransientAnomalyDetector(TransientAnomalyConfig config = {});

  [[nodiscard]] TransientAnomalyFrameResult
  process(const AcousticFeatureFrame &features,
          const std::optional<OnsetEvent> &onset,
          OperatingState operating_state);

  // A discontinuity discards an unfinished baseline because it is no longer a
  // continuous verified-normal interval. A completed frozen baseline survives.
  // Returns true when unfinished calibration samples were discarded.
  [[nodiscard]] bool handle_discontinuity() noexcept;
  void reset() noexcept;

  [[nodiscard]] const TransientAnomalyConfig &config() const noexcept {
    return config_;
  }
  [[nodiscard]] std::size_t baseline_frames_required() const noexcept {
    return baseline_frames_required_;
  }
  [[nodiscard]] std::size_t baseline_frames_collected() const noexcept {
    return baseline_frames_collected_;
  }
  [[nodiscard]] const std::optional<TransientNormalBaseline> &baseline() const noexcept {
    return baseline_;
  }
  [[nodiscard]] std::size_t scored_event_count() const noexcept {
    return scored_event_count_;
  }
  [[nodiscard]] std::size_t anomaly_candidate_count() const noexcept {
    return anomaly_candidate_count_;
  }
  [[nodiscard]] std::size_t emitted_anomaly_count() const noexcept {
    return emitted_anomaly_count_;
  }

private:
  struct RetainedFeature {
    AcousticFeatureFrame features;
    bool monitoring_eligible{};
    std::optional<AcousticFeatureZScores> z_scores;
  };

  [[nodiscard]] AcousticFeatureZScores
  standardize(const AcousticFeatureFrame &features) const;
  void add_baseline_observation(const AcousticFeatureFrame &features);
  void finalize_baseline();
  void validate_input(const AcousticFeatureFrame &features,
                      const std::optional<OnsetEvent> &onset) const;

  TransientAnomalyConfig config_;
  std::size_t baseline_frames_required_{};
  std::size_t baseline_frames_collected_{};
  std::vector<double> baseline_flux_;
  std::vector<double> baseline_rms_;
  std::vector<double> baseline_peak_;
  std::vector<double> baseline_zero_crossing_;
  std::optional<TransientNormalBaseline> baseline_;
  std::deque<RetainedFeature> retained_features_;
  std::optional<std::size_t> last_frame_index_;
  std::optional<double> last_frame_center_seconds_;
  std::optional<double> last_available_seconds_;
  std::optional<double> last_emitted_anomaly_seconds_;
  std::size_t scored_event_count_{};
  std::size_t anomaly_candidate_count_{};
  std::size_t emitted_anomaly_count_{};
};

} // namespace hero_audio

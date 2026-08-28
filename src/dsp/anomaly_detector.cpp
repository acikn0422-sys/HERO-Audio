#include "hero_audio/anomaly_detector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace hero_audio {
namespace {

[[nodiscard]] double median(std::vector<double> values) {
  if (values.empty()) {
    throw std::logic_error("Cannot calculate a baseline from zero observations");
  }
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  if (values.size() % 2 != 0) {
    return values[middle];
  }
  return (values[middle - 1] + values[middle]) / 2.0;
}

[[nodiscard]] RobustFeatureStats make_robust_stats(
    const std::vector<double> &values, double relative_floor,
    double absolute_floor) {
  const double center = median(values);
  std::vector<double> deviations;
  deviations.reserve(values.size());
  for (const double value : values) {
    deviations.push_back(std::abs(value - center));
  }
  const double mad = median(std::move(deviations));
  const double floor = std::max(absolute_floor, std::abs(center) * relative_floor);
  return RobustFeatureStats{
      .median = center,
      .median_absolute_deviation = mad,
      .scale = std::max(1.4826 * mad, floor),
  };
}

[[nodiscard]] double positive_z(double value, const RobustFeatureStats &stats) {
  return std::max(0.0, (value - stats.median) / stats.scale);
}

void require_finite_non_negative(double value, const char *name) {
  if (!std::isfinite(value) || value < 0.0) {
    throw std::invalid_argument(std::string(name) +
                                " must be finite and non-negative");
  }
}

} // namespace

std::string_view to_string(OperatingState state) noexcept {
  switch (state) {
  case OperatingState::Idle:
    return "idle";
  case OperatingState::Startup:
    return "startup";
  case OperatingState::Steady:
    return "steady";
  case OperatingState::Shutdown:
    return "shutdown";
  }
  return "unknown";
}

std::string_view to_string(AnomalyPhase phase) noexcept {
  switch (phase) {
  case AnomalyPhase::Gated:
    return "gated";
  case AnomalyPhase::Calibrating:
    return "calibrating";
  case AnomalyPhase::Monitoring:
    return "monitoring";
  }
  return "unknown";
}

std::string_view to_string(AnomalyReason reason) noexcept {
  switch (reason) {
  case AnomalyReason::SpectralFlux:
    return "spectral_flux";
  case AnomalyReason::FrameRms:
    return "frame_rms";
  case AnomalyReason::PeakAbsolute:
    return "peak_absolute";
  }
  return "unknown";
}

OperatingState parse_operating_state(std::string_view text) {
  if (text == "idle") {
    return OperatingState::Idle;
  }
  if (text == "startup") {
    return OperatingState::Startup;
  }
  if (text == "steady") {
    return OperatingState::Steady;
  }
  if (text == "shutdown") {
    return OperatingState::Shutdown;
  }
  throw std::invalid_argument(
      "Operating state must be idle, startup, steady, or shutdown");
}

TransientAnomalyDetector::TransientAnomalyDetector(TransientAnomalyConfig config)
    : config_(config) {
  if (config_.sample_rate_hz == 0 || config_.hop_size_samples == 0) {
    throw std::invalid_argument("Anomaly sample rate and hop size must be non-zero");
  }
  require_finite_non_negative(config_.baseline_seconds, "Baseline seconds");
  if (config_.baseline_seconds == 0.0) {
    throw std::invalid_argument("Baseline seconds must be greater than zero");
  }
  require_finite_non_negative(config_.flux_z_threshold, "Flux z threshold");
  require_finite_non_negative(config_.rms_z_threshold, "RMS z threshold");
  require_finite_non_negative(config_.peak_z_threshold, "Peak z threshold");
  if (config_.flux_z_threshold == 0.0 || config_.rms_z_threshold == 0.0 ||
      config_.peak_z_threshold == 0.0) {
    throw std::invalid_argument("Anomaly z thresholds must be greater than zero");
  }
  require_finite_non_negative(config_.anomaly_refractory_seconds,
                              "Anomaly refractory seconds");
  require_finite_non_negative(config_.robust_scale_relative_floor,
                              "Robust relative scale floor");
  require_finite_non_negative(config_.flux_scale_absolute_floor,
                              "Flux absolute scale floor");
  require_finite_non_negative(config_.rms_scale_absolute_floor,
                              "RMS absolute scale floor");
  require_finite_non_negative(config_.peak_scale_absolute_floor,
                              "Peak absolute scale floor");
  require_finite_non_negative(config_.zero_crossing_scale_absolute_floor,
                              "Zero-crossing absolute scale floor");
  if (config_.retained_feature_frames < 2) {
    throw std::invalid_argument("At least two recent feature frames must be retained");
  }

  const long double exact_frames =
      static_cast<long double>(config_.baseline_seconds) *
      static_cast<long double>(config_.sample_rate_hz) /
      static_cast<long double>(config_.hop_size_samples);
  if (!std::isfinite(static_cast<double>(exact_frames)) ||
      exact_frames > static_cast<long double>(std::numeric_limits<std::size_t>::max())) {
    throw std::invalid_argument("Baseline duration produces an unsupported frame count");
  }
  baseline_frames_required_ = static_cast<std::size_t>(std::ceil(exact_frames));
  if (baseline_frames_required_ < 3) {
    throw std::invalid_argument("Baseline must contain at least three analysis frames");
  }
  baseline_flux_.reserve(baseline_frames_required_);
  baseline_rms_.reserve(baseline_frames_required_);
  baseline_peak_.reserve(baseline_frames_required_);
  baseline_zero_crossing_.reserve(baseline_frames_required_);
}

void TransientAnomalyDetector::validate_input(
    const AcousticFeatureFrame &features,
    const std::optional<OnsetEvent> &onset) const {
  if (!std::isfinite(features.frame_center_seconds) ||
      !std::isfinite(features.available_seconds) ||
      features.frame_center_seconds > features.available_seconds ||
      !std::isfinite(features.spectral_flux) || features.spectral_flux < 0.0F ||
      !std::isfinite(features.frame_rms) || features.frame_rms < 0.0 ||
      !std::isfinite(features.peak_absolute) || features.peak_absolute < 0.0 ||
      !std::isfinite(features.zero_crossing_rate) ||
      features.zero_crossing_rate < 0.0 || features.zero_crossing_rate > 1.0) {
    throw std::invalid_argument("Acoustic anomaly feature frame is invalid");
  }
  if (last_frame_index_.has_value() && features.frame_index != *last_frame_index_ + 1) {
    throw std::invalid_argument("Anomaly detector requires consecutive feature frames");
  }
  if (last_frame_center_seconds_.has_value() &&
      features.frame_center_seconds <= *last_frame_center_seconds_) {
    throw std::invalid_argument("Anomaly frame-center time must increase");
  }
  if (last_available_seconds_.has_value() &&
      features.available_seconds <= *last_available_seconds_) {
    throw std::invalid_argument("Anomaly frame-availability time must increase");
  }
  if (onset.has_value() &&
      (onset->frame_index > features.frame_index ||
       !std::isfinite(onset->onset_time_seconds) ||
       !std::isfinite(onset->emitted_at_seconds) ||
       onset->onset_time_seconds > onset->emitted_at_seconds)) {
    throw std::invalid_argument("Onset is not synchronized with anomaly feature frame");
  }
}

void TransientAnomalyDetector::add_baseline_observation(
    const AcousticFeatureFrame &features) {
  baseline_flux_.push_back(static_cast<double>(features.spectral_flux));
  baseline_rms_.push_back(features.frame_rms);
  baseline_peak_.push_back(features.peak_absolute);
  baseline_zero_crossing_.push_back(features.zero_crossing_rate);
  ++baseline_frames_collected_;
}

void TransientAnomalyDetector::finalize_baseline() {
  baseline_ = TransientNormalBaseline{
      .spectral_flux = make_robust_stats(
          baseline_flux_, config_.robust_scale_relative_floor,
          config_.flux_scale_absolute_floor),
      .frame_rms = make_robust_stats(
          baseline_rms_, config_.robust_scale_relative_floor,
          config_.rms_scale_absolute_floor),
      .peak_absolute = make_robust_stats(
          baseline_peak_, config_.robust_scale_relative_floor,
          config_.peak_scale_absolute_floor),
      .zero_crossing_rate = make_robust_stats(
          baseline_zero_crossing_, config_.robust_scale_relative_floor,
          config_.zero_crossing_scale_absolute_floor),
  };
  baseline_flux_.clear();
  baseline_rms_.clear();
  baseline_peak_.clear();
  baseline_zero_crossing_.clear();
}

AcousticFeatureZScores TransientAnomalyDetector::standardize(
    const AcousticFeatureFrame &features) const {
  if (!baseline_.has_value()) {
    throw std::logic_error("Cannot standardize features before baseline completion");
  }
  return AcousticFeatureZScores{
      .spectral_flux = positive_z(static_cast<double>(features.spectral_flux),
                                  baseline_->spectral_flux),
      .frame_rms = positive_z(features.frame_rms, baseline_->frame_rms),
      .peak_absolute =
          positive_z(features.peak_absolute, baseline_->peak_absolute),
      .zero_crossing_rate = positive_z(features.zero_crossing_rate,
                                       baseline_->zero_crossing_rate),
  };
}

TransientAnomalyFrameResult TransientAnomalyDetector::process(
    const AcousticFeatureFrame &features, const std::optional<OnsetEvent> &onset,
    OperatingState operating_state) {
  validate_input(features, onset);
  const bool steady = operating_state == OperatingState::Steady;
  const bool monitoring_eligible = steady && baseline_.has_value();
  std::optional<AcousticFeatureZScores> current_z_scores;
  AnomalyPhase phase = AnomalyPhase::Gated;

  if (!steady) {
    phase = AnomalyPhase::Gated;
    // An unfinished baseline must be one continuous verified-normal steady
    // interval. Entering any other operating state invalidates that interval.
    if (!baseline_.has_value() && baseline_frames_collected_ != 0) {
      baseline_frames_collected_ = 0;
      baseline_flux_.clear();
      baseline_rms_.clear();
      baseline_peak_.clear();
      baseline_zero_crossing_.clear();
    }
  } else if (!baseline_.has_value()) {
    phase = AnomalyPhase::Calibrating;
    add_baseline_observation(features);
    if (baseline_frames_collected_ == baseline_frames_required_) {
      finalize_baseline();
    }
  } else {
    phase = AnomalyPhase::Monitoring;
    current_z_scores = standardize(features);
  }

  retained_features_.push_back(RetainedFeature{
      .features = features,
      .monitoring_eligible = monitoring_eligible,
      .z_scores = current_z_scores,
  });
  while (retained_features_.size() > config_.retained_feature_frames) {
    retained_features_.pop_front();
  }

  std::optional<TransientEventScore> scored_event;
  if (steady && onset.has_value() && baseline_.has_value()) {
    const auto retained = std::find_if(
        retained_features_.begin(), retained_features_.end(),
        [&](const RetainedFeature &candidate) {
          return candidate.features.frame_index == onset->frame_index;
        });
    if (retained == retained_features_.end()) {
      throw std::logic_error("Onset feature frame fell outside retained history");
    }
    if (retained->monitoring_eligible && retained->z_scores.has_value()) {
      const auto &z = *retained->z_scores;
      const double flux_score = z.spectral_flux / config_.flux_z_threshold;
      const double rms_score = z.frame_rms / config_.rms_z_threshold;
      const double peak_score = z.peak_absolute / config_.peak_z_threshold;
      double score = flux_score;
      AnomalyReason reason = AnomalyReason::SpectralFlux;
      if (rms_score > score) {
        score = rms_score;
        reason = AnomalyReason::FrameRms;
      }
      if (peak_score > score) {
        score = peak_score;
        reason = AnomalyReason::PeakAbsolute;
      }

      const bool above_threshold = score >= 1.0;
      const bool suppressed = above_threshold &&
                              last_emitted_anomaly_seconds_.has_value() &&
                              onset->onset_time_seconds -
                                      *last_emitted_anomaly_seconds_ <
                                  config_.anomaly_refractory_seconds;
      const bool emitted = above_threshold && !suppressed;
      scored_event = TransientEventScore{
          .frame_index = onset->frame_index,
          .onset_time_seconds = onset->onset_time_seconds,
          .emitted_at_seconds = onset->emitted_at_seconds,
          .score = score,
          .primary_reason = reason,
          .above_threshold = above_threshold,
          .suppressed_by_refractory = suppressed,
          .emitted_anomaly = emitted,
          .features = retained->features,
          .z_scores = z,
      };
      ++scored_event_count_;
      if (above_threshold) {
        ++anomaly_candidate_count_;
      }
      if (emitted) {
        last_emitted_anomaly_seconds_ = onset->onset_time_seconds;
        ++emitted_anomaly_count_;
      }
    }
  }

  last_frame_index_ = features.frame_index;
  last_frame_center_seconds_ = features.frame_center_seconds;
  last_available_seconds_ = features.available_seconds;
  return TransientAnomalyFrameResult{
      .phase = phase,
      .baseline_frames_collected = baseline_frames_collected_,
      .baseline_frames_required = baseline_frames_required_,
      .baseline_progress =
          static_cast<double>(baseline_frames_collected_) /
          static_cast<double>(baseline_frames_required_),
      .current_z_scores = current_z_scores,
      .scored_event = scored_event,
  };
}

bool TransientAnomalyDetector::handle_discontinuity() noexcept {
  const bool discarded_unfinished_baseline =
      !baseline_.has_value() && baseline_frames_collected_ != 0;
  if (!baseline_.has_value()) {
    baseline_frames_collected_ = 0;
    baseline_flux_.clear();
    baseline_rms_.clear();
    baseline_peak_.clear();
    baseline_zero_crossing_.clear();
  }
  retained_features_.clear();
  last_frame_index_.reset();
  last_frame_center_seconds_.reset();
  last_available_seconds_.reset();
  last_emitted_anomaly_seconds_.reset();
  return discarded_unfinished_baseline;
}

void TransientAnomalyDetector::reset() noexcept {
  baseline_frames_collected_ = 0;
  baseline_flux_.clear();
  baseline_rms_.clear();
  baseline_peak_.clear();
  baseline_zero_crossing_.clear();
  baseline_.reset();
  retained_features_.clear();
  last_frame_index_.reset();
  last_frame_center_seconds_.reset();
  last_available_seconds_.reset();
  last_emitted_anomaly_seconds_.reset();
  scored_event_count_ = 0;
  anomaly_candidate_count_ = 0;
  emitted_anomaly_count_ = 0;
}

} // namespace hero_audio

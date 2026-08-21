#include "hero_audio/causal_onset.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <stdexcept>
#include <string>

namespace hero_audio {
namespace {

void validate_config(const CausalOnsetConfig &config) {
  if (config.threshold_history_frames == 0) {
    throw std::invalid_argument("Causal threshold history must contain at least one frame");
  }
  if (!std::isfinite(config.threshold_stddev_multiplier) ||
      config.threshold_stddev_multiplier < 0.0) {
    throw std::invalid_argument(
        "Threshold standard-deviation multiplier must be finite and non-negative");
  }
  if (!std::isfinite(config.threshold_offset) || config.threshold_offset < 0.0) {
    throw std::invalid_argument("Threshold offset must be finite and non-negative");
  }
  if (!std::isfinite(config.minimum_flux) || config.minimum_flux < 0.0) {
    throw std::invalid_argument("Minimum flux must be finite and non-negative");
  }
  if (!std::isfinite(config.refractory_seconds) || config.refractory_seconds < 0.0) {
    throw std::invalid_argument("Refractory interval must be finite and non-negative");
  }
}

} // namespace

CausalOnsetDetector::CausalOnsetDetector(CausalOnsetConfig config) : config_(config) {
  validate_config(config_);
}

void CausalOnsetDetector::validate_frame(const SpectralFluxFrame &frame) const {
  if (!std::isfinite(frame.frame_start_seconds) || !std::isfinite(frame.frame_center_seconds) ||
      !std::isfinite(frame.available_seconds) || !std::isfinite(frame.spectral_flux) ||
      frame.spectral_flux < 0.0F) {
    throw std::invalid_argument("Spectral Flux frame contains a non-finite or negative value");
  }
  if (frame.frame_start_seconds > frame.frame_center_seconds ||
      frame.frame_center_seconds > frame.available_seconds) {
    throw std::invalid_argument("Spectral Flux frame timestamps are not ordered");
  }
  if (last_frame_index_.has_value() && frame.frame_index != *last_frame_index_ + 1) {
    throw std::invalid_argument("Causal onset detector requires consecutive frame indices");
  }
  if (last_frame_center_seconds_.has_value() &&
      frame.frame_center_seconds <= *last_frame_center_seconds_) {
    throw std::invalid_argument("Frame center timestamps must be strictly increasing");
  }
  if (last_available_seconds_.has_value() && frame.available_seconds <= *last_available_seconds_) {
    throw std::invalid_argument("Frame availability timestamps must be strictly increasing");
  }
}

double CausalOnsetDetector::threshold_from_history() const {
  const double count = static_cast<double>(history_.size());
  const double sum = std::accumulate(history_.begin(), history_.end(), 0.0);
  const double mean = sum / count;
  double squared_error_sum = 0.0;
  for (const float value : history_) {
    const double error = static_cast<double>(value) - mean;
    squared_error_sum += error * error;
  }
  const double standard_deviation = std::sqrt(squared_error_sum / count);
  return mean + config_.threshold_stddev_multiplier * standard_deviation + config_.threshold_offset;
}

std::optional<OnsetEvent> CausalOnsetDetector::process(const SpectralFluxFrame &frame) {
  validate_frame(frame);
  std::optional<OnsetEvent> emitted;
  bool extended_plateau = false;

  // A candidate is held for exactly one frame. The newly arrived frame is used
  // only as its right neighbour; candidate thresholding happened previously.
  if (pending_candidate_.has_value()) {
    if (pending_candidate_->frame.spectral_flux > frame.spectral_flux) {
      const double onset_time = pending_candidate_->frame.frame_center_seconds;
      const bool outside_refractory =
          !last_emitted_onset_seconds_.has_value() ||
          onset_time - *last_emitted_onset_seconds_ >= config_.refractory_seconds;
      if (outside_refractory) {
        const double delay_ms = (frame.available_seconds - onset_time) * 1000.0;
        if (delay_ms < 0.0 || !std::isfinite(delay_ms)) {
          throw std::invalid_argument("Onset emission time precedes its signal time");
        }
        emitted = OnsetEvent{
            .frame_index = pending_candidate_->frame.frame_index,
            .onset_time_seconds = onset_time,
            .emitted_at_seconds = frame.available_seconds,
            .algorithm_delay_ms = delay_ms,
            .spectral_flux = pending_candidate_->frame.spectral_flux,
            .threshold = pending_candidate_->threshold,
        };
        last_emitted_onset_seconds_ = onset_time;
      }
    } else if (pending_candidate_->frame.spectral_flux == frame.spectral_flux) {
      // Carry a flat peak forward without recomputing its threshold. The final
      // frame of the plateau is emitted when the first strict decrease arrives.
      pending_candidate_->frame = frame;
      extended_plateau = true;
    }
  }
  if (!extended_plateau) {
    pending_candidate_.reset();
  }

  // history_ contains prior frames only at this point. This ordering is the
  // causal guarantee: current flux is appended after its threshold is computed.
  if (!extended_plateau && !history_.empty()) {
    const double threshold = threshold_from_history();
    const double current_flux = static_cast<double>(frame.spectral_flux);
    const double previous_flux = static_cast<double>(history_.back());
    if (current_flux > config_.minimum_flux && current_flux > threshold &&
        current_flux >= previous_flux) {
      pending_candidate_ = Candidate{.frame = frame, .threshold = threshold};
    }
  }

  history_.push_back(frame.spectral_flux);
  if (history_.size() > config_.threshold_history_frames) {
    history_.pop_front();
  }
  last_frame_index_ = frame.frame_index;
  last_frame_center_seconds_ = frame.frame_center_seconds;
  last_available_seconds_ = frame.available_seconds;
  return emitted;
}

void CausalOnsetDetector::reset() noexcept {
  history_.clear();
  pending_candidate_.reset();
  last_emitted_onset_seconds_.reset();
  last_frame_index_.reset();
  last_frame_center_seconds_.reset();
  last_available_seconds_.reset();
}

std::vector<OnsetEvent> detect_causal_onsets(std::span<const SpectralFluxFrame> frames,
                                             CausalOnsetConfig config) {
  CausalOnsetDetector detector(config);
  std::vector<OnsetEvent> result;
  for (const auto &frame : frames) {
    if (auto event = detector.process(frame); event.has_value()) {
      result.push_back(*event);
    }
  }
  return result;
}

void write_onsets_csv(std::ostream &output, std::span<const OnsetEvent> events) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  output << "frame_index,onset_time_seconds,emitted_at_seconds,algorithm_delay_ms,"
            "spectral_flux,threshold\n";
  output << std::fixed << std::setprecision(9);
  for (const auto &event : events) {
    output << event.frame_index << ',' << event.onset_time_seconds << ','
           << event.emitted_at_seconds << ',' << event.algorithm_delay_ms << ','
           << event.spectral_flux << ',' << event.threshold << '\n';
  }
  output.flags(old_flags);
  output.precision(old_precision);
  if (!output) {
    throw std::runtime_error("Unable to write onset CSV");
  }
}

void write_onsets_csv(const std::filesystem::path &path, std::span<const OnsetEvent> events) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open onset CSV: " + path.string());
  }
  write_onsets_csv(output, events);
}

} // namespace hero_audio

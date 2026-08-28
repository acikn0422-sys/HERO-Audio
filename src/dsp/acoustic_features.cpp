#include "hero_audio/acoustic_features.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hero_audio {

AcousticFeatureExtractor::AcousticFeatureExtractor(
    AcousticFeatureExtractorConfig config)
    : config_(config), sample_ring_(config.frame_size_samples, 0.0F) {
  if (config_.frame_size_samples == 0 || config_.hop_size_samples == 0) {
    throw std::invalid_argument("Acoustic feature frame and hop sizes must be non-zero");
  }
  if (config_.frame_size_samples < config_.hop_size_samples ||
      config_.frame_size_samples % config_.hop_size_samples != 0) {
    throw std::invalid_argument(
        "Acoustic feature frame size must be an integer multiple of hop size");
  }
}

std::optional<AcousticFeatureFrame> AcousticFeatureExtractor::process_hop(
    std::span<const float> samples, const StreamingHopResult &streaming_result) {
  const auto frame_size = config_.frame_size_samples;
  const auto hop_size = config_.hop_size_samples;
  if (samples.size() != hop_size) {
    throw std::invalid_argument("Acoustic feature input must contain exactly one hop");
  }
  if (std::any_of(samples.begin(), samples.end(),
                  [](float sample) { return !std::isfinite(sample); })) {
    throw std::invalid_argument("Acoustic feature input contains a non-finite sample");
  }
  if (total_samples_received_ > std::numeric_limits<std::size_t>::max() - hop_size) {
    throw std::overflow_error("Acoustic feature sample counter overflow");
  }

  const auto expected_total = total_samples_received_ + hop_size;
  const bool feature_expected = expected_total >= frame_size;
  if (streaming_result.input_hop_index != input_hop_index_ ||
      streaming_result.total_samples_received != expected_total ||
      streaming_result.flux_frame.has_value() != feature_expected) {
    throw std::invalid_argument(
        "Acoustic feature input is not synchronized with StreamingProcessor");
  }
  if (streaming_result.flux_frame.has_value() &&
      streaming_result.flux_frame->frame_index != frame_index_) {
    throw std::invalid_argument("Acoustic feature frame index is not consecutive");
  }

  for (const float sample : samples) {
    sample_ring_[write_position_] = sample;
    write_position_ = (write_position_ + 1) % frame_size;
  }
  total_samples_received_ = expected_total;
  ++input_hop_index_;

  if (!streaming_result.flux_frame.has_value()) {
    return std::nullopt;
  }

  // write_position_ points to the oldest sample after the hop is inserted, so
  // this loop examines the identical 1024-sample frame used by the FFT path.
  double squared_sum = 0.0;
  double peak_absolute = 0.0;
  std::size_t zero_crossings = 0;
  float previous = sample_ring_[write_position_];
  for (std::size_t offset = 0; offset < frame_size; ++offset) {
    const float sample = sample_ring_[(write_position_ + offset) % frame_size];
    const double value = static_cast<double>(sample);
    squared_sum += value * value;
    peak_absolute = std::max(peak_absolute, std::abs(value));
    if (offset != 0 && ((previous >= 0.0F) != (sample >= 0.0F))) {
      ++zero_crossings;
    }
    previous = sample;
  }

  const auto &flux = *streaming_result.flux_frame;
  AcousticFeatureFrame result{
      .frame_index = flux.frame_index,
      .frame_center_seconds = flux.frame_center_seconds,
      .available_seconds = flux.available_seconds,
      .spectral_flux = flux.spectral_flux,
      .frame_rms = std::sqrt(squared_sum / static_cast<double>(frame_size)),
      .peak_absolute = peak_absolute,
      .zero_crossing_rate =
          frame_size > 1
              ? static_cast<double>(zero_crossings) /
                    static_cast<double>(frame_size - 1)
              : 0.0,
  };
  ++frame_index_;
  return result;
}

void AcousticFeatureExtractor::reset() noexcept {
  std::fill(sample_ring_.begin(), sample_ring_.end(), 0.0F);
  write_position_ = 0;
  total_samples_received_ = 0;
  input_hop_index_ = 0;
  frame_index_ = 0;
}

} // namespace hero_audio

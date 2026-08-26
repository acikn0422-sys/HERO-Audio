#include "hero_audio/streaming_processor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hero_audio {
namespace {

void validate_config(const StreamingProcessorConfig &config, const FFTBackend &fft) {
  const auto frame_size = config.spectral_flux.frame_size_samples;
  const auto hop_size = config.spectral_flux.hop_size_samples;
  if (config.sample_rate_hz == 0) {
    throw std::invalid_argument("Streaming sample rate must be non-zero");
  }
  if (frame_size == 0 || hop_size == 0) {
    throw std::invalid_argument("Streaming frame and hop sizes must be non-zero");
  }
  if (frame_size < hop_size || frame_size % hop_size != 0) {
    throw std::invalid_argument(
        "Streaming frame size must be an integer multiple of hop size");
  }
  if (frame_size != fft.fft_size()) {
    throw std::invalid_argument("Streaming frame size must match FFT backend size");
  }
}

[[nodiscard]] StreamingProcessorConfig
validated_config(StreamingProcessorConfig config, const FFTBackend &fft) {
  validate_config(config, fft);
  return config;
}

} // namespace

StreamingProcessor::StreamingProcessor(FFTBackend &fft, StreamingProcessorConfig config)
    : config_(validated_config(config, fft)), fft_(fft),
      hann_window_(make_hann_window(config_.spectral_flux.frame_size_samples)),
      sample_ring_(config_.spectral_flux.frame_size_samples, 0.0F),
      windowed_frame_(config_.spectral_flux.frame_size_samples, 0.0F),
      spectrum_(config_.spectral_flux.frame_size_samples / 2 + 1),
      previous_magnitude_(config_.spectral_flux.frame_size_samples / 2 + 1, 0.0F),
      current_magnitude_(config_.spectral_flux.frame_size_samples / 2 + 1, 0.0F),
      onset_detector_(config_.onset) {}

StreamingHopResult StreamingProcessor::push_hop(std::span<const float> new_samples) {
  const auto frame_size = config_.spectral_flux.frame_size_samples;
  const auto hop_size = config_.spectral_flux.hop_size_samples;
  if (new_samples.size() != hop_size) {
    throw std::invalid_argument("Streaming push must contain exactly one hop");
  }
  if (std::any_of(new_samples.begin(), new_samples.end(),
                  [](float sample) { return !std::isfinite(sample); })) {
    throw std::invalid_argument("Streaming hop contains a non-finite sample");
  }
  if (total_samples_received_ > std::numeric_limits<std::size_t>::max() - hop_size) {
    throw std::overflow_error("Streaming sample counter overflow");
  }

  const std::size_t current_hop_index = input_hop_index_;
  for (const float sample : new_samples) {
    sample_ring_[write_position_] = sample;
    write_position_ = (write_position_ + 1) % frame_size;
  }
  total_samples_received_ += hop_size;
  ++input_hop_index_;

  StreamingHopResult result{
      .input_hop_index = current_hop_index,
      .total_samples_received = total_samples_received_,
  };
  if (total_samples_received_ < frame_size) {
    return result;
  }

  // write_position_ always points at the oldest sample after a complete hop is
  // inserted. Walking the ring from that position reconstructs exactly the
  // same frame order as offline mono_samples[start + sample].
  for (std::size_t sample = 0; sample < frame_size; ++sample) {
    const auto ring_index = (write_position_ + sample) % frame_size;
    windowed_frame_[sample] = sample_ring_[ring_index] * hann_window_[sample];
  }

  fft_.execute(windowed_frame_, spectrum_);
  for (std::size_t bin = 0; bin < spectrum_.size(); ++bin) {
    current_magnitude_[bin] = std::abs(spectrum_[bin]);
    if (!std::isfinite(current_magnitude_[bin])) {
      throw std::overflow_error("Streaming FFT magnitude is not finite");
    }
  }

  double flux = 0.0;
  if (frame_index_ != 0) {
    for (std::size_t bin = 0; bin < current_magnitude_.size(); ++bin) {
      flux += std::max(0.0F, current_magnitude_[bin] - previous_magnitude_[bin]);
    }
  }
  if (flux > static_cast<double>(std::numeric_limits<float>::max())) {
    throw std::overflow_error("Streaming Spectral Flux exceeds float32 range");
  }

  const auto start_sample = frame_index_ * hop_size;
  const double start = static_cast<double>(start_sample);
  const double frame_size_as_double = static_cast<double>(frame_size);
  const double sample_rate = static_cast<double>(config_.sample_rate_hz);
  result.flux_frame = SpectralFluxFrame{
      .frame_index = frame_index_,
      .frame_start_sample = start_sample,
      .frame_start_seconds = start / sample_rate,
      .frame_center_seconds = (start + frame_size_as_double / 2.0) / sample_rate,
      .available_seconds = (start + frame_size_as_double) / sample_rate,
      .spectral_flux = static_cast<float>(flux),
  };
  result.onset = onset_detector_.process(*result.flux_frame);

  previous_magnitude_.swap(current_magnitude_);
  ++frame_index_;
  return result;
}

void StreamingProcessor::reset() noexcept {
  std::fill(sample_ring_.begin(), sample_ring_.end(), 0.0F);
  std::fill(windowed_frame_.begin(), windowed_frame_.end(), 0.0F);
  std::fill(spectrum_.begin(), spectrum_.end(), std::complex<float>{});
  std::fill(previous_magnitude_.begin(), previous_magnitude_.end(), 0.0F);
  std::fill(current_magnitude_.begin(), current_magnitude_.end(), 0.0F);
  onset_detector_.reset();
  write_position_ = 0;
  total_samples_received_ = 0;
  input_hop_index_ = 0;
  frame_index_ = 0;
}

} // namespace hero_audio

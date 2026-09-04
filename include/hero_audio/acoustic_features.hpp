#pragma once

#include "hero_audio/streaming_processor.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace hero_audio {

// The anomaly layer observes the same raw hop as StreamingProcessor, but does
// not execute another FFT. It combines inexpensive time-domain measurements
// with the Spectral Flux already produced by the main onset pipeline.
struct AcousticFeatureExtractorConfig {
  std::size_t frame_size_samples{1024};
  std::size_t hop_size_samples{256};
};

struct AcousticFeatureFrame {
  std::size_t frame_index{};
  double frame_center_seconds{};
  double available_seconds{};
  float spectral_flux{};
  double frame_rms{};
  double peak_absolute{};
  double zero_crossing_rate{};
};

class AcousticFeatureExtractor {
public:
  explicit AcousticFeatureExtractor(AcousticFeatureExtractorConfig config = {});

  // samples and streaming_result must describe the same newly arrived hop.
  // No feature exists during the initial frame-fill period.
  [[nodiscard]] std::optional<AcousticFeatureFrame>
  process_hop(std::span<const float> samples,
              const StreamingHopResult &streaming_result);

  void reset() noexcept;

  [[nodiscard]] const AcousticFeatureExtractorConfig &config() const noexcept {
    return config_;
  }

private:
  AcousticFeatureExtractorConfig config_;
  std::vector<float> sample_ring_;
  std::size_t write_position_{};
  std::size_t total_samples_received_{};
  std::size_t input_hop_index_{};
  std::size_t frame_index_{};
};

} // namespace hero_audio

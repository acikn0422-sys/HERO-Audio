#pragma once

#include "hero_audio/spectral_flux.hpp"

#include <cstddef>
#include <deque>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <vector>

namespace hero_audio {

struct CausalOnsetConfig {
  std::size_t threshold_history_frames{16};
  double threshold_stddev_multiplier{1.5};
  double threshold_offset{0.0};
  double minimum_flux{0.0};
  double refractory_seconds{0.030};
};

struct OnsetEvent {
  std::size_t frame_index{};
  double onset_time_seconds{};
  double emitted_at_seconds{};
  double algorithm_delay_ms{};
  float spectral_flux{};
  double threshold{};
};

// Streaming detector. process() consumes exactly one consecutive Spectral Flux
// frame and may emit the previously buffered candidate after one-frame peak
// confirmation. No flush operation exists: the last frame cannot be confirmed
// without a future frame, matching real streaming behaviour.
class CausalOnsetDetector {
public:
  explicit CausalOnsetDetector(CausalOnsetConfig config = {});

  [[nodiscard]] std::optional<OnsetEvent> process(const SpectralFluxFrame &frame);
  void reset() noexcept;

  [[nodiscard]] const CausalOnsetConfig &config() const noexcept { return config_; }

private:
  struct Candidate {
    SpectralFluxFrame frame;
    double threshold{};
  };

  [[nodiscard]] double threshold_from_history() const;
  void validate_frame(const SpectralFluxFrame &frame) const;

  CausalOnsetConfig config_;
  std::deque<float> history_;
  std::optional<Candidate> pending_candidate_;
  std::optional<double> last_emitted_onset_seconds_;
  std::optional<std::size_t> last_frame_index_;
  std::optional<double> last_frame_center_seconds_;
  std::optional<double> last_available_seconds_;
};

[[nodiscard]] std::vector<OnsetEvent>
detect_causal_onsets(std::span<const SpectralFluxFrame> frames, CausalOnsetConfig config = {});

void write_onsets_csv(std::ostream &output, std::span<const OnsetEvent> events);
void write_onsets_csv(const std::filesystem::path &path, std::span<const OnsetEvent> events);

} // namespace hero_audio

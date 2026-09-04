#pragma once

#include "hero_audio/causal_onset.hpp"
#include "hero_audio/fft_backend.hpp"
#include "hero_audio/spectral_flux.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hero_audio {

struct StreamingProcessorConfig {
  std::uint32_t sample_rate_hz{48000};
  SpectralFluxConfig spectral_flux;
  CausalOnsetConfig onset;
};

// One call represents exactly one newly arrived hop. During startup, a result
// has no flux_frame until enough samples exist to fill the first FFT window.
struct StreamingHopResult {
  std::size_t input_hop_index{}; // Zero-based input hop index.
  std::size_t total_samples_received{};
  std::optional<SpectralFluxFrame> flux_frame;
  std::optional<OnsetEvent> onset;
};

// Stateful, causal CPU pipeline. All sample/FFT work buffers are allocated in
// the constructor; push_hop() reuses them and never reads future samples.
class StreamingProcessor {
public:
  StreamingProcessor(FFTBackend &fft, StreamingProcessorConfig config = {});

  // Exactly hop_size_samples new mono float32 samples must be supplied. The
  // function throws before changing state if the span has the wrong size or
  // contains a non-finite value.
  [[nodiscard]] StreamingHopResult push_hop(std::span<const float> new_samples);
  void reset() noexcept;

  [[nodiscard]] const StreamingProcessorConfig &config() const noexcept { return config_; }
  [[nodiscard]] std::size_t input_hops_received() const noexcept { return input_hop_index_; }
  [[nodiscard]] std::size_t frames_produced() const noexcept { return frame_index_; }

private:
  StreamingProcessorConfig config_;
  FFTBackend &fft_;
  std::vector<float> hann_window_;
  std::vector<float> sample_ring_;
  std::vector<float> windowed_frame_;
  std::vector<std::complex<float>> spectrum_;
  std::vector<float> previous_magnitude_;
  std::vector<float> current_magnitude_;
  CausalOnsetDetector onset_detector_;
  std::size_t write_position_{};
  std::size_t total_samples_received_{};
  std::size_t input_hop_index_{};
  std::size_t frame_index_{};
};

} // namespace hero_audio

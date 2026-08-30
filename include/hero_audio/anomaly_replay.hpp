#pragma once

#include "hero_audio/acoustic_features.hpp"
#include "hero_audio/anomaly_detector.hpp"
#include "hero_audio/fft_backend.hpp"
#include "hero_audio/streaming_processor.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hero_audio {

struct AnomalyReplayConfig {
  StreamingProcessorConfig streaming;
  TransientAnomalyConfig anomaly;
  OperatingState operating_state{OperatingState::Steady};
};

struct AnomalyReplayFrame {
  std::size_t input_hop_index{};
  AcousticFeatureFrame features;
  TransientAnomalyFrameResult decision;
};

struct AnomalyReplayEvent {
  std::size_t input_hop_index{};
  TransientEventScore event;
};

struct AnomalyReplayResult {
  std::uint32_t sample_rate_hz{};
  std::size_t input_sample_count{};
  std::size_t processed_sample_count{};
  std::size_t ignored_tail_sample_count{};
  std::size_t processed_hop_count{};
  std::string backend_name;
  AnomalyReplayConfig config;
  std::vector<AnomalyReplayFrame> frames;
  std::vector<AnomalyReplayEvent> emitted_events;
  std::optional<TransientNormalBaseline> baseline;
  std::size_t baseline_frames_required{};
  std::size_t baseline_frames_collected{};
  std::size_t scored_event_count{};
  std::size_t anomaly_candidate_count{};
};

// Deterministic replay: no sleeping, no wall-clock timing and no implicit tail
// padding. Input is consumed in consecutive full hops by the exact live DSP and
// anomaly classes. The input sample rate overrides both embedded config rates.
[[nodiscard]] AnomalyReplayResult replay_anomaly_audio(
    std::span<const float> mono_samples, std::uint32_t sample_rate_hz,
    FFTBackend &fft, AnomalyReplayConfig config = {});

void write_anomaly_replay_frames_csv(std::ostream &output,
                                     const AnomalyReplayResult &result);
void write_anomaly_replay_frames_csv(const std::filesystem::path &path,
                                     const AnomalyReplayResult &result);
void write_anomaly_replay_events_csv(std::ostream &output,
                                     const AnomalyReplayResult &result);
void write_anomaly_replay_events_csv(const std::filesystem::path &path,
                                     const AnomalyReplayResult &result);
void write_anomaly_replay_summary_json(std::ostream &output,
                                       const AnomalyReplayResult &result,
                                       std::string_view input_path);
void write_anomaly_replay_summary_json(const std::filesystem::path &path,
                                       const AnomalyReplayResult &result,
                                       std::string_view input_path);

} // namespace hero_audio

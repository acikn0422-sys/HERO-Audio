#pragma once

#include "hero_audio/spsc_hop_queue.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hero_audio {

struct CoreAudioCaptureStats {
  std::uint64_t callback_count{};
  std::uint64_t input_timeline_frame_count{};
  std::uint64_t rendered_frame_count{};
  std::uint64_t enqueued_hop_count{};
  std::uint64_t dropped_hop_count{};
  std::uint64_t render_error_count{};
  std::uint32_t partial_sample_count{};
  std::int32_t last_render_status{};
};

// macOS AUHAL capture adapter. The implementation configures the current
// default input device as mono packed float32 at its native sample rate. The
// real-time callback renders into preallocated storage and only aggregates and
// enqueues complete 256-sample blocks; FFT and file I/O stay on the consumer.
class CoreAudioInput {
public:
  explicit CoreAudioInput(LiveAudioHopQueue &queue);
  ~CoreAudioInput();

  CoreAudioInput(const CoreAudioInput &) = delete;
  CoreAudioInput &operator=(const CoreAudioInput &) = delete;

  void start();
  void stop() noexcept;

  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] double sample_rate_hz() const noexcept;
  [[nodiscard]] std::uint32_t source_channel_count() const noexcept;
  [[nodiscard]] std::uint32_t maximum_frames_per_slice() const noexcept;
  [[nodiscard]] std::uint32_t device_id() const noexcept;
  [[nodiscard]] const std::string &device_name() const noexcept;
  [[nodiscard]] CoreAudioCaptureStats stats() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// CoreAudio host time is monotonic. These wrappers let the live CLI measure
// callback-entry-to-consumer delay without exposing Apple types in its source.
[[nodiscard]] std::uint64_t coreaudio_current_host_time() noexcept;
[[nodiscard]] double coreaudio_host_time_delta_ms(std::uint64_t earlier,
                                                  std::uint64_t later) noexcept;

} // namespace hero_audio

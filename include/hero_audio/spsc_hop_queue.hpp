#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace hero_audio {

// One complete block transferred from the real-time capture callback to the
// analysis thread. first_sample_index stays on the absolute capture timeline,
// so the consumer can detect and recover from queue overflow discontinuities.
template <std::size_t HopSize> struct AudioHopBlock {
  std::array<float, HopSize> samples{};
  std::uint64_t sequence{};
  std::uint64_t first_sample_index{};
  std::uint64_t callback_host_time{};
};

// Fixed-storage single-producer/single-consumer queue. The audio callback is
// the only producer and the analysis loop is the only consumer. Neither path
// locks nor allocates. Capacity is the number of complete hop blocks retained.
template <std::size_t HopSize, std::size_t Capacity> class SpscHopQueue {
  static_assert(HopSize > 0, "SPSC hop size must be non-zero");
  static_assert(Capacity > 0, "SPSC queue capacity must be non-zero");
  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "The real-time SPSC queue requires lock-free atomic indices");

public:
  using value_type = AudioHopBlock<HopSize>;

  [[nodiscard]] static constexpr std::size_t hop_size() noexcept { return HopSize; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // Producer-only. A failed push leaves both queue contents and indices
  // unchanged, allowing the callback to count and report a dropped hop.
  [[nodiscard]] bool try_push(std::span<const float, HopSize> samples,
                              std::uint64_t sequence,
                              std::uint64_t first_sample_index,
                              std::uint64_t callback_host_time) noexcept {
    const std::size_t write = write_index_.load(std::memory_order_relaxed);
    const std::size_t read = read_index_.load(std::memory_order_acquire);
    if (write - read >= Capacity) {
      return false;
    }

    auto &slot = slots_[write % Capacity];
    std::copy(samples.begin(), samples.end(), slot.samples.begin());
    slot.sequence = sequence;
    slot.first_sample_index = first_sample_index;
    slot.callback_host_time = callback_host_time;
    write_index_.store(write + 1, std::memory_order_release);
    return true;
  }

  // Consumer-only. The output block is changed only when an item is present.
  [[nodiscard]] bool try_pop(value_type &output) noexcept {
    const std::size_t read = read_index_.load(std::memory_order_relaxed);
    const std::size_t write = write_index_.load(std::memory_order_acquire);
    if (read == write) {
      return false;
    }

    output = slots_[read % Capacity];
    read_index_.store(read + 1, std::memory_order_release);
    return true;
  }

  // A diagnostic snapshot only; the value can change immediately when the
  // producer and consumer are active and must not be used for synchronization.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::size_t write = write_index_.load(std::memory_order_acquire);
    const std::size_t read = read_index_.load(std::memory_order_acquire);
    return write - read;
  }

private:
  static_assert(std::is_trivially_copyable_v<value_type>);
  std::array<value_type, Capacity> slots_{};

  // Keep the frequently written indices on separate cache lines to avoid the
  // callback and consumer invalidating the same cache line on every hop.
  alignas(64) std::atomic<std::size_t> write_index_{0};
  alignas(64) std::atomic<std::size_t> read_index_{0};
};

inline constexpr std::size_t kLiveHopSize = 256;
inline constexpr std::size_t kLiveQueueCapacity = 64;
using LiveAudioHopBlock = AudioHopBlock<kLiveHopSize>;
using LiveAudioHopQueue = SpscHopQueue<kLiveHopSize, kLiveQueueCapacity>;

} // namespace hero_audio

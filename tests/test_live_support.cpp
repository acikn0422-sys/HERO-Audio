#include "hero_audio/spsc_hop_queue.hpp"
#include "hero_audio/wav_reader.hpp"
#include "hero_audio/wav_writer.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool near(double actual, double expected, double tolerance = 4.0e-5) {
  return std::abs(actual - expected) <= tolerance;
}

bool test_queue_capacity_and_order() {
  hero_audio::SpscHopQueue<4, 2> queue;
  const std::array<float, 4> first{1.0F, 2.0F, 3.0F, 4.0F};
  const std::array<float, 4> second{5.0F, 6.0F, 7.0F, 8.0F};
  const std::array<float, 4> rejected{9.0F, 10.0F, 11.0F, 12.0F};
  if (!queue.try_push(first, 10, 40, 1000) ||
      !queue.try_push(second, 11, 44, 2000) ||
      queue.try_push(rejected, 12, 48, 3000) || queue.size_approx() != 2) {
    return false;
  }

  hero_audio::AudioHopBlock<4> block;
  if (!queue.try_pop(block) || block.sequence != 10 || block.first_sample_index != 40 ||
      block.callback_host_time != 1000 || block.samples != first) {
    return false;
  }
  if (!queue.try_pop(block) || block.sequence != 11 || block.samples != second ||
      queue.try_pop(block) || queue.size_approx() != 0) {
    return false;
  }
  return true;
}

bool test_queue_concurrent_transfer() {
  constexpr std::size_t item_count = 50000;
  hero_audio::SpscHopQueue<8, 32> queue;
  std::atomic<bool> producer_done{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> abort_transfer{false};

  std::thread producer([&] {
    for (std::size_t item = 0; item < item_count; ++item) {
      std::array<float, 8> samples{};
      samples.fill(static_cast<float>(item));
      while (!queue.try_push(samples, item, item * samples.size(), item + 100)) {
        if (abort_transfer.load(std::memory_order_acquire)) {
          return;
        }
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::size_t expected = 0;
  hero_audio::AudioHopBlock<8> block;
  while (expected < item_count) {
    if (!queue.try_pop(block)) {
      if (producer_done.load(std::memory_order_acquire) && queue.size_approx() == 0) {
        failed.store(true, std::memory_order_relaxed);
        abort_transfer.store(true, std::memory_order_release);
        break;
      }
      std::this_thread::yield();
      continue;
    }
    if (block.sequence != expected || block.first_sample_index != expected * 8 ||
        block.callback_host_time != expected + 100 ||
        block.samples.front() != static_cast<float>(expected) ||
        block.samples.back() != static_cast<float>(expected)) {
      failed.store(true, std::memory_order_relaxed);
      abort_transfer.store(true, std::memory_order_release);
      break;
    }
    ++expected;
  }
  producer.join();
  return !failed.load(std::memory_order_relaxed) && expected == item_count;
}

class LocalPath {
public:
  LocalPath() : path_(std::filesystem::current_path() / "hero_audio_live_writer_test.wav") {}
  ~LocalPath() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &get() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

bool test_streaming_wav_writer_round_trip() {
  LocalPath path;
  {
    hero_audio::Pcm16WavWriter writer(path.get(), 48000);
    const std::array<float, 5> signal{-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
    writer.append(signal);
    writer.append_silence(3);
    if (writer.sample_count() != 8 || writer.finalized()) {
      return false;
    }
    writer.finalize();
    writer.finalize(); // Idempotent finalization is safe for cleanup paths.
    if (!writer.finalized()) {
      return false;
    }
    try {
      writer.append(signal);
      return false;
    } catch (const std::logic_error &) {
    }
  }

  const auto audio = hero_audio::read_wav(path.get());
  if (audio.sample_rate_hz != 48000 || audio.source_channels != 1 ||
      audio.mono_samples.size() != 8) {
    return false;
  }
  const std::array<double, 8> expected{-1.0, -0.5, 0.0, 0.5, 1.0, 0.0, 0.0, 0.0};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (!near(audio.mono_samples[index], expected[index])) {
      return false;
    }
  }
  return true;
}

bool test_wav_writer_rejects_non_finite_without_advancing() {
  LocalPath path;
  hero_audio::Pcm16WavWriter writer(path.get(), 48000);
  const std::array<float, 3> invalid{0.25F,
                                     std::numeric_limits<float>::quiet_NaN(), 0.5F};
  try {
    writer.append(invalid);
    return false;
  } catch (const std::invalid_argument &) {
  }
  return writer.sample_count() == 0;
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, bool (*)()>> tests{
      {"queue capacity and order", test_queue_capacity_and_order},
      {"queue concurrent transfer", test_queue_concurrent_transfer},
      {"streaming WAV writer round trip", test_streaming_wav_writer_round_trip},
      {"WAV writer non-finite rejection",
       test_wav_writer_rejects_non_finite_without_advancing},
  };
  for (const auto &[name, test] : tests) {
    if (!test()) {
      std::cerr << "Live support test failed: " << name << '\n';
      return 1;
    }
  }
  std::cout << "Live support tests passed\n";
  return 0;
}

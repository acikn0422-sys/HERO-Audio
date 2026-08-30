#include "hero_audio/anomaly_replay.hpp"
#include "hero_audio/fft_backend.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

std::vector<float> make_replay_audio() {
  constexpr std::size_t hop_size = 256;
  std::vector<float> samples(10 * hop_size + 17, 0.0F);
  for (std::size_t sample = 6 * hop_size; sample < 7 * hop_size; ++sample) {
    samples[sample] = sample % 2 == 0 ? 0.8F : -0.8F;
  }
  return samples;
}

bool test_replay_is_deterministic_and_ignores_tail() {
  const auto samples = make_replay_audio();
  hero_audio::AnomalyReplayConfig config;
  config.anomaly.baseline_seconds = 3.0;

  auto first_fft = hero_audio::make_fft_backend(
      hero_audio::FFTBackendKind::Reference, 1024);
  auto second_fft = hero_audio::make_fft_backend(
      hero_audio::FFTBackendKind::Reference, 1024);
  const auto first = hero_audio::replay_anomaly_audio(samples, 256, *first_fft, config);
  const auto second = hero_audio::replay_anomaly_audio(samples, 256, *second_fft, config);
  if (first.processed_hop_count != 10 || first.processed_sample_count != 2560 ||
      first.ignored_tail_sample_count != 17 || first.frames.size() != 7 ||
      !first.baseline.has_value() || first.baseline_frames_required != 3) {
    return false;
  }

  std::ostringstream first_frames;
  std::ostringstream second_frames;
  std::ostringstream first_events;
  std::ostringstream second_events;
  std::ostringstream first_summary;
  std::ostringstream second_summary;
  hero_audio::write_anomaly_replay_frames_csv(first_frames, first);
  hero_audio::write_anomaly_replay_frames_csv(second_frames, second);
  hero_audio::write_anomaly_replay_events_csv(first_events, first);
  hero_audio::write_anomaly_replay_events_csv(second_events, second);
  hero_audio::write_anomaly_replay_summary_json(first_summary, first, "input.wav");
  hero_audio::write_anomaly_replay_summary_json(second_summary, second, "input.wav");
  return first_frames.str() == second_frames.str() &&
         first_events.str() == second_events.str() &&
         first_summary.str() == second_summary.str() &&
         first_summary.str().find("\"deterministic_non_timing_output\": true") !=
             std::string::npos;
}

bool test_replay_rejects_non_finite_even_in_tail() {
  auto samples = make_replay_audio();
  samples.back() = std::numeric_limits<float>::quiet_NaN();
  auto fft = hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, 1024);
  try {
    static_cast<void>(hero_audio::replay_anomaly_audio(samples, 256, *fft));
    return false;
  } catch (const std::invalid_argument &) {
  }
  return true;
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, bool (*)()>> tests{
      {"deterministic replay and tail policy",
       test_replay_is_deterministic_and_ignores_tail},
      {"non-finite replay input", test_replay_rejects_non_finite_even_in_tail},
  };
  for (const auto &[name, test] : tests) {
    if (!test()) {
      std::cerr << "Anomaly replay test failed: " << name << '\n';
      return 1;
    }
  }
  std::cout << "Anomaly replay tests passed\n";
  return 0;
}

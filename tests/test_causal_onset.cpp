#include "hero_audio/causal_onset.hpp"
#include "hero_audio/fft_backend.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::size_t kFrameSize = 1024;
constexpr std::size_t kHopSize = 256;

bool near(double actual, double expected, double tolerance = 1.0e-8) {
  return std::abs(actual - expected) <= tolerance;
}

std::vector<hero_audio::SpectralFluxFrame> make_frames(const std::vector<float> &flux) {
  std::vector<hero_audio::SpectralFluxFrame> frames;
  frames.reserve(flux.size());
  for (std::size_t index = 0; index < flux.size(); ++index) {
    const auto start_sample = index * kHopSize;
    frames.push_back(hero_audio::SpectralFluxFrame{
        .frame_index = index,
        .frame_start_sample = start_sample,
        .frame_start_seconds = static_cast<double>(start_sample) / kSampleRate,
        .frame_center_seconds =
            (static_cast<double>(start_sample) + kFrameSize / 2.0) / kSampleRate,
        .available_seconds = (static_cast<double>(start_sample) + kFrameSize) / kSampleRate,
        .spectral_flux = flux[index],
    });
  }
  return frames;
}

bool test_silence_produces_no_onset() {
  const auto frames = make_frames(std::vector<float>(40, 0.0F));
  return hero_audio::detect_causal_onsets(frames).empty();
}

bool test_one_frame_confirmation_and_causal_threshold() {
  const auto frames = make_frames({0.0F, 0.0F, 0.0F, 10.0F, 1.0F});
  hero_audio::CausalOnsetDetector detector;
  for (std::size_t index = 0; index < 4; ++index) {
    if (detector.process(frames[index]).has_value()) {
      return false;
    }
  }
  const auto event = detector.process(frames[4]);
  return event.has_value() && event->frame_index == 3 && near(event->threshold, 0.0) &&
         near(event->onset_time_seconds, frames[3].frame_center_seconds) &&
         near(event->emitted_at_seconds, frames[4].available_seconds) &&
         near(event->algorithm_delay_ms, 16.0);
}

bool test_nonzero_threshold_formula() {
  const auto events = hero_audio::detect_causal_onsets(make_frames({0.0F, 2.0F, 10.0F, 0.0F}));
  // History before frame 2 is [0, 2]: mean=1, population stddev=1.
  return events.size() == 1 && events[0].frame_index == 2 && near(events[0].threshold, 2.5);
}

bool test_history_capacity_discards_old_frames() {
  const auto frames = make_frames({100.0F, 0.0F, 0.0F, 10.0F, 0.0F});
  const auto events = hero_audio::detect_causal_onsets(
      frames, hero_audio::CausalOnsetConfig{.threshold_history_frames = 2});
  return events.size() == 1 && events[0].frame_index == 3 && near(events[0].threshold, 0.0);
}

bool test_plateau_selects_last_frame() {
  const auto events =
      hero_audio::detect_causal_onsets(make_frames({0.0F, 0.0F, 10.0F, 10.0F, 0.0F}));
  return events.size() == 1 && events[0].frame_index == 3;
}

bool test_refractory_keeps_first_confirmed_peak() {
  const auto events = hero_audio::detect_causal_onsets(
      make_frames({0.0F, 0.0F, 10.0F, 0.0F, 12.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 20.0F, 0.0F}));
  return events.size() == 2 && events[0].frame_index == 2 && events[1].frame_index == 10;
}

bool test_offline_equals_streaming() {
  const auto frames = make_frames({0.0F, 0.0F, 8.0F, 0.0F, 0.0F, 0.0F, 15.0F, 1.0F, 0.0F});
  const auto offline = hero_audio::detect_causal_onsets(frames);
  hero_audio::CausalOnsetDetector detector;
  std::vector<hero_audio::OnsetEvent> streaming;
  for (const auto &frame : frames) {
    if (auto event = detector.process(frame); event.has_value()) {
      streaming.push_back(*event);
    }
  }
  if (offline.size() != streaming.size()) {
    return false;
  }
  for (std::size_t index = 0; index < offline.size(); ++index) {
    if (offline[index].frame_index != streaming[index].frame_index ||
        !near(offline[index].onset_time_seconds, streaming[index].onset_time_seconds) ||
        !near(offline[index].threshold, streaming[index].threshold)) {
      return false;
    }
  }
  return true;
}

bool test_impulse_audio_end_to_end() {
  std::vector<float> samples(4096, 0.0F);
  samples[1536] = 1.0F;
  samples[3072] = 1.0F;
  auto fft = hero_audio::make_fft_backend(hero_audio::FFTBackendKind::Reference, 1024);
  const auto flux = hero_audio::compute_spectral_flux(samples, 48000, *fft);
  const auto events = hero_audio::detect_causal_onsets(flux);
  return events.size() == 2 && near(events[0].onset_time_seconds, 1536.0 / kSampleRate) &&
         near(events[1].onset_time_seconds, 3072.0 / kSampleRate) &&
         near(events[0].algorithm_delay_ms, 16.0) && near(events[1].algorithm_delay_ms, 16.0);
}

bool test_reset_restarts_state() {
  const auto frames = make_frames({0.0F, 10.0F, 0.0F});
  hero_audio::CausalOnsetDetector detector;
  for (const auto &frame : frames) {
    static_cast<void>(detector.process(frame));
  }
  detector.reset();
  std::optional<hero_audio::OnsetEvent> event;
  for (const auto &frame : frames) {
    event = detector.process(frame);
  }
  return event.has_value() && event->frame_index == 1;
}

bool test_end_of_stream_does_not_flush_candidate() {
  return hero_audio::detect_causal_onsets(make_frames({0.0F, 10.0F})).empty();
}

bool test_rejects_nonconsecutive_frames() {
  auto frames = make_frames({0.0F, 1.0F});
  frames[1].frame_index = 2;
  hero_audio::CausalOnsetDetector detector;
  static_cast<void>(detector.process(frames[0]));
  try {
    static_cast<void>(detector.process(frames[1]));
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

bool test_rejects_invalid_input() {
  try {
    static_cast<void>(hero_audio::CausalOnsetDetector(
        hero_audio::CausalOnsetConfig{.threshold_history_frames = 0}));
    return false;
  } catch (const std::invalid_argument &) {
  }

  auto frames = make_frames({0.0F, 1.0F});
  frames[1].spectral_flux = std::numeric_limits<float>::quiet_NaN();
  hero_audio::CausalOnsetDetector detector;
  static_cast<void>(detector.process(frames[0]));
  try {
    static_cast<void>(detector.process(frames[1]));
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

bool test_csv_output() {
  const std::vector<hero_audio::OnsetEvent> events{{
      .frame_index = 3,
      .onset_time_seconds = 0.0266666667,
      .emitted_at_seconds = 0.0426666667,
      .algorithm_delay_ms = 16.0,
      .spectral_flux = 10.0F,
      .threshold = 0.0,
  }};
  std::ostringstream output;
  hero_audio::write_onsets_csv(output, events);
  const std::string expected =
      "frame_index,onset_time_seconds,emitted_at_seconds,algorithm_delay_ms,"
      "spectral_flux,threshold\n"
      "3,0.026666667,0.042666667,16.000000000,10.000000000,0.000000000\n";
  return output.str() == expected;
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, bool (*)()>> tests{
      {"silence", test_silence_produces_no_onset},
      {"one-frame confirmation", test_one_frame_confirmation_and_causal_threshold},
      {"nonzero threshold formula", test_nonzero_threshold_formula},
      {"history capacity", test_history_capacity_discards_old_frames},
      {"plateau", test_plateau_selects_last_frame},
      {"refractory", test_refractory_keeps_first_confirmed_peak},
      {"offline equals streaming", test_offline_equals_streaming},
      {"impulse audio end to end", test_impulse_audio_end_to_end},
      {"reset", test_reset_restarts_state},
      {"end-of-stream candidate", test_end_of_stream_does_not_flush_candidate},
      {"nonconsecutive frames", test_rejects_nonconsecutive_frames},
      {"invalid input", test_rejects_invalid_input},
      {"CSV output", test_csv_output},
  };
  for (const auto &[name, test] : tests) {
    if (!test()) {
      std::cerr << "Causal onset detector test failed: " << name << '\n';
      return 1;
    }
  }
  std::cout << "Causal onset detector tests passed\n";
  return 0;
}

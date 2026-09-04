#include "hero_audio/acoustic_features.hpp"
#include "hero_audio/anomaly_detector.hpp"
#include "hero_audio/anomaly_output.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool near(double actual, double expected, double tolerance = 1.0e-9) {
  return std::abs(actual - expected) <= tolerance;
}

hero_audio::SpectralFluxFrame flux_frame(std::size_t index, float flux) {
  const double start = static_cast<double>(index) * 0.5;
  return hero_audio::SpectralFluxFrame{
      .frame_index = index,
      .frame_start_sample = index * 2,
      .frame_start_seconds = start,
      .frame_center_seconds = start + 0.5,
      .available_seconds = start + 1.0,
      .spectral_flux = flux,
  };
}

hero_audio::AcousticFeatureFrame feature(std::size_t index, float flux,
                                         double rms, double peak,
                                         double zero_crossing = 0.1) {
  const double center = static_cast<double>(index) * 0.1 + 0.05;
  return hero_audio::AcousticFeatureFrame{
      .frame_index = index,
      .frame_center_seconds = center,
      .available_seconds = center + 0.05,
      .spectral_flux = flux,
      .frame_rms = rms,
      .peak_absolute = peak,
      .zero_crossing_rate = zero_crossing,
  };
}

hero_audio::OnsetEvent onset_for(std::size_t frame_index) {
  const double onset_time = static_cast<double>(frame_index) * 0.1 + 0.05;
  return hero_audio::OnsetEvent{
      .frame_index = frame_index,
      .onset_time_seconds = onset_time,
      .emitted_at_seconds = onset_time + 0.15,
      .algorithm_delay_ms = 150.0,
      .spectral_flux = 10.0F,
      .threshold = 2.0,
  };
}

hero_audio::TransientAnomalyConfig small_config() {
  return hero_audio::TransientAnomalyConfig{
      .sample_rate_hz = 10,
      .hop_size_samples = 1,
      .baseline_seconds = 0.3,
      .flux_z_threshold = 6.0,
      .rms_z_threshold = 6.0,
      .peak_z_threshold = 6.0,
      .anomaly_refractory_seconds = 0.5,
  };
}

std::vector<std::string> csv_fields(std::string_view line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto comma = line.find(',', start);
    const auto end = comma == std::string_view::npos ? line.size() : comma;
    fields.emplace_back(line.substr(start, end - start));
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  if (!fields.empty() && !fields.back().empty() && fields.back().back() == '\n') {
    fields.back().pop_back();
  }
  return fields;
}

bool test_feature_extractor_uses_matching_frame() {
  hero_audio::AcousticFeatureExtractor extractor(
      hero_audio::AcousticFeatureExtractorConfig{
          .frame_size_samples = 4,
          .hop_size_samples = 2,
      });
  const std::array<float, 2> hop{1.0F, -1.0F};
  const hero_audio::StreamingHopResult first{
      .input_hop_index = 0,
      .total_samples_received = 2,
  };
  if (extractor.process_hop(hop, first).has_value()) {
    return false;
  }
  const hero_audio::StreamingHopResult second{
      .input_hop_index = 1,
      .total_samples_received = 4,
      .flux_frame = flux_frame(0, 3.0F),
  };
  const auto result = extractor.process_hop(hop, second);
  if (!result.has_value() || result->frame_index != 0 ||
      !near(result->spectral_flux, 3.0) || !near(result->frame_rms, 1.0) ||
      !near(result->peak_absolute, 1.0) ||
      !near(result->zero_crossing_rate, 1.0)) {
    return false;
  }

  extractor.reset();
  try {
    const std::array<float, 2> invalid{
        0.0F, std::numeric_limits<float>::quiet_NaN()};
    static_cast<void>(extractor.process_hop(invalid, first));
    return false;
  } catch (const std::invalid_argument &) {
  }
  return true;
}

bool test_fixed_baseline_and_scored_onset() {
  hero_audio::TransientAnomalyDetector detector(small_config());
  const auto first = detector.process(feature(0, 1.0F, 1.0, 1.0), std::nullopt,
                                      hero_audio::OperatingState::Steady);
  const auto second = detector.process(feature(1, 2.0F, 1.1, 1.2), std::nullopt,
                                       hero_audio::OperatingState::Steady);
  const auto third = detector.process(feature(2, 1.0F, 0.9, 0.8), std::nullopt,
                                      hero_audio::OperatingState::Steady);
  if (first.phase != hero_audio::AnomalyPhase::Calibrating ||
      second.phase != hero_audio::AnomalyPhase::Calibrating ||
      third.phase != hero_audio::AnomalyPhase::Calibrating ||
      !near(third.baseline_progress, 1.0) || !detector.baseline().has_value()) {
    return false;
  }
  const auto baseline_before = *detector.baseline();

  const auto anomalous = detector.process(feature(3, 10.0F, 3.0, 4.0), std::nullopt,
                                          hero_audio::OperatingState::Steady);
  if (anomalous.phase != hero_audio::AnomalyPhase::Monitoring ||
      !anomalous.current_z_scores.has_value() ||
      anomalous.scored_event.has_value()) {
    return false; // A loud frame alone is not an anomaly without a confirmed onset.
  }
  const auto emitted = detector.process(feature(4, 1.0F, 1.0, 1.0), onset_for(3),
                                        hero_audio::OperatingState::Steady);
  if (!emitted.scored_event.has_value() ||
      !emitted.scored_event->above_threshold ||
      !emitted.scored_event->emitted_anomaly ||
      emitted.scored_event->score < 1.0 || detector.scored_event_count() != 1 ||
      detector.anomaly_candidate_count() != 1 ||
      detector.emitted_anomaly_count() != 1) {
    return false;
  }

  // A second high-scoring event is audited but suppressed during refractory.
  static_cast<void>(detector.process(feature(5, 10.0F, 3.0, 4.0), std::nullopt,
                                     hero_audio::OperatingState::Steady));
  const auto suppressed = detector.process(feature(6, 1.0F, 1.0, 1.0), onset_for(5),
                                           hero_audio::OperatingState::Steady);
  if (!suppressed.scored_event.has_value() ||
      !suppressed.scored_event->above_threshold ||
      !suppressed.scored_event->suppressed_by_refractory ||
      suppressed.scored_event->emitted_anomaly ||
      detector.emitted_anomaly_count() != 1) {
    return false;
  }

  // Monitoring samples do not adapt the frozen normal baseline.
  return near(detector.baseline()->spectral_flux.median,
              baseline_before.spectral_flux.median) &&
         near(detector.baseline()->frame_rms.scale,
              baseline_before.frame_rms.scale);
}

bool test_operating_state_gate_and_discontinuity() {
  hero_audio::TransientAnomalyDetector detector(small_config());
  const auto gated = detector.process(feature(0, 50.0F, 10.0, 10.0), std::nullopt,
                                      hero_audio::OperatingState::Startup);
  if (gated.phase != hero_audio::AnomalyPhase::Gated ||
      detector.baseline_frames_collected() != 0) {
    return false;
  }
  static_cast<void>(detector.process(feature(1, 1.0F, 1.0, 1.0), std::nullopt,
                                     hero_audio::OperatingState::Steady));
  const auto left_steady = detector.process(
      feature(2, 1.0F, 1.0, 1.0), std::nullopt,
      hero_audio::OperatingState::Startup);
  if (left_steady.phase != hero_audio::AnomalyPhase::Gated ||
      detector.baseline_frames_collected() != 0) {
    return false;
  }

  static_cast<void>(detector.process(feature(3, 1.0F, 1.0, 1.0), std::nullopt,
                                     hero_audio::OperatingState::Steady));
  if (!detector.handle_discontinuity() ||
      detector.baseline_frames_collected() != 0) {
    return false;
  }

  // A reset segment starts at frame zero and must collect a fresh continuous
  // verified-normal interval because the prior calibration was incomplete.
  static_cast<void>(detector.process(feature(0, 1.0F, 1.0, 1.0), std::nullopt,
                                     hero_audio::OperatingState::Steady));
  static_cast<void>(detector.process(feature(1, 1.0F, 1.0, 1.0), std::nullopt,
                                     hero_audio::OperatingState::Steady));
  static_cast<void>(detector.process(feature(2, 1.0F, 1.0, 1.0), std::nullopt,
                                     hero_audio::OperatingState::Steady));
  if (!detector.baseline().has_value() || detector.handle_discontinuity()) {
    return false;
  }
  const auto monitoring = detector.process(feature(0, 1.0F, 1.0, 1.0), std::nullopt,
                                           hero_audio::OperatingState::Steady);
  return monitoring.phase == hero_audio::AnomalyPhase::Monitoring &&
         detector.baseline().has_value();
}

bool test_invalid_configuration_and_order() {
  try {
    auto config = small_config();
    config.flux_z_threshold = 0.0;
    hero_audio::TransientAnomalyDetector invalid(config);
    return false;
  } catch (const std::invalid_argument &) {
  }

  hero_audio::TransientAnomalyDetector detector(small_config());
  static_cast<void>(detector.process(feature(0, 1.0F, 1.0, 1.0), std::nullopt,
                                     hero_audio::OperatingState::Steady));
  try {
    static_cast<void>(detector.process(feature(2, 1.0F, 1.0, 1.0), std::nullopt,
                                       hero_audio::OperatingState::Steady));
    return false;
  } catch (const std::invalid_argument &) {
  }
  return hero_audio::parse_operating_state("steady") ==
             hero_audio::OperatingState::Steady &&
         hero_audio::to_string(hero_audio::AnomalyReason::FrameRms) == "frame_rms";
}

bool test_anomaly_csv_column_alignment() {
  const auto features = feature(3, 10.0F, 3.0, 4.0, 0.25);
  const hero_audio::AcousticFeatureZScores z{
      .spectral_flux = 9.0,
      .frame_rms = 8.0,
      .peak_absolute = 7.0,
      .zero_crossing_rate = 1.0,
  };
  const hero_audio::TransientEventScore event{
      .frame_index = 3,
      .onset_time_seconds = 0.35,
      .emitted_at_seconds = 0.50,
      .score = 1.5,
      .primary_reason = hero_audio::AnomalyReason::SpectralFlux,
      .above_threshold = true,
      .suppressed_by_refractory = false,
      .emitted_anomaly = true,
      .features = features,
      .z_scores = z,
  };
  const hero_audio::TransientAnomalyFrameResult result{
      .phase = hero_audio::AnomalyPhase::Monitoring,
      .baseline_frames_collected = 3,
      .baseline_frames_required = 3,
      .baseline_progress = 1.0,
      .current_z_scores = z,
      .scored_event = event,
  };

  std::ostringstream frame_header;
  std::ostringstream frame_row;
  hero_audio::write_anomaly_frame_csv_header(frame_header);
  hero_audio::write_anomaly_frame_csv_row(
      frame_row, 10, 2, 5.0, hero_audio::OperatingState::Steady, features,
      result, 0.02);
  const auto header_fields = csv_fields(frame_header.str());
  const auto row_fields = csv_fields(frame_row.str());
  if (header_fields.size() != 26 || row_fields.size() != header_fields.size() ||
      row_fields[5] != "steady" || row_fields[6] != "monitoring" ||
      row_fields[18] != "3" || row_fields[19] != "1" ||
      row_fields[21] != "spectral_flux") {
    return false;
  }

  std::ostringstream event_header;
  std::ostringstream event_row;
  hero_audio::write_anomaly_event_csv_header(event_header);
  hero_audio::write_anomaly_event_csv_row(
      event_row, 10, 2, 5.0, hero_audio::OperatingState::Steady, event, 151.0);
  const auto event_header_fields = csv_fields(event_header.str());
  const auto event_row_fields = csv_fields(event_row.str());
  return event_header_fields.size() == 18 &&
         event_row_fields.size() == event_header_fields.size() &&
         event_row_fields[2] == "unexpected_transient" &&
         event_row_fields[3] == "steady" && event_row_fields[9] == "spectral_flux";
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, bool (*)()>> tests{
      {"feature extractor matching frame", test_feature_extractor_uses_matching_frame},
      {"fixed baseline and scored onset", test_fixed_baseline_and_scored_onset},
      {"operating state gate and discontinuity",
       test_operating_state_gate_and_discontinuity},
      {"invalid configuration and order", test_invalid_configuration_and_order},
      {"anomaly CSV column alignment", test_anomaly_csv_column_alignment},
  };
  for (const auto &[name, test] : tests) {
    if (!test()) {
      std::cerr << "Anomaly detector test failed: " << name << '\n';
      return 1;
    }
  }
  std::cout << "Anomaly detector tests passed\n";
  return 0;
}

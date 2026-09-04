#include "hero_audio/anomaly_output.hpp"

#include <iomanip>
#include <optional>
#include <ostream>

namespace hero_audio {
namespace {

void write_optional_number(std::ostream &output,
                           const std::optional<double> &value) {
  if (value.has_value()) {
    output << *value;
  }
}

} // namespace

void write_anomaly_frame_csv_header(std::ostream &output) {
  output << "sequence,segment_index,frame_index,frame_center_seconds,"
            "available_seconds,operating_state,phase,baseline_frames_collected,"
            "baseline_frames_required,baseline_progress,spectral_flux,frame_rms,"
            "peak_absolute,zero_crossing_rate,flux_positive_z,rms_positive_z,"
            "peak_positive_z,zero_crossing_positive_z,scored_onset_frame_index,"
            "onset_scored,anomaly_score,primary_reason,above_threshold,"
            "suppressed_by_refractory,anomaly_emitted,anomaly_analysis_ms\n";
}

void write_anomaly_frame_csv_row(
    std::ostream &output, std::uint64_t sequence, std::size_t segment_index,
    double segment_offset_seconds, OperatingState operating_state,
    const AcousticFeatureFrame &features,
    const TransientAnomalyFrameResult &result, double anomaly_analysis_ms) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  std::optional<double> flux_z;
  std::optional<double> rms_z;
  std::optional<double> peak_z;
  std::optional<double> zero_crossing_z;
  if (result.current_z_scores.has_value()) {
    flux_z = result.current_z_scores->spectral_flux;
    rms_z = result.current_z_scores->frame_rms;
    peak_z = result.current_z_scores->peak_absolute;
    zero_crossing_z = result.current_z_scores->zero_crossing_rate;
  }

  output << std::fixed << std::setprecision(9) << sequence << ',' << segment_index
         << ',' << features.frame_index << ','
         << segment_offset_seconds + features.frame_center_seconds << ','
         << segment_offset_seconds + features.available_seconds << ','
         << to_string(operating_state) << ',' << to_string(result.phase) << ','
         << result.baseline_frames_collected << ','
         << result.baseline_frames_required << ',' << result.baseline_progress << ','
         << features.spectral_flux << ',' << features.frame_rms << ','
         << features.peak_absolute << ',' << features.zero_crossing_rate << ',';
  write_optional_number(output, flux_z);
  output << ',';
  write_optional_number(output, rms_z);
  output << ',';
  write_optional_number(output, peak_z);
  output << ',';
  write_optional_number(output, zero_crossing_z);
  output << ',';
  if (result.scored_event.has_value()) {
    output << result.scored_event->frame_index;
  }
  output << ',' << (result.scored_event.has_value() ? 1 : 0) << ',';
  if (result.scored_event.has_value()) {
    const auto &event = *result.scored_event;
    output << event.score << ',' << to_string(event.primary_reason) << ','
           << (event.above_threshold ? 1 : 0) << ','
           << (event.suppressed_by_refractory ? 1 : 0) << ','
           << (event.emitted_anomaly ? 1 : 0);
  } else {
    output << ",,,,";
  }
  output << ',' << anomaly_analysis_ms << '\n';
  output.flags(old_flags);
  output.precision(old_precision);
}

void write_anomaly_event_csv_header(std::ostream &output) {
  output << "sequence,segment_index,class,operating_state,frame_index,"
            "onset_time_seconds,emitted_at_audio_seconds,"
            "estimated_software_detection_delay_ms,anomaly_score,primary_reason,"
            "spectral_flux,frame_rms,peak_absolute,zero_crossing_rate,"
            "flux_positive_z,rms_positive_z,peak_positive_z,"
            "zero_crossing_positive_z\n";
}

void write_anomaly_event_csv_row(
    std::ostream &output, std::uint64_t sequence, std::size_t segment_index,
    double segment_offset_seconds, OperatingState operating_state,
    const TransientEventScore &event,
    double estimated_software_detection_delay_ms) {
  const auto old_flags = output.flags();
  const auto old_precision = output.precision();
  output << std::fixed << std::setprecision(9) << sequence << ',' << segment_index
         << ",unexpected_transient," << to_string(operating_state) << ','
         << event.frame_index << ','
         << segment_offset_seconds + event.onset_time_seconds << ','
         << segment_offset_seconds + event.emitted_at_seconds << ','
         << estimated_software_detection_delay_ms << ',' << event.score << ','
         << to_string(event.primary_reason) << ',' << event.features.spectral_flux << ','
         << event.features.frame_rms << ',' << event.features.peak_absolute << ','
         << event.features.zero_crossing_rate << ','
         << event.z_scores.spectral_flux << ',' << event.z_scores.frame_rms << ','
         << event.z_scores.peak_absolute << ','
         << event.z_scores.zero_crossing_rate << '\n';
  output.flags(old_flags);
  output.precision(old_precision);
}

} // namespace hero_audio

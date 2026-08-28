#pragma once

#include "hero_audio/anomaly_detector.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace hero_audio {

// CSV serialization lives outside the live executable so column ordering can
// be unit-tested without opening a microphone device.
void write_anomaly_frame_csv_header(std::ostream &output);
void write_anomaly_frame_csv_row(
    std::ostream &output, std::uint64_t sequence, std::size_t segment_index,
    double segment_offset_seconds, OperatingState operating_state,
    const AcousticFeatureFrame &features,
    const TransientAnomalyFrameResult &result, double anomaly_analysis_ms);

void write_anomaly_event_csv_header(std::ostream &output);
void write_anomaly_event_csv_row(
    std::ostream &output, std::uint64_t sequence, std::size_t segment_index,
    double segment_offset_seconds, OperatingState operating_state,
    const TransientEventScore &event,
    double estimated_software_detection_delay_ms);

} // namespace hero_audio

#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="${1:-${repo_dir}/data/local/anomaly-session-$(date -u +%Y%m%dT%H%M%SZ)}"
duration_seconds="${2:-60}"
baseline_seconds="${3:-30}"
live_binary="${repo_dir}/build/macos-arm64-release/hero-audio-live"

echo "HERO-Audio steady-state transient anomaly session"
echo "Output: ${output_dir}"
echo "Total duration: ${duration_seconds} seconds"
echo "Verified-normal baseline: first ${baseline_seconds} seconds"
echo
echo "Protocol:"
echo "  1. Keep the microphone and sound source fixed."
echo "  2. During baseline, keep the system in verified normal steady operation."
echo "  3. Wait for: anomaly_baseline_complete: monitoring has started"
echo "  4. Only after that message, introduce the pre-defined test events."
echo "  5. Write the real event times into human-labels.csv after listening."
echo
read -r -p "Press Enter when the normal steady condition is ready... "

(
  cd "${repo_dir}"
  cmake --preset macos-arm64-release
  cmake --build --preset macos-arm64-release
)

set +e
"${live_binary}" "${output_dir}" \
  --seconds "${duration_seconds}" \
  --backend fftw \
  --operating-state steady \
  --anomaly-baseline-seconds "${baseline_seconds}"
capture_status=$?
set -e

if [[ -d "${output_dir}" && ! -e "${output_dir}/human-labels.csv" ]]; then
  printf '%s\n' \
    'start_seconds,end_seconds,class,operating_state,confidence,annotator' \
    > "${output_dir}/human-labels.csv"
fi

echo
echo "Do not copy anomaly-events.csv into human-labels.csv."
echo "Listen to capture.wav and annotate independently."
echo "Valid v1 labels: normal_background, normal_transition, anomaly_impact, anomaly_burst."
exit "${capture_status}"

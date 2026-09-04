#!/usr/bin/env bash
set -euo pipefail

# Interactively collect five self-recorded, independent CoreAudio sessions.
# Audio stays under data/local/, which is ignored to prevent accidental upload.
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
duration_seconds="${1:-10}"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
collection_dir="${repo_dir}/data/local/live-five-${timestamp}"
manifest_path="${collection_dir}/manifest.csv"
live_binary="${repo_dir}/build/macos-arm64-release/hero-audio-live"

scenarios=(
  "quiet-room-hand-claps"
  "desk-or-table-taps"
  "finger-snaps-or-key-clicks"
  "mixed-everyday-room-sound"
  "quiet-room-negative-control"
)
splits=("development" "development" "development" "held-out" "held-out")

cd "${repo_dir}"
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release

mkdir -p "${collection_dir}"
if [[ -e "${manifest_path}" ]]; then
  echo "Refusing to overwrite existing manifest: ${manifest_path}" >&2
  exit 1
fi
printf '%s\n' \
  'recording_id,scenario,split,session_directory,reference_csv,annotation_status' \
  > "${manifest_path}"

for index in 0 1 2 3 4; do
  number="$((index + 1))"
  recording_id="$(printf 'live-%02d' "${number}")"
  scenario="${scenarios[index]}"
  split="${splits[index]}"
  session_dir="${collection_dir}/${recording_id}-${scenario}"

  echo
  echo "Recording ${number}/5: ${scenario} (${duration_seconds} seconds)"
  echo "Prepare the sound, then press Return. Use Ctrl-C to stop the collection."
  read -r

  "${live_binary}" "${session_dir}" \
    --seconds "${duration_seconds}" \
    --backend fftw

  # Human annotations remain deliberately separate from model predictions.
  printf '%s\n' 'onset_time_seconds' > "${session_dir}/references.csv"
  printf '%s,%s,%s,%s,%s,%s\n' \
    "${recording_id}" "${scenario}" "${split}" "${session_dir}" \
    "${session_dir}/references.csv" "pending" >> "${manifest_path}"
done

echo
echo "Five-session collection complete: ${collection_dir}"
echo "Next: listen to each capture.wav and enter human onset times in references.csv."

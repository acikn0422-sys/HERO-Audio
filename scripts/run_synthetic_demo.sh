#!/usr/bin/env bash
set -euo pipefail

# Recreate the complete first-stage evidence chain without editing source data.
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
raw_dir="results/raw/synthetic-demo"
processed_dir="results/processed/synthetic-demo"
python_bin="${repo_dir}/.venv/bin/python"

if [[ ! -x "${python_bin}" ]]; then
  echo "Analysis environment missing. Run: ./scripts/bootstrap_analysis.sh" >&2
  exit 1
fi

cd "${repo_dir}"
mkdir -p "${raw_dir}" "${processed_dir}"

# The release preset requires FFTW3f, preventing accidental publication of
# reference-radix2 performance as the official CPU baseline.
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release

"${python_bin}" "${repo_dir}/scripts/generate_synthetic_clicks.py" \
  "${raw_dir}/synthetic-clicks.wav" \
  "${raw_dir}/references.csv"

"${repo_dir}/build/macos-arm64-release/hero-audio" \
  "${raw_dir}/synthetic-clicks.wav" \
  "${raw_dir}/spectral-flux.csv" \
  "${raw_dir}/onsets.csv" \
  "${raw_dir}/diagnostics.csv"

"${repo_dir}/build/macos-arm64-release/hero-audio-eval" \
  "${raw_dir}/onsets.csv" \
  "${raw_dir}/references.csv" \
  --matches "${raw_dir}/matches.csv" \
  --metrics "${processed_dir}/metrics.json" \
  --tolerance-ms 50

"${repo_dir}/build/macos-arm64-release/hero-audio-bench" \
  "${raw_dir}/synthetic-clicks.wav" \
  "${raw_dir}/offline-benchmark-runs.csv" \
  "${processed_dir}/offline-benchmark-summary.json" \
  --warmup 3 \
  --runs 5 \
  --backend fftw

"${repo_dir}/build/macos-arm64-release/hero-audio-stream" \
  "${raw_dir}/synthetic-clicks.wav" \
  "${raw_dir}/streaming-hop-measurements.csv" \
  "${processed_dir}/streaming-benchmark-summary.json" \
  --warmup-passes 1 \
  --passes 5 \
  --backend fftw

"${python_bin}" "${repo_dir}/scripts/plot_onset_diagnostics.py" \
  "${raw_dir}/diagnostics.csv" \
  "${raw_dir}/onsets.csv" \
  "${processed_dir}/spectral-flux-threshold-onsets.png" \
  --references "${raw_dir}/references.csv" \
  --title "HERO-Audio: Synthetic Click Onset Detection"

echo "Raw evidence: ${repo_dir}/${raw_dir}"
echo "Processed outputs: ${repo_dir}/${processed_dir}"

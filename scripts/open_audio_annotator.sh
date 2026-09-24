#!/usr/bin/env bash
set -euo pipefail

# Opens the local tool, not a hosted service. Audio is selected by the user in
# the browser; this script never reads a recording or starts the microphone.
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
page="${repo_dir}/tools/audio-annotator/index.html"
if [[ ! -f "${page}" ]]; then
  echo "Annotation page is missing: ${page}" >&2
  exit 1
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Open this file in a browser: ${page}"
  exit 0
fi
if [[ -d "/Applications/Google Chrome.app" ]]; then
  /usr/bin/open -a "/Applications/Google Chrome.app" "${page}"
else
  /usr/bin/open "${page}"
fi
echo "Opened the local annotation tool. Select your PCM16 listening WAV in the page."
echo "No audio or labels are uploaded. Save a draft before closing the page."

#!/usr/bin/env bash
set -euo pipefail

# Keep Python packages out of the system interpreter and inside the repository.
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -x /opt/homebrew/bin/python3 ]]; then
  analysis_python="/opt/homebrew/bin/python3"
else
  analysis_python="$(command -v python3)"
fi

if ! "${analysis_python}" -c 'import sys; raise SystemExit(sys.version_info < (3, 11))'; then
  echo "Python 3.11 or newer is required. Run: brew bundle" >&2
  exit 1
fi

"${analysis_python}" -m venv "${repo_dir}/.venv"
"${repo_dir}/.venv/bin/python" -m pip install \
  --requirement "${repo_dir}/requirements-analysis.txt"

echo "Analysis environment ready: ${repo_dir}/.venv"

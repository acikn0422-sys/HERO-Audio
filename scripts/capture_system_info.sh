#!/usr/bin/env bash
set -euo pipefail

echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "git_commit=$(git rev-parse --verify HEAD 2>/dev/null || echo uncommitted)"
echo "os=$(sw_vers -productName 2>/dev/null || uname -s)"
echo "os_version=$(sw_vers -productVersion 2>/dev/null || uname -r)"
echo "architecture=$(uname -m)"
hardware_overview="$(system_profiler SPHardwareDataType 2>/dev/null || true)"
chip_fallback="$(awk -F ': ' '/^[[:space:]]*Chip:/{print $2; exit}' <<<"${hardware_overview}")"
cores_fallback="$(awk -F ': ' '/^[[:space:]]*Total Number of Cores:/{print $2; exit}' <<<"${hardware_overview}")"
memory_fallback="$(awk -F ': ' '/^[[:space:]]*Memory:/{print $2; exit}' <<<"${hardware_overview}")"
echo "chip=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "${chip_fallback:-unknown}")"
echo "physical_cpu=$(sysctl -n hw.physicalcpu 2>/dev/null || echo "${cores_fallback:-unknown}")"
echo "logical_cpu=$(sysctl -n hw.logicalcpu 2>/dev/null || echo "${cores_fallback:-unknown}")"
echo "memory=$(sysctl -n hw.memsize 2>/dev/null || echo "${memory_fallback:-unknown}")"
echo "compiler=$(c++ --version 2>/dev/null | head -n 1 || echo unavailable)"
echo "cmake=$(cmake --version 2>/dev/null | head -n 1 || echo unavailable)"
echo "fftw3f=$(pkg-config --modversion fftw3f 2>/dev/null || echo unavailable)"

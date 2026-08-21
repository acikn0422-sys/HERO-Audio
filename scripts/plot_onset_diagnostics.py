#!/usr/bin/env python3
"""Plot Spectral Flux, causal threshold, detected onsets, and references."""

from __future__ import annotations

import argparse
import csv
import math
import os
from pathlib import Path
from typing import Iterable


def require_columns(fieldnames: Iterable[str] | None, required: set[str], path: Path) -> None:
    available = set(fieldnames or [])
    missing = sorted(required - available)
    if missing:
        raise ValueError(f"{path} is missing CSV columns: {', '.join(missing)}")


def read_diagnostics(path: Path) -> tuple[list[float], list[float], list[float]]:
    times: list[float] = []
    flux: list[float] = []
    threshold: list[float] = []
    with path.open(newline="", encoding="utf-8") as source:
        rows = csv.DictReader(source)
        require_columns(
            rows.fieldnames,
            {"frame_center_seconds", "spectral_flux", "causal_threshold"},
            path,
        )
        for row in rows:
            times.append(float(row["frame_center_seconds"]))
            flux.append(float(row["spectral_flux"]))
            threshold_text = row["causal_threshold"].strip()
            threshold.append(float(threshold_text) if threshold_text else math.nan)
    if not times:
        raise ValueError(f"{path} contains no diagnostic frames")
    return times, flux, threshold


def read_detected_onsets(path: Path) -> tuple[list[float], list[float]]:
    times: list[float] = []
    flux: list[float] = []
    with path.open(newline="", encoding="utf-8") as source:
        rows = csv.DictReader(source)
        require_columns(rows.fieldnames, {"onset_time_seconds", "spectral_flux"}, path)
        for row in rows:
            times.append(float(row["onset_time_seconds"]))
            flux.append(float(row["spectral_flux"]))
    return times, flux


def read_reference_onsets(path: Path) -> list[float]:
    """Accept the same headered or one-value-per-line formats as the C++ evaluator."""

    with path.open(newline="", encoding="utf-8") as source:
        nonempty = [line.strip() for line in source if line.strip()]
    if not nonempty:
        return []
    if nonempty[0].split(",")[0].strip() == "onset_time_seconds":
        with path.open(newline="", encoding="utf-8") as source:
            rows = csv.DictReader(source)
            require_columns(rows.fieldnames, {"onset_time_seconds"}, path)
            return [float(row["onset_time_seconds"]) for row in rows]
    return [float(line) for line in nonempty]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("diagnostics_csv", type=Path)
    parser.add_argument("onsets_csv", type=Path)
    parser.add_argument("output_image", type=Path)
    parser.add_argument("--references", type=Path)
    parser.add_argument("--title", default="HERO-Audio Causal Onset Detection")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    times, flux, threshold = read_diagnostics(arguments.diagnostics_csv)
    onset_times, onset_flux = read_detected_onsets(arguments.onsets_csv)
    reference_times = (
        read_reference_onsets(arguments.references) if arguments.references else []
    )

    # Keep Matplotlib's font/config cache inside the ignored repository cache;
    # this avoids writing user-global state and works inside sandboxed runners.
    repository_root = Path(__file__).resolve().parent.parent
    os.environ.setdefault("MPLCONFIGDIR", str(repository_root / ".cache" / "matplotlib"))

    # Select a non-interactive backend before importing pyplot so the script is
    # deterministic in Terminal, CI, and machines without a display server.
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    figure, axis = plt.subplots(figsize=(12, 5.5))
    axis.plot(times, flux, color="#1769aa", linewidth=1.25, label="Spectral Flux")
    axis.plot(times, threshold, color="#ef6c00", linewidth=1.2, label="Causal Threshold")
    if onset_times:
        axis.scatter(
            onset_times,
            onset_flux,
            color="#c62828",
            marker="v",
            s=55,
            zorder=4,
            label="Detected Onset",
        )
    for index, onset_time in enumerate(reference_times):
        axis.axvline(
            onset_time,
            color="#2e7d32",
            linestyle="--",
            linewidth=0.9,
            alpha=0.65,
            label="Reference Onset" if index == 0 else None,
        )

    axis.set_title(arguments.title)
    axis.set_xlabel("Time (seconds)")
    axis.set_ylabel("Spectral Flux (unnormalized magnitude difference)")
    axis.set_xlim(left=0.0, right=max(times))
    axis.set_ylim(bottom=0.0)
    axis.grid(True, alpha=0.22)
    axis.legend(loc="upper right")
    figure.tight_layout()

    arguments.output_image.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(arguments.output_image, dpi=180, bbox_inches="tight")
    plt.close(figure)
    print(f"Plot: {arguments.output_image}")
    print(f"Detected onsets: {len(onset_times)}")
    print(f"Reference onsets: {len(reference_times)}")
    print(f"Matplotlib: {matplotlib.__version__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Generate a deterministic 48 kHz mono WAV and its known onset annotations."""

from __future__ import annotations

import argparse
import array
import csv
import math
from pathlib import Path
import sys
import wave


SAMPLE_RATE_HZ = 48_000
DURATION_SECONDS = 5.0
# Every timestamp is aligned to the 256-sample hop used by baseline.json.
ONSET_TIMES_SECONDS = (0.512, 1.024, 1.536, 2.560, 4.096)


def build_click_track() -> array.array:
    """Return signed PCM16 samples containing five short decaying clicks."""

    sample_count = int(DURATION_SECONDS * SAMPLE_RATE_HZ)
    samples = array.array("h", [0]) * sample_count
    click_length = 96
    for onset_seconds in ONSET_TIMES_SECONDS:
        onset_sample = round(onset_seconds * SAMPLE_RATE_HZ)
        for offset in range(click_length):
            # Alternating polarity gives a broadband transient. The exponential
            # decay avoids a second hard edge at the end of the click.
            envelope = math.exp(-offset / 14.0)
            polarity = 1.0 if offset % 2 == 0 else -1.0
            value = round(0.9 * 32767.0 * envelope * polarity)
            samples[onset_sample + offset] = value
    if sys.byteorder != "little":
        samples.byteswap()
    return samples


def write_wav(path: Path, samples: array.array) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE_HZ)
        output.writeframes(samples.tobytes())


def write_references(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["onset_time_seconds"])
        for onset_seconds in ONSET_TIMES_SECONDS:
            writer.writerow([f"{onset_seconds:.9f}"])


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav_output", type=Path)
    parser.add_argument("reference_output", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    samples = build_click_track()
    write_wav(arguments.wav_output, samples)
    write_references(arguments.reference_output)
    print(f"WAV: {arguments.wav_output}")
    print(f"References: {arguments.reference_output}")
    print(f"Known onsets: {len(ONSET_TIMES_SECONDS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Reference vibration DSP for offline validation and report-quality analysis."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from scipy.fft import rfft, rfftfreq
from scipy.signal import get_window, welch


def _band_energy(frequency: np.ndarray, power: np.ndarray, centre: float, width: float) -> float:
    if centre <= 0:
        return 0.0
    selected = np.abs(frequency - centre) <= width
    return float(np.sum(power[selected]))


def extract_features(samples: list[float] | np.ndarray, sample_rate_hz: float, rpm: float = 0) -> dict:
    """Return time-domain, Hann-window FFT and Welch-PSD motor features.

    Samples are acceleration in g and must be uniformly spaced. The input size
    is deliberately restricted to a power of two to match the live backend.
    """
    signal = np.asarray(samples, dtype=float)
    if signal.ndim != 1 or signal.size < 64 or signal.size & (signal.size - 1):
        raise ValueError("sample count must be a power of two and at least 64")
    if not np.all(np.isfinite(signal)):
        raise ValueError("samples contain non-finite values")
    if not np.isfinite(sample_rate_hz) or sample_rate_hz <= 0:
        raise ValueError("sample_rate_hz must be positive")

    mean_g = float(np.mean(signal))
    centred = signal - mean_g
    rms_g = float(np.sqrt(np.mean(np.square(centred))))
    peak_g = float(np.max(np.abs(centred)))
    window = get_window("hann", signal.size, fftbins=False)
    transform = rfft(centred * window)
    frequency = rfftfreq(signal.size, 1.0 / sample_rate_hz)
    amplitude = np.abs(transform) * 2.0 / np.sum(window)
    amplitude[[0, -1]] /= 2.0
    dominant_index = int(np.argmax(amplitude[1:]) + 1)

    welch_frequency, psd = welch(
        centred,
        fs=sample_rate_hz,
        window="hann",
        nperseg=min(256, signal.size),
        noverlap=min(128, signal.size // 2),
        detrend=False,
        scaling="density",
    )
    shaft_hz = rpm / 60.0 if rpm > 0 else 0.0
    resolution = sample_rate_hz / signal.size

    return {
        "sampleCount": int(signal.size),
        "sampleRateHz": float(sample_rate_hz),
        "binWidthHz": float(resolution),
        "meanG": mean_g,
        "rmsG": rms_g,
        "stdG": float(np.std(centred, ddof=1)),
        "peakG": peak_g,
        "peakToPeakG": float(np.ptp(centred)),
        "crestFactor": peak_g / rms_g if rms_g > 1e-12 else 0.0,
        "dominantHz": float(frequency[dominant_index]),
        "dominantAmplitudeG": float(amplitude[dominant_index]),
        "shaftHz": float(shaft_hz),
        "oneXEnergy": _band_energy(welch_frequency, psd, shaft_hz, max(1.5 * resolution, 1.0)),
        "twoXEnergy": _band_energy(welch_frequency, psd, 2 * shaft_hz, max(1.5 * resolution, 1.0)),
        "spectrum": [
            {"frequencyHz": float(freq), "amplitudeG": float(value)}
            for freq, value in zip(frequency, amplitude, strict=True)
        ],
        "welchPsd": [
            {"frequencyHz": float(freq), "powerG2PerHz": float(value)}
            for freq, value in zip(welch_frequency, psd, strict=True)
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze a motor vibration JSON block")
    parser.add_argument("input", type=Path, help="JSON file with samples, sampleRateHz and optional rpm")
    parser.add_argument("--output", type=Path, help="Write features to this JSON file")
    args = parser.parse_args()
    payload = json.loads(args.input.read_text(encoding="utf-8"))
    result = extract_features(payload["samples"], payload["sampleRateHz"], payload.get("rpm", 0))
    rendered = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)


if __name__ == "__main__":
    main()


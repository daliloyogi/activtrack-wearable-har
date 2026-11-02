"""
Signal helpers shared by the Collect and History tabs.

Sample arrays are (N, 7): [ax, ay, az, gx, gy, gz, node_ms]  (g, dps, ms).
Older Firebase records may only have 6-7 columns without a usable node_ms.
"""

import numpy as np

DEFAULT_FS = 50     # Hz, dipakai jika take tidak menyimpan sample_rate_hz
ROLL_SEC   = 0.5    # jendela rolling std, detik


def roll_win(fs: int) -> int:
    return max(2, round(fs * ROLL_SEC))


def rolling_std(arr: np.ndarray, win: int) -> tuple:
    """Return (std_acc, std_gyr) each shape (N, 3), trailing window of `win` samples.
    Vectorized (cumulative sums) so it is cheap enough to run every frame."""
    n = arr.shape[0]
    if n == 0:
        return np.zeros((0, 3)), np.zeros((0, 3))
    x = arr[:, 0:6].astype(float)
    c1 = np.vstack([np.zeros(6), np.cumsum(x, axis=0)])
    c2 = np.vstack([np.zeros(6), np.cumsum(x * x, axis=0)])
    end   = np.arange(1, n + 1)
    start = np.maximum(0, end - win)
    cnt   = (end - start)[:, None]
    mean  = (c1[end] - c1[start]) / cnt
    var   = (c2[end] - c2[start]) / cnt - mean * mean
    std   = np.sqrt(np.clip(var, 0, None))
    return std[:, 0:3], std[:, 3:6]


def time_axis(arr: np.ndarray, fs: int) -> np.ndarray:
    """Seconds from the first sample. Uses node_ms (col 6) when it is present and
    monotonic, so lost/late packets show their real timing; else index / fs."""
    n = arr.shape[0]
    if arr.ndim == 2 and arr.shape[1] > 6 and n > 1:
        t = arr[:, 6]
        if np.all(np.diff(t) > 0):
            return (t - t[0]) / 1000.0
    return np.arange(n) / (fs or DEFAULT_FS)


def span_seconds(arr: np.ndarray, fs: int) -> float:
    """Duration covered by the samples (first to last + one sample period)."""
    if arr.shape[0] == 0:
        return 0.0
    return float(time_axis(arr, fs)[-1]) + 1.0 / (fs or DEFAULT_FS)

"""
Window features computed ON THE NODE (src/node/features.h) and forwarded by the
ground station as `F,...` lines. This module holds the column names and a Python
reference implementation of the exact same formulas (for checking the firmware and
for recomputing features from raw data in a training pipeline).

Window: FEAT_WIN_SEC seconds, a new window every FEAT_STEP_SEC seconds (see node firmware).
Inputs: accel in g, gyro in deg/s. All std are population std (ddof=0).

  accel_* / gyro_*    : stats of the magnitude |a| = sqrt(ax²+ay²+az²), |g| likewise
                        rms = sqrt(mean(m²)), energy = mean(m²)
  accelX_mean ...     : per-axis mean / std
  standing_indicator  : cos(angle) between the window's mean gravity vector and the axis that
                        points up while standing (STAND_AXIS / STAND_SIGN in the node firmware)
                        1 = upright, 0 = horizontal, -1 = upside down; 0 if |mean| < 0.1 g
  axis_gap_1 / _2     : |mean| of the accel axes sorted m1 >= m2 >= m3 -> m1-m2 / m2-m3
  axis_gap_ratio      : gap_1 / (gap_1 + gap_2), 0..1
"""

import numpy as np

FEAT_VERSION = 1
FEAT_WIN_SEC = 2.0      # harus sama dengan FEAT_WIN_SEC di src/node/main.cpp
FEAT_STEP_SEC = 1.0
MIN_GRAVITY_G = 0.1

FEATURE_COLUMNS = [
    "accel_std", "accel_mean", "accel_max", "accel_min", "accel_range", "accel_rms", "accel_energy",
    "gyro_std", "gyro_mean", "gyro_max", "gyro_min", "gyro_range", "gyro_rms", "gyro_energy",
    "accelX_mean", "accelY_mean", "accelZ_mean", "accelX_std", "accelY_std", "accelZ_std",
    "gyroX_mean", "gyroY_mean", "gyroZ_mean", "gyroX_std", "gyroY_std", "gyroZ_std",
    "standing_indicator", "axis_gap_1", "axis_gap_2", "axis_gap_ratio",
]
FEAT_COUNT = len(FEATURE_COLUMNS)

# rows stored per take / node: window meta (node clock) + features
FEAT_META = ["win", "t_start_ms", "t_end_ms", "n"]
FEAT_ALL_COLUMNS = FEAT_META + FEATURE_COLUMNS
COL_T_START, COL_T_END, COL_N = 1, 2, 3
COL_F0 = len(FEAT_META)


def _mag_stats(m: np.ndarray) -> list:
    mean = m.mean()
    sq = (m * m).mean()
    return [np.sqrt(((m - mean) ** 2).mean()), mean, m.max(), m.min(), m.max() - m.min(),
            np.sqrt(sq), sq]


def compute_window(acc: np.ndarray, gyr: np.ndarray, stand_axis: int = 1,
                   stand_sign: int = -1) -> np.ndarray:
    """acc, gyr: (N, 3) in g and deg/s. Returns the 30 features as float32,
    in FEATURE_COLUMNS order (same as the node)."""
    a = np.asarray(acc, dtype=np.float64)
    g = np.asarray(gyr, dtype=np.float64)
    out = _mag_stats(np.sqrt((a * a).sum(axis=1))) + _mag_stats(np.sqrt((g * g).sum(axis=1)))
    ma, mg = a.mean(axis=0), g.mean(axis=0)
    out += list(ma) + list(np.sqrt(((a - ma) ** 2).mean(axis=0)))
    out += list(mg) + list(np.sqrt(((g - mg) ** 2).mean(axis=0)))

    gnorm = np.sqrt((ma * ma).sum())
    out.append(0.0 if gnorm < MIN_GRAVITY_G else stand_sign * ma[stand_axis] / gnorm)
    m1, m2, m3 = sorted(np.abs(ma), reverse=True)
    gap1, gap2 = m1 - m2, m2 - m3
    out += [gap1, gap2, gap1 / (gap1 + gap2) if gap1 + gap2 > 1e-9 else 0.0]
    return np.array(out, dtype=np.float32)

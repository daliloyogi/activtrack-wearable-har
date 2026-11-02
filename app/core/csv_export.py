"""
Local CSV export. Per node, containing all kept takes of the session in recording order:

  <session>_node<id>_accel.csv    : timestamp,accelX,accelY,accelZ,activityMode
  <session>_node<id>_gyro.csv     : timestamp,gyroX,gyroY,gyroZ,activityMode
  <session>_node<id>_features.csv : ts,<30 node features>,activity_mode   (one row per window)

  timestamp / ts : Unix epoch, ms (PC clock at REC start + node clock offset of the sample);
                   ts of a feature row = its window's last sample
  accel / gyro   : g / deg/s
  activityMode / activity_mode : activity code, see labels.STATE_LABELS
                   (feature window: majority activity of its samples)
  features       : computed on the node (float32), see app/core/features.py
"""

import csv
import os

import numpy as np

from app.core.dsp import time_axis
from app.core.features import COL_F0, COL_T_END, FEATURE_COLUMNS
from app.core.session_manager import COL_ACT, COL_MS

ACCEL_HEADER    = ["timestamp", "accelX", "accelY", "accelZ", "activityMode"]
GYRO_HEADER     = ["timestamp", "gyroX", "gyroY", "gyroZ", "activityMode"]
FEATURES_HEADER = ["ts"] + FEATURE_COLUMNS + ["activity_mode"]


def _t0_ms(take) -> float:
    """Epoch ms of the take's first stored sample (a trimmed take starts at its trim point)."""
    return (take.started_at + (take.trim_s[0] if take.trim_s else 0.0)) * 1000.0


def timestamps_ms(take, arr: np.ndarray) -> np.ndarray:
    """Epoch ms per raw sample."""
    return np.round(_t0_ms(take) + time_axis(arr, take.sample_rate_hz) * 1000).astype(np.int64)


def feature_timestamps_ms(take, node_id: int) -> np.ndarray:
    """Epoch ms per feature window (its last sample), on the same clock as timestamps_ms."""
    arr, feat = take.data[node_id], take.features[node_id]
    return np.round(_t0_ms(take) + feat[:, COL_T_END] - arr[0, COL_MS]).astype(np.int64)


def export_session(session, folder: str) -> list:
    """Write the CSV files into `folder`; returns the written paths.
    The features file is only written for nodes that sent features."""
    takes = sorted(session.kept_takes, key=lambda t: t.started_at)
    nodes = sorted({nid for take in takes for nid in take.data})
    paths = []
    for nid in nodes:
        base = os.path.join(folder, f"{session.session_name}_node{nid}")
        acc_path, gyr_path = base + "_accel.csv", base + "_gyro.csv"
        with open(acc_path, "w", newline="", encoding="utf-8") as fa, \
             open(gyr_path, "w", newline="", encoding="utf-8") as fg:
            wa, wg = csv.writer(fa), csv.writer(fg)
            wa.writerow(ACCEL_HEADER)
            wg.writerow(GYRO_HEADER)
            for take in takes:
                arr = take.data.get(nid)
                if arr is None or arr.shape[0] == 0:
                    continue
                ts = timestamps_ms(take, arr)
                acts = arr[:, COL_ACT].astype(int)
                for i, row in enumerate(arr):
                    wa.writerow([int(ts[i]), round(float(row[0]), 4), round(float(row[1]), 4),
                                 round(float(row[2]), 4), int(acts[i])])
                    wg.writerow([int(ts[i]), round(float(row[3]), 4), round(float(row[4]), 4),
                                 round(float(row[5]), 4), int(acts[i])])
        paths += [acc_path, gyr_path]

        feat_takes = [t for t in takes if nid in t.features and nid in t.data]
        if feat_takes:
            feat_path = base + "_features.csv"
            with open(feat_path, "w", newline="", encoding="utf-8") as ff:
                wf = csv.writer(ff)
                wf.writerow(FEATURES_HEADER)
                for take in feat_takes:
                    ts = feature_timestamps_ms(take, nid)
                    acts = take.feature_activity(nid)
                    for i, row in enumerate(take.features[nid]):
                        # float32 dari node, ditulis sependek mungkin tanpa kehilangan presisi
                        wf.writerow([int(ts[i])] + [str(np.float32(v)) for v in row[COL_F0:]]
                                    + [int(acts[i])])
            paths.append(feat_path)
    return paths

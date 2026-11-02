"""
Session and take management.

A Session holds a list of Takes recorded in one sitting.
Each Take stores raw sensor data per node, buffered locally. Every sample carries
the activity that was active when it arrived (switchable live during REC); on save
the take is split into segments (runs of one activity) and every Walking/Running
segment gets a pace (seconds per km) entered manually.
Next to the raw samples a take stores the window features computed on the node
(app/core/features.py); each window gets the majority activity of its samples.
Data is only pushed to Firebase when the user explicitly pushes the session.
"""

import re
import time
from dataclasses import dataclass, field
from datetime import datetime
from typing import Optional

import numpy as np

from app.core.dsp import DEFAULT_FS, span_seconds, time_axis
from app.core.features import (FEAT_ALL_COLUMNS, FEAT_VERSION, COL_F0, COL_T_END,
                               COL_T_START)
from app.core.labels import ACTIVITIES, MOVING_ACTIVITIES, activity_code, activity_name
from app.core.pace import format_pace, speed_kmh

RAW_COLUMNS = ["ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps", "node_ms", "activity"]
COL_ACT = 7
COL_MS = 6
_EPS = 1e-6

MAX_SESSION_NAME_LEN = 64
_UNSAFE_NAME_CHARS = re.compile(r"[^A-Za-z0-9_-]+")


def sanitize_session_name(name: str) -> str:
    """Make a user-typed session name safe as a file name and as a Firebase key.

    The name ends up in local CSV paths and in `recordings/<name>`, so path separators,
    dots (".." traversal) and Firebase-reserved characters (. $ # [ ] /) must not survive.
    Anything outside [A-Za-z0-9_-] becomes "_". Returns "" if nothing usable is left
    (the caller then falls back to the auto-generated name).
    """
    return _UNSAFE_NAME_CHARS.sub("_", name.strip()).strip("_")[:MAX_SESSION_NAME_LEN]
_FEAT_EDGE_MS = 1.0     # toleransi batas jendela fitur vs sampel raw pertama/terakhir


def f32(v) -> float:
    """Node feature value (float32) as the shortest float that round-trips it."""
    return float(str(np.float32(v)))


@dataclass
class Segment:
    """One run of a single activity. Times in seconds from the take's first sample."""
    start_s: float
    end_s: float
    activity: str
    pace_s_per_km: Optional[int] = None

    @property
    def duration(self) -> float:
        return self.end_s - self.start_s

    @property
    def needs_pace(self) -> bool:
        return self.activity in MOVING_ACTIVITIES

    @property
    def label(self) -> str:
        if self.needs_pace and self.pace_s_per_km:
            return f"{self.activity} {format_pace(self.pace_s_per_km)}"
        return self.activity

    def to_dict(self) -> dict:
        d = {
            "start_s": round(self.start_s, 3),
            "end_s": round(self.end_s, 3),
            "duration_s": round(self.duration, 3),
            "activity": self.activity,
        }
        if self.needs_pace and self.pace_s_per_km:
            d.update(pace=format_pace(self.pace_s_per_km),
                     pace_s_per_km=self.pace_s_per_km,
                     speed_kmh=round(speed_kmh(self.pace_s_per_km), 2))
        return d


@dataclass
class Take:
    index: int
    started_at: float
    ended_at: float
    # node_id -> (N, 8) array [ax,ay,az,gx,gy,gz,node_ms,activity]  (g, dps, ms, kode)
    data: dict = field(default_factory=dict)
    kept: bool = False
    sample_rate_hz: int = DEFAULT_FS
    segments: list = field(default_factory=list)   # list[Segment], diisi saat disimpan
    # (start_s, end_s) bagian rekaman asli yang disimpan; None = utuh
    trim_s: Optional[tuple] = None
    # node_id -> {"status": ..., "accel_calibrated": bool} saat REC dimulai
    calibration: dict = field(default_factory=dict)
    # node_id -> (M, 34) array [win, t_start_ms, t_end_ms, n, f0..f29], dihitung di node
    features: dict = field(default_factory=dict)

    @property
    def label(self) -> str:
        return " → ".join(s.label for s in self.segments) if self.segments else "—"

    @property
    def activities(self) -> list:
        return list(dict.fromkeys(s.activity for s in self.segments))

    @property
    def duration(self) -> float:
        """Duration of the stored samples (after trimming)."""
        if not self.data:
            return self.ended_at - self.started_at
        return max(span_seconds(arr, self.sample_rate_hz) for arr in self.data.values())

    @property
    def sample_count(self) -> int:
        if not self.data:
            return 0
        return max(arr.shape[0] for arr in self.data.values())

    @property
    def ref_node(self) -> int:
        return min(self.data)

    @property
    def feature_count(self) -> int:
        return max((f.shape[0] for f in self.features.values()), default=0)

    def clip_features(self):
        """Keep only the feature windows that lie completely inside the stored raw
        samples of the same node (node clock), e.g. after trimming."""
        out = {}
        for nid, feat in self.features.items():
            arr = self.data.get(nid)
            if arr is None or arr.shape[0] == 0 or feat.shape[0] == 0:
                continue
            lo, hi = arr[:, COL_MS].min(), arr[:, COL_MS].max()
            keep = ((feat[:, COL_T_START] >= lo - _FEAT_EDGE_MS) &
                    (feat[:, COL_T_END] <= hi + _FEAT_EDGE_MS))
            if keep.any():
                out[nid] = feat[keep]
        self.features = out

    def feature_activity(self, node_id: int) -> np.ndarray:
        """Activity code per feature window: majority of the node's raw samples inside
        the window (after relabeling); nearest sample if the window's samples were lost."""
        feat = self.features.get(node_id)
        arr = self.data.get(node_id)
        if feat is None or arr is None or arr.shape[0] == 0:
            return np.zeros(0, dtype=int)
        ms, codes = arr[:, COL_MS], arr[:, COL_ACT].astype(int)
        out = np.empty(feat.shape[0], dtype=int)
        for i, (t0, t1) in enumerate(feat[:, [COL_T_START, COL_T_END]]):
            inside = codes[(ms >= t0 - _FEAT_EDGE_MS) & (ms <= t1 + _FEAT_EDGE_MS)]
            if inside.size:
                out[i] = np.bincount(inside).argmax()
            else:
                out[i] = codes[np.abs(ms - t1).argmin()]
        return out

    def detect_segments(self) -> list:
        """Segments from the activity column of the reference node. Paces are taken
        over from already-saved segments that overlap with the same activity."""
        if not self.data:
            return []
        arr = self.data[self.ref_node]
        t = time_axis(arr, self.sample_rate_hz)
        codes = arr[:, COL_ACT].astype(int)
        starts = [0] + [i for i in range(1, len(codes)) if codes[i] != codes[i - 1]]
        end_total = span_seconds(arr, self.sample_rate_hz)
        segs = []
        for k, i in enumerate(starts):
            end = float(t[starts[k + 1]]) if k + 1 < len(starts) else end_total
            segs.append(Segment(float(t[i]), end, activity_name(codes[i])))
        for seg in segs:
            best, best_ov = None, 0.0
            for old in self.segments:
                ov = min(seg.end_s, old.end_s) - max(seg.start_s, old.start_s)
                if old.activity == seg.activity and ov > best_ov:
                    best, best_ov = old, ov
            if best:
                seg.pace_s_per_km = best.pace_s_per_km
        return segs

    def apply(self, segments: list, trim: Optional[tuple] = None):
        """Write segment activities into every node's activity column (by time),
        cut to `trim` (start_s, end_s) and store the rebased segments."""
        a, b = trim if trim else (0.0, float("inf"))
        new_data = {}
        for nid, arr in self.data.items():
            arr = arr.copy()
            t = time_axis(arr, self.sample_rate_hz)
            for k, seg in enumerate(segments):
                end = seg.end_s if k + 1 < len(segments) else float("inf")
                m = (t >= seg.start_s - _EPS) & (t < end - _EPS)
                arr[m, COL_ACT] = activity_code(seg.activity)
            part = arr[(t >= a - _EPS) & (t <= b + _EPS)]
            if part.shape[0]:
                new_data[nid] = part
        self.data = new_data
        self.clip_features()
        if trim:
            # relatif ke rekaman asli, juga jika take ini sudah pernah dipotong
            base = self.trim_s[0] if self.trim_s else 0.0
            self.trim_s = (round(base + a, 3), round(base + b, 3))

        # segment times relative to the first stored sample; merge identical neighbours
        total = self.duration
        out = []
        for seg in segments:
            s = max(seg.start_s, a) - a
            e = min(seg.end_s, b) - a
            if e - s <= _EPS:
                continue
            pace = seg.pace_s_per_km if seg.needs_pace else None
            if out and out[-1].activity == seg.activity and out[-1].pace_s_per_km == pace:
                out[-1].end_s = e
            else:
                out.append(Segment(s, e, seg.activity, pace))
        if out:
            out[0].start_s = 0.0
            out[-1].end_s = total
        self.segments = out

    def summary_stats(self, node_id: int) -> dict:
        """Return std stats for one node (used for Firebase upload)."""
        arr = self.data.get(node_id)
        if arr is None or arr.shape[0] < 2:
            return {}
        s = arr[:, 0:6].std(axis=0)
        return {
            "std_ax": float(s[0]), "std_ay": float(s[1]), "std_az": float(s[2]),
            "std_gx": float(s[3]), "std_gy": float(s[4]), "std_gz": float(s[5]),
            "dev_acc": float(s[0:3].mean()),
            "dev_gyr": float(s[3:6].mean()),
            "n_samples": arr.shape[0],
        }


class SessionManager:
    """
    Manages the current recording session.

    Usage:
        sm = SessionManager("Runner_A_treadmill")
        sm.start_take(sample_rate_hz=50)
        take = sm.stop_take(data_per_node)
        segs = take.detect_segments()           # edit activity / fill pace
        sm.save_take(take.index, segs, trim=(1.5, 42.0))
        # or sm.discard_take(take.index)
        payload = sm.build_firebase_payload()
    """

    def __init__(self, session_name: str = ""):
        self.session_name = sanitize_session_name(session_name) or self._auto_name()
        self.created_at   = time.time()
        self.takes: list[Take] = []
        self._active_take: Optional[Take] = None
        self._recording = False

    # ── session name ──────────────────────────────────────────────────────────

    @staticmethod
    def _auto_name() -> str:
        return datetime.now().strftime("session_%Y%m%d_%H%M%S")

    # ── recording control ─────────────────────────────────────────────────────

    @property
    def is_recording(self) -> bool:
        return self._recording

    @property
    def active_take(self) -> Optional[Take]:
        return self._active_take

    def start_take(self, sample_rate_hz: int = DEFAULT_FS):
        if self._recording:
            return
        self._active_take = Take(
            index=len(self.takes),
            sample_rate_hz=sample_rate_hz,
            started_at=time.time(),
            ended_at=0.0,
        )
        self._recording = True

    def stop_take(self, data: dict[int, np.ndarray], features: Optional[dict] = None) -> Take:
        """
        Finalize the active take with captured data.
        data:     {node_id: np.ndarray shape (N, 8)}
        features: {node_id: np.ndarray shape (M, 34)} window features from the nodes
        Returns the completed Take (not kept until save_take()).
        """
        if not self._recording or self._active_take is None:
            raise RuntimeError("No active take to stop.")

        take = self._active_take
        take.ended_at = time.time()
        take.data = data
        take.features = dict(features or {})
        take.clip_features()
        take.kept = False
        self.takes.append(take)
        self._active_take = None
        self._recording = False
        return take

    # ── save / discard ────────────────────────────────────────────────────────

    def save_take(self, index: int, segments: list, trim: Optional[tuple] = None):
        take = self.takes[index]
        take.apply(segments, trim)
        take.kept = True

    def discard_take(self, index: int):
        self.takes[index].kept = False

    # ── stats ─────────────────────────────────────────────────────────────────

    @property
    def kept_takes(self) -> list[Take]:
        return [t for t in self.takes if t.kept]

    @property
    def total_takes(self) -> int:
        return len(self.takes)

    @property
    def total_kept(self) -> int:
        return len(self.kept_takes)

    # ── Firebase push ─────────────────────────────────────────────────────────

    def build_firebase_payload(self) -> dict:
        """Build the full payload for all kept takes."""
        payload = {
            "meta": {
                "session": self.session_name,
                "created_at": self.created_at,
                "total_takes": self.total_takes,
                "kept_takes": self.total_kept,
            },
            "takes": {}
        }
        for take in self.kept_takes:
            take_entry = {
                "label":          take.label,
                "activities":     take.activities,
                "segments":       [s.to_dict() for s in take.segments],
                "started_at":     take.started_at,
                "duration_s":     round(take.duration, 3),
                "sample_rate_hz": take.sample_rate_hz,
                "columns":        RAW_COLUMNS,
                "activity_codes": ACTIVITIES,
                "nodes": {}
            }
            paces = {s.pace_s_per_km for s in take.segments if s.pace_s_per_km}
            if len(paces) == 1:                 # satu pace -> juga di level take
                p = paces.pop()
                take_entry.update(pace=format_pace(p), pace_s_per_km=p,
                                  speed_kmh=round(speed_kmh(p), 2))
            if take.trim_s:
                take_entry["trim_s"] = list(take.trim_s)
            if take.calibration:
                take_entry["calibration"] = {
                    f"node_{nid}": info for nid, info in take.calibration.items()}
            if take.features:
                take_entry["feature_columns"] = FEAT_ALL_COLUMNS + ["activity"]
                take_entry["feature_version"] = FEAT_VERSION

            for node_id, arr in take.data.items():
                stats = take.summary_stats(node_id)
                take_entry["nodes"][f"node_{node_id}"] = {
                    **stats,
                    "raw": arr.tolist(),
                }
                feat = take.features.get(node_id)
                if feat is not None:
                    acts = take.feature_activity(node_id)
                    take_entry["nodes"][f"node_{node_id}"]["features"] = [
                        [int(v) for v in row[:COL_F0]] + [f32(v) for v in row[COL_F0:]] + [int(a)]
                        for row, a in zip(feat, acts)]
            payload["takes"][f"take_{take.index:03d}"] = take_entry
        return payload

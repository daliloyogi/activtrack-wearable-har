"""
Serial reader — runs in a QThread, emits parsed sensor rows via Qt signals.
Reads the serial protocol of Ground Station v5 (mode DEBUG, 921600 baud).

GS -> PC (one CSV line per message):
    #GS,<versi>,<mode>                                    boot info
    D,gsMs,node,seq,nodeMs,ax,ay,az,gx,gy,gz,battMv,flags IMU data (node RUN)
    H,gsMs,node,seq,battMv,flags                          heartbeat (node STOP)
    E,gsMs,event,target,rateHz,src                        START/STOP/RATE/RESET/LOST/BACK
    F,gsMs,node,win,tStartMs,tEndMs,n,rateHz,ver,f0..f29  window features computed on the node
    P,gsMs,node,win,tEndMs,act,p0..p3,paceS,flags         EBM model result on the node
    S,gsMs,node,state,rateHz,rx,lost[,cal,featOk,featDrop] node stats, every 1 s
    C,gsMs,node,cal,calCount                              node calibration status changed
    G,gsMs,battV,battPct,badPkt                           GS status, every 1 s
    OK,<cmd>  /  ERR,<cmd>,<reason>                       command replies
    Units: ax..az = mg, gx..gz = 0.1 dps, battMv = mV

PC -> GS: PING | START [n] | STOP [n] | RATE <hz> | RESET | CAL [n]
"""

import time
import threading
from collections import deque

import numpy as np
import serial
import serial.tools.list_ports
from PyQt6.QtCore import QThread, pyqtSignal

from app.core.features import FEAT_COUNT

DEFAULT_BAUD  = 921600
PING_PERIOD_S = 1.0     # GS menganggap PC online jika ada PING dalam 3 detik


def list_ports():
    return [p.device for p in serial.tools.list_ports.comports()]


def parse_line(line):
    """
    Parse one GS line.
    Return (kind, fields) where kind is "D", "H", "F", "E", "S", "G", "OK", "ERR",
    fields is a dict (D/H/F) or list of raw strings (others). None if invalid.
    """
    p = line.split(",")
    kind = p[0]
    try:
        if kind == "D" and len(p) == 13:
            return "D", {
                "gs_ms":   int(p[1]),
                "node":    int(p[2]),
                "seq":     int(p[3]),
                "node_ms": int(p[4]),
                "ax": int(p[5]), "ay": int(p[6]), "az": int(p[7]),
                "gx": int(p[8]), "gy": int(p[9]), "gz": int(p[10]),
                "batt_mv": int(p[11]),
                "flags":   int(p[12]),
            }
        if kind == "H" and len(p) == 6:
            return "H", {
                "gs_ms":   int(p[1]),
                "node":    int(p[2]),
                "seq":     int(p[3]),
                "batt_mv": int(p[4]),
                "flags":   int(p[5]),
            }
        if kind == "F" and len(p) == 9 + FEAT_COUNT:
            return "F", {
                "gs_ms":   int(p[1]),
                "node":    int(p[2]),
                "win":     int(p[3]),
                "t_start": int(p[4]),
                "t_end":   int(p[5]),
                "n":       int(p[6]),
                "rate":    int(p[7]),
                "version": int(p[8]),
                "f":       [float(x) for x in p[9:]],
            }
    except ValueError:
        return None
    if kind in ("E", "S", "G", "C", "P", "OK", "ERR"):
        return kind, p[1:]
    return None


class SerialReader(QThread):
    """
    Background thread that reads serial port and maintains per-node ring buffers.

    Take capture runs in this thread (start_capture / stop_capture), so no sample
    is lost or delayed even when the UI thread is busy drawing.

    Signals:
        row_received(node_id, row_tuple)  — every IMU sample (D line)
        node_discovered(node_id)          — first time a node is seen (D or H)
        message_received(kind, fields)    — E/S/G/C/OK/ERR lines from the GS
        error(message)                    — port open failure
        disconnected()                    — serial dropped / closed
    """

    row_received     = pyqtSignal(int, tuple)
    node_discovered  = pyqtSignal(int)
    message_received = pyqtSignal(str, list)
    error            = pyqtSignal(str)
    disconnected     = pyqtSignal()

    def __init__(self, port, baud=DEFAULT_BAUD, window_sec=5, fs=50, init_commands=()):
        super().__init__()
        self.port       = port
        self.baud       = baud
        self.init_commands = list(init_commands)   # dikirim sekali setelah port terbuka
        self.buffer_len = window_sec * fs
        self._stop      = threading.Event()

        self._lock       = threading.Lock()
        self._bufs: dict[int, deque] = {}   # node_id -> deque of (ax,ay,az,gx,gy,gz,node_ms,activity)
        self._nodes: list[int] = []
        self._last_seq: dict[int, int] = {}
        self._lost: dict[int, int] = {}      # seq gaps seen by the PC per node
        self._batt: dict[int, int] = {}      # latest node battery, mV

        self._activity  = 0             # kode aktivitas yang ditempel ke setiap sampel
        self._capturing = False
        self._cap: dict[int, list] = {}
        # fitur jendela dari node: node_id -> list of (win, t_start_ms, t_end_ms, n, f0..f29)
        self._feat_cap: dict[int, list] = {}
        self._feat_done: dict[int, list] = {}      # hasil capture terakhir (lihat stop_capture)
        self._feat_n: dict[int, int] = {}          # jendela diterima sejak connect
        self._feat_missing: dict[int, int] = {}    # nomor jendela yang terlewat
        self._feat_last: dict[int, tuple] = {}     # node -> (monotonic time, win)

        # serial delay estimate: (PC clock - GS clock) minus its running minimum
        self._offset_base = None
        self._offset_t    = 0.0
        self._delay_ms    = 0.0

        self._ser = None
        self._tx_lock = threading.Lock()

    # ── public API (thread-safe) ──────────────────────────────────────────────

    def stop(self):
        self._stop.set()

    @property
    def nodes(self) -> list[int]:
        with self._lock:
            return list(self._nodes)

    def lost_count(self, node_id: int) -> int:
        with self._lock:
            return self._lost.get(node_id, 0)

    def battery_mv(self, node_id: int) -> int:
        with self._lock:
            return self._batt.get(node_id, 0)

    def get_buffer(self, node_id: int) -> np.ndarray:
        """Return copy of ring buffer as (N, 8) array [ax,ay,az,gx,gy,gz,node_ms,activity]."""
        with self._lock:
            dq = self._bufs.get(node_id)
            if not dq:
                return np.empty((0, 8))
            return np.array(dq, dtype=float)

    def set_activity(self, code: int):
        """Activity code stamped on every sample from now on (switchable during REC)."""
        with self._lock:
            self._activity = int(code)

    def last_node_ms(self, node_id: int):
        with self._lock:
            dq = self._bufs.get(node_id)
            return dq[-1][6] if dq else None

    def clear_buffers(self):
        """Empty the ring buffers (e.g. after the sample rate changes)."""
        with self._lock:
            for dq in self._bufs.values():
                dq.clear()

    def start_capture(self):
        with self._lock:
            self._cap = {nid: [] for nid in self._nodes}
            self._feat_cap = {}
            self._capturing = True

    def stop_capture(self) -> dict[int, np.ndarray]:
        """Stop capturing, return {node_id: (N, 8) array} for nodes with data.
        The node features of the same capture are kept for captured_features()."""
        with self._lock:
            self._capturing = False
            cap, self._cap = self._cap, {}
            self._feat_done, self._feat_cap = self._feat_cap, {}
        return {nid: np.array(rows, dtype=float) for nid, rows in cap.items() if rows}

    def captured_features(self) -> dict[int, np.ndarray]:
        """Node feature windows of the last capture: {node_id: (M, 34) array}
        [win, t_start_ms, t_end_ms, n, f0..f29] (see app/core/features.py)."""
        with self._lock:
            done = self._feat_done
        return {nid: np.array(rows, dtype=float) for nid, rows in done.items() if rows}

    def capture_feature_count(self) -> int:
        """Feature windows captured so far (node with the most windows)."""
        with self._lock:
            return max((len(v) for v in self._feat_cap.values()), default=0)

    def feature_status(self, node_id: int) -> tuple:
        """(windows received, windows missed, seconds since the last one or None)."""
        with self._lock:
            last = self._feat_last.get(node_id)
            return (self._feat_n.get(node_id, 0), self._feat_missing.get(node_id, 0),
                    time.monotonic() - last[0] if last else None)

    def capture_count(self) -> int:
        """Samples captured so far (node with the most samples)."""
        with self._lock:
            return max((len(v) for v in self._cap.values()), default=0)

    def serial_delay_ms(self) -> float:
        """Estimated extra delay between the GS receiving a packet and this app
        parsing it (0 = no backlog). Grows if the PC cannot keep up."""
        with self._lock:
            return self._delay_ms

    def send_command(self, cmd: str) -> bool:
        """Send a command to the GS, e.g. "START", "STOP 2", "RATE 50"."""
        with self._tx_lock:
            if self._ser is None or not self._ser.is_open:
                return False
            try:
                self._ser.write((cmd.strip() + "\n").encode("ascii"))
                return True
            except Exception:
                return False

    # ── internals ─────────────────────────────────────────────────────────────

    def _register_node(self, node_id: int) -> bool:
        """Must be called with self._lock held. Returns True if the node is new
        (caller emits node_discovered after releasing the lock)."""
        if node_id in self._bufs:
            return False
        self._bufs[node_id] = deque(maxlen=self.buffer_len)
        self._nodes.append(node_id)
        self._nodes.sort()
        return True

    def _update_delay(self, gs_ms: int):
        """Must be called with self._lock held."""
        now = time.perf_counter()
        offset = now * 1000.0 - gs_ms
        if self._offset_base is None or offset < self._offset_base:
            self._offset_base = offset
        else:
            # ikuti drift clock PC vs ESP32 (maks ~100 ppm)
            self._offset_base += (now - self._offset_t) * 0.1
        self._offset_t = now
        delay = max(0.0, offset - self._offset_base)
        # naik cepat, turun pelan -> angka yang tampil = delay terburuk terakhir
        self._delay_ms = delay if delay > self._delay_ms else self._delay_ms * 0.98 + delay * 0.02

    def _handle_data(self, d: dict):
        node_id = d["node"]
        with self._lock:
            # Scale to standard units: mg -> g, 0.1 dps -> dps
            row = (d["ax"] / 1000.0, d["ay"] / 1000.0, d["az"] / 1000.0,
                   d["gx"] / 10.0,   d["gy"] / 10.0,   d["gz"] / 10.0,
                   d["node_ms"], self._activity)
            is_new = self._register_node(node_id)
            prev = self._last_seq.get(node_id)
            if prev is not None:
                gap = (d["seq"] - prev - 1) & 0xFFFF
                if 0 < gap < 1000:          # gap besar = node reboot, abaikan
                    self._lost[node_id] = self._lost.get(node_id, 0) + gap
            self._last_seq[node_id] = d["seq"]
            self._batt[node_id] = d["batt_mv"]
            self._bufs[node_id].append(row)
            if self._capturing:
                self._cap.setdefault(node_id, []).append(row)
            self._update_delay(d["gs_ms"])

        if is_new:
            self.node_discovered.emit(node_id)
        self.row_received.emit(node_id, row)

    def _handle_features(self, d: dict):
        node_id = d["node"]
        row = (d["win"], d["t_start"], d["t_end"], d["n"], *d["f"])
        with self._lock:
            last = self._feat_last.get(node_id)
            if last is not None:
                gap = (d["win"] - last[1] - 1) & 0xFFFF
                if 0 < gap < 100:           # gap besar = node reboot
                    self._feat_missing[node_id] = self._feat_missing.get(node_id, 0) + gap
            self._feat_last[node_id] = (time.monotonic(), d["win"])
            self._feat_n[node_id] = self._feat_n.get(node_id, 0) + 1
            if self._capturing:
                self._feat_cap.setdefault(node_id, []).append(row)

    def _handle_heartbeat(self, h: dict):
        with self._lock:
            is_new = self._register_node(h["node"])
            self._last_seq[h["node"]] = h["seq"]
            self._batt[h["node"]] = h["batt_mv"]
        if is_new:
            self.node_discovered.emit(h["node"])

    def _handle_line(self, line: str):
        if not line or line.startswith("#"):
            return
        rec = parse_line(line)
        if rec is None:
            return
        kind, fields = rec
        if kind == "D":
            self._handle_data(fields)
        elif kind == "H":
            self._handle_heartbeat(fields)
        elif kind == "F":
            self._handle_features(fields)
        elif not (kind == "OK" and fields[:1] == ["PING"]):
            self.message_received.emit(kind, fields)

    # ── QThread.run ───────────────────────────────────────────────────────────

    def run(self):
        try:
            ser = serial.Serial()
            ser.port     = self.port
            ser.baudrate = self.baud
            ser.timeout  = 0.2
            ser.dtr = False     # jangan reset ESP32 saat port dibuka
            ser.rts = False
            ser.open()
        except Exception as e:
            self.error.emit(f"Gagal buka {self.port}: {e}")
            return

        ser.reset_input_buffer()
        self._ser = ser
        self._stop.clear()
        for cmd in self.init_commands:
            self.send_command(cmd)
        next_ping = 0.0
        pending = b""

        while not self._stop.is_set():
            now = time.monotonic()
            if now >= next_ping:
                self.send_command("PING")
                next_ping = now + PING_PERIOD_S

            # Baca semua byte yang sudah ada sekaligus. ser.readline() membaca
            # 1 byte per panggilan (beberapa syscall per byte di Windows) dan
            # tidak sanggup mengikuti ~100 baris/detik -> antrean & delay makin lama.
            try:
                chunk = ser.read(ser.in_waiting or 1)   # idle: tunggu maks timeout
            except Exception:
                break   # port hilang (USB dicabut)
            if not chunk:
                continue
            *lines, pending = (pending + chunk).split(b"\n")
            if len(pending) > 1024:     # sampah tanpa newline
                pending = b""
            for raw in lines:
                self._handle_line(raw.decode("utf-8", "ignore").strip())

        with self._tx_lock:
            self._ser = None
        try:
            ser.close()
        except Exception:
            pass
        self.disconnected.emit()

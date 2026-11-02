"""
Collect Tab — main data collection interface.

Workflow:
  Connect -> node streaming (live plot) -> pilih aktivitas -> ● REC
  -> selama REC ganti aktivitas kapan saja (tombol atau tombol keyboard 1-4)
  -> ■ STOP -> review: potong awal/akhir (seret area hijau), koreksi aktivitas
     per segmen, isi pace (m:ss /km) untuk segmen Walking/Running
  -> ✓ SIMPAN (atau ✗ BUANG) -> ulangi -> Push ke Firebase / Export CSV

Layout:
  Left  (310px)  : serial + ground station + session + take list + export/push
  Right (stretch): aktivitas + REC + live plots + review panel
"""

import os
import time
import numpy as np

from PyQt6.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QLabel, QLineEdit,
    QPushButton, QComboBox, QTableWidget, QTableWidgetItem,
    QGroupBox, QSplitter, QMessageBox, QHeaderView, QFrame,
    QFileDialog, QStyle,
)
from PyQt6.QtCore import Qt, QTimer, pyqtSignal, QRegularExpression
from PyQt6.QtGui import QColor, QRegularExpressionValidator, QKeySequence, QShortcut

import pyqtgraph as pg

from app.core import calibration as calib
from app.core.dsp import DEFAULT_FS, roll_win, rolling_std, time_axis, span_seconds
from app.core.labels import ACTIVITIES, MOVING_ACTIVITIES, DEFAULT_ACTIVITY, activity_code
from app.core.pace import PACE_MIN_S, PACE_MAX_S, parse_pace, format_pace, speed_kmh
from app.core.serial_reader import SerialReader, list_ports, DEFAULT_BAUD
from app.core.session_manager import SessionManager, Segment
from app.core.csv_export import export_session
from app.core.firebase_client import FirebaseClient
from app.ui import theme
from app.ui.theme import set_prop, set_tone, style_plot, plot_title, qcolor

GS_RATES   = [10, 20, 40, 50]   # RATE_OPTIONS di firmware ground station (40 = rate model)
WINDOW_SEC = 5              # lebar plot live default, detik (bisa diubah di Settings)
STD_SEC    = 0.2            # jendela bar std live, detik
LINE_PLOTS = ("acc", "gyr", "racc", "rgyr")
PACE_STEP_S = 5             # tombol panah atas/bawah di kolom pace


def _sep_line(color: str, label: str, pos: float = 0.0) -> pg.InfiniteLine:
    return pg.InfiniteLine(
        pos=pos, angle=90,
        pen=pg.mkPen(color, width=1.5, style=Qt.PenStyle.DashLine),
        label=label,
        labelOpts={"position": 0.92, "color": color, "fill": qcolor(theme.theme().surface, 0.85)},
    )


def _style_region(reg: pg.LinearRegionItem, t):
    reg.setBrush(qcolor(t.success, 0.10))
    reg.setHoverBrush(qcolor(t.success, 0.18))
    for ln in reg.lines:
        ln.setPen(pg.mkPen(t.success, width=2))
        ln.setHoverPen(pg.mkPen(t.success, width=3))


class SmoothRange:
    """Y-range that grows immediately but shrinks slowly -> no per-frame jumping."""
    def __init__(self, floor: float):
        self.floor = floor
        self.top = floor

    def update(self, value: float) -> float:
        target = max(value * 1.3, self.floor)
        self.top = target if target > self.top else self.top * 0.97 + target * 0.03
        return self.top


class PaceEdit(QLineEdit):
    """m:ss input; arrow up/down = ±5 s."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setPlaceholderText("m:ss")
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setValidator(QRegularExpressionValidator(
            QRegularExpression(r"^\d{0,2}([:.]\d{0,2})?$")))
        self.setToolTip(
            f"Pace menit:detik per km ({format_pace(PACE_MIN_S)}–{format_pace(PACE_MAX_S)}).\n"
            f"Panah atas/bawah = ±{PACE_STEP_S} s, Enter = simpan."
        )

    def keyPressEvent(self, e):
        if e.key() in (Qt.Key.Key_Up, Qt.Key.Key_Down):
            cur = parse_pace(self.text()) or 6 * 60
            step = PACE_STEP_S if e.key() == Qt.Key.Key_Down else -PACE_STEP_S  # bawah = lebih lambat
            self.setText(format_pace(min(PACE_MAX_S, max(PACE_MIN_S, cur + step))))
            return
        super().keyPressEvent(e)


class CollectTab(QWidget):
    serial_status_changed = pyqtSignal(str, bool)
    nodes_changed         = pyqtSignal(list)

    def __init__(self, firebase: FirebaseClient):
        super().__init__()
        self.firebase  = firebase
        self.reader    = None
        self.session   = None
        self.settings  = None        # SettingsTab, di-set oleh MainWindow
        self._plot_node = None
        self._fs = DEFAULT_FS        # sample rate aktif (dikonfirmasi GS lewat event E)
        self._window_sec = WINDOW_SEC
        self._gs_rate = None         # rate terakhir yang diumumkan GS
        self._node_state: dict[int, str] = {}

        self._activity = activity_code(DEFAULT_ACTIVITY)   # kode aktif (index ACTIVITIES)
        self._rec_t0 = 0.0
        self._live_marks: list = []  # [(activity, {node_id: node_ms})] pergantian saat REC
        self._live_lines: list = []  # [(plot_key, InfiniteLine, mark_idx)]

        self._review_take = None     # Take yang sedang di-review (None = mode live)
        self._seg_rows: list = []    # per segmen: dict(seg, combo, pace)
        self._sep_lines: list = []   # [(plot_key, InfiniteLine)] batas segmen di review
        self._regions: list = []     # LinearRegionItem per line plot (potongan)
        self._trim_full = (0.0, 0.0)
        self._region_sync = False
        self._last_pace: dict[str, int] = {}   # pace terakhir per aktivitas, jadi isian awal
        self._tick = 0

        self._yr = {"acc_std": SmoothRange(0.05), "gyr_std": SmoothRange(5.0),
                    "racc": SmoothRange(0.05), "rgyr": SmoothRange(5.0)}
        self._y_applied: dict[str, float] = {}
        self._node_info: dict[int, tuple] = {}   # node -> (state, rate, loss, batt) terakhir
        self._node_cal: dict[int, list] = {}     # node -> [cal byte, calCount or None]
        self._cal_wait: dict[int, float] = {}    # node -> waktu CAL dikirim (belum mulai)
        self._node_feat: dict[int, tuple] = {}   # node -> (featOk, featDrop) dari GS
        self._node_pred: dict[int, tuple] = {}   # node -> (t, act, probs, pace_s, flags) model EBM
        self._node_model_lbl: dict[int, QLabel] = {}

        self._build_ui()
        theme.manager().changed.connect(self._apply_theme)
        self._apply_theme(theme.theme())
        for i in range(len(ACTIVITIES)):
            sc = QShortcut(QKeySequence(str(i + 1)), self)
            sc.activated.connect(lambda code=i: self._set_activity(code))

        self._plot_timer = QTimer()
        self._plot_timer.setInterval(33)   # ~30 fps
        self._plot_timer.timeout.connect(self._on_tick)

    # ── UI ────────────────────────────────────────────────────────────────────

    def _build_ui(self):
        root = QHBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(self._build_left())
        splitter.addWidget(self._build_right())
        splitter.setSizes([310, 970])
        root.addWidget(splitter)

    # ── left panel ────────────────────────────────────────────────────────────

    def _build_left(self) -> QWidget:
        w = QWidget(); w.setMaximumWidth(330)
        lay = QVBoxLayout(w)
        lay.setSpacing(8)

        # --- serial connection ---
        conn_box = QGroupBox("Koneksi Serial")
        cl = QVBoxLayout(conn_box)
        row1 = QHBoxLayout()
        self._port_cb = QComboBox(); self._port_cb.setMinimumWidth(90)
        self._refresh_btn = QPushButton()
        self._refresh_btn.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_BrowserReload))
        self._refresh_btn.setProperty("role", "ghost")
        self._refresh_btn.setFixedWidth(34)
        self._refresh_btn.setToolTip("Refresh daftar port")
        self._refresh_btn.clicked.connect(self._refresh_ports)
        row1.addWidget(QLabel("Port:")); row1.addWidget(self._port_cb)
        row1.addWidget(self._refresh_btn)
        self._connect_btn = QPushButton("Connect")
        self._connect_btn.setCheckable(True)
        self._connect_btn.clicked.connect(self._toggle_connect)
        cl.addLayout(row1)
        cl.addWidget(self._connect_btn)
        lay.addWidget(conn_box)

        # --- ground station control ---
        lay.addWidget(self._build_gs_box())

        # --- session ---
        sess_box = QGroupBox("Session")
        sl = QVBoxLayout(sess_box)
        self._sess_name_edit = QLineEdit()
        self._sess_name_edit.setPlaceholderText("Nama session (kosong = auto)")
        self._new_sess_btn = QPushButton("New Session")
        self._new_sess_btn.clicked.connect(self._new_session)
        self._sess_label = QLabel("—")
        set_tone(self._sess_label, "accent")
        sl.addWidget(QLabel("Nama:"))
        sl.addWidget(self._sess_name_edit)
        sl.addWidget(self._new_sess_btn)
        sl.addWidget(self._sess_label)
        lay.addWidget(sess_box)

        # --- take list ---
        take_box = QGroupBox("Take List")
        tl = QVBoxLayout(take_box)
        self._take_table = QTableWidget(0, 4)
        self._take_table.setHorizontalHeaderLabels(["#", "Label", "Durasi", "Status"])
        hh = self._take_table.horizontalHeader()
        hh.setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
        hh.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self._take_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._take_table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._take_table.setAlternatingRowColors(True)
        self._take_table.verticalHeader().setVisible(False)
        self._take_table.setProperty("dense", True)
        self._take_table.setToolTip("Double-click take untuk review ulang (ubah label / pace)")
        self._take_table.cellDoubleClicked.connect(self._on_take_double_clicked)
        tl.addWidget(self._take_table)

        self._take_stats = QLabel("Takes: 0  |  Disimpan: 0")
        self._take_stats.setAlignment(Qt.AlignmentFlag.AlignCenter)
        set_tone(self._take_stats, "muted")
        tl.addWidget(self._take_stats)
        lay.addWidget(take_box, stretch=1)

        # --- action buttons ---
        self._push_btn = QPushButton("☁  Push Take Tersimpan → Firebase")
        self._push_btn.setEnabled(False)
        self._push_btn.setMinimumHeight(34)
        self._push_btn.setProperty("role", "primary")
        self._push_btn.clicked.connect(self._push_to_firebase)

        self._export_btn = QPushButton("💾  Export CSV Lokal")
        self._export_btn.setEnabled(False)
        self._export_btn.setMinimumHeight(34)
        self._export_btn.setToolTip(
            "Per node: <sesi>_node<N>_accel.csv, _gyro.csv\n"
            "  timestamp (epoch ms), X, Y, Z (g / °/s), activityMode\n"
            "dan <sesi>_node<N>_features.csv (fitur jendela dari node)\n"
            "  ts, 30 fitur, activity_mode\n"
            "activityMode: " + ", ".join(f"{i}={a}" for i, a in enumerate(ACTIVITIES)))
        self._export_btn.clicked.connect(self._export_csv)

        lay.addWidget(self._push_btn)
        lay.addWidget(self._export_btn)

        self._refresh_ports()
        return w

    def _build_gs_box(self) -> QGroupBox:
        box = QGroupBox("Ground Station")
        gl = QVBoxLayout(box)
        gl.setSpacing(6)

        rate_row = QHBoxLayout()
        rate_row.addWidget(QLabel("Sample rate:"))
        self._rate_cb = QComboBox()
        for hz in GS_RATES:
            self._rate_cb.addItem(f"{hz} Hz", userData=hz)
        self._rate_cb.setCurrentIndex(GS_RATES.index(DEFAULT_FS))
        self._rate_cb.setToolTip("Node di-STOP sebentar saat rate diganti.")
        self._rate_cb.activated.connect(self._on_rate_selected)
        rate_row.addWidget(self._rate_cb, stretch=1)
        gl.addLayout(rate_row)

        btn_row = QHBoxLayout()
        self._start_btn = QPushButton("▶ START semua")
        self._stop_btn  = QPushButton("■ STOP semua")
        self._start_btn.clicked.connect(lambda: self._send_gs("START"))
        self._stop_btn.clicked.connect(lambda: self._send_gs("STOP"))
        btn_row.addWidget(self._start_btn)
        btn_row.addWidget(self._stop_btn)
        gl.addLayout(btn_row)

        self._cal_all_btn = QPushButton("Kalibrasi IMU semua node")
        self._cal_all_btn.setToolTip(
            "Node harus diam dan datar ±3 detik.\n"
            "Gyro + accel dikalibrasi, hasil disimpan di flash node.")
        self._cal_all_btn.clicked.connect(lambda: self._request_calibration(None))
        gl.addWidget(self._cal_all_btn)

        # per node: [status 2 baris | tombol Kalibrasi]
        self._node_rows: dict[int, tuple] = {}   # node -> (row widget, label, button)
        self._node_status_lay = QVBoxLayout()
        self._node_status_lay.setSpacing(4)
        gl.addLayout(self._node_status_lay)

        self._gs_status_lbl = QLabel("GS: —")
        self._delay_lbl = QLabel("Delay serial: —")
        self._delay_lbl.setToolTip(
            "Antrean data antara GS dan app. Normalnya < 20 ms;\n"
            "kalau terus naik berarti PC tidak sanggup mengikuti data."
        )
        self._gs_msg_lbl = QLabel("")
        self._gs_msg_lbl.setWordWrap(True)
        for lbl in (self._gs_status_lbl, self._delay_lbl, self._gs_msg_lbl):
            lbl.setProperty("dense", True)
            set_tone(lbl, "muted")
        gl.addWidget(self._gs_status_lbl)
        gl.addWidget(self._delay_lbl)
        gl.addWidget(self._gs_msg_lbl)

        self._set_gs_controls_enabled(False)
        return box

    # ── right panel ───────────────────────────────────────────────────────────

    def _build_right(self) -> QWidget:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setSpacing(6)

        # --- controls bar ---
        ctrl = QHBoxLayout()
        node_lbl = QLabel("Node")
        set_tone(node_lbl, "muted")
        ctrl.addWidget(node_lbl)
        self._node_cb = QComboBox(); self._node_cb.setMinimumWidth(90)
        self._node_cb.currentIndexChanged.connect(self._on_node_changed)
        ctrl.addWidget(self._node_cb)
        ctrl.addSpacing(16)

        act_lbl = QLabel("Aktivitas")
        set_tone(act_lbl, "muted")
        ctrl.addWidget(act_lbl)
        self._act_btns: list[QPushButton] = []
        for i, act in enumerate(ACTIVITIES):
            btn = QPushButton(f"{i + 1}  {act}")
            btn.setCheckable(True)
            btn.setProperty("activity", act)
            btn.setToolTip(f"Tombol keyboard {i + 1} — bisa diganti kapan saja, juga saat REC")
            btn.clicked.connect(lambda checked, code=i: self._set_activity(code))
            ctrl.addWidget(btn)
            self._act_btns.append(btn)
        self._style_activity_buttons()

        ctrl.addStretch()

        self._rec_btn = QPushButton("● REC")
        self._rec_btn.setCheckable(True)
        self._rec_btn.setMinimumWidth(230)
        self._rec_btn.setMinimumHeight(36)
        self._rec_btn.setEnabled(False)
        self._rec_btn.setProperty("role", "rec")
        self._rec_btn.clicked.connect(self._toggle_rec)
        ctrl.addWidget(self._rec_btn)
        lay.addLayout(ctrl)

        # --- plots ---
        self._plots  = {}
        self._curves = {}
        self._plot_widget = self._build_plots()
        plot_card = QFrame()
        plot_card.setProperty("card", True)
        pc = QVBoxLayout(plot_card)
        pc.setContentsMargins(6, 6, 6, 6)
        pc.addWidget(self._plot_widget)
        lay.addWidget(plot_card, stretch=1)

        # --- review panel (hidden until STOP) ---
        self._review_panel = self._build_review_panel()
        self._review_panel.setVisible(False)
        lay.addWidget(self._review_panel)

        return w

    def _line_plot(self, glw, row, col, title, key, prefix):
        p = glw.addPlot(row=row, col=col)
        plot_title(p, title)
        p.showGrid(x=True, y=True, alpha=theme.theme().grid_alpha)
        p.setLabel("bottom", "detik")
        p.setClipToView(True)
        p.setDownsampling(auto=True, mode="peak")
        p.setMouseEnabled(x=False, y=False)
        p.hideButtons()
        # legend dulu, baru kurva -> kurva otomatis masuk legend
        p.addLegend(offset=(8, 4), colCount=3, labelTextSize="8pt")
        # pen width 1 = jalur gambar cepat Qt (width > 1 jauh lebih lambat)
        self._curves[key] = {
            k: p.plot(pen=pg.mkPen(theme.theme().axis[k], width=1), name=f"{prefix}{k.lower()}")
            for k in ("X", "Y", "Z")
        }
        self._plots[key] = p
        return p

    def _build_plots(self) -> pg.GraphicsLayoutWidget:
        glw = pg.GraphicsLayoutWidget()

        p_acc = self._line_plot(glw, 0, 0, "Accel X/Y/Z  (g)", "acc", "a")
        p_gyr = self._line_plot(glw, 0, 1, "Gyro X/Y/Z  (°/s)", "gyr", "g")
        p_acc.setYRange(-4, 4)
        p_gyr.setYRange(-500, 500)

        p_acc_std = glw.addPlot(row=1, col=0)
        p_gyr_std = glw.addPlot(row=1, col=1)
        plot_title(p_acc_std, "Std Accel  (g)")
        plot_title(p_gyr_std, "Std Gyro  (°/s)")
        colors = [theme.theme().axis[k] for k in ("X", "Y", "Z")]
        self._bar_acc = pg.BarGraphItem(x=[0, 1, 2], height=[0, 0, 0], width=0.6, brushes=colors)
        self._bar_gyr = pg.BarGraphItem(x=[0, 1, 2], height=[0, 0, 0], width=0.6, brushes=colors)
        p_acc_std.addItem(self._bar_acc)
        p_gyr_std.addItem(self._bar_gyr)
        for p in (p_acc_std, p_gyr_std):
            p.showGrid(y=True, alpha=theme.theme().grid_alpha)
            p.setXRange(-0.5, 2.5)
            p.setMouseEnabled(x=False, y=False)
            p.hideButtons()
            p.getAxis("bottom").setTicks([[(0, "X"), (1, "Y"), (2, "Z")]])
        self._plots["acc_std"] = p_acc_std
        self._plots["gyr_std"] = p_gyr_std

        self._line_plot(glw, 2, 0, "Rolling Std Accel  (g)", "racc", "σa")
        self._line_plot(glw, 2, 1, "Rolling Std Gyro  (°/s)", "rgyr", "σg")

        # baris bar std lebih pendek -> plot garis dapat ruang lebih
        for row, stretch in ((0, 5), (1, 3), (2, 5)):
            glw.ci.layout.setRowStretchFactor(row, stretch)
        self._set_live_x_range()
        return glw

    def _build_review_panel(self) -> QFrame:
        frame = QFrame()
        frame.setProperty("card", True)
        lay = QVBoxLayout(frame)
        lay.setContentsMargins(14, 10, 14, 10)
        lay.setSpacing(6)

        self._review_title = QLabel("Review Take")
        set_tone(self._review_title, "title")
        lay.addWidget(self._review_title)

        # --- potongan ---
        trim_row = QHBoxLayout()
        self._trim_lbl = QLabel("")
        set_tone(self._trim_lbl, "muted")
        self._trim_reset_btn = QPushButton("Reset potongan")
        self._trim_reset_btn.setProperty("role", "ghost")
        self._trim_reset_btn.clicked.connect(self._reset_trim)
        trim_row.addWidget(self._trim_lbl, stretch=1)
        trim_row.addWidget(self._trim_reset_btn)
        lay.addLayout(trim_row)

        # --- segmen: aktivitas + pace ---
        self._seg_table = QTableWidget(0, 6)
        self._seg_table.setHorizontalHeaderLabels(
            ["#", "Aktivitas", "Waktu", "Durasi", "Pace (/km)", "Kecepatan"])
        hh = self._seg_table.horizontalHeader()
        hh.setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
        hh.setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        self._seg_table.verticalHeader().setVisible(False)
        self._seg_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._seg_table.setSelectionMode(QTableWidget.SelectionMode.NoSelection)
        self._seg_table.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._seg_table.verticalHeader().setDefaultSectionSize(36)
        lay.addWidget(self._seg_table)

        # --- status + tombol ---
        btn_row = QHBoxLayout()
        self._review_status = QLabel("")
        self._review_status.setWordWrap(True)
        self._save_btn = QPushButton("✓  SIMPAN")
        self._save_btn.setMinimumHeight(40)
        self._save_btn.setMinimumWidth(120)
        self._save_btn.setProperty("role", "success")
        self._discard_btn = QPushButton("✗  BUANG")
        self._discard_btn.setMinimumHeight(40)
        self._discard_btn.setMinimumWidth(110)
        self._discard_btn.setProperty("role", "danger")
        self._save_btn.clicked.connect(self._on_save)
        self._discard_btn.clicked.connect(self._on_discard)
        btn_row.addWidget(self._review_status, stretch=1)
        btn_row.addWidget(self._save_btn)
        btn_row.addWidget(self._discard_btn)
        lay.addLayout(btn_row)

        return frame

    # ── serial ────────────────────────────────────────────────────────────────

    def _refresh_ports(self):
        self._port_cb.clear()
        ports = list_ports()
        self._port_cb.addItems(ports if ports else ["(no port)"])

    def _toggle_connect(self, checked: bool):
        if checked:
            port = self._port_cb.currentText()
            if not port or port == "(no port)":
                self._connect_btn.setChecked(False)
                return
            baud = self.settings.baud if self.settings else DEFAULT_BAUD
            self._window_sec = self.settings.window_sec if self.settings else WINDOW_SEC
            self._set_live_x_range()
            self.reader = SerialReader(
                port, baud=baud, window_sec=self._window_sec, fs=max(GS_RATES),
                # GS menolak RATE saat node RUN -> STOP, set rate, lalu langsung
                # START supaya plot live langsung jalan dengan rate yang benar
                init_commands=["STOP", f"RATE {self._rate_cb.currentData()}", "START"],
            )
            self.reader.set_activity(self._activity)
            self.reader.node_discovered.connect(self._on_node_discovered)
            self.reader.message_received.connect(self._on_gs_message)
            self.reader.error.connect(self._on_serial_error)
            self.reader.disconnected.connect(self._on_disconnected)
            self.reader.start()
            self._connect_btn.setText("Disconnect")
            self._rec_btn.setEnabled(self._review_take is None)
            self._set_gs_controls_enabled(True)
            self._plot_timer.start()
            self.serial_status_changed.emit(port, True)
        else:
            self._disconnect()

    def _disconnect(self):
        if self._rec_btn.isChecked():
            self._rec_btn.setChecked(False)
            self._toggle_rec(False)       # simpan apa yang sudah terekam ke review
        if self.reader:
            self.reader.stop()
            self.reader.wait(2000)
            self.reader = None
        self._plot_timer.stop()
        self._connect_btn.setText("Connect")
        self._connect_btn.setChecked(False)
        self._rec_btn.setEnabled(False)
        self._reset_gs_panel()
        self._node_cb.clear()
        self._plot_node = None
        self.serial_status_changed.emit("Terputus", False)
        self.nodes_changed.emit([])

    def _on_serial_error(self, msg: str):
        self._connect_btn.setChecked(False)
        self._connect_btn.setText("Connect")
        self._rec_btn.setEnabled(False)
        self._reset_gs_panel()
        self.serial_status_changed.emit(msg, False)
        QMessageBox.critical(self, "Serial Error", msg)

    def _on_disconnected(self):
        if self._rec_btn.isChecked() and self.reader:
            self._rec_btn.setChecked(False)
            self._toggle_rec(False)
        self._connect_btn.setChecked(False)
        self._connect_btn.setText("Connect")
        self._rec_btn.setEnabled(False)
        self._reset_gs_panel()
        self.serial_status_changed.emit("Terputus", False)

    # ── ground station ────────────────────────────────────────────────────────

    def _set_gs_controls_enabled(self, on: bool):
        for w in (self._rate_cb, self._start_btn, self._stop_btn):
            w.setEnabled(on)
        self._update_cal_buttons()

    def _cal_busy(self, node_id: int) -> bool:
        """Sedang kalibrasi, atau perintah baru dikirim (< 8 s, sesudahnya boleh coba lagi)."""
        cal = self._node_cal.get(node_id, [0, None])[0]
        waiting = node_id in self._cal_wait and time.monotonic() - self._cal_wait[node_id] < 8
        return waiting or calib.status(cal) == calib.CAL_RUNNING

    def _update_cal_buttons(self):
        """Kalibrasi hanya saat tersambung dan tidak sedang REC."""
        ok = self.reader is not None and not (self.session and self.session.is_recording)
        self._cal_all_btn.setEnabled(ok and bool(self._node_rows)
                                     and not all(self._cal_busy(n) for n in self._node_rows))
        for node_id, (_row, _l1, _l2, btn) in self._node_rows.items():
            btn.setEnabled(ok and not self._cal_busy(node_id))

    def _reset_gs_panel(self):
        self._set_gs_controls_enabled(False)
        self._gs_rate = None
        self._node_state.clear()
        for row, *_ in self._node_rows.values():
            row.deleteLater()
        self._node_rows.clear()
        self._node_cal.clear()
        self._cal_wait.clear()
        self._node_pred.clear()
        self._node_model_lbl.clear()
        self._gs_status_lbl.setText("GS: —")
        self._delay_lbl.setText("Delay serial: —")
        set_tone(self._delay_lbl, "muted")
        self._gs_msg_lbl.setText("")
        self._node_info.clear()

    def _send_gs(self, cmd: str):
        if not (self.reader and self.reader.send_command(cmd)):
            self._show_gs_msg(f"Gagal kirim: {cmd}", error=True)

    def _show_gs_msg(self, text: str, error: bool = False):
        set_tone(self._gs_msg_lbl, "danger" if error else "muted")
        self._gs_msg_lbl.setText(f"{time.strftime('%H:%M:%S')}  {text}")

    def _on_rate_selected(self, _):
        hz = self._rate_cb.currentData()
        if hz == self._gs_rate:
            return
        # GS hanya menerima RATE saat semua node STOP -> stop, ganti, jalan lagi
        was_running = any(st == "RUN" for st in self._node_state.values())
        for cmd in ["STOP", f"RATE {hz}"] + (["START"] if was_running else []):
            self._send_gs(cmd)

    def _sync_rate_cb(self):
        if self._gs_rate in GS_RATES:
            self._rate_cb.setCurrentIndex(GS_RATES.index(self._gs_rate))

    def _set_fs(self, hz: int):
        """Rate dikonfirmasi GS -> plot & take berikutnya memakai rate ini."""
        if hz <= 0 or hz == self._fs:
            return
        self._fs = hz
        if self.reader:
            self.reader.clear_buffers()   # sampel lama beda rate, buang

    def _on_gs_message(self, kind: str, f: list):
        if kind == "S" and len(f) >= 6:
            # S,gsMs,node,state,rateHz,rx,lost[,cal]
            try:
                node_id, rate_hz, rx, lost = int(f[1]), int(f[3]), int(f[4]), int(f[5])
            except ValueError:
                return
            state = f[2]
            self._node_state[node_id] = state
            if len(f) >= 7 and f[6].isdigit() and state not in ("---", "OFF"):
                self._node_cal.setdefault(node_id, [0, None])[0] = int(f[6])
                if calib.status(int(f[6])) == calib.CAL_RUNNING:
                    self._cal_wait.pop(node_id, None)
            if len(f) >= 9 and f[7].isdigit() and f[8].isdigit():
                self._node_feat[node_id] = (int(f[7]), int(f[8]))
            loss = 100.0 * lost / (rx + lost) if rx + lost else 0.0
            batt = self.reader.battery_mv(node_id) / 1000.0 if self.reader else 0.0
            batt_txt = f"{batt:.2f} V" if batt > 0 else "— V"
            self._node_info[node_id] = (state, rate_hz, loss, batt_txt)
            self._render_node_status(node_id)
        elif kind == "C" and len(f) >= 4:
            # C,gsMs,node,cal,calCount
            try:
                node_id, cal, count = int(f[1]), int(f[2]), int(f[3])
            except ValueError:
                return
            prev = self._node_cal.get(node_id, [0, None])
            finished = prev[1] is not None and count != prev[1] and calib.is_done(cal)
            requested = node_id in self._cal_wait or calib.status(prev[0]) == calib.CAL_RUNNING
            self._node_cal[node_id] = [cal, count]
            if calib.status(cal) == calib.CAL_RUNNING or finished:
                self._cal_wait.pop(node_id, None)
            if finished and requested:
                text, _tone = calib.describe(cal)
                self._show_gs_msg(f"N{node_id}: kalibrasi {text}", error=calib.is_failure(cal))
            self._render_node_status(node_id)
            self._update_cal_buttons()
        elif kind == "P" and len(f) >= 11:
            # P,gsMs,node,win,tEndMs,act,p0,p1,p2,p3,paceS,flags
            try:
                node_id, act, pace, flags = int(f[1]), int(f[4]), int(f[9]), int(f[10])
                probs = [int(v) / 255.0 for v in f[5:9]]
            except ValueError:
                return
            self._node_pred[node_id] = (time.monotonic(), act, probs, pace, flags)
            self._render_node_model(node_id)
        elif kind == "G" and len(f) >= 4:
            # G,gsMs,battV,battPct,badPkt
            self._gs_status_lbl.setText(f"GS: {f[1]} V ({f[2]}%) · bad pkt {f[3]}")
        elif kind == "E" and len(f) >= 5:
            # E,gsMs,event,target,rateHz,src
            event, target, rate, src = f[1], f[2], f[3], f[4]
            try:
                self._gs_rate = int(rate)
            except ValueError:
                pass
            else:
                self._sync_rate_cb()
                if not (self.session and self.session.is_recording):
                    self._set_fs(self._gs_rate)
            self._show_gs_msg(f"{event} {target} ({rate} Hz, dari {src})",
                              error=(event == "LOST"))
        elif kind == "ERR":
            cmd    = f[0] if f else "?"
            reason = f[1] if len(f) > 1 else ""
            if cmd == "RATE":
                self._sync_rate_cb()
                if reason == "MEREKAM":
                    reason = "hentikan (STOP) semua node dulu"
            self._show_gs_msg(f"Ditolak: {cmd} — {reason}", error=True)

    def _render_node_status(self, node_id: int):
        t = theme.theme()
        state, rate_hz, loss, batt_txt = self._node_info.get(node_id, ("---", 0, 0.0, "— V"))
        line1 = (f"<span style='color:{t.node_state.get(state, t.text)};'>●</span>&nbsp; "
                 f"<b>N{node_id}</b>&nbsp; {state}"
                 f"<span style='color:{t.muted};'> · {rate_hz} Hz · loss {loss:.1f}% · {batt_txt}</span>")
        cal = self._node_cal.get(node_id, [None, None])[0]
        tip = ""
        if node_id in self._cal_wait:
            waited = time.monotonic() - self._cal_wait[node_id]
            text, tone = (("menunggu node…", "accent") if waited < 8
                          else ("tidak direspons — offline?", "danger"))
        elif cal is None:
            text, tone = "status belum diketahui", "muted"
        else:
            text, tone = calib.describe(cal)
            tip = calib.detail(cal)
        color = {"success": t.success, "warning": t.warning, "danger": t.danger,
                 "accent": t.accent}.get(tone, t.muted)
        _row, lbl1, lbl2, _btn = self._node_row(node_id)
        lbl1.setText(line1)
        lbl1.setToolTip(self._feature_tip(node_id))
        lbl2.setText(f"<span style='color:{t.faint};'>&nbsp;&nbsp;&nbsp;&nbsp;IMU</span>&nbsp; "
                     f"<span style='color:{color};'>{text}</span>")
        lbl2.setToolTip(tip)

    def _render_node_model(self, node_id: int):
        """Baris 'Model' di status node: tebakan EBM terbaru dari node."""
        lbl = self._node_model_lbl.get(node_id)
        pred = self._node_pred.get(node_id)
        if lbl is None:
            self._node_row(node_id)
            lbl = self._node_model_lbl[node_id]
        if pred is None:
            lbl.setVisible(False)
            return
        t = theme.theme()
        stamp, act, probs, pace, flags = pred
        stale = time.monotonic() - stamp > 3.0
        head = f"<span style='color:{t.faint};'>&nbsp;&nbsp;&nbsp;&nbsp;Model</span>&nbsp; "
        if flags & 0x08:
            body, color = "self-test GAGAL (ekspor ulang model)", t.danger
        elif not flags & 0x01 or act >= len(ACTIVITIES):
            body, color = "belum ada di firmware node", t.muted
        else:
            name = ACTIVITIES[act]
            body = f"{name} {probs[act] * 100:.0f}%"
            if flags & 0x02 and pace:
                body += f" · {format_pace(pace)} /km"
            if flags & 0x04:
                body += " · rate ≠ training"
            color = t.muted if stale else (t.warning if flags & 0x04 else t.activity.get(name, t.text))
        lbl.setText(head + f"<span style='color:{color};'>{body}</span>")
        lbl.setToolTip(
            "Tebakan model EBM yang berjalan di node (jendela 20 sampel; model dilatih 40 Hz)\n" +
            "\n".join(f"{a}: {p * 100:.1f}%" for a, p in zip(ACTIVITIES, probs)) +
            ("\nSitting/Standing dikoreksi aturan sumbu paha" if flags & 0x20 else "") +
            ("\n(lebih dari 3 s lalu)" if stale else ""))
        lbl.setVisible(True)

    def _feature_tip(self, node_id: int) -> str:
        if self.reader is None:
            return ""
        got, missed, age = self.reader.feature_status(node_id)
        gs_ok, gs_drop = self._node_feat.get(node_id, (None, None))
        if not got and not gs_ok:
            return ("Fitur jendela: belum ada dari node ini "
                    "(node harus RUN ≥ 2 detik; firmware node & GS versi fitur).")
        tip = f"Fitur jendela dari node: {got} diterima app, {missed} terlewat"
        if gs_drop is not None:
            tip += f"\nGround station: {gs_ok} lengkap, {gs_drop} tidak lengkap (dibuang)"
        if age is not None:
            tip += f"\nTerakhir {age:.1f} s lalu"
        return tip

    def _node_row(self, node_id: int) -> tuple:
        row = self._node_rows.get(node_id)
        if row is None:
            # baris 1: status node (lebar penuh) · baris 2: status kalibrasi + tombol
            w = QWidget()
            vl = QVBoxLayout(w)
            vl.setContentsMargins(0, 0, 0, 0)
            vl.setSpacing(0)
            lbl1 = QLabel()
            lbl1.setProperty("dense", True)
            hl = QHBoxLayout()
            hl.setContentsMargins(0, 0, 0, 0)
            hl.setSpacing(6)
            lbl2 = QLabel()
            lbl2.setProperty("dense", True)
            lbl2.setWordWrap(True)
            btn = QPushButton("Kalibrasi")
            btn.setProperty("dense", True)
            btn.setToolTip(f"Kalibrasi IMU node {node_id} (diam & datar ±3 detik)")
            btn.clicked.connect(lambda _c, n=node_id: self._request_calibration(n))
            hl.addWidget(lbl2, stretch=1)
            hl.addWidget(btn)
            vl.addWidget(lbl1)
            vl.addLayout(hl)
            lbl3 = QLabel()                 # baris 3: hasil model EBM di node (jika ada)
            lbl3.setProperty("dense", True)
            lbl3.setVisible(False)
            vl.addWidget(lbl3)
            self._node_model_lbl[node_id] = lbl3
            row = (w, lbl1, lbl2, btn)
            self._node_rows[node_id] = row
            idx = sorted(self._node_rows).index(node_id)
            self._node_status_lay.insertWidget(idx, w)
            self._update_cal_buttons()
        return row

    def _request_calibration(self, node_id):
        """node_id None = semua node yang terlihat."""
        if self.reader is None:
            return
        targets = sorted(self._node_rows) if node_id is None else [node_id]
        if not targets:
            return
        names = ", ".join(f"N{n}" for n in targets)
        answer = QMessageBox.question(
            self, "Kalibrasi IMU",
            f"Kalibrasi {names}:\n\n"
            "1. Letakkan node di permukaan datar (meja), sisi mana saja menghadap bawah.\n"
            "2. Jangan disentuh sampai node berbunyi 2x (±3 detik).\n\n"
            "Gyro dan accel dikalibrasi ulang dan disimpan di flash node.\n"
            "Kalau node bergerak, kalibrasi gagal dan kalibrasi lama tetap dipakai.",
            QMessageBox.StandardButton.Ok | QMessageBox.StandardButton.Cancel,
        )
        if answer != QMessageBox.StandardButton.Ok:
            return
        self._send_gs("CAL" if node_id is None else f"CAL {node_id}")
        now = time.monotonic()
        for n in targets:
            self._cal_wait[n] = now
            self._render_node_status(n)
        self._show_gs_msg(f"Kalibrasi diminta: {names} — jangan sentuh node")
        self._update_cal_buttons()

    # ── nodes ─────────────────────────────────────────────────────────────────

    def _on_node_discovered(self, node_id: int):
        self._node_cb.addItem(f"Node {node_id}", userData=node_id)
        if self._plot_node is None:
            self._plot_node = node_id
        if self.reader:
            self.nodes_changed.emit(self.reader.nodes)

    def _on_node_changed(self, _):
        data = self._node_cb.currentData()
        if data is not None:
            self._plot_node = data
            if self._review_take is not None:
                self._render_review()

    # ── session ───────────────────────────────────────────────────────────────

    def _new_session(self):
        if self.session and self.session.is_recording:
            QMessageBox.warning(self, "Recording Aktif", "Hentikan recording dulu.")
            return
        name = self._sess_name_edit.text().strip()
        self.session = SessionManager(name)
        self._sess_label.setText(self.session.session_name)
        self._refresh_take_table()

    # ── activity (live) ───────────────────────────────────────────────────────

    def _style_activity_buttons(self):
        for i, btn in enumerate(self._act_btns):
            btn.setChecked(i == self._activity)

    def _set_activity(self, code: int):
        if code == self._activity:
            self._style_activity_buttons()      # klik tombol yang sudah aktif -> tetap aktif
            return
        self._activity = code
        self._style_activity_buttons()
        if self.reader:
            self.reader.set_activity(code)
        if self.session and self.session.is_recording and self.reader:
            # tandai pergantian di plot live (posisi per node, pakai node_ms terakhir)
            act = ACTIVITIES[code]
            stamp = {nid: self.reader.last_node_ms(nid) for nid in self.reader.nodes}
            self._live_marks.append((act, stamp))
            self._add_live_lines(len(self._live_marks) - 1)
            self._update_rec_label()

    def _add_live_lines(self, mi: int):
        act = self._live_marks[mi][0]
        for key in LINE_PLOTS:
            line = _sep_line(theme.theme().activity[act], act, pos=1e9)   # posisi diatur tiap frame
            self._plots[key].addItem(line)
            self._live_lines.append((key, line, mi))

    def _remove_live_lines(self):
        for key, line, _ in self._live_lines:
            self._plots[key].removeItem(line)
        self._live_lines = []

    def _clear_live_lines(self):
        self._remove_live_lines()
        self._live_marks = []

    # ── recording ─────────────────────────────────────────────────────────────

    def _toggle_rec(self, checked: bool):
        if checked:
            if self.reader is None:
                self._rec_btn.setChecked(False)
                return
            if self.session is None:
                self._new_session()
            self.session.start_take(sample_rate_hz=self._fs)
            self.session.active_take.calibration = {
                nid: {"status": calib.key(v[0]), "accel_calibrated": calib.accel_valid(v[0])}
                for nid, v in self._node_cal.items()
            }
            self._clear_live_lines()
            self.reader.set_activity(self._activity)
            self.reader.start_capture()
            self._rec_t0 = time.monotonic()
            self._rate_cb.setEnabled(False)
            self._update_cal_buttons()
            self._update_rec_label()
            # node yang belum streaming -> minta START
            if not self._node_state or any(st != "RUN" for st in self._node_state.values()):
                self._send_gs("START")
        else:
            if self.session is None or not self.session.is_recording:
                return
            data = self.reader.stop_capture() if self.reader else {}
            feats = self.reader.captured_features() if self.reader else {}
            take = self.session.stop_take(data, feats)
            self._clear_live_lines()
            self._rate_cb.setEnabled(self.reader is not None)
            self._update_cal_buttons()
            if self._gs_rate:
                self._set_fs(self._gs_rate)   # rate yang berubah selama take
            self._rec_btn.setText("● REC")
            if take.sample_count < 2:
                self._show_gs_msg("Take kosong (tidak ada data dari node) — dibuang.", error=True)
                self._refresh_take_table()
                return
            self._enter_review(take)

    def _update_rec_label(self):
        el = time.monotonic() - self._rec_t0
        n = self.reader.capture_count() if self.reader else 0
        k = self.reader.capture_feature_count() if self.reader else 0
        self._rec_btn.setText(
            f"■ STOP   {int(el // 60):02d}:{el % 60:04.1f} · {ACTIVITIES[self._activity]}"
            f" · {n} · F{k}")
        self._rec_btn.setToolTip(f"{n} sampel raw · {k} jendela fitur dari node")

    # ── review ────────────────────────────────────────────────────────────────

    def _enter_review(self, take):
        self._review_take = take
        nodes = ", ".join(str(n) for n in sorted(take.data))
        self._review_title.setText(
            f"Review Take #{take.index + 1}  —  {take.duration:.1f} s  ·  "
            f"{take.sample_count} sampel  ·  Node {nodes}  ·  {take.sample_rate_hz} Hz  ·  "
            + (f"{take.feature_count} jendela fitur" if take.features
               else "fitur node: tidak ada")
        )
        self._add_trim_regions(span_seconds(self._review_arr(), take.sample_rate_hz))
        self._build_segment_rows(take.detect_segments())
        self._render_review()
        self._review_panel.setVisible(True)
        self._rec_btn.setEnabled(False)
        self._focus_next_pace()

    def _exit_review(self):
        self._review_take = None
        self._clear_segment_lines()
        self._remove_trim_regions()
        self._seg_table.setRowCount(0)
        self._seg_rows = []
        self._review_panel.setVisible(False)
        self._rec_btn.setEnabled(self.reader is not None)
        self._set_live_x_range()
        self._y_applied.clear()
        for key in LINE_PLOTS:
            for c in self._curves[key].values():
                c.setData([], [])

    def _review_arr(self) -> np.ndarray:
        take = self._review_take
        arr = take.data.get(self._plot_node)
        if arr is None:
            arr = take.data[take.ref_node]
        return arr

    def _render_review(self):
        take = self._review_take
        arr = self._review_arr()
        xx = time_axis(arr, take.sample_rate_hz)
        dur = span_seconds(arr, take.sample_rate_hz)
        for key in LINE_PLOTS:
            self._plots[key].setXRange(0, dur, padding=0.01)
        s_acc, s_gyr = rolling_std(arr, roll_win(take.sample_rate_hz))
        self._set_curves(arr, xx, s_acc, s_gyr)
        self._y_applied.clear()
        self._plots["racc"].setYRange(0, max(s_acc.max() * 1.3, 0.05), padding=0)
        self._plots["rgyr"].setYRange(0, max(s_gyr.max() * 1.3, 5.0), padding=0)
        self._draw_segment_lines()
        self._update_trim_info()

    # --- segmen ---

    def _build_segment_rows(self, segs: list):
        self._seg_table.setRowCount(len(segs))
        self._seg_rows = []
        for r, seg in enumerate(segs):
            combo = QComboBox()
            combo.setMinimumWidth(130)
            combo.addItems(ACTIVITIES)
            combo.setCurrentText(seg.activity)
            combo.currentTextChanged.connect(lambda name, row=r: self._on_seg_activity_changed(row, name))
            pace = PaceEdit()
            pace.setFixedWidth(70)
            known = seg.pace_s_per_km or self._last_pace.get(seg.activity)
            if seg.needs_pace and known:
                pace.setText(format_pace(known))
            pace.textChanged.connect(lambda _t, row=r: self._on_seg_pace_changed(row))
            pace.returnPressed.connect(self._on_pace_return)

            self._seg_table.setItem(r, 0, QTableWidgetItem(str(r + 1)))
            self._seg_table.setCellWidget(r, 1, combo)
            self._seg_table.setItem(r, 2, QTableWidgetItem(""))
            self._seg_table.setItem(r, 3, QTableWidgetItem(""))
            self._seg_table.setCellWidget(r, 4, pace)
            self._seg_table.setItem(r, 5, QTableWidgetItem(""))
            self._seg_rows.append({"seg": seg, "combo": combo, "pace": pace})
        self._seg_table.resizeColumnsToContents()
        self._seg_table.setColumnWidth(1, 146)
        self._seg_table.setColumnWidth(4, 90)
        # tinggi tabel = isi (maks 4 baris terlihat, sisanya scroll)
        rows = min(len(segs), 4)
        h = (self._seg_table.horizontalHeader().sizeHint().height()
             + rows * self._seg_table.verticalHeader().defaultSectionSize()
             + 2 * self._seg_table.frameWidth() + 2)
        self._seg_table.setFixedHeight(h)

    def _on_seg_activity_changed(self, row: int, name: str):
        rec = self._seg_rows[row]
        if name in MOVING_ACTIVITIES and not rec["pace"].text() and name in self._last_pace:
            rec["pace"].setText(format_pace(self._last_pace[name]))
        self._draw_segment_lines()
        self._update_segment_view()
        if name in MOVING_ACTIVITIES:
            rec["pace"].setFocus()
            rec["pace"].selectAll()

    def _on_seg_pace_changed(self, row: int):
        self._update_segment_view()

    def _segment_visible_range(self, seg) -> tuple:
        a, b = self._trim_range()
        return max(seg.start_s, a), min(seg.end_s, b)

    def _update_segment_view(self):
        """Refresh waktu/durasi/kecepatan per segmen (terpotong), lalu validasi."""
        missing = bad = 0
        for r, rec in enumerate(self._seg_rows):
            s, e = self._segment_visible_range(rec["seg"])
            visible = e - s > 1e-6
            self._seg_table.setRowHidden(r, not visible)
            act = rec["combo"].currentText()
            moving = act in MOVING_ACTIVITIES
            rec["pace"].setEnabled(moving)
            self._seg_table.item(r, 2).setText(f"{s:.1f} – {e:.1f} s")
            self._seg_table.item(r, 3).setText(f"{max(0.0, e - s):.1f} s")
            self._seg_table.item(r, 0).setForeground(QColor(theme.theme().activity[act]))
            speed_item = self._seg_table.item(r, 5)
            if not moving:
                speed_item.setText("—")
                set_prop(rec["pace"], "invalid", False)
                continue
            pace = parse_pace(rec["pace"].text())
            if pace:
                speed_item.setText(f"{speed_kmh(pace):.2f} km/h")
                set_prop(rec["pace"], "invalid", False)
            else:
                speed_item.setText("")
                set_prop(rec["pace"], "invalid", visible)
                if visible:
                    if rec["pace"].text().strip():
                        bad += 1
                    else:
                        missing += 1
        a, b = self._trim_range()
        enough = b - a > 0.5            # minimal 0.5 s data yang disimpan
        if not enough:
            msg, tone = "Bagian yang disimpan terlalu pendek.", "danger"
        elif bad:
            msg, tone = (f"Format pace m:ss, {format_pace(PACE_MIN_S)}–{format_pace(PACE_MAX_S)} /km.",
                         "danger")
        elif missing:
            msg, tone = f"Isi pace untuk {missing} segmen Walking/Running.", "warning"
        else:
            msg, tone = "Siap disimpan  ·  Enter = simpan", "success"
        self._review_status.setText(msg)
        set_tone(self._review_status, tone)
        self._save_btn.setEnabled(enough and not bad and not missing)

    def _focus_next_pace(self) -> bool:
        """Focus the first visible Walking/Running pace field that is still invalid."""
        for r, rec in enumerate(self._seg_rows):
            if (not self._seg_table.isRowHidden(r) and rec["pace"].isEnabled()
                    and not parse_pace(rec["pace"].text())):
                rec["pace"].setFocus()
                rec["pace"].selectAll()
                return True
        return False

    def _on_pace_return(self):
        if self._save_btn.isEnabled():
            self._on_save()
        else:
            self._focus_next_pace()

    def _current_segments(self) -> list:
        segs = []
        for rec in self._seg_rows:
            act = rec["combo"].currentText()
            pace = parse_pace(rec["pace"].text()) if act in MOVING_ACTIVITIES else None
            segs.append(Segment(rec["seg"].start_s, rec["seg"].end_s, act, pace))
        return segs

    def _draw_segment_lines(self):
        self._clear_segment_lines()
        for rec in self._seg_rows[1:]:          # batas di awal setiap segmen berikutnya
            act = rec["combo"].currentText()
            for key in LINE_PLOTS:
                line = _sep_line(theme.theme().activity[act], act, pos=rec["seg"].start_s)
                self._plots[key].addItem(line)
                self._sep_lines.append((key, line))

    def _clear_segment_lines(self):
        for key, line in self._sep_lines:
            self._plots[key].removeItem(line)
        self._sep_lines = []

    # --- potongan (trim) ---

    def _add_trim_regions(self, dur: float):
        self._remove_trim_regions()
        for key in LINE_PLOTS:
            reg = pg.LinearRegionItem(values=(0, dur), bounds=(0, dur))
            _style_region(reg, theme.theme())
            reg.setZValue(-10)
            reg.sigRegionChanged.connect(self._on_region_changed)
            self._plots[key].addItem(reg)
            self._regions.append(reg)
        self._trim_full = (0.0, dur)

    def _remove_trim_regions(self):
        for key, reg in zip(LINE_PLOTS, self._regions):
            self._plots[key].removeItem(reg)
        self._regions = []

    def _on_region_changed(self, src):
        if self._region_sync:
            return
        self._region_sync = True
        rgn = src.getRegion()
        for reg in self._regions:
            if reg is not src:
                reg.setRegion(rgn)
        self._region_sync = False
        self._update_trim_info()

    def _trim_range(self) -> tuple:
        if not self._regions:
            return self._trim_full
        a, b = self._regions[0].getRegion()
        return (max(0.0, a), min(self._trim_full[1], b))

    def _reset_trim(self):
        if self._regions:
            self._regions[0].setRegion(self._trim_full)

    def _update_trim_info(self):
        take = self._review_take
        if take is None:
            return
        a, b = self._trim_range()
        full = self._trim_full[1]
        cut = a > 0.005 or b < full - 0.005
        self._trim_lbl.setText(
            f"Disimpan: {a:.1f} – {b:.1f} s  ({b - a:.1f} s dari {full:.1f} s)"
            + ("" if cut else "   ·   seret batas area hijau di plot untuk membuang awal/akhir")
        )
        self._trim_reset_btn.setEnabled(cut)
        # bar std = bagian yang akan disimpan
        arr = self._review_arr()
        t = time_axis(arr, take.sample_rate_hz)
        part = arr[(t >= a) & (t <= b)]
        if part.shape[0] >= 2:
            std_a, std_g = part[:, 0:3].std(axis=0), part[:, 3:6].std(axis=0)
            self._bar_acc.setOpts(height=std_a)
            self._bar_gyr.setOpts(height=std_g)
            self._plots["acc_std"].setYRange(0, max(std_a.max() * 1.3, 0.05), padding=0)
            self._plots["gyr_std"].setYRange(0, max(std_g.max() * 1.3, 5.0), padding=0)
        self._update_segment_view()

    # --- simpan / buang ---

    def _on_save(self):
        take = self._review_take
        if take is None or not self._save_btn.isEnabled():
            return
        a, b = self._trim_range()
        full = self._trim_full[1]
        trim = None if (a <= 0.005 and b >= full - 0.005) else (a, b)
        segs = self._current_segments()
        self.session.save_take(take.index, segs, trim=trim)
        for seg in take.segments:
            if seg.pace_s_per_km:
                self._last_pace[seg.activity] = seg.pace_s_per_km
        self._refresh_take_table()
        self._exit_review()

    def _on_discard(self):
        take = self._review_take
        if take is None:
            return
        self.session.discard_take(take.index)
        self._refresh_take_table()
        self._exit_review()

    # ── take table ────────────────────────────────────────────────────────────

    def _refresh_take_table(self):
        takes = self.session.takes if self.session else []
        th = theme.theme()
        self._take_table.setRowCount(len(takes))
        for row, t in enumerate(takes):
            self._take_table.setItem(row, 0, QTableWidgetItem(str(t.index + 1)))
            lbl = QTableWidgetItem(t.label)
            lbl.setToolTip(t.label)
            if t.segments:
                lbl.setForeground(QColor(th.activity.get(t.segments[0].activity, th.text)))
            self._take_table.setItem(row, 1, lbl)
            self._take_table.setItem(row, 2, QTableWidgetItem(f"{t.duration:.1f}s"))
            status_item = QTableWidgetItem("✓ SIMPAN" if t.kept else "✗ BUANG")
            status_item.setForeground(QColor(th.success if t.kept else th.danger))
            self._take_table.setItem(row, 3, status_item)

        kept  = self.session.total_kept if self.session else 0
        total = self.session.total_takes if self.session else 0
        self._take_stats.setText(f"Takes: {total}  |  Disimpan: {kept}")
        self._push_btn.setEnabled(kept > 0)
        self._export_btn.setEnabled(kept > 0)

    def _on_take_double_clicked(self, row: int, _col: int):
        """Review ulang take (label/pace/potongan), juga take yang sudah dibuang."""
        if self.session is None or row >= len(self.session.takes):
            return
        if self.session.is_recording or self._review_take is not None:
            return
        take = self.session.takes[row]
        if take.sample_count < 2:
            return
        self._enter_review(take)

    # ── theme ─────────────────────────────────────────────────────────────────

    def _apply_theme(self, t):
        """Restyle everything that the global stylesheet cannot reach."""
        self._plot_widget.setBackground(t.plot_bg)
        for p in self._plots.values():
            style_plot(p, t)
        for key in LINE_PLOTS:
            for k, c in self._curves[key].items():
                c.setPen(pg.mkPen(t.axis[k], width=1))
        brushes = [pg.mkBrush(t.axis[k]) for k in ("X", "Y", "Z")]
        self._bar_acc.setOpts(brushes=brushes, pens=[pg.mkPen(None)] * 3)
        self._bar_gyr.setOpts(brushes=brushes, pens=[pg.mkPen(None)] * 3)
        for reg in self._regions:
            _style_region(reg, t)
        if self._live_lines:
            self._remove_live_lines()
            for mi in range(len(self._live_marks)):
                self._add_live_lines(mi)
        if self._review_take is not None:
            self._draw_segment_lines()
            self._update_segment_view()
        for node_id in self._node_rows:
            self._render_node_status(node_id)
        self._refresh_take_table()

    # ── plots ─────────────────────────────────────────────────────────────────

    def _set_live_x_range(self):
        for key in LINE_PLOTS:
            if key in self._plots:
                self._plots[key].setXRange(-self._window_sec, 0, padding=0)

    def _on_tick(self):
        self._tick += 1
        if self.session and self.session.is_recording and self._tick % 3 == 0:
            self._update_rec_label()
        if self._cal_wait and self._tick % 30 == 0:
            for node_id in list(self._cal_wait):
                self._render_node_status(node_id)     # "menunggu" -> "offline?" setelah 8 s
            self._update_cal_buttons()
        if self._node_pred and self._tick % 30 == 0:
            for node_id in list(self._node_pred):
                self._render_node_model(node_id)      # tebakan basi -> abu-abu
        if self.reader and self._tick % 15 == 0:
            d = self.reader.serial_delay_ms()
            self._delay_lbl.setText(f"Delay serial: {d:.0f} ms")
            set_tone(self._delay_lbl, "success" if d < 50 else "warning" if d < 250 else "danger")
        if self._review_take is None:
            self._update_live_plots()

    def _set_curves(self, arr, xx, s_acc, s_gyr):
        for i, k in enumerate(("X", "Y", "Z")):
            self._curves["acc"][k].setData(xx, arr[:, i], skipFiniteCheck=True)
            self._curves["gyr"][k].setData(xx, arr[:, 3 + i], skipFiniteCheck=True)
            self._curves["racc"][k].setData(xx, s_acc[:, i], skipFiniteCheck=True)
            self._curves["rgyr"][k].setData(xx, s_gyr[:, i], skipFiniteCheck=True)

    def _update_live_plots(self):
        if self.reader is None or self._plot_node is None:
            return
        fs  = self._fs
        arr = self._live_window(self.reader.get_buffer(self._plot_node))
        if arr.shape[0] == 0:
            return

        xx = time_axis(arr, fs)
        xx = xx - xx[-1]                       # -WINDOW_SEC .. 0
        s_acc, s_gyr = rolling_std(arr, roll_win(fs))
        self._set_curves(arr, xx, s_acc, s_gyr)

        # garis pergantian aktivitas selama REC bergeser ke kiri
        if self._live_lines and arr.shape[1] > 6:
            last_ms = arr[-1, 6]
            for _, line, mi in self._live_lines:
                ms = self._live_marks[mi][1].get(self._plot_node)
                line.setPos((ms - last_ms) / 1000.0 if ms is not None else 1e9)

        last = arr[-max(2, round(fs * STD_SEC)):]
        if last.shape[0] >= 2:
            std_a = last[:, 0:3].std(axis=0)
            std_g = last[:, 3:6].std(axis=0)
        else:
            std_a = std_g = np.zeros(3)
        self._bar_acc.setOpts(height=std_a)
        self._bar_gyr.setOpts(height=std_g)
        self._set_y("acc_std", self._yr["acc_std"].update(std_a.max()))
        self._set_y("gyr_std", self._yr["gyr_std"].update(std_g.max()))
        self._set_y("racc", self._yr["racc"].update(s_acc.max()))
        self._set_y("rgyr", self._yr["rgyr"].update(s_gyr.max()))

    def _set_y(self, key: str, top: float):
        """Apply a y-range only when it changed > 3% (avoids axis relayout every frame)."""
        prev = self._y_applied.get(key)
        if prev is None or abs(top - prev) > 0.03 * prev:
            self._plots[key].setYRange(0, top, padding=0)
            self._y_applied[key] = top

    def _live_window(self, arr: np.ndarray) -> np.ndarray:
        """Last `window_sec` seconds of the live buffer, by node_ms."""
        if arr.shape[0] == 0 or arr.shape[1] <= 6:
            return arr[-self._window_sec * self._fs:]
        t = arr[:, 6]
        resets = np.nonzero(np.diff(t) <= 0)[0]    # node reboot -> timestamp mundur
        if resets.size:
            arr, t = arr[resets[-1] + 1:], t[resets[-1] + 1:]
        return arr[t >= t[-1] - self._window_sec * 1000]

    # ── firebase push ─────────────────────────────────────────────────────────

    def _push_to_firebase(self):
        if self.session is None or self.session.total_kept == 0:
            return
        payload = self.session.build_firebase_payload()
        ok, msg = self.firebase.push_session(self.session.session_name, payload)
        if ok:
            QMessageBox.information(self, "Upload Berhasil", msg)
        else:
            QMessageBox.critical(self, "Upload Gagal", msg)

    # ── CSV export ────────────────────────────────────────────────────────────

    def _export_csv(self):
        if self.session is None or self.session.total_kept == 0:
            return
        folder = QFileDialog.getExistingDirectory(
            self, "Pilih Folder Export CSV", os.path.expanduser("~")
        )
        if not folder:
            return
        try:
            paths = export_session(self.session, folder)
        except OSError as e:
            QMessageBox.critical(
                self, "Export Gagal",
                f"{e}\n\nTutup file CSV yang sedang dibuka (mis. di Excel) lalu coba lagi.")
            return
        note = ""
        if not any(p.endswith("_features.csv") for p in paths):
            note = ("\n\nTidak ada fitur jendela dari node di take yang disimpan — "
                    "pastikan firmware node & GS sudah di-upload ulang.")
        QMessageBox.information(
            self, "Export Selesai",
            f"{len(paths)} file CSV disimpan di:\n{folder}\n\n"
            + "\n".join(os.path.basename(p) for p in paths) + note
        )

    # ── cleanup ───────────────────────────────────────────────────────────────

    def cleanup(self):
        self._plot_timer.stop()
        if self.reader:
            self.reader.stop()
            self.reader.wait(2000)

"""
History Tab — browse sessions stored in Firebase, filter takes by activity and pace.
"""

import numpy as np
from PyQt6.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QLabel, QPushButton,
    QListWidget, QListWidgetItem, QGroupBox, QSplitter, QFrame, QGridLayout,
)
from PyQt6.QtCore import Qt

import pyqtgraph as pg

from app.core.firebase_client import FirebaseClient
from app.core.dsp import DEFAULT_FS, roll_win, rolling_std, time_axis
from app.core.labels import ACTIVITIES
from app.core.pace import format_pace, speed_kmh
from app.ui import theme
from app.ui.theme import set_tone, style_plot, plot_title, qcolor

ALL = "Semua"
NO_PACE = "Tanpa pace"


def _as_pace(p):
    return int(p) if isinstance(p, (int, float)) and p > 0 else None


def _segments(t: dict) -> list:
    """(activity, pace or None, start_s) per segment, for every take format:
    segments (sekarang), take pace-saja, atau take lama berlabel aktivitas."""
    segs = t.get("segments")
    if isinstance(segs, list) and segs:
        return [(s.get("activity", "?"), _as_pace(s.get("pace_s_per_km")), s.get("start_s", 0.0))
                for s in segs if isinstance(s, dict)]
    pace = _as_pace(t.get("pace_s_per_km"))
    if pace:
        return [("Running", pace, 0.0)]
    acts = t.get("activities") or [t.get("label", "?")]
    return [(a, None, 0.0) for a in acts]


def _pace_bucket(pace) -> str:
    """"5:xx" for pace 5:00–5:59 /km."""
    return f"{pace // 60}:xx" if pace else NO_PACE


def _bucket_key(b: str):
    return (1, 0) if b == NO_PACE else (0, int(b.split(":")[0]))


class HistoryTab(QWidget):
    def __init__(self, firebase: FirebaseClient):
        super().__init__()
        self.firebase = firebase
        self._sessions_data: dict = {}
        self._act_filter  = ALL
        self._pace_filter = ALL
        self._act_btns:  dict[str, QPushButton] = {}
        self._pace_btns: dict[str, QPushButton] = {}
        self._sep_lines: list = []   # [(plot, InfiniteLine), ...]
        self._build_ui()
        theme.manager().changed.connect(self._apply_theme)
        self._apply_theme(theme.theme())

    # ── UI ────────────────────────────────────────────────────────────────────

    def _build_ui(self):
        root = QHBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(self._build_left())
        splitter.addWidget(self._build_right())
        splitter.setSizes([320, 960])
        root.addWidget(splitter)

    # ── left panel ────────────────────────────────────────────────────────────

    def _build_left(self) -> QWidget:
        w = QWidget(); w.setMaximumWidth(340)
        lay = QVBoxLayout(w); lay.setSpacing(6)

        # --- filter: aktivitas (tetap) + pace (dibuat ulang setiap refresh) ---
        filter_box = QGroupBox("Filter")
        fl = QVBoxLayout(filter_box)
        act_row = QGridLayout(); act_row.setSpacing(6)
        for i, key in enumerate([ALL] + ACTIVITIES):
            btn = QPushButton(key)
            btn.setCheckable(True)
            btn.setProperty("activity" if key in ACTIVITIES else "chip",
                            key if key in ACTIVITIES else True)
            btn.clicked.connect(lambda checked, k=key: self._set_act_filter(k))
            act_row.addWidget(btn, i // 3, i % 3)
            self._act_btns[key] = btn
        fl.addLayout(act_row)
        self._pace_lay = QHBoxLayout(); self._pace_lay.setSpacing(4)
        fl.addLayout(self._pace_lay)
        lay.addWidget(filter_box)
        self._style_act_buttons()
        self._rebuild_pace_buttons([])

        # --- session list ---
        sess_box = QGroupBox("Sessions")
        sl = QVBoxLayout(sess_box)
        self._sess_list = QListWidget()
        self._sess_list.setProperty("dense", True)
        self._sess_list.currentItemChanged.connect(self._on_session_selected)
        sl.addWidget(self._sess_list)
        self._refresh_btn = QPushButton("↻  Refresh dari Firebase")
        self._refresh_btn.clicked.connect(self.refresh)
        sl.addWidget(self._refresh_btn)
        lay.addWidget(sess_box, stretch=1)

        # --- take list ---
        take_box = QGroupBox("Takes")
        tl = QVBoxLayout(take_box)
        self._take_list = QListWidget()
        self._take_list.setProperty("dense", True)
        self._take_list.currentItemChanged.connect(self._on_take_selected)
        tl.addWidget(self._take_list)
        lay.addWidget(take_box, stretch=1)

        return w

    def _style_act_buttons(self):
        for k, btn in self._act_btns.items():
            btn.setChecked(k == self._act_filter)

    def _rebuild_pace_buttons(self, buckets: list):
        while self._pace_lay.count():
            item = self._pace_lay.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        self._pace_btns.clear()
        if self._pace_filter not in buckets:
            self._pace_filter = ALL
        lbl = QLabel("Pace")
        set_tone(lbl, "muted")
        self._pace_lay.addWidget(lbl)
        for key in [ALL] + buckets:
            btn = QPushButton(key)
            btn.setCheckable(True)
            btn.setProperty("chip", True)
            btn.setChecked(key == self._pace_filter)
            btn.clicked.connect(lambda checked, k=key: self._set_pace_filter(k))
            self._pace_lay.addWidget(btn)
            self._pace_btns[key] = btn
        self._pace_lay.addStretch()

    # ── right panel ───────────────────────────────────────────────────────────

    def _build_right(self) -> QWidget:
        w = QWidget()
        lay = QVBoxLayout(w); lay.setSpacing(6)

        # --- info card ---
        self._info_card = self._build_info_card()
        lay.addWidget(self._info_card)

        # --- 6 plots (3×2) ---
        self._glw = pg.GraphicsLayoutWidget()
        ax_col = theme.theme().axis

        self._p_acc = self._glw.addPlot(row=0, col=0)
        self._p_gyr = self._glw.addPlot(row=0, col=1)
        plot_title(self._p_acc, "Accel X/Y/Z  (g)")
        plot_title(self._p_gyr, "Gyro X/Y/Z  (°/s)")
        for p in (self._p_acc, self._p_gyr):
            p.showGrid(x=True, y=True, alpha=theme.theme().grid_alpha)
            p.setLabel("bottom", "detik")
        self._p_acc.setYRange(-4, 4)
        self._p_gyr.setYRange(-500, 500)

        self._p_acc.addLegend(offset=(8, 4), colCount=3, labelTextSize="8pt")
        self._c_acc = {
            k: self._p_acc.plot(pen=pg.mkPen(ax_col[k], width=1), name=f"a{k.lower()}")
            for k in ("X", "Y", "Z")
        }
        self._p_gyr.addLegend(offset=(8, 4), colCount=3, labelTextSize="8pt")
        self._c_gyr = {
            k: self._p_gyr.plot(pen=pg.mkPen(ax_col[k], width=1), name=f"g{k.lower()}")
            for k in ("X", "Y", "Z")
        }

        # Row 1: std bar charts
        self._p_acc_std = self._glw.addPlot(row=1, col=0)
        self._p_gyr_std = self._glw.addPlot(row=1, col=1)
        plot_title(self._p_acc_std, "Std Accel  (g)")
        plot_title(self._p_gyr_std, "Std Gyro  (°/s)")
        colors = [ax_col[k] for k in ("X", "Y", "Z")]
        self._bar_acc = pg.BarGraphItem(x=[0, 1, 2], height=[0, 0, 0], width=0.6, brushes=colors)
        self._bar_gyr = pg.BarGraphItem(x=[0, 1, 2], height=[0, 0, 0], width=0.6, brushes=colors)
        self._p_acc_std.addItem(self._bar_acc)
        self._p_gyr_std.addItem(self._bar_gyr)
        for p in (self._p_acc_std, self._p_gyr_std):
            p.showGrid(y=True, alpha=theme.theme().grid_alpha)
            p.setXRange(-0.5, 2.5)
            p.getAxis("bottom").setTicks([[(0, "X"), (1, "Y"), (2, "Z")]])

        # Row 2: rolling std line charts
        self._p_racc = self._glw.addPlot(row=2, col=0)
        self._p_rgyr = self._glw.addPlot(row=2, col=1)
        plot_title(self._p_racc, "Rolling Std Accel  (g)")
        plot_title(self._p_rgyr, "Rolling Std Gyro  (°/s)")
        for p in (self._p_racc, self._p_rgyr):
            p.showGrid(x=True, y=True, alpha=theme.theme().grid_alpha)
            p.setLabel("bottom", "detik")
            p.setYRange(0, 1)
        self._p_racc.addLegend(offset=(8, 4), colCount=3, labelTextSize="8pt")
        self._c_racc = {
            k: self._p_racc.plot(pen=pg.mkPen(ax_col[k], width=1), name=f"σa{k.lower()}")
            for k in ("X", "Y", "Z")
        }
        self._p_rgyr.addLegend(offset=(8, 4), colCount=3, labelTextSize="8pt")
        self._c_rgyr = {
            k: self._p_rgyr.plot(pen=pg.mkPen(ax_col[k], width=1), name=f"σg{k.lower()}")
            for k in ("X", "Y", "Z")
        }

        plot_card = QFrame()
        plot_card.setProperty("card", True)
        pc = QVBoxLayout(plot_card)
        pc.setContentsMargins(6, 6, 6, 6)
        pc.addWidget(self._glw)
        for row, stretch in ((0, 5), (1, 3), (2, 5)):
            self._glw.ci.layout.setRowStretchFactor(row, stretch)
        lay.addWidget(plot_card, stretch=1)
        return w

    def _build_info_card(self) -> QFrame:
        card = QFrame()
        card.setProperty("card", True)
        card.setFixedHeight(68)
        lay = QVBoxLayout(card)
        lay.setContentsMargins(14, 8, 14, 8)
        lay.setSpacing(3)

        self._info_session = QLabel("Belum ada session dipilih")
        set_tone(self._info_session, "title")

        self._info_take = QLabel("")
        set_tone(self._info_take, "muted")

        lay.addWidget(self._info_session)
        lay.addWidget(self._info_take)
        return card

    # ── theme ─────────────────────────────────────────────────────────────────

    def _apply_theme(self, t):
        self._glw.setBackground(t.plot_bg)
        plots = (self._p_acc, self._p_gyr, self._p_acc_std, self._p_gyr_std,
                 self._p_racc, self._p_rgyr)
        for p in plots:
            style_plot(p, t)
        for curves in (self._c_acc, self._c_gyr, self._c_racc, self._c_rgyr):
            for k, c in curves.items():
                c.setPen(pg.mkPen(t.axis[k], width=1))
        brushes = [pg.mkBrush(t.axis[k]) for k in ("X", "Y", "Z")]
        for bar in (self._bar_acc, self._bar_gyr):
            bar.setOpts(brushes=brushes, pens=[pg.mkPen(None)] * 3)
        item = self._take_list.currentItem()
        if item is not None:
            self._on_take_selected(item, None)     # gambar ulang garis segmen

    # ── filter ────────────────────────────────────────────────────────────────

    def _set_act_filter(self, key: str):
        self._act_filter = key
        self._style_act_buttons()
        self._apply_filter()

    def _set_pace_filter(self, key: str):
        self._pace_filter = key
        for k, btn in self._pace_btns.items():
            btn.setChecked(k == key)
        self._apply_filter()

    def _seg_matches(self, act: str, pace) -> bool:
        return ((self._act_filter == ALL or act == self._act_filter)
                and (self._pace_filter == ALL or _pace_bucket(pace) == self._pace_filter))

    def _take_matches(self, t: dict) -> bool:
        """True if one segment matches both the activity and the pace filter."""
        return any(self._seg_matches(act, pace) for act, pace, _ in _segments(t))

    def _apply_filter(self):
        self._sess_list.clear()
        self._take_list.clear()
        for sess_name, sess_data in self._sessions_data.items():
            if not isinstance(sess_data, dict):
                continue
            takes = [t for t in sess_data.get("takes", {}).values() if isinstance(t, dict)]
            match = [t for t in takes if self._take_matches(t)]
            if not match:
                continue
            paces = sorted(p for t in match for a, p, _ in _segments(t)
                           if p and self._seg_matches(a, p))
            if paces:
                rng = (format_pace(paces[0]) if paces[0] == paces[-1]
                       else f"{format_pace(paces[0])}–{format_pace(paces[-1])}")
                info = f"{len(match)} takes  ·  {rng} /km"
            else:
                info = f"{len(match)} takes"
            item = QListWidgetItem(f"{sess_name}  ·  {info}")
            item.setData(Qt.ItemDataRole.UserRole, sess_name)
            self._sess_list.addItem(item)

    # ── data ─────────────────────────────────────────────────────────────────

    def refresh(self):
        self._sess_list.clear()
        self._take_list.clear()
        self._info_session.setText("Memuat…")
        self._info_take.setText("")
        data, err = self.firebase.fetch_sessions()
        if err:
            self._info_session.setText(f"Error: {err}")
            return
        self._sessions_data = data or {}
        buckets = sorted({
            _pace_bucket(pace)
            for s in self._sessions_data.values() if isinstance(s, dict)
            for t in s.get("takes", {}).values() if isinstance(t, dict)
            for _, pace, _ in _segments(t)
        }, key=_bucket_key)
        self._rebuild_pace_buttons(buckets)
        self._apply_filter()
        self._info_session.setText("Belum ada session dipilih")

    def _on_session_selected(self, item, _prev):
        if item is None:
            return
        sess_name = item.data(Qt.ItemDataRole.UserRole)
        sess_data = self._sessions_data.get(sess_name, {})
        takes     = sess_data.get("takes", {})
        meta      = sess_data.get("meta", {})

        total = meta.get("total_takes", len(takes))
        kept  = meta.get("kept_takes", "?")
        self._info_session.setText(sess_name)
        self._info_take.setText(f"{total} takes direkam  ·  {kept} disimpan")

        self._take_list.clear()
        for take_key, take_data in sorted(takes.items()):
            if not isinstance(take_data, dict) or not self._take_matches(take_data):
                continue
            item2 = QListWidgetItem(f"{take_key}  ·  {self._take_label(take_data)}  ·  "
                                    f"{take_data.get('duration_s', 0):.1f}s")
            item2.setData(Qt.ItemDataRole.UserRole, (sess_name, take_key))
            self._take_list.addItem(item2)

    @staticmethod
    def _take_label(t: dict) -> str:
        segs = _segments(t)
        label = " → ".join(f"{a} {format_pace(p)}" if p else a for a, p, _ in segs)
        paces = {p for _, p, _ in segs if p}
        if len(paces) == 1:
            label += f" ({speed_kmh(next(iter(paces))):.1f} km/h)"
        return label

    def _on_take_selected(self, item, _prev):
        if item is None:
            return
        sess_name, take_key = item.data(Qt.ItemDataRole.UserRole)
        take_data = self._sessions_data.get(sess_name, {}).get("takes", {}).get(take_key, {})
        nodes     = take_data.get("nodes", {})

        if not nodes:
            self._info_take.setText("Tidak ada data node.")
            return

        node_key  = next(iter(nodes))
        node_data = nodes[node_key]
        raw       = node_data.get("raw", [])
        dur       = take_data.get("duration_s", 0)
        n_samp    = node_data.get("n_samples", len(raw))
        # take lama (sebelum GS v5) belum menyimpan sample_rate_hz -> anggap 50 Hz
        fs        = int(take_data.get("sample_rate_hz", DEFAULT_FS)) or DEFAULT_FS
        trim      = take_data.get("trim_s")
        trim_txt  = f"  ·  potong {trim[0]:.1f}–{trim[1]:.1f} s" if trim else ""

        self._info_take.setText(
            f"{take_key}  ·  {self._take_label(take_data)}  ·  {dur:.1f}s  ·  "
            f"{n_samp} sampel  ·  {fs} Hz  ·  {node_key}{trim_txt}"
        )

        if not raw:
            return

        arr = np.array(raw, dtype=float)
        xx  = time_axis(arr, fs)

        for i, k in enumerate(("X", "Y", "Z")):
            self._c_acc[k].setData(xx, arr[:, i])
            self._c_gyr[k].setData(xx, arr[:, 3 + i])

        std_a = arr[:, 0:3].std(axis=0)
        std_g = arr[:, 3:6].std(axis=0)
        self._bar_acc.setOpts(height=std_a)
        self._bar_gyr.setOpts(height=std_g)
        self._p_acc_std.setYRange(0, max(std_a.max() * 1.4, 0.05))
        self._p_gyr_std.setYRange(0, max(std_g.max() * 1.4, 5.0))

        s_acc, s_gyr = rolling_std(arr, roll_win(fs))
        for i, k in enumerate(("X", "Y", "Z")):
            self._c_racc[k].setData(xx, s_acc[:, i])
            self._c_rgyr[k].setData(xx, s_gyr[:, i])
        if s_acc.max() > 0:
            self._p_racc.setYRange(0, max(s_acc.max() * 1.4, 0.05))
        if s_gyr.max() > 0:
            self._p_rgyr.setYRange(0, max(s_gyr.max() * 1.4, 5.0))

        segs = take_data.get("segments")
        if isinstance(segs, list) and len(segs) > 1:
            self._draw_segment_lines(segs)
        else:
            self._draw_separators(take_data.get("label_changes", []), xx)

    # ── separator lines ──────────────────────────────────────────────────────

    def _draw_segment_lines(self, segs: list):
        self._clear_separators()
        line_plots = (self._p_acc, self._p_gyr, self._p_racc, self._p_rgyr)
        for seg in segs[1:]:
            act = seg.get("activity", "?")
            t = theme.theme()
            color = t.activity.get(act, t.text)
            label = f"{act} {seg.get('pace')}" if seg.get("pace") else act
            for p in line_plots:
                line = pg.InfiniteLine(
                    pos=float(seg.get("start_s", 0.0)), angle=90,
                    pen=pg.mkPen(color, width=1.5, style=Qt.PenStyle.DashLine),
                    label=label,
                    labelOpts={"position": 0.92, "color": color, "fill": qcolor(t.surface, 0.85)},
                )
                p.addItem(line)
                self._sep_lines.append((p, line))

    def _clear_separators(self):
        for plot, line in self._sep_lines:
            plot.removeItem(line)
        self._sep_lines = []

    def _draw_separators(self, label_changes: list, xx: np.ndarray):
        self._clear_separators()
        if len(label_changes) <= 1:
            return
        line_plots = (self._p_acc, self._p_gyr, self._p_racc, self._p_rgyr)
        for entry in label_changes[1:]:   # skip first (sample 0)
            sample_idx = entry.get("sample_idx", 0) if isinstance(entry, dict) else entry[0]
            label      = entry.get("label", "?")    if isinstance(entry, dict) else entry[1]
            t = theme.theme()
            color = t.activity.get(label, t.text)
            for p in line_plots:
                line = pg.InfiniteLine(
                    pos=float(xx[min(sample_idx, len(xx) - 1)]), angle=90,
                    pen=pg.mkPen(color, width=1.5, style=Qt.PenStyle.DashLine),
                    label=label,
                    labelOpts={"position": 0.9, "color": color, "fill": qcolor(t.surface, 0.85)},
                )
                p.addItem(line)
                self._sep_lines.append((p, line))

"""
Settings Tab — tampilan (tema), Firebase, serial.
Changes are applied immediately (or on reconnect where noted).
"""

from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QFormLayout, QLineEdit, QPushButton,
    QGroupBox, QLabel, QSpinBox, QFileDialog, QComboBox,
)
from PyQt6.QtCore import Qt

from app.core.firebase_client import FirebaseClient
from app.ui import theme


class SettingsTab(QWidget):
    def __init__(self, firebase: FirebaseClient, parent=None):
        super().__init__(parent)
        self.firebase = firebase
        self._build_ui()
        theme.manager().changed.connect(self._on_theme_changed)

    def _build_ui(self):
        outer = QHBoxLayout(self)
        outer.setContentsMargins(24, 20, 24, 20)
        col = QVBoxLayout()
        col.setSpacing(16)
        col.setAlignment(Qt.AlignmentFlag.AlignTop)
        outer.addLayout(col, stretch=0)
        outer.addStretch(1)
        self.setMinimumWidth(0)

        # ── Tampilan ──────────────────────────────────────────────────────────
        ui_box = QGroupBox("Tampilan")
        ui_box.setMinimumWidth(520)
        ui_lay = QFormLayout(ui_box)
        self._theme_cb = QComboBox()
        for key, label in theme.MODES.items():
            self._theme_cb.addItem(label, userData=key)
        self._theme_cb.setCurrentIndex(list(theme.MODES).index(theme.manager().mode))
        self._theme_cb.activated.connect(
            lambda _: theme.manager().set_mode(self._theme_cb.currentData()))
        ui_lay.addRow("Tema:", self._theme_cb)
        col.addWidget(ui_box)

        # ── Firebase settings ─────────────────────────────────────────────────
        fb_box = QGroupBox("Firebase")
        fb_box.setMinimumWidth(520)
        fb_lay = QFormLayout(fb_box)

        self._key_edit = QLineEdit(self.firebase.key_path)
        key_row = QWidget()
        key_rl = QHBoxLayout(key_row)
        key_rl.setContentsMargins(0, 0, 0, 0)
        browse_btn = QPushButton("Browse…")
        browse_btn.clicked.connect(self._browse_key)
        key_rl.addWidget(self._key_edit); key_rl.addWidget(browse_btn)

        self._db_url_edit = QLineEdit(self.firebase.db_url)

        self._fb_status = QLabel("")
        theme.set_tone(self._fb_status, "muted")

        reconnect_btn = QPushButton("Reconnect Firebase")
        reconnect_btn.clicked.connect(self._reconnect_firebase)

        fb_lay.addRow("Service Account Key:", key_row)
        fb_lay.addRow("Database URL:", self._db_url_edit)
        fb_lay.addRow("", reconnect_btn)
        fb_lay.addRow("Status:", self._fb_status)
        col.addWidget(fb_box)

        # ── Serial settings ───────────────────────────────────────────────────
        ser_box = QGroupBox("Serial  ·  berlaku setelah Connect ulang")
        ser_box.setMinimumWidth(520)
        ser_lay = QFormLayout(ser_box)

        self._baud_spin = QSpinBox()
        self._baud_spin.setRange(9600, 921600)
        self._baud_spin.setValue(921600)
        self._baud_spin.setSingleStep(9600)

        self._win_spin = QSpinBox()
        self._win_spin.setRange(1, 30)
        self._win_spin.setValue(5)
        self._win_spin.setSuffix(" s")

        ser_lay.addRow("Baud Rate:", self._baud_spin)
        ser_lay.addRow("Plot Window:", self._win_spin)
        note = QLabel("Sample rate diatur di panel Ground Station (tab Collect).")
        theme.set_tone(note, "faint")
        ser_lay.addRow(note)
        col.addWidget(ser_box)

    # ── actions ───────────────────────────────────────────────────────────────

    def _on_theme_changed(self, _t):
        mode = theme.manager().mode
        self._theme_cb.setCurrentIndex(list(theme.MODES).index(mode))

    def _browse_key(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Pilih Service Account Key", "", "JSON Files (*.json)"
        )
        if path:
            self._key_edit.setText(path)

    def _reconnect_firebase(self):
        self.firebase.key_path = self._key_edit.text().strip()
        self.firebase.db_url   = self._db_url_edit.text().strip()
        # reset existing app
        try:
            import firebase_admin
            if firebase_admin._apps:
                firebase_admin.delete_app(firebase_admin.get_app())
        except Exception:
            pass
        self.firebase._ready = False
        ok, msg = self.firebase.connect()
        self._fb_status.setText(msg)
        theme.set_tone(self._fb_status, "success" if ok else "danger")

    # ── accessors for collect tab ─────────────────────────────────────────────

    @property
    def baud(self) -> int:
        return self._baud_spin.value()

    @property
    def window_sec(self) -> int:
        return self._win_spin.value()

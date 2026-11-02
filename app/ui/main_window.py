"""
ActivTrack — Main Window
QMainWindow with three tabs: Collect | History | Settings
"""

from PyQt6.QtWidgets import (
    QMainWindow, QTabWidget, QStatusBar, QLabel, QWidget, QPushButton
)

from app.core.firebase_client import FirebaseClient
from app.ui import theme
from app.ui.collect_tab import CollectTab
from app.ui.history_tab import HistoryTab
from app.ui.settings_tab import SettingsTab


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ActivTrack — Data Collector")
        self.resize(1320, 840)

        self.firebase = FirebaseClient()

        self._build_ui()
        self._init_firebase()
        theme.manager().changed.connect(self._on_theme_changed)
        self._on_theme_changed(theme.theme())

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self):
        self.tabs = QTabWidget()
        self.tabs.setDocumentMode(True)
        self.setCentralWidget(self.tabs)

        self.collect_tab  = CollectTab(self.firebase)
        self.history_tab  = HistoryTab(self.firebase)
        self.settings_tab = SettingsTab(self.firebase, self)
        self.collect_tab.settings = self.settings_tab

        self.tabs.addTab(self.collect_tab,  "Collect")
        self.tabs.addTab(self.history_tab,  "History")
        self.tabs.addTab(self.settings_tab, "Settings")

        # status bar
        self._status_serial   = QLabel("Serial: —")
        self._status_firebase = QLabel("Firebase: —")
        self._status_nodes    = QLabel("Nodes: —")
        theme.set_tone(self._status_serial, "muted")
        theme.set_tone(self._status_firebase, "muted")
        theme.set_tone(self._status_nodes, "muted")

        self._theme_btn = QPushButton()
        self._theme_btn.setProperty("role", "ghost")
        self._theme_btn.setToolTip("Ganti tema Gelap / Terang (pilihan lain di Settings)")
        self._theme_btn.clicked.connect(lambda: theme.manager().toggle())

        sb = QStatusBar()
        sb.setSizeGripEnabled(False)
        sb.addWidget(self._status_serial)
        sb.addWidget(_separator())
        sb.addWidget(self._status_nodes)
        sb.addPermanentWidget(self._status_firebase)
        sb.addPermanentWidget(_separator())
        sb.addPermanentWidget(self._theme_btn)
        self.setStatusBar(sb)

        # wire collect tab → status bar
        self.collect_tab.serial_status_changed.connect(self._on_serial_status)
        self.collect_tab.nodes_changed.connect(self._on_nodes_changed)

        # refresh history when switching to that tab
        self.tabs.currentChanged.connect(self._on_tab_changed)

    # ── Firebase init ─────────────────────────────────────────────────────────

    def _init_firebase(self):
        ok, msg = self.firebase.connect()
        self._status_firebase.setText(f"Firebase: {msg}")
        theme.set_tone(self._status_firebase, "success" if ok else "warning")

    # ── slots ─────────────────────────────────────────────────────────────────

    def _on_theme_changed(self, t):
        self._theme_btn.setText("☀  Terang" if t.dark else "☾  Gelap")

    def _on_serial_status(self, msg: str, ok: bool):
        self._status_serial.setText(f"Serial: {msg}")
        theme.set_tone(self._status_serial, "success" if ok else "danger")

    def _on_nodes_changed(self, nodes: list):
        if nodes:
            self._status_nodes.setText(f"Nodes: {', '.join(str(n) for n in nodes)}")
        else:
            self._status_nodes.setText("Nodes: —")

    def _on_tab_changed(self, index: int):
        if self.tabs.widget(index) is self.history_tab:
            self.history_tab.refresh()

    def closeEvent(self, event):
        self.collect_tab.cleanup()
        super().closeEvent(event)


def _separator() -> QWidget:
    sep = QLabel("·")
    theme.set_tone(sep, "faint")
    return sep

"""
Theme system — Gelap (default) / Terang / Ikuti sistem, switchable at runtime.

Widgets never hard-code colours. They use:
  * dynamic properties that the global stylesheet understands
      QPushButton[role="primary"|"success"|"danger"|"rec"|"ghost"]
      QPushButton[activity="Running"] (checkable chip),  QPushButton[chip="true"]
      QLabel[tone="muted"|"faint"|"accent"|"success"|"warning"|"danger"|"title"]
      QFrame[card="true"],  QLineEdit[invalid="true"],  *[dense="true"]
  * theme() tokens for things drawn outside Qt stylesheets (pyqtgraph plots,
    item foregrounds, rich text)
and connect to manager().changed to restyle when the theme switches.
"""

import os
import tempfile
from dataclasses import dataclass, field

import pyqtgraph as pg
from PyQt6.QtCore import QObject, QSettings, Qt, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QGuiApplication, QPalette
from PyQt6.QtWidgets import QApplication, QWidget

MODES = {"dark": "Gelap", "light": "Terang", "system": "Ikuti sistem"}


@dataclass(frozen=True)
class Theme:
    name: str
    dark: bool
    bg: str             # window
    surface: str        # cards, group boxes, tables
    raised: str         # inputs, buttons
    hover: str
    alt_row: str
    border: str
    border_strong: str
    text: str
    muted: str
    faint: str
    accent: str
    accent_hover: str
    on_accent: str
    success: str
    danger: str
    warning: str
    plot_bg: str
    plot_axis: str
    plot_text: str
    grid_alpha: float
    axis: dict = field(default_factory=dict)       # X/Y/Z curve colours
    activity: dict = field(default_factory=dict)   # Standing/Sitting/Walking/Running
    node_state: dict = field(default_factory=dict)


DARK = Theme(
    name="dark", dark=True,
    bg="#0d1117", surface="#141a22", raised="#1a212b", hover="#222a36", alt_row="#171d26",
    border="#232b36", border_strong="#303a48",
    text="#e6edf3", muted="#9aa5b4", faint="#5f6b7a",
    accent="#6ea8fe", accent_hover="#8cbaff", on_accent="#08111f",
    success="#3fb970", danger="#f0625a", warning="#e0a83a",
    plot_bg="#141a22", plot_axis="#3a4554", plot_text="#8a96a6", grid_alpha=0.45,
    axis={"X": "#ff8a80", "Y": "#69d49a", "Z": "#74b4ff"},
    activity={"Standing": "#74b4ff", "Sitting": "#b89cff", "Walking": "#f2a65e", "Running": "#69d49a"},
    node_state={"RUN": "#3fb970", "STP": "#e0a83a", "OFF": "#f0625a", "CAL": "#6ea8fe",
                "---": "#5f6b7a"},
)

LIGHT = Theme(
    name="light", dark=False,
    bg="#f3f5f8", surface="#ffffff", raised="#ffffff", hover="#eef1f5", alt_row="#f8f9fb",
    border="#e1e5eb", border_strong="#c9d0da",
    text="#1b222c", muted="#5a6472", faint="#98a1ad",
    accent="#2f6fdb", accent_hover="#255fc2", on_accent="#ffffff",
    success="#1e8a4a", danger="#d1343b", warning="#a86b00",
    plot_bg="#ffffff", plot_axis="#c3cad4", plot_text="#5a6472", grid_alpha=0.55,
    axis={"X": "#d6333c", "Y": "#1e8a4a", "Z": "#1f66d1"},
    activity={"Standing": "#1f66d1", "Sitting": "#7c4dd6", "Walking": "#c25e00", "Running": "#1e8a4a"},
    node_state={"RUN": "#1e8a4a", "STP": "#a86b00", "OFF": "#d1343b", "CAL": "#2f6fdb",
                "---": "#98a1ad"},
)


def rgba(hex_color: str, alpha: float) -> str:
    c = QColor(hex_color)
    return f"rgba({c.red()}, {c.green()}, {c.blue()}, {int(alpha * 255)})"


def qcolor(hex_color: str, alpha: float = 1.0) -> QColor:
    c = QColor(hex_color)
    c.setAlphaF(alpha)
    return c


# ── stylesheet ────────────────────────────────────────────────────────────────

def _icon_dir() -> str:
    d = os.path.join(tempfile.gettempdir(), "activtrack_theme")
    os.makedirs(d, exist_ok=True)
    return d


def _chevron(color: str, up: bool) -> str:
    """Write a small chevron SVG in `color`, return a stylesheet-friendly path."""
    path_d = "M1 5l4-4 4 4" if up else "M1 1l4 4 4-4"
    name = f"chev_{'up' if up else 'down'}_{color.lstrip('#')}.svg"
    path = os.path.join(_icon_dir(), name)
    if not os.path.exists(path):
        with open(path, "w", encoding="utf-8") as f:
            f.write(
                '<svg xmlns="http://www.w3.org/2000/svg" width="10" height="6" viewBox="0 0 10 6">'
                f'<path d="{path_d}" fill="none" stroke="{color}" stroke-width="1.6" '
                'stroke-linecap="round" stroke-linejoin="round"/></svg>'
            )
    return path.replace("\\", "/")


def build_stylesheet(t: Theme) -> str:
    down = _chevron(t.muted, up=False)
    up = _chevron(t.muted, up=True)
    sel = rgba(t.accent, 0.20)

    chips = []
    for act, c in t.activity.items():
        chips.append(f"""
QPushButton[activity="{act}"] {{
    background: transparent; color: {c}; border: 1px solid {rgba(c, 0.40)};
    border-radius: 15px; padding: 5px 13px; font-weight: 500;
}}
QPushButton[activity="{act}"]:hover {{ background: {rgba(c, 0.12)}; border-color: {c}; }}
QPushButton[activity="{act}"]:checked {{
    background: {c}; color: {t.bg}; border-color: {c}; font-weight: 600;
}}""")

    return f"""
* {{ outline: 0; }}
QWidget {{ color: {t.text}; }}
QMainWindow, QDialog {{ background: {t.bg}; }}
QToolTip {{
    background: {t.raised}; color: {t.text}; border: 1px solid {t.border_strong};
    border-radius: 6px; padding: 6px 8px;
}}

/* tabs */
QTabWidget::pane {{ border: none; border-top: 1px solid {t.border}; background: {t.bg}; }}
QTabBar {{ background: transparent; }}
QTabBar::tab {{
    background: transparent; color: {t.muted}; border: none;
    border-bottom: 2px solid transparent; padding: 10px 20px; margin-right: 2px; font-weight: 600;
}}
QTabBar::tab:hover {{ color: {t.text}; }}
QTabBar::tab:selected {{ color: {t.text}; border-bottom: 2px solid {t.accent}; }}

/* cards */
QGroupBox {{
    background: {t.surface}; border: 1px solid {t.border}; border-radius: 10px;
    margin-top: 16px; padding: 14px 10px 10px 10px;
}}
QGroupBox::title {{
    subcontrol-origin: margin; subcontrol-position: top left;
    left: 12px; top: 3px; padding: 0 4px; color: {t.muted}; font-weight: 600;
}}
QFrame[card="true"] {{ background: {t.surface}; border: 1px solid {t.border}; border-radius: 10px; }}
QSplitter::handle {{ background: transparent; width: 8px; }}

/* labels */
QLabel {{ background: transparent; }}
QLabel[tone="muted"]   {{ color: {t.muted}; }}
QLabel[tone="faint"]   {{ color: {t.faint}; }}
QLabel[tone="accent"]  {{ color: {t.accent}; font-weight: 600; }}
QLabel[tone="success"] {{ color: {t.success}; }}
QLabel[tone="warning"] {{ color: {t.warning}; }}
QLabel[tone="danger"]  {{ color: {t.danger}; }}
QLabel[tone="title"]   {{ color: {t.text}; font-size: 12pt; font-weight: 600; }}
*[dense="true"] {{ font-size: 9pt; }}

/* buttons */
QPushButton {{
    background: {t.raised}; color: {t.text}; border: 1px solid {t.border_strong};
    border-radius: 7px; padding: 6px 14px; min-height: 18px;
}}
QPushButton:hover {{ background: {t.hover}; border-color: {t.faint}; }}
QPushButton:pressed {{ background: {t.border}; }}
QPushButton:checked {{ background: {rgba(t.accent, 0.16)}; color: {t.accent}; border-color: {t.accent}; }}
QPushButton:disabled {{ background: {t.surface}; color: {t.faint}; border-color: {t.border}; }}

QPushButton[role="primary"] {{
    background: {t.accent}; color: {t.on_accent}; border: 1px solid {t.accent}; font-weight: 600;
}}
QPushButton[role="primary"]:hover {{ background: {t.accent_hover}; border-color: {t.accent_hover}; }}
QPushButton[role="success"] {{
    background: {t.success}; color: #ffffff; border: 1px solid {t.success}; font-weight: 600;
}}
QPushButton[role="success"]:hover {{ background: {rgba(t.success, 0.85)}; }}
QPushButton[role="danger"] {{
    background: transparent; color: {t.danger}; border: 1px solid {rgba(t.danger, 0.55)}; font-weight: 600;
}}
QPushButton[role="danger"]:hover {{ background: {rgba(t.danger, 0.12)}; border-color: {t.danger}; }}
QPushButton[role="rec"] {{
    background: transparent; color: {t.danger}; border: 1.5px solid {t.danger};
    border-radius: 9px; padding: 7px 16px; font-weight: 700;
}}
QPushButton[role="rec"]:hover {{ background: {rgba(t.danger, 0.12)}; }}
QPushButton[role="rec"]:checked {{ background: {t.danger}; color: #ffffff; }}
QPushButton[role="ghost"] {{ background: transparent; border: 1px solid transparent; color: {t.muted}; }}
QPushButton[role="ghost"]:hover {{ background: {t.hover}; color: {t.text}; }}
QPushButton[role="primary"]:disabled, QPushButton[role="success"]:disabled,
QPushButton[role="danger"]:disabled, QPushButton[role="rec"]:disabled {{
    background: {t.surface}; color: {t.faint}; border: 1px solid {t.border};
}}
QPushButton[chip="true"] {{
    background: transparent; color: {t.muted}; border: 1px solid {t.border_strong};
    border-radius: 13px; padding: 3px 11px;
}}
QPushButton[chip="true"]:hover {{ color: {t.text}; border-color: {t.faint}; }}
QPushButton[chip="true"]:checked {{
    background: {t.accent}; color: {t.on_accent}; border-color: {t.accent}; font-weight: 600;
}}
{''.join(chips)}

/* inputs */
QLineEdit, QSpinBox, QComboBox {{
    background: {t.raised}; color: {t.text}; border: 1px solid {t.border_strong};
    border-radius: 7px; padding: 5px 8px; min-height: 18px;
    selection-background-color: {t.accent}; selection-color: {t.on_accent};
}}
QLineEdit:hover, QSpinBox:hover, QComboBox:hover {{ border-color: {t.faint}; }}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {{ border: 1px solid {t.accent}; }}
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {{
    background: {t.surface}; color: {t.faint}; border-color: {t.border};
}}
QLineEdit[invalid="true"] {{ border: 1px solid {t.danger}; }}
QLineEdit[size="big"] {{ font-size: 12pt; font-weight: 600; }}
QComboBox {{ padding-right: 26px; }}
QComboBox::drop-down {{ border: none; width: 24px; subcontrol-position: center right; }}
QComboBox::down-arrow {{ image: url({down}); width: 10px; height: 6px; }}
QComboBox QAbstractItemView {{
    background: {t.raised}; color: {t.text}; border: 1px solid {t.border_strong};
    border-radius: 6px; padding: 4px; outline: 0;
    selection-background-color: {t.hover}; selection-color: {t.text};
}}
QSpinBox {{ padding-right: 22px; }}
QSpinBox::up-button, QSpinBox::down-button {{
    subcontrol-origin: border; width: 20px; border: none; background: transparent;
}}
QSpinBox::up-button {{ subcontrol-position: top right; }}
QSpinBox::down-button {{ subcontrol-position: bottom right; }}
QSpinBox::up-arrow {{ image: url({up}); width: 9px; height: 6px; }}
QSpinBox::down-arrow {{ image: url({down}); width: 9px; height: 6px; }}

/* tables & lists */
QTableWidget, QTableView, QListWidget {{
    background: {t.surface}; alternate-background-color: {t.alt_row};
    border: 1px solid {t.border}; border-radius: 8px; gridline-color: {t.border};
    selection-background-color: {sel}; selection-color: {t.text};
}}
QTableWidget::item, QTableView::item {{ padding: 3px 6px; border: none; }}
QTableWidget::item:selected, QTableView::item:selected {{ background: {sel}; color: {t.text}; }}
QHeaderView {{ background: transparent; }}
QHeaderView::section {{
    background: {t.surface}; color: {t.muted}; border: none;
    border-bottom: 1px solid {t.border}; padding: 6px 8px; font-weight: 600;
}}
QTableCornerButton::section {{ background: {t.surface}; border: none; }}
QListWidget {{ padding: 4px; }}
QListWidget::item {{ padding: 7px 8px; border-radius: 6px; }}
QListWidget::item:hover {{ background: {t.hover}; }}
QListWidget::item:selected {{ background: {sel}; color: {t.text}; }}

/* scrollbars */
QScrollBar:vertical {{ background: transparent; width: 10px; margin: 2px; }}
QScrollBar:horizontal {{ background: transparent; height: 10px; margin: 2px; }}
QScrollBar::handle:vertical {{ background: {t.border_strong}; border-radius: 3px; min-height: 28px; }}
QScrollBar::handle:horizontal {{ background: {t.border_strong}; border-radius: 3px; min-width: 28px; }}
QScrollBar::handle:hover {{ background: {t.faint}; }}
QScrollBar::add-line, QScrollBar::sub-line {{ width: 0; height: 0; }}
QScrollBar::add-page, QScrollBar::sub-page {{ background: none; }}

/* status bar */
QStatusBar {{ background: {t.surface}; border-top: 1px solid {t.border}; color: {t.muted}; }}
QStatusBar::item {{ border: none; }}
QStatusBar QLabel {{ padding: 0 4px; }}

QMessageBox QLabel {{ color: {t.text}; }}
"""


def _palette(t: Theme) -> QPalette:
    p = QPalette()
    R = QPalette.ColorRole
    for role, col in (
        (R.Window, t.bg), (R.WindowText, t.text), (R.Base, t.raised),
        (R.AlternateBase, t.alt_row), (R.Text, t.text), (R.Button, t.raised),
        (R.ButtonText, t.text), (R.Highlight, t.accent), (R.HighlightedText, t.on_accent),
        (R.ToolTipBase, t.raised), (R.ToolTipText, t.text), (R.PlaceholderText, t.faint),
        (R.Link, t.accent), (R.Mid, t.border), (R.Midlight, t.border_strong),
        (R.Dark, t.bg), (R.Shadow, "#000000"), (R.Light, t.hover),
    ):
        p.setColor(role, QColor(col))
    for role in (R.Text, R.WindowText, R.ButtonText):
        p.setColor(QPalette.ColorGroup.Disabled, role, QColor(t.faint))
    return p


# ── manager ───────────────────────────────────────────────────────────────────

class ThemeManager(QObject):
    changed = pyqtSignal(object)    # Theme

    def __init__(self, app: QApplication):
        super().__init__()
        self._app = app
        self._settings = QSettings("ActivTrack", "ActivTrack")
        mode = self._settings.value("theme", "dark")
        self._mode = mode if mode in MODES else "dark"
        self._theme = DARK
        hints = QGuiApplication.styleHints()
        if hasattr(hints, "colorSchemeChanged"):
            hints.colorSchemeChanged.connect(lambda *_: self._mode == "system" and self.apply())

    @property
    def mode(self) -> str:
        return self._mode

    @property
    def theme(self) -> Theme:
        return self._theme

    def _resolve(self) -> Theme:
        if self._mode == "light":
            return LIGHT
        if self._mode == "system":
            hints = QGuiApplication.styleHints()
            scheme = getattr(hints, "colorScheme", lambda: None)()
            return LIGHT if scheme == Qt.ColorScheme.Light else DARK
        return DARK

    def set_mode(self, mode: str):
        if mode not in MODES:
            return
        self._mode = mode
        self._settings.setValue("theme", mode)
        self.apply()

    def toggle(self):
        """Gelap <-> Terang (from 'system' this switches away from what is shown)."""
        self.set_mode("light" if self._theme.dark else "dark")

    def apply(self):
        t = self._resolve()
        self._theme = t
        self._app.setPalette(_palette(t))
        self._app.setStyleSheet(build_stylesheet(t))
        pg.setConfigOption("background", t.plot_bg)
        pg.setConfigOption("foreground", t.plot_text)
        self.changed.emit(t)


_manager: ThemeManager | None = None


def init(app: QApplication) -> ThemeManager:
    """Call once, before building windows."""
    global _manager
    app.setStyle("Fusion")          # stylesheet & palette are honoured consistently
    font = QFont()
    font.setFamilies(["Segoe UI Variable Text", "Segoe UI", "Inter", "Helvetica Neue", "Arial"])
    font.setPointSizeF(9.75)
    app.setFont(font)
    pg.setConfigOptions(antialias=True)
    _manager = ThemeManager(app)
    _manager.apply()
    return _manager


def manager() -> ThemeManager:
    if _manager is None and QApplication.instance() is not None:
        init(QApplication.instance())       # mis. saat widget dibuat tanpa activtrack_app.py
    return _manager


def theme() -> Theme:
    m = manager()
    return m.theme if m else DARK


# ── widget helpers ────────────────────────────────────────────────────────────

def set_prop(w: QWidget, name: str, value):
    """Set a dynamic property and re-polish so the stylesheet picks it up."""
    if w.property(name) == value:
        return
    w.setProperty(name, value)
    w.style().unpolish(w)
    w.style().polish(w)


def set_tone(w: QWidget, tone: str):
    set_prop(w, "tone", tone)


def style_plot(p: pg.PlotItem, t: Theme):
    """Axes, grid, title and legend of one PlotItem in theme colours."""
    for name in ("left", "bottom"):
        ax = p.getAxis(name)
        ax.setPen(pg.mkPen(t.plot_axis))
        ax.setTextPen(pg.mkPen(t.plot_text))
        ax.setZValue(-1)            # grid di belakang data, bukan di atas bar/kurva
    grid = p.ctrl.xGridCheck.isChecked() or p.ctrl.yGridCheck.isChecked()
    if grid:
        p.showGrid(x=p.ctrl.xGridCheck.isChecked(), y=p.ctrl.yGridCheck.isChecked(),
                   alpha=t.grid_alpha)
    title = getattr(p, "_plain_title", None)
    if title:
        p.setTitle(title, color=t.muted, size="9pt")
    legend = getattr(p, "legend", None)
    if legend is not None:
        legend.setBrush(pg.mkBrush(qcolor(t.plot_bg, 0.75)))
        legend.setPen(pg.mkPen(t.border))
        legend.opts["labelTextColor"] = pg.mkColor(t.text)
        for _sample, label in legend.items:
            label.setText(label.text, color=t.text)
        legend.update()


def plot_title(p: pg.PlotItem, title: str):
    p._plain_title = title
    p.setTitle(title, color=theme().muted, size="9pt")

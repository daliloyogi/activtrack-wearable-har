"""
ActivTrack — Entry Point
Run: python activtrack_app.py
"""

import sys
from PyQt6.QtWidgets import QApplication

from app.ui import theme


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("ActivTrack")
    app.setOrganizationName("ActivTrack")
    theme.init(app)                 # tema tersimpan (default Gelap), sebelum window dibuat

    from app.ui.main_window import MainWindow
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()

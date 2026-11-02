"""
Activity labels. Each sample carries the activity code (index in ACTIVITIES)
that was active when it arrived; it can be switched live during REC.
The same code is written as `activityMode` in the local CSV export.
"""

ACTIVITIES = ["Sitting", "Standing", "Walking", "Running"]   # kode 0..3 (kolom "activity")
STATE_LABELS = dict(enumerate(ACTIVITIES))                   # {0: "Sitting", 1: "Standing", ...}
MOVING_ACTIVITIES = {"Walking", "Running"}                   # segmen ini wajib diisi pace
DEFAULT_ACTIVITY = "Standing"                                # aktivitas awal saat app dibuka
# warna per aktivitas ada di app/ui/theme.py (beda untuk tema gelap/terang)


def activity_code(name: str) -> int:
    return ACTIVITIES.index(name)


def activity_name(code) -> str:
    try:
        return ACTIVITIES[int(code)]
    except (ValueError, IndexError, TypeError):
        return "?"

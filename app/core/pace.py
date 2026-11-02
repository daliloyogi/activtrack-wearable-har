"""
Pace helpers. Pace is stored as seconds per km (int) and shown as "m:ss".
"""

import re
from typing import Optional

PACE_MIN_S = 2 * 60     # 2:00 /km  (~30 km/h)
PACE_MAX_S = 30 * 60    # 30:00 /km (~2 km/h, jalan pelan)

_PACE_RE = re.compile(r"^\s*(\d{1,2})\s*[:.]\s*([0-5]\d)\s*$")


def parse_pace(text: str) -> Optional[int]:
    """"5:30" / "5.30" -> 330. None if the text is not a valid pace in range."""
    m = _PACE_RE.match(text or "")
    if not m:
        return None
    s = int(m.group(1)) * 60 + int(m.group(2))
    return s if PACE_MIN_S <= s <= PACE_MAX_S else None


def format_pace(sec_per_km: Optional[int]) -> str:
    if not sec_per_km:
        return "—"
    return f"{sec_per_km // 60}:{sec_per_km % 60:02d}"


def speed_kmh(sec_per_km: Optional[int]) -> float:
    return 3600.0 / sec_per_km if sec_per_km else 0.0

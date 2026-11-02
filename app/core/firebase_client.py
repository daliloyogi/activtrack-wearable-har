"""
Firebase client — wraps firebase_admin for ActivTrack.
Handles init, push session payload, and fetch history.
"""

import json
import os
from typing import Optional

try:
    import firebase_admin
    from firebase_admin import credentials, db as rtdb
    _FIREBASE_AVAILABLE = True
except ImportError:
    _FIREBASE_AVAILABLE = False


_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOCAL_CONFIG_FILE = os.path.join(_PROJECT_ROOT, "config.local.json")  # not committed, see config.example.json


def _local_config() -> dict:
    try:
        with open(LOCAL_CONFIG_FILE, encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


# Connection settings are never hard-coded: environment variable > config.local.json > default.
_cfg = _local_config()
FIREBASE_DB_URL = os.environ.get("ACTIVTRACK_FIREBASE_URL") or _cfg.get("firebase_db_url", "")
FIREBASE_KEY    = (os.environ.get("ACTIVTRACK_FIREBASE_KEY") or _cfg.get("firebase_key")
                   or os.path.join(_PROJECT_ROOT, "serviceAccountKey.json"))


class FirebaseClient:
    def __init__(self, key_path: str = FIREBASE_KEY, db_url: str = FIREBASE_DB_URL):
        self.key_path = key_path
        self.db_url   = db_url
        self._ready   = False
        self._error   = ""

    # ── init ─────────────────────────────────────────────────────────────────

    def connect(self) -> tuple[bool, str]:
        """Try to init Firebase. Returns (success, message)."""
        if not _FIREBASE_AVAILABLE:
            self._error = "firebase-admin tidak terinstall."
            return False, self._error
        if not os.path.exists(self.key_path):
            self._error = f"Key file tidak ditemukan: {self.key_path}"
            return False, self._error
        if not self.db_url.startswith("https://"):
            self._error = "Database URL belum diatur (harus diawali https://)."
            return False, self._error
        try:
            if not firebase_admin._apps:
                cred = credentials.Certificate(self.key_path)
                firebase_admin.initialize_app(cred, {"databaseURL": self.db_url})
            self._ready = True
            return True, "Firebase terhubung."
        except Exception as e:
            self._error = str(e)
            return False, self._error

    @property
    def is_ready(self) -> bool:
        return self._ready

    @property
    def last_error(self) -> str:
        return self._error

    # ── push ─────────────────────────────────────────────────────────────────

    def push_session(self, session_name: str, payload: dict) -> tuple[bool, str]:
        """Push full session payload under recordings/{session_name}."""
        if not self._ready:
            return False, "Firebase belum terhubung."
        try:
            rtdb.reference(f"recordings/{session_name}").set(payload)
            return True, f"Session '{session_name}' berhasil disimpan."
        except Exception as e:
            return False, str(e)

    # ── fetch history ─────────────────────────────────────────────────────────

    def fetch_sessions(self) -> tuple[Optional[dict], str]:
        """Fetch all session keys from Firebase."""
        if not self._ready:
            return None, "Firebase belum terhubung."
        try:
            data = rtdb.reference("recordings").get()
            return data or {}, ""
        except Exception as e:
            return None, str(e)

    def fetch_session(self, session_name: str) -> tuple[Optional[dict], str]:
        """Fetch one full session."""
        if not self._ready:
            return None, "Firebase belum terhubung."
        try:
            data = rtdb.reference(f"recordings/{session_name}").get()
            return data or {}, ""
        except Exception as e:
            return None, str(e)

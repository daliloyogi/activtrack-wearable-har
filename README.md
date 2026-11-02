# ActivTrack – Wearable Activity Recognition (nRF24 + ESP32)

Firmware and PC tooling for a small wearable system that recognises **sitting, standing,
walking and running** from an IMU and raises a staged buzzer alarm when a position is held
too long.

```
 ┌────────────┐  nRF24L01   ┌────────────────┐  WiFi / HTTPS  ┌───────────────────┐
 │ Node 1 / 2 │ ──────────▶ │ Ground Station │ ─────────────▶ │ Firebase RTDB     │
 │ ESP32-C3   │ ◀────────── │ ESP32 + OLED   │ ◀───────────── │ (web dashboard)   │
 │ MPU6050    │  commands   │ 5 buttons      │  session rules └───────────────────┘
 └────────────┘             └───────┬────────┘
                                    │ USB serial (DEBUG mode)
                            ┌───────▼────────┐
                            │ ActivTrack app │  PyQt6: record, label, export CSV
                            └────────────────┘
```

## Repository layout

| Path | Purpose |
|------|---------|
| `src/node/` | Wearable firmware (IMU sampling, on-device feature extraction + EBM classifier, buzzer) |
| `src/ground_station/` | Base-station firmware (radio hub, OLED UI, cloud sync, alarm logic) |
| `src/diag_nrf/` | nRF24 wiring diagnostic for the ground-station board |
| `app/`, `activtrack_app.py` | PC data-collection app (PyQt6) |
| `tools/` | Model export / label utilities |
| `docs/oled_preview/` | Screenshots of the OLED screens |

## Hardware

- **Ground station:** ESP32 DevKitC v4, SSD1306 OLED, nRF24L01, 5 buttons, buzzer
- **Nodes (×2):** ESP32-C3 SuperMini, MPU6050, nRF24L01, button, buzzer, battery

## Build & flash (PlatformIO)

```bash
pio run -e ground_station -t upload
pio run -e node1 -t upload
pio run -e node2 -t upload
```

Environments: `ground_station`, `gs_diag`, `node1`, `node2` (see `platformio.ini`).

### Ground-station configuration

Credentials are **not** part of the repository.

```bash
cp src/ground_station/secrets_example.h src/ground_station/secrets.h
# then fill in WIFI_SSID, WIFI_PASS, FB_DB_HOST, CLOUD_TZ
```

`secrets.h` is git-ignored. WiFi can also be changed at runtime from the device
(*SETTING → Cloud → WiFi*); the credentials are then stored in the ESP32's flash (NVS).

## Operating modes

- **DEBUG** – nodes stream raw IMU data to the PC app over USB serial for recording and labelling.
- **PRODUCTION** – sessions are controlled from the web dashboard; the ground station uploads
  activity history and drives the buzzer alarm.

### Staged buzzer alarm

Once the dashboard rule is exceeded (e.g. "sitting 20 min") the node buzzer escalates through
three stages. Every stage uses the same 200 ms beep; only the repeat interval shortens.

| Stage | Name | When | Beep interval |
|-------|------|------|---------------|
| 1 | Notice | first 30 s over the limit | 5 s |
| 2 | Warning | 30–60 s over the limit | 3 s |
| 3 | Penalty | more than 60 s over the limit | 1 s |

Repeated violations within one session jump straight to a higher stage.
To demonstrate the sounds without a session, use **SETTING → Buzzer → Demo alarm** on the
ground station.

## PC app

```bash
python -m venv .venv && .venv\Scripts\activate     # Windows
pip install -r requirements.txt
python activtrack_app.py                           # or run_app.bat
```

Firebase upload is optional. Provide connection details through environment variables
(`ACTIVTRACK_FIREBASE_URL`, `ACTIVTRACK_FIREBASE_KEY`) or a local, git-ignored
`config.local.json` (copy `config.example.json`). The service-account key file must stay
out of version control.

## Security notes

- **Never commit** `serviceAccountKey.json`, `config.local.json` or `secrets.h`
  (all are in `.gitignore`). If a key was ever shared or committed, revoke it in the Google
  Cloud console and create a new one.
- The ground station talks to Firebase over HTTPS with a pinned root-CA bundle
  (`cloud_ca.h`); certificate validation is not disabled.
- The ground station reads and writes the Realtime Database **without a login or API key**.
  Restrict the database **security rules** so that only the paths it needs
  (`/deviceState/<id>` read, `/deviceHistory/<ssid>` write) are reachable and
  everything else is denied. Do not leave the database in open/test mode.
- Session names typed in the PC app are sanitised before they are used in file names or
  database paths.
- `tools/export_ebm.py` loads pickled models; only open model files you trust.

## License

No license has been chosen yet. Add a `LICENSE` file before publishing if you want others to
be able to use the code.

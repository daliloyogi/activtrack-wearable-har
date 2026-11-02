"""
IMU calibration status reported by the nodes (byte `cal` in the node packet,
forwarded by the ground station in `C,...` and `S,...` lines).

  bit0-3 : status (CAL_*)       bit7 : accel offsets valid (stored in node flash)
Must match the defines in src/node/main.cpp and src/ground_station/main.cpp.
"""

CAL_NONE       = 0
CAL_RUNNING    = 1
CAL_OK         = 2   # gyro + accel
CAL_GYRO_ONLY  = 3   # node tidak datar: hanya gyro
CAL_FAIL_MOVE  = 4   # bergerak: kalibrasi lama tetap dipakai
CAL_FAIL_IMU   = 5
CAL_BOOT       = 6   # bias gyro dari pengukuran saat boot
CAL_BOOT_SAVED = 7   # bergerak saat boot: bias gyro dari flash
CAL_ACC_VALID  = 0x80

_KEYS = {
    CAL_NONE: "none", CAL_RUNNING: "running", CAL_OK: "ok", CAL_GYRO_ONLY: "gyro_only",
    CAL_FAIL_MOVE: "fail_moving", CAL_FAIL_IMU: "fail_imu", CAL_BOOT: "boot",
    CAL_BOOT_SAVED: "boot_saved",
}


def status(cal: int) -> int:
    return cal & 0x0F


def accel_valid(cal: int) -> bool:
    return bool(cal & CAL_ACC_VALID)


def is_done(cal: int) -> bool:
    return status(cal) not in (CAL_NONE, CAL_RUNNING)


def is_failure(cal: int) -> bool:
    return status(cal) in (CAL_FAIL_MOVE, CAL_FAIL_IMU)


def key(cal: int) -> str:
    """Short machine-readable name, stored with each take."""
    return _KEYS.get(status(cal), "unknown")


def describe(cal: int) -> tuple:
    """(short text, tone) for the UI. tone: success | warning | danger | accent."""
    st, acc = status(cal), accel_valid(cal)
    acc_txt = "accel OK" if acc else "accel belum"
    if st == CAL_RUNNING:
        return "kalibrasi… jangan digerakkan", "accent"
    if st == CAL_OK:
        return "gyro + accel OK", "success"
    if st == CAL_GYRO_ONLY:
        return "gyro OK · accel dilewati", "warning"
    if st == CAL_FAIL_MOVE:
        return "gagal: node bergerak", "danger"
    if st == CAL_FAIL_IMU:
        return "gagal: IMU error", "danger"
    if st == CAL_BOOT:
        return f"gyro (boot) · {acc_txt}", "success" if acc else "warning"
    if st == CAL_BOOT_SAVED:
        return f"gyro dari flash · {acc_txt}", "warning"
    return "belum dikalibrasi", "warning"


def detail(cal: int) -> str:
    """Longer explanation (tooltip)."""
    st, acc = status(cal), accel_valid(cal)
    acc_txt = ("Offset accel dari kalibrasi tersimpan di flash node." if acc
               else "Accel belum pernah dikalibrasi — lakukan kalibrasi dengan node datar.")
    return {
        CAL_RUNNING: "Node sedang mengukur. Jangan disentuh sampai berbunyi 2x.",
        CAL_OK: "Bias gyro dan offset accel baru sudah disimpan di flash node.",
        CAL_GYRO_ONLY: "Node tidak datar: hanya bias gyro yang diperbarui. " + acc_txt,
        CAL_FAIL_MOVE: "Node bergerak saat kalibrasi; kalibrasi sebelumnya tetap dipakai. "
                       "Letakkan diam di meja lalu coba lagi.",
        CAL_FAIL_IMU: "Sampel IMU tidak terbaca. Periksa sambungan MPU6050.",
        CAL_BOOT: "Bias gyro diukur saat node dinyalakan. " + acc_txt,
        CAL_BOOT_SAVED: "Node bergerak saat dinyalakan, bias gyro diambil dari flash. " + acc_txt,
    }.get(st, "Node belum melaporkan kalibrasi apa pun. " + acc_txt)

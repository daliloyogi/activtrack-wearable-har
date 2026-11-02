#pragma once
// Contoh konfigurasi. Salin jadi secrets.h (tidak ikut git) lalu isi. Dipakai jika secrets.h tidak ada.

// WiFi awal (bisa diganti dari GS: SETTING > Cloud > WiFi, tersimpan di flash)
#define WIFI_SSID ""
#define WIFI_PASS ""

// Realtime Database proyek web (tanpa https:// dan tanpa '/'). Tanpa login / API key:
// GS hanya membaca /deviceState/{did} dan menulis /deviceHistory/{ssid} (ESP32_GUIDE.md §3).
#define FB_DB_HOST "<project>-default-rtdb.<region>.firebasedatabase.app"

// Uji dengan emulator di jaringan lokal (ESP32_GUIDE.md §15.2): aktifkan dua baris ini (http, bukan https)
// #define FB_EMULATOR_HOST "192.168.1.10:9000"
// #define FB_EMULATOR_NS "demo-activity-dashboard-default-rtdb"

#define CLOUD_TZ "WIB-7" // zona waktu POSIX untuk log: WIB-7, WITA-8, WIT-9

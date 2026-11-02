#pragma once
/*
 * Mode PRODUCTION: sesi dikendalikan web lewat Firebase Realtime Database (ESP32_GUIDE.md).
 *
 * Setiap node = satu wearable dengan device ID (did, mis. "W-01"), disimpan di flash GS dan bisa
 * diubah dari OLED (SETTING > Cloud). GS menjadi gateway WiFi untuk kedua node:
 *   - baca  GET  /deviceState/{did}            (tiap STATE_POLL_MS, tanpa auth)
 *              {"active", "ssid", "buzzerRule":{"rule","thresholdMinutes"}, "intervalSec", "uploadEverySec"}
 *              active=true -> node RUN & rekam ke seri ssid; active=false -> node STOP, kirim sisa (flush)
 *   - tulis PATCH /deviceHistory/{ssid}.json   {"<ts 10 digit>":{"label":"sitting","conf":91,"buzzer":false}}
 *              batch <= 60 bacaan tiap uploadEverySec, retry backoff + jitter, 401 -> baca ulang state
 *
 * Bacaan dibuat di loop() (core 1, cloudLoop) tiap intervalSec dari laporan node 5 s, jadi tidak
 * pernah tertunda oleh jaringan. Kerja jaringan (WiFi, jam NTP, HTTP) di task sendiri di core 0.
 * Buzzer: dihitung di GS, hasilnya ikut tiap bacaan dan membunyikan buzzer node. Duduk DAN berdiri
 * selalu diawasi (runSec >= ambang*60, berpindah posisi = hitung ulang, aktivitas lain = reset).
 * Ambang default keduanya BUZZ_DEFAULT_MIN (1) menit; buzzerRule dari web (satu posisi) hanya
 * menggantikan ambang posisinya, posisi lain tetap default.
 * WiFi: jaringan pilihan GS (flash "wifi") atau secrets.h; host database di secrets.h.
 */
#include <stddef.h>
#include <stdint.h>

#define CLOUD_NODES 2
#define CLOUD_ACTS 4      // kode aktivitas node: HARUS sama dengan app/core/labels.py (0 Sitting ..)
#define CLOUD_AP_MAX 12   // jaringan hasil scan yang disimpan (terkuat dulu)
#define CLOUD_DID_MAX 40  // device ID: A-Z a-z 0-9 - _, 1..40 karakter
#define CLOUD_SSID_MAX 96 // id seri dari /deviceState (opak)

// Status koneksi untuk OLED (HARUS sama urutan dengan UI_CLOUD_* di ui.h)
enum CloudState : uint8_t
{
  CLOUD_OFF,     // bukan mode PROD
  CLOUD_WIFI,    // menyambung WiFi / menunggu jawaban server pertama
  CLOUD_NO_WIFI, // WiFi belum tersambung, terus dicoba
  CLOUD_ONLINE,  // request terakhir ke database sukses
  CLOUD_ERR      // request terakhir gagal (code)
};

struct CloudAp
{
  char ssid[33];
  int8_t rssi; // dBm
  bool secure; // butuh sandi
};

// Pekerjaan WiFi dari halaman WiFi GS (HARUS sama urutan dengan UI_WJ_* di ui.h)
enum CloudWifiJob : uint8_t
{
  CWJ_IDLE,
  CWJ_SCANNING,
  CWJ_SCAN_DONE,
  CWJ_SCAN_FAIL,
  CWJ_CONNECTING,   // mencoba jaringan baru
  CWJ_CONNECT_OK,   // tersambung, disimpan
  CWJ_CONNECT_FAIL  // gagal, kembali ke jaringan lama (reason)
};

struct CloudStatus
{
  CloudState state;
  int16_t code;      // kode HTTP / error HTTPClient (<0) terakhir yang gagal
  uint32_t lastOkMs; // millis() request sukses terakhir, 0 = belum pernah
  char ssid[33];     // jaringan WiFi yang dipakai
  bool wifiUp;
  int8_t rssi;
  CloudWifiJob job;
  uint8_t reason; // alasan putus WiFi saat CWJ_CONNECT_FAIL (wifi_err_reason_t)
};

// Sesi satu node menurut /deviceState/{did} (HARUS sama urutan dengan UI_SESS_* di ui.h)
enum CloudSess : uint8_t
{
  SESS_UNKNOWN,   // state belum terbaca (offline / ID belum diisi)
  SESS_IDLE,      // active = false / tidak ada node state
  SESS_RECORDING, // active = true: node RUN, bacaan dibuat & dikirim
  SESS_FLUSHING   // baru di-stop: mengirim sisa bacaan ke seri yang sama
};

struct CloudNode
{
  char did[CLOUD_DID_MAX + 1];
  CloudSess sess;
  bool watchSitting;    // posisi yang sedang dihitung: true = sitting, false = standing
  uint8_t thresholdMin; // ambang posisi itu (menit)
  uint8_t thrSitMin;    // ambang duduk (default BUZZ_DEFAULT_MIN, atau buzzerRule web)
  uint8_t thrStandMin;  // ambang berdiri (idem)
  uint16_t intervalSec;
  bool buzzer;          // aturan terpenuhi pada bacaan terakhir -> buzzer node ON
  uint16_t runMin;      // lama posisi yang diawasi (menit, untuk tampilan "duduk 21m")
  uint32_t runSec;       // sama, dalam detik (presisi penuh; buzz_stage.h butuh ini untuk tingkat 30/60 dtk)
  uint16_t queued;      // bacaan belum terkirim
  uint32_t gen;          // naik tiap seri (ssid) baru
};

void cloudBegin();               // sekali di setup()
void cloudConfig(bool active);   // active = mode PROD (selain itu WiFi dimatikan)
// Setiap hasil baru node (bukan kiriman ulang). Dipanggil dari loop(), sama dengan cloudLoop().
void cloudReport(uint8_t idx, uint8_t act, uint8_t confPct);
void cloudLoop();                // tiap loop(): bacaan per intervalSec + aturan buzzer
CloudStatus cloudStatus();
CloudNode cloudNode(uint8_t idx);
// Ganti device ID node (disimpan di flash; sesi node itu diulang). false = format tidak valid
bool cloudSetDeviceId(uint8_t idx, const char *did);
bool cloudValidDeviceId(const char *did);

// Scan jaringan (di mode apa pun; WiFi dinyalakan sementara). Selesai: cloudStatus().job = CWJ_SCAN_DONE
void cloudScan();
uint8_t cloudScanResults(CloudAp *out, uint8_t max);
// Coba sambung ke jaringan lain (pass "" = terbuka). Berhasil -> disimpan & dipakai seterusnya;
// gagal dalam beberapa detik -> kembali ke jaringan lama. Hasil di cloudStatus().job
void cloudConnect(const char *ssid, const char *pass);

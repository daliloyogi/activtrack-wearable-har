#pragma once
/*
 * Tampilan OLED ground station (SSD1306 128x64) - "HAR  Human Activity Tracker".
 *
 * Semua gambar ada di ui.cpp dan hanya memakai API Adafruit_GFX, terpisah dari logika radio
 * di main.cpp: main.cpp mengisi UiState lalu memanggil uiDraw(). Karena itu tampilan juga
 * bisa dirender di PC (GFXcanvas1) untuk pratinjau.
 *
 * Tombol (dipakai di teks petunjuk): OK = ENTER, \x1e\x1f = UP/DOWN, \x11\x10 = LEFT/RIGHT.
 */

#include <Adafruit_GFX.h>

#define UI_W 128
#define UI_H 64
#define UI_NODES 2
#define UI_BOOT_MS 2800 // durasi animasi boot

// Bit ResultPacket.flags yang dipakai tampilan (HARUS sama dengan RES_* di main.cpp)
#define UI_RES_HAS_ACT 0x01
#define UI_RES_HAS_PACE 0x02
#define UI_RES_RATE_DIFF 0x04
#define UI_RES_TEST_FAIL 0x08
#define UI_ACT_NONE 0xFF

// Halaman SETTING: menu utama + submenu, maksimal UI_SET_ITEMS item (submenu Buzzer 4, lainnya 3)
#define UI_SET_ITEMS 4
enum UiSetMenu : uint8_t
{
  UI_MENU_TOP,
  UI_MENU_BUZZER,
  UI_MENU_CLOUD
};
enum : uint8_t
{
  UI_TOP_MODE,
  UI_TOP_BUZZER, // -> UI_MENU_BUZZER
  UI_TOP_CLOUD   // -> UI_MENU_CLOUD
};
enum : uint8_t
{
  UI_BZ_GS,    // buzzer ground station ON/OFF
  UI_BZ_DEMO,  // -> layar demo alarm (3 tingkat)
  UI_BZ_RULE1, // aturan buzzer node 1 dari web (hanya tampil)
  UI_BZ_RULE2  // aturan buzzer node 2 dari web
};
#define UI_BZ_ITEMS 4  // jumlah item submenu Buzzer
#define UI_SET_ITEMS_3 3 // jumlah item menu utama & submenu Cloud
// Layar demo alarm: baris 0..2 = tingkat 1..3, UI_DEMO_ALL = semua berurutan
#define UI_DEMO_ALL 3
enum : uint8_t
{
  UI_CL_WIFI, // -> layar WiFi
  UI_CL_ID1,  // -> keyboard device ID node 1
  UI_CL_ID2   // -> keyboard device ID node 2
};

// Layar KONEKSI saat boot
enum UiNet : uint8_t
{
  UI_NET_NONE,
  UI_NET_CHECK, // menyambung WiFi + database
  UI_NET_ONLINE,
  UI_NET_OFFLINE
};
#define UI_CLOUD_NEVER 0xFFFFFFFFUL

// Status koneksi database (HARUS sama urutan dengan CloudState di cloud.h)
enum UiCloud : uint8_t
{
  UI_CLOUD_OFF,
  UI_CLOUD_WIFI,    // menyambung WiFi / menunggu jawaban pertama
  UI_CLOUD_NO_WIFI, // WiFi belum tersambung, terus dicoba
  UI_CLOUD_ONLINE,  // request terakhir sukses
  UI_CLOUD_ERR      // request terakhir gagal
};

// Sesi node dari web (HARUS sama urutan dengan CloudSess di cloud.h)
enum UiSess : uint8_t
{
  UI_SESS_UNKNOWN,
  UI_SESS_IDLE,
  UI_SESS_RECORDING,
  UI_SESS_FLUSHING
};

// ---- WiFi: daftar hasil scan, keyboard (sandi WiFi / device ID), menyambung
#define UI_AP_MAX 12
#define UI_KB_PAGES 4  // abc, ABC, 123, simbol
#define UI_KB_ROWS 3   // baris karakter
#define UI_KB_COLS 10
#define UI_KB_KEYS 5   // baris bawah: UiKbKey
enum UiKbKey : uint8_t
{
  UI_KEY_PAGE,   // ganti halaman huruf
  UI_KEY_SPACE,
  UI_KEY_DEL,
  UI_KEY_CANCEL, // batal
  UI_KEY_OK      // selesai (sambung / simpan)
};
// Karakter keyboard [halaman][baris] (dipakai ui.cpp untuk menggambar, main.cpp untuk mengetik)
extern const char UI_KB[UI_KB_PAGES][UI_KB_ROWS][UI_KB_COLS + 1];

enum UiWifiScreen : uint8_t
{
  UI_WIFI_OFF, // tidak di layar WiFi / keyboard
  UI_WIFI_LIST,
  UI_WIFI_PASS, // keyboard sandi WiFi
  UI_WIFI_CONNECT,
  UI_WIFI_ID // keyboard device ID node
};

// Pekerjaan WiFi (HARUS sama urutan dengan CloudWifiJob di cloud.h)
enum UiWifiJob : uint8_t
{
  UI_WJ_IDLE,
  UI_WJ_SCANNING,
  UI_WJ_SCAN_DONE,
  UI_WJ_SCAN_FAIL,
  UI_WJ_CONNECTING,
  UI_WJ_CONNECT_OK,
  UI_WJ_CONNECT_FAIL
};

struct UiAp
{
  char ssid[33];
  int8_t rssi;
  bool secure;
};

enum UiNodeState : uint8_t
{
  UI_NODE_NONE, // belum pernah terlihat
  UI_NODE_OFF,  // hilang
  UI_NODE_STOP,
  UI_NODE_RUN,
  UI_NODE_CAL // sedang kalibrasi
};

struct UiNode
{
  UiNodeState state;
  bool pending;    // perintah START/STOP belum diterapkan node
  uint16_t pktHz;  // paket per detik yang diterima
  float lossPct;
  uint16_t battMv;
  bool imuOk;
  uint8_t cal;     // bit0-3 status kalibrasi, bit7 accel valid
  bool calPending; // kalibrasi diminta, node belum mulai
  float acc[3];    // g (mode DEBUG, saat RUN)
  float gyr[3];    // deg/s
  // hasil model terbaru (mode PROD)
  bool hasResult;
  uint8_t act;     // kode aktivitas 0..3, UI_ACT_NONE = tidak ada
  uint8_t prob[4]; // probabilitas / porsi suara x255
  uint16_t paceS;  // s/km
  uint8_t flags;   // UI_RES_*
  // sesi web (mode PROD)
  char did[41];     // device ID
  uint8_t sess;     // UiSess
  uint8_t buzzStage; // 0 = normal, 1 pemberitahuan, 2 peringatan, 3 hukuman (buzz_stage.h BUZZ_STAGES)
  bool watchSit;    // posisi yang sedang dihitung: true = duduk, false = berdiri
  uint8_t thrSit;   // ambang duduk (menit)
  uint8_t thrStand; // ambang berdiri (menit)
  uint16_t runMin;  // lama posisi yang diawasi (menit)
  uint16_t queued;  // bacaan belum terkirim
};

struct UiState
{
  uint32_t nowMs;
  uint8_t page;     // 0 = utama, 1..UI_NODES = node, UI_NODES + 1 = setting
  uint8_t numPages;
  bool prod;        // mode PRODUCTION
  bool pcOnline;
  bool anyRun;      // ada node yang diminta RUN
  uint8_t rateHz;
  float gsBattV;
  int gsBattPct;
  uint32_t badPkt;
  bool buzzerOn;
  uint8_t setMenu;    // UiSetMenu yang terbuka
  uint8_t setItem;    // item terpilih di menu itu
  uint8_t netCheck;   // UiNet; != UI_NET_NONE -> layar KONEKSI menggantikan halaman
  uint8_t netSecLeft; // sisa detik menunggu (UI_NET_CHECK)
  // demo alarm (submenu Buzzer > Demo alarm)
  bool demo;          // layar demo menggantikan halaman
  uint8_t demoSel;    // baris terpilih (0..UI_DEMO_ALL)
  uint8_t demoStage;  // tingkat yang sedang dibunyikan, 0 = diam
  uint8_t demoLeftS;  // sisa detik tingkat itu
  // database (mode PROD)
  uint8_t cloud;      // UI_CLOUD_*
  int16_t cloudCode;  // kode HTTP / error terakhir
  uint32_t cloudAgoS; // detik sejak request sukses terakhir, UI_CLOUD_NEVER = belum pernah
  // WiFi (item SETTING + layar WiFi)
  const char *wifiSsid; // jaringan yang dipakai
  bool wifiUp;
  int8_t wifiRssi;
  uint8_t wifiScreen; // UiWifiScreen; != UI_WIFI_OFF -> layar WiFi / keyboard menggantikan halaman
  uint8_t wifiJob;    // UiWifiJob
  uint8_t wifiReason; // alasan gagal sambung (wifi_err_reason_t)
  const UiAp *aps;
  uint8_t apCount;
  uint8_t apSel;       // 0 = "Scan ulang", 1.. = aps[apSel - 1]
  const char *wTarget; // jaringan yang dipilih (layar menyambung)
  const char *wPass;   // teks yang sedang diketik (sandi / device ID)
  const char *kbTitle; // judul keyboard ("SANDI", "ID NODE 1")
  const char *kbHint;  // teks kanan header keyboard (nama jaringan / karakter yang boleh)
  uint8_t kbPage, kbRow, kbCol; // kbRow == UI_KB_ROWS -> baris tombol bawah (kbCol = UiKbKey)
  const char *toast; // pesan singkat di tengah layar, nullptr = tidak ada
  UiNode node[UI_NODES];
};

// Satu frame halaman sesuai s.page (tidak memanggil display())
void uiDraw(Adafruit_GFX &d, const UiState &s);

// Animasi boot, t = ms sejak mulai. Return false setelah UI_BOOT_MS (frame terakhir tetap digambar).
bool uiBoot(Adafruit_GFX &d, uint32_t t, const char *version);

// Layar tunggu singkat (mis. "Menyiapkan radio...")
void uiSplash(Adafruit_GFX &d, const char *line1, const char *line2, uint32_t nowMs);

// nRF24 tidak terdeteksi, GS mencoba lagi
void uiNrfError(Adafruit_GFX &d, uint16_t attempt, uint32_t nowMs);

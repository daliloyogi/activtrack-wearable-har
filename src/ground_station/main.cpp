#include <Arduino.h>

/*
 * HAR - Human Activity Tracker
 * Ground Station v1.6 - nRF24L01 + OLED SSD1306 + kontrol START/STOP node + sesi web (Firebase)
 * Tampilan OLED (animasi boot, halaman, toast): ui.cpp
 * Sesi web lewat Firebase Realtime Database (mode PROD, ESP32_GUIDE.md): cloud.cpp, WiFi/host di secrets.h
 * Board : ESP32 DevKitC v4
 *
 * v1.5: dua mode
 *   DEBUG      -> semua payload dikirim ke laptop lewat Serial (dibaca gs_debug.py),
 *                 laptop juga bisa kirim perintah (START/STOP/RATE/RESET)
 *   PRODUCTION -> tanpa laptop: node menjalankan model EBM dan hanya mengirim status +
 *                 hasil tebakan (aktivitas, keyakinan, pace); OLED menampilkan hasil per node.
 *                 Serial hanya log "#CLOUD ..." (status WiFi / upload).
 *   Boot selalu langsung PRODUCTION; DEBUG dipilih di SETTING (tidak disimpan, reboot = PROD lagi).
 * v1.6: mode PROD dikendalikan web (ESP32_GUIDE.md): tiap node = wearable dengan device ID
 *     (SETTING > Cloud). GS membaca /deviceState/{did}: active=true -> node RUN, bacaan per intervalSec
 *     (label, conf, buzzer) dikirim ke /deviceHistory/{ssid}; active=false -> node STOP + kirim sisa.
 *     Aturan buzzer (duduk/berdiri, ambang menit) dari web, dihitung GS, buzzer node ikut.
 *     WiFi selalu menyala di mode PROD.
 *
 * Perintah ke node dikirim lewat ACK PAYLOAD nRF24 (berbasis STATE).
 * Tombol node hanya MEMINTA START/STOP, ground station yang memutuskan.
 *
 * Page  : 1 = Ground Station, 2 = Node 1, 3 = Node 2, 4 = SETTING
 * Tombol:
 *   LEFT / RIGHT        : pindah page
 *   ENTER               : START/STOP (page 1 = semua node, page node = node itu saja)
 *                         hanya mode DEBUG (juga tombol node & app PC); mode PROD = lewat web
 *   ENTER tahan 1 detik : page 1 = reset statistik, page node = KALIBRASI IMU node itu
 *   UP / DOWN           : ubah sample rate (hanya bisa saat semua node STOP)
 *   Page SETTING        : UP/DOWN pilih item, ENTER ubah nilai / buka submenu (tersimpan di flash),
 *                         LEFT di submenu = kembali:
 *                           Mode    DEBUG / PRODUCTION (tanpa reboot, node ikut pindah);
 *                                   hanya bisa saat semua node terputus (mati / belum terlihat)
 *                           Buzzer > Buzzer GS ON/OFF, Demo alarm (bunyikan tingkat 1..3 di GS & node),
 *                                    Aturan N1/N2 (buzzer node, dari web: tampil saja)
 *                           Cloud  > WiFi (scan jaringan & ganti koneksi, sandi lewat keyboard OLED),
 *                                    ID node 1 / ID node 2 (device ID wearable, keyboard OLED)
 * Boot (mode PROD): layar KONEKSI (WiFi + baca database) sampai online / maks 20 s;
 *   selama offline, baris bawah layar PROD menampilkan "OFFLINE: ..." (terbalik).
 *
 * ===================== PROTOKOL SERIAL (mode DEBUG, 921600) =====================
 * GS -> PC (satu baris teks per pesan, dipisah koma):
 *   #GS,<versi>,<mode>                                   info saat boot
 *   D,gsMs,node,seq,nodeMs,ax,ay,az,gx,gy,gz,battMv,flags data IMU (node RUN)
 *   H,gsMs,node,seq,battMv,flags                          heartbeat (node STOP)
 *   E,gsMs,event,target,rateHz,src                        START/STOP/RATE/RESET/LOST/BACK
 *   F,gsMs,node,win,tStartMs,tEndMs,n,rateHz,ver,f0..f29  fitur jendela dihitung di node
 *   P,gsMs,node,win,tEndMs,act,p0,p1,p2,p3,paceS,flags   hasil model EBM di node
 *   S,gsMs,node,state,rateHz,rx,lost,cal,featOk,featDrop  statistik node, tiap 1 detik
 *   C,gsMs,node,cal,calCount                              status kalibrasi node berubah
 *   G,gsMs,battV,battPct,badPkt                           status ground station, tiap 1 detik
 *   OK,<perintah>  /  ERR,<perintah>,<alasan>             balasan perintah PC
 *   Satuan: ax..az = mg, gx..gz = 0.1 dps, battMv = mV, gsMs/nodeMs = millis()
 *   flags: bit0 = IMU OK, bit1 = RUN, bit2 = sedang kalibrasi
 *   cal  : bit0-3 status (0 belum, 1 berjalan, 2 OK gyro+accel, 3 gyro saja/tidak datar,
 *          4 gagal bergerak, 5 gagal IMU, 6 gyro dari boot, 7 gyro dari flash), bit7 = accel valid
 *   state: RUN / STP / CAL / OFF / ---
 *   F    : 30 fitur (urutan & rumus: src/node/features.h), float32 ditulis %.9g (presisi penuh);
 *          tStartMs/tEndMs = millis() node sampel pertama/terakhir jendela, n = jumlah sampel,
 *          win = nomor jendela (16 bit). Satu jendela = FEAT_PARTS paket radio FeatPacket;
 *          jendela yang tidak lengkap dibuang (featDrop).
 *   P    : act = kode aktivitas (255 = tanpa model), p0..p3 = probabilitas per kode x255,
 *          paceS = pace s/km (0 = tidak ada), flags: bit0 model aktivitas, bit1 pace,
 *          bit2 rate beda dari data training, bit3 self-test model gagal,
 *          bit4 gabungan 5 s (mode PROD: p0..p3 = porsi suara jendela), bit5 aturan sumbu paha
 *          mengubah tebakan Sitting/Standing model
 *
 * PC -> GS (diakhiri newline, huruf besar/kecil bebas):
 *   PING | START | START 1 | STOP | STOP 2 | RATE 20 | RESET | CAL | CAL 1
 * ================================================================================
 *
 * Library: RF24 (TMRh20), Adafruit SSD1306, Adafruit GFX
 */

#define RF24_SPI_SPEED 1000000
#define FW_VERSION "V1.6"

#include <SPI.h>
#include <RF24.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <stdarg.h>
#include "cloud.h"
#include "buzz_stage.h"
#include "ui.h"

// ================================ PIN ======================================
#define PIN_SCK 18
#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_CSN 5
#define PIN_CE 4
#define PIN_SDA 21
#define PIN_SCL 22
#define PIN_BTN_UP 25
#define PIN_BTN_DOWN 35 // input-only, pull-up 10k eksternal
#define PIN_BTN_LEFT 34 // input-only, pull-up 10k eksternal
#define PIN_BTN_RIGHT 33
#define PIN_BTN_ENT 32
#define PIN_BUZZER 2
#define PIN_LED_G 27
#define PIN_LED_R 15
#define PIN_GS_BATT 39

// ============================== KONFIGURASI ================================
#define SERIAL_BAUD 921600
#define OLED_ADDR 0x3C
#define RF_CHANNEL 76
#define RF_INFO "250kbps"
#define RF_PA_LEVEL RF24_PA_MAX     // jangkauan maks; WAJIB kapasitor 10-100 uF di VCC nRF (bisa macet tanpa itu)
#define RF_DATA_RATE RF24_250KBPS   // kepekaan +9 dB vs 1 Mbps (hanya nRF24L01+ / Si24R1)
#define NUM_NODES 2
#define NUM_PAGES (NUM_NODES + 2)
#define PAGE_SETTING (NUM_NODES + 1) // halaman terakhir: setting
#define NODE_TIMEOUT_MS 1000
#define PROD_NODE_TIMEOUT_MS 12000 // PROD: node RUN hanya memancar tiap 5 s (HARUS > 2x interval node)
#define OLED_PERIOD_MS 200         // DEBUG (radio sibuk: gambar ulang OLED memakan ~25 ms)
#define OLED_PERIOD_PROD_MS 100    // PROD: figur aktivitas beranimasi
#define TOAST_MS 1500              // lama pesan singkat di OLED
#define LONG_PRESS_MS 1000
#define PC_TIMEOUT_MS 3000 // PC dianggap terhubung jika ada perintah/PING dalam waktu ini
#define NRF_RETRY_MS 1000  // nRF tidak terdeteksi saat boot -> coba lagi setiap ...

#define BUZZER_ACTIVE 1  // 1 = buzzer aktif (HIGH/LOW), 0 = buzzer pasif (pakai tone)
#define BUZZER_FREQ 2700 // hanya untuk buzzer pasif

const uint8_t RATE_OPTIONS[] = {10, 20, 40, 50}; // Hz (40 = rate data training model)
#define NUM_RATES (sizeof(RATE_OPTIONS) / sizeof(RATE_OPTIONS[0]))

// ============================ BATERAI BASE STATION =========================
const float R1 = 27000.0;
const float R2 = 100000.0;
const float DIVIDER_RATIO = (R1 + R2) / R2;
const float BATT_FULL = 4.00;
const float BATT_EMPTY = 3.30;
const uint8_t BATT_SAMPLES = 16;
const uint8_t BATT_LOW_PCT = 10; // alarm buzzer di bawah persen ini

// ============================== PROTOKOL ===================================
#define PKT_MAGIC 0xA5
#define CMD_MAGIC 0x5A
#define PKT_FEAT_MAGIC 0xA6
#define PKT_RES_MAGIC 0xA7
#define RES_HAS_ACT 0x01
#define RES_HAS_PACE 0x02
#define RES_RATE_DIFF 0x04
#define RES_TEST_FAIL 0x08
#define RES_AGGREGATED 0x10
#define RES_POSTURE 0x20
#define ACT_NONE 0xFF
#define ACT_COUNT 4        // kode aktivitas: HARUS sama dengan app/core/labels.py
#define RES_STALE_MS 12000 // hasil model lebih tua dari ini dianggap tidak ada (PROD lapor tiap 5 s)
#define FEAT_COUNT 30      // HARUS sama dengan src/node/features.h
#define FEAT_CHUNK 28      // byte FeatWin per paket
#define FEAT_PARTS 5       // paket per jendela
#define FEAT_ALL_PARTS ((1 << FEAT_PARTS) - 1)
#define FLAG_IMU_OK 0x01
#define FLAG_STREAMING 0x02
#define FLAG_CALIBRATING 0x04

// Status kalibrasi (Packet.cal bit0-3), HARUS sama dengan node & app
#define CAL_NONE 0
#define CAL_RUNNING 1
#define CAL_OK 2
#define CAL_GYRO_ONLY 3
#define CAL_FAIL_MOVE 4
#define CAL_FAIL_IMU 5
#define CAL_BOOT 6
#define CAL_BOOT_SAVED 7
#define CAL_ACC_VALID 0x80
#define REQ_RUN 0x80
#define REQ_ALL 0x40
#define REQ_CNT_MASK 0x3F

// Node -> Ground Station (HARUS identik dengan node)
struct __attribute__((packed)) Packet
{
  uint8_t magic;
  uint8_t nodeId;
  uint16_t seq;
  uint8_t flags; // bit0 = IMU OK, bit1 = sedang kirim data (RUN)
  uint32_t tMs;
  int16_t ax, ay, az; // mg
  int16_t gx, gy, gz; // 0.1 dps
  uint16_t battMv;
  uint8_t req;      // permintaan dari tombol node: bit7 = RUN, bit6 = SEMUA node, bit0-5 = counter
  uint8_t cal;      // bit0-3 status kalibrasi (CAL_*), bit7 = offset accel valid
  uint8_t calCount; // bertambah setiap kalibrasi selesai
};

// Ground Station -> Node, lewat ACK payload (HARUS identik dengan node)
struct __attribute__((packed)) Cmd
{
  uint8_t magic;
  uint8_t run; // 1 = kirim data IMU, 0 = heartbeat saja
  uint8_t rateHz;
  uint8_t cal;         // 1 = mulai kalibrasi; ditahan sampai node terlihat mulai / selesai
  uint8_t mode;        // 0 = DEBUG, 1 = PRODUCTION
  uint8_t buzz;        // tingkat buzzer node (mode PROD): 0 mati, 1..BUZZ_STAGES = makin sering/keras
                       // (dari overtime, lihat buzz_stage.h & webSync())
};

// Fitur satu jendela dari node, terpecah dalam FEAT_PARTS FeatPacket (HARUS identik dengan node)
struct __attribute__((packed)) FeatWin
{
  uint32_t tStartMs; // millis() node, sampel pertama jendela
  uint32_t tEndMs;   // sampel terakhir
  uint16_t win;      // nomor jendela
  uint8_t n;         // jumlah sampel
  uint8_t rateHz;
  uint8_t version;
  uint8_t reserved[3];
  float f[FEAT_COUNT];
};

struct __attribute__((packed)) FeatPacket
{
  uint8_t magic; // PKT_FEAT_MAGIC
  uint8_t nodeId;
  uint8_t win;  // 8 bit bawah FeatWin.win
  uint8_t part; // 0 .. FEAT_PARTS-1
  uint8_t data[FEAT_CHUNK];
};

static_assert(sizeof(Packet) <= 32, "Packet melebihi 32 byte");
static_assert(sizeof(FeatPacket) == 32, "FeatPacket harus 32 byte");
static_assert(sizeof(FeatWin) <= FEAT_PARTS * FEAT_CHUNK, "FeatWin tidak muat di FEAT_PARTS paket");

// Hasil model EBM satu jendela dari node (HARUS identik dengan node)
struct __attribute__((packed)) ResultPacket
{
  uint8_t magic; // PKT_RES_MAGIC
  uint8_t nodeId;
  uint16_t win;
  uint32_t tEndMs;
  uint8_t act;             // kode aktivitas, ACT_NONE = tanpa model
  uint8_t prob[ACT_COUNT]; // probabilitas per kode x 255
  uint16_t paceS;          // s/km, 0 = tidak ada
  uint8_t flags;           // RES_*
};
static_assert(sizeof(ResultPacket) == 16, "ResultPacket harus 16 byte");

static_assert(UI_RES_HAS_ACT == RES_HAS_ACT && UI_RES_HAS_PACE == RES_HAS_PACE &&
                  UI_RES_RATE_DIFF == RES_RATE_DIFF && UI_RES_TEST_FAIL == RES_TEST_FAIL &&
                  UI_ACT_NONE == ACT_NONE,
              "UI_RES_* di ui.h harus sama dengan RES_*");
static_assert(UI_NODES == NUM_NODES, "UI_NODES (ui.h) harus sama dengan NUM_NODES");
static_assert(CLOUD_NODES == NUM_NODES && CLOUD_ACTS == ACT_COUNT, "cloud.h tidak cocok dengan protokol");
#define SAME_ENUM(a, b) ((int)(a) == (int)(b))
static_assert(SAME_ENUM(UI_CLOUD_OFF, CLOUD_OFF) && SAME_ENUM(UI_CLOUD_WIFI, CLOUD_WIFI) &&
                  SAME_ENUM(UI_CLOUD_NO_WIFI, CLOUD_NO_WIFI) && SAME_ENUM(UI_CLOUD_ONLINE, CLOUD_ONLINE) &&
                  SAME_ENUM(UI_CLOUD_ERR, CLOUD_ERR),
              "UI_CLOUD_* (ui.h) harus sama urutan dengan CloudState (cloud.h)");
static_assert(SAME_ENUM(UI_WJ_IDLE, CWJ_IDLE) && SAME_ENUM(UI_WJ_SCANNING, CWJ_SCANNING) &&
                  SAME_ENUM(UI_WJ_SCAN_DONE, CWJ_SCAN_DONE) && SAME_ENUM(UI_WJ_SCAN_FAIL, CWJ_SCAN_FAIL) &&
                  SAME_ENUM(UI_WJ_CONNECTING, CWJ_CONNECTING) && SAME_ENUM(UI_WJ_CONNECT_OK, CWJ_CONNECT_OK) &&
                  SAME_ENUM(UI_WJ_CONNECT_FAIL, CWJ_CONNECT_FAIL),
              "UI_WJ_* (ui.h) harus sama urutan dengan CloudWifiJob (cloud.h)");
static_assert(SAME_ENUM(UI_SESS_UNKNOWN, SESS_UNKNOWN) && SAME_ENUM(UI_SESS_IDLE, SESS_IDLE) &&
                  SAME_ENUM(UI_SESS_RECORDING, SESS_RECORDING) && SAME_ENUM(UI_SESS_FLUSHING, SESS_FLUSHING),
              "UI_SESS_* (ui.h) harus sama urutan dengan CloudSess (cloud.h)");
static_assert(UI_AP_MAX == CLOUD_AP_MAX && sizeof(UiAp::ssid) == sizeof(CloudAp::ssid), "UiAp != CloudAp");

// ============================ TIPE (sebelum fungsi) ========================
// Arduino IDE membuat prototipe fungsi otomatis di atas fungsi pertama,
// jadi semua tipe yang dipakai di parameter/return harus sudah didefinisikan di sini.
enum RunMode : uint8_t
{
  MODE_DEBUG = 0,
  MODE_PROD = 1
};
enum BtnEvent : uint8_t
{
  BTN_EV_NONE,
  BTN_EV_PRESS,
  BTN_EV_SHORT,
  BTN_EV_LONG
};

struct NodeStat
{
  bool seen;
  bool online;
  bool lostLogged; // event LOST sudah dikirim, tunggu BACK
  Packet last;
  uint32_t lastRxMs;
  uint32_t rx;
  uint32_t lost;
  uint16_t rateCnt;
  uint16_t rateHz;
  uint8_t lastReq;   // permintaan tombol terakhir yang sudah diproses
  uint32_t featOk;   // jendela fitur lengkap diteruskan ke PC
  uint32_t featDrop; // jendela fitur tidak lengkap (ada bagian hilang)
  bool resSeen;
  ResultPacket res; // hasil model terakhir
  uint32_t resRxMs;
};

// Penyusunan ulang jendela fitur per node
struct FeatRx
{
  bool active; // sedang mengumpulkan bagian jendela 'win'
  uint8_t win;
  uint8_t mask; // bit per bagian yang sudah diterima
  bool doneValid;
  uint8_t doneWin; // jendela terakhir yang sudah lengkap (bagian kiriman ulang diabaikan)
  uint8_t buf[FEAT_PARTS * FEAT_CHUNK];
};

struct Button
{
  uint8_t pin;
  bool stable;
  bool lastRead;
  uint32_t tChange;
  uint32_t tPress;
  bool longFired;
};

// ================================ STATE ====================================
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
RF24 radio(PIN_CE, PIN_CSN);
const byte addrNode[][6] = {"1Node", "2Node"}; // pipe 1 = node 1, pipe 2 = node 2

RunMode mode = MODE_DEBUG;

// Setting (halaman SETTING, disimpan di Preferences "gs")
// Menu SETTING (UI_MENU_*) berisi UI_SET_ITEMS item (UI_TOP_*, UI_BZ_*, UI_CL_* di ui.h)
uint8_t setItemCount(uint8_t menu) { return menu == UI_MENU_BUZZER ? UI_BZ_ITEMS : UI_SET_ITEMS_3; }
bool buzzerOn = true;
// Demo alarm (SETTING > Buzzer > Demo alarm): membunyikan pola tingkat 1..3 di GS dan kedua node
#define DEMO_STAGE_MS 12000 // lama tiap tingkat (tingkat 1: 3 bunyi, 2: 4 bunyi, 3: 12 bunyi)
bool demoOpen = false;      // layar demo terbuka
uint8_t demoSel = 0;        // baris terpilih (0..2 = tingkat 1..3, UI_DEMO_ALL = semua)
uint8_t demoStage = 0;      // tingkat yang sedang dibunyikan, 0 = diam
uint32_t demoT0 = 0;        // mulai tingkat ini (satu tingkat) / mulai urutan (semua)
uint32_t demoLastBeep = 0;
// Buzzer node (mode PROD): tingkat 0..BUZZ_STAGES dari overtime (buzz_stage.h)
uint8_t nodeBuzzStage[NUM_NODES] = {0, 0}; // ikut Cmd
uint8_t setMenu = UI_MENU_TOP; // menu SETTING yang terbuka
// Layar KONEKSI saat boot (lihat netCheckStart)
#define NET_CHECK_MS 20000       // tunggu online maksimal
#define NET_ONLINE_SHOW_MS 1500  // "ONLINE" tampil sebelum lanjut
#define NET_OFFLINE_SHOW_MS 10000
uint8_t netCheck = UI_NET_NONE;
uint32_t netCheckMs = 0; // mulai fase sekarang
uint8_t setItem = 0;       // item yang dipilih di halaman SETTING
NodeStat nodes[NUM_NODES];
FeatRx featRx[NUM_NODES];
bool wantRun[NUM_NODES] = {false, false};    // keputusan ground station
bool calPending[NUM_NODES] = {false, false}; // perintah kalibrasi belum diterima node
uint8_t rateIdx = 0;
uint32_t badPkt = 0;
uint32_t ledGOffAt = 0;
float gsBattV = 0;
int gsBattPct = 0;
uint8_t page = 0;
bool redrawNow = true;
uint32_t pcLastMs = 0;

Button btnUp = {PIN_BTN_UP, HIGH, HIGH, 0, 0, false};
Button btnDown = {PIN_BTN_DOWN, HIGH, HIGH, 0, 0, false};
Button btnLeft = {PIN_BTN_LEFT, HIGH, HIGH, 0, 0, false};
Button btnRight = {PIN_BTN_RIGHT, HIGH, HIGH, 0, 0, false};
Button btnEnter = {PIN_BTN_ENT, HIGH, HIGH, 0, 0, false};

// ================================ BUZZER ===================================
struct
{
  uint8_t remaining;
  uint16_t onMs, offMs;
  bool on;
  uint32_t tNext;
} bz = {0, 0, 0, false, 0};

void buzzerOut(bool on)
{
#if BUZZER_ACTIVE
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
#else
  if (on)
    tone(PIN_BUZZER, BUZZER_FREQ);
  else
    noTone(PIN_BUZZER);
#endif
}

// Non-blocking: bunyi 'count' kali
void beep(uint8_t count, uint16_t onMs = 60, uint16_t offMs = 80)
{
  if (!buzzerOn)
    return; // buzzer dimatikan di SETTING
  bz.remaining = count;
  bz.onMs = onMs;
  bz.offMs = offMs;
  bz.on = true;
  bz.tNext = millis() + onMs;
  buzzerOut(true);
}

void buzzerUpdate()
{
  if (bz.remaining == 0)
    return;
  uint32_t now = millis();
  if ((int32_t)(now - bz.tNext) < 0)
    return;
  if (bz.on)
  {
    buzzerOut(false);
    bz.on = false;
    bz.remaining--;
    bz.tNext = now + bz.offMs;
  }
  else
  {
    buzzerOut(true);
    bz.on = true;
    bz.tNext = now + bz.onMs;
  }
}

// ================================ TOMBOL ===================================
// BTN_EV_PRESS saat ditekan, BTN_EV_SHORT saat dilepas (sebelum long), BTN_EV_LONG saat ditahan
BtnEvent updateButton(Button &b)
{
  uint32_t now = millis();
  bool r = digitalRead(b.pin);
  if (r != b.lastRead)
  {
    b.lastRead = r;
    b.tChange = now;
  }
  if (now - b.tChange > 30 && r != b.stable)
  { // debounce 30 ms
    b.stable = r;
    if (r == LOW)
    {
      b.tPress = now;
      b.longFired = false;
      return BTN_EV_PRESS;
    }
    if (!b.longFired)
      return BTN_EV_SHORT;
  }
  if (b.stable == LOW && !b.longFired && now - b.tPress >= LONG_PRESS_MS)
  {
    b.longFired = true;
    return BTN_EV_LONG;
  }
  return BTN_EV_NONE;
}

// ============================ OUTPUT SERIAL DEBUG ==========================
// Semua output ke PC lewat fungsi ini, supaya mode PRODUCTION otomatis diam
void dbg(const char *fmt, ...)
{
  if (mode != MODE_DEBUG)
    return;
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  Serial.print(line);
}

void logEvent(const char *ev, const char *target, const char *src)
{
  dbg("E,%lu,%s,%s,%u,%s\n", (unsigned long)millis(), ev, target, RATE_OPTIONS[rateIdx], src);
}

bool pcOnline()
{
  return mode == MODE_DEBUG && pcLastMs != 0 && (millis() - pcLastMs) < PC_TIMEOUT_MS;
}

// ================================ TOAST ====================================
char toastBuf[24];
uint32_t toastUntil = 0;

// Pesan singkat di tengah OLED selama TOAST_MS (umpan balik setiap aksi)
void showToast(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(toastBuf, sizeof(toastBuf), fmt, ap);
  va_end(ap);
  toastUntil = millis() + TOAST_MS;
  if (!toastUntil)
    toastUntil = 1;
  redrawNow = true;
}

// ================================ UTIL =====================================
bool anyWantRun()
{
  for (uint8_t i = 0; i < NUM_NODES; i++)
    if (wantRun[i])
      return true;
  return false;
}

void resetStats()
{
  memset(nodes, 0, sizeof(nodes));
  memset(featRx, 0, sizeof(featRx));
  badPkt = 0;
}

void readGsBattery()
{
  uint32_t acc = 0;
  for (uint8_t i = 0; i < BATT_SAMPLES; i++)
    acc += analogReadMilliVolts(PIN_GS_BATT);
  float v = (acc / (float)BATT_SAMPLES) / 1000.0f * DIVIDER_RATIO;
  gsBattV = (gsBattV == 0) ? v : (gsBattV * 0.8f + v * 0.2f);
  float pct = (gsBattV - BATT_EMPTY) / (BATT_FULL - BATT_EMPTY) * 100.0f;
  gsBattPct = (int)constrain(pct, 0.0f, 100.0f);
}

// Tanpa paket selama ini = node OFF. PROD: node yang RUN hanya lapor tiap 5 s.
uint32_t nodeTimeoutMs()
{
  return mode == MODE_PROD ? PROD_NODE_TIMEOUT_MS : NODE_TIMEOUT_MS;
}

// Ada node yang masih terhubung (paket diterima dalam batas timeout)
bool anyNodeOnline()
{
  uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_NODES; i++)
    if (nodes[i].seen && now - nodes[i].lastRxMs < nodeTimeoutMs())
      return true;
  return false;
}

const char *nodeState(NodeStat &n, uint32_t now)
{
  if (!n.seen)
    return "---";
  if (now - n.lastRxMs >= nodeTimeoutMs())
    return "OFF";
  if (n.last.flags & FLAG_CALIBRATING)
    return "CAL";
  return (n.last.flags & FLAG_STREAMING) ? "RUN" : "STP";
}

// '*' = perintah belum diterapkan node (masih menunggu ACK berikutnya)
char pendingMark(uint8_t idx, uint32_t now)
{
  NodeStat &n = nodes[idx];
  if (!n.seen || now - n.lastRxMs >= nodeTimeoutMs())
    return ' ';
  bool running = n.last.flags & FLAG_STREAMING;
  return (running != wantRun[idx]) ? '*' : ' ';
}

float lossPct(NodeStat &n)
{
  return (n.rx + n.lost) ? 100.0f * n.lost / (float)(n.rx + n.lost) : 0.0f;
}

// ============================ PERINTAH KE NODE =============================
Cmd makeCmd(uint8_t idx)
{
  Cmd c = {CMD_MAGIC, (uint8_t)(wantRun[idx] ? 1 : 0), RATE_OPTIONS[rateIdx],
           (uint8_t)(calPending[idx] ? 1 : 0), (uint8_t)(mode == MODE_PROD ? 1 : 0),
           (uint8_t)(mode == MODE_PROD || demoStage ? nodeBuzzStage[idx] : 0)};
  return c;
}

// Isi ulang ACK payload semua pipe (dipanggil saat perintah berubah)
void loadAckPayloads()
{
  radio.flush_tx();
  for (uint8_t i = 0; i < NUM_NODES; i++)
  {
    Cmd c = makeCmd(i);
    radio.writeAckPayload(i + 1, &c, sizeof(c));
  }
}

// ACK payload satu pipe terpakai setiap ada paket masuk -> isi lagi
void reloadAck(uint8_t pipe)
{
  Cmd c = makeCmd(pipe - 1);
  if (!radio.writeAckPayload(pipe, &c, sizeof(c)))
    loadAckPayloads(); // FIFO penuh -> reset
}

// Satu-satunya tempat status RUN/STOP diubah. idx < 0 = semua node.
// src: "gs" (tombol ground station), "node1"/"node2" (tombol node), "pc" (laptop)
void setRun(int idx, bool run, const char *src)
{
  char target[12];
  if (idx < 0)
  {
    for (uint8_t i = 0; i < NUM_NODES; i++)
      wantRun[i] = run;
    strlcpy(target, "ALL", sizeof(target));
  }
  else
  {
    wantRun[idx] = run;
    snprintf(target, sizeof(target), "N%d", idx + 1);
  }
  logEvent(run ? "START" : "STOP", target, src);
  if (idx < 0)
    showToast(run ? "START semua node" : "STOP semua node");
  else
    showToast(run ? "START node %d" : "STOP node %d", idx + 1);
  if (run)
    beep(1, 150);
  else
    beep(2, 60, 60);
  loadAckPayloads();
  redrawNow = true;
}

// Minta node kalibrasi IMU (node harus diam & datar). idx < 0 = semua node.
void requestCal(int idx, const char *src)
{
  char target[12];
  if (idx < 0)
  {
    for (uint8_t i = 0; i < NUM_NODES; i++)
      calPending[i] = true;
    strlcpy(target, "ALL", sizeof(target));
  }
  else
  {
    calPending[idx] = true;
    snprintf(target, sizeof(target), "N%d", idx + 1);
  }
  logEvent("CAL", target, src);
  if (idx < 0)
    showToast("Kalibrasi semua...");
  else
    showToast("Kalibrasi N%d...", idx + 1);
  loadAckPayloads();
  beep(1, 40);
  redrawNow = true;
}

// Mode PROD: START/STOP hanya lewat web (perintah Firebase -> setRun(..., "cloud")), tombol ditolak
bool buttonRunAllowed()
{
  if (mode != MODE_PROD)
    return true;
  showToast("Kontrol lewat web");
  beep(3, 40, 40);
  return false;
}

void toggleRun(int idx)
{
  if (!buttonRunAllowed())
    return;
  if (idx < 0)
    setRun(-1, !anyWantRun(), "gs");
  else
    setRun(idx, !wantRun[idx], "gs");
}

// Permintaan dari tombol node (dipanggil dari handleRadio)
void applyNodeRequest(uint8_t idx, uint8_t req)
{
  if (!buttonRunAllowed())
    return; // node versi baru sudah menolak sendiri di PROD; ini untuk jaga-jaga
  char src[8];
  snprintf(src, sizeof(src), "node%d", idx + 1);
  setRun((req & REQ_ALL) ? -1 : (int)idx, (req & REQ_RUN) != 0, src);
}

// Rate hanya boleh diubah saat semua node STOP (supaya satu sesi rekaman satu rate)
bool setRate(uint8_t newIdx, const char *src)
{
  if (anyWantRun())
    return false;
  rateIdx = newIdx;
  loadAckPayloads();
  logEvent("RATE", "ALL", src);
  redrawNow = true;
  return true;
}

void changeRate(int dir)
{
  int idx = (int)rateIdx + dir;
  if (idx < 0 || idx >= (int)NUM_RATES)
  {
    beep(1, 15);
    return;
  }
  if (!setRate(idx, "gs"))
  {
    showToast("Rate terkunci");
    beep(3, 40, 40);
    return;
  } // ditolak saat merekam
  showToast("Rate %u Hz", RATE_OPTIONS[rateIdx]);
  beep(1, 30);
}

void loadSettings()
{
  Preferences prefs;
  prefs.begin("gs", true);
  buzzerOn = prefs.getBool("buzz", true);
  prefs.end();
}

void saveSettings()
{
  Preferences prefs;
  prefs.begin("gs", false);
  prefs.putBool("buzz", buzzerOn);
  prefs.end();
}

// Sesi web hanya di mode PROD (cloud.cpp mematikan WiFi selain itu)
void applyCloud()
{
  cloudConfig(mode == MODE_PROD);
}

// Ganti mode saat berjalan (halaman SETTING). Mode ikut Cmd ke node; tidak disimpan (boot selalu PROD).
void applyMode(RunMode m, const char *src)
{
  if (m == mode)
  {
    beep(1, 15);
    return;
  }
  if (anyNodeOnline())
  { // ganti mode hanya saat semua node terputus (tidak di tengah sesi)
    showToast("Matikan semua node");
    beep(3, 40, 40);
    return;
  }
  const char *name = (m == MODE_PROD) ? "PROD" : "DEBUG";
  logEvent("MODE", name, src); // DEBUG -> PROD: tercatat sebelum serial diam
  mode = m;
  if (mode == MODE_DEBUG)
  { // PROD -> DEBUG: serial aktif lagi
    dbg("#GS,%s,%s\n", FW_VERSION, "DEBUG");
    logEvent("MODE", name, src);
  }
  for (uint8_t i = 0; i < NUM_NODES; i++)
    wantRun[i] = false; // mode baru = sesi baru: node mulai dari STOP (PROD tidak bisa STOP lewat tombol)
  loadAckPayloads();    // node pindah mode pada ACK berikutnya
  applyCloud();         // WiFi menyala / mati mengikuti mode
  showToast(m == MODE_PROD ? "Mode PRODUCTION" : "Mode DEBUG");
  beep(2, 40, 60);
  redrawNow = true;
}

// ============================ WIFI (scan & ganti) ==========================
// Layar WiFi (SETTING > WiFi) menggantikan halaman sampai ditutup:
//   Daftar : UP/DOWN pilih, OK = pilih jaringan / scan ulang, LEFT = kembali ke SETTING
//   Sandi  : panah = gerak, OK = ketik / tombol bawah, tahan OK = sambung
//   Sambung: cloud.cpp mencoba; berhasil -> tersimpan, gagal -> jaringan lama tetap dipakai
// Keyboard yang sama dipakai untuk device ID node (SETTING > Cloud > ID node n): UI_WIFI_ID
#define WIFI_MIN_PASS 8      // WPA/WPA2: sandi minimal 8 karakter
#define WIFI_OK_SHOW_MS 2000 // layar "Terhubung" sebelum kembali ke SETTING
UiWifiScreen wifiScreen = UI_WIFI_OFF;
UiAp wAps[UI_AP_MAX];
uint8_t wApN = 0, wApSel = 0;
bool wApLoaded = false; // hasil scan sudah disalin dari cloud.cpp
char wTarget[33] = "";  // jaringan yang dipilih
bool wTargetSecure = false;
char wPass[64] = ""; // sandi yang diketik (WPA maks 63)
uint8_t wPassLen = 0;
uint8_t kbPage = 0, kbRow = 0, kbCol = 0;
uint32_t wResultMs = 0; // millis() hasil sambung pertama terlihat
uint8_t kbNode = 0;     // node yang device ID-nya diketik (UI_WIFI_ID)
char kbTitleBuf[12] = "";

void wifiScan()
{
  cloudScan();
  wApLoaded = false;
  wApN = 0;
  wApSel = 0;
}

void wifiOpen()
{
  wifiScreen = UI_WIFI_LIST;
  wifiScan();
  beep(1, 15);
}

void wifiConnect()
{
  cloudConnect(wTarget, wTargetSecure ? wPass : "");
  wifiScreen = UI_WIFI_CONNECT;
  wResultMs = 0;
  beep(1, 40);
}

// OK di daftar: baris 0 = scan ulang, lainnya = pilih jaringan
void wifiChoose()
{
  if (wApSel == 0 || wApSel > wApN)
  {
    wifiScan();
    beep(1, 15);
    return;
  }
  const UiAp &a = wAps[wApSel - 1];
  strlcpy(wTarget, a.ssid, sizeof(wTarget));
  wTargetSecure = a.secure;
  wPass[0] = 0;
  wPassLen = 0;
  if (!a.secure)
  {
    wifiConnect(); // jaringan terbuka: langsung sambung
    return;
  }
  kbPage = kbRow = kbCol = 0;
  wifiScreen = UI_WIFI_PASS;
  beep(1, 15);
}

void kbType(char c)
{
  if (wPassLen >= sizeof(wPass) - 1)
  {
    beep(3, 30, 30);
    return;
  }
  wPass[wPassLen++] = c;
  wPass[wPassLen] = 0;
  beep(1, 10);
}

// Keyboard device ID: mulai dari ID sekarang
void idOpen(uint8_t idx)
{
  kbNode = idx;
  strlcpy(wPass, cloudNode(idx).did, sizeof(wPass));
  wPassLen = strlen(wPass);
  snprintf(kbTitleBuf, sizeof(kbTitleBuf), "ID NODE %u", idx + 1);
  kbPage = 1; // ID biasanya huruf besar + angka
  kbRow = kbCol = 0;
  wifiScreen = UI_WIFI_ID;
  beep(1, 15);
}

void idSubmit()
{
  if (!cloudSetDeviceId(kbNode, wPass))
  {
    showToast(wPassLen ? "Hanya A-Z 0-9 - _" : "ID kosong");
    beep(3, 40, 40);
    return;
  }
  char t[24];
  snprintf(t, sizeof(t), "N%u", kbNode + 1);
  logEvent("DEVICEID", t, wPass);
  showToast("ID N%u: %s", kbNode + 1, wPass);
  wifiScreen = UI_WIFI_OFF; // kembali ke SETTING
  beep(2, 40, 60);
}

void kbSubmit()
{
  if (wifiScreen == UI_WIFI_ID)
  {
    idSubmit();
    return;
  }
  if (wPassLen < WIFI_MIN_PASS)
  {
    showToast("Min %u karakter", WIFI_MIN_PASS);
    beep(3, 40, 40);
    return;
  }
  wifiConnect();
}

void kbPress()
{
  if (kbRow < UI_KB_ROWS)
  {
    kbType(UI_KB[kbPage][kbRow][kbCol]);
    return;
  }
  switch (kbCol)
  {
  case UI_KEY_PAGE:
    kbPage = (kbPage + 1) % UI_KB_PAGES;
    beep(1, 15);
    break;
  case UI_KEY_SPACE:
    kbType(' ');
    break;
  case UI_KEY_DEL:
    if (wPassLen)
      wPass[--wPassLen] = 0;
    beep(1, 15);
    break;
  case UI_KEY_CANCEL:
    wifiScreen = wifiScreen == UI_WIFI_ID ? UI_WIFI_OFF : UI_WIFI_LIST; // ID: kembali ke SETTING
    beep(1, 15);
    break;
  default:
    kbSubmit();
  }
}

// Gerak kursor: baris karakter (10 kolom) <-> baris tombol bawah (5 tombol, 2 kolom per tombol)
void kbMove(int8_t dRow, int8_t dCol)
{
  if (dRow)
  {
    uint8_t rows = UI_KB_ROWS + 1;
    uint8_t nr = (kbRow + rows + dRow) % rows;
    if (kbRow == UI_KB_ROWS && nr != UI_KB_ROWS)
      kbCol *= 2;
    else if (kbRow != UI_KB_ROWS && nr == UI_KB_ROWS)
      kbCol /= 2;
    kbRow = nr;
  }
  else
  {
    uint8_t n = kbRow == UI_KB_ROWS ? UI_KB_KEYS : UI_KB_COLS;
    kbCol = (kbCol + n + dCol) % n;
  }
}

void wifiButtons()
{
  BtnEvent eUp = updateButton(btnUp), eDown = updateButton(btnDown);
  BtnEvent eLeft = updateButton(btnLeft), eRight = updateButton(btnRight);
  BtnEvent eEnt = updateButton(btnEnter);
  bool up = eUp == BTN_EV_PRESS, down = eDown == BTN_EV_PRESS;
  bool left = eLeft == BTN_EV_PRESS, right = eRight == BTN_EV_PRESS;
  bool ok = eEnt == BTN_EV_SHORT, okLong = eEnt == BTN_EV_LONG;
  if (up || down || left || right || ok || okLong)
    redrawNow = true;

  if (wifiScreen == UI_WIFI_LIST)
  {
    uint8_t n = wApN + 1;
    if (up || down)
    {
      wApSel = (wApSel + n + (up ? -1 : 1)) % n;
      beep(1, 10);
    }
    if (left)
    {
      wifiScreen = UI_WIFI_OFF; // kembali ke SETTING
      beep(1, 15);
    }
    else if (ok)
      wifiChoose();
  }
  else if (wifiScreen == UI_WIFI_PASS || wifiScreen == UI_WIFI_ID)
  {
    if (up || down)
      kbMove(up ? -1 : 1, 0);
    if (left || right)
      kbMove(0, left ? -1 : 1);
    if (ok)
      kbPress();
    else if (okLong)
      kbSubmit();
  }
  else // UI_WIFI_CONNECT
  {
    CloudWifiJob job = cloudStatus().job;
    if (job == CWJ_CONNECT_FAIL)
    {
      if (left)
        wifiScreen = UI_WIFI_LIST;
      else if (ok && wTargetSecure)
        wifiScreen = UI_WIFI_PASS; // perbaiki sandi
      else if (ok)
        wifiConnect();
    }
    else if (job == CWJ_CONNECT_OK && (ok || left))
      wifiScreen = UI_WIFI_OFF;
  }
}

// Tiap loop(): salin hasil scan, tanggapi hasil sambung
void wifiUpdate(uint32_t now)
{
  if (wifiScreen == UI_WIFI_OFF)
    return;
  CloudStatus cs = cloudStatus();
  if (wifiScreen == UI_WIFI_LIST && !wApLoaded && cs.job == CWJ_SCAN_DONE)
  {
    CloudAp aps[CLOUD_AP_MAX];
    wApN = cloudScanResults(aps, CLOUD_AP_MAX);
    for (uint8_t i = 0; i < wApN; i++)
    {
      memcpy(wAps[i].ssid, aps[i].ssid, sizeof(wAps[i].ssid));
      wAps[i].rssi = aps[i].rssi;
      wAps[i].secure = aps[i].secure;
    }
    wApLoaded = true;
    wApSel = wApN ? 1 : 0;
    redrawNow = true;
  }
  if (wifiScreen == UI_WIFI_CONNECT && (cs.job == CWJ_CONNECT_OK || cs.job == CWJ_CONNECT_FAIL))
  {
    if (!wResultMs)
    {
      wResultMs = now ? now : 1;
      if (cs.job == CWJ_CONNECT_OK)
        beep(2, 60, 80);
      else
        beep(3, 40, 40);
      redrawNow = true;
    }
    else if (cs.job == CWJ_CONNECT_OK && now - wResultMs >= WIFI_OK_SHOW_MS)
    {
      wifiScreen = UI_WIFI_OFF;
      showToast("WiFi: %s", wTarget);
    }
  }
}

// ============================== DEMO ALARM =================================
// Periode bunyi tiap tingkat = sama dengan BUZZ_STAGE_PERIOD_MS di node (bunyi 200 ms)
const uint16_t DEMO_PERIOD_MS[BUZZ_STAGES] = {5000, 3000, 1000};

// Ganti tingkat yang dibunyikan (0 = diam); node ikut lewat Cmd.buzz pada ACK berikutnya
void demoSetStage(uint8_t stage)
{
  if (stage == demoStage)
    return;
  demoStage = stage;
  for (uint8_t i = 0; i < NUM_NODES; i++)
    nodeBuzzStage[i] = stage;
  if (stage)
    demoLastBeep = millis() - DEMO_PERIOD_MS[stage - 1]; // langsung berbunyi
  loadAckPayloads();
  redrawNow = true;
}

void demoStart()
{
  demoT0 = millis();
  demoStage = 0; // paksa demoSetStage menerapkan tingkat awal
  demoSetStage(demoSel == UI_DEMO_ALL ? 1 : demoSel + 1);
  logEvent("DEMO", "ALARM", "gs");
}

void demoStop()
{
  demoSetStage(0);
}

// Tiap loop(): ganti tingkat / selesai sesuai waktu, bunyikan buzzer GS
void demoUpdate(uint32_t now)
{
  if (!demoStage)
    return;
  uint32_t el = now - demoT0;
  if (demoSel == UI_DEMO_ALL)
  {
    if (el >= (uint32_t)DEMO_STAGE_MS * BUZZ_STAGES)
    {
      demoStop();
      showToast("Demo selesai");
      return;
    }
    demoSetStage(1 + el / DEMO_STAGE_MS);
  }
  else if (el >= DEMO_STAGE_MS)
  {
    demoStop();
    showToast("Demo selesai");
    return;
  }
  if (now - demoLastBeep >= DEMO_PERIOD_MS[demoStage - 1])
  {
    demoLastBeep = now;
    beep(1, 200, 0);
  }
  redrawNow = true; // hitung mundur detik
}

void demoClose()
{
  demoStop();
  demoOpen = false;
  setMenu = UI_MENU_BUZZER;
  setItem = UI_BZ_DEMO;
  redrawNow = true;
}

void demoButtons()
{
  BtnEvent eUp = updateButton(btnUp), eDown = updateButton(btnDown);
  BtnEvent eLeft = updateButton(btnLeft);
  updateButton(btnRight);
  BtnEvent eEnt = updateButton(btnEnter);
  if (eLeft == BTN_EV_PRESS)
  {
    demoClose();
    return;
  }
  if ((eUp == BTN_EV_PRESS || eDown == BTN_EV_PRESS) && !demoStage)
  { // pilihan terkunci selama berbunyi
    demoSel = eUp == BTN_EV_PRESS ? (demoSel + UI_DEMO_ALL) % (UI_DEMO_ALL + 1) : (demoSel + 1) % (UI_DEMO_ALL + 1);
    redrawNow = true;
  }
  if (eEnt == BTN_EV_SHORT)
  {
    if (demoStage)
      demoStop();
    else
      demoStart();
    redrawNow = true;
  }
}

// ENTER di halaman SETTING: ubah nilai item yang dipilih / buka submenu
void settingEnter()
{
  if (setMenu == UI_MENU_TOP)
  {
    if (setItem == UI_TOP_MODE)
    {
      applyMode(mode == MODE_DEBUG ? MODE_PROD : MODE_DEBUG, "gs");
    }
    else
    {
      setMenu = setItem == UI_TOP_BUZZER ? UI_MENU_BUZZER : UI_MENU_CLOUD;
      setItem = 0;
      beep(1, 15);
    }
  }
  else if (setMenu == UI_MENU_BUZZER)
  {
    if (setItem == UI_BZ_GS)
    {
      buzzerOn = !buzzerOn;
      saveSettings();
      logEvent("BUZZER", buzzerOn ? "ON" : "OFF", "gs");
      showToast(buzzerOn ? "Buzzer GS ON" : "Buzzer GS OFF");
      beep(1, 80); // hanya terdengar kalau baru dinyalakan
    }
    else if (setItem == UI_BZ_DEMO)
    {
      demoOpen = true;
      demoSel = 0;
    }
    else
    { // ambang buzzer node: default duduk & berdiri 1 mnt, buzzerRule web (/deviceState) menggantikan satu posisi
      showToast("Diatur dari web");
      beep(1, 15);
    }
  }
  else // UI_MENU_CLOUD
  {
    if (setItem == UI_CL_WIFI)
      wifiOpen();
    else
      idOpen(setItem == UI_CL_ID1 ? 0 : 1);
  }
  redrawNow = true;
}

// LEFT di submenu SETTING: kembali ke menu utama (item submenu itu terpilih)
void settingBack()
{
  setItem = setMenu == UI_MENU_BUZZER ? UI_TOP_BUZZER : UI_TOP_CLOUD;
  setMenu = UI_MENU_TOP;
  beep(1, 15);
  redrawNow = true;
}

void doReset(const char *src)
{
  resetStats();
  logEvent("RESET", "ALL", src);
  showToast("Statistik direset");
  beep(1, 300);
  redrawNow = true;
}

// ============================ PERINTAH DARI PC =============================
void processPcCommand(char *cmd)
{
  for (char *p = cmd; *p; p++)
    *p = toupper((unsigned char)*p);

  char word[8] = "";
  int arg = -1;
  if (sscanf(cmd, "%7s %d", word, &arg) < 1)
    return;

  if (!strcmp(word, "PING"))
  {
    dbg("OK,PING\n");
  }
  else if (!strcmp(word, "START") || !strcmp(word, "STOP"))
  {
    bool run = !strcmp(word, "START");
    int idx = -1;
    if (arg != -1)
    {
      if (arg < 1 || arg > NUM_NODES)
      {
        dbg("ERR,%s,NODE\n", word);
        return;
      }
      idx = arg - 1;
    }
    setRun(idx, run, "pc");
    dbg("OK,%s\n", word);
  }
  else if (!strcmp(word, "RATE"))
  {
    int found = -1;
    for (uint8_t i = 0; i < NUM_RATES; i++)
      if (RATE_OPTIONS[i] == arg)
        found = i;
    if (found < 0)
    {
      dbg("ERR,RATE,NILAI\n");
      return;
    }
    if (!setRate(found, "pc"))
    {
      dbg("ERR,RATE,MEREKAM\n");
      return;
    }
    dbg("OK,RATE\n");
  }
  else if (!strcmp(word, "RESET"))
  {
    doReset("pc");
    dbg("OK,RESET\n");
  }
  else if (!strcmp(word, "CAL"))
  {
    int idx = -1;
    if (arg != -1)
    {
      if (arg < 1 || arg > NUM_NODES)
      {
        dbg("ERR,CAL,NODE\n");
        return;
      }
      idx = arg - 1;
    }
    requestCal(idx, "pc");
    dbg("OK,CAL\n");
  }
  else
  {
    dbg("ERR,%s,TIDAK_DIKENAL\n", word);
  }
}

void handlePcSerial()
{
  static char buf[32];
  static uint8_t len = 0;
  while (Serial.available())
  {
    char c = Serial.read();
    if (c == '\r')
      continue;
    if (c != '\n')
    {
      if (len < sizeof(buf) - 1)
        buf[len++] = c;
      continue;
    }
    buf[len] = 0;
    len = 0;
    pcLastMs = millis();
    processPcCommand(buf);
  }
}

// ================================ RADIO ====================================
// Baris F ke PC. Float ditulis %.9g supaya nilai float32 dari node utuh.
void printFeat(uint8_t nodeId, const FeatWin &w)
{
  if (mode != MODE_DEBUG)
    return;
  char line[600];
  int len = snprintf(line, sizeof(line), "F,%lu,%u,%u,%lu,%lu,%u,%u,%u", (unsigned long)millis(),
                     nodeId, w.win, (unsigned long)w.tStartMs, (unsigned long)w.tEndMs,
                     w.n, w.rateHz, w.version);
  for (int k = 0; k < FEAT_COUNT && len < (int)sizeof(line) - 24; k++)
    len += snprintf(line + len, sizeof(line) - len, ",%.9g", (double)w.f[k]);
  line[len++] = '\n';
  line[len] = 0;
  Serial.print(line);
}

// Kumpulkan bagian jendela fitur; lengkap -> baris F
void handleFeatPacket(const FeatPacket &fp)
{
  uint8_t idx = fp.nodeId - 1;
  FeatRx &r = featRx[idx];
  NodeStat &n = nodes[idx];
  if (r.doneValid && fp.win == r.doneWin)
    return; // kiriman ulang (ACK hilang) dari jendela yang sudah lengkap
  if (!r.active || fp.win != r.win)
  {
    if (r.active)
      n.featDrop++; // jendela sebelumnya tidak lengkap
    r.active = true;
    r.win = fp.win;
    r.mask = 0;
  }
  memcpy(r.buf + fp.part * FEAT_CHUNK, fp.data, FEAT_CHUNK);
  r.mask |= 1 << fp.part;
  if (r.mask != FEAT_ALL_PARTS)
    return;

  r.active = false;
  r.doneValid = true;
  r.doneWin = r.win;
  n.featOk++;
  FeatWin w;
  memcpy(&w, r.buf, sizeof(w));
  printFeat(fp.nodeId, w);
}

// Hasil model dari node: simpan untuk OLED (PROD), teruskan ke PC (DEBUG)
void handleResult(const ResultPacket &r)
{
  NodeStat &n = nodes[r.nodeId - 1];
  bool dup = n.resSeen && n.res.win == r.win; // kiriman ulang (ACK hilang)
  n.res = r;
  n.resSeen = true;
  n.resRxMs = millis();
  if (dup)
    return;
  if (r.act < ACT_COUNT && (r.flags & RES_HAS_ACT))
    cloudReport(r.nodeId - 1, r.act, (r.prob[r.act] * 100 + 127) / 255); // bacaan sesi web (mode PROD)
  dbg("P,%lu,%u,%u,%lu,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long)n.resRxMs, r.nodeId, r.win,
      (unsigned long)r.tEndMs, r.act, r.prob[0], r.prob[1], r.prob[2], r.prob[3], r.paceS, r.flags);
  // OLED diperbarui periodik (OLED_PERIOD_MS): hasil datang 5x/detik per node
}

void handleRadio()
{
  uint8_t pipe;
  while (radio.available(&pipe))
  {
    uint8_t len = radio.getDynamicPayloadSize();
    if (len == 0)
    { // payload korup, sudah di-flush oleh library
      badPkt++;
      continue;
    }
    if (len == sizeof(ResultPacket))
    {
      ResultPacket r;
      radio.read(&r, sizeof(r));
      if (pipe >= 1 && pipe <= NUM_NODES)
        reloadAck(pipe);
      if (r.magic != PKT_RES_MAGIC || r.nodeId < 1 || r.nodeId > NUM_NODES || pipe != r.nodeId ||
          (r.act != ACT_NONE && r.act >= ACT_COUNT))
      {
        badPkt++;
        continue;
      }
      handleResult(r);
      continue;
    }
    if (len == sizeof(FeatPacket))
    {
      FeatPacket fp;
      radio.read(&fp, sizeof(fp));
      if (pipe >= 1 && pipe <= NUM_NODES)
        reloadAck(pipe);
      if (fp.magic != PKT_FEAT_MAGIC || fp.nodeId < 1 || fp.nodeId > NUM_NODES ||
          pipe != fp.nodeId || fp.part >= FEAT_PARTS)
      {
        badPkt++;
        continue;
      }
      handleFeatPacket(fp);
      continue;
    }
    if (len != sizeof(Packet))
    {
      uint8_t junk[32];
      radio.read(junk, len);
      badPkt++;
      continue;
    }

    Packet p;
    radio.read(&p, sizeof(p));
    uint32_t rxMs = millis();
    if (pipe >= 1 && pipe <= NUM_NODES)
      reloadAck(pipe);

    if (p.magic != PKT_MAGIC || p.nodeId < 1 || p.nodeId > NUM_NODES || pipe != p.nodeId)
    {
      badPkt++;
      continue;
    }

    NodeStat &n = nodes[p.nodeId - 1];
    bool baseline = !n.seen; // paket pertama: jangan anggap permintaan baru
    if (n.seen)
    {
      uint16_t gap = (uint16_t)(p.seq - n.last.seq);
      if (gap == 0)
        continue; // duplikat
      if (gap < 1000)
        n.lost += gap - 1;
      else
        baseline = true; // gap besar = node reboot
    }

    uint8_t prevCal = n.last.cal, prevCalCnt = n.last.calCount;
    bool firstPkt = !n.seen;

    n.seen = true;
    n.last = p;
    n.lastRxMs = rxMs;
    n.rx++;
    n.rateCnt++;

    digitalWrite(PIN_LED_G, HIGH);
    ledGOffAt = rxMs + 20;

    // Payload ke PC (mode DEBUG)
    if (p.flags & FLAG_STREAMING)
    {
      dbg("D,%lu,%u,%u,%lu,%d,%d,%d,%d,%d,%d,%u,%u\n",
          (unsigned long)rxMs, p.nodeId, p.seq, (unsigned long)p.tMs,
          p.ax, p.ay, p.az, p.gx, p.gy, p.gz, p.battMv, p.flags);
    }
    else
    {
      dbg("H,%lu,%u,%u,%u,%u\n", (unsigned long)rxMs, p.nodeId, p.seq, p.battMv, p.flags);
    }

    // Kalibrasi: node sudah mulai / selesai -> lepas perintah, laporkan perubahan ke PC
    uint8_t idx = p.nodeId - 1;
    bool calDone = !baseline && p.calCount != prevCalCnt;
    if (calPending[idx] && ((p.flags & FLAG_CALIBRATING) || calDone))
    {
      calPending[idx] = false;
      loadAckPayloads();
    }
    if (firstPkt || p.cal != prevCal || p.calCount != prevCalCnt)
    {
      dbg("C,%lu,%u,%u,%u\n", (unsigned long)rxMs, p.nodeId, p.cal, p.calCount);
      redrawNow = true;
    }
    if (calDone)
    {
      uint8_t st = p.cal & 0x0F;
      if (st == CAL_OK || st == CAL_GYRO_ONLY)
        beep(2, 60, 80);
      else
        beep(3, 40, 40);
    }

    // Permintaan START/STOP dari tombol node
    if (baseline)
    {
      n.lastReq = p.req;
    }
    else if (p.req != n.lastReq)
    {
      n.lastReq = p.req;
      applyNodeRequest(p.nodeId - 1, p.req);
    }
  }
}

// ================================ TAMPILAN =================================
// Semua gambar ada di ui.cpp; di sini hanya mengisi UiState dari status ground station.

// Hasil model terbaru node; nullptr jika belum ada / sudah basi
const ResultPacket *freshResult(uint8_t idx, uint32_t now)
{
  NodeStat &n = nodes[idx];
  if (!n.resSeen || now - n.resRxMs > RES_STALE_MS)
    return nullptr;
  return &n.res;
}

void fillUiNode(UiNode &u, uint8_t idx, uint32_t now)
{
  NodeStat &n = nodes[idx];
  const char *st = nodeState(n, now);
  u.state = !strcmp(st, "RUN")   ? UI_NODE_RUN
            : !strcmp(st, "STP") ? UI_NODE_STOP
            : !strcmp(st, "CAL") ? UI_NODE_CAL
            : !strcmp(st, "OFF") ? UI_NODE_OFF
                                 : UI_NODE_NONE;
  u.pending = pendingMark(idx, now) == '*';
  u.pktHz = n.rateHz;
  u.lossPct = lossPct(n);
  u.battMv = n.last.battMv;
  u.imuOk = n.last.flags & FLAG_IMU_OK;
  u.cal = n.last.cal;
  u.calPending = calPending[idx];
  u.acc[0] = n.last.ax / 1000.0f;
  u.acc[1] = n.last.ay / 1000.0f;
  u.acc[2] = n.last.az / 1000.0f;
  u.gyr[0] = n.last.gx / 10.0f;
  u.gyr[1] = n.last.gy / 10.0f;
  u.gyr[2] = n.last.gz / 10.0f;
  const ResultPacket *r = freshResult(idx, now);
  u.hasResult = r != nullptr;
  u.act = r ? r->act : UI_ACT_NONE;
  if (r)
  {
    memcpy(u.prob, r->prob, sizeof(u.prob));
    u.paceS = r->paceS;
    u.flags = r->flags;
  }
  CloudNode cn = cloudNode(idx); // sesi web
  memcpy(u.did, cn.did, sizeof(u.did));
  u.sess = cn.sess;
  u.buzzStage = nodeBuzzStage[idx]; // tingkat yang sama dengan yang dikirim ke node (Cmd.buzz)
  u.watchSit = cn.watchSitting;
  u.thrSit = cn.thrSitMin;
  u.thrStand = cn.thrStandMin;
  u.runMin = cn.runMin;
  u.queued = cn.queued;
}

void drawOled()
{
  uint32_t now = millis();
  UiState s;
  memset(&s, 0, sizeof(s));
  s.nowMs = now;
  s.page = page;
  s.numPages = NUM_PAGES;
  s.prod = mode == MODE_PROD;
  s.pcOnline = pcOnline();
  s.anyRun = anyWantRun();
  s.rateHz = RATE_OPTIONS[rateIdx];
  s.gsBattV = gsBattV;
  s.gsBattPct = gsBattPct;
  s.badPkt = badPkt;
  s.buzzerOn = buzzerOn;
  s.setMenu = setMenu;
  s.setItem = setItem;
  s.netCheck = netCheck;
  s.netSecLeft = netCheck == UI_NET_CHECK ? (NET_CHECK_MS - (now - netCheckMs) + 999) / 1000 : 0;
  s.demo = demoOpen;
  s.demoSel = demoSel;
  s.demoStage = demoStage;
  if (demoStage)
  {
    uint32_t el = now - demoT0;
    uint32_t into = demoSel == UI_DEMO_ALL ? el % DEMO_STAGE_MS : (el < DEMO_STAGE_MS ? el : DEMO_STAGE_MS);
    uint32_t left = DEMO_STAGE_MS - into;
    s.demoLeftS = (left + 999) / 1000;
  }
  CloudStatus cs = cloudStatus();
  s.cloud = cs.state;
  s.cloudCode = cs.code;
  s.cloudAgoS = cs.lastOkMs ? (now - cs.lastOkMs) / 1000 : UI_CLOUD_NEVER;
  s.wifiSsid = cs.ssid; // cs hidup sampai uiDraw() selesai
  s.wifiUp = cs.wifiUp;
  s.wifiRssi = cs.rssi;
  s.wifiScreen = wifiScreen;
  s.wifiJob = cs.job;
  s.wifiReason = cs.reason;
  s.aps = wAps;
  s.apCount = wApLoaded ? wApN : 0;
  s.apSel = wApSel;
  s.wTarget = wTarget;
  s.wPass = wPass;
  s.kbTitle = wifiScreen == UI_WIFI_ID ? kbTitleBuf : "SANDI";
  s.kbHint = wifiScreen == UI_WIFI_ID ? "A-Z 0-9 - _" : wTarget;
  s.kbPage = kbPage;
  s.kbRow = kbRow;
  s.kbCol = kbCol;
  s.toast = (toastUntil && (int32_t)(toastUntil - now) > 0) ? toastBuf : nullptr;
  for (uint8_t i = 0; i < NUM_NODES; i++)
    fillUiNode(s.node[i], i, now);
  uiDraw(oled, s);
  oled.display();
}

// Animasi boot "HAR - Human Activity Tracker" (ui.cpp)
void bootAnimation()
{
  uint32_t t0 = millis();
  bool chirp = false;
  while (true)
  {
    uint32_t t = millis() - t0;
    bool more = uiBoot(oled, t, FW_VERSION);
    oled.display();
    if (!chirp && t >= 1100)
    {
      beep(1, 30); // logo mendarat
      chirp = true;
    }
    buzzerUpdate();
    if (!more)
      break;
  }
  while (bz.remaining)
  {
    buzzerUpdate();
    delay(1);
  }
}

// ================================ SESI WEB =================================
// Mode PROD: node RUN selama sesinya merekam (/deviceState active), buzzer node mengikuti aturan web
void webSync()
{
  if (mode != MODE_PROD)
    return;
  bool cmdChanged = false;
  for (uint8_t i = 0; i < NUM_NODES; i++)
  {
    CloudNode cn = cloudNode(i);
    bool want = cn.sess == SESS_RECORDING;
    if (want != wantRun[i])
      setRun(i, want, "web");
    uint8_t stage = buzzStageFor(cn);
    if (demoStage)
      continue; // demo alarm memegang tingkat buzzer node
    if (stage != nodeBuzzStage[i])
    {
      bool justStarted = nodeBuzzStage[i] == 0 && stage > 0;
      nodeBuzzStage[i] = stage;
      cmdChanged = true;
      if (justStarted)
      { // aturan terpenuhi: GS ikut memberi tahu (buzzer node berbunyi lewat Cmd)
        beep(3, 200, 150);
        showToast("N%u %s %u mnt!", i + 1, cn.watchSitting ? "duduk" : "berdiri", cn.runMin);
      }
    }
  }
  if (cmdChanged)
    loadAckPayloads();
}

// ============================== CEK KONEKSI ================================
// Saat boot (mode PROD, kirim ON): layar KONEKSI sampai WiFi + login Firebase OK (ONLINE)
// atau gagal / NET_CHECK_MS (OFFLINE). OK = lewati.
void netCheckStart()
{
  if (mode != MODE_PROD)
    return;
  netCheck = UI_NET_CHECK;
  netCheckMs = millis();
}

void netCheckUpdate(uint32_t now)
{
  if (netCheck == UI_NET_NONE)
    return;
  CloudState st = cloudStatus().state;
  if (netCheck == UI_NET_CHECK)
  {
    if (st == CLOUD_ONLINE)
    {
      netCheck = UI_NET_ONLINE;
      netCheckMs = now;
      beep(2, 40, 60);
      Serial.println("#NET ONLINE");
    }
    else if (st == CLOUD_ERR || now - netCheckMs >= NET_CHECK_MS)
    {
      netCheck = UI_NET_OFFLINE;
      netCheckMs = now;
      beep(3, 150, 100);
      Serial.printf("#NET OFFLINE (cloud %u)\n", st);
    }
  }
  else if (now - netCheckMs >= (netCheck == UI_NET_ONLINE ? NET_ONLINE_SHOW_MS : NET_OFFLINE_SHOW_MS))
  {
    netCheck = UI_NET_NONE;
    redrawNow = true;
  }
}

// ================================ INPUT ====================================
void handleButtons()
{
  if (netCheck != UI_NET_NONE)
  { // layar KONEKSI: OK = lewati (tombol lain diabaikan)
    updateButton(btnUp);
    updateButton(btnDown);
    updateButton(btnLeft);
    updateButton(btnRight);
    if (updateButton(btnEnter) == BTN_EV_SHORT)
    {
      netCheck = UI_NET_NONE;
      beep(1, 15);
      redrawNow = true;
    }
    return;
  }
  if (wifiScreen != UI_WIFI_OFF)
  {
    wifiButtons();
    return;
  }
  if (demoOpen)
  {
    demoButtons();
    return;
  }
  BtnEvent eRight = updateButton(btnRight), eLeft = updateButton(btnLeft);
  if (page == PAGE_SETTING && setMenu != UI_MENU_TOP)
  { // submenu SETTING: LEFT = kembali, RIGHT tidak dipakai
    if (eLeft == BTN_EV_PRESS)
      settingBack();
  }
  else if (eRight == BTN_EV_PRESS || eLeft == BTN_EV_PRESS)
  {
    page = eRight == BTN_EV_PRESS ? (page + 1) % NUM_PAGES : (page + NUM_PAGES - 1) % NUM_PAGES;
    setMenu = UI_MENU_TOP;
    redrawNow = true;
    beep(1, 15);
  }
  bool up = updateButton(btnUp) == BTN_EV_PRESS;
  bool down = updateButton(btnDown) == BTN_EV_PRESS;
  if (page == PAGE_SETTING && (up || down))
  {
    uint8_t n = setItemCount(setMenu);
    setItem = up ? (setItem + n - 1) % n : (setItem + 1) % n;
    beep(1, 15);
    redrawNow = true;
  }
  else if (up || down)
  {
    changeRate(up ? +1 : -1);
    redrawNow = true;
  }

  BtnEvent e = updateButton(btnEnter);
  if (page == PAGE_SETTING)
  {
    if (e == BTN_EV_SHORT)
      settingEnter();
  }
  else if (e == BTN_EV_SHORT)
    toggleRun(page == 0 ? -1 : page - 1);
  else if (e == BTN_EV_LONG)
  {
    if (page == 0)
      doReset("gs");
    else
      requestCal(page - 1, "gs"); // node harus diam & datar
  }
}

// ================================ RADIO ====================================
#define RADIO_CHECK_MS 1000 // cek register nRF tiap ...
uint32_t radioReinitCount = 0;

// Semua setting radio (HARUS sama dengan node). Dipakai saat boot dan saat pemulihan.
// false = data rate ditolak modul (bukan nRF24L01+ asli)
bool radioConfigure()
{
  radio.setPALevel(RF_PA_LEVEL);
  bool rateOk = radio.setDataRate(RF_DATA_RATE);
  radio.setChannel(RF_CHANNEL);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(true);
  radio.enableDynamicPayloads(); // wajib untuk ACK payload
  radio.enableAckPayload();

  radio.openReadingPipe(1, addrNode[0]);
  radio.openReadingPipe(2, addrNode[1]);
  radio.startListening();
  loadAckPayloads(); // perintah sesuai status sekarang (boot: semua STOP)
  return rateOk;
}

bool radioReinit(const char *why)
{
  radioReinitCount++;
  bool ok = radio.begin(&SPI) && radioConfigure();
  Serial.printf("#NRF INIT ULANG #%lu (%s): %s\n", (unsigned long)radioReinitCount, why,
                ok ? "OK" : "GAGAL");
  return ok;
}

// nRF bisa berhenti menerima tanpa tanda apa pun: tegangan turun sesaat (arus TX PA / WiFi)
// mereset registernya ke default (channel 2, 2 Mbps, power-down). Tanpa ini GS harus di-reset.
void radioWatchdog(uint32_t now)
{
  static uint32_t lastCheck = 0;
  static bool silentHandled = false; // init ulang "semua node diam" sudah dicoba di gangguan ini
  if (now - lastCheck < RADIO_CHECK_MS)
    return;
  lastCheck = now;

  // 1) Channel kembali ke default (2) = nRF ter-reset (hampir pasti masalah daya).
  //    Data rate tidak dicek: modul klon bisa memang menolak 250 kbps.
  if (radio.getChannel() != RF_CHANNEL)
  {
    showToast(radioReinit("register reset, cek daya nRF") ? "Radio dipulihkan" : "Radio error!");
    return;
  }

  // 2) Register benar tapi semua node yang pernah terlihat diam (node selalu memancar:
  //    heartbeat 4 Hz saat STOP, laporan 5 s di PROD) -> mungkin radio macet: init ulang SEKALI.
  //    Tanpa toast: node yang memang dimatikan juga terlihat seperti ini.
  bool anySeen = false;
  for (uint8_t i = 0; i < NUM_NODES; i++)
    if (nodes[i].seen)
      anySeen = true;
  if (!anySeen || anyNodeOnline())
  {
    silentHandled = false;
    return;
  }
  if (!silentHandled)
  {
    silentHandled = true;
    radioReinit("semua node diam");
  }
}

// ============================== SETUP & LOOP ===============================
void setup()
{
  Serial.setTxBufferSize(2048); // cukup untuk burst data 2 node x 50 Hz
  Serial.begin(SERIAL_BAUD);
  delay(300);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED_G, OUTPUT);
  digitalWrite(PIN_LED_G, LOW);
  pinMode(PIN_LED_R, OUTPUT);
  digitalWrite(PIN_LED_R, LOW);
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT); // GPIO34/35 tanpa pull-up internal
  pinMode(PIN_BTN_LEFT, INPUT);
  pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);
  pinMode(PIN_BTN_ENT, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    Serial.println("#OLED tidak terdeteksi!");
  }
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);

  loadSettings();
  bootAnimation();
  mode = MODE_PROD; // boot selalu PRODUCTION; DEBUG lewat SETTING
  Serial.println("#MODE PRODUCTION (boot)");

  uiSplash(oled, "Menyiapkan radio...", mode == MODE_DEBUG ? "mode DEBUG" : "mode PRODUCTION", millis());
  oled.display();

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
  // nRF dicoba terus tiap NRF_RETRY_MS: kabel/daya bisa dibetulkan tanpa reset GS
  uint16_t attempt = 0;
  while (!radio.begin(&SPI))
  {
    attempt++;
    Serial.printf("#nRF24L01 TIDAK terdeteksi! (percobaan %u, coba lagi tiap %u ms)\n",
                  attempt, NRF_RETRY_MS);
    digitalWrite(PIN_LED_R, HIGH);
    if (attempt == 1)
      beep(5, 100, 100);
    uint32_t tWait = millis();
    while (millis() - tWait < NRF_RETRY_MS)
    {
      uiNrfError(oled, attempt, millis());
      oled.display();
      buzzerUpdate();
      delay(20);
    }
  }
  digitalWrite(PIN_LED_R, LOW);
  if (attempt)
  {
    Serial.printf("#nRF24L01 terdeteksi setelah %u percobaan\n", attempt);
    beep(1, 150);
  }

  if (!radioConfigure())
  {
    Serial.println("#nRF24: data rate " RF_INFO " ditolak modul!");
    uiSplash(oled, "nRF24: data rate", RF_INFO " ditolak modul!", millis());
    oled.display();
    beep(5, 40, 40);
    uint32_t tMsg = millis();
    while (millis() - tMsg < 2000)
    {
      buzzerUpdate();
      delay(5);
    }
  }

  resetStats();
  readGsBattery();

  dbg("#GS,%s,%s\n", FW_VERSION, mode == MODE_DEBUG ? "DEBUG" : "PRODUCTION");
  dbg("#FMT,D,gsMs,node,seq,nodeMs,ax,ay,az,gx,gy,gz,battMv,flags\n");
  dbg("#FMT,F,gsMs,node,win,tStartMs,tEndMs,n,rateHz,ver,f0..f%d\n", FEAT_COUNT - 1);
  dbg("#FMT,P,gsMs,node,win,tEndMs,act,p0,p1,p2,p3,paceS,flags\n");

  // History Firebase (task di core 0). Dimulai setelah nRF siap supaya arus WiFi tidak
  // mengganggu inisialisasi radio.
  cloudBegin();
  applyCloud();
  netCheckStart(); // layar KONEKSI: user langsung tahu GS online / offline
}

void loop()
{
  handleRadio();
  radioWatchdog(millis());
  if (mode == MODE_DEBUG)
    handlePcSerial();
  handleButtons();
  buzzerUpdate();

  uint32_t now = millis();

  // ---- Tiap 1 detik: rate paket, baterai, statistik ke PC ----
  static uint32_t lastSec = 0;
  if (now - lastSec >= 1000)
  {
    lastSec = now;
    for (auto &n : nodes)
    {
      n.rateHz = n.rateCnt;
      n.rateCnt = 0;
    }
    readGsBattery();

    for (uint8_t i = 0; i < NUM_NODES; i++)
    {
      NodeStat &n = nodes[i];
      dbg("S,%lu,%u,%s,%u,%lu,%lu,%u,%lu,%lu\n", (unsigned long)now, i + 1, nodeState(n, now),
          n.rateHz, (unsigned long)n.rx, (unsigned long)n.lost, n.last.cal,
          (unsigned long)n.featOk, (unsigned long)n.featDrop);
    }
    dbg("G,%lu,%.2f,%d,%lu\n", (unsigned long)now, gsBattV, gsBattPct, (unsigned long)badPkt);
  }

  // ---- Alarm baterai base station lemah (tiap 60 detik) ----
  static uint32_t lastLowBeep = 0;
  if (gsBattV > 2.5f && gsBattPct < BATT_LOW_PCT && now - lastLowBeep >= 60000)
  {
    lastLowBeep = now;
    beep(2, 200, 150);
  }

  // ---- Deteksi node hilang / kembali saat sedang merekam ----
  bool anyOff = false;
  for (uint8_t i = 0; i < NUM_NODES; i++)
  {
    NodeStat &n = nodes[i];
    bool online = n.seen && (now - n.lastRxMs) < nodeTimeoutMs();
    char target[12];
    snprintf(target, sizeof(target), "N%d", i + 1);

    if (n.online && !online && wantRun[i])
    {
      beep(3, 100, 100);
      logEvent("LOST", target, "gs");
      showToast("N%d hilang!", i + 1);
      n.lostLogged = true;
    }
    if (!n.online && online && n.lostLogged)
    {
      logEvent("BACK", target, "gs");
      showToast("N%d kembali", i + 1);
      n.lostLogged = false;
    }
    n.online = online;
    if (n.seen && !online)
      anyOff = true;
  }

  // ---- LED ----
  if (ledGOffAt && (int32_t)(now - ledGOffAt) >= 0)
  {
    digitalWrite(PIN_LED_G, LOW);
    ledGOffAt = 0;
  }
  digitalWrite(PIN_LED_R, anyOff ? HIGH : LOW);

  wifiUpdate(now);
  netCheckUpdate(now);
  demoUpdate(now);
  cloudLoop(); // bacaan sesi web per intervalSec (tidak menunggu jaringan)
  webSync();

  // ---- OLED ----
  static uint32_t lastDraw = 0;
  if (redrawNow || now - lastDraw >= (mode == MODE_PROD ? OLED_PERIOD_PROD_MS : OLED_PERIOD_MS))
  {
    redrawNow = false;
    lastDraw = now;
    drawOled();
  }
}

#include <Arduino.h>

/*
 * Node TX v5 - MPU6050 + baterai + buzzer + tombol -> nRF24 -> Ground Station, kontrol START/STOP
 * Board : ESP32-C3 SuperMini
 * Upload kode yang SAMA ke kedua node, cukup ganti NODE_ID (1 atau 2).
 *
 * Perilaku:
 *   - Default saat nyala: STOP -> kirim heartbeat 4 Hz (baterai + status), tanpa data IMU
 *   - Ground station kirim perintah RUN/STOP + sample rate lewat ACK payload
 *   - RUN -> kirim data IMU sesuai sample rate
 *
 * Tombol node (GPIO10 ke GND):
 *   tekan singkat      : START/STOP node ini
 *   tahan 1-3 detik    : START/STOP SEMUA node (bunyi tik di detik ke-1, aksi saat dilepas)
 *   tahan >= 3 detik   : KALIBRASI IMU node ini (lepas tombol, letakkan datar, jangan disentuh)
 *   Node hanya MEMINTA START/STOP, ground station yang memutuskan -> status selalu sinkron.
 *   Mode PROD: START/STOP lewat tombol ditolak (bunyi 3x), hanya lewat Firebase; kalibrasi tetap bisa.
 *   Butuh link ke ground station; kalau putus, permintaan ditolak (3x cepat).
 *
 * Kalibrasi IMU (dari app / GS / tombol node):
 *   Node harus DIAM dan DATAR (sisi mana saja menghadap bawah) selama ~2 detik.
 *   - gyro : bias dihitung ulang
 *   - accel: offset dihitung, sumbu dengan gravitasi terbesar dianggap vertikal (+-1 g)
 *   Bergerak -> gagal, kalibrasi lama tetap dipakai. Tidak datar -> hanya gyro.
 *   Hasil disimpan di flash (Preferences "imucal") dan dipakai lagi setelah reboot.
 *   Saat boot bias gyro tetap diukur ulang; kalau node bergerak saat boot,
 *   bias gyro dari flash yang dipakai.
 *
 * Bunyi buzzer:
 *   klik               : tombol ditekan, permintaan dikirim
 *   1x pendek          : kalibrasi dimulai (jangan disentuh)
 *   2x pendek          : kalibrasi selesai / berhasil
 *   3x cepat           : kalibrasi gagal (bergerak / IMU error)
 *   3x cepat           : permintaan ditolak (link putus)
 *   2x pendek        : kalibrasi gyro selesai, node boleh digerakkan
 *   1x panjang       : mulai RUN
 *   2x pendek        : STOP
 *   3x pendek        : link ke ground station putus (> 2 detik tanpa ACK)
 *   1x pendek        : link tersambung lagi
 *   5x cepat         : MPU6050 tidak terdeteksi
 *   2x panjang/60 dtk: baterai node lemah
 *
 * Fitur jendela (dataset activity recognition / model di node):
 *   Saat RUN, setiap FEAT_STEP_SEC detik node menghitung 30 fitur dari FEAT_WIN_SEC detik
 *   terakhir (rumus di features.h, accel/gyro resolusi penuh & terkalibrasi) lalu mengirimnya
 *   dalam FEAT_PARTS paket FeatPacket di sela paket data. Ground station menyusun ulang dan
 *   meneruskan ke PC sebagai baris F,...
 *   standing_indicator memakai sumbu "atas saat berdiri": STAND_AXIS / STAND_SIGN (features.h,
 *   bisa diubah per node lewat build flag di platformio.ini).
 *
 * Model EBM (ebm_model.h, dibuat tools/export_ebm.py dari model interpret / har_ebm_model.c):
 *   Jendela model = EBM_WIN_SAMPLES sampel (20 @ 50 Hz = 0,4 s), baru setiap EBM_HOP_SAMPLES.
 *   Fitur model = har_extract_features() (har_features.c, rumus pipeline training) dari nilai
 *   terkuantisasi yang sama dengan data yang direkam app (mg, 0,1 dps) -> tebak aktivitas
 *   (+ pace untuk Walking/Running) -> ResultPacket. Fitur features.h (baris F) terpisah.
 *   Saat boot model diuji dengan vektor uji di ebm_model.h (hasil di serial monitor).
 *
 * Mode (dipilih di ground station, dikirim lewat ACK):
 *   DEBUG      : data mentah tiap sampel + fitur + hasil model (untuk rekam dataset)
 *   PRODUCTION : sample rate = EBM_RATE_HZ (rate data training, 40 Hz). Setiap PROD_REPORT_SEC
 *                (5 s = 10 jendela x 20 sampel) hasil semua jendela digabung (voting) lalu
 *                dikirim sekali ke ground station bersama paket status. Di antaranya node tidak
 *                memancar (kecuali tombol ditekan), jadi perintah STOP dari GS paling lambat 5 s.
 *
 * Duduk / berdiri (node di paha): kalau model menebak Sitting/Standing, keputusan akhirnya
 *   dari arah gravitasi. SIT_AXIS = sumbu accel yang TEGAK saat duduk (paha mendatar, hasil uji
 *   hardware: X). |cos| sudut gravitasi ke sumbu itu >= SIT_MIN_COS (45 derajat) -> Sitting,
 *   selain itu (sumbu itu mendatar = paha tegak) -> Standing. Tidak terpengaruh putaran node
 *   di sekitar sumbu itu. Serial monitor menampilkan accel rata-rata + keputusan tiap detik.
 *
 * Buzzer sesi web (mode PROD): ground station menghitung aturan dari web (duduk / berdiri terlalu
 *   lama, ESP32_GUIDE.md §8), lalu mengirim Cmd.buzz sebagai TINGKAT 0..BUZZ_STAGES (bukan on/off):
 *   makin lama overtime, dan makin sering siswa "akal-akalan" balik ke posisi itu dalam satu sesi,
 *   makin tinggi tingkatnya -> buzzer makin sering & makin keras (BUZZ_STAGE_* di bawah). Berhenti
 *   begitu posisi berubah / sesi STOP (ground station mengirim Cmd.buzz = 0).
 *
 * PENTING: biarkan node DIAM ~1 detik saat dinyalakan (kalibrasi bias gyro).
 */

#define RF24_SPI_SPEED 1000000

#include <SPI.h>
#include <Wire.h>
#include <RF24.h>
#include <Preferences.h>
#include <math.h>
#include "features.h"
#include "ebm_model.h"
#include "har_ebm.h"

static_assert(EBM_WIN_SAMPLES == HAR_WINDOW_SIZE, "EBM_WIN_SAMPLES harus sama dengan HAR_WINDOW_SIZE");

// ================== KONFIGURASI ==================
#ifndef NODE_ID
#define NODE_ID 1 // <-- default; di PlatformIO diisi lewat env node1/node2
#endif
#define HB_PERIOD_MS 250 // heartbeat saat STOP (4 Hz)
#define DEFAULT_RATE 10  // Hz, dipakai sampai ada perintah dari ground station

// nRF24
#define PIN_CE 3
#define PIN_CS 7
#define PIN_SCK 4
#define PIN_MISO 5
#define PIN_MOSI 6

// MPU6050
#define PIN_SDA 8
#define PIN_SCL 9

// Buzzer
#define PIN_BUZZER 0
#define BUZZER_ACTIVE 1  // 1 = buzzer aktif (HIGH/LOW), 0 = buzzer pasif (pakai tone)
#define BUZZER_FREQ 2700 // hanya untuk buzzer pasif

// Baterai node -> GPIO1 (R1 = ke BAT+, R2 = ke GND), tambahkan 100nF di GPIO1-GND
#define PIN_BATT 1
const float R1 = 27.0; // <-- sesuaikan dengan resistor yang terpasang
const float R2 = 100.0;
const float DIVIDER_RATIO = (R1 + R2) / R2;
const uint8_t BATT_SAMPLES = 16;
const uint16_t BATT_LOW_MV = 3400; // alarm baterai lemah

#define LINK_LOST_MS 2000 // tanpa ACK selama ini = link putus

// Tombol START/STOP (aktif LOW, pull-up internal). Jangan pakai GPIO9 (BOOT) karena itu SCL.
#define PIN_BTN 10
#define LONG_PRESS_MS 1000
#define CAL_PRESS_MS 3000 // tahan selama ini = kalibrasi IMU

// Kalibrasi IMU
#define CAL_SAMPLES 400          // x 5 ms = 2 detik
#define CAL_BOOT_SAMPLES 200     // saat boot, 1 detik (sama seperti sebelumnya)
#define CAL_PERIOD_MS 5
#define CAL_DELAY_APP_MS 500     // jeda setelah bunyi mulai (perintah dari app/GS)
#define CAL_DELAY_BUTTON_MS 2000 // jeda setelah tombol: waktu untuk melepas & meletakkan node
#define CAL_TIMEOUT_MS 6000
#define CAL_MAX_GYRO_SD 0.6f     // dps, lebih dari ini = bergerak
#define CAL_MAX_ACC_SD 0.02f     // g
#define CAL_MAX_TILT_G 0.20f     // g di sumbu horizontal, lebih = tidak datar

// Fitur jendela
#define FEAT_WIN_SEC 2       // panjang jendela, detik (HARUS sama dengan app/core/features.py)
#define FEAT_STEP_SEC 1      // jendela baru setiap ... detik
#define FEAT_MAX_SAMPLES 200 // kapasitas buffer = FEAT_WIN_SEC x rate maks (100 Hz)
#define FEAT_TX_GAP_MS 2     // jeda antar paket fitur
#define FEAT_TX_RETRY 3      // satu bagian gagal terkirim lebih dari ini -> jendela dibuang
// =================================================

#define RF_CHANNEL 76
#define RF_PA_LEVEL RF24_PA_MAX     // jangkauan maks; WAJIB kapasitor 10-100 uF di VCC nRF (bisa macet tanpa itu)
#define RF_DATA_RATE RF24_250KBPS   // kepekaan +9 dB vs 1 Mbps (hanya nRF24L01+ / Si24R1)
#define TX_SLACK_MS 6               // paket sela (hasil/fitur) hanya jika sampel berikutnya >= ini lagi
                                    // (250 kbps: ~2,5 ms per paket + ACK, retry menambah ~3 ms)
#define PKT_MAGIC 0xA5
#define CMD_MAGIC 0x5A
#define PKT_FEAT_MAGIC 0xA6
#define PKT_RES_MAGIC 0xA7
#define MODE_DEBUG 0
#define MODE_PROD 1
#define RES_HAS_ACT 0x01    // ResultPacket.flags: model aktivitas ada
#define RES_HAS_PACE 0x02   // pace diisi
#define RES_RATE_DIFF 0x04  // sample rate beda dengan data training
#define RES_TEST_FAIL 0x08  // self-test model gagal saat boot
#define RES_AGGREGATED 0x10 // PROD: gabungan beberapa jendela (prob = porsi suara per kelas)
#define RES_POSTURE 0x20    // aturan sumbu paha mengubah tebakan Sitting/Standing model
// PROD: buzzer sesi web, tingkat 1..3 dari Cmd.buzz (0 = mati), 1 menit sampai tingkat maks
// (GS: overtime + pelanggaran berulang, lihat buzz_stage.h & webSync() di ground station):
//   1 pemberitahuan (0-30 dtk) : beep 0,2 dtk tiap 5 dtk
//   2 peringatan    (30-60 dtk): beep 0,2 dtk tiap 3 dtk
//   3 hukuman       (>= 60 dtk): beep 0,2 dtk tiap 1 dtk
#define BUZZ_STAGES 3
const uint32_t BUZZ_STAGE_PERIOD_MS[BUZZ_STAGES] = {5000, 3000, 1000};
const uint8_t BUZZ_STAGE_COUNT[BUZZ_STAGES] = {1, 1, 1};
const uint16_t BUZZ_STAGE_ON_MS[BUZZ_STAGES] = {200, 200, 200};
const uint16_t BUZZ_STAGE_OFF_MS[BUZZ_STAGES] = {0, 0, 0}; // count = 1: tidak dipakai

// Mode PRODUCTION
#define PROD_REPORT_SEC 5          // kirim hasil gabungan setiap ... detik
#define PROD_STATUS_MAX_MS 10000   // tanpa laporan (mis. IMU error) tetap kirim status tiap ...
#define PROD_LINK_LOST_MS 12000    // RUN di PROD memancar tiap 5 s -> batas link putus lebih lama
#define SIT_AXIS 0                 // sumbu accel yang tegak saat DUDUK: 0 = X, 1 = Y, 2 = Z
#define SIT_MIN_COS 0.7071f        // |cos| >= cos 45 derajat -> Sitting, lebih kecil -> Standing
#define ACT_NONE 0xFF
#define ACT_SITTING 0 // kode aktivitas, HARUS sama dengan app/core/labels.py
#define ACT_STANDING 1
#define ACT_WALKING 2
#define ACT_RUNNING 3
#define ACT_COUNT 4
#define PACE_MIN_S 120 // s/km, sama dengan app/core/pace.py
#define PACE_MAX_S 1800
#define FEAT_CHUNK 28 // byte FeatWin per paket
#define FEAT_PARTS 5  // paket per jendela
#define FLAG_IMU_OK 0x01
#define FLAG_STREAMING 0x02
#define FLAG_CALIBRATING 0x04
#define REQ_RUN 0x80
#define REQ_ALL 0x40
#define REQ_CNT_MASK 0x3F
#define MPU_ADDR 0x68
#define ACC_LSB_G 8192.0f    // +-4 g
#define GYRO_LSB_DPS 65.5f   // +-500 dps

// Status kalibrasi (Packet.cal bit0-3), HARUS sama dengan ground station & app
#define CAL_NONE 0
#define CAL_RUNNING 1
#define CAL_OK 2         // gyro + accel
#define CAL_GYRO_ONLY 3  // tidak datar: hanya gyro
#define CAL_FAIL_MOVE 4  // bergerak: kalibrasi lama dipakai
#define CAL_FAIL_IMU 5
#define CAL_BOOT 6       // bias gyro dari pengukuran saat boot
#define CAL_BOOT_SAVED 7 // bergerak saat boot: bias gyro dari flash
#define CAL_ACC_VALID 0x80 // bit7: offset accel dari kalibrasi tersimpan

RF24 radio(PIN_CE, PIN_CS);
const byte addrNode[][6] = {"1Node", "2Node"};

// HARUS identik dengan ground station
struct __attribute__((packed)) Packet
{
  uint8_t magic;
  uint8_t nodeId;
  uint16_t seq;
  uint8_t flags; // bit0 = IMU OK, bit1 = RUN
  uint32_t tMs;
  int16_t ax, ay, az; // mg
  int16_t gx, gy, gz; // 0.1 dps
  uint16_t battMv;
  uint8_t req;      // permintaan tombol: bit7 = RUN, bit6 = SEMUA node, bit0-5 = counter
  uint8_t cal;      // bit0-3 status kalibrasi (CAL_*), bit7 = offset accel valid
  uint8_t calCount; // bertambah setiap kalibrasi selesai (berhasil atau gagal)
};

struct __attribute__((packed)) Cmd
{
  uint8_t magic;
  uint8_t run;
  uint8_t rateHz;
  uint8_t cal; // 1 = mulai kalibrasi (ground station menahan sampai node mulai)
  uint8_t mode; // MODE_DEBUG / MODE_PROD
  uint8_t buzz; // 1 = bunyikan buzzer (aturan sesi web terpenuhi, mode PROD)
};

// Fitur satu jendela, dikirim terpecah dalam FEAT_PARTS FeatPacket (HARUS identik dengan ground station)
struct __attribute__((packed)) FeatWin
{
  uint32_t tStartMs; // millis() sampel pertama jendela
  uint32_t tEndMs;   // millis() sampel terakhir
  uint16_t win;      // nomor jendela
  uint8_t n;         // jumlah sampel
  uint8_t rateHz;
  uint8_t version;   // FEAT_VERSION
  uint8_t reserved[3];
  float f[FEAT_COUNT]; // urutan kolom: lihat features.h
};

struct __attribute__((packed)) FeatPacket
{
  uint8_t magic; // PKT_FEAT_MAGIC
  uint8_t nodeId;
  uint8_t win;   // 8 bit bawah FeatWin.win (penyusunan ulang di ground station)
  uint8_t part;  // 0 .. FEAT_PARTS-1
  uint8_t data[FEAT_CHUNK];
};

static_assert(sizeof(Packet) <= 32, "Packet melebihi 32 byte");
static_assert(sizeof(FeatPacket) == 32, "FeatPacket harus 32 byte");
static_assert(sizeof(FeatWin) <= FEAT_PARTS * FEAT_CHUNK, "FeatWin tidak muat di FEAT_PARTS paket");

// Hasil model satu jendela (HARUS identik dengan ground station)
struct __attribute__((packed)) ResultPacket
{
  uint8_t magic; // PKT_RES_MAGIC
  uint8_t nodeId;
  uint16_t win;    // nomor jendela fitur
  uint32_t tEndMs; // millis() sampel terakhir jendela
  uint8_t act;     // kode aktivitas tebakan, ACT_NONE = tanpa model
  uint8_t prob[ACT_COUNT]; // probabilitas per kode aktivitas x 255
  uint16_t paceS;  // pace s/km, 0 = tidak ada
  uint8_t flags;   // RES_*
};
static_assert(sizeof(ResultPacket) == 16, "ResultPacket harus 16 byte");

uint16_t seq = 0;
uint32_t txOk = 0, txFail = 0;
bool imuOk = false;
uint32_t lastImuRetry = 0;
int32_t gyroBias[3] = {0, 0, 0};
int32_t accelOff[3] = {0, 0, 0}; // LSB, dikurangkan dari raw accel
bool accelCalValid = false;
bool savedGyro = false;
uint8_t calStatus = CAL_NONE;
uint8_t calCount = 0;
Preferences prefs;
uint16_t battMv = 0;
float battMvFilt = 0;

bool streaming = false; // default STOP
uint8_t rateHz = DEFAULT_RATE;
uint16_t periodMs = 1000 / DEFAULT_RATE;

uint32_t lastAckMs = 0;
bool linkUp = false;
uint32_t nextTx = 0;

// Fitur jendela
ImuSample featBuf[FEAT_MAX_SAMPLES];  // ring buffer sampel RUN
uint32_t featT[FEAT_MAX_SAMPLES];     // millis() per sampel
double featScratch[2 * FEAT_MAX_SAMPLES];
uint16_t featHead = 0, featCount = 0, featSince = 0;
uint16_t featWinSeq = 0;
uint8_t featTx[FEAT_PARTS * FEAT_CHUNK]; // jendela yang sedang dikirim
uint8_t featTxWin = 0;
int8_t featPart = -1; // bagian berikut yang dikirim, -1 = tidak ada
uint8_t featRetry = 0;
uint32_t featLastTx = 0;
uint32_t featSent = 0, featDropped = 0;

// Mode & hasil model
uint8_t nodeMode = MODE_DEBUG;
uint8_t cmdRateHz = DEFAULT_RATE; // rate dari ground station (mode DEBUG)
uint32_t lastStatusTx = 0;
ResultPacket res;
bool resPending = false;
uint8_t resRetry = 0;
bool modelTestOk = true;

// Gabungan hasil jendela untuk laporan PROD
struct ProdAgg
{
  uint16_t votes[ACT_COUNT];
  float probSum[ACT_COUNT];
  uint16_t n;
  uint32_t paceSum;
  uint16_t paceN;
  uint8_t flags;
  uint32_t tEndMs;
} agg;
uint16_t reportSeq = 0;
bool statusNow = false; // kirim paket status secepatnya (laporan PROD / tombol)
void aggAdd(const ResultPacket &w);
void aggReport();
uint16_t prodReportWindows();

uint8_t buzzStage = 0; // Cmd.buzz dari ground station: 0 mati, 1..BUZZ_STAGES

// Jendela model HAR (nilai terkuantisasi = data yang direkam app)
ImuSample harBuf[EBM_WIN_SAMPLES];
uint32_t harT[EBM_WIN_SAMPLES];
uint16_t harHead = 0, harCount = 0, harSince = 0, harWinSeq = 0;
har_window_t harWin;

uint8_t reqCnt = 0;  // bertambah setiap tombol ditekan
uint8_t reqByte = 0; // dikirim di setiap paket sampai ada permintaan baru

// Tombol (tipe ditaruh sebelum fungsi pertama, lihat catatan Arduino IDE)
enum BtnEvent : uint8_t
{
  BTN_EV_NONE,
  BTN_EV_SHORT, // dilepas < 1 detik
  BTN_EV_HOLD,  // masih ditekan, sudah 1 detik (untuk bunyi tik)
  BTN_EV_LONG,  // dilepas antara 1-3 detik
  BTN_EV_XLONG  // masih ditekan, sudah 3 detik
};
struct Button
{
  uint8_t pin;
  bool stable;
  bool lastRead;
  uint32_t tChange;
  uint32_t tPress;
  bool holdFired;
  bool xlongFired;
};
Button btn = {PIN_BTN, HIGH, HIGH, 0, 0, false, false};

// Kalibrasi non-blocking (tipe ditaruh sebelum fungsi pertama)
struct CalRun
{
  bool active;
  uint32_t startAt; // mulai ambil sampel (setelah bunyi / tangan dilepas)
  uint32_t lastMs;
  uint32_t deadline;
  uint16_t n;
  double sum[6], sq[6]; // ax ay az gx gy gz (LSB)
};
CalRun cal = {};

Packet pkt;

// ================== Buzzer (non-blocking) ==================
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

void beep(uint8_t count, uint16_t onMs = 60, uint16_t offMs = 80)
{
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

// Untuk dipakai di setup() sebelum loop jalan
void beepBlocking(uint8_t count, uint16_t onMs = 60, uint16_t offMs = 80)
{
  beep(count, onMs, offMs);
  while (bz.remaining)
  {
    buzzerUpdate();
    delay(1);
  }
}

// ================== Tombol ==================
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
      b.holdFired = b.xlongFired = false;
    }
    else
    { // dilepas
      if (b.xlongFired)
        return BTN_EV_NONE; // kalibrasi sudah dipicu
      return b.holdFired ? BTN_EV_LONG : BTN_EV_SHORT;
    }
  }
  if (b.stable == LOW)
  {
    uint32_t held = now - b.tPress;
    if (!b.holdFired && held >= LONG_PRESS_MS)
    {
      b.holdFired = true;
      return BTN_EV_HOLD;
    }
    if (!b.xlongFired && held >= CAL_PRESS_MS)
    {
      b.xlongFired = true;
      return BTN_EV_XLONG;
    }
  }
  return BTN_EV_NONE;
}

// Kirim permintaan kebalikan status sekarang; ground station yang menerapkan
void requestToggle(bool all)
{
  if (!linkUp)
  {
    beep(3, 40, 40);
    Serial.println("Tombol: link putus, permintaan ditolak");
    return;
  }
  if (nodeMode == MODE_PROD)
  { // PROD: START/STOP hanya lewat perintah Firebase (ground station); tombol node hanya kalibrasi
    beep(3, 40, 40);
    Serial.println("Tombol: mode PROD, START/STOP hanya lewat Firebase");
    return;
  }
  reqCnt = (reqCnt + 1) & REQ_CNT_MASK;
  reqByte = (streaming ? 0 : REQ_RUN) | (all ? REQ_ALL : 0) | reqCnt;
  nextTx = millis(); // kirim secepatnya
  statusNow = true;  // juga saat PROD (status biasanya hanya tiap 5 s)
  beep(1, 20);
  Serial.printf("Tombol: minta %s %s\n", streaming ? "STOP" : "RUN", all ? "SEMUA" : "node ini");
}

// ================== MPU6050 ==================
bool mpuWrite(uint8_t reg, uint8_t val)
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool mpuReadRaw(int16_t raw[7])
{ // ax ay az temp gx gy gz
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)14) != 14)
    return false;
  for (int i = 0; i < 7; i++)
  {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    raw[i] = (int16_t)((hi << 8) | lo);
  }
  return true;
}

bool mpuInit()
{
  if (!mpuWrite(0x6B, 0x00))
    return false;
  delay(50);
  mpuWrite(0x6B, 0x01); // clock PLL gyro X
  mpuWrite(0x1A, 0x03); // DLPF ~44 Hz
  mpuWrite(0x1B, 0x08); // gyro  +-500 dps -> 65.5 LSB/dps
  mpuWrite(0x1C, 0x08); // accel +-4 g     -> 8192 LSB/g
  return true;
}

void featReset();

// ================== Kalibrasi IMU ==================
void calReset(CalRun &c)
{
  c.n = 0;
  for (int k = 0; k < 6; k++)
    c.sum[k] = c.sq[k] = 0;
}

void calAdd(CalRun &c, const int16_t raw[7])
{
  static const int idx[6] = {0, 1, 2, 4, 5, 6}; // lewati suhu (raw[3])
  for (int k = 0; k < 6; k++)
  {
    double v = raw[idx[k]];
    c.sum[k] += v;
    c.sq[k] += v * v;
  }
  c.n++;
}

// Hitung bias/offset dari sampel diam. Tidak mengubah apa pun kalau node bergerak.
// withAccel = false -> hanya bias gyro (dipakai saat boot).
uint8_t calApply(const CalRun &c, bool withAccel)
{
  double mean[6], sd[6];
  for (int k = 0; k < 6; k++)
  {
    mean[k] = c.sum[k] / c.n;
    double var = c.sq[k] / c.n - mean[k] * mean[k];
    sd[k] = var > 0 ? sqrt(var) : 0;
  }
  float gyroSd = fmax(sd[3], fmax(sd[4], sd[5])) / GYRO_LSB_DPS;
  float accSd = fmax(sd[0], fmax(sd[1], sd[2])) / ACC_LSB_G;
  Serial.printf("Kalibrasi: %u sampel, sd gyro %.2f dps, sd accel %.3f g\n", c.n, gyroSd, accSd);
  if (gyroSd > CAL_MAX_GYRO_SD || accSd > CAL_MAX_ACC_SD)
    return CAL_FAIL_MOVE;

  for (int k = 0; k < 3; k++)
    gyroBias[k] = lround(mean[3 + k]);
  if (!withAccel)
    return CAL_BOOT;

  // sumbu dengan gravitasi terbesar = vertikal (node datar di salah satu sisinya)
  int d = 0;
  for (int k = 1; k < 3; k++)
    if (fabs(mean[k]) > fabs(mean[d]))
      d = k;
  bool flat = fabs(mean[d]) > 0.8 * ACC_LSB_G && fabs(mean[d]) < 1.2 * ACC_LSB_G;
  for (int k = 0; k < 3; k++)
    if (k != d && fabs(mean[k]) > CAL_MAX_TILT_G * ACC_LSB_G)
      flat = false;
  if (!flat)
    return CAL_GYRO_ONLY;

  for (int k = 0; k < 3; k++)
  {
    double expected = (k == d) ? (mean[d] > 0 ? ACC_LSB_G : -ACC_LSB_G) : 0;
    accelOff[k] = lround(mean[k] - expected);
  }
  accelCalValid = true;
  return CAL_OK;
}

void calLoad()
{
  prefs.begin("imucal", true);
  savedGyro = prefs.getBool("g_ok", false);
  accelCalValid = prefs.getBool("a_ok", false);
  gyroBias[0] = prefs.getInt("gb0", 0);
  gyroBias[1] = prefs.getInt("gb1", 0);
  gyroBias[2] = prefs.getInt("gb2", 0);
  accelOff[0] = prefs.getInt("ao0", 0);
  accelOff[1] = prefs.getInt("ao1", 0);
  accelOff[2] = prefs.getInt("ao2", 0);
  prefs.end();
  if (!accelCalValid)
    accelOff[0] = accelOff[1] = accelOff[2] = 0;
  Serial.printf("Kalibrasi flash: gyro %s, accel %s (offset LSB %ld %ld %ld)\n",
                savedGyro ? "ada" : "-", accelCalValid ? "ada" : "-",
                (long)accelOff[0], (long)accelOff[1], (long)accelOff[2]);
}

void calSave(bool accel)
{
  prefs.begin("imucal", false);
  prefs.putBool("g_ok", true);
  prefs.putInt("gb0", gyroBias[0]);
  prefs.putInt("gb1", gyroBias[1]);
  prefs.putInt("gb2", gyroBias[2]);
  if (accel)
  {
    prefs.putBool("a_ok", true);
    prefs.putInt("ao0", accelOff[0]);
    prefs.putInt("ao1", accelOff[1]);
    prefs.putInt("ao2", accelOff[2]);
  }
  prefs.end();
  savedGyro = true;
}

// Saat boot (blocking, 1 detik): ukur bias gyro. Bergerak -> pakai bias dari flash.
void calibrateAtBoot()
{
  int32_t saved[3] = {gyroBias[0], gyroBias[1], gyroBias[2]};
  CalRun c;
  calReset(c);
  int16_t raw[7];
  for (int i = 0; i < CAL_BOOT_SAMPLES; i++)
  {
    if (mpuReadRaw(raw))
      calAdd(c, raw);
    delay(CAL_PERIOD_MS);
  }
  if (c.n < CAL_BOOT_SAMPLES / 2)
  {
    calStatus = CAL_FAIL_IMU;
    return;
  }
  uint8_t r = calApply(c, false);
  if (r == CAL_FAIL_MOVE)
  {
    if (savedGyro)
    {
      for (int k = 0; k < 3; k++)
        gyroBias[k] = saved[k];
      calStatus = CAL_BOOT_SAVED;
    }
    else
    { // belum pernah dikalibrasi: pakai rata-rata apa adanya (perilaku lama)
      for (int k = 0; k < 3; k++)
        gyroBias[k] = lround(c.sum[3 + k] / c.n);
      calStatus = CAL_FAIL_MOVE;
    }
  }
  else
  {
    calStatus = CAL_BOOT;
    calSave(false);
  }
  Serial.printf("Bias gyro (LSB): %ld %ld %ld  [status %u]\n",
                (long)gyroBias[0], (long)gyroBias[1], (long)gyroBias[2], calStatus);
}

void calFail(uint8_t status)
{
  cal.active = false;
  calStatus = status;
  calCount++;
  beep(3, 40, 40);
}

// Mulai kalibrasi non-blocking; pengambilan sampel mulai setelah delayMs
void startCalibration(uint16_t delayMs)
{
  if (cal.active)
    return;
  if (!imuOk)
  {
    calFail(CAL_FAIL_IMU);
    return;
  }
  calReset(cal);
  cal.active = true;
  cal.startAt = millis() + delayMs;
  cal.lastMs = 0;
  cal.deadline = cal.startAt + CAL_TIMEOUT_MS;
  calStatus = CAL_RUNNING;
  beep(1, 40); // selesai jauh sebelum sampel pertama (buzzer = getaran)
  Serial.println("Kalibrasi IMU: letakkan datar, jangan digerakkan");
}

void calibrationUpdate(uint32_t now)
{
  if (!cal.active || (int32_t)(now - cal.startAt) < 0)
    return;
  if (now - cal.lastMs >= CAL_PERIOD_MS)
  {
    cal.lastMs = now;
    int16_t raw[7];
    if (mpuReadRaw(raw))
      calAdd(cal, raw);
  }
  if (cal.n >= CAL_SAMPLES)
  {
    cal.active = false;
    uint8_t r = calApply(cal, true);
    if (r == CAL_FAIL_MOVE)
    {
      calFail(r);
      Serial.println("Kalibrasi GAGAL: node bergerak, kalibrasi lama tetap dipakai");
      return;
    }
    calSave(r == CAL_OK);
    calStatus = r;
    calCount++;
    featReset(); // offset berubah -> jendela fitur diulang
    beep(2, 60, 80);
    Serial.printf("Kalibrasi %s. Bias gyro %ld %ld %ld, offset accel %ld %ld %ld (LSB)\n",
                  r == CAL_OK ? "OK" : "gyro saja (tidak datar)",
                  (long)gyroBias[0], (long)gyroBias[1], (long)gyroBias[2],
                  (long)accelOff[0], (long)accelOff[1], (long)accelOff[2]);
  }
  else if ((int32_t)(now - cal.deadline) >= 0)
  {
    calFail(CAL_FAIL_IMU);
    Serial.println("Kalibrasi GAGAL: sampel IMU tidak cukup");
  }
}

// ================== Baterai ==================
void updateBattery()
{
  uint32_t acc = 0;
  for (uint8_t i = 0; i < BATT_SAMPLES; i++)
    acc += analogReadMilliVolts(PIN_BATT);
  float mv = (acc / (float)BATT_SAMPLES) * DIVIDER_RATIO;
  battMvFilt = (battMvFilt == 0) ? mv : (battMvFilt * 0.8f + mv * 0.2f);
  battMv = (uint16_t)battMvFilt;
}

// ================== Fitur jendela ==================
uint16_t featWinSamples()
{
  uint16_t n = (uint16_t)rateHz * FEAT_WIN_SEC;
  return n > FEAT_MAX_SAMPLES ? FEAT_MAX_SAMPLES : (n < 2 ? 2 : n);
}

// Buang isi jendela (RUN/STOP, rate, kalibrasi, IMU error). Bagian yang sedang dikirim tetap dikirim.
void featReset()
{
  featCount = featSince = 0;
  harCount = harSince = 0;
  memset(&agg, 0, sizeof(agg));
}

// ================== Model EBM ==================
uint8_t prob255(float p)
{
  return (uint8_t)lroundf(constrain(p, 0.0f, 1.0f) * 255.0f);
}

// Nilai model pace -> s/km
float paceToSecPerKm(float y)
{
#if EBM_PACE_UNIT == 1
  return y * 60.0f; // min/km
#elif EBM_PACE_UNIT == 2
  return y > 0.1f ? 3600.0f / y : 0.0f; // km/h
#else
  return y;
#endif
}

// Sitting / Standing dari rata-rata accel jendela (fitur accelX/Y/Z_mean), node di paha:
// paha tegak -> sumbu STAND_AXIS searah atas. ACT_NONE jika gravitasi tidak jelas.
// Pose terakhir (untuk serial monitor)
struct
{
  float a[3];      // accel rata-rata jendela, g
  float cosSit;    // |cos| sudut gravitasi ke SIT_AXIS
  uint8_t model;   // tebakan model sebelum aturan
  uint8_t final;   // hasil akhir
  bool valid;
} lastPose = {};

uint8_t postureRule(const float *f)
{
  float mx = f[HAR_F_ACCELX_MEAN], my = f[HAR_F_ACCELY_MEAN], mz = f[HAR_F_ACCELZ_MEAN];
  float g = sqrtf(mx * mx + my * my + mz * mz);
  lastPose.a[0] = mx;
  lastPose.a[1] = my;
  lastPose.a[2] = mz;
  lastPose.cosSit = g > 0 ? fabsf(f[HAR_F_ACCELX_MEAN + SIT_AXIS]) / g : 0;
  if (g < 0.5f)
    return ACT_NONE;
  return lastPose.cosSit >= SIT_MIN_COS ? ACT_SITTING : ACT_STANDING;
}

// f = 30 fitur har_extract_features(); win / tEndMs = jendela model
void runModels(const float *f, uint16_t win, uint32_t tEndMs, ResultPacket &r)
{
  memset(&r, 0, sizeof(r));
  r.magic = PKT_RES_MAGIC;
  r.nodeId = NODE_ID;
  r.win = win;
  r.tEndMs = tEndMs;
  r.act = ACT_NONE;
  if (!modelTestOk)
    r.flags |= RES_TEST_FAIL;
#if EBM_HAS_ACTIVITY
  float p[EBM_MAX_OUT], pc[ACT_COUNT] = {0, 0, 0, 0};
  uint8_t nc = ebmPredictProba(EBM_ACTIVITY, f, p);
  float best = -1;
  for (uint8_t k = 0; k < nc; k++)
  {
    pc[EBM_ACT_CODE[k]] = p[k];
    if (p[k] > best)
    {
      best = p[k];
      r.act = EBM_ACT_CODE[k];
    }
  }
  // Diam: duduk / berdiri diputuskan dari arah gravitasi (lebih andal daripada model)
  lastPose.model = r.act;
  lastPose.valid = true;
  uint8_t post = postureRule(f); // dihitung setiap jendela (serial monitor)
  if ((r.act == ACT_SITTING || r.act == ACT_STANDING) && post != ACT_NONE)
  {
    if (post != r.act)
      r.flags |= RES_POSTURE;
    pc[post] += pc[post == ACT_SITTING ? ACT_STANDING : ACT_SITTING];
    pc[post == ACT_SITTING ? ACT_STANDING : ACT_SITTING] = 0;
    r.act = post;
  }
  lastPose.final = r.act;
  for (uint8_t k = 0; k < ACT_COUNT; k++)
    r.prob[k] = prob255(pc[k]);
  r.flags |= RES_HAS_ACT;
  if (rateHz != EBM_RATE_HZ)
    r.flags |= RES_RATE_DIFF; // jendela 20 sampel jadi beda durasi dengan data training
#endif
#if EBM_HAS_PACE
  // pace hanya untuk aktivitas bergerak (tanpa model aktivitas: selalu)
  if (!EBM_HAS_ACTIVITY || r.act == ACT_WALKING || r.act == ACT_RUNNING)
  {
    float pace = paceToSecPerKm(ebmPredict(EBM_PACE, f));
    if (pace > 0)
    {
      r.paceS = (uint16_t)lroundf(constrain(pace, (float)PACE_MIN_S, (float)PACE_MAX_S));
      r.flags |= RES_HAS_PACE;
    }
  }
#endif
}

// Cek model dengan vektor uji dari exporter (hasil interpret di PC)
void modelSelfTest()
{
#if EBM_HAS_ACTIVITY || EBM_HAS_PACE
  float worst = 0;
  for (int i = 0; i < EBM_TEST_N; i++)
  {
#if EBM_HAS_ACTIVITY
    float p[EBM_MAX_OUT];
    ebmPredictProba(EBM_ACTIVITY, EBM_TEST_X[i], p);
    for (int k = 0; k < EBM_ACT_CLASSES; k++)
      worst = fmaxf(worst, fabsf(p[k] - EBM_TEST_ACT_PROB[i][k]));
#endif
#if EBM_HAS_PACE
    float y = ebmPredict(EBM_PACE, EBM_TEST_X[i]);
    worst = fmaxf(worst, fabsf(y - EBM_TEST_PACE_RAW[i]) / fmaxf(1.0f, fabsf(EBM_TEST_PACE_RAW[i])));
#endif
  }
  modelTestOk = worst < 1e-3f;
  uint32_t t0 = micros();
  float f[HAR_N_FEATURES];
  har_extract_features(&harWin, f);
  uint32_t t1 = micros();
  ResultPacket r;
  runModels(EBM_TEST_X[0], 0, 0, r);
  uint32_t t2 = micros();
  Serial.printf("Model EBM: aktivitas %s, pace %s, %u sampel @ %u Hz | self-test %s (selisih %.1e) | "
                "fitur %lu us + model %lu us\n",
                EBM_HAS_ACTIVITY ? "ada" : "-", EBM_HAS_PACE ? "ada" : "-", EBM_WIN_SAMPLES, EBM_RATE_HZ,
                modelTestOk ? "OK" : "GAGAL", worst, (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
  if (!modelTestOk)
    beepBlocking(4, 40, 40);
#else
  Serial.println("Model EBM: belum ada (jalankan tools/export_ebm.py)");
#endif
}

// API har_ebm.h (har_classify / har_ebm_predict) memakai model ebm_model.h:
// skor log-odds per kelas model (urutan EBM_ACT_CODE)
extern "C" void har_ebm_predict(const float features[HAR_N_FEATURES], float scores[HAR_N_CLASSES])
{
#if EBM_HAS_ACTIVITY
  double s[EBM_MAX_OUT];
  ebmScores(EBM_ACTIVITY, features, s);
  for (int k = 0; k < HAR_N_CLASSES; k++)
    scores[k] = k < EBM_ACTIVITY.nOut ? (float)s[k] : -1e30f;
#else
  for (int k = 0; k < HAR_N_CLASSES; k++)
    scores[k] = 0.0f;
#endif
}

// Jendela model penuh -> fitur HAR -> model -> ResultPacket
void harRun()
{
  for (uint16_t i = 0; i < EBM_WIN_SAMPLES; i++)
  {
    const ImuSample &s = harBuf[(harHead + i) % EBM_WIN_SAMPLES]; // harHead = sampel tertua
    harWin.accel_x[i] = s.a[0];
    harWin.accel_y[i] = s.a[1];
    harWin.accel_z[i] = s.a[2];
    harWin.gyro_x[i] = s.g[0];
    harWin.gyro_y[i] = s.g[1];
    harWin.gyro_z[i] = s.g[2];
  }
  float f[HAR_N_FEATURES];
  har_extract_features(&harWin, f);
  ResultPacket w;
  runModels(f, harWinSeq++, harT[(harHead + EBM_WIN_SAMPLES - 1) % EBM_WIN_SAMPLES], w);
  if (nodeMode != MODE_PROD)
  { // DEBUG: setiap jendela langsung dikirim
    res = w;
    resPending = true;
    resRetry = 0;
    return;
  }
  aggAdd(w);
  if (agg.n >= prodReportWindows())
    aggReport();
}

// Jumlah jendela per laporan PROD (40 Hz x 5 s / 20 sampel = 10)
uint16_t prodReportWindows()
{
  uint32_t n = (uint32_t)PROD_REPORT_SEC * rateHz / EBM_HOP_SAMPLES;
  return n < 1 ? 1 : (uint16_t)n;
}

void aggAdd(const ResultPacket &w)
{
  agg.flags |= w.flags;
  agg.tEndMs = w.tEndMs;
  if (w.act < ACT_COUNT)
  {
    agg.votes[w.act]++;
    for (uint8_t k = 0; k < ACT_COUNT; k++)
      agg.probSum[k] += w.prob[k] / 255.0f;
  }
  if (w.flags & RES_HAS_PACE)
  {
    agg.paceSum += w.paceS;
    agg.paceN++;
  }
  agg.n++;
}

// Voting mayoritas (seri -> probabilitas rata-rata terbesar) -> satu ResultPacket
void aggReport()
{
  ResultPacket &r = res;
  memset(&r, 0, sizeof(r));
  r.magic = PKT_RES_MAGIC;
  r.nodeId = NODE_ID;
  r.win = reportSeq++;
  r.tEndMs = agg.tEndMs;
  r.act = ACT_NONE;
  uint16_t voted = 0;
  for (uint8_t k = 0; k < ACT_COUNT; k++)
    voted += agg.votes[k];
  for (uint8_t k = 0; k < ACT_COUNT; k++)
  {
    r.prob[k] = voted ? (uint8_t)((agg.votes[k] * 255u + voted / 2) / voted) : 0;
    if (agg.votes[k] && (r.act == ACT_NONE || agg.votes[k] > agg.votes[r.act] ||
                         (agg.votes[k] == agg.votes[r.act] && agg.probSum[k] > agg.probSum[r.act])))
      r.act = k;
  }
  r.flags = (agg.flags & (RES_HAS_ACT | RES_RATE_DIFF | RES_TEST_FAIL | RES_POSTURE)) | RES_AGGREGATED;
  if (agg.paceN && (r.act == ACT_WALKING || r.act == ACT_RUNNING))
  {
    r.paceS = (uint16_t)((agg.paceSum + agg.paceN / 2) / agg.paceN);
    r.flags |= RES_HAS_PACE;
  }
  memset(&agg, 0, sizeof(agg));
  resPending = true;
  resRetry = 0;
  statusNow = true; // status ikut dikirim bersama laporan
}

// Satu sampel RUN masuk jendela model, persis nilai yang dikirim ke app (mg, 0,1 dps)
void harAdd(const Packet &p)
{
  ImuSample &s = harBuf[harHead];
  s.a[0] = p.ax / 1000.0f;
  s.a[1] = p.ay / 1000.0f;
  s.a[2] = p.az / 1000.0f;
  s.g[0] = p.gx / 10.0f;
  s.g[1] = p.gy / 10.0f;
  s.g[2] = p.gz / 10.0f;
  harT[harHead] = p.tMs;
  harHead = (harHead + 1) % EBM_WIN_SAMPLES;
  if (harCount < EBM_WIN_SAMPLES)
    harCount++;
  if (++harSince >= EBM_HOP_SAMPLES && harCount >= EBM_WIN_SAMPLES)
  {
    harSince = 0;
    harRun();
  }
}

// Rate yang dipakai: PROD + ada model -> rate data training, selain itu dari ground station
uint8_t effectiveRate()
{
  if (nodeMode == MODE_PROD && (EBM_HAS_ACTIVITY || EBM_HAS_PACE))
    return EBM_RATE_HZ;
  return cmdRateHz;
}

void featMake()
{
  uint16_t n = featWinSamples();
  uint16_t first = (featHead + FEAT_MAX_SAMPLES - n) % FEAT_MAX_SAMPLES;
  uint16_t last = (featHead + FEAT_MAX_SAMPLES - 1) % FEAT_MAX_SAMPLES;
  FeatWin w;
  memset(&w, 0, sizeof(w));
  computeFeatures(featBuf, FEAT_MAX_SAMPLES, first, n, featScratch, w.f);
  w.tStartMs = featT[first];
  w.tEndMs = featT[last];
  w.win = featWinSeq++;
  w.n = (uint8_t)n;
  w.rateHz = rateHz;
  w.version = FEAT_VERSION;
  if (featPart >= 0)
    featDropped++; // jendela sebelumnya belum selesai terkirim -> diganti
  memset(featTx, 0, sizeof(featTx));
  memcpy(featTx, &w, sizeof(w));
  featTxWin = (uint8_t)w.win;
  featPart = 0;
  featRetry = 0;
}

// Satu sampel RUN (raw MPU6050) masuk jendela, dalam satuan g dan dps resolusi penuh
void featAdd(const int16_t raw[7], uint32_t tMs)
{
  ImuSample &s = featBuf[featHead];
  for (int k = 0; k < 3; k++)
  {
    s.a[k] = (raw[k] - accelOff[k]) / ACC_LSB_G;
    s.g[k] = (raw[4 + k] - gyroBias[k]) / GYRO_LSB_DPS;
  }
  featT[featHead] = tMs;
  featHead = (featHead + 1) % FEAT_MAX_SAMPLES;
  if (featCount < FEAT_MAX_SAMPLES)
    featCount++;
  featSince++;
  // PROD: fitur features.h tidak dikirim -> tidak dihitung
  if (nodeMode != MODE_PROD && featCount >= featWinSamples() &&
      featSince >= (uint16_t)rateHz * FEAT_STEP_SEC)
  {
    featSince = 0;
    featMake();
  }
}

// ================== Paket ==================
void buildPacket(Packet &p)
{
  int16_t raw[7];
  imuOk = mpuReadRaw(raw); // tetap dibaca saat STOP supaya status IMU selalu update

  p.magic = PKT_MAGIC;
  p.nodeId = NODE_ID;
  p.seq = 0; // diisi saat benar-benar dikirim
  p.flags = (imuOk ? FLAG_IMU_OK : 0) | (streaming ? FLAG_STREAMING : 0) |
            (cal.active ? FLAG_CALIBRATING : 0);
  p.tMs = millis();

  if (streaming && imuOk)
  {
    p.ax = (int16_t)((raw[0] - accelOff[0]) * 1000 / 8192);
    p.ay = (int16_t)((raw[1] - accelOff[1]) * 1000 / 8192);
    p.az = (int16_t)((raw[2] - accelOff[2]) * 1000 / 8192);
    p.gx = (int16_t)((raw[4] - gyroBias[0]) * 10 / 65.5f);
    p.gy = (int16_t)((raw[5] - gyroBias[1]) * 10 / 65.5f);
    p.gz = (int16_t)((raw[6] - gyroBias[2]) * 10 / 65.5f);
    featAdd(raw, p.tMs);
    harAdd(p);
  }
  else
  {
    if (streaming)
      featReset(); // sampel hilang (IMU error) -> jendela diulang
    p.ax = p.ay = p.az = 0;
    p.gx = p.gy = p.gz = 0;
  }
  p.battMv = battMv;
  p.req = reqByte;
  p.cal = (calStatus & 0x0F) | (accelCalValid ? CAL_ACC_VALID : 0);
  p.calCount = calCount;
}

// Baca perintah yang ikut di ACK dari ground station
void handleAck()
{
  while (radio.available())
  {
    uint8_t len = radio.getDynamicPayloadSize();
    if (len == sizeof(Cmd))
    {
      Cmd c;
      radio.read(&c, sizeof(c));
      if (c.magic != CMD_MAGIC || c.rateHz < 1 || c.rateHz > 100)
        continue;

      if (c.cal && !cal.active)
        startCalibration(CAL_DELAY_APP_MS); // diabaikan selama kalibrasi berjalan

      bool newRun = (c.run != 0);
      uint8_t newMode = c.mode == MODE_PROD ? MODE_PROD : MODE_DEBUG;
      cmdRateHz = c.rateHz;
      uint8_t newBuzzStage = c.buzz > BUZZ_STAGES ? BUZZ_STAGES : c.buzz;
      if (newBuzzStage != buzzStage)
      {
        buzzStage = newBuzzStage;
        Serial.printf("Buzzer sesi web: %s\n", buzzStage ? "ON" : "OFF");
        if (buzzStage)
          Serial.printf("  tingkat %u/%u\n", buzzStage, BUZZ_STAGES);
      }
      if (newMode != nodeMode)
      {
        nodeMode = newMode;
        featPart = -1; // PROD tidak mengirim fitur
        featReset();   // jendela & gabungan laporan PROD mulai dari awal
        Serial.printf("MODE: %s\n", nodeMode == MODE_PROD ? "PRODUCTION" : "DEBUG");
      }
      uint8_t newRate = effectiveRate();
      if (newRun != streaming || newRate != rateHz)
      {
        if (newRun && !streaming)
          beep(1, 150); // mulai RUN
        else if (!newRun && streaming)
          beep(2, 60, 60); // STOP
        streaming = newRun;
        rateHz = newRate;
        featReset();
        periodMs = 1000 / rateHz;
        Serial.printf("PERINTAH: %s @ %u Hz\n", streaming ? "RUN" : "STOP", rateHz);
      }
    }
    else if (len > 0)
    {
      uint8_t junk[32];
      radio.read(junk, len);
    }
  }
}

// Hasil radio.write (paket data maupun fitur)
void onTxResult(bool ok)
{
  if (!ok)
  {
    txFail++;
    return;
  }
  txOk++;
  lastAckMs = millis();
  if (!linkUp)
  {
    linkUp = true;
    beep(1, 40); // link tersambung
    Serial.println("Link ke ground station OK");
  }
  handleAck();
}

// Kirim hasil model jendela terakhir
void resSendNext()
{
  featLastTx = millis();
  bool ok = radio.write(&res, sizeof(res));
  onTxResult(ok);
  if (ok || ++resRetry > FEAT_TX_RETRY)
    resPending = false;
}

// Kirim satu bagian jendela fitur yang sedang antre
void featSendNext()
{
  FeatPacket fp;
  fp.magic = PKT_FEAT_MAGIC;
  fp.nodeId = NODE_ID;
  fp.win = featTxWin;
  fp.part = (uint8_t)featPart;
  memcpy(fp.data, featTx + featPart * FEAT_CHUNK, FEAT_CHUNK);
  featLastTx = millis();
  bool ok = radio.write(&fp, sizeof(fp));
  onTxResult(ok);
  if (ok)
  {
    featRetry = 0;
    if (++featPart >= FEAT_PARTS)
    {
      featPart = -1;
      featSent++;
    }
  }
  else if (++featRetry > FEAT_TX_RETRY)
  {
    featPart = -1;
    featDropped++;
  }
}

void setup()
{
  pinMode(PIN_BUZZER, OUTPUT);
  buzzerOut(false);
  pinMode(PIN_BTN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== NODE %d v5 ===\n", NODE_ID);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  calLoad();
  imuOk = mpuInit();
  if (imuOk)
  {
    Serial.println("MPU6050 OK, kalibrasi gyro... jangan digerakkan");
    calibrateAtBoot();       // buzzer diam selama kalibrasi (getaran)
    beepBlocking(2, 60, 80); // selesai, boleh digerakkan
  }
  else
  {
    Serial.println("MPU6050 TIDAK terdeteksi!");
    beepBlocking(5, 40, 40);
  }

  updateBattery();

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  if (!radio.begin(&SPI))
  {
    Serial.println("nRF24L01 TIDAK terdeteksi!");
    while (1)
    {
      beepBlocking(1, 500);
      delay(1500);
    } // bunyi panjang berulang
  }

  radio.setPALevel(RF_PA_LEVEL);
  if (!radio.setDataRate(RF_DATA_RATE))
  {
    Serial.println("nRF24: data rate ditolak modul (250 kbps hanya nRF24L01+ / Si24R1)!");
    beepBlocking(5, 40, 40);
  }
  radio.setChannel(RF_CHANNEL);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(true);
  radio.enableDynamicPayloads(); // wajib untuk ACK payload
  radio.enableAckPayload();
  radio.setRetries(NODE_ID == 1 ? 3 : 6, 15); // delay retry beda per node

  radio.openWritingPipe(addrNode[NODE_ID - 1]);
  radio.stopListening();

  nextTx = millis() + NODE_ID * 3; // fasa awal beda antar node
  modelSelfTest();
  Serial.println("Status awal: STOP (menunggu perintah ground station)");
}

void loop()
{
  static uint32_t lastBatt = 0;
  static uint32_t lastReport = 0;
  static uint32_t lastLowBeep = 0;
  uint32_t now = millis();

  buzzerUpdate();

  // Buzzer sesi web (mode PROD): berbunyi makin sering sesuai tingkat, berhenti saat buzzStage = 0
  static uint32_t lastBuzz = 0;
  // (tidak digerbang mode/streaming supaya demo alarm dari menu GS juga berbunyi; GS yang menjaga buzz = 0)
  if (buzzStage && linkUp && now - lastBuzz >= BUZZ_STAGE_PERIOD_MS[buzzStage - 1])
  {
    lastBuzz = now;
    beep(BUZZ_STAGE_COUNT[buzzStage - 1], BUZZ_STAGE_ON_MS[buzzStage - 1], BUZZ_STAGE_OFF_MS[buzzStage - 1]);
  }

  BtnEvent e = updateButton(btn);
  if (e == BTN_EV_SHORT)
    requestToggle(false);
  else if (e == BTN_EV_HOLD)
    beep(1, 15); // tik: dilepas sekarang = START/STOP semua, tahan terus = kalibrasi
  else if (e == BTN_EV_LONG)
    requestToggle(true);
  else if (e == BTN_EV_XLONG)
    startCalibration(CAL_DELAY_BUTTON_MS);

  calibrationUpdate(now);

  // IMU lepas -> coba init ulang tiap 1 detik
  if (!imuOk && now - lastImuRetry >= 1000)
  {
    lastImuRetry = now;
    if (mpuInit())
      Serial.println("MPU6050 tersambung lagi");
  }

  if (now - lastBatt >= 1000)
  {
    lastBatt = now;
    updateBattery();
  }

  // Baterai lemah -> bunyi tiap 60 detik (abaikan jika pembacaan tidak masuk akal)
  if (battMv > 2500 && battMv < BATT_LOW_MV && now - lastLowBeep >= 60000)
  {
    lastLowBeep = now;
    beep(2, 200, 150);
  }

  uint16_t period = streaming ? periodMs : HB_PERIOD_MS;
  if ((int32_t)(now - nextTx) >= 0)
  {
    nextTx += period; // jadwal tetap, tanpa drift
    if ((int32_t)(now - nextTx) >= 0)
      nextTx = now + period; // tertinggal -> sinkron ulang

    buildPacket(pkt); // baca IMU (+ jendela fitur/model saat RUN)
    // PROD saat RUN: data mentah tidak dikirim; status hanya bersama laporan 5 s / tombol
    bool prodRun = streaming && nodeMode == MODE_PROD;
    if (!prodRun || statusNow || now - lastStatusTx >= PROD_STATUS_MAX_MS)
    {
      statusNow = false;
      lastStatusTx = now;
      pkt.seq = seq++;
      onTxResult(radio.write(&pkt, sizeof(pkt)));
    }
  }
  // Hasil model & paket fitur di sela paket data (tidak tepat sebelum jadwal berikutnya)
  else if ((resPending || featPart >= 0) && millis() - featLastTx >= FEAT_TX_GAP_MS &&
           (int32_t)(nextTx - millis()) > TX_SLACK_MS)
  {
    if (resPending)
      resSendNext();
    else
      featSendNext();
  }

  // Link putus: tidak ada ACK selama LINK_LOST_MS (PROD saat RUN memancar tiap 5 s)
  uint32_t lostMs = (streaming && nodeMode == MODE_PROD) ? PROD_LINK_LOST_MS : LINK_LOST_MS;
  if (linkUp && millis() - lastAckMs > lostMs)
  {
    linkUp = false;
    beep(3, 60, 80);
    Serial.println("Link ke ground station PUTUS");
  }

  if (now - lastReport >= 1000)
  {
    lastReport = now;
    Serial.printf("%s %s %uHz | ok=%lu fail=%lu | fitur %lu drop %lu | IMU %s | Bat %u mV | link %s\n",
                  nodeMode == MODE_PROD ? "PROD" : "DBG", streaming ? "RUN " : "STOP",
                  streaming ? rateHz : 1000 / HB_PERIOD_MS,
                  (unsigned long)txOk, (unsigned long)txFail,
                  (unsigned long)featSent, (unsigned long)featDropped,
                  imuOk ? "OK" : "ERR", battMv, linkUp ? "OK" : "PUTUS");
    if (streaming && lastPose.valid)
    {
      static const char *const NAMES[ACT_COUNT] = {"Sitting", "Standing", "Walking", "Running"};
      Serial.printf("  pose: accel %+.2f %+.2f %+.2f g | |cos %c| %.2f | model %s -> %s\n",
                    lastPose.a[0], lastPose.a[1], lastPose.a[2], "XYZ"[SIT_AXIS], lastPose.cosSit,
                    lastPose.model < ACT_COUNT ? NAMES[lastPose.model] : "-",
                    lastPose.final < ACT_COUNT ? NAMES[lastPose.final] : "-");
    }
  }
}

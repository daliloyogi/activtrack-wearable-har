// Mode PRODUCTION: sesi dari web lewat Firebase Realtime Database (ESP32_GUIDE.md). Lihat cloud.h.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include "cloud.h"
#include "cloud_ca.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets_example.h"
#endif

#define CLOUD_CORE 0      // loop() di core 1
#define CLOUD_STACK 12288 // handshake TLS butuh stack besar
#define CLOUD_PRIO 1
#define LOOP_MS 250             // task bangun minimal tiap ...
#define WIFI_RETRY_MS 15000     // belum tersambung selama ini -> WiFi.begin ulang
#define HTTP_TIMEOUT_MS 10000
#define TLS_HANDSHAKE_S 15
#define STATE_POLL_MS 3000      // baca /deviceState tiap ... (panduan §13: <= 10 s)
#define STATE_RETRY_MS 5000     // setelah baca gagal
#define RING 1024               // bacaan antre per node (8 KB): > 1 jam pada 5 s (panduan §9)
#define CHUNK 60                // bacaan per PATCH, terlama dulu (panduan §10.3)
#define BODY_MAX 4096           // 60 bacaan x ~55 B
#define BACKOFF_MIN_MS 1000     // kirim gagal: 1, 2, 4 ... s + jitter (panduan §10.4)
#define BACKOFF_MAX_MS 60000
#define JITTER_MS 500
#define DENY_MAX 3              // batch ditolak 401 berturut-turut dengan seri sama -> dibuang (§10.6)
#define FLUSH_DEADLINE_MS 300000 // sisa bacaan setelah stop: dicoba maks 5 menit (§5)
#define REPORT_GRACE_MS 3000    // laporan node (tiap 5 s) boleh terlambat ... untuk mengisi interval
#define EPOCH_VALID 1700000000  // jam sudah tersinkron (panduan §6)
#define TRY_MS 15000            // jaringan WiFi baru tidak tersambung selama ini -> jaringan lama
#define AP_SCAN_TMP 32
static const char *const DEFAULT_DID[CLOUD_NODES] = {"W-01", "W-02"};

// Kode aktivitas node (0 Sitting, 1 Standing, ...) -> label web (huruf kecil, persis)
static const char *const LABEL[CLOUD_ACTS] = {"sitting", "standing", "walking", "running"};
#define ACT_SITTING 0
#define ACT_STANDING 1

// Database: produksi HTTPS; emulator di jaringan lokal untuk uji (panduan §15.2) jika
// FB_EMULATOR_HOST ("ip:9000") & FB_EMULATOR_NS didefinisikan di secrets.h
#ifdef FB_EMULATOR_HOST
#define DB_BASE "http://" FB_EMULATOR_HOST
#define DB_QUERY "?ns=" FB_EMULATOR_NS
#define DB_TLS false
#else
#define DB_BASE "https://" FB_DB_HOST
#define DB_QUERY ""
#define DB_TLS true
#endif

struct Reading // satu bacaan per intervalSec
{
  uint32_t ts; // detik epoch UTC, kelipatan intervalSec (kunci 10 digit)
  uint8_t label;
  uint8_t conf;
  bool buzzer;
};

struct DevState // isi /deviceState/{did}
{
  bool active;
  char ssid[CLOUD_SSID_MAX];
  bool watchSit;
  uint8_t thr;
  uint16_t interval;
  uint16_t uploadEvery;
};

// ---- dibagi loop() <-> task (dijaga mux)
struct Shared
{
  char did[CLOUD_DID_MAX + 1];
  CloudSess sess;
  uint32_t gen; // naik tiap seri baru: antrean dikosongkan
  bool watchSit;
  uint8_t thr;
  uint16_t interval;
  Reading ring[RING];
  uint16_t head, count;
  uint32_t dropped; // bacaan terbuang karena antrean penuh
};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t task = nullptr;
static bool cfgActive = false;
static CloudStatus shSt = {}; // state CLOUD_OFF, job CWJ_IDLE
static Shared sh[CLOUD_NODES];
static CloudAp shAps[CLOUD_AP_MAX];
static uint8_t shApN = 0;
static bool reqScan = false, reqTry = false; // halaman WiFi GS
static char reqSsid[33], reqPass[65];
static bool reqDid[CLOUD_NODES];
static char reqDidVal[CLOUD_NODES][CLOUD_DID_MAX + 1];

// ---- milik loop() (cloudReport / cloudLoop / cloudNode): pembuat bacaan
struct Tick
{
  bool rec;
  uint32_t gen;
  uint32_t nextTs; // ts bacaan berikutnya (0 = belum mulai)
  uint32_t runSec; // lama posisi yang diawasi (panduan §8)
  bool buzzer;
  uint16_t cnt[CLOUD_ACTS]; // laporan node sejak bacaan terakhir
  uint32_t confSum[CLOUD_ACTS];
  uint8_t lastAct, lastConf;
  uint32_t lastMs; // millis() laporan terakhir, 0 = belum ada di seri ini
};
static Tick tk[CLOUD_NODES];

// ---- milik task cloud
struct NodeNet
{
  char did[CLOUD_DID_MAX + 1];
  DevState st;
  bool known; // state pernah terbaca
  bool recording, flushing;
  char series[CLOUD_SSID_MAX]; // seri yang sedang direkam / di-flush
  uint32_t flushDeadline;
  uint32_t nextPollMs;
  bool dirty; // baca state segera (mis. setelah 401)
  uint32_t lastUploadMs, nextTryMs, backoffMs;
  uint8_t deny;
};
static NodeNet nn[CLOUD_NODES];
static bool wasActive = false;
static WiFiClientSecure tls; // dipakai ulang (keep-alive): tanpa handshake TLS tiap request
static WiFiClient plain;     // emulator (http)
static HTTPClient http;
static bool wifiOn = false, wifiUp = false;
static uint32_t wifiBeginMs = 0;
static char curSsid[33] = "", curPass[65] = ""; // jaringan aktif (flash "wifi" / secrets.h)
static char trySsid[33], tryPass[65];
static bool tryActive = false, tryStarted = false;
static uint32_t tryMs = 0;
static volatile uint8_t lastReason = 0; // alasan putus WiFi terakhir (event, wifi_err_reason_t)
static char body[BODY_MAX];
static size_t bodyLen = 0;

// ============================================================== util
static void cloudLog(const char *fmt, ...)
{
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.printf("#CLOUD %s\n", b); // baris '#' diabaikan app
}

static void setState(CloudState s, int16_t code = 0)
{
  portENTER_CRITICAL(&mux);
  shSt.state = s;
  shSt.code = code;
  portEXIT_CRITICAL(&mux);
}

static void setOk()
{
  uint32_t now = millis();
  portENTER_CRITICAL(&mux);
  shSt.state = CLOUD_ONLINE;
  shSt.code = 0;
  shSt.lastOkMs = now ? now : 1;
  portEXIT_CRITICAL(&mux);
}

static void setWifi(bool up, int8_t rssi)
{
  portENTER_CRITICAL(&mux);
  shSt.wifiUp = up;
  shSt.rssi = up ? rssi : 0;
  portEXIT_CRITICAL(&mux);
}

static void setJob(CloudWifiJob j, uint8_t reason = 0)
{
  portENTER_CRITICAL(&mux);
  shSt.job = j;
  shSt.reason = reason;
  portEXIT_CRITICAL(&mux);
}

static bool timeValid()
{
  return time(nullptr) > EPOCH_VALID;
}

static long clampL(long v, long lo, long hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

// Jam dari header "Date: Wed, 01 Oct 2026 06:37:05 GMT" (cadangan jika NTP diblokir jaringan)
static void setClockFromHttpDate(const String &d)
{
  int day, year, hh, mm, ss;
  char mon[4] = "";
  if (sscanf(d.c_str(), "%*[^,], %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6)
    return;
  static const char *const M = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *p = strlen(mon) == 3 ? strstr(M, mon) : nullptr;
  if (!p || (p - M) % 3)
    return;
  int m = (p - M) / 3 + 1;
  // hari sejak 1970-01-01 (days_from_civil, H. Hinnant)
  int y = year - (m <= 2);
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + doe - 719468L;
  struct timeval tv = {(time_t)(days * 86400L + hh * 3600L + mm * 60L + ss), 0};
  settimeofday(&tv, nullptr);
  cloudLog("jam dari header HTTP: %s", d.c_str());
}

// Segmen path URL: karakter selain A-Z a-z 0-9 - _ . ~ di-%-kan (id seri opak)
static String urlSeg(const char *s)
{
  String out;
  char b[4];
  for (; *s; s++)
  {
    char c = *s;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      b[0] = c;
      b[1] = 0;
    }
    else
      snprintf(b, sizeof(b), "%%%02X", (unsigned char)c);
    out += b;
  }
  return out;
}

static void put(const char *fmt, ...)
{
  if (bodyLen >= BODY_MAX)
    return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(body + bodyLen, BODY_MAX - bodyLen, fmt, ap);
  va_end(ap);
  bodyLen = (n < 0 || bodyLen + n >= BODY_MAX) ? BODY_MAX : bodyLen + n; // BODY_MAX = meluap
}

// ============================================================== antrean bacaan
static uint16_t queued(uint8_t i)
{
  portENTER_CRITICAL(&mux);
  uint16_t c = sh[i].count;
  portEXIT_CRITICAL(&mux);
  return c;
}

// Buang n bacaan terlama, hanya jika seri belum berganti (gen sama)
static void dropFront(uint8_t i, uint16_t n, uint32_t gen)
{
  portENTER_CRITICAL(&mux);
  if (sh[i].gen == gen)
  {
    if (n > sh[i].count)
      n = sh[i].count;
    sh[i].head = (sh[i].head + n) % RING;
    sh[i].count -= n;
  }
  portEXIT_CRITICAL(&mux);
}

static void dropAll(uint8_t i)
{
  portENTER_CRITICAL(&mux);
  sh[i].head = sh[i].count = 0;
  sh[i].gen++;
  portEXIT_CRITICAL(&mux);
}

// Sesi node untuk loop() / OLED
static void publish(uint8_t i)
{
  NodeNet &x = nn[i];
  CloudSess s = x.recording ? SESS_RECORDING : x.flushing ? SESS_FLUSHING : x.known ? SESS_IDLE : SESS_UNKNOWN;
  portENTER_CRITICAL(&mux);
  sh[i].sess = s;
  sh[i].watchSit = x.st.watchSit;
  sh[i].thr = x.st.thr;
  sh[i].interval = x.st.interval;
  memcpy(sh[i].did, x.did, sizeof(sh[i].did));
  portEXIT_CRITICAL(&mux);
}

// ============================================================== sesi (panduan §4, §5)
static void startSeries(uint8_t i)
{
  NodeNet &x = nn[i];
  strlcpy(x.series, x.st.ssid, sizeof(x.series));
  x.recording = true;
  x.flushing = false;
  x.lastUploadMs = millis();
  x.nextTryMs = 0;
  x.backoffMs = 0;
  x.deny = 0;
  dropAll(i); // seri baru = antrean kosong
  cloudLog("N%u (%s): MULAI seri %s, aturan %s %u mnt, %u s, kirim %u s", i + 1, x.did, x.series,
           x.st.watchSit ? "sitting" : "standing", x.st.thr, x.st.interval, x.st.uploadEvery);
}

static void stopSeries(uint8_t i)
{
  NodeNet &x = nn[i];
  x.recording = false;
  x.flushing = queued(i) > 0; // kirim sisa ke seri yang SAMA (ssid tetap di state)
  x.flushDeadline = millis() + FLUSH_DEADLINE_MS;
  x.nextTryMs = 0;
  cloudLog("N%u: STOP, sisa %u bacaan dikirim", i + 1, queued(i));
}

// Lupakan sesi node (ID diganti / mode DEBUG); bacaan yang belum terkirim dibuang
static void resetNode(uint8_t i)
{
  NodeNet &x = nn[i];
  uint16_t lost = queued(i);
  if (lost)
    cloudLog("N%u: %u bacaan belum terkirim dibuang", i + 1, lost);
  char did[CLOUD_DID_MAX + 1];
  memcpy(did, x.did, sizeof(did));
  memset(&x, 0, sizeof(x));
  memcpy(x.did, did, sizeof(did));
  x.st.watchSit = true;
  x.dirty = true;
  dropAll(i);
  publish(i);
}

static bool parseState(const String &json, DevState &n)
{
  memset(&n, 0, sizeof(n));
  n.watchSit = true;
  n.thr = 20;
  n.interval = 5;
  n.uploadEvery = 30;
  JsonDocument doc;
  if (deserializeJson(doc, json.c_str()))
    return false;
  if (doc.isNull())
    return true; // belum pernah dipakai: tidak aktif
  n.active = doc["active"] | false;
  strlcpy(n.ssid, doc["ssid"] | "", sizeof(n.ssid));
  const char *rule = doc["buzzerRule"]["rule"] | "sitting";
  n.watchSit = strcmp(rule, "standing") != 0;
  n.thr = (uint8_t)clampL(doc["buzzerRule"]["thresholdMinutes"] | 20, 1, 240);
  n.interval = (uint16_t)clampL(doc["intervalSec"] | 5, 1, 3600);
  n.uploadEvery = (uint16_t)clampL(doc["uploadEverySec"] | 30, 1, 3600);
  if (!n.ssid[0])
    n.active = false; // tanpa seri tidak bisa merekam
  return true;
}

static void applyState(uint8_t i, const DevState &n)
{
  NodeNet &x = nn[i];
  bool reassigned = x.recording && strcmp(n.ssid, x.series) != 0;
  bool wasKnown = x.known;
  x.st = n;
  x.known = true;
  if (n.active && (!x.recording || reassigned))
  {
    if (reassigned) // seri lama tidak bisa ditulis lagi (panduan §4.4)
      cloudLog("N%u: dipindah ke seri lain, %u bacaan seri lama dibuang", i + 1, queued(i));
    startSeries(i);
  }
  else if (!n.active && x.recording)
    stopSeries(i);
  else if (!wasKnown)
    cloudLog("N%u (%s): %s", i + 1, x.did, n.active ? "aktif" : "tidak ada sesi");
  publish(i);
}

// ============================================================== HTTP
// Satu request (keep-alive; koneksi basi -> sambung ulang sekali). out = isi jawaban (opsional).
static int dbRequest(const String &url, const char *patchBody, size_t len, String *out)
{
  int code = 0;
  for (uint8_t attempt = 0; attempt < 2; attempt++)
  {
    bool ok = DB_TLS ? http.begin(tls, url) : http.begin(plain, url);
    if (!ok)
      return HTTPC_ERROR_CONNECTION_REFUSED;
    static const char *hdrs[] = {"Date"};
    http.collectHeaders(hdrs, 1);
    if (patchBody)
    {
      http.addHeader("Content-Type", "application/json");
      code = http.PATCH((uint8_t *)patchBody, len);
    }
    else
      code = http.GET();
    if (code > 0)
    {
      String date = http.header("Date");
      String resp = (out || (code != 200 && code != 204)) ? http.getString() : String();
      http.end();
      if (out)
        *out = resp;
      else if (code != 200 && code != 204)
        cloudLog("HTTP %d: %.100s", code, resp.c_str());
      if (!timeValid() && date.length())
        setClockFromHttpDate(date);
      return code;
    }
    http.end();
    DB_TLS ? tls.stop() : plain.stop();
    cloudLog("request gagal: %d %s%s", code, HTTPClient::errorToString(code).c_str(),
             attempt ? "" : ", sambung ulang");
  }
  return code;
}

// GET /deviceState/{did}
static void pollState(uint8_t i)
{
  NodeNet &x = nn[i];
  String url = String(DB_BASE) + "/deviceState/" + urlSeg(x.did) + ".json" + DB_QUERY;
  String resp;
  int code = dbRequest(url, nullptr, 0, &resp);
  x.dirty = false;
  if (code == 200)
  {
    DevState n;
    if (parseState(resp, n))
    {
      applyState(i, n);
      setOk();
    }
    else
      cloudLog("N%u: state tidak bisa dibaca: %.80s", i + 1, resp.c_str());
    x.nextPollMs = millis() + STATE_POLL_MS;
    return;
  }
  setState(CLOUD_ERR, (int16_t)code);
  x.nextPollMs = millis() + STATE_RETRY_MS;
}

static bool uploadDue(uint8_t i, uint32_t now)
{
  NodeNet &x = nn[i];
  uint16_t c = queued(i);
  if (!c)
  {
    if (x.flushing)
    { // sisa terkirim semua -> IDLE
      x.flushing = false;
      cloudLog("N%u: flush selesai", i + 1);
      publish(i);
    }
    return false;
  }
  if (x.flushing && (int32_t)(now - x.flushDeadline) >= 0)
  {
    cloudLog("N%u: flush melewati batas waktu, %u bacaan dibuang", i + 1, c);
    x.flushing = false;
    dropAll(i);
    publish(i);
    return false;
  }
  if (!x.recording && !x.flushing)
    return false;
  if ((int32_t)(now - x.nextTryMs) < 0)
    return false;
  return x.flushing || c >= CHUNK || now - x.lastUploadMs >= x.st.uploadEvery * 1000UL;
}

// PATCH /deviceHistory/{ssid}.json: <= CHUNK bacaan terlama. true = sukses
static bool upload(uint8_t i)
{
  NodeNet &x = nn[i];
  Reading buf[CHUNK];
  uint16_t n;
  uint32_t gen;
  portENTER_CRITICAL(&mux);
  n = sh[i].count < CHUNK ? sh[i].count : CHUNK;
  for (uint16_t k = 0; k < n; k++)
    buf[k] = sh[i].ring[(sh[i].head + k) % RING];
  gen = sh[i].gen;
  portEXIT_CRITICAL(&mux);
  if (!n)
    return true;

  bodyLen = 0;
  put("{");
  for (uint16_t k = 0; k < n; k++)
    put("%s\"%lu\":{\"label\":\"%s\",\"conf\":%u,\"buzzer\":%s}", k ? "," : "", (unsigned long)buf[k].ts,
        LABEL[buf[k].label % CLOUD_ACTS], buf[k].conf, buf[k].buzzer ? "true" : "false");
  put("}");
  if (bodyLen >= BODY_MAX)
  {
    cloudLog("body meluap");
    return false;
  }
  String url = String(DB_BASE) + "/deviceHistory/" + urlSeg(x.series) + ".json" + DB_QUERY +
               (DB_QUERY[0] ? "&" : "?") + "print=silent";
  int code = dbRequest(url, body, bodyLen, nullptr);
  uint32_t now = millis();
  if (code == 200 || code == 204)
  {
    dropFront(i, n, gen); // hanya yang terkirim
    x.lastUploadMs = now;
    x.backoffMs = 0;
    x.deny = 0;
    x.nextTryMs = now;
    setOk();
    cloudLog("N%u: kirim %u bacaan OK, antre %u", i + 1, n, queued(i));
    return true;
  }
  if (code == 401)
  { // ditolak Rules: seri diganti (baca ulang state) atau data salah (§10.6)
    x.dirty = true;
    if (++x.deny >= DENY_MAX)
    {
      cloudLog("N%u: batch ditolak %u kali, %u bacaan dibuang", i + 1, DENY_MAX, n);
      dropFront(i, n, gen);
      x.deny = 0;
    }
    x.nextTryMs = now + BACKOFF_MIN_MS;
  }
  else if (code == 400)
  { // JSON / kunci tidak valid: bug firmware, mengulang tidak akan berhasil
    cloudLog("N%u: batch ditolak 400, dibuang: %.80s", i + 1, body);
    dropFront(i, n, gen);
  }
  else
  { // jaringan / server: simpan, coba lagi dengan backoff + jitter
    x.backoffMs = x.backoffMs ? min<uint32_t>(x.backoffMs * 2, BACKOFF_MAX_MS) : BACKOFF_MIN_MS;
    x.nextTryMs = now + x.backoffMs + esp_random() % JITTER_MS;
  }
  setState(CLOUD_ERR, (int16_t)code);
  return false;
}

// ============================================================== WiFi
static void wifiStop()
{
  if (!wifiOn)
    return;
  http.end();
  tls.stop();
  plain.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiOn = wifiUp = false;
  setWifi(false, 0);
  cloudLog("WiFi mati");
}

// Nyalakan WiFi mode STA tanpa menyambung
static void wifiStaOn(uint32_t now)
{
  if (wifiOn)
    return;
  WiFi.persistent(false); // jangan tulis kredensial ke flash tiap begin
  WiFi.setHostname("HAR-GS");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiOn = true;
  wifiUp = false;
  wifiBeginMs = now;
}

// true = tersambung. Menyambung / mencoba ulang tanpa memblokir.
static bool wifiReady(uint32_t now)
{
  if (!curSsid[0])
  {
    setState(CLOUD_NO_WIFI);
    return false; // belum ada jaringan (pilih di halaman WiFi GS / secrets.h)
  }
  if (!wifiOn)
  {
    wifiStaOn(now);
    WiFi.begin(curSsid, curPass);
    setState(CLOUD_WIFI);
    cloudLog("WiFi menyambung ke \"%s\"", curSsid);
    return false;
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    if (!wifiUp)
    {
      wifiUp = true;
      cloudLog("WiFi OK, IP %s, RSSI %d dBm, kanal %d", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
               WiFi.channel());
      configTzTime(CLOUD_TZ, "pool.ntp.org", "time.google.com");
      setState(CLOUD_WIFI); // ONLINE setelah jawaban database pertama
      for (uint8_t i = 0; i < CLOUD_NODES; i++)
        nn[i].dirty = true; // mungkin ada START/STOP yang terlewat saat putus (§4.3)
    }
    setWifi(true, WiFi.RSSI());
    return true;
  }
  setWifi(false, 0);
  if (wifiUp)
  {
    wifiUp = false;
    wifiBeginMs = now;
    tls.stop();
    plain.stop();
    setState(CLOUD_WIFI);
    cloudLog("WiFi putus");
  }
  if (now - wifiBeginMs >= WIFI_RETRY_MS)
  {
    // status: 1 = SSID tidak ditemukan, 4 = gagal (sandi salah?), 6 = terputus
    cloudLog("WiFi belum tersambung (status %d), coba lagi", (int)WiFi.status());
    wifiBeginMs = now;
    WiFi.disconnect();
    WiFi.begin(curSsid, curPass);
    setState(CLOUD_NO_WIFI);
  }
  return false;
}

// Jaringan pilihan dari halaman WiFi GS; belum pernah dipilih -> secrets.h
static void loadWifi()
{
  Preferences p;
  p.begin("wifi", false);
  String ss = p.getString("ssid", ""), pw = p.getString("pass", "");
  p.end();
  bool saved = ss.length() > 0;
  strlcpy(curSsid, saved ? ss.c_str() : WIFI_SSID, sizeof(curSsid));
  strlcpy(curPass, saved ? pw.c_str() : WIFI_PASS, sizeof(curPass));
  portENTER_CRITICAL(&mux);
  memcpy(shSt.ssid, curSsid, sizeof(shSt.ssid));
  portEXIT_CRITICAL(&mux);
  cloudLog("WiFi \"%s\" (%s)", curSsid, saved ? "pilihan GS" : "secrets.h");
}

static void saveWifi()
{
  Preferences p;
  p.begin("wifi", false);
  p.putString("ssid", curSsid);
  p.putString("pass", curPass);
  p.end();
  portENTER_CRITICAL(&mux);
  memcpy(shSt.ssid, curSsid, sizeof(shSt.ssid));
  portEXIT_CRITICAL(&mux);
}

// Device ID tiap node (flash "cloud" did1/did2), default W-01 / W-02
static void loadDids()
{
  Preferences p;
  p.begin("cloud", false);
  for (uint8_t i = 0; i < CLOUD_NODES; i++)
  {
    char key[8];
    snprintf(key, sizeof(key), "did%u", i + 1);
    String v = p.getString(key, "");
    strlcpy(nn[i].did, cloudValidDeviceId(v.c_str()) ? v.c_str() : DEFAULT_DID[i], sizeof(nn[i].did));
    nn[i].st.watchSit = true;
    nn[i].dirty = true;
    publish(i);
    cloudLog("N%u = device \"%s\"", i + 1, nn[i].did);
  }
  p.end();
}

static void saveDid(uint8_t i)
{
  Preferences p;
  p.begin("cloud", false);
  char key[8];
  snprintf(key, sizeof(key), "did%u", i + 1);
  p.putString(key, nn[i].did);
  p.end();
}

// Setelah scan / coba jaringan: kembali ke keadaan normal
static void afterJob(bool active, bool connected)
{
  if (!active)
    wifiStop(); // mode DEBUG: WiFi dimatikan lagi
  else if (!connected)
  { // sambung ulang jaringan aktif dari awal pada putaran berikutnya
    WiFi.disconnect();
    wifiOn = wifiUp = false;
    setWifi(false, 0);
  }
}

// Scan jaringan (memblokir task ~2-4 s). SSID sama digabung (sinyal terkuat), urut terkuat dulu.
static void doScan(uint32_t now, bool active)
{
  bool connected = wifiOn && WiFi.status() == WL_CONNECTED;
  wifiStaOn(now);
  if (!connected)
    WiFi.disconnect(); // ESP32 tidak bisa scan saat sedang mencoba menyambung
  int n = WiFi.scanNetworks(false, false);
  CloudAp tmp[AP_SCAN_TMP];
  uint8_t k = 0;
  for (int i = 0; i < n; i++)
  {
    String ss = WiFi.SSID(i);
    if (!ss.length() || ss.length() > 32)
      continue; // jaringan tersembunyi
    int8_t r = WiFi.RSSI(i);
    bool sec = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    uint8_t j = 0;
    while (j < k && strcmp(tmp[j].ssid, ss.c_str()))
      j++;
    if (j < k)
    { // beberapa AP satu SSID
      if (r > tmp[j].rssi)
      {
        tmp[j].rssi = r;
        tmp[j].secure = sec;
      }
      continue;
    }
    if (k == AP_SCAN_TMP)
      continue;
    strlcpy(tmp[k].ssid, ss.c_str(), sizeof(tmp[k].ssid));
    tmp[k].rssi = r;
    tmp[k].secure = sec;
    k++;
  }
  WiFi.scanDelete();
  for (uint8_t a = 1; a < k; a++)
    for (uint8_t b = a; b > 0 && tmp[b].rssi > tmp[b - 1].rssi; b--)
    {
      CloudAp t = tmp[b];
      tmp[b] = tmp[b - 1];
      tmp[b - 1] = t;
    }
  if (k > CLOUD_AP_MAX)
    k = CLOUD_AP_MAX;
  portENTER_CRITICAL(&mux);
  memcpy(shAps, tmp, sizeof(CloudAp) * k);
  shApN = k;
  shSt.job = n >= 0 ? CWJ_SCAN_DONE : CWJ_SCAN_FAIL;
  portEXIT_CRITICAL(&mux);
  cloudLog("scan WiFi: %d AP, %u jaringan", n, k);
  afterJob(active, connected);
}

// Coba jaringan baru (beberapa putaran, tidak memblokir). Berhasil -> disimpan & dipakai;
// tidak tersambung dalam TRY_MS -> kembali ke jaringan lama.
static void tryStep(uint32_t now, bool active)
{
  if (!tryStarted)
  {
    http.end();
    tls.stop();
    plain.stop();
    bool was = wifiOn;
    wifiStaOn(now);
    if (was)
      WiFi.disconnect();
    wifiUp = false;
    setWifi(false, 0);
    lastReason = 0;
    WiFi.begin(trySsid, tryPass);
    tryStarted = true;
    tryMs = now;
    cloudLog("coba WiFi \"%s\"", trySsid);
    return;
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  if (!ok && now - tryMs < TRY_MS)
    return;
  tryActive = tryStarted = false;
  if (ok)
  {
    strlcpy(curSsid, trySsid, sizeof(curSsid));
    strlcpy(curPass, tryPass, sizeof(curPass));
    saveWifi();
    setJob(CWJ_CONNECT_OK);
    cloudLog("WiFi diganti ke \"%s\" (tersimpan)", curSsid);
  }
  else
  {
    setJob(CWJ_CONNECT_FAIL, lastReason);
    cloudLog("gagal sambung \"%s\" (alasan %u), kembali ke \"%s\"", trySsid, lastReason, curSsid);
  }
  afterJob(active, ok);
}

// ============================================================== task
// Satu putaran task. true = masih ada antrean yang harus segera dikirim (tanpa menunggu LOOP_MS).
static bool cloudStep()
{
  uint32_t now = millis();
  bool active, rs, rt, rd[CLOUD_NODES];
  char rdv[CLOUD_NODES][CLOUD_DID_MAX + 1];
  portENTER_CRITICAL(&mux);
  active = cfgActive;
  rs = reqScan;
  rt = reqTry;
  if (rt)
  {
    memcpy(trySsid, reqSsid, sizeof(trySsid));
    memcpy(tryPass, reqPass, sizeof(tryPass));
    reqTry = false;
  }
  if (rs && !rt && !tryActive)
    reqScan = false;
  for (uint8_t i = 0; i < CLOUD_NODES; i++)
  {
    rd[i] = reqDid[i];
    reqDid[i] = false;
    memcpy(rdv[i], reqDidVal[i], sizeof(rdv[i]));
  }
  portEXIT_CRITICAL(&mux);

  for (uint8_t i = 0; i < CLOUD_NODES; i++)
    if (rd[i] && strcmp(rdv[i], nn[i].did))
    { // device ID diganti dari OLED: sesi node itu mulai dari awal
      memcpy(nn[i].did, rdv[i], sizeof(nn[i].did));
      saveDid(i);
      cloudLog("N%u = device \"%s\" (diganti)", i + 1, nn[i].did);
      resetNode(i);
    }

  // halaman WiFi GS: coba jaringan baru / scan (juga di mode DEBUG)
  if (rt)
  {
    tryActive = true;
    tryStarted = false;
  }
  if (tryActive)
  {
    tryStep(now, active);
    return false;
  }
  if (rs)
  {
    doScan(now, active);
    return false;
  }

  if (!active)
  {
    if (wasActive)
    { // keluar dari PROD: sesi dilupakan (dibaca ulang saat PROD lagi)
      for (uint8_t i = 0; i < CLOUD_NODES; i++)
        resetNode(i);
      wasActive = false;
    }
    wifiStop();
    setState(CLOUD_OFF);
    return false;
  }
  wasActive = true;
  if (!wifiReady(now))
    return false;

  for (uint8_t i = 0; i < CLOUD_NODES; i++)
    if (nn[i].did[0] && (nn[i].dirty || (int32_t)(now - nn[i].nextPollMs) >= 0))
      pollState(i);

  bool more = false;
  for (uint8_t i = 0; i < CLOUD_NODES; i++)
    if (uploadDue(i, millis()) && upload(i))
      more |= queued(i) >= CHUNK || (nn[i].flushing && queued(i)); // susulan: chunk berikutnya segera
  return more;
}

static void cloudInit()
{
  setenv("TZ", CLOUD_TZ, 1);
  tzset();
  loadWifi();
  loadDids();
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        uint8_t r = info.wifi_sta_disconnected.reason;
        if (r != WIFI_REASON_ASSOC_LEAVE) // ASSOC_LEAVE = WiFi.disconnect() sendiri
          lastReason = r;
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  tls.setCACert(CLOUD_ROOT_CA);
  tls.setHandshakeTimeout(TLS_HANDSHAKE_S);
  http.setReuse(true);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
}

static void cloudTask(void *)
{
  cloudInit();
  bool more = false;
  for (;;)
  {
    if (!more) // antrean panjang: chunk berikutnya langsung dikirim
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LOOP_MS));
    more = cloudStep();
  }
}

// ============================================================== bacaan (loop(), core 1)
// Label & confidence satu interval dari laporan node di interval itu (terbanyak; seri -> confidence
// lebih besar). Tidak ada laporan -> laporan terakhir bila belum terlalu lama, selain itu celah.
static bool makeReading(Tick &t, uint16_t iv, uint32_t nowMs, uint8_t &label, uint8_t &conf)
{
  uint8_t best = 0;
  for (uint8_t a = 1; a < CLOUD_ACTS; a++)
    if (t.cnt[a] > t.cnt[best] || (t.cnt[a] == t.cnt[best] && t.confSum[a] > t.confSum[best]))
      best = a;
  if (t.cnt[best])
  {
    label = best;
    conf = (uint8_t)((t.confSum[best] + t.cnt[best] / 2) / t.cnt[best]);
    return true;
  }
  if (t.lastMs && nowMs - t.lastMs <= iv * 1000UL + REPORT_GRACE_MS)
  {
    label = t.lastAct;
    conf = t.lastConf;
    return true;
  }
  return false; // node tidak melapor (mis. di luar jangkauan): celah = offline di web
}

static void clearCounts(Tick &t)
{
  memset(t.cnt, 0, sizeof(t.cnt));
  memset(t.confSum, 0, sizeof(t.confSum));
}

void cloudLoop()
{
  time_t nowS = time(nullptr);
  uint32_t nowMs = millis();
  for (uint8_t i = 0; i < CLOUD_NODES; i++)
  {
    Tick &t = tk[i];
    CloudSess sess;
    uint32_t gen;
    bool watchSit;
    uint8_t thr;
    uint16_t iv;
    portENTER_CRITICAL(&mux);
    sess = sh[i].sess;
    gen = sh[i].gen;
    watchSit = sh[i].watchSit;
    thr = sh[i].thr;
    iv = sh[i].interval ? sh[i].interval : 5;
    portEXIT_CRITICAL(&mux);

    if (sess != SESS_RECORDING)
    { // IDLE / FLUSHING: tidak merekam, buzzer mati, hitungan direset (§5)
      t.rec = false;
      t.runSec = 0;
      t.buzzer = false;
      continue;
    }
    if (!t.rec || gen != t.gen)
    { // seri baru
      memset(&t, 0, sizeof(t));
      t.rec = true;
      t.gen = gen;
    }
    if (nowS < EPOCH_VALID)
      continue; // tanpa jam tidak merekam (§6)
    uint32_t now = (uint32_t)nowS;
    if (!t.nextTs)
    {
      t.nextTs = (now / iv + 1) * iv;
      continue;
    }
    if (now < t.nextTs)
      continue;
    if (now - t.nextTs >= 2u * iv)
    { // jam meloncat (sinkron NTP): mulai lagi dari interval berikutnya
      t.nextTs = (now / iv + 1) * iv;
      clearCounts(t);
      continue;
    }
    Reading r;
    if (makeReading(t, iv, nowMs, r.label, r.conf))
    { // aturan buzzer (§8): posisi diawasi bertambah, posisi lain reset, ON jika >= ambang
      uint8_t watched = watchSit ? ACT_SITTING : ACT_STANDING;
      t.runSec = r.label == watched ? t.runSec + iv : 0;
      t.buzzer = t.runSec >= thr * 60UL;
      r.ts = t.nextTs;
      r.buzzer = t.buzzer;
      portENTER_CRITICAL(&mux);
      Shared &s = sh[i];
      if (s.sess == SESS_RECORDING && s.gen == gen)
      {
        if (s.count == RING)
        { // antrean penuh (putus sangat lama): buang yang terlama
          s.head = (s.head + 1) % RING;
          s.count--;
          s.dropped++;
        }
        s.ring[(s.head + s.count) % RING] = r;
        s.count++;
      }
      portEXIT_CRITICAL(&mux);
    }
    clearCounts(t);
    t.nextTs += iv;
  }
}

// ============================================================== API
void cloudBegin()
{
  if (!task)
    xTaskCreatePinnedToCore(cloudTask, "cloud", CLOUD_STACK, nullptr, CLOUD_PRIO, &task, CLOUD_CORE);
}

void cloudConfig(bool active)
{
  portENTER_CRITICAL(&mux);
  cfgActive = active;
  portEXIT_CRITICAL(&mux);
  if (task)
    xTaskNotifyGive(task);
}

void cloudReport(uint8_t idx, uint8_t act, uint8_t confPct)
{
  if (idx >= CLOUD_NODES || act >= CLOUD_ACTS)
    return;
  Tick &t = tk[idx];
  t.cnt[act]++;
  t.confSum[act] += confPct > 100 ? 100 : confPct;
  t.lastAct = act;
  t.lastConf = confPct > 100 ? 100 : confPct;
  t.lastMs = millis();
  if (!t.lastMs)
    t.lastMs = 1;
}

CloudStatus cloudStatus()
{
  portENTER_CRITICAL(&mux);
  CloudStatus s = shSt;
  portEXIT_CRITICAL(&mux);
  return s;
}

CloudNode cloudNode(uint8_t idx)
{
  CloudNode n = {};
  if (idx >= CLOUD_NODES)
    return n;
  portENTER_CRITICAL(&mux);
  memcpy(n.did, sh[idx].did, sizeof(n.did));
  n.sess = sh[idx].sess;
  n.watchSitting = sh[idx].watchSit;
  n.thresholdMin = sh[idx].thr;
  n.intervalSec = sh[idx].interval;
  n.queued = sh[idx].count;
  n.gen = sh[idx].gen;
  portEXIT_CRITICAL(&mux);
  const Tick &t = tk[idx];
  bool rec = n.sess == SESS_RECORDING && t.rec;
  n.buzzer = rec && t.buzzer;
  n.runMin = rec ? t.runSec / 60 : 0;
  n.runSec = rec ? t.runSec : 0;
  return n;
}

bool cloudValidDeviceId(const char *did)
{
  size_t n = did ? strlen(did) : 0;
  if (n < 1 || n > CLOUD_DID_MAX)
    return false;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)did[i]) && did[i] != '-' && did[i] != '_')
      return false;
  return true;
}

bool cloudSetDeviceId(uint8_t idx, const char *did)
{
  if (idx >= CLOUD_NODES || !cloudValidDeviceId(did))
    return false;
  portENTER_CRITICAL(&mux);
  strlcpy(reqDidVal[idx], did, sizeof(reqDidVal[idx]));
  reqDid[idx] = true;
  strlcpy(sh[idx].did, did, sizeof(sh[idx].did)); // OLED langsung menampilkan ID baru
  portEXIT_CRITICAL(&mux);
  if (task)
    xTaskNotifyGive(task);
  return true;
}

void cloudScan()
{
  portENTER_CRITICAL(&mux);
  reqScan = true;
  shApN = 0;
  shSt.job = CWJ_SCANNING;
  portEXIT_CRITICAL(&mux);
  if (task)
    xTaskNotifyGive(task);
}

uint8_t cloudScanResults(CloudAp *out, uint8_t max)
{
  portENTER_CRITICAL(&mux);
  uint8_t n = shApN < max ? shApN : max;
  memcpy(out, shAps, sizeof(CloudAp) * n);
  portEXIT_CRITICAL(&mux);
  return n;
}

void cloudConnect(const char *ssid, const char *pass)
{
  portENTER_CRITICAL(&mux);
  strlcpy(reqSsid, ssid, sizeof(reqSsid));
  strlcpy(reqPass, pass ? pass : "", sizeof(reqPass));
  reqTry = true;
  shSt.job = CWJ_CONNECTING;
  shSt.reason = 0;
  portEXIT_CRITICAL(&mux);
  if (task)
    xTaskNotifyGive(task);
}

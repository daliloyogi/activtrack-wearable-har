// Tampilan OLED ground station "HAR - Human Activity Tracker". Lihat ui.h.
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef PI
#define PI 3.14159265358979f
#endif

static const char *const ACT_NAME[4] = {"Duduk", "Berdiri", "Jalan", "Lari"}; // kode 0..3
enum : uint8_t
{
  POSE_SIT = 0,
  POSE_STAND = 1,
  POSE_WALK = 2,
  POSE_RUN = 3
};

// ============================================================== teks
static int16_t textW(const char *s, uint8_t size = 1)
{
  int n = (int)strlen(s);
  return n ? (int16_t)(n * 6 * size - size) : 0;
}

static void text(Adafruit_GFX &d, int16_t x, int16_t y, const char *s, uint8_t size = 1, uint16_t c = 1)
{
  d.setTextSize(size);
  d.setTextColor(c);
  d.setCursor(x, y);
  d.print(s);
}

static void textC(Adafruit_GFX &d, int16_t cx, int16_t y, const char *s, uint8_t size = 1, uint16_t c = 1)
{
  text(d, cx - textW(s, size) / 2, y, s, size, c);
}

static void textR(Adafruit_GFX &d, int16_t xr, int16_t y, const char *s, uint8_t size = 1, uint16_t c = 1)
{
  text(d, xr - textW(s, size) + 1, y, s, size, c);
}

static void clear(Adafruit_GFX &d)
{
  d.fillScreen(0);
  d.cp437(true); // karakter panah / centang CP437
  d.setTextWrap(false);
}

// ============================================================== ikon
static int battPct(uint16_t mv)
{
  return mv < 3300 ? 0 : mv >= 4100 ? 100 : (mv - 3300) / 8;
}

static void iconBattery(Adafruit_GFX &d, int16_t x, int16_t y, int pct, uint16_t c) // 13x7
{
  d.drawRect(x, y, 11, 7, c);
  d.fillRect(x + 11, y + 2, 2, 3, c);
  pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
  int w = (pct * 7 + 50) / 100;
  if (pct > 0 && w == 0)
    w = 1;
  if (w)
    d.fillRect(x + 2, y + 2, w, 3, c);
}

static uint8_t signalLevel(const UiNode &n)
{
  if (n.state == UI_NODE_NONE || n.state == UI_NODE_OFF)
    return 0;
  return n.lossPct <= 2 ? 4 : n.lossPct <= 5 ? 3 : n.lossPct <= 15 ? 2 : 1;
}

static void iconSignal(Adafruit_GFX &d, int16_t x, int16_t y, uint8_t level, uint16_t c) // 11x7
{
  static const uint8_t H[4] = {2, 3, 5, 7};
  for (uint8_t i = 0; i < 4; i++)
  {
    int16_t bx = x + i * 3;
    if (i < level)
      d.fillRect(bx, y + 7 - H[i], 2, H[i], c);
    else
      d.drawFastHLine(bx, y + 6, 2, c);
  }
}

static void iconMute(Adafruit_GFX &d, int16_t x, int16_t y, uint16_t c) // 10x7
{
  d.fillRect(x, y + 2, 2, 3, c);
  d.fillTriangle(x + 2, y + 3, x + 5, y, x + 5, y + 6, c);
  d.drawLine(x + 7, y + 1, x + 9, y + 5, c);
  d.drawLine(x + 9, y + 1, x + 7, y + 5, c);
}

static void iconLock(Adafruit_GFX &d, int16_t x, int16_t y, uint16_t c) // 5x7
{
  d.drawFastVLine(x + 1, y + 1, 2, c);
  d.drawFastVLine(x + 3, y + 1, 2, c);
  d.drawPixel(x + 2, y, c);
  d.fillRect(x, y + 3, 5, 4, c);
}

static void iconWarn(Adafruit_GFX &d, int16_t x, int16_t y, uint16_t c) // 15x13
{
  d.drawTriangle(x + 7, y, x, y + 12, x + 14, y + 12, c);
  d.drawFastVLine(x + 7, y + 4, 5, c);
  d.drawPixel(x + 7, y + 10, c);
}

// Awan 12x7: penuh = online, garis = sedang menyambung (berkedip), garis + coret = offline
static void iconCloud(Adafruit_GFX &d, int16_t x, int16_t y, uint8_t st, uint32_t now, uint16_t c)
{
  bool busy = st == UI_CLOUD_WIFI;
  if (busy && (now / 500) % 2)
    return;
  uint16_t bg = c ? 0 : 1;
  d.fillCircle(x + 3, y + 4, 2, c);
  d.fillCircle(x + 6, y + 3, 3, c);
  d.fillCircle(x + 9, y + 4, 2, c);
  d.fillRect(x + 3, y + 4, 7, 3, c);
  if (st == UI_CLOUD_ONLINE)
    return;
  d.fillCircle(x + 3, y + 4, 1, bg);
  d.fillCircle(x + 6, y + 3, 2, bg);
  d.fillCircle(x + 9, y + 4, 1, bg);
  d.fillRect(x + 3, y + 4, 7, 2, bg);
  if (!busy)
    d.drawLine(x + 2, y + 6, x + 10, y, c);
}

static void spinner(Adafruit_GFX &d, int16_t cx, int16_t y, uint32_t now) // 3 titik berjalan
{
  uint8_t on = (now / 250) % 3;
  for (uint8_t i = 0; i < 3; i++)
  {
    if (i == on)
      d.fillCircle(cx - 6 + i * 6, y, 2, 1);
    else
      d.drawPixel(cx - 6 + i * 6, y, 1);
  }
}

// ============================================================== figur orang
static void seg(Adafruit_GFX &d, float x0, float y0, float x1, float y1, bool bold, uint16_t c)
{
  int16_t a = (int16_t)lroundf(x0), b = (int16_t)lroundf(y0);
  int16_t e = (int16_t)lroundf(x1), f = (int16_t)lroundf(y1);
  d.drawLine(a, b, e, f, c);
  if (bold)
    d.drawLine(a + 1, b, e + 1, f, c);
}

// Orang sederhana menghadap kanan. hx = x pinggul, ground = y lantai, h = tinggi (px),
// ph = fase langkah 0..1. Sudut dalam derajat dari arah tegak ke bawah, positif = ke depan.
static void figure(Adafruit_GFX &d, int16_t hx, int16_t ground, int16_t h, uint8_t pose, float ph, uint16_t c)
{
  const float D = PI / 180.0f;
  float u = h / 24.0f;
  float head = 2.8f * u, torso = 7.5f * u, thigh = 5.6f * u, shin = 5.6f * u, uarm = 4.2f * u, farm = 3.8f * u;
  float lean = 0, th[2] = {5, -5}, kn[2] = {0, 0}, ua[2] = {18, -18}, el[2] = {4, -4};
  float sw = sinf(ph * 2 * PI);
  if (pose == POSE_WALK)
  {
    lean = 4;
    th[0] = 24 * sw;
    th[1] = -24 * sw;
    kn[0] = 8 + (sw < 0 ? -28 * sw : 0);
    kn[1] = 8 + (sw > 0 ? 28 * sw : 0);
    ua[0] = -26 * sw;
    ua[1] = 26 * sw;
    el[0] = el[1] = 25;
  }
  else if (pose == POSE_RUN)
  {
    lean = 16;
    th[0] = 48 * sw;
    th[1] = -48 * sw;
    kn[0] = 30 + (sw < 0 ? -70 * sw : 5);
    kn[1] = 30 + (sw > 0 ? 70 * sw : 5);
    ua[0] = -50 * sw;
    ua[1] = 50 * sw;
    el[0] = el[1] = 95;
  }
  else if (pose == POSE_SIT)
  {
    th[0] = th[1] = 90;
    kn[0] = 90;
    kn[1] = 90 - 14 * sw; // kaki mengayun pelan
    ua[0] = ua[1] = 20;
    el[0] = el[1] = 50;
  }
  else // POSE_STAND: lengan berayun sedikit
  {
    ua[0] = 18 + 6 * sw;
    ua[1] = -18 - 6 * sw;
  }
  bool bold = h >= 18;

  float drop = 0; // tinggi pinggul dari lantai = kaki terendah
  for (int i = 0; i < 2; i++)
  {
    float y = thigh * cosf(th[i] * D) + shin * cosf((th[i] - kn[i]) * D);
    if (y > drop)
      drop = y;
  }
  float hy = ground - drop;

  if (pose == POSE_SIT) // kursi
  {
    float sy = hy + 2 * u + 1, bx = hx - 3.5f * u, fx = hx + thigh * 0.9f;
    seg(d, bx, sy, fx, sy, false, c);
    seg(d, bx, sy, bx - 1 * u, hy - torso * 0.9f, false, c);
    seg(d, bx, sy, bx, ground, false, c);
    seg(d, fx, sy, fx, ground, false, c);
  }

  for (int i = 0; i < 2; i++) // kaki
  {
    float kx = hx + thigh * sinf(th[i] * D), ky = hy + thigh * cosf(th[i] * D);
    float a = (th[i] - kn[i]) * D;
    seg(d, hx, hy, kx, ky, bold, c);
    seg(d, kx, ky, kx + shin * sinf(a), ky + shin * cosf(a), bold, c);
  }
  float nx = hx + torso * sinf(lean * D), ny = hy - torso * cosf(lean * D);
  seg(d, hx, hy, nx, ny, bold, c);
  for (int i = 0; i < 2; i++) // lengan
  {
    float ex = nx + uarm * sinf(ua[i] * D), ey = ny + uarm * cosf(ua[i] * D);
    float a = (ua[i] + el[i]) * D;
    seg(d, nx, ny, ex, ey, false, c);
    seg(d, ex, ey, ex + farm * sinf(a), ey + farm * cosf(a), false, c);
  }
  float hcx = nx + (head + 0.8f) * sinf(lean * D), hcy = ny - (head + 0.8f) * cosf(lean * D);
  d.fillCircle((int16_t)lroundf(hcx), (int16_t)lroundf(hcy), (int16_t)lroundf(head), c);
}

// Figur untuk kode aktivitas, bergerak untuk jalan / lari. cx = tengah, ground = y lantai.
static void activityFigure(Adafruit_GFX &d, int16_t cx, int16_t ground, int16_t h, uint8_t act,
                           uint32_t now, uint16_t c)
{
  float u = h / 24.0f;
  float ph = act == POSE_RUN    ? (now % 800) / 800.0f // 8 pose per siklus pada OLED 10 fps
             : act == POSE_WALK ? (now % 1100) / 1100.0f
                                : (now % 2400) / 2400.0f; // pose diam: gerakan pelan
  int16_t hx = cx - (int16_t)lroundf((act == POSE_SIT ? 3.0f : act == POSE_RUN ? 2.0f : 0.5f) * u);
  figure(d, hx, ground, h, act, ph, c);
}

// ============================================================== bagian halaman
static const char *stateText(const UiNode &n)
{
  switch (n.state)
  {
  case UI_NODE_RUN:
    return "RUN";
  case UI_NODE_STOP:
    return "STOP";
  case UI_NODE_OFF:
    return "OFF";
  case UI_NODE_CAL:
    return "KAL";
  default:
    return "---";
  }
}

// Status node di kotak (28x11); berkedip saat perintah belum diterapkan node
static void pill(Adafruit_GFX &d, int16_t x, int16_t y, const UiNode &n, uint32_t now)
{
  bool inv = n.state == UI_NODE_RUN || n.state == UI_NODE_CAL;
  if (n.pending && (now / 300) % 2)
    inv = !inv;
  if (inv)
    d.fillRoundRect(x, y, 28, 11, 2, 1);
  else
    d.drawRoundRect(x, y, 28, 11, 2, 1);
  textC(d, x + 14, y + 2, stateText(n), 1, inv ? 0 : 1);
}

static const char *calText(const UiNode &n)
{
  if (n.calPending)
    return "kalib: tunggu";
  switch (n.cal & 0x0F)
  {
  case 1:
    return "kalibrasi...";
  case 2:
    return "kalib OK";
  case 3:
    return "kalib gyro";
  case 4:
  case 5:
    return "kalib GAGAL";
  case 6:
  case 7:
    return (n.cal & 0x80) ? "kalib OK" : "kalib gyro";
  default:
    return "belum kalib";
  }
}

// Bar atas: judul kiri; mode, status cloud (PROD), buzzer mati, baterai GS kanan
static void header(Adafruit_GFX &d, const char *title, const UiState &s)
{
  d.fillRect(0, 0, UI_W, 10, 1);
  text(d, 2, 1, title, 1, 0);
  int16_t x = UI_W - 14;
  iconBattery(d, x, 1, s.gsBattPct, 0);
  if (!s.buzzerOn)
  {
    x -= 13;
    iconMute(d, x, 1, 0);
  }
  if (s.prod)
  {
    x -= 15;
    iconCloud(d, x, 1, s.cloud, s.nowMs, 0);
  }
  const char *m = s.prod ? "PROD" : "DEBUG";
  x -= textW(m) + 4;
  text(d, x, 1, m, 1, 0);
}

// Mode PROD tanpa koneksi cloud: teks untuk baris bawah (nullptr = online / sedang menyambung)
static const char *offlineText(const UiState &s)
{
  if (!s.prod)
    return nullptr;
  switch (s.cloud)
  {
  case UI_CLOUD_NO_WIFI:
    return "OFFLINE: WiFi";
  case UI_CLOUD_ERR:
    return "OFFLINE: server";
  default:
    return nullptr;
  }
}

// Bar bawah: petunjuk tombol (bergantian tiap 3 s) + titik halaman.
// Mode PROD offline: diganti "OFFLINE: ..." terbalik; sedang menyambung: "menyambung cloud" bergantian.
static void footer(Adafruit_GFX &d, const UiState &s, const char *const *hints, uint8_t n)
{
  const char *off = offlineText(s);
  uint16_t c = 1;
  if (off)
  { // offline: baris bawah terbalik terus-menerus, mudah terlihat
    d.fillRect(0, 54, UI_W, 10, 1);
    text(d, 1, 56, off, 1, 0);
    c = 0;
  }
  else
  {
    d.drawFastHLine(0, 54, UI_W, 1);
    bool connecting = s.prod && s.cloud == UI_CLOUD_WIFI;
    if (connecting && (!n || (s.nowMs / 3000) % 2))
      text(d, 0, 56, "menyambung cloud");
    else if (n)
      text(d, 0, 56, hints[(s.nowMs / 3000) % n]);
  }
  for (uint8_t i = 0; i < s.numPages; i++)
  {
    int16_t cx = UI_W - 3 - (s.numPages - 1 - i) * 4;
    if (i == s.page)
      d.fillCircle(cx, 59, 2, c);
    else
      d.drawPixel(cx, 59, c);
  }
}

// Pesan singkat: menutupi area isi (antara header dan footer)
static void toast(Adafruit_GFX &d, const char *msg)
{
  d.fillRect(0, 11, UI_W, 43, 0);
  d.drawRoundRect(4, 16, UI_W - 8, 33, 4, 1);
  d.drawRoundRect(5, 17, UI_W - 10, 31, 3, 1);
  textC(d, UI_W / 2, 29, msg);
}

// ============================================================== halaman utama
static void pageMainDebug(Adafruit_GFX &d, const UiState &s)
{
  header(d, "HAR", s);
  char b[24];
  for (uint8_t i = 0; i < UI_NODES; i++)
  {
    const UiNode &n = s.node[i];
    int16_t y = 12 + i * 12;
    snprintf(b, sizeof(b), "N%u", i + 1);
    text(d, 2, y + 2, b);
    pill(d, 16, y, n, s.nowMs);
    iconSignal(d, 48, y + 2, signalLevel(n), 1);
    if (n.state == UI_NODE_NONE || n.state == UI_NODE_OFF)
    {
      text(d, 63, y + 2, n.state == UI_NODE_OFF ? "hilang" : "menunggu");
      continue;
    }
    snprintf(b, sizeof(b), "%uHz", n.pktHz);
    text(d, 63, y + 2, b);
    iconBattery(d, 94, y + 2, battPct(n.battMv), 1);
    snprintf(b, sizeof(b), "%.1f", n.battMv / 1000.0f);
    textR(d, 127, y + 2, b);
  }

  snprintf(b, sizeof(b), "Rate %u Hz", s.rateHz);
  text(d, 2, 38, b);
  if (s.anyRun)
    iconLock(d, 2 + textW(b) + 3, 38, 1);
  textR(d, 127, 38, s.pcOnline ? "PC \xfb" : "PC -");
  if (s.badPkt)
  {
    snprintf(b, sizeof(b), "paket rusak: %lu", (unsigned long)s.badPkt);
    text(d, 2, 46, b);
  }

  const char *h[3] = {s.anyRun ? "OK: stop semua" : "OK: mulai semua",
                      s.anyRun ? "rate terkunci" : "\x1e\x1f: ganti rate", "tahan OK: reset"};
  footer(d, s, h, 3);
}

static void pageMainProd(Adafruit_GFX &d, const UiState &s)
{
  header(d, "HAR", s);
  char b[16];
  for (uint8_t i = 0; i < UI_NODES; i++)
  {
    const UiNode &n = s.node[i];
    int16_t x0 = i * 64, cx = x0 + 31;
    bool fresh = n.state == UI_NODE_RUN && n.hasResult;
    uint8_t stage = fresh ? n.buzzStage : 0; // 0 normal, 1 pemberitahuan, 2 peringatan, 3 hukuman
    bool alarm = stage >= 2;                 // kotak berkedip mulai PERINGATAN
    bool inv = alarm && (s.nowMs / (stage >= 3 ? 200 : 600)) % 2;
    uint16_t c = inv ? 0 : 1;
    if (inv)
      d.fillRect(x0, 11, 63, 43, 1);
    snprintf(b, sizeof(b), "N%u", i + 1);
    text(d, x0 + 2, 12, b, 1, c);
    iconSignal(d, x0 + 50, 12, signalLevel(n), c);

    if (n.state != UI_NODE_RUN)
    {
      const char *t = n.state == UI_NODE_STOP ? "berhenti" : n.state == UI_NODE_OFF ? "hilang"
                                                         : n.state == UI_NODE_CAL   ? "kalibrasi"
                                                                                    : "belum ada";
      if (n.state == UI_NODE_STOP)
      {
        d.fillRect(cx - 5, 21, 3, 12, c);
        d.fillRect(cx + 3, 21, 3, 12, c);
      }
      else if (n.state == UI_NODE_CAL)
      {
        spinner(d, cx, 27, s.nowMs);
      }
      else
      {
        textC(d, cx, 20, "?", 2, c);
      }
      textC(d, cx, 38, t, 1, c);
      if (n.state == UI_NODE_STOP)
        textC(d, cx, 46, "via web", 1, c); // PROD: START/STOP lewat web
      continue;
    }
    if (!n.hasResult)
    {
      spinner(d, cx, 27, s.nowMs);
      textC(d, cx, 38, "menunggu", 1, c);
      textC(d, cx, 46, "hasil 5 s", 1, c);
      continue;
    }
    if (n.act >= 4 || !(n.flags & UI_RES_HAS_ACT))
    {
      textC(d, cx, 20, "?", 2, c);
      textC(d, cx, 38, "model -", 1, c);
      continue;
    }
    activityFigure(d, cx, 36, 20, n.act, s.nowMs, c);
    if (stage >= 1)
      snprintf(b, sizeof(b), "%s %um", ACT_NAME[n.act], n.runMin);
    else
      snprintf(b, sizeof(b), "%s", ACT_NAME[n.act]);
    textC(d, cx, 38, b, 1, c);
    if (n.flags & UI_RES_TEST_FAIL)
      snprintf(b, sizeof(b), "model error");
    else if (stage >= 3)
      snprintf(b, sizeof(b), "%s", n.watchSit ? "BANGUN!" : "ISTIRAHAT!");
    else if (stage == 2)
      snprintf(b, sizeof(b), "PERINGATAN");
    else
      snprintf(b, sizeof(b), "%u%%", (n.prob[n.act] * 100 + 127) / 255);
    textC(d, cx, 46, b, 1, c);
  }
  for (int16_t y = 12; y < 54; y += 2)
    d.drawPixel(63, y, 1);

  const char *h[2] = {"kontrol: web", "\x11\x10: detail node"}; // PROD: tombol tidak START/STOP
  footer(d, s, h, 2);
}

// ============================================================== halaman node
// PRODUCTION: hanya aktivitas terkini, besar & beranimasi. Status node di header.
static void pageNodeProd(Adafruit_GFX &d, const UiState &s, uint8_t idx)
{
  const UiNode &n = s.node[idx];
  char b[24];
  snprintf(b, sizeof(b), "NODE %u", idx + 1);
  d.fillRect(0, 0, UI_W, 10, 1);
  text(d, 2, 1, b, 1, 0);
  iconSignal(d, 98, 1, signalLevel(n), 0);
  iconBattery(d, UI_W - 14, 1, battPct(n.battMv), 0);

  bool run = n.state == UI_NODE_RUN;
  const char *h[2] = {"kontrol: web", "tahan OK kalibrasi"}; // PROD: START/STOP lewat web
  bool ok = run && n.hasResult && n.act < 4 && (n.flags & UI_RES_HAS_ACT);
  if (!ok)
  {
    if (!run)
    {
      const char *t = n.state == UI_NODE_STOP ? "berhenti" : n.state == UI_NODE_OFF ? "node hilang"
                                                         : n.state == UI_NODE_CAL   ? "kalibrasi..."
                                                                                    : "belum ada data";
      if (n.state == UI_NODE_STOP)
      {
        d.fillRoundRect(55, 16, 6, 20, 1, 1);
        d.fillRoundRect(67, 16, 6, 20, 1, 1);
      }
      else if (n.state == UI_NODE_CAL)
      {
        spinner(d, 64, 26, s.nowMs);
      }
      else
      {
        textC(d, 64, 15, "?", 3);
      }
      textC(d, 64, 42, t);
    }
    else
    {
      spinner(d, 64, 24, s.nowMs);
      textC(d, 64, 34, n.hasResult ? "model belum ada" : "menunggu hasil");
      if (!n.hasResult)
        textC(d, 64, 43, "(tiap 5 detik)");
    }
    footer(d, s, h, 2);
    return;
  }

  uint8_t stage = n.buzzStage; // 0 normal, 1 pemberitahuan, 2 peringatan, 3 hukuman
  bool alarm = stage >= 2;     // berkedip mulai PERINGATAN
  bool inv = alarm && (s.nowMs / (stage >= 3 ? 200 : 600)) % 2;
  uint16_t c = inv ? 0 : 1;
  if (inv)
    d.fillRect(0, 11, UI_W, 43, 1);
  activityFigure(d, 26, 51, 36, n.act, s.nowMs, c);
  textR(d, 124, 16, ACT_NAME[n.act], 2, c);

  uint8_t conf = (n.prob[n.act] * 100 + 127) / 255;
  const char *posisi = n.watchSit ? "duduk" : "berdiri";
  char l1[16] = "", l2[20] = "";
  if (n.flags & UI_RES_TEST_FAIL)
    snprintf(l1, sizeof(l1), "model error");
  else if (stage >= 3)
  {
    snprintf(l1, sizeof(l1), "%s", n.watchSit ? "BANGUN!" : "ISTIRAHAT!");
    snprintf(l2, sizeof(l2), "%s %u mnt", posisi, n.runMin);
  }
  else if (stage == 2)
  {
    snprintf(l1, sizeof(l1), "PERINGATAN");
    snprintf(l2, sizeof(l2), "%s %u mnt", posisi, n.runMin);
  }
  else if (stage == 1)
  {
    snprintf(l1, sizeof(l1), "%u%%", conf);
    snprintf(l2, sizeof(l2), "diingatkan");
  }
  else
    snprintf(l1, sizeof(l1), "%u%%", conf);
  textR(d, 124, 36, l1, 1, c);
  textR(d, 124, 45, l2, 1, c);
  footer(d, s, h, 2);
}

static void pageNode(Adafruit_GFX &d, const UiState &s, uint8_t idx)
{
  if (s.prod)
  {
    pageNodeProd(d, s, idx);
    return;
  }
  const UiNode &n = s.node[idx];
  char b[28];
  snprintf(b, sizeof(b), "NODE %u", idx + 1);
  header(d, b, s);
  pill(d, 2, 12, n, s.nowMs);
  iconSignal(d, 34, 14, signalLevel(n), 1);
  bool run = n.state == UI_NODE_RUN;
  const char *h[3] = {run ? "OK: stop node" : "OK: mulai node", "tahan OK kalibrasi", "\x11\x10: halaman"};

  if (n.state == UI_NODE_NONE)
  {
    textC(d, 64, 26, "belum ada data");
    textC(d, 64, 36, "nyalakan node");
    footer(d, s, h + 2, 1);
    return;
  }
  iconBattery(d, 49, 14, battPct(n.battMv), 1);
  snprintf(b, sizeof(b), "%.2fV %uHz", n.battMv / 1000.0f, n.pktHz);
  textR(d, 127, 14, b);
  text(d, 2, 25, n.imuOk ? "IMU OK" : "IMU ERROR");
  textR(d, 127, 25, calText(n));
  if (run)
  {
    snprintf(b, sizeof(b), "A%+6.2f%+6.2f%+6.2f", n.acc[0], n.acc[1], n.acc[2]);
    text(d, 2, 35, b);
    snprintf(b, sizeof(b), "G%+6.0f%+6.0f%+6.0f", n.gyr[0], n.gyr[1], n.gyr[2]);
    text(d, 2, 44, b);
  }
  else
  {
    textC(d, 64, 39, n.state == UI_NODE_OFF ? "tidak terjangkau" : "tidak merekam");
  }
  footer(d, s, h, 3);
}

// ============================================================== halaman setting
// Teks dipotong agar muat maxChars karakter ("~" = terpotong)
static void fitText(char *b, size_t n, const char *t, uint8_t maxChars)
{
  if (strlen(t) > maxChars)
    snprintf(b, n, "%.*s~", maxChars - 1, t);
  else
    snprintf(b, n, "%s", t);
}

static void agoText(char *b, size_t n, const char *what, uint32_t agoS)
{
  if (agoS < 60)
    snprintf(b, n, "%s %lu dtk lalu", what, (unsigned long)agoS);
  else if (agoS < 3600)
    snprintf(b, n, "%s %lu mnt lalu", what, (unsigned long)(agoS / 60));
  else
    snprintf(b, n, "%s %lu jam lalu", what, (unsigned long)(agoS / 3600));
}

// Status koneksi database untuk baris keterangan (maks 21 karakter)
static void cloudText(char *b, size_t n, const UiState &s)
{
  if (!s.prod)
  {
    snprintf(b, n, "aktif di mode PROD");
    return;
  }
  switch (s.cloud)
  {
  case UI_CLOUD_WIFI:
    snprintf(b, n, s.wifiUp ? "menghubungi server" : "WiFi menyambung...");
    break;
  case UI_CLOUD_NO_WIFI:
    snprintf(b, n, "WiFi tidak terhubung");
    break;
  case UI_CLOUD_ONLINE:
    agoText(b, n, "online", s.cloudAgoS);
    break;
  case UI_CLOUD_ERR:
    if (s.cloudCode < 0)
      snprintf(b, n, "koneksi gagal (%d)", s.cloudCode);
    else
      snprintf(b, n, "gagal HTTP %d", s.cloudCode);
    break;
  default:
    snprintf(b, n, "cloud mati");
  }
}

// Aturan buzzer node dari web ("duduk 20m"), "-" = tidak ada sesi
static void ruleText(char *b, size_t n, const UiNode &u)
{
  if (u.sess == UI_SESS_RECORDING || u.sess == UI_SESS_FLUSHING)
    snprintf(b, n, "%s %um", u.watchSit ? "duduk" : "berdiri", u.thrMin);
  else
    snprintf(b, n, "-");
}

// Sesi node untuk keterangan item ID node
static void sessText(char *b, size_t n, const UiNode &u)
{
  switch (u.sess)
  {
  case UI_SESS_IDLE:
    snprintf(b, n, "sesi: tidak aktif");
    break;
  case UI_SESS_RECORDING:
    if (u.queued > 12)
      snprintf(b, n, "merekam, antre %u", u.queued);
    else
      snprintf(b, n, "sesi: merekam");
    break;
  case UI_SESS_FLUSHING:
    snprintf(b, n, "kirim sisa (%u)", u.queued);
    break;
  default:
    snprintf(b, n, "sesi: belum terbaca");
  }
}

// Menu utama (Mode, Buzzer >, Cloud >) dan submenu Buzzer / Cloud, masing-masing 3 baris
static void pageSetting(Adafruit_GFX &d, const UiState &s)
{
  static const char *const TITLE[3] = {"SETTING", "BUZZER", "CLOUD"};
  uint8_t menu = s.setMenu <= UI_MENU_CLOUD ? s.setMenu : UI_MENU_TOP;
  header(d, TITLE[menu], s);
  char v[UI_SET_ITEMS][16], dsc[UI_SET_ITEMS][24];
  const char *lbl[UI_SET_ITEMS], *desc[UI_SET_ITEMS] = {dsc[0], dsc[1], dsc[2], dsc[3]};
  bool sub[UI_SET_ITEMS] = {false, false, false, false}; // membuka submenu / layar lain (tanda panah)
  // submenu Buzzer 4 baris (rapat, tanpa keterangan); lainnya 3 baris + keterangan
  const uint8_t rows = menu == UI_MENU_BUZZER ? UI_BZ_ITEMS : UI_SET_ITEMS_3;
  const int16_t rowH = rows == UI_BZ_ITEMS ? 10 : 11;
  bool modeLocked = false;
  if (menu == UI_MENU_TOP)
  {
    // mode hanya bisa diganti saat semua node terputus (sama dengan anyNodeOnline() di main.cpp)
    for (uint8_t i = 0; i < UI_NODES; i++)
      if (s.node[i].state != UI_NODE_NONE && s.node[i].state != UI_NODE_OFF)
        modeLocked = true;
    lbl[UI_TOP_MODE] = "Mode";
    lbl[UI_TOP_BUZZER] = "Buzzer";
    lbl[UI_TOP_CLOUD] = "Cloud";
    snprintf(v[UI_TOP_MODE], 16, "%s", s.prod ? "PRODUCTION" : "DEBUG");
    snprintf(v[UI_TOP_BUZZER], 16, "%s", s.buzzerOn ? "ON" : "OFF");
    snprintf(v[UI_TOP_CLOUD], 16, "%s", !s.prod ? "-" : s.cloud == UI_CLOUD_ONLINE ? "online" : "offline");
    sub[UI_TOP_BUZZER] = sub[UI_TOP_CLOUD] = true;
    snprintf(dsc[UI_TOP_MODE], 24, "%s",
             modeLocked ? "matikan semua node" : s.prod ? "PROD: sesi dari web" : "DEBUG: rekam ke app");
    snprintf(dsc[UI_TOP_BUZZER], 24, "buzzer GS & aturan");
    cloudText(dsc[UI_TOP_CLOUD], 24, s);
  }
  else if (menu == UI_MENU_BUZZER)
  {
    lbl[UI_BZ_GS] = "Buzzer GS";
    lbl[UI_BZ_DEMO] = "Demo alarm";
    lbl[UI_BZ_RULE1] = "Aturan N1";
    lbl[UI_BZ_RULE2] = "Aturan N2";
    snprintf(v[UI_BZ_GS], 16, "%s", s.buzzerOn ? "ON" : "OFF");
    v[UI_BZ_DEMO][0] = 0;
    sub[UI_BZ_DEMO] = true;
    ruleText(v[UI_BZ_RULE1], 16, s.node[0]);
    ruleText(v[UI_BZ_RULE2], 16, s.node[1]);
  }
  else
  {
    lbl[UI_CL_WIFI] = "WiFi";
    lbl[UI_CL_ID1] = "ID node 1";
    lbl[UI_CL_ID2] = "ID node 2";
    fitText(v[UI_CL_WIFI], 16, s.wifiSsid && s.wifiSsid[0] ? s.wifiSsid : "-", 12);
    fitText(v[UI_CL_ID1], 16, s.node[0].did[0] ? s.node[0].did : "-", 8);
    fitText(v[UI_CL_ID2], 16, s.node[1].did[0] ? s.node[1].did : "-", 8);
    sub[UI_CL_WIFI] = sub[UI_CL_ID1] = sub[UI_CL_ID2] = true;
    if (s.wifiUp)
      snprintf(dsc[UI_CL_WIFI], 24, "%d dBm, OK: ganti", s.wifiRssi);
    else
      snprintf(dsc[UI_CL_WIFI], 24, "OK: scan & ganti");
    sessText(dsc[UI_CL_ID1], 24, s.node[0]);
    sessText(dsc[UI_CL_ID2], 24, s.node[1]);
  }
  for (uint8_t i = 0; i < rows; i++)
  {
    int16_t y = 12 + i * rowH;
    bool sel = i == s.setItem;
    if (sel)
      d.fillRoundRect(0, y, UI_W, 10, 2, 1);
    uint16_t c = sel ? 0 : 1;
    text(d, 4, y + 1, lbl[i], 1, c);
    int16_t xr = sub[i] ? 114 : 123;
    textR(d, xr, y + 1, v[i], 1, c);
    if (sub[i])
      text(d, 118, y + 1, "\x10", 1, c); // panah: buka submenu / layar
    if (menu == UI_MENU_TOP && i == UI_TOP_MODE && modeLocked)
      iconLock(d, xr - textW(v[i]) - 7, y + 1, c);
  }
  if (rows == UI_SET_ITEMS_3)
    text(d, 2, 46, desc[s.setItem % UI_SET_ITEMS_3]);
  const char *h[1] = {menu == UI_MENU_TOP ? "\x1e\x1f pilih  OK ubah" : "OK ubah  \x11 kembali"};
  footer(d, s, h, 1);
}

// ============================================================== layar KONEKSI (boot)
// mark: 1 = OK, 2 = gagal, 0 = sedang dicek (titik berkedip)
static void netMark(Adafruit_GFX &d, int16_t x, int16_t y, uint8_t mark, uint32_t now)
{
  if (mark == 1)
    text(d, x, y, "\xfb");
  else if (mark == 2)
    text(d, x, y, "X");
  else if ((now / 300) % 2)
    d.fillCircle(x + 2, y + 3, 2, 1);
}

// WiFi -> login Firebase -> ONLINE / OFFLINE (main.cpp netCheck*)
static void pageNetCheck(Adafruit_GFX &d, const UiState &s)
{
  header(d, "KONEKSI", s);
  bool checking = s.netCheck == UI_NET_CHECK;
  bool online = s.cloud == UI_CLOUD_ONLINE;
  bool fail = s.cloud == UI_CLOUD_ERR;
  char b[24], ct[24];
  text(d, 2, 13, "WiFi");
  fitText(b, sizeof(b), s.wifiSsid && s.wifiSsid[0] ? s.wifiSsid : "-", 13);
  text(d, 36, 13, b);
  netMark(d, 120, 13, s.wifiUp ? 1 : checking ? 0 : 2, s.nowMs);

  text(d, 2, 23, "Cloud");
  if (online)
    snprintf(ct, sizeof(ct), "Firebase OK");
  else if (s.cloud == UI_CLOUD_WIFI && s.wifiUp)
    snprintf(ct, sizeof(ct), "menghubungi...");
  else if (s.cloud == UI_CLOUD_ERR && s.cloudCode < 0)
    snprintf(ct, sizeof(ct), "koneksi gagal");
  else if (s.cloud == UI_CLOUD_ERR)
    snprintf(ct, sizeof(ct), "HTTP %d", s.cloudCode);
  else
    snprintf(ct, sizeof(ct), "menunggu WiFi");
  text(d, 36, 23, ct);
  netMark(d, 120, 23, online ? 1 : (fail || !checking) ? 2 : 0, s.nowMs);

  d.drawFastHLine(0, 54, UI_W, 1);
  if (s.netCheck == UI_NET_ONLINE)
  {
    textC(d, UI_W / 2, 35, "ONLINE", 2);
    text(d, 0, 56, "siap dipakai");
  }
  else if (s.netCheck == UI_NET_OFFLINE)
  {
    d.fillRoundRect(16, 33, 96, 19, 3, 1);
    textC(d, UI_W / 2, 35, "OFFLINE", 2, 0);
    text(d, 0, 56, s.wifiUp ? "cek host & Rules" : "cek WiFi & sandi");
    textR(d, 127, 56, "OK");
  }
  else
  {
    spinner(d, UI_W / 2, 42, s.nowMs);
    snprintf(b, sizeof(b), "OK: lewati  %us", s.netSecLeft);
    text(d, 0, 56, b);
  }
}

// ============================================================== layar WiFi
const char UI_KB[UI_KB_PAGES][UI_KB_ROWS][UI_KB_COLS + 1] = {
    {"abcdefghij", "klmnopqrst", "uvwxyz.-_@"},
    {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ.-_@"},
    {"1234567890", "!#$%&*+=?/", "()[]{}<>:;"},
    {"\"',\\^`|~.-", "_@!#$%&*+=", "1234567890"},
};
static const char *const KB_PAGE_NAME[UI_KB_PAGES] = {"abc", "ABC", "123", "#+="};
static const char *const KEY_HINT[UI_KB_KEYS] = {"ganti huruf", "spasi", "hapus", "batal", "selesai"};

static uint8_t rssiLevel(int8_t rssi)
{
  return rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
}

static void wifiHeader(Adafruit_GFX &d, const char *title, const char *right)
{
  d.fillRect(0, 0, UI_W, 10, 1);
  text(d, 2, 1, title, 1, 0);
  if (right)
    textR(d, 126, 1, right, 1, 0);
}

static void hintBar(Adafruit_GFX &d, const char *hint)
{
  d.drawFastHLine(0, 54, UI_W, 1);
  text(d, 0, 56, hint);
}

// Daftar hasil scan: baris 0 = scan ulang, lalu jaringan (sinyal, gembok, nama, centang = dipakai)
static void pageWifiList(Adafruit_GFX &d, const UiState &s)
{
  bool scanning = s.wifiJob == UI_WJ_SCANNING;
  wifiHeader(d, "PILIH WIFI", nullptr);
  const uint8_t ROWS = 4;
  uint8_t total = 1 + s.apCount;
  uint8_t first = s.apSel ? s.apSel - 1 : 0;
  if (total <= ROWS)
    first = 0;
  else if (first > total - ROWS)
    first = total - ROWS;
  for (uint8_t r = 0; r < ROWS && first + r < total; r++)
  {
    uint8_t i = first + r;
    int16_t y = 12 + r * 10;
    bool sel = i == s.apSel;
    if (sel)
      d.fillRoundRect(0, y, 123, 10, 2, 1);
    uint16_t c = sel ? 0 : 1;
    if (i == 0)
    {
      text(d, 4, y + 1, scanning ? "Memindai..." : "Scan ulang", 1, c);
      continue;
    }
    const UiAp &a = s.aps[i - 1];
    iconSignal(d, 3, y + 1, rssiLevel(a.rssi), c);
    if (a.secure)
      iconLock(d, 16, y + 1, c);
    char b[20];
    fitText(b, sizeof(b), a.ssid, 15);
    text(d, 24, y + 1, b, 1, c);
    if (s.wifiSsid && !strcmp(a.ssid, s.wifiSsid))
      text(d, 116, y + 1, "\xfb", 1, c);
  }
  if (total > ROWS)
  { // penggulir
    const int16_t TY = 12, TH = 40;
    int16_t th = TH * ROWS / total;
    int16_t ty = TY + (TH - th) * first / (total - ROWS);
    for (int16_t y = TY; y < TY + TH; y += 2)
      d.drawPixel(126, y, 1);
    d.fillRect(125, ty, 3, th, 1);
  }
  if (!s.apCount)
  {
    if (scanning)
      spinner(d, 64, 36, s.nowMs);
    else
      textC(d, 64, 33, s.wifiJob == UI_WJ_SCAN_FAIL ? "scan gagal" : "tidak ada jaringan");
  }
  hintBar(d, "OK pilih  \x11 kembali");
}

// Keyboard (sandi WiFi / device ID): panah = gerak, OK = ketik / tombol, tahan OK = selesai
static void pageWifiPass(Adafruit_GFX &d, const UiState &s)
{
  bool onKeys = s.kbRow >= UI_KB_ROWS;
  const char *title = s.kbTitle ? s.kbTitle : "SANDI";
  char t[22];
  fitText(t, sizeof(t), s.kbHint ? s.kbHint : "", 20 - (uint8_t)strlen(title));
  wifiHeader(d, title, onKeys ? KEY_HINT[s.kbCol % UI_KB_KEYS] : t);
  // kolom sandi: 20 karakter terakhir + kursor berkedip
  d.drawRect(0, 11, UI_W, 11, 1);
  const char *p = s.wPass ? s.wPass : "";
  size_t n = strlen(p);
  const char *shown = n > 20 ? p + n - 20 : p;
  text(d, 3, 13, shown);
  if ((s.nowMs / 400) % 2)
    d.drawFastVLine(3 + (int16_t)strlen(shown) * 6, 13, 7, 1);
  // grid karakter 10 x 3
  uint8_t pg = s.kbPage % UI_KB_PAGES;
  for (uint8_t r = 0; r < UI_KB_ROWS; r++)
    for (uint8_t c = 0; c < UI_KB_COLS; c++)
    {
      int16_t x = 4 + c * 12, y = 24 + r * 9;
      bool sel = !onKeys && r == s.kbRow && c == s.kbCol;
      if (sel)
        d.fillRoundRect(x, y - 1, 11, 9, 1, 1);
      char ch[2] = {UI_KB[pg][r][c], 0};
      text(d, x + 3, y, ch, 1, sel ? 0 : 1);
    }
  // tombol bawah: halaman huruf, spasi, hapus, batal, OK
  d.drawFastHLine(0, 51, UI_W, 1);
  for (uint8_t k = 0; k < UI_KB_KEYS; k++)
  {
    int16_t x = 1 + k * 25, cx = x + 12;
    bool sel = onKeys && k == s.kbCol;
    if (sel)
      d.fillRoundRect(x, 53, 24, 11, 2, 1);
    uint16_t c = sel ? 0 : 1;
    switch (k)
    {
    case UI_KEY_PAGE:
      textC(d, cx, 55, KB_PAGE_NAME[(pg + 1) % UI_KB_PAGES], 1, c);
      break;
    case UI_KEY_SPACE: // tanda spasi
      d.drawFastHLine(cx - 6, 61, 13, c);
      d.drawFastVLine(cx - 6, 58, 3, c);
      d.drawFastVLine(cx + 6, 58, 3, c);
      break;
    case UI_KEY_DEL:
      textC(d, cx, 55, "\x1b", 1, c);
      break;
    case UI_KEY_CANCEL:
      textC(d, cx, 55, "X", 1, c);
      break;
    default:
      textC(d, cx, 55, "OK", 1, c);
    }
  }
}

static const char *wifiReasonText(uint8_t r)
{
  switch (r)
  {
  case 2:   // AUTH_EXPIRE
  case 15:  // 4WAY_HANDSHAKE_TIMEOUT
  case 202: // AUTH_FAIL
  case 204: // HANDSHAKE_TIMEOUT
    return "sandi salah?";
  case 201: // NO_AP_FOUND
    return "tidak ditemukan";
  case 0:
    return "tidak menjawab";
  default:
    return nullptr;
  }
}

static void pageWifiConnect(Adafruit_GFX &d, const UiState &s)
{
  wifiHeader(d, "WIFI", nullptr);
  char t[24];
  fitText(t, sizeof(t), s.wTarget ? s.wTarget : "", 21);
  if (s.wifiJob == UI_WJ_CONNECT_OK)
  {
    textC(d, 64, 13, "\xfb", 2);
    textC(d, 64, 33, "Terhubung");
    textC(d, 64, 43, t);
    return;
  }
  if (s.wifiJob == UI_WJ_CONNECT_FAIL)
  {
    iconWarn(d, 4, 13, 1);
    text(d, 24, 13, "Gagal sambung");
    char b[16];
    const char *why = wifiReasonText(s.wifiReason);
    if (!why)
    {
      snprintf(b, sizeof(b), "kode %u", s.wifiReason);
      why = b;
    }
    text(d, 24, 23, why);
    textC(d, 64, 38, t);
    hintBar(d, "OK ulangi  \x11 daftar");
    return;
  }
  spinner(d, 64, 19, s.nowMs);
  textC(d, 64, 28, "Menyambung ke");
  textC(d, 64, 38, t);
  hintBar(d, "maks 15 detik");
}

// ============================================================== layar DEMO ALARM
// Baris: tingkat 1..3 dan "Semua" (1 -> 2 -> 3). Baris yang sedang berbunyi menampilkan sisa detik.
static void pageDemo(Adafruit_GFX &d, const UiState &s)
{
  static const char *const LBL[4] = {"1 Pemberitahuan", "2 Peringatan", "3 Hukuman", "Semua 1>2>3"};
  header(d, "DEMO ALARM", s);
  for (uint8_t i = 0; i < 4; i++)
  {
    int16_t y = 12 + i * 10;
    bool sel = i == s.demoSel;
    // baris yang sedang berbunyi: tingkat itu sendiri, atau "Semua" bila dipilih
    bool playing = s.demoStage && (i == s.demoSel && (i == UI_DEMO_ALL || i + 1 == s.demoStage));
    if (sel)
      d.fillRoundRect(0, y, UI_W, 10, 2, 1);
    uint16_t c = sel ? 0 : 1;
    text(d, 4, y + 1, LBL[i], 1, c);
    if (playing)
    {
      char b[12];
      if (i == UI_DEMO_ALL)
        snprintf(b, sizeof(b), "T%u %us", s.demoStage, s.demoLeftS);
      else
        snprintf(b, sizeof(b), "%us", s.demoLeftS);
      textR(d, 123, y + 1, b, 1, c);
    }
  }
  const char *h[1] = {s.demoStage ? "OK stop  \x11 kembali" : "OK bunyi  \x11 kembali"};
  footer(d, s, h, 1);
}

// ============================================================== API
void uiDraw(Adafruit_GFX &d, const UiState &s)
{
  clear(d);
  if (s.netCheck != UI_NET_NONE)
    pageNetCheck(d, s);
  else if (s.demo)
    pageDemo(d, s);
  else if (s.wifiScreen == UI_WIFI_LIST)
    pageWifiList(d, s);
  else if (s.wifiScreen == UI_WIFI_PASS || s.wifiScreen == UI_WIFI_ID)
    pageWifiPass(d, s);
  else if (s.wifiScreen == UI_WIFI_CONNECT)
    pageWifiConnect(d, s);
  else if (s.page == 0)
  {
    if (s.prod)
      pageMainProd(d, s);
    else
      pageMainDebug(d, s);
  }
  else if (s.page <= UI_NODES)
  {
    pageNode(d, s, s.page - 1);
  }
  else
  {
    pageSetting(d, s);
  }
  if (s.toast)
    toast(d, s.toast);
}

static float easeOut(float k)
{
  k = k < 0 ? 0 : k > 1 ? 1 : k;
  return 1 - (1 - k) * (1 - k);
}

bool uiBoot(Adafruit_GFX &d, uint32_t t, const char *version)
{
  clear(d);
  // 1. pelari melintas
  if (t < 1500)
  {
    float x = -14 + t * (156.0f / 1500.0f);
    figure(d, (int16_t)x, 52, 22, POSE_RUN, (t % 420) / 420.0f, 1);
    int16_t off = (t / 12) % 8;
    for (int16_t gx = -off; gx < UI_W; gx += 8)
      d.drawFastHLine(gx, 53, 4, 1);
  }
  // 2. logo turun
  if (t >= 600)
  {
    float k = easeOut((t - 600) / 500.0f);
    text(d, 10, (int16_t)(-24 + k * 36), "HAR", 3, 1);
  }
  // 3. garis + "Human Activity Tracker" muncul huruf per huruf
  if (t >= 1100)
  {
    d.drawFastVLine(64, 12, (int16_t)(26 * easeOut((t - 1100) / 300.0f)), 1);
    static const char *const L[3] = {"Human", "Activity", "Tracker"};
    int shown = (int)((t - 1100) / 30);
    for (int i = 0; i < 3; i++)
    {
      int n = shown - i * 5;
      if (n <= 0)
        continue;
      char buf[12];
      snprintf(buf, sizeof(buf), "%.*s", n, L[i]);
      text(d, 69, 12 + i * 9, buf);
    }
  }
  // 4. subjudul + bar memuat
  if (t >= 1700)
  {
    char b[28];
    snprintf(b, sizeof(b), "ground station %s", version);
    textC(d, UI_W / 2, 41, b);
    float k = (t - 1700) / (float)(UI_BOOT_MS - 1900);
    k = k > 1 ? 1 : k;
    d.drawRoundRect(14, 51, 100, 6, 2, 1);
    d.fillRect(16, 53, (int16_t)(96 * k), 2, 1);
  }
  return t < UI_BOOT_MS;
}

void uiSplash(Adafruit_GFX &d, const char *line1, const char *line2, uint32_t nowMs)
{
  clear(d);
  textC(d, UI_W / 2, 4, "HAR", 2);
  d.drawFastHLine(24, 22, 80, 1);
  if (line1)
    textC(d, UI_W / 2, 28, line1);
  if (line2)
    textC(d, UI_W / 2, 38, line2);
  spinner(d, UI_W / 2, 54, nowMs);
}

void uiNrfError(Adafruit_GFX &d, uint16_t attempt, uint32_t nowMs)
{
  clear(d);
  d.fillRect(0, 0, UI_W, 10, 1);
  text(d, 2, 1, "RADIO GAGAL", 1, 0);
  iconWarn(d, 4, 15, 1);
  text(d, 24, 14, "nRF24 tidak");
  text(d, 24, 23, "terdeteksi");
  text(d, 2, 35, "Cek kabel SPI, 3V3,");
  text(d, 2, 44, "GND & kapasitor nRF");
  d.drawFastHLine(0, 54, UI_W, 1);
  char b[24];
  snprintf(b, sizeof(b), "coba lagi #%u", attempt);
  text(d, 0, 56, b);
  spinner(d, 116, 59, nowMs);
}

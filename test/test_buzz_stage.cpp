// Tes host (PC, tanpa hardware) untuk alarm buzzer: src/ground_station/buzz_stage.h
// (buzzAdvance = hitung durasi posisi, buzzThresholdFor = ambang posisi, buzzStageFor = tingkat bunyi).
// Fungsi yang diuji SAMA dengan yang dipakai cloudLoop() di cloud.cpp; hanya
// `buzzer = pos valid && runSec >= ambang*60` ditiru di Sim::report().
//
// Jalankan (butuh compiler C++ di PC):  c++ -std=c++17 -I src/ground_station test/test_buzz_stage.cpp -o test_buzz && ./test_buzz
#include <cstdio>
#include <initializer_list>
#include "buzz_stage.h"

enum { SITTING = 0, STANDING = 1, WALKING = 2, RUNNING = 3 };

static int fails = 0;
#define CHECK(cond, ...)             \
  do                                 \
  {                                  \
    if (!(cond))                     \
    {                                \
      fails++;                       \
      printf("GAGAL: " __VA_ARGS__); \
      printf("\n");                  \
    }                                \
  } while (0)

// Simulasi satu node: laporan tiap `iv` detik. Ambang default duduk & berdiri = BUZZ_DEFAULT_MIN;
// buzzerRule web mengganti ambang SATU posisi (setWebRule).
struct Sim
{
  CloudNode cn{};
  uint32_t t = 0;
  uint8_t pos = BUZZ_POS_NONE;
  uint8_t thrSit = BUZZ_DEFAULT_MIN, thrStand = BUZZ_DEFAULT_MIN;

  explicit Sim(uint16_t iv = 5)
  {
    cn.sess = SESS_RECORDING;
    cn.intervalSec = iv;
  }
  void setWebRule(uint8_t position, uint8_t thrMin)
  {
    (position == STANDING ? thrStand : thrSit) = thrMin;
  }
  uint8_t report(uint8_t label)
  {
    t += cn.intervalSec;
    buzzAdvance(cn.runSec, pos, label, cn.intervalSec);
    cn.thresholdMin = buzzThresholdFor(pos, thrSit, thrStand);
    cn.buzzer = pos != BUZZ_POS_NONE && cn.runSec >= (uint32_t)cn.thresholdMin * 60UL;
    return buzzStageFor(cn);
  }
  // `sec` detik berturut-turut di `label`; kembalikan tingkat terakhir. onset = detik tingkat >= 1 pertama.
  uint8_t hold(uint8_t label, int sec, int *onset = nullptr)
  {
    uint8_t st = 0;
    for (int i = 0; i < sec / cn.intervalSec; i++)
    {
      st = report(label);
      if (onset && *onset < 0 && st >= 1)
        *onset = (int)t;
    }
    return st;
  }
};

static const char *name(uint8_t p) { return p == SITTING ? "duduk" : "berdiri"; }

int main()
{
  // 1) DEFAULT: duduk dan berdiri masing-masing diam <60 dtk, tingkat 1/2/3 di 60/90/120
  for (uint8_t p : {(uint8_t)SITTING, (uint8_t)STANDING})
  {
    Sim s;
    int onset = -1, st2 = -1, st3 = -1;
    for (int i = 0; i < 30; i++)
    {
      uint8_t st = s.report(p);
      if (s.t < 60)
        CHECK(st == 0, "%s: diam sebelum 60 dtk (t=%u, tingkat %u)", name(p), s.t, st);
      if (st >= 1 && onset < 0)
        onset = s.t;
      if (st >= 2 && st2 < 0)
        st2 = s.t;
      if (st >= 3 && st3 < 0)
        st3 = s.t;
    }
    CHECK(onset == 60, "%s: tingkat 1 di t=60 (didapat %d)", name(p), onset);
    CHECK(st2 == 90, "%s: tingkat 2 di t=90 (didapat %d)", name(p), st2);
    CHECK(st3 == 120, "%s: tingkat 3 di t=120 (didapat %d)", name(p), st3);
  }

  // 2) Gerak sebelum 60 dtk: hitungan reset, alarm tidak pernah bunyi
  for (uint8_t p : {(uint8_t)SITTING, (uint8_t)STANDING})
  {
    Sim s;
    CHECK(s.hold(p, 55) == 0, "%s: belum 60 dtk", name(p));
    CHECK(s.report(WALKING) == 0 && s.cn.runSec == 0, "%s: gerak membatalkan, runSec 0", name(p));
    CHECK(s.hold(p, 55) == 0, "%s: hitungan mulai dari nol", name(p));
  }

  // 3) Pelanggaran lalu bergerak: alarm mati, pelanggaran berikut mulai lagi dari tingkat 1
  {
    Sim s;
    CHECK(s.hold(STANDING, 130) == 3, "tingkat 3 di akhir pelanggaran pertama");
    CHECK(s.report(WALKING) == 0, "alarm mati begitu posisi berubah");
    CHECK(s.hold(STANDING, 60) == 1, "pelanggaran kedua mulai dari tingkat 1");
  }

  // 4) Berjalan / berlari tidak pernah memicu
  {
    Sim s;
    int onset = -1;
    s.hold(WALKING, 300, &onset);
    s.hold(RUNNING, 300, &onset);
    CHECK(onset < 0, "jalan/lari tidak memicu");
  }

  // 5) Sesi tidak merekam: tidak pernah berbunyi
  {
    Sim s;
    s.hold(STANDING, 100);
    s.cn.sess = SESS_IDLE;
    CHECK(buzzStageFor(s.cn) == 0, "tidak berbunyi saat sesi tidak merekam");
  }

  // 6) Web mengirim buzzerRule standing 20 mnt: berdiri pakai ambang web, DUDUK TETAP default 1 mnt
  {
    Sim s;
    s.setWebRule(STANDING, 20);
    int onset = -1;
    s.hold(STANDING, 150, &onset);
    CHECK(onset < 0, "berdiri 150 dtk belum memicu (ambang web 20 mnt), onset %d", onset);
    Sim d;
    d.setWebRule(STANDING, 20);
    onset = -1;
    d.hold(SITTING, 70, &onset);
    CHECK(onset == 60, "duduk tetap default 1 mnt walau web mengatur berdiri (onset %d)", onset);
  }

  // 7) Web mengirim buzzerRule sitting 2 mnt: duduk onset 120, BERDIRI TETAP default 1 mnt
  {
    Sim s;
    s.setWebRule(SITTING, 2);
    int onset = -1;
    s.hold(SITTING, 150, &onset);
    CHECK(onset == 120, "duduk ambang web 2 mnt: onset 120 (didapat %d)", onset);
    Sim d;
    d.setWebRule(SITTING, 2);
    onset = -1;
    d.hold(STANDING, 70, &onset);
    CHECK(onset == 60, "berdiri tetap default 1 mnt walau web mengatur duduk (onset %d)", onset);
  }

  // 8) Web mengirim 1 mnt untuk berdiri: sama dengan default, kedua posisi tetap 60 dtk
  {
    Sim s;
    s.setWebRule(STANDING, 1);
    int a = -1, b = -1;
    s.hold(STANDING, 70, &a);
    s.report(WALKING);
    s.hold(SITTING, 70, &b);
    CHECK(a == 60, "berdiri memicu di t=60 (didapat %d)", a);
    // jalan di t=75, duduk mulai terhitung di t=80 (runSec 5), mencapai 60 dtk di t=135
    CHECK(b == 135, "duduk memicu 60 dtk sejak mulai duduk (didapat %d)", b);
  }

  // 9) Duduk lalu langsung berdiri: hitungan posisi baru (tidak digabung)
  {
    Sim s;
    s.hold(SITTING, 55);
    CHECK(s.report(STANDING) == 0 && s.cn.runSec == 5, "pindah posisi: hitungan mulai lagi (runSec %u)",
          s.cn.runSec);
    int onset = -1;
    s.hold(STANDING, 60, &onset);
    // berdiri mulai terhitung di t=60 (runSec 5), mencapai 60 dtk di t=115
    CHECK(onset == 115, "berdiri memicu 60 dtk sejak pindah, bukan sejak duduk (didapat %d)", onset);
  }

  // 10) Web mengganti ambang di tengah sesi: berlaku seketika
  {
    Sim s;
    s.hold(STANDING, 30);
    s.setWebRule(STANDING, 20);
    CHECK(s.hold(STANDING, 300) == 0, "ambang web 20 mnt menggantikan default 1 mnt");
  }

  printf(fails ? "\n%d tes GAGAL\n" : "\nSemua tes lolos\n", fails);
  return fails ? 1 : 0;
}

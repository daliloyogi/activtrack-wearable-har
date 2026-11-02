#pragma once
/*
 * Tingkat buzzer node (mode PROD), 3 tingkat dalam 1 menit sejak aturan web terpenuhi:
 *   1 PEMBERITAHUAN  overtime <  BUZZ_STAGE1_MAX_S (30 dtk)  beep pendek, jeda renggang
 *   2 PERINGATAN     overtime <  BUZZ_STAGE2_MAX_S (60 dtk)  beep pendek, jeda lebih rapat
 *   3 HUKUMAN        overtime >= BUZZ_STAGE2_MAX_S           beep tiap 1 dtk (periode & pola di node)
 * overtime = lama melebihi ambang web (bukan dari awal duduk/berdiri).
 *
 * Pelanggaran berulang dalam sesi yang sama (siswa berdiri sebentar lalu duduk lagi) melompat
 * langsung ke tingkat lebih tinggi: pelanggaran ke-2 +1 tingkat, ke-3 dst +2 (dibatasi tingkat 3).
 *
 * Flag `buzzer` yang dikirim ke Firebase (CloudNode::buzzer, ESP32_GUIDE.md §8) TIDAK dipengaruhi
 * oleh ini - ini hanya pola bunyi fisik + tampilan OLED.
 *
 * Dipisah dari main.cpp (murni, tanpa hardware) supaya bisa diuji di PC: scratchpad cloudtest.
 */
#include <stdint.h>
#include "cloud.h"

#define BUZZ_STAGES 3
#define BUZZ_STAGE1_MAX_S 30 // tingkat 1 (pemberitahuan): overtime < ini
#define BUZZ_STAGE2_MAX_S 60 // tingkat 2 (peringatan): overtime < ini; >= ini -> tingkat 3 (hukuman)

struct BuzzTracker
{
  uint32_t gen;         // gen seri terakhir dilihat (CloudNode::gen beda -> reset hitungan)
  uint8_t offenseCount; // kali buzzer mati->nyala dalam sesi ini
  bool wasOn;
};

// Tingkat 0 (mati) .. BUZZ_STAGES. Panggil sekali per node tiap putaran (webSync()).
inline uint8_t buzzStageFor(BuzzTracker &bt, const CloudNode &cn)
{
  if (cn.gen != bt.gen)
  { // seri baru (web menekan Start lagi): hitungan pelanggaran mulai dari nol
    bt.gen = cn.gen;
    bt.offenseCount = 0;
    bt.wasOn = false;
  }
  bool on = cn.sess == SESS_RECORDING && cn.buzzer;
  if (on && !bt.wasOn)
    bt.offenseCount++; // tepi mati->nyala = pelanggaran baru (atau kembali setelah posisi berubah)
  bt.wasOn = on;
  if (!on)
    return 0;
  uint32_t thrSec = (uint32_t)cn.thresholdMin * 60UL;
  uint32_t overtimeSec = cn.runSec > thrSec ? cn.runSec - thrSec : 0;
  uint8_t base = overtimeSec < BUZZ_STAGE1_MAX_S ? 1 : overtimeSec < BUZZ_STAGE2_MAX_S ? 2 : 3;
  uint8_t stage = base + (bt.offenseCount - 1);
  return stage > BUZZ_STAGES ? BUZZ_STAGES : stage;
}

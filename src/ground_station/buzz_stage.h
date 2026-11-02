#pragma once
/*
 * Tingkat buzzer node (mode PROD), 3 tingkat dalam 1 menit sejak aturan web terpenuhi:
 *   1 PEMBERITAHUAN  overtime <  BUZZ_STAGE1_MAX_S (30 dtk)  beep pendek, jeda renggang
 *   2 PERINGATAN     overtime <  BUZZ_STAGE2_MAX_S (60 dtk)  beep pendek, jeda lebih rapat
 *   3 HUKUMAN        overtime >= BUZZ_STAGE2_MAX_S           beep tiap 1 dtk (periode & pola di node)
 * overtime = lama melebihi ambang web (bukan dari awal duduk/berdiri).
 *
 * Tidak ada memori pelanggaran: begitu posisi berubah (runSec kembali 0, buzzer web mati) bunyi
 * berhenti, dan pelanggaran berikutnya mulai lagi dari tingkat 1.
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

/*
 * Duduk DAN berdiri selalu diawasi, masing-masing punya ambang sendiri. Default keduanya
 * BUZZ_DEFAULT_MIN. buzzerRule dari web (satu posisi) hanya menggantikan ambang posisinya;
 * posisi lain tetap memakai default.
 */
#define BUZZ_DEFAULT_MIN 1
#define BUZZ_POS_NONE 0xFF // tidak sedang duduk / berdiri

// Maju satu laporan (iv detik) dengan label aktivitas `label` (0 sitting, 1 standing, sama dgn labels.py).
// runSec = lama berturut-turut di posisi yang sama; pos = posisi itu. Berpindah posisi (termasuk duduk
// langsung ke berdiri) mulai lagi dari satu laporan; selain duduk/berdiri mereset ke 0.
inline void buzzAdvance(uint32_t &runSec, uint8_t &pos, uint8_t label, uint16_t iv)
{
  if (label > 1)
  {
    runSec = 0;
    pos = BUZZ_POS_NONE;
    return;
  }
  runSec = pos == label ? runSec + iv : iv;
  pos = label;
}

// Ambang (menit) posisi `pos` (0 sitting, 1 standing); di luar keduanya -> ambang duduk.
inline uint8_t buzzThresholdFor(uint8_t pos, uint8_t thrSit, uint8_t thrStand)
{
  return pos == 1 ? thrStand : thrSit;
}

// Tingkat 0 (mati) .. BUZZ_STAGES. Tanpa state: hanya fungsi dari CloudNode saat ini.
inline uint8_t buzzStageFor(const CloudNode &cn)
{
  if (!(cn.sess == SESS_RECORDING && cn.buzzer))
    return 0;
  uint32_t thrSec = (uint32_t)cn.thresholdMin * 60UL;
  uint32_t overtimeSec = cn.runSec > thrSec ? cn.runSec - thrSec : 0;
  return overtimeSec < BUZZ_STAGE1_MAX_S ? 1 : overtimeSec < BUZZ_STAGE2_MAX_S ? 2 : 3;
}

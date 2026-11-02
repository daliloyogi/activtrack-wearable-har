#pragma once
/*
 * Fitur satu jendela IMU, dihitung di node (tanpa ketergantungan Arduino).
 * HARUS sama persis dengan app/core/features.py (referensi Python + nama kolom).
 *
 * Input per sampel: accel (g) dan gyro (deg/s) resolusi penuh, sudah dikalibrasi.
 * Semua std = populasi (dibagi N). Hitungan pakai double, hasil dikirim float32.
 *
 *  0 accel_std   1 accel_mean   2 accel_max   3 accel_min   4 accel_range   5 accel_rms   6 accel_energy
 *  7 gyro_std    8 gyro_mean    9 gyro_max   10 gyro_min   11 gyro_range   12 gyro_rms   13 gyro_energy
 *      -> dari magnitudo |a| = sqrt(ax²+ay²+az²) dan |g| = sqrt(gx²+gy²+gz²)
 *         rms = sqrt(mean(m²)), energy = mean(m²)
 * 14-16 accelX/Y/Z_mean   17-19 accelX/Y/Z_std   20-22 gyroX/Y/Z_mean   23-25 gyroX/Y/Z_std
 * 26 standing_indicator : cos sudut antara gravitasi rata-rata jendela dan sumbu "atas saat
 *                         berdiri" (STAND_AXIS, STAND_SIGN) -> 1 = tegak, 0 = mendatar, -1 = terbalik
 * 27 axis_gap_1         : |mean| sumbu accel terurut m1 >= m2 >= m3  ->  m1 - m2
 * 28 axis_gap_2         : m2 - m3
 * 29 axis_gap_ratio     : gap_1 / (gap_1 + gap_2), 0..1 (1 = satu sumbu dominan)
 */

#include <math.h>
#include <stdint.h>

#ifndef STAND_AXIS
#define STAND_AXIS 1 // sumbu accel yang mengarah ke ATAS saat pemakai berdiri: 0 = X, 1 = Y, 2 = Z
                     // node di paha: berdiri -> accel Y ~ -1 g (hasil uji hardware 2026-09-27)
#endif
#ifndef STAND_SIGN
#define STAND_SIGN -1 // +1 / -1 (arah sumbu itu saat berdiri)
#endif

#define FEAT_COUNT 30
#define FEAT_VERSION 1
#define FEAT_MIN_GRAVITY_G 0.1 // gravitasi rata-rata lebih kecil = arah tidak jelas -> indikator 0

struct ImuSample
{
  float a[3]; // g
  float g[3]; // deg/s
};

// Statistik satu sinyal (magnitudo): mean, std, max, min, range, rms, energy -> out[0..6]
static inline void featMagStats(const double *m, uint16_t n, float *out)
{
  double sum = 0, sq = 0, mx = m[0], mn = m[0];
  for (uint16_t i = 0; i < n; i++)
  {
    sum += m[i];
    sq += m[i] * m[i];
    if (m[i] > mx)
      mx = m[i];
    if (m[i] < mn)
      mn = m[i];
  }
  double mean = sum / n, dev = 0;
  for (uint16_t i = 0; i < n; i++)
    dev += (m[i] - mean) * (m[i] - mean);
  out[0] = (float)sqrt(dev / n);
  out[1] = (float)mean;
  out[2] = (float)mx;
  out[3] = (float)mn;
  out[4] = (float)(mx - mn);
  out[5] = (float)sqrt(sq / n);
  out[6] = (float)(sq / n);
}

// buf = ring buffer berkapasitas cap, first = index sampel tertua, n = jumlah sampel (>= 1).
// scratch = minimal 2*n double (magnitudo accel & gyro).
static inline void computeFeatures(const ImuSample *buf, uint16_t cap, uint16_t first, uint16_t n,
                                   double *scratch, float out[FEAT_COUNT])
{
  double *ma = scratch, *mg = scratch + n;
  double sum[6] = {0, 0, 0, 0, 0, 0};
  for (uint16_t i = 0; i < n; i++)
  {
    const ImuSample &s = buf[(first + i) % cap];
    ma[i] = sqrt((double)s.a[0] * s.a[0] + (double)s.a[1] * s.a[1] + (double)s.a[2] * s.a[2]);
    mg[i] = sqrt((double)s.g[0] * s.g[0] + (double)s.g[1] * s.g[1] + (double)s.g[2] * s.g[2]);
    for (int k = 0; k < 3; k++)
    {
      sum[k] += s.a[k];
      sum[3 + k] += s.g[k];
    }
  }
  featMagStats(ma, n, out + 0);
  featMagStats(mg, n, out + 7);

  double mean[6], dev[6] = {0, 0, 0, 0, 0, 0};
  for (int k = 0; k < 6; k++)
    mean[k] = sum[k] / n;
  for (uint16_t i = 0; i < n; i++)
  {
    const ImuSample &s = buf[(first + i) % cap];
    for (int k = 0; k < 3; k++)
    {
      dev[k] += (s.a[k] - mean[k]) * (s.a[k] - mean[k]);
      dev[3 + k] += (s.g[k] - mean[3 + k]) * (s.g[k] - mean[3 + k]);
    }
  }
  for (int k = 0; k < 3; k++)
  {
    out[14 + k] = (float)mean[k];
    out[17 + k] = (float)sqrt(dev[k] / n);
    out[20 + k] = (float)mean[3 + k];
    out[23 + k] = (float)sqrt(dev[3 + k] / n);
  }

  // orientasi dari gravitasi rata-rata jendela
  double gnorm = sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
  out[26] = gnorm < FEAT_MIN_GRAVITY_G ? 0.0f : (float)(STAND_SIGN * mean[STAND_AXIS] / gnorm);

  double m1 = fabs(mean[0]), m2 = fabs(mean[1]), m3 = fabs(mean[2]), t;
  if (m1 < m2) { t = m1; m1 = m2; m2 = t; }
  if (m2 < m3) { t = m2; m2 = m3; m3 = t; }
  if (m1 < m2) { t = m1; m1 = m2; m2 = t; }
  double gap1 = m1 - m2, gap2 = m2 - m3;
  out[27] = (float)gap1;
  out[28] = (float)gap2;
  out[29] = (gap1 + gap2) > 1e-9 ? (float)(gap1 / (gap1 + gap2)) : 0.0f;
}

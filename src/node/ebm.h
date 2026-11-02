#pragma once
/*
 * Inferensi Explainable Boosting Machine (interpret.glassbox EBM) di node.
 * Model = intercept + jumlah skor per term; setiap term (1 fitur atau pasangan 2 fitur)
 * adalah tabel skor per bin. Tabel dibuat oleh tools/export_ebm.py -> ebm_model.h.
 *
 * Bin fitur kontinu: jumlah cut <= x (sama dengan interpret: searchsorted(cuts, x, 'right')),
 * cut sudah dibulatkan ke atas ke float32 sehingga hasilnya identik untuk input float32.
 * Bin "missing" / "unknown" tidak disimpan (fitur node tidak pernah NaN; NaN -> term dilewati).
 * Skor disimpan float32, atau int16 / int8 dengan skala per term (skor = q * scale) supaya
 * tabel lebih kecil; exporter mengecek pengaruhnya terhadap tebakan.
 */

#include <math.h>
#include <stdint.h>

enum : uint8_t
{
  EBM_REGRESSION = 0, // link identity -> satu nilai
  EBM_BINARY = 1,     // link logit    -> 2 kelas
  EBM_MULTICLASS = 2  // link mlogit   -> softmax nOut kelas
};

#define EBM_MAX_OUT 8

struct EbmCuts
{
  const float *v; // cut naik
  uint16_t n;
};

struct EbmTerm
{
  uint8_t nFeat;    // 1 atau 2
  uint8_t feat[2];  // index fitur (urutan har_feature_names)
  uint16_t cuts[2]; // index ke EbmModel.cuts
  uint32_t off;     // offset tabel skor (elemen) di EbmModel.scores, layout [bin0][bin1][out]
  float scale;      // skor = nilai tabel x scale (1 untuk float32)
};

struct EbmModel
{
  uint8_t kind;
  uint8_t nOut; // skor per bin: 1 (regresi / biner) atau jumlah kelas
  uint16_t nTerms;
  const float *intercept; // nOut
  const EbmTerm *terms;
  const EbmCuts *cuts;
  uint8_t scoreBits;  // 32 = float, 16 = int16, 8 = int8
  const void *scores;
};

static inline uint16_t ebmBin(const EbmCuts &c, float x)
{
  uint16_t lo = 0, hi = c.n;
  while (lo < hi)
  {
    uint16_t mid = (uint16_t)((lo + hi) / 2);
    if (x >= c.v[mid])
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

// Skor mentah (sebelum link), out[0..nOut-1]
static inline void ebmScores(const EbmModel &m, const float *x, double *out)
{
  for (uint8_t k = 0; k < m.nOut; k++)
    out[k] = m.intercept[k];
  for (uint16_t i = 0; i < m.nTerms; i++)
  {
    const EbmTerm &t = m.terms[i];
    float v0 = x[t.feat[0]];
    if (isnan(v0))
      continue;
    uint32_t idx = ebmBin(m.cuts[t.cuts[0]], v0);
    if (t.nFeat == 2)
    {
      float v1 = x[t.feat[1]];
      if (isnan(v1))
        continue;
      const EbmCuts &c1 = m.cuts[t.cuts[1]];
      idx = idx * (uint32_t)(c1.n + 1) + ebmBin(c1, v1);
    }
    uint32_t at = t.off + idx * m.nOut;
    if (m.scoreBits == 16)
    {
      const int16_t *s = (const int16_t *)m.scores + at;
      for (uint8_t k = 0; k < m.nOut; k++)
        out[k] += (double)s[k] * t.scale;
    }
    else if (m.scoreBits == 8)
    {
      const int8_t *s = (const int8_t *)m.scores + at;
      for (uint8_t k = 0; k < m.nOut; k++)
        out[k] += (double)s[k] * t.scale;
    }
    else
    {
      const float *s = (const float *)m.scores + at;
      for (uint8_t k = 0; k < m.nOut; k++)
        out[k] += s[k];
    }
  }
}

// Klasifikasi: probabilitas per kelas model (biner: 2, multikelas: nOut). Return jumlah kelas.
static inline uint8_t ebmPredictProba(const EbmModel &m, const float *x, float *prob)
{
  double s[EBM_MAX_OUT];
  ebmScores(m, x, s);
  if (m.kind == EBM_BINARY)
  {
    double p1 = 1.0 / (1.0 + exp(-s[0]));
    prob[0] = (float)(1.0 - p1);
    prob[1] = (float)p1;
    return 2;
  }
  double mx = s[0], sum = 0;
  for (uint8_t k = 1; k < m.nOut; k++)
    if (s[k] > mx)
      mx = s[k];
  for (uint8_t k = 0; k < m.nOut; k++)
  {
    s[k] = exp(s[k] - mx);
    sum += s[k];
  }
  for (uint8_t k = 0; k < m.nOut; k++)
    prob[k] = (float)(s[k] / sum);
  return m.nOut;
}

// Regresi: nilai prediksi (satuan target saat training)
static inline float ebmPredict(const EbmModel &m, const float *x)
{
  double s[EBM_MAX_OUT];
  ebmScores(m, x, s);
  return (float)s[0];
}

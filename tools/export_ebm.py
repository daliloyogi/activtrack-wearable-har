"""
Export the node's activity model (and optional pace model) to src/node/ebm_model.h.

Model input = the 30 HAR features of src/node/har_features.c (har_extract_features),
computed on the node over EBM_WIN_SAMPLES samples at EBM_RATE_HZ (default 20 samples at
40 Hz = 0.5 s), a new window every EBM_HOP_SAMPLES samples (default 20: back to back, so
PROD mode votes over 10 windows per 5 s report). Feature names/order are
har_feature_names (the same names as app/core/features.py, but the HAR formulas).

    python tools/export_ebm.py --activity models/ebm_model.pkl        # dari interpret
    python tools/export_ebm.py --from-c tools/har_reference/har_ebm_model.c   # dari export_cpp.py
    python tools/export_ebm.py --activity act.pkl --pace pace.pkl --pace-unit s_per_km
    python tools/export_ebm.py --empty                                  # header tanpa model

Then build & upload node1 and node2.

Classes: names (Sitting, Standing, ...) map by name. Integer classes are taken as the codes
of the current app (app/core/labels.py: Sitting=0, Standing=1, Walking=2, Running=3) unless
--class-order gives their names, e.g. for data recorded with the OLD app order:
    --class-order Standing,Sitting,Walking,Running
The pace model is a regressor; --pace-unit tells what it predicts.

The output is compacted (unused missing/unknown bins dropped, equal neighbouring bins
merged) and the scores are stored as int16 with a scale per term (--scores; int16 changes
no predicted class in practice, int8 is smaller but changes a few). It is checked against
the source model on random inputs, and carries test vectors that the node checks at boot
(serial monitor: "self-test OK").
Default EBM settings give tables of several MB (too big); for new models train with e.g.
    ExplainableBoostingClassifier(max_bins=128, max_interaction_bins=16, interactions=10)
--activity/--pace need `interpret` (to unpickle) and pandas; --from-c only numpy.
"""

import argparse
import datetime
import os
import pickle
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from app.core.features import FEATURE_COLUMNS, FEAT_COUNT  # noqa: E402  (nama = har_feature_names)
from app.core.labels import ACTIVITIES  # noqa: E402

OUT_DEFAULT = os.path.join(ROOT, "src", "node", "ebm_model.h")
FLASH_BUDGET = 2_600_000        # byte; partisi node huge_app = 3 MB (firmware sendiri ~0.3 MB)
KIND = {"identity": 0, "logit": 1, "mlogit": 2}   # EBM_REGRESSION / EBM_BINARY / EBM_MULTICLASS
PACE_UNITS = {"s_per_km": 0, "min_per_km": 1, "kmh": 2}
SCORE_BITS = {"float": 32, "int16": 16, "int8": 8}
# selisih maks yang diterima vs model sumber: probabilitas (klasifikasi) per format skor
PROB_TOL = {32: 1e-4, 16: 1e-3, 8: 5e-2}
N_TEST = 4
CLASS_ORDER = None      # --class-order: nama aktivitas untuk kelas integer 0, 1, 2, ...


# ── helpers ──────────────────────────────────────────────────────────────────

def load_model(path):
    # SECURITY: joblib/pickle run arbitrary code while loading. Only open model files you
    # trained yourself or got from a source you trust.
    try:
        import joblib
        return joblib.load(path)
    except ImportError:
        with open(path, "rb") as f:
            return pickle.load(f)


def feature_map(names) -> list:
    """Index in the 30-feature vector for every model feature."""
    names = [str(n) for n in names]
    if all(n in FEATURE_COLUMNS for n in names):
        return [FEATURE_COLUMNS.index(n) for n in names]
    if len(names) == FEAT_COUNT and all(n.startswith("feature_") for n in names):
        print("  ! nama fitur generik (dilatih dari array tanpa nama): dianggap urutan har_feature_names")
        return list(range(FEAT_COUNT))
    unknown = [n for n in names if n not in FEATURE_COLUMNS]
    sys.exit(f"Fitur model tidak dikenal node: {unknown}\nNama yang dikenal: {FEATURE_COLUMNS}")


def class_codes(classes) -> list:
    codes = []
    lower = [a.lower() for a in ACTIVITIES]
    for c in classes:
        if isinstance(c, (bool, np.bool_)):
            sys.exit(f"Kelas tidak didukung: {c!r}")
        idx = None
        if isinstance(c, (int, np.integer)) or (isinstance(c, (float, np.floating)) and float(c).is_integer()):
            idx = int(c)
        elif isinstance(c, str) and c.strip().isdigit():
            idx = int(c)
        if idx is not None and CLASS_ORDER:
            if not 0 <= idx < len(CLASS_ORDER):
                sys.exit(f"Kelas {idx} tidak ada di --class-order {CLASS_ORDER}")
            name = CLASS_ORDER[idx].strip().lower()
            if name not in lower:
                sys.exit(f"--class-order: '{CLASS_ORDER[idx]}' bukan aktivitas {ACTIVITIES}")
            code = lower.index(name)
        elif idx is not None:
            code = idx
        elif isinstance(c, str) and c.strip().lower() in lower:
            code = lower.index(c.strip().lower())
        else:
            sys.exit(f"Kelas {c!r} tidak bisa dipetakan ke aktivitas {ACTIVITIES} (atau kode 0..3)")
        if not 0 <= code < len(ACTIVITIES):
            sys.exit(f"Kode kelas {code} di luar 0..{len(ACTIVITIES) - 1}")
        codes.append(code)
    if len(set(codes)) != len(codes):
        sys.exit(f"Kelas ganda setelah dipetakan: {classes}")
    return codes


def cuts_f32(c) -> np.ndarray:
    """Round cut points UP to float32: for a float32 x, x >= c  <=>  x >= ceil_f32(c).
    Cuts that already are float32 (C export) stay unchanged."""
    c = np.asarray(c, dtype=np.float64)
    c32 = c.astype(np.float32)
    low = c32.astype(np.float64) < c
    c32[low] = np.nextafter(c32[low], np.float32(np.inf))
    return c32


def merge_equal_bins(cuts: np.ndarray, scores: np.ndarray):
    """Main term: merge neighbouring bins with identical (float32) scores. Lossless."""
    keep = [0] + [i for i in range(1, len(scores)) if not np.array_equal(scores[i], scores[i - 1])]
    return cuts[[i - 1 for i in keep[1:]]], scores[keep]


def link_predict(kind: int, s: np.ndarray) -> np.ndarray:
    """Raw scores (n, n_out) -> value (regression) or class probabilities."""
    if kind == 0:
        return s[:, 0]
    if kind == 1:
        p1 = 1.0 / (1.0 + np.exp(-s[:, 0]))
        return np.c_[1 - p1, p1]
    e = np.exp(s - s.max(axis=1, keepdims=True))
    return e / e.sum(axis=1, keepdims=True)


# ── exported model ───────────────────────────────────────────────────────────

class Exported:
    """raw_terms: [(feature idx list, cuts list, scores [b0,(b1),n_out] of the regular bins)]"""

    def __init__(self, role, link, classes, intercept, model_features, raw_terms, source):
        self.role, self.link, self.source = role, link, source
        if link not in KIND:
            sys.exit(f"Link EBM '{link}' tidak didukung (hanya identity / logit / mlogit)")
        if role == "activity" and link == "identity":
            sys.exit("model aktivitas harus classifier")
        if role == "pace" and link != "identity":
            sys.exit("--pace harus ExplainableBoostingRegressor")
        self.kind = KIND[link]
        self.classes = class_codes(classes) if link != "identity" else []
        self.n_out = len(self.classes) if link == "mlogit" else 1
        if self.n_out > 8:
            sys.exit("Maksimal 8 kelas (EBM_MAX_OUT)")
        self.intercept = np.atleast_1d(np.asarray(intercept, dtype=np.float64)).astype(np.float32)
        self.model_features = model_features
        self.terms = []
        rows_in = rows_out = 0
        for feats, cuts, sc in raw_terms:
            cuts = [cuts_f32(c) for c in cuts]
            sc = np.asarray(sc, dtype=np.float64)
            if sc.ndim == len(feats):
                sc = sc[..., None]
            assert sc.shape[:len(feats)] == tuple(len(c) + 1 for c in cuts), (sc.shape, [len(c) for c in cuts])
            sc = sc.astype(np.float32)
            rows_in += int(np.prod(sc.shape[:len(feats)]))
            if len(feats) == 1:
                cuts[0], sc = merge_equal_bins(cuts[0], sc)
            rows_out += int(np.prod(sc.shape[:len(feats)]))
            if np.any(sc):                                   # term tanpa pengaruh dibuang
                self.terms.append((list(feats), cuts, sc))
        self.rows = (rows_in, rows_out)
        self.quantize(32)

    def quantize(self, bits: int):
        """Store scores as float32 (32) or intN with one scale per term."""
        self.bits = bits
        self.qterms = []
        for feats, cuts, sc in self.terms:
            if bits == 32:
                self.qterms.append((feats, cuts, sc, np.float32(1.0)))
                continue
            qmax = 2 ** (bits - 1) - 1
            scale = np.float32(np.abs(sc.astype(np.float64)).max() / qmax)
            q = np.clip(np.round(sc.astype(np.float64) / np.float64(scale)), -qmax, qmax)
            self.qterms.append((feats, cuts, q.astype(np.int16 if bits == 16 else np.int8), scale))

    @classmethod
    def from_ebm(cls, ebm, role, source):
        fmap = feature_map(ebm.feature_names_in_)
        types = list(ebm.feature_types_in_)
        link = str(ebm.link_)
        n_out = len(ebm.classes_) if link == "mlogit" else 1
        raw = []
        for tf, sc in zip(ebm.term_features_, ebm.term_scores_):
            if len(tf) > 2:
                sys.exit(f"Term {len(tf)} fitur tidak didukung (maks. pasangan)")
            for f in tf:
                if types[f] != "continuous":
                    sys.exit(f"Fitur '{ebm.feature_names_in_[f]}' bertipe {types[f]}; hanya kontinu")
            cuts = [ebm.bins_[f][min(len(tf), len(ebm.bins_[f])) - 1] for f in tf]
            sc = np.asarray(sc, dtype=np.float64)
            if n_out == 1:
                sc = sc[..., None]
            raw.append(([fmap[f] for f in tf], cuts, sc[tuple(slice(1, -1) for _ in tf)]))
        classes = list(ebm.classes_) if link != "identity" else []
        exp = cls(role, link, classes, ebm.intercept_, [FEATURE_COLUMNS[i] for i in fmap], raw, source)
        exp.input_cuts = {fmap[fi]: np.asarray(ebm.bins_[fi][0], float) for fi in range(len(fmap))}
        exp._ebm = ebm
        return exp

    @classmethod
    def from_c(cls, path):
        """Tables of a har_ebm_model.c written by export_cpp.py (univariate EBM classifier)."""
        src = open(path, encoding="utf-8").read()
        arr = {m.group(2): np.array([float(v.strip().rstrip("fF")) for v in m.group(3).split(",") if v.strip()])
               for m in re.finditer(r"static\s+const\s+(float|unsigned\s+char|unsigned\s+short|int)\s+(\w+)"
                                    r"\s*\[\s*\d*\s*\]\s*=\s*\{(.*?)\};", src, re.S)}
        need = ["har_intercept", "har_term_feature", "har_cut_off", "har_score_off", "har_cuts", "har_scores"]
        missing = [k for k in need if k not in arr]
        if missing:
            sys.exit(f"{path}: array {missing} tidak ditemukan (bukan keluaran export_cpp.py?)")
        intercept = arr["har_intercept"]
        n_out = intercept.size
        m = re.search(r"Classes\s*:\s*\[(.*?)\]", src)
        classes = [int(x) for x in re.findall(r"-?\d+", re.sub(r"np\.\w+\(", "", m.group(1)))] if m else list(range(n_out))
        if len(classes) != n_out:
            sys.exit(f"Jumlah kelas {classes} tidak sama dengan intercept ({n_out})")
        tf, co, so = (arr[k].astype(int) for k in ("har_term_feature", "har_cut_off", "har_score_off"))
        raw, full = [], []
        for t in range(tf.size):
            cuts = arr["har_cuts"][co[t]:co[t + 1]].astype(np.float32)   # literal float32 = nilai asli
            sc = arr["har_scores"][so[t]:so[t + 1]].reshape(-1, n_out)
            if len(sc) != len(cuts) + 3:
                sys.exit(f"term {t}: {len(sc)} baris skor untuk {len(cuts)} cut (harus cut + 3)")
            if sc[0].any() or sc[-1].any():
                print(f"  ! term {t}: bin missing/unknown tidak nol (diabaikan; fitur node tidak pernah NaN)")
            raw.append(([int(tf[t])], [cuts], sc[1:-1]))
            full.append((int(tf[t]), cuts.astype(np.float32), sc))
        link = "mlogit" if n_out > 2 else "logit"
        if n_out == 2:
            sys.exit("Model biner dari C belum didukung")
        feats = sorted({int(f) for f in tf})
        exp = cls("activity", link, classes, intercept, [FEATURE_COLUMNS[i] for i in feats], raw, path)
        exp.input_cuts = {int(tf[t]): arr["har_cuts"][co[t]:co[t + 1]] for t in range(tf.size)}
        exp._c_full = full
        return exp

    # emulation of ebm.h (float32 input, float32 tables, double accumulation)
    def raw_scores(self, x30: np.ndarray) -> np.ndarray:
        x30 = np.asarray(x30, dtype=np.float32)
        out = np.tile(self.intercept.astype(np.float64), (len(x30), 1))
        for feats, cuts, sc, scale in self.qterms:
            idx = tuple(np.searchsorted(c, x30[:, f], side="right") for f, c in zip(feats, cuts))
            v = sc[idx].astype(np.float64)
            out += v if self.bits == 32 else v * np.float64(scale)
        return out

    def predict(self, x30: np.ndarray) -> np.ndarray:
        return link_predict(self.kind, self.raw_scores(x30))

    def reference(self, x30: np.ndarray) -> np.ndarray:
        """Prediction of the SOURCE model (interpret, or the original C tables)."""
        if hasattr(self, "_c_full"):
            x30 = np.asarray(x30, dtype=np.float32)
            s = np.tile(self.intercept.astype(np.float64), (len(x30), 1))
            for f, cuts, sc in self._c_full:          # bin = searchsorted(right) + 1, seperti bin_index()
                s += sc[np.searchsorted(cuts, x30[:, f], side="right") + 1]
            return link_predict(self.kind, s)
        import pandas as pd
        ebm = self._ebm
        cols = [FEATURE_COLUMNS.index(f) for f in self.model_features]
        df = pd.DataFrame(np.asarray(x30, dtype=np.float32)[:, cols].astype(np.float64),
                          columns=[str(c) for c in ebm.feature_names_in_])
        return np.asarray(ebm.predict(df) if self.kind == 0 else ebm.predict_proba(df), dtype=np.float64)

    def size_bytes(self) -> int:
        n = 4 * self.intercept.size
        for _feats, cuts, sc in self.terms:
            n += sc.size * self.bits // 8 + 4 * sum(c.size for c in cuts)
        return n + 16 * len(self.terms)


def random_inputs(input_cuts: dict, n: int, rng) -> np.ndarray:
    """(n, 30) float32 inputs spread over the model's bins (incl. exact cut values)."""
    x = np.zeros((n, FEAT_COUNT), dtype=np.float32)
    for col, cuts in input_cuts.items():
        cuts = np.asarray(cuts, dtype=np.float64)
        if cuts.size == 0:
            x[:, col] = rng.normal(0, 1, n)
            continue
        lo, hi = cuts.min(), cuts.max()
        span = (hi - lo) or 1.0
        v = rng.uniform(lo - 0.1 * span, hi + 0.1 * span, n)
        exact = rng.random(n) < 0.3
        v[exact] = rng.choice(cuts, exact.sum())             # tepat di cut (uji batas bin)
        x[:, col] = v.astype(np.float32)
    return x


def verify(exp: Exported, rng, n=4000):
    x = random_inputs(exp.input_cuts, n, rng)
    ref = exp.reference(x)
    got = exp.predict(x)
    err = float(np.max(np.abs(ref - got)))
    same = float(np.mean(ref.argmax(1) == got.argmax(1))) if exp.kind else 1.0
    return x, ref, err, same


# ── C output ─────────────────────────────────────────────────────────────────

def cf(v) -> str:
    s = "%.9g" % float(v)
    if not any(ch in s for ch in ".eEn"):
        s += ".0"
    return s + "f"


def c_array(name: str, ctype: str, values, per_line: int = 8, fmt=cf) -> str:
    vals = list(values) or [0]
    lines = ["  " + ", ".join(fmt(v) for v in vals[i:i + per_line]) + ","
             for i in range(0, len(vals), per_line)]
    return f"static const {ctype} {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def model_c(exp: Exported, prefix: str, sym: str) -> str:
    index, cuts_data, cuts_desc = {}, [], []

    def cut_id(c):
        key = c.tobytes()
        if key not in index:
            index[key] = len(cuts_desc)
            cuts_desc.append((len(cuts_data), c.size))
            cuts_data.extend(c.tolist())
        return index[key]

    scores, terms = [], []
    for feats, cuts, sc, scale in exp.qterms:
        ids = [cut_id(c) for c in cuts]
        terms.append((len(feats), feats + [0] * (2 - len(feats)), ids + [0] * (2 - len(ids)), len(scores),
                      scale))
        scores.extend(sc.reshape(-1).tolist())
    ctype = {32: "float", 16: "int16_t", 8: "int8_t"}[exp.bits]
    fmt = cf if exp.bits == 32 else (lambda v: str(int(v)))
    out = [f"// {sym}: {len(exp.terms)} term, {len(cuts_desc)} tabel cut, skor {ctype}, "
           f"{exp.size_bytes() / 1024:.1f} KB"]
    out.append(c_array(f"{prefix}_intercept", "float", exp.intercept))
    out.append(c_array(f"{prefix}_cuts_data", "float", cuts_data))
    out.append(f"static const EbmCuts {prefix}_cuts[] = {{\n" +
               "\n".join(f"  {{{prefix}_cuts_data + {o}, {n}}}," for o, n in cuts_desc) + "\n};\n")
    out.append(c_array(f"{prefix}_scores", ctype, scores, per_line=8 if exp.bits == 32 else 16, fmt=fmt))
    out.append(f"static const EbmTerm {prefix}_terms[] = {{\n" +
               "\n".join(f"  {{{nf}, {{{f[0]}, {f[1]}}}, {{{c[0]}, {c[1]}}}, {off}, {cf(scale)}}},"
                         for nf, f, c, off, scale in terms) + "\n};\n")
    out.append(f"static const EbmModel {sym} = {{{exp.kind}, {exp.n_out}, {len(exp.terms)}, "
               f"{prefix}_intercept, {prefix}_terms, {prefix}_cuts, {exp.bits}, {prefix}_scores}};\n")
    return "\n".join(out)


def header(args, act, pace, tests) -> str:
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    h = ["#pragma once",
         "// DIHASILKAN OTOMATIS oleh tools/export_ebm.py - jangan diedit manual.",
         f"// Dibuat: {now}",
         "// Input model: har_extract_features() (src/node/har_features.c)."]
    if act:
        h.append(f"// Aktivitas: {os.path.basename(act.source)}  kelas -> "
                 f"{[ACTIVITIES[c] for c in act.classes]}")
        h.append(f"//   fitur: {', '.join(act.model_features)}")
    if pace:
        h.append(f"// Pace: {os.path.basename(pace.source)}  (target {args.pace_unit})")
    h += ["", '#include "ebm.h"', "",
          f"#define EBM_HAS_ACTIVITY {1 if act else 0}",
          f"#define EBM_HAS_PACE {1 if pace else 0}",
          f"#define EBM_RATE_HZ {args.rate_hz}      // sample rate data training (mode PROD memakai ini)",
          f"#define EBM_WIN_SAMPLES {args.win_samples}  // sampel per jendela model (= HAR_WINDOW_SIZE)",
          f"#define EBM_HOP_SAMPLES {args.hop_samples}  // jendela baru setiap ... sampel",
          f"#define EBM_PACE_UNIT {PACE_UNITS[args.pace_unit]}     // 0 = s/km, 1 = min/km, 2 = km/h", ""]
    if act:
        h.append(f"#define EBM_ACT_CLASSES {len(act.classes)}")
        h.append(c_array("EBM_ACT_CODE", "uint8_t", act.classes, fmt=str))
        h.append(model_c(act, "ebm_act", "EBM_ACTIVITY"))
    if pace:
        h.append(model_c(pace, "ebm_pace", "EBM_PACE"))
    if act or pace:
        x, pa, pr = tests
        h.append("// Self-test saat boot: input fitur dan hasil yang harus dihitung node (tabel ini)")
        h.append(f"#define EBM_TEST_N {len(x)}")
        h.append(f"static const float EBM_TEST_X[EBM_TEST_N][{FEAT_COUNT}] = {{\n" +
                 "\n".join("  {" + ", ".join(cf(v) for v in row) + "}," for row in x) + "\n};\n")
        if act:
            h.append("static const float EBM_TEST_ACT_PROB[EBM_TEST_N][EBM_ACT_CLASSES] = {\n" +
                     "\n".join("  {" + ", ".join(cf(v) for v in row) + "}," for row in pa) + "\n};\n")
        if pace:
            h.append(c_array("EBM_TEST_PACE_RAW", "float", pr))
    return "\n".join(h) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--activity", help="ExplainableBoostingClassifier (.pkl / .joblib)")
    src.add_argument("--from-c", help="har_ebm_model.c hasil export_cpp.py")
    ap.add_argument("--pace", help="ExplainableBoostingRegressor untuk pace (opsional)")
    ap.add_argument("--pace-unit", choices=sorted(PACE_UNITS), default="s_per_km",
                    help="target model pace (default s_per_km)")
    ap.add_argument("--rate-hz", type=int, default=40, help="sample rate data training (default 40)")
    ap.add_argument("--win-samples", type=int, default=20, help="sampel per jendela (default 20)")
    ap.add_argument("--hop-samples", type=int, default=0, help="langkah jendela (default = panjang jendela)")
    ap.add_argument("--class-order",
                    help="nama untuk kelas integer model, mis. Standing,Sitting,Walking,Running "
                         "(data app versi lama). Default: kode app sekarang (Sitting=0)")
    ap.add_argument("--scores", choices=sorted(SCORE_BITS), default="int16",
                    help="format tabel skor (default int16: -40%% ukuran, kelas tidak berubah)")
    ap.add_argument("--empty", action="store_true", help="header tanpa model")
    ap.add_argument("-o", "--out", default=OUT_DEFAULT)
    args = ap.parse_args()
    if not args.empty and not (args.activity or args.from_c or args.pace):
        ap.error("beri --activity / --from-c dan/atau --pace, atau --empty")
    if args.rate_hz not in (10, 20, 40, 50):
        print(f"  ! rate {args.rate_hz} Hz bukan pilihan ground station (10/20/40/50)")
    args.hop_samples = args.hop_samples or args.win_samples
    global CLASS_ORDER
    CLASS_ORDER = [c for c in args.class_order.split(",") if c.strip()] if args.class_order else None

    rng = np.random.default_rng(1)
    act = pace = None
    tests = None
    if not args.empty:
        xs = []
        sources = [("activity", args.activity), ("from_c", args.from_c), ("pace", args.pace)]
        for role, path in sources:
            if not path:
                continue
            if role == "from_c":
                exp = Exported.from_c(path)
            else:
                exp = Exported.from_ebm(load_model(path), role, path)
            exp.quantize(SCORE_BITS[args.scores])
            x, ref, err, same = verify(exp, rng)
            rin, rout = exp.rows
            print(f"{exp.role}: {os.path.basename(path)}  link={exp.link}  term={len(exp.terms)}  "
                  f"fitur={len(exp.model_features)}  skor {args.scores}  {exp.size_bytes() / 1024:.1f} KB  "
                  f"(bin {rin} -> {rout})")
            print(f"  cek vs model sumber ({len(x)} input acak): selisih maks {err:.2e}"
                  + (f", kelas sama {same * 100:.2f}%" if exp.kind else ""))
            tol = (PROB_TOL[exp.bits] if exp.kind
                   else 1e-2 * max(1.0, float(np.abs(ref).max()) / 100) * (1 if exp.bits == 32 else 100))
            if err > tol:
                sys.exit("  ! selisih terlalu besar - model tidak diekspor")
            if exp.role == "activity":
                act = exp
            else:
                pace = exp
            xs.append(x[:N_TEST])
        x = xs[0]
        pa = act.predict(x) if act else np.zeros((len(x), 0))    # node harus menghitung persis ini
        pr = pace.predict(x) if pace else np.zeros(len(x))
        tests = (x, pa, pr)
        total = (act.size_bytes() if act else 0) + (pace.size_bytes() if pace else 0)
        print(f"total tabel model: {total / 1024:.1f} KB (batas {FLASH_BUDGET / 1024:.0f} KB)")
        if total > FLASH_BUDGET:
            sys.exit("  ! terlalu besar untuk flash node. Latih ulang dengan tabel lebih kecil, mis.\n"
                     "    ExplainableBoostingClassifier(max_bins=128, max_interaction_bins=16, interactions=10)")

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(header(args, act, pace, tests))
    try:
        shown = os.path.relpath(args.out, ROOT)
    except ValueError:          # drive lain (Windows)
        shown = args.out
    print(f"ditulis: {shown}  -> build & upload node1 dan node2")


if __name__ == "__main__":
    main()

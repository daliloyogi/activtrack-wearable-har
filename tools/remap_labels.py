"""
Tukar kode label Standing <-> Sitting (0 <-> 1) di file CSV lama.

Data yang direkam dengan app versi lama memakai Standing=0, Sitting=1. App sekarang memakai
Sitting=0, Standing=1 (app/core/labels.py). Jalankan skrip ini pada data lama sebelum
dicampur dengan data baru. Walking (2) dan Running (3) tidak berubah; label berupa nama
(mis. "Standing") juga tidak diubah karena tidak ambigu.

    python tools/remap_labels.py "data_lama/*.csv"                # -> *_remap.csv di folder yang sama
    python tools/remap_labels.py "data_lama/*.csv" --out data_v2   # -> folder lain, nama sama

Kolom label dicari otomatis: activityMode, activity_mode, activity, label (atau --column).
File asli tidak pernah ditimpa.
"""

import argparse
import csv
import glob
import os
import sys

LABEL_COLUMNS = ("activityMode", "activity_mode", "activity", "label")
SWAP = {"0": "1", "1": "0", "0.0": "1", "1.0": "0"}


def remap_file(path: str, out_path: str, column: str = None) -> tuple:
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.reader(f))
    if not rows:
        return 0, 0
    header = rows[0]
    names = [column] if column else LABEL_COLUMNS
    col = next((header.index(c) for c in names if c in header), None)
    if col is None:
        sys.exit(f"{path}: kolom label {list(names)} tidak ditemukan (header: {header})")
    swapped = 0
    for row in rows[1:]:
        if col < len(row) and row[col].strip() in SWAP:
            row[col] = SWAP[row[col].strip()]
            swapped += 1
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        csv.writer(f).writerows(rows)
    return swapped, len(rows) - 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="file CSV atau pola, mis. \"data/*.csv\"")
    ap.add_argument("--out", help="folder hasil (default: folder yang sama, akhiran _remap)")
    ap.add_argument("--column", help="nama kolom label (default: dicari otomatis)")
    args = ap.parse_args()

    paths = sorted({p for pat in args.files for p in (glob.glob(pat) or [pat])})
    if args.out:
        os.makedirs(args.out, exist_ok=True)
    for path in paths:
        if not os.path.isfile(path):
            print(f"lewati (tidak ada): {path}")
            continue
        if args.out:
            out = os.path.join(args.out, os.path.basename(path))
        else:
            base, ext = os.path.splitext(path)
            out = base + "_remap" + (ext or ".csv")
        if os.path.abspath(out) == os.path.abspath(path):
            sys.exit(f"{path}: hasil akan menimpa file asli; pilih --out folder lain")
        swapped, total = remap_file(path, out, args.column)
        print(f"{path} -> {out}: {swapped} dari {total} baris ditukar (0<->1)")


if __name__ == "__main__":
    main()

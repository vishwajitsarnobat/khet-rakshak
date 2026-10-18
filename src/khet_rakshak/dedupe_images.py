# KHET-RAKSHAK: remove duplicate, near-duplicate, broken and tiny images from ./dataset
# Run:  pip install pillow
#       python dedupe_images.py           (preview only, deletes nothing)
#       python dedupe_images.py --delete  (actually deletes)
#
# - Exact duplicates: same file bytes (MD5)
# - Near duplicates: same picture resized/recompressed (perceptual hash, distance <= THRESHOLD)
# - Checks across ALL label folders: if the same image sits in two labels, both copies are removed,
#   because it would confuse the model.
import hashlib
import os
import sys
from collections import defaultdict

from PIL import Image


def dhash(im, size=8):
    """Perceptual 'difference hash': 64-bit number that stays the same when an image is resized or recompressed."""
    g = im.convert("L").resize((size + 1, size), Image.LANCZOS)
    px = list(g.tobytes())
    bits = 0
    for row in range(size):
        for col in range(size):
            left = px[row * (size + 1) + col]
            right = px[row * (size + 1) + col + 1]
            bits = (bits << 1) | (1 if left > right else 0)
    return bits


def distance(a, b):
    return (a ^ b).bit_count()


ROOT = "dataset"
THRESHOLD = 5  # 0 = identical look; raise to 8 to catch more, lower to 3 to be stricter
MIN_SIZE = 64  # images smaller than 64 px on a side are useless at 96x96
DELETE = "--delete" in sys.argv

exts = (".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif")
files = []
for label in sorted(os.listdir(ROOT)):
    d = os.path.join(ROOT, label)
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            if f.lower().endswith(exts):
                files.append((label, os.path.join(d, f)))

to_delete = {}  # path -> reason
seen_md5 = {}
kept = []  # (label, path, phash)

for label, path in files:
    try:
        with open(path, "rb") as fh:
            md5 = hashlib.md5(fh.read()).hexdigest()
        with Image.open(path) as im:
            im.load()
            w, h = im.size
            ph = dhash(im)
    except Exception:
        to_delete[path] = "broken file"
        continue
    if w < MIN_SIZE or h < MIN_SIZE:
        to_delete[path] = f"too small ({w}x{h})"
        continue
    if md5 in seen_md5:
        first_label, first_path = seen_md5[md5]
        to_delete[path] = f"exact copy of {first_path}"
        if first_label != label:
            to_delete[first_path] = f"same image also in '{label}'"
        continue
    seen_md5[md5] = (label, path)

    match = None
    for k_label, k_path, k_ph in kept:
        if distance(ph, k_ph) <= THRESHOLD:
            match = (k_label, k_path)
            break
    if match:
        to_delete[path] = f"near copy of {match[1]}"
        if match[0] != label:
            to_delete[match[1]] = f"same image also in '{label}'"
        continue
    kept.append((label, path, ph))

per_label = defaultdict(int)
for p, why in sorted(to_delete.items()):
    print(f"{'DELETE' if DELETE else 'would delete'}: {p}  [{why}]")
    per_label[p.split(os.sep)[1]] += 1
    if DELETE and os.path.exists(p):
        os.remove(p)

print("\nSummary")
totals = defaultdict(int)
for label, _ in files:
    totals[label] += 1
for label in sorted(totals):
    print(
        f"  {label:10s} {totals[label]:4d} found, {per_label[label]:3d} removed, {totals[label] - per_label[label]:4d} left"
    )
if not DELETE:
    print("\nPreview only. Run again with --delete to remove these files.")

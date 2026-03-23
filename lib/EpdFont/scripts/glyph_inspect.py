#!/usr/bin/env python3
"""Dump glyph bitmap values at each quantization stage, or compare 1x vs 2x
supersampling metrics.

Uses the same parameters as fontconvert.py (150 DPI, FT_LOAD_RENDER) and the
same quantization math so the values match the generated font headers exactly.
"""
import freetype
import argparse
import os
import sys
import math

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FONT = os.path.join(SCRIPT_DIR, "..", "builtinFonts", "source", "Bookerly", "Bookerly-Regular.ttf")

parser = argparse.ArgumentParser(description="Inspect glyph bitmap quantization stages.")
parser.add_argument("--font", default=DEFAULT_FONT, help="Path to font file.")
parser.add_argument("--size", type=int, default=12, help="Font size in pt (default: 12).")
parser.add_argument("--force-autohint", action="store_true")
parser.add_argument("--light-autohint", action="store_true")
parser.add_argument("--2bit-thresholds", dest="thresholds_2bit", default="64,128,192",
                    help="1-3 comma-separated 8-bit boundaries for 2-bit quantization "
                         "(level 0/1, 1/2, 2/3). Omitted values keep defaults: 64,128,192.")
parser.add_argument("--compare-2x", action="store_true",
                    help="Compare 1x vs 2x ppem metrics for all ASCII + extended Latin glyphs.")
parser.add_argument("--supersample", nargs="?", const="2", default=None,
                    metavar="N",
                    help="Show Nx supersampled 2-bit result alongside the standard output. "
                         "Default factor is 2 if no value given.")
parser.add_argument("--downsample-filter", dest="downsample_filter", default="box",
                    choices=["box", "gaussian", "lanczos", "max"],
                    help="Downsampling filter for --supersample (default: box).")
parser.add_argument("glyphs", nargs="*", default=["k", "l"],
                    help="Characters to inspect (default: k l).")
args = parser.parse_args()

if args.supersample is not None:
    try:
        args.supersample = int(args.supersample)
    except ValueError:
        args.glyphs.insert(0, args.supersample)
        args.supersample = 2

_defaults = [64, 128, 192]
_parts = [int(x) for x in args.thresholds_2bit.split(",")]
t1, t2, t3 = (_parts + _defaults[len(_parts):])[0:3]

load_flags = freetype.FT_LOAD_RENDER
if args.force_autohint:
    load_flags |= freetype.FT_LOAD_FORCE_AUTOHINT
elif args.light_autohint:
    load_flags |= freetype.FT_LOAD_TARGET_LIGHT | freetype.FT_LOAD_FORCE_AUTOHINT

SHADE_2BIT = [" .", " ░", " ▓", " █"]
SHADE_2BIT = [" .", " |", " ░", " █"]
DIFF_SHADE = {
    -3: " ⊖", -2: " ⊖", -1: " ⊖",
     0: " ·",
    +1: " ⊕", +2: " ⊕", +3: " ⊕",
}

def quantize_2bit(v):
    """8-bit → 2-bit using configurable thresholds (matches fontconvert.py)."""
    if v >= t3: return 3
    if v >= t2: return 2
    if v >= t1: return 1
    return 0

def print_grid(label, rows, width, col_width):
    print(f"  {label}:")
    col_hdr = "     " + "".join(f"{x:>{col_width}}" for x in range(width))
    print(col_hdr)
    for y, row in enumerate(rows):
        cells = "".join(f"{v:>{col_width}}" for v in row)
        print(f"  {y:2d} |{cells}")
    print()

def print_2bit_grid(label, rows2, width):
    print(f"  {label}:")
    col_hdr = "     " + "".join(f"{x:>3}" for x in range(width))
    print(col_hdr)
    for y, row in enumerate(rows2):
        shaded = "".join(SHADE_2BIT[v] for v in row)
        nums   = "".join(f"{v:>3}" for v in row)
        print(f"  {y:2d} |{nums}   {shaded}")
    print()

def print_diff_grid(label, rows_std, rows_ss, width):
    print(f"  {label}:")
    col_hdr = "     " + "".join(f"{x:>3}" for x in range(width))
    print(col_hdr)
    changed = 0
    total = 0
    for y, (row_a, row_b) in enumerate(zip(rows_std, rows_ss)):
        diffs = [b - a for a, b in zip(row_a, row_b)]
        nums = "".join(f"{d:>+3d}" if d != 0 else "  ·" for d in diffs)
        shaded = "".join(DIFF_SHADE[d] for d in diffs)
        print(f"  {y:2d} |{nums}   {shaded}")
        changed += sum(1 for d in diffs if d != 0)
        total += len(diffs)
    print(f"\n  {changed}/{total} pixels changed")
    print()

def build_kernel(N, filter_name):
    """Build an NxN weight kernel for the given filter.

    Returns a flat list of N*N float weights normalized so that
    sum(w) * 255 == 255 * N * N  (i.e. weights sum to N*N).
    For 'box' and 'max', returns None (handled as special cases).
    """
    if filter_name in ("box", "max"):
        return None

    center = (N - 1) / 2.0
    raw = []

    if filter_name == "gaussian":
        sigma = N / 3.0
        for r in range(N):
            for c in range(N):
                d2 = (r - center) ** 2 + (c - center) ** 2
                raw.append(math.exp(-d2 / (2 * sigma * sigma)))

    elif filter_name == "lanczos":
        a = 2.0
        for r in range(N):
            for c in range(N):
                dy = (r - center) / (N / 2.0)
                dx = (c - center) / (N / 2.0)
                d = math.sqrt(dx * dx + dy * dy)
                if d < 1e-9:
                    raw.append(1.0)
                elif d >= a:
                    raw.append(0.0)
                else:
                    raw.append((math.sin(math.pi * d) / (math.pi * d)) *
                               (math.sin(math.pi * d / a) / (math.pi * d / a)))

    raw_sum = sum(raw)
    scale = N * N / raw_sum
    return [w * scale for w in raw]


def supersample_glyph(face_1x, ch, factor, filter_name="box"):
    """Render at Nx, align to 1x grid, downsample NxN blocks to 2-bit.

    Returns (rows_ss, sums, max_sum, info_nx, raw8_nx) where:
      rows_ss  — 2D list of 2-bit quantized values
      sums     — 2D list of filtered block values (0 – 255*N*N)
      max_sum  — theoretical maximum (255 * N * N)
      info_nx  — tuple (wn, hn, leftn, topn, ox, oy, factor)
      raw8_nx  — 2D list of 8-bit values from the Nx raster
    """
    N = factor
    kernel = build_kernel(N, filter_name)

    bmp_1x = face_1x.glyph.bitmap
    w1 = bmp_1x.width
    h1 = bmp_1x.rows
    left1 = face_1x.glyph.bitmap_left
    top1 = face_1x.glyph.bitmap_top

    face_nx = freetype.Face(args.font)
    face_nx.set_char_size(args.size << 6, args.size << 6, 150 * N, 150 * N)
    face_nx.load_char(ch, load_flags)
    bmp_nx = face_nx.glyph.bitmap
    wn = bmp_nx.width
    hn = bmp_nx.rows
    leftn = face_nx.glyph.bitmap_left
    topn = face_nx.glyph.bitmap_top

    buf_nx = list(bmp_nx.buffer)

    raw8_nx = []
    for y in range(hn):
        raw8_nx.append([buf_nx[y * wn + x] for x in range(wn)])

    ox = N * left1 - leftn
    oy = topn - N * top1

    def get_nx(r, c):
        if 0 <= r < hn and 0 <= c < wn:
            return buf_nx[r * wn + c]
        return 0

    max_sum = 255 * N * N
    scale = N * N
    st1 = t1 * scale
    st2 = t2 * scale
    st3 = t3 * scale

    rows_ss = []
    sums = []
    for y in range(h1):
        row = []
        sum_row = []
        for x in range(w1):
            r0 = oy + y * N
            c0 = ox + x * N

            if filter_name == "max":
                mv = max(get_nx(r0 + dr, c0 + dc)
                         for dr in range(N) for dc in range(N))
                s = mv * scale
            elif kernel is not None:
                s = int(round(sum(get_nx(r0 + dr, c0 + dc) * kernel[dr * N + dc]
                                  for dr in range(N) for dc in range(N))))
            else:
                s = sum(get_nx(r0 + dr, c0 + dc)
                        for dr in range(N) for dc in range(N))

            sum_row.append(s)
            if s >= st3:     row.append(3)
            elif s >= st2:   row.append(2)
            elif s >= st1:   row.append(1)
            else:            row.append(0)
        sums.append(sum_row)
        rows_ss.append(row)

    return rows_ss, sums, max_sum, (wn, hn, leftn, topn, ox, oy, N), raw8_nx

def inspect_glyph(ch):
    face = freetype.Face(args.font)
    face.set_char_size(args.size << 6, args.size << 6, 150, 150)
    cp = ord(ch)
    face.load_char(ch, load_flags)
    bmp = face.glyph.bitmap
    left = face.glyph.bitmap_left
    top = face.glyph.bitmap_top
    w, h = bmp.width, bmp.rows

    print(f"=== '{ch}' (U+{cp:04X}) ===")
    print(f"  bitmap: {w}x{h}  left: {left}  top: {top}")
    print()

    raw8 = []
    for y in range(h):
        row = [bmp.buffer[y * w + x] for x in range(w)]
        raw8.append(row)

    rows2 = [[quantize_2bit(v) for v in row] for row in raw8]

    print_grid("8-bit (0-255)", raw8, w, 4)
    print_2bit_grid(f"2-bit standard  [thresholds: {t1},{t2},{t3}]", rows2, w)

    if args.supersample:
        N = args.supersample
        filt = args.downsample_filter
        rows_ss, sums, max_sum, (wn, hn, leftn, topn, ox, oy, _), raw8_nx = \
            supersample_glyph(face, ch, N, filt)
        scale = N * N
        st1, st2, st3 = t1 * scale, t2 * scale, t3 * scale
        sum_digits = len(str(max_sum)) + 1
        print(f"  {N}x bitmap: {wn}x{hn}  left: {leftn}  top: {topn}  "
              f"align offset: ox={ox} oy={oy}  filter: {filt}")
        print()
        print_grid(f"{N}x 8-bit raster (0-255)", raw8_nx, wn, 4)
        print_grid(f"{N}x{N} block values (0-{max_sum})  "
                   f"[{filt} filter, thresholds: ≥{st1}→1, ≥{st2}→2, ≥{st3}→3  "
                   f"(scaled from {t1},{t2},{t3})]",
                   sums, w, sum_digits)
        print_2bit_grid(f"2-bit supersampled  [{N}x {filt}]", rows_ss, w)
        print_diff_grid("diff (supersample − standard)", rows2, rows_ss, w)

def fp4_from_ft16_16(val):
    return (val + (1 << 11)) >> 12

def compare_2x():
    """Compare glyph metrics at 1x vs 2x ppem to quantify supersampling impact."""
    face1x = freetype.Face(args.font)
    face2x = freetype.Face(args.font)
    size = args.size
    face1x.set_char_size(size << 6, size << 6, 150, 150)
    face2x.set_char_size(size << 6, size << 6, 300, 300)

    ppem_1x = size * 150.0 / 72.0
    ppem_2x = size * 300.0 / 72.0
    print(f"Comparing 1x (ppem={ppem_1x:.1f}, 150 DPI) vs 2x (ppem={ppem_2x:.1f}, 300 DPI)")
    print(f"Font: {args.font}")
    print(f"Size: {size}pt")
    print()

    asc1 = face1x.size.ascender
    asc2 = face2x.size.ascender
    desc1 = face1x.size.descender
    desc2 = face2x.size.descender
    h1 = face1x.size.height
    h2 = face2x.size.height

    def nc(v): return int(math.ceil(v / 64))
    def nf(v): return int(math.floor(v / 64))

    print("Global metrics (26.6 fixed-point, then norm_ceil/floor to integer):")
    print(f"  {'metric':<12} {'1x raw':>8} {'2x raw':>8} {'1x int':>7} {'2x/2':>7} {'delta':>6}")
    print(f"  {'ascender':<12} {asc1:>8} {asc2:>8} {nc(asc1):>7} {nc(asc2)/2:>7.1f} {nc(asc2)/2 - nc(asc1):>+6.1f}")
    print(f"  {'descender':<12} {desc1:>8} {desc2:>8} {nf(desc1):>7} {nf(desc2)/2:>7.1f} {nf(desc2)/2 - nf(desc1):>+6.1f}")
    print(f"  {'height':<12} {h1:>8} {h2:>8} {nc(h1):>7} {nc(h2)/2:>7.1f} {nc(h2)/2 - nc(h1):>+6.1f}")
    print()

    test_ranges = [(0x20, 0x7E), (0xC0, 0xFF)]
    glyphs = []
    for start, end in test_ranges:
        for cp in range(start, end + 1):
            if face1x.get_char_index(cp) > 0:
                glyphs.append(cp)

    left_deltas = []
    top_deltas = []
    width_deltas = []
    height_deltas = []
    advance_deltas = []
    mismatched = []

    for cp in glyphs:
        face1x.load_char(chr(cp), load_flags)
        g1 = face1x.glyph
        left1 = g1.bitmap_left
        top1 = g1.bitmap_top
        w1 = g1.bitmap.width
        h1g = g1.bitmap.rows
        adv1 = fp4_from_ft16_16(g1.linearHoriAdvance)

        face2x.load_char(chr(cp), load_flags)
        g2 = face2x.glyph
        left2_exact = g2.bitmap_left / 2.0
        top2_exact = g2.bitmap_top / 2.0
        w2_exact = g2.bitmap.width / 2.0
        h2_exact = g2.bitmap.rows / 2.0
        adv2 = fp4_from_ft16_16(g2.linearHoriAdvance)

        dl = left2_exact - left1
        dt = top2_exact - top1
        dw = w2_exact - w1
        dh = h2_exact - h1g
        da = adv2 - adv1 * 2

        left_deltas.append(dl)
        top_deltas.append(dt)
        width_deltas.append(dw)
        height_deltas.append(dh)
        advance_deltas.append(da)

        if dl != 0 or dt != 0 or dw != 0 or dh != 0:
            mismatched.append((cp, left1, top1, w1, h1g, adv1,
                               left2_exact, top2_exact, w2_exact, h2_exact, adv2, da))

    print(f"Per-glyph comparison ({len(glyphs)} glyphs tested):")
    print(f"  Glyphs with ANY metric mismatch: {len(mismatched)} / {len(glyphs)}")
    print()

    for name, deltas in [("bitmap_left", left_deltas), ("bitmap_top", top_deltas),
                          ("width", width_deltas), ("height", height_deltas)]:
        nonzero = [d for d in deltas if d != 0]
        if nonzero:
            print(f"  {name}: {len(nonzero)} mismatches, "
                  f"range [{min(nonzero):+.1f}, {max(nonzero):+.1f}], "
                  f"mean {sum(abs(d) for d in nonzero)/len(nonzero):.2f}")
        else:
            print(f"  {name}: all match")

    nonzero_adv = [d for d in advance_deltas if d != 0]
    if nonzero_adv:
        print(f"  advance_x (12.4 fp): {len(nonzero_adv)} mismatches, "
              f"range [{min(nonzero_adv):+d}, {max(nonzero_adv):+d}] (1 = 1/16 px)")
    else:
        print(f"  advance_x (12.4 fp): all match (2x is exactly 2× 1x)")
    print()

    if mismatched:
        print("Mismatched glyphs (showing 2x/2 - 1x deltas):")
        print(f"  {'char':<6} {'left':>6} {'top':>6} {'width':>6} {'height':>6} {'adv_fp4':>8}")
        for (cp, l1, t1, w1, h1g, a1, l2, t2, w2, h2g, a2, da) in mismatched:
            ch = chr(cp) if 0x20 < cp < 0x7F else f"U+{cp:04X}"
            dl = l2 - l1
            dt = t2 - t1
            dw = w2 - w1
            dh = h2g - h1g
            def fmt(v):
                return f"{v:+.1f}" if v != 0 else "."
            print(f"  {ch:<6} {fmt(dl):>6} {fmt(dt):>6} {fmt(dw):>6} {fmt(dh):>6} {da:>+8d}" if da != 0
                  else f"  {ch:<6} {fmt(dl):>6} {fmt(dt):>6} {fmt(dw):>6} {fmt(dh):>6} {'.':>8}")

if args.compare_2x:
    compare_2x()
else:
    for ch in args.glyphs:
        inspect_glyph(ch)

#!/usr/bin/env python3
"""Unpack, repack, and downsize images in EPUB files.

Usage:
    python epub_tool.py unpack   book.epub [output_dir]
    python epub_tool.py pack     dir/      [output.epub]
    python epub_tool.py downsize dir/      [--max-width 480] [--max-height 800]
                                           [--dither] [--jpeg-quality 85]

The downsize command targets the Xteink X4 (480x800, 2-bit grayscale / 4 levels).
Requires: pip install Pillow
"""

import argparse
import os
import sys
import zipfile
from pathlib import Path

PROCESSABLE_IMAGES = {".jpg", ".jpeg", ".png", ".gif"}
GRAYSCALE_4_LUT = [round(v / 85) * 85 for v in range(256)]


def unpack(epub_path: str, output_dir: str | None = None) -> None:
    epub = Path(epub_path)
    if not epub.is_file():
        sys.exit(f"File not found: {epub}")

    dest = Path(output_dir) if output_dir else epub.with_suffix("")
    if dest.exists():
        sys.exit(f"Destination already exists: {dest}")

    with zipfile.ZipFile(epub, "r") as zf:
        zf.extractall(dest)

    print(f"Unpacked to {dest}/")


def pack(source_dir: str, output_path: str | None = None) -> None:
    src = Path(source_dir)
    if not src.is_dir():
        sys.exit(f"Not a directory: {src}")

    mimetype_file = src / "mimetype"
    if not mimetype_file.is_file():
        sys.exit(f"Missing mimetype file in {src} — not a valid EPUB structure")

    out = Path(output_path) if output_path else src.with_suffix(".epub")
    if out.exists():
        sys.exit(f"Output already exists: {out}")

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(mimetype_file, "mimetype", compress_type=zipfile.ZIP_STORED)

        for root, dirs, files in os.walk(src):
            dirs.sort()
            for fname in sorted(files):
                full = Path(root) / fname
                arcname = full.relative_to(src).as_posix()
                if arcname == "mimetype":
                    continue
                zf.write(full, arcname)

    print(f"Packed {out} ({out.stat().st_size:,} bytes)")


def _quantize_grayscale(gray, dither: bool):
    """Quantize an L-mode image to 4 grayscale levels (0, 85, 170, 255)."""
    from PIL import Image

    if not dither:
        return gray.point(GRAYSCALE_4_LUT)

    # Floyd-Steinberg dithering against a 4-level grayscale palette
    pal_img = Image.new("P", (1, 1))
    pal_data = []
    for v in (0, 85, 170, 255):
        pal_data.extend([v, v, v])
    pal_data.extend([0] * (768 - len(pal_data)))
    pal_img.putpalette(pal_data)

    quantized = (
        gray.convert("RGB")
        .quantize(colors=4, palette=pal_img, dither=Image.Dither.FLOYDSTEINBERG)
        .convert("L")
    )
    return quantized


def downsize(
    source_dir: str,
    max_width: int = 480,
    max_height: int = 800,
    dither: bool = False,
    jpeg_quality: int = 85,
) -> None:
    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow is required for downsize: pip install Pillow")

    src = Path(source_dir)
    if not src.is_dir():
        sys.exit(f"Not a directory: {src}")

    total_before = 0
    total_after = 0
    processed = 0

    for root, _dirs, files in os.walk(src):
        for fname in files:
            full = Path(root) / fname
            ext = full.suffix.lower()
            if ext not in PROCESSABLE_IMAGES:
                if ext in {".bmp", ".tiff", ".tif", ".webp"}:
                    print(f"  skip (unsupported format, convert manually): {full.relative_to(src)}")
                continue

            original_size = full.stat().st_size

            try:
                img = Image.open(full)
            except Exception:
                print(f"  skip (unreadable): {full.relative_to(src)}")
                continue

            if getattr(img, "n_frames", 1) > 1:
                print(f"  skip (animated): {full.relative_to(src)}")
                continue

            total_before += original_size
            orig_w, orig_h = img.size

            gray = img.convert("L")

            scale = min(max_width / orig_w, max_height / orig_h, 1.0)
            if scale < 1.0:
                gray = gray.resize(
                    (int(orig_w * scale), int(orig_h * scale)), Image.LANCZOS
                )

            gray = _quantize_grayscale(gray, dither)

            if ext in {".jpg", ".jpeg"}:
                gray.save(full, "JPEG", quality=jpeg_quality)
            elif ext == ".gif":
                gray.save(full, "GIF")
            else:
                gray.save(full, "PNG")

            new_size = full.stat().st_size
            total_after += new_size
            processed += 1

            pct = (1 - new_size / original_size) * 100 if original_size else 0
            size_tag = f"{orig_w}x{orig_h} -> {gray.size[0]}x{gray.size[1]}, " if scale < 1.0 else ""
            print(f"  {full.relative_to(src)}: {size_tag}{original_size:,} -> {new_size:,} ({pct:.0f}% smaller)")

    if processed == 0:
        print("No images found to process.")
        return

    saved = total_before - total_after
    pct_total = (saved / total_before) * 100 if total_before else 0
    print(f"\n{processed} images processed. {total_before:,} -> {total_after:,} bytes ({saved:,} saved, {pct_total:.0f}%)")


def main() -> None:
    parser = argparse.ArgumentParser(description="Unpack/repack/downsize EPUB files.")
    sub = parser.add_subparsers(dest="command")

    p_unpack = sub.add_parser("unpack", help="Extract an EPUB to a directory")
    p_unpack.add_argument("epub", help="Path to .epub file")
    p_unpack.add_argument("output", nargs="?", help="Output directory (default: epub name without extension)")

    p_pack = sub.add_parser("pack", help="Repack a directory into an EPUB")
    p_pack.add_argument("directory", help="Path to unpacked EPUB directory")
    p_pack.add_argument("output", nargs="?", help="Output .epub path (default: directory name + .epub)")

    p_down = sub.add_parser("downsize", help="Downsize images for 480x800 2-bit grayscale display")
    p_down.add_argument("directory", help="Path to unpacked EPUB directory")
    p_down.add_argument("--max-width", type=int, default=480, help="Max image width in pixels (default: 480)")
    p_down.add_argument("--max-height", type=int, default=800, help="Max image height in pixels (default: 800)")
    p_down.add_argument("--dither", action="store_true", help="Apply Floyd-Steinberg dithering (better gradients)")
    p_down.add_argument("--jpeg-quality", type=int, default=85, help="JPEG output quality 1-95 (default: 85)")

    args = parser.parse_args()
    if args.command == "unpack":
        unpack(args.epub, args.output)
    elif args.command == "pack":
        pack(args.directory, args.output)
    elif args.command == "downsize":
        downsize(args.directory, args.max_width, args.max_height, args.dither, args.jpeg_quality)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()

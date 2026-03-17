#!/usr/bin/env python3
"""Downsize images in EPUB files to a maximum of 800px on their longest edge."""

import argparse
import io
import os
import sys
import zipfile

from PIL import Image

MAX_DIMENSION = 800
SUFFIX = ".x4"


def downsize_image(data: bytes) -> bytes | None:
    """Resize an image so its longest edge is at most MAX_DIMENSION.

    Returns the resized image bytes, or None if no resizing was needed.
    """
    try:
        img = Image.open(io.BytesIO(data))
    except Exception:
        return None

    width, height = img.size
    longest = max(width, height)

    if longest <= MAX_DIMENSION:
        return None

    scale = MAX_DIMENSION / longest
    new_size = (round(width * scale), round(height * scale))
    resized = img.resize(new_size, Image.LANCZOS)

    buf = io.BytesIO()
    fmt = img.format or "PNG"
    save_kwargs = {}
    if fmt.upper() == "JPEG":
        save_kwargs["quality"] = 85
    resized.save(buf, format=fmt, **save_kwargs)
    return buf.getvalue()


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tiff", ".webp", ".svg"}


def is_image_entry(name: str) -> bool:
    _, ext = os.path.splitext(name.lower())
    return ext in IMAGE_EXTENSIONS and ext != ".svg"


def process_epub(src_path: str) -> None:
    base, ext = os.path.splitext(src_path)
    dst_path = f"{base}{SUFFIX}{ext}"

    if os.path.exists(dst_path):
        print(f"Skipping (already exists): {dst_path}")
        return

    print(f"Processing: {src_path}")

    resized_images: dict[str, bytes] = {}

    with zipfile.ZipFile(src_path, "r") as zin:
        for item in zin.infolist():
            if is_image_entry(item.filename):
                resized = downsize_image(zin.read(item.filename))
                if resized is not None:
                    resized_images[item.filename] = resized

    if not resized_images:
        print(f"  Skipped: no images need resizing in {src_path}")
        return

    with zipfile.ZipFile(src_path, "r") as zin:
        with zipfile.ZipFile(dst_path, "w") as zout:
            for item in zin.infolist():
                if item.filename == "mimetype":
                    zout.writestr(item, zin.read(item.filename), compress_type=zipfile.ZIP_STORED)
                elif item.filename in resized_images:
                    zout.writestr(item, resized_images[item.filename], compress_type=item.compress_type)
                else:
                    zout.writestr(item, zin.read(item.filename), compress_type=item.compress_type)

    print(
        f"  Done: {len(resized_images)} image(s) resized -> {dst_path}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Downsize images in EPUB files to max 800px on longest edge."
    )
    parser.add_argument(
        "epubs",
        nargs="+",
        metavar="FILE",
        help="Path(s) to .epub files to process",
    )
    args = parser.parse_args()

    for path in args.epubs:
        if not os.path.isfile(path):
            print(f"Error: not a file: {path}", file=sys.stderr)
            continue
        if not path.lower().endswith(".epub"):
            print(f"Warning: skipping non-epub file: {path}", file=sys.stderr)
            continue
        process_epub(path)


if __name__ == "__main__":
    main()

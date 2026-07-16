"""Prepare the logo source asset (src/images/Logo256.png) from original art.

Takes artwork drawn on a solid black background (e.g. the original badge
export) and produces the transparent source PNG that scripts/convert_logo.py
consumes:

  1. Background removal: flood-fills black from the image borders and makes
     it transparent. Interior black (e.g. the badge's sky) is not
     border-connected and survives. The transparent region is dilated 2px to
     swallow the antialiased fringe where background met artwork.
  2. Uniform outline: strokes the opaque region's boundary with a black ring
     of constant thickness (a disk swept along the actual edge, like an image
     editor's stroke layer). This follows the artwork's real, possibly
     imperfect edge -- no circle fitting. The ring gives the badge a boundary
     on the white page and disappears on the black page. Width 0 disables it.
  3. Crops to content and pads to a centered square with a small transparent
     margin, then saves as grayscale+alpha.

Usage: python scripts/prepare_logo_source.py original.png [ring_width]
  ring_width: outline thickness in source pixels; default is content
  size / 64 (matching a 4px ring at a 256px render), 0 for no outline.
"""

import os
import sys

USAGE = 'Usage: python scripts/prepare_logo_source.py original.png [ring_width]'


def remove_background(img):
    """Return an opacity mask (L, 0/255): border-connected black -> 0."""
    from PIL import ImageDraw, ImageFilter

    w, h = img.size
    # Trinary map keeps the flood-fill sentinel (255) unambiguous.
    tri = img.point(lambda p: 0 if p < 40 else 128)
    for seed in [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]:
        if tri.getpixel(seed) == 0:
            ImageDraw.floodfill(tri, seed, 255, thresh=0)
    bg = tri.point(lambda p: 255 if p == 255 else 0)
    bg = bg.filter(ImageFilter.MaxFilter(5))  # eat the antialiased fringe
    return bg.point(lambda p: 0 if p else 255)


def stroke_boundary(opaque, ring_width):
    """Return a mask (L, 0/255) of a uniform stroke around the opaque region."""
    from PIL import Image, ImageChops, ImageDraw, ImageFilter

    w, h = opaque.size
    boundary = ImageChops.subtract(opaque, opaque.filter(ImageFilter.MinFilter(3)))
    overlay = Image.new('L', (w, h), 0)
    draw = ImageDraw.Draw(overlay)
    data = boundary.tobytes()
    for i, v in enumerate(data):
        if v:
            x = i % w
            y = i // w
            draw.ellipse([x - ring_width, y - ring_width, x + ring_width, y + ring_width], fill=255)
    return ImageChops.subtract(overlay, opaque)


def main():
    if len(sys.argv) < 2 or len(sys.argv) > 3 or sys.argv[1] in ('-h', '--help'):
        print(USAGE)
        sys.exit(0 if len(sys.argv) > 1 and sys.argv[1] in ('-h', '--help') else 1)

    from PIL import Image, ImageChops

    img = Image.open(sys.argv[1]).convert('L')
    opaque = remove_background(img)
    bbox = opaque.getbbox()
    if bbox is None:
        sys.exit('error: no opaque content found')
    content = max(bbox[2] - bbox[0], bbox[3] - bbox[1])
    ring_width = int(sys.argv[2]) if len(sys.argv) > 2 else round(content / 64)

    if ring_width > 0:
        ring = stroke_boundary(opaque, ring_width)
        img.paste(0, mask=ring)
        opaque = ImageChops.lighter(opaque, ring)
        bbox = opaque.getbbox()

    # Crop to content, pad to a centered square with a small transparent margin.
    la = Image.merge('LA', (img, opaque)).crop(bbox)
    cw, ch = la.size
    side = max(cw, ch) + max(cw, ch) // 100
    canvas = Image.new('LA', (side, side), (255, 0))
    canvas.paste(la, ((side - cw) // 2, (side - ch) // 2))

    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(project_root, 'src', 'images', 'Logo256.png')
    canvas.save(out, optimize=True)
    print(f'Wrote {out} ({side}x{side}, ring_width={ring_width})')
    print('Now run: python scripts/convert_logo.py')


if __name__ == '__main__':
    main()

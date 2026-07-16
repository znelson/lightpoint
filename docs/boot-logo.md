# Boot / Sleep Logo

The logo shown by `BootActivity` and `SleepActivity` lives in
`src/images/Logo256.h`, generated from the source art `src/images/Logo256.png`
by `scripts/convert_logo.py`. Do not edit the header by hand.

## Regenerating

```bash
# Defaults: reads src/images/Logo256.png, writes src/images/Logo256.h at 256x256
python scripts/convert_logo.py

# Explicit input, size, and an optional preview PNG (on-screen appearance)
python scripts/convert_logo.py src/images/Logo256.png 256 /tmp/logo_preview.png
```

Requires Pillow (`scripts/requirements.txt`). To swap the artwork, replace
`src/images/Logo256.png` and rerun the script.

For artwork drawn on a solid black background (like the original badge
export), `scripts/prepare_logo_source.py` produces the transparent source
PNG:

```bash
python scripts/prepare_logo_source.py path/to/original.png  # then convert_logo.py
```

## Source art requirements

The source is a square grayscale PNG with an alpha channel. Transparency
defines the background: pixels with alpha below 128 are omitted at draw time,
letting the page (white on boot/LIGHT sleep, black on dark sleep) show
through. The converter makes no assumption about the artwork's shape.

For the current badge art, `prepare_logo_source.py` flood-filled the
original's solid black background to transparent from the borders (the
interior black sky is not border-connected, so it survives) and stroked the
opaque region's boundary with a black ring of uniform thickness. The stroke
follows the artwork's real, slightly non-circular edge -- no circle fitting
-- giving the badge a boundary on the white page while disappearing into the
black one.

No pre-rotation is applied: the logo is rendered through
`GfxRenderer::drawImage2Bit`, which draws via `drawPixel` and is therefore
orientation-correct (unlike `drawImage`, which copies bytes in panel-native
orientation and requires pre-rotated data, as `scripts/convert_icon.py` does
for the small UI icons).

## Format: packed 2-bit grayscale plus opacity mask

The header contains two arrays:

- `Logo256`: `size * size / 4` bytes of tones, 4 pixels per byte, MSB pair
  first, gray levels 0 (black) to 3 (white) -- the same row layout as the
  2-bit BMP path in `Bitmap`/`drawBitmap`.
- `Logo256Mask`: `size * size / 8` bytes of opacity, 8 pixels per byte, MSB
  first, 1 = opaque. Built from the source alpha (>= 128 is opaque).
  Transparent pixels store tone 3 so they stay unflagged in the grayscale
  planes even if drawn without the mask.

`GfxRenderer::drawImage2Bit` decodes them at draw time according to the
current render mode, skipping transparent pixels in every mode and deriving
each pass the differential grayscale refresh needs:

| Render mode     | Draws (where opaque)                       |
|-----------------|--------------------------------------------|
| `BW`            | black where level < 3, white where level 3 |
| `GRAYSCALE_LSB` | 1-bit where level == 1                     |
| `GRAYSCALE_MSB` | 1-bit where level == 1 or 2                |

**Boot renders 1-bit only, for speed.** The grayscale refresh costs two 48 KB
plane copies over SPI plus a custom-LUT gray refresh -- around a second at
exactly the moment the user is waiting for the device to come up.
`BootActivity` instead draws a single BW pass with `bwWhiteThreshold = 2`
(light gray renders white, the best standalone 1-bit rendition) and never
touches grayscale state.

**Sleep renders the full 4 levels.** `SleepActivity` draws the BW base (grays
render black), refreshes, then renders each gray plane into the framebuffer
(`setRenderMode` + `drawImage2Bit`, `copyGrayscale{Lsb,Msb}Buffers`) and runs
`displayGrayBuffer()`, which lifts level 1 to dark gray and level 2 to light
gray. Gray levels render black in the base because the differential grayscale
LUT drives a fixed waveform per plane-bit pair: a gray-flagged pixel must
start black on the panel or its final tone is wrong.

After the grayscale display, the activity re-renders the BW frame and calls
`cleanupGrayscaleWithFrameBuffer()`, which re-syncs the controller's RED RAM
with the real frame. Skipping this leaves the grayscale planes in controller
RAM, and the next differential refresh compares against them and ghosts the
logo. This is the same re-render-then-cleanup pattern as
`XtcReaderActivity`'s grayscale path (which avoids the 48 KB
`storeBwBuffer`/`restoreBwBuffer` alternative used by the EPUB reader).

The artwork renders with the same tones on both the white (LIGHT) and black
(dark/inverted) sleep backgrounds: the logo is drawn after `invertScreen()`,
and its transparent background simply lets either page show through.

## Changing the size

- The size must be a multiple of 8 (the mask packs 8 pixels per byte).
- Update `logoSize` in `BootActivity.cpp` and `SleepActivity.cpp`, and the
  array names if you change them (`Logo<size>`, `Logo<size>Mask`).
- Flash cost is `3 * size * size / 8` bytes (24 KB at 256); RAM cost is zero.
  Decode happens per pixel at draw time, which is negligible next to the
  e-ink refresh and only runs on the boot and sleep screens.

## Verifying on device

Flash and watch the boot screen: a single fast 1-bit render (light grays show
white). Then check the sleep screen in both modes: the gray tones (rainbow
trail, moon shading) appear one grayscale refresh after the black-and-white
pass, with identical tones in both modes -- LIGHT on a white page, dark mode
as a round badge on the black page. After waking from sleep, the home screen
and reader should show no logo ghosting.

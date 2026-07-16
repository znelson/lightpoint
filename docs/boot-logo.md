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

## Source art requirements

The converter expects a square image with a circular badge on a black
background (it detects the badge by scanning for the outermost bright ring).
It then:

- masks everything outside the badge circle to white, so the black corners of
  the source do not render as a black square on the white screen;
- draws a thin black outline ring, because the badge's own white ring is
  invisible against the white page.

No pre-rotation is applied: the logo is rendered through
`GfxRenderer::drawImage2Bit`, which draws via `drawPixel` and is therefore
orientation-correct (unlike `drawImage`, which copies bytes in panel-native
orientation and requires pre-rotated data, as `scripts/convert_icon.py` does
for the small UI icons).

## Format: packed 2-bit grayscale

The header contains a single array of `size * size / 4` bytes: 4 pixels per
byte, MSB pair first, gray levels 0 (black) to 3 (white) -- the same row
layout as the 2-bit BMP path in `Bitmap`/`drawBitmap`.

`GfxRenderer::drawImage2Bit` decodes it at draw time according to the current
render mode, deriving each pass the differential grayscale refresh needs:

| Render mode     | Draws                                      |
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
(dark/inverted) sleep backgrounds. The logo is drawn after `invertScreen()`,
and in dark mode `SleepActivity` paints the square's corners outside the
badge circle black (`maskRoundedRectOutsideCorners`) so they blend with the
page; the corners are white (level 3) in the stored image and receive no
gray drive.

## Changing the size

- The size must be a multiple of 4 (pixels are packed 4 per byte).
- Update `logoSize` in `BootActivity.cpp` and `SleepActivity.cpp`, and the
  array name if you change it (`Logo<size>`).
- Flash cost is `size * size / 4` bytes (16 KB at 256); RAM cost is zero.
  Decode happens per pixel at draw time, which is negligible next to the
  e-ink refresh and only runs on the boot and sleep screens.

## Verifying on device

Flash and watch the boot screen: a single fast 1-bit render (light grays show
white). Then check the sleep screen in both modes: the gray tones (rainbow
trail, moon shading) appear one grayscale refresh after the black-and-white
pass, with identical tones in both modes -- LIGHT on a white page, dark mode
as a round badge on the black page. After waking from sleep, the home screen
and reader should show no logo ghosting.

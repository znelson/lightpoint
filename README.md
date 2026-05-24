# LightPoint Reader

LightPoint is open-source e-reader firmware, targeting ESP32C3-based Xteink [X4](https://www.xteink.com/products/xteink-x4) and [X3](https://www.xteink.com/products/xteink-x3). Community-built, fully hackable, free forever.

This is an opinionated, lightweight fork of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader).

---

## Changes from upstream CrossPoint

### Removed

- **Web server** -- no built-in HTTP server, WebDAV, or Calibre wireless library support
- **OPDS** -- no OPDS browser or catalogue-based book downloading
- **KOSync** -- no KOReader progress sync
- **Legacy settings migration** -- binary-to-JSON settings upgrade path dropped; clean installs only
- **QR code display** -- "Show page as QR" reader menu option removed
- **Auto page turn** -- automatic timed page-turning feature removed
- **Font downloader** -- "Manage Fonts" settings entry and over-the-air font download removed; fonts may still be placed on the SD card manually

### Added

- **TOC anchor navigation for multi-chapter spine files** -- chapter selection navigates to the correct anchor within a shared spine document, rather than always jumping to the top of the file (upstream issue [#383](https://github.com/crosspoint-reader/crosspoint-reader/issues/383))
- **Multi-spine chapter page counting** -- TOC chapters that span multiple spine files now show accurate aggregated page counts in the status bar and reader menu, instead of the per-spine count. The last TOC entry is capped to its own spine so appendices and copyright pages do not inflate the final chapter's count (upstream issue [#1131](https://github.com/crosspoint-reader/crosspoint-reader/issues/1131))

### Changed

- **Courier Prime replaces Open Dyslexic** -- the third font slot is now a monospace face (Courier Prime), completing the serif / sans-serif / monospace triad
- **Build system migrated from Arduino to ESP-IDF** -- replaced the Arduino framework with native ESP-IDF, removing the Arduino HAL layer and porting all hardware calls to ESP-IDF APIs directly
- **ESP-IDF upgraded to 6.x** -- further upgraded to ESP-IDF 6.0.1 (GCC 15.2); no user-visible behavior change

---

LightPoint is **not affiliated with Xteink or any device manufacturer**.

Huge shoutout to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which inspired this project.

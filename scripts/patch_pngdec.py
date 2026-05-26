"""
PlatformIO pre-build script: patch PNGdec for ESP-IDF builds.

Patch 1 (PNGdec.h): PNGdec.h includes <Arduino.h> in its catch-all #else
branch. ESP-IDF defines ESP_PLATFORM, so we add that to the existing guard.

Patch 2 (zlib C files): inflate.c and infback.c contain intentional switch
fallthroughs that GCC 15.2 now warns about as errors (-Wimplicit-fallthrough).
We prepend a file-scoped diagnostic suppress pragma to each affected file.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os

_PNGDEC_H_OLD = "#if defined( __MACH__ ) || defined( __LINUX__ ) || defined( __MCUXPRESSO )"
_PNGDEC_H_NEW = "#if defined( __MACH__ ) || defined( __LINUX__ ) || defined( __MCUXPRESSO ) || defined( ESP_PLATFORM )"

_FALLTHROUGH_SENTINEL = "#pragma GCC diagnostic ignored \"-Wimplicit-fallthrough\""
_FALLTHROUGH_PREFIX = (
    "#pragma GCC diagnostic push\n"
    "#pragma GCC diagnostic ignored \"-Wimplicit-fallthrough\"\n"
)
_FALLTHROUGH_SUFFIX = "\n#pragma GCC diagnostic pop\n"

_ZLIB_FILES = ("inflate.c", "infback.c")


def _patch_header(src_dir):
    header = os.path.join(src_dir, "PNGdec.h")
    if not os.path.isfile(header):
        return
    with open(header, "r") as f:
        content = f.read()
    if _PNGDEC_H_NEW in content:
        return
    if _PNGDEC_H_OLD not in content:
        import sys
        sys.stderr.write("ERROR: PNGdec.h patch target not found in %s\n" % header)
        raise SystemExit(1)
    with open(header, "w") as f:
        f.write(content.replace(_PNGDEC_H_OLD, _PNGDEC_H_NEW, 1))
    print("Patched PNGdec.h: %s" % header)


def _patch_zlib_fallthrough(src_dir):
    for fname in _ZLIB_FILES:
        path = os.path.join(src_dir, fname)
        if not os.path.isfile(path):
            continue
        with open(path, "r") as f:
            content = f.read()
        if _FALLTHROUGH_SENTINEL in content:
            continue  # already patched
        with open(path, "w") as f:
            f.write(_FALLTHROUGH_PREFIX + content + _FALLTHROUGH_SUFFIX)
        print("Patched PNGdec/%s: suppressed -Wimplicit-fallthrough" % fname)


def patch_pngdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        src_dir = os.path.join(libdeps_dir, env_dir, "PNGdec", "src")
        if not os.path.isdir(src_dir):
            continue
        _patch_header(src_dir)
        _patch_zlib_fallthrough(src_dir)


patch_pngdec(env)  # noqa: F821

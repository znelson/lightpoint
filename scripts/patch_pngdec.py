"""
PlatformIO pre-build script: patch PNGdec for ESP-IDF builds.

PNGdec.h includes <Arduino.h> in its catch-all #else branch (anything that
isn't __MACH__, __LINUX__, or __MCUXPRESSO__). ESP-IDF defines ESP_PLATFORM,
so we add that to the existing guard so the Arduino include is skipped.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os

OLD = "#if defined( __MACH__ ) || defined( __LINUX__ ) || defined( __MCUXPRESSO )"
NEW = "#if defined( __MACH__ ) || defined( __LINUX__ ) || defined( __MCUXPRESSO ) || defined( ESP_PLATFORM )"


def patch_pngdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        header = os.path.join(libdeps_dir, env_dir, "PNGdec", "src", "PNGdec.h")
        if not os.path.isfile(header):
            continue
        with open(header, "r") as f:
            content = f.read()
        if NEW in content:
            return  # already patched
        if OLD not in content:
            import sys
            sys.stderr.write("ERROR: PNGdec patch target not found in %s\n" % header)
            raise SystemExit(1)
        with open(header, "w") as f:
            f.write(content.replace(OLD, NEW, 1))
        print("Patched PNGdec: %s" % header)


patch_pngdec(env)  # noqa: F821

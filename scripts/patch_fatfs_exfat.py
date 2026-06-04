"""
PlatformIO pre-build script: enable exFAT in ESP-IDF's bundled FatFs.

FF_FS_EXFAT is hardcoded to 0 in ffconf.h with no Kconfig option to override
it. exFAT support is required for SD cards > 32GB, which are commonly formatted
as exFAT by default on Windows and macOS.

FF_USE_LABEL, FF_USE_FASTSEEK, and FF_USE_DYN_BUFFER are defined as
CONFIG_FATFS_* macros that are absent from sdkconfig.h (disabled options
generate no #define). With FF_FS_EXFAT=0 they were dead code; enabling exFAT
compiles them into boolean expressions, causing undeclared-identifier errors.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import sys

PATCHES = [
    ("#define FF_FS_EXFAT\t\t0",                            "#define FF_FS_EXFAT\t\t1"),
    ("#define FF_USE_FASTSEEK\tCONFIG_FATFS_USE_FASTSEEK",  "#define FF_USE_FASTSEEK\t0"),
    ("#define FF_USE_LABEL\tCONFIG_FATFS_USE_LABEL",        "#define FF_USE_LABEL\t0"),
    ("#define FF_USE_DYN_BUFFER CONFIG_FATFS_USE_DYN_BUFFERS", "#define FF_USE_DYN_BUFFER 1"),
]


def patch_fatfs_exfat(env):
    idf_path = os.path.join(env.get("PROJECT_PACKAGES_DIR", ""), "framework-espidf")
    if not os.path.isdir(idf_path):
        packages_dir = env.get("PROJECT_PACKAGES_DIR", "")
        for entry in os.listdir(packages_dir) if os.path.isdir(packages_dir) else []:
            if entry.startswith("framework-espidf"):
                idf_path = os.path.join(packages_dir, entry)
                break

    ffconf = os.path.join(idf_path, "components", "fatfs", "src", "ffconf.h")
    if not os.path.isfile(ffconf):
        sys.stderr.write("ERROR: ffconf.h not found at %s\n" % ffconf)
        raise SystemExit(1)

    with open(ffconf, "r") as f:
        content = f.read()

    changed = False
    for old, new in PATCHES:
        if old in content:
            content = content.replace(old, new, 1)
            changed = True

    if changed:
        with open(ffconf, "w") as f:
            f.write(content)
        print("Patched FatFs exFAT: %s" % ffconf)


patch_fatfs_exfat(env)  # noqa: F821

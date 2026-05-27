"""
Print a SHA-256 digest of the platformio.ini fields that determine what gets
downloaded into ~/.platformio/packages: platform, board, framework, lib_deps.

Used by .github/workflows/*.yml to key the PlatformIO package cache without
invalidating it on unrelated changes (version bumps, build flag tweaks,
env-specific config).
"""

import configparser
import hashlib
import sys

KEYS_AFFECTING_PACKAGES = ('platform', 'board', 'framework', 'lib_deps')


def compute_key(ini_path):
    cp = configparser.ConfigParser(strict=False, interpolation=None)
    cp.read(ini_path)
    parts = []
    for section in sorted(cp.sections()):
        for key in KEYS_AFFECTING_PACKAGES:
            if cp.has_option(section, key):
                parts.append(f'[{section}] {key} = {cp.get(section, key).strip()}')
    return hashlib.sha256('\n'.join(parts).encode()).hexdigest()


if __name__ == '__main__':
    ini_path = sys.argv[1] if len(sys.argv) > 1 else 'platformio.ini'
    print(compute_key(ini_path))

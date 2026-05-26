"""
PlatformIO pre-build script: ensure PlatformIO penv has required Python packages.

intelhex is required by esptool for bootloader/firmware image generation but is
not always present in PlatformIO's isolated Python environment. Install it if
missing so the first build on a clean machine doesn't fail mid-link.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import subprocess
import sys

_REQUIRED = ["intelhex"]


def ensure_penv_deps():
    for pkg in _REQUIRED:
        try:
            __import__(pkg)
        except ImportError:
            print("pio-penv: installing missing dependency '%s'" % pkg)
            subprocess.check_call([sys.executable, "-m", "pip", "install", pkg])


ensure_penv_deps()

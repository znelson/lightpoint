"""
PlatformIO pre-build script: fix old-style declaration ordering in IDF C files.

GCC 15.2 with -Werror rejects declarations where specifiers don't come first
(-Wold-style-declaration). The IDF package has several patterns that trigger this:

  1. 'const [attrs] static'           ->  'static [attrs] const'
  2. '__attribute__(...) const static' ->  'static const __attribute__(...)'
  3. 'static <type> inline'           ->  'static inline <type>'
  4. '<known-type> static'            ->  'static <known-type>'  (line-start only)
  5. '<known-type> inline'            ->  'inline <known-type>'  (line-start only)
  6. '<ATTR_MACRO> const static'      ->  '<ATTR_MACRO> static const'  (DRAM_ATTR etc.)

Patterns 4 and 5 only apply to a whitelist of known C type tokens to avoid
false-positive matches in comments or macro expansions.

A sentinel file prevents re-scanning on subsequent builds.
"""

Import("env")  # noqa: F821

import os
import re

# Type tokens for patterns 4/5 (single-token return types seen in IDF)
_KNOWN_TYPES = (
    "esp_err_t", "uint64_t", "uint32_t", "uint16_t", "uint8_t",
    "int64_t", "int32_t", "int16_t", "int8_t", "size_t",
    "unsigned", "double", "signed", "float", "short", "long",
    "void", "bool", "char", "int",
)
_TYPES_RE = "(?:" + "|".join(sorted(_KNOWN_TYPES, key=len, reverse=True)) + ")"

# Compiled patterns + replacement functions
_RULES = [
    # 1. const [attr] static  ->  static [attr] const
    (
        re.compile(
            r"^(const(?:\s+__attribute__\s*\(\([^)]*\)\))*)\s+(static\b)",
            re.MULTILINE,
        ),
        lambda m: m.group(2) + m.group(1)[len("const"):].rstrip() + " const",
    ),
    # 2. __attribute__(...) const static  ->  static const __attribute__(...)
    (
        re.compile(
            r"^(__attribute__\s*\(\([^)]*\)\)\s+)const\s+static\b",
            re.MULTILINE,
        ),
        lambda m: "static const " + m.group(1),
    ),
    # 3. static <type> inline  ->  static inline <type>
    (
        re.compile(r"\bstatic\s+(\w+)\s+inline\b"),
        lambda m: "static inline " + m.group(1),
    ),
    # 4. <known-type> static  ->  static <known-type>  (line start only)
    (
        re.compile(r"^(" + _TYPES_RE + r")\s+static\b\s+", re.MULTILINE),
        lambda m: "static " + m.group(1) + " ",
    ),
    # 5. <known-type> inline  ->  inline <known-type>  (line start only)
    (
        re.compile(r"^(" + _TYPES_RE + r")\s+inline\b\s+", re.MULTILINE),
        lambda m: "inline " + m.group(1) + " ",
    ),
    # 6. <ATTR_MACRO> const static  ->  <ATTR_MACRO> static const
    #    Covers IDF macros like DRAM_ATTR, IRAM_ATTR, RTC_DATA_ATTR, etc.
    (
        re.compile(r"^([A-Z][A-Z0-9_]*_ATTR\b\s+)const\s+static\b", re.MULTILINE),
        lambda m: m.group(1) + "static const",
    ),
]


def _find_idf_root(_env):
    for var in ("PIOHOME_DIR", "PROJECT_PACKAGES_DIR"):
        val = _env.get(var, "")
        if val:
            p = os.path.join(val, "framework-espidf")
            if os.path.isdir(p):
                return p
    piohome = os.environ.get("PLATFORMIO_HOME_DIR", os.path.expanduser("~/.platformio"))
    p = os.path.join(piohome, "packages", "framework-espidf")
    return p if os.path.isdir(p) else None


def patch_idf_old_style_decls(_env):
    idf_root = _find_idf_root(_env)
    if idf_root is None:
        print("patch_espidf_ota: IDF root not found -- skipping")
        return

    sentinel = os.path.join(idf_root, ".patched_const_static")
    if os.path.exists(sentinel):
        return  # Already patched this IDF installation

    patched_count = 0
    for dirpath, _dirnames, filenames in os.walk(os.path.join(idf_root, "components")):
        for fname in filenames:
            if not fname.endswith(".c"):
                continue
            path = os.path.join(dirpath, fname)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue

            result = text
            total = 0
            for pattern, repl in _RULES:
                result, n = pattern.subn(repl, result)
                total += n

            if total == 0:
                continue

            with open(path, "w", encoding="utf-8") as f:
                f.write(result)
            patched_count += 1

    with open(sentinel, "w") as f:
        f.write("patched\n")

    print("patch_espidf_ota: fixed old-style decls in %d IDF C files" % patched_count)


patch_idf_old_style_decls(env)  # noqa: F821

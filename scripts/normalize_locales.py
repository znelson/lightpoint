#!/usr/bin/env python3
"""
Normalize locale YAML translation files to LightPoint conventions.

Conventions enforced (non-English files only):
  1. Branding: the key STR_CROSSPOINT becomes STR_LIGHTPOINT, and the
     exact-case brand string "CrossPoint" in values becomes "LightPoint".
     Lowercase identifiers (e.g. the .crosspoint SD directory) are never
     rewritten.
  2. No unknown keys: entries whose key does not exist in english.yaml are
     removed. This covers strings for features LightPoint has removed.
  3. No empty values: gen_i18n.py already treats these as missing; removed.
  4. English-identical values: reported as warnings by default, since a few
     are deliberate (words genuinely identical in that language, e.g. "OK").
     With --strip-identical they are removed instead - gen_i18n.py falls
     back to English for missing keys, so the rendered string is unchanged.
     Use --strip-identical when normalizing a file incoming from upstream,
     where identical values are untranslated copies, not deliberate.
  5. Metadata: _language_name, _language_code, _order must be present. An
     _order that collides with another file's is reassigned to the next
     free integer (reported loudly).

english.yaml is the reference and is never modified.

Normalization is behavior-preserving by construction: gen_i18n.py output is
byte-identical before and after, because missing keys inherit the English
string and the generator's blob dedup already collapses English-identical
values to a shared offset.

Usage:
    python scripts/normalize_locales.py [files...] [options]

With no file arguments every *.yaml in the translations directory is
processed. Passing specific files (e.g. a locale file incoming from an
upstream merge) restricts normalization to those; english.yaml and the
_order uniqueness checks still come from the full directory.

Typical uses:
    python scripts/normalize_locales.py --check            # CI / validation
    python scripts/normalize_locales.py                    # after a merge
    python scripts/normalize_locales.py --strip-identical \\
        lib/I18n/translations/newlocale.yaml               # incoming file
"""

import argparse
import os
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_i18n import _unescape_yaml_value, parse_yaml_file  # noqa: E402

import re  # noqa: E402

# Must stay in sync with the accepted line format in gen_i18n.parse_yaml_file.
LINE_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*"(.*)"$')

RENAMED_KEYS = {
    "STR_CROSSPOINT": "STR_LIGHTPOINT",
}

# Exact case only: the brand is spelled "CrossPoint"; lowercase occurrences
# (the .crosspoint SD directory) must survive untouched.
BRAND_REPLACEMENTS = [
    ("CrossPoint", "LightPoint"),
]

METADATA_KEYS = ("_language_name", "_language_code", "_order")


class Note:
    """One change made (or warning raised) at a specific line."""

    def __init__(self, line_num: int, action: str, detail: str):
        self.line_num = line_num
        self.action = action
        self.detail = detail

    def __str__(self) -> str:
        return f"    line {self.line_num}: {self.action}: {self.detail}"


def find_english_file(translations_dir: Path) -> Path:
    for yf in sorted(translations_dir.glob("*.yaml")):
        try:
            data = parse_yaml_file(str(yf))
        except ValueError:
            # Unparseable sibling (e.g. conflict markers mid-merge): it is
            # not the English reference, so keep looking.
            continue
        if data.get("_language_code", "").upper() == "EN":
            return yf
    raise ValueError(f"No YAML file with _language_code EN in {translations_dir}")


def collect_orders(translations_dir: Path) -> Dict[str, List[str]]:
    """Map each _order value to the filenames that claim it."""
    orders: Dict[str, List[str]] = {}
    for yf in sorted(translations_dir.glob("*.yaml")):
        try:
            data = parse_yaml_file(str(yf))
        except ValueError:
            # A sibling file that is mid-merge (conflict markers) must not
            # abort normalization of the files actually requested; it just
            # cannot vote in the _order collision check until resolved.
            print(f"{yf.name}: WARNING: unparseable, skipped for _order check")
            continue
        order = data.get("_order")
        if order is not None:
            orders.setdefault(order, []).append(yf.name)
    return orders


def next_free_order(orders: Dict[str, List[str]]) -> int:
    taken = set()
    for order in orders:
        try:
            taken.add(int(order))
        except ValueError:
            pass
    return max(taken, default=0) + 1


def normalize_file(
    filepath: Path,
    english: Dict[str, str],
    orders: Dict[str, List[str]],
    strip_identical: bool,
) -> Tuple[Optional[str], List[Note], List[Note]]:
    """
    Return (new_content, changes, warnings). new_content is None when the
    file is already normalized.
    """
    original = filepath.read_text(encoding="utf-8")
    out_lines: List[str] = []
    changes: List[Note] = []
    warnings: List[Note] = []
    seen: set = set()

    for line_num, raw_line in enumerate(original.splitlines(), start=1):
        line = raw_line.rstrip("\r")

        if not line.strip():
            out_lines.append("")
            continue

        match = LINE_RE.match(line)
        if not match:
            raise ValueError(
                f"{filepath}:{line_num}: bad format: {line!r}\n"
                f'  Expected:  KEY: "value"'
            )

        key, raw_value = match.group(1), match.group(2)
        rebuilt = False

        if key in RENAMED_KEYS:
            new_key = RENAMED_KEYS[key]
            changes.append(Note(line_num, "rename key", f"{key} -> {new_key}"))
            key = new_key
            rebuilt = True

        for old, new in BRAND_REPLACEMENTS:
            if old in raw_value:
                changes.append(Note(line_num, "rebrand value", key))
                raw_value = raw_value.replace(old, new)
                rebuilt = True

        if key in seen:
            raise ValueError(f"{filepath}:{line_num}: duplicate key '{key}'")
        seen.add(key)

        if key.startswith("_"):
            if key == "_order":
                claimants = [
                    c for c in orders.get(raw_value, []) if c != filepath.name
                ]
                if claimants:
                    new_order = str(next_free_order(orders))
                    changes.append(
                        Note(
                            line_num,
                            "reassign _order",
                            f"{raw_value} collides with {', '.join(claimants)};"
                            f" now {new_order}",
                        )
                    )
                    raw_value = new_order
                    orders.setdefault(new_order, []).append(filepath.name)
                    rebuilt = True
            out_lines.append(f'{key}: "{raw_value}"' if rebuilt else line)
            continue

        if key not in english:
            changes.append(Note(line_num, "drop unknown key", key))
            continue

        value = _unescape_yaml_value(raw_value, str(filepath), line_num)

        if not value.strip():
            changes.append(Note(line_num, "drop empty value", key))
            continue

        if value == english[key]:
            if strip_identical:
                changes.append(Note(line_num, "drop English-identical", key))
                continue
            warnings.append(Note(line_num, "English-identical value", key))

        out_lines.append(f'{key}: "{raw_value}"' if rebuilt else line)

    for meta in METADATA_KEYS:
        if meta not in seen:
            raise ValueError(f"{filepath}: missing required metadata key '{meta}'")

    # Collapse blank-line runs left behind by dropped entries and trim both
    # ends, keeping a single trailing newline.
    collapsed: List[str] = []
    for line in out_lines:
        if not line and collapsed and not collapsed[-1]:
            continue
        collapsed.append(line)
    while collapsed and not collapsed[0]:
        collapsed.pop(0)
    while collapsed and not collapsed[-1]:
        collapsed.pop()
    new_content = "\n".join(collapsed) + "\n"

    if new_content == original:
        return None, changes, warnings
    return new_content, changes, warnings


def summarize(notes: List[Note]) -> str:
    counts: Dict[str, int] = {}
    for n in notes:
        counts[n.action] = counts.get(n.action, 0) + 1
    return ", ".join(f"{count} {action}" for action, count in sorted(counts.items()))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Normalize locale YAML files to LightPoint conventions."
    )
    parser.add_argument(
        "files",
        nargs="*",
        help="Specific locale files to normalize (default: all in --dir)",
    )
    parser.add_argument(
        "--dir",
        default="lib/I18n/translations",
        help="Translations directory (default: lib/I18n/translations)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Report files needing normalization and exit 1; write nothing",
    )
    parser.add_argument(
        "--strip-identical",
        action="store_true",
        help="Remove entries whose value equals the English value (intended "
        "for files incoming from upstream; a few identical values in "
        "maintained files are deliberate)",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Report every dropped/rewritten entry and every warning key",
    )
    args = parser.parse_args()

    translations_dir = Path(args.dir)
    if not translations_dir.is_dir():
        print(f"Error: translations directory not found: {translations_dir}")
        sys.exit(1)

    try:
        english_file = find_english_file(translations_dir)
        english = {
            k: v
            for k, v in parse_yaml_file(str(english_file)).items()
            if not k.startswith("_")
        }
        orders = collect_orders(translations_dir)

        if args.files:
            targets = [Path(f) for f in args.files]
        else:
            targets = sorted(translations_dir.glob("*.yaml"))

        dirty = 0
        for target in targets:
            if not target.is_file():
                raise FileNotFoundError(f"File not found: {target}")
            if target.resolve() == english_file.resolve():
                if args.files:
                    print(f"{target.name}: skipped (English is the reference)")
                continue

            new_content, changes, warnings = normalize_file(
                target, english, orders, args.strip_identical
            )

            if warnings:
                print(f"{target.name}: WARNING: {summarize(warnings)}")
                if args.verbose:
                    for w in warnings:
                        print(w)

            if new_content is None:
                if args.verbose:
                    print(f"{target.name}: ok")
                continue

            dirty += 1
            print(f"{target.name}: {summarize(changes)}")
            if args.verbose:
                for c in changes:
                    print(c)

            if not args.check:
                target.write_text(new_content, encoding="utf-8", newline="\n")

        if dirty == 0:
            print(f"All {len(targets)} file(s) already normalized.")
        elif args.check:
            print(f"\n{dirty} file(s) need normalization (run without --check to fix).")
            sys.exit(1)
        else:
            print(f"\nNormalized {dirty} file(s).")

    except Exception as e:
        print(f"\nError: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()

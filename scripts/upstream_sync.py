#!/usr/bin/env python3
"""
Driver for the upstream (CrossPoint) -> LightPoint catch-up process.

Subcommands:
    status   Show the last merged anchor, drift, and the next candidate
             commits annotated with risk flags from .upstream-sync.yaml.
    reap     Enforce the removed-features manifest on a tree: re-delete
             resurrected paths, warn on relocation sources, grep for
             banned symbols.
    start    Begin a sync batch: worktree + branch from main, merge the
             anchor (or record an ours-merge with --reject), then run the
             mechanical pre-resolution (reap, locale normalization,
             submodule pointer) and report what is left for judgment.
    verify   Post-resolution gauntlet: no unmerged files, i18n regen,
             normalize --check, reap --check, host gtest suite, clang-format
             on changed sources, FILE_VERSION heuristic, pio run (unless
             --quick).
    pr       Push the branch and open the PR (or --dry-run to preview the
             generated body). Reminds that the PR must be MERGED, never
             squashed.
    walk     Per-commit walkthrough of how a batch landed in the fork:
             for every upstream commit in a sync merge, classify each
             touched file's disposition in the result (adopted verbatim /
             adapted / relocated / dropped per manifest / kept ours).
             Fork-side diffs are cut down to the hunks attributed to
             the commit being walked; unattributed hunks are listed as
             residue at the end. Works on any sync merge, historical ones
             included; -i steps interactively with diffs on demand.

Runbook (step-by-step workflow, conventions, iron rules):
.claude/skills/upstream-sync/SKILL.md. Process rationale and history:
Vault/Chromanaut/docs/plans/upstream-merge-process.md (Obsidian vault).
The manifest and this script are read from the repo this file lives in, so
a merge worktree does not need them committed.

Direction: sync branches start from main and merge the upstream anchor in,
so "ours" is always LightPoint during resolution, and --reject can use
git's -s ours (there is no -s theirs).
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path
from typing import Dict, List, Optional, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
MANIFEST_PATH = REPO_ROOT / ".upstream-sync.yaml"
NORMALIZER = SCRIPT_DIR / "normalize_locales.py"

UPSTREAM_BRANCH = "upstream/develop"
SYNC_BRANCH = "upstream-merge"
PR_REPO = "znelson/lightpoint"
PUSH_REMOTE = "znelson"
SOURCE_EXTS = ("*.cpp", "*.h", "*.c", "*.py", "*.ini")
SUBMODULE_PATHS = (".gitmodules", "open-x4-sdk")


def run(args: List[str], cwd: Optional[Path] = None, check: bool = True) -> str:
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(args)}\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


def git(tree: Path, *args: str, check: bool = True) -> str:
    return run(["git", "-C", str(tree)] + list(args), check=check)


# ---------------------------------------------------------------------------
# Manifest parsing (YAML subset: block lists, "key: value", full-line
# comments; same no-PyYAML policy as gen_i18n.py)
# ---------------------------------------------------------------------------


def _parse_lines(text: str) -> List[Tuple[int, str]]:
    out = []
    for raw in text.splitlines():
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        out.append((indent, raw.strip()))
    return out


def _unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


def _parse_block(lines: List[Tuple[int, str]], i: int, indent: int):
    """Parse a block (list or dict) whose entries sit at *indent*."""
    if lines[i][1].startswith("- "):
        return _parse_list(lines, i, indent)
    return _parse_dict(lines, i, indent)


def _parse_list(lines, i, indent):
    items = []
    while i < len(lines) and lines[i][0] == indent and lines[i][1].startswith("- "):
        content = lines[i][1][2:]
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):(?:\s+(.*))?$", content)
        if m:
            # List item is a dict; its keys continue at indent + 2.
            item = {}
            key, val = m.group(1), m.group(2)
            if val is not None:
                item[key] = _unquote(val)
                i += 1
            else:
                i += 1
                sub, i = _parse_block(lines, i, indent + 4)
                item[key] = sub
            while i < len(lines) and lines[i][0] == indent + 2:
                item2, i = _parse_dict_entry(lines, i, indent + 2)
                item.update(item2)
            items.append(item)
        else:
            items.append(_unquote(content))
            i += 1
    return items, i


def _parse_dict_entry(lines, i, indent):
    m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):(?:\s+(.*))?$", lines[i][1])
    if not m:
        raise ValueError(f"manifest parse error at: {lines[i][1]!r}")
    key, val = m.group(1), m.group(2)
    if val is not None:
        return {key: _unquote(val)}, i + 1
    i += 1
    if i >= len(lines) or lines[i][0] <= indent:
        return {key: None}, i
    sub, i = _parse_block(lines, i, lines[i][0])
    return {key: sub}, i


def _parse_dict(lines, i, indent):
    result = {}
    while i < len(lines) and lines[i][0] == indent and not lines[i][1].startswith("- "):
        entry, i = _parse_dict_entry(lines, i, indent)
        result.update(entry)
    return result, i


def load_manifest() -> Dict:
    if not MANIFEST_PATH.is_file():
        raise FileNotFoundError(f"manifest not found: {MANIFEST_PATH}")
    lines = _parse_lines(MANIFEST_PATH.read_text(encoding="utf-8"))
    data, i = _parse_dict(lines, 0, 0)
    if i != len(lines):
        raise ValueError(f"manifest parse stopped at: {lines[i][1]!r}")
    for section in ("features", "relocations", "repo_meta"):
        if section not in data:
            raise ValueError(f"manifest missing section: {section}")
    return data


def glob_to_regex(pattern: str) -> "re.Pattern":
    i, out = 0, ""
    while i < len(pattern):
        if pattern[i : i + 3] == "**/":
            out += "(?:.*/)?"
            i += 3
        elif pattern[i : i + 2] == "**":
            out += ".*"
            i += 2
        elif pattern[i] == "*":
            out += "[^/]*"
            i += 1
        elif pattern[i] == "?":
            out += "[^/]"
            i += 1
        else:
            out += re.escape(pattern[i])
            i += 1
    return re.compile("^" + out + "$")


class Manifest:
    def __init__(self, data: Dict):
        self.features = data["features"]
        self.relocations = data["relocations"]
        self.repo_meta_paths = data["repo_meta"].get("paths", [])
        self.removal_globs: List[Tuple[str, "re.Pattern"]] = []
        for feature in self.features:
            for p in feature.get("paths") or []:
                self.removal_globs.append((feature["name"], glob_to_regex(p)))
        for p in self.repo_meta_paths:
            self.removal_globs.append(("repo-meta", glob_to_regex(p)))
        self.reloc_globs = [
            (r["from"], r["to"], glob_to_regex(r["from"])) for r in self.relocations
        ]
        self.symbols: List[str] = []
        for feature in self.features:
            self.symbols.extend(feature.get("symbols") or [])

    def match_removal(self, path: str) -> Optional[str]:
        for name, rx in self.removal_globs:
            if rx.match(path):
                return name
        return None

    def match_relocation(self, path: str) -> Optional[Tuple[str, str]]:
        for src, dst, rx in self.reloc_globs:
            if rx.match(path):
                return (src, dst)
        return None


# ---------------------------------------------------------------------------
# status
# ---------------------------------------------------------------------------


def find_anchor(tree: Path) -> str:
    return git(tree, "merge-base", "main", UPSTREAM_BRANCH).strip()


def cmd_status(args) -> int:
    tree = REPO_ROOT
    manifest = Manifest(load_manifest())
    if args.fetch:
        print("Fetching upstream...")
        git(tree, "fetch", "upstream", "--prune")
        git(tree, "fetch", PUSH_REMOTE, check=False)
    behind = git(tree, "rev-list", "--count", f"main..{PUSH_REMOTE}/main",
                 check=False).strip()
    if behind and behind != "0":
        print(f"NOTE: local main is {behind} commit(s) behind {PUSH_REMOTE}/main"
              " (start will fast-forward it)")
    anchor = find_anchor(tree)
    anchor_desc = git(
        tree, "log", "-1", "--format=%h %ad %s", "--date=short", anchor
    ).strip()
    total = git(tree, "rev-list", "--count", f"{anchor}..{UPSTREAM_BRANCH}").strip()
    fp = git(
        tree, "rev-list", "--count", "--first-parent", f"{anchor}..{UPSTREAM_BRANCH}"
    ).strip()
    print(f"Last anchor:  {anchor_desc}")
    print(f"Drift:        {fp} first-parent commits ({total} total) on {UPSTREAM_BRANCH}")
    print()

    log = git(
        tree,
        "log",
        "--first-parent",
        "--reverse",
        "--format=%H|%h|%ad|%s",
        "--date=short",
        f"{anchor}..{UPSTREAM_BRANCH}",
    ).strip()
    if not log:
        print("Nothing to sync.")
        return 0

    rows = []
    for line in log.splitlines()[: args.limit]:
        sha, short, date, subject = line.split("|", 3)
        files = git(tree, "diff", "--name-only", f"{sha}^", sha, check=False).split()
        flags = set()
        for f in files:
            if manifest.match_removal(f):
                flags.add("MANIFEST")
            if manifest.match_relocation(f):
                flags.add("RELOC")
            if f.startswith("lib/I18n/translations/"):
                flags.add("I18N")
            if f in SUBMODULE_PATHS or re.search(r"(^|/)[\w-]*-sdk$", f):
                flags.add("SDK")
        tags = git(tree, "tag", "--points-at", sha, check=False).split()
        stat = git(tree, "diff", "--shortstat", f"{sha}^", sha, check=False).strip()
        m = re.search(r"(\d+) insertion", stat)
        n = re.search(r"(\d+) deletion", stat)
        size = f"+{m.group(1) if m else 0}/-{n.group(1) if n else 0}"
        rows.append((short, date, size, sorted(flags), tags, subject))

    prev_flags = None
    for short, date, size, flags, tags, subject in rows:
        if prev_flags is not None and flags != prev_flags:
            print("  " + "-" * 76)
        prev_flags = flags
        flag_str = ",".join(flags) if flags else "-"
        tag_str = f"  [tag: {', '.join(tags)}]" if tags else ""
        print(f"  {short} {date} {size:>12}  {flag_str:18} {subject[:60]}{tag_str}")

    shown = min(args.limit, len(log.splitlines()))
    print(f"\nShowing {shown} of {fp} first-parent candidates (oldest first).")
    print("Flags: MANIFEST=touches removed-feature path, RELOC=touches relocated")
    print("path, I18N=translations, SDK=submodule/SDK. Separators mark flag changes.")
    return 0


# ---------------------------------------------------------------------------
# walk
# ---------------------------------------------------------------------------


def _blob(tree: Path, commit: str, path: str) -> Optional[str]:
    out = git(tree, "rev-parse", "-q", "--verify", f"{commit}:{path}",
              check=False).strip()
    return out or None


def _is_upstream_commit(tree: Path, commit: str) -> bool:
    return subprocess.run(
        ["git", "-C", str(tree), "merge-base", "--is-ancestor", commit,
         UPSTREAM_BRANCH],
        capture_output=True,
    ).returncode == 0


def _resolve_sync_merge(tree: Path, merge: str, depth: int = 0):
    """
    Given a sync merge (inner merge or the GitHub PR merge wrapping it, in
    either historical direction), return (merge, main_parent, anchor), or
    None if *merge* is not recognizably a sync merge.
    """
    parents = git(tree, "log", "-1", "--format=%p", merge, check=False).split()
    if len(parents) != 2:
        return None
    p1, p2 = parents
    up1, up2 = _is_upstream_commit(tree, p1), _is_upstream_commit(tree, p2)
    if up1 != up2:
        anchor = p1 if up1 else p2
        main_parent = p2 if up1 else p1
        return merge, main_parent, anchor
    if depth == 0:
        # A GitHub PR merge has (main tip, inner sync merge); descend once,
        # reporting against the inner merge, which is the faithful record
        # of how the batch was applied.
        for p in (p2, p1):
            resolved = _resolve_sync_merge(tree, p, depth + 1)
            if resolved:
                return resolved
    return None


def _relocation_target(src_glob: str, dst: str, path: str) -> str:
    if src_glob.endswith("/**") and dst.endswith("/**"):
        return dst[:-3] + path[len(src_glob) - 3:]
    return dst


def _disposition(tree: Path, manifest: Manifest, path: str,
                 merge: str, main_parent: str, anchor: str) -> str:
    result = _blob(tree, merge, path)
    upstream = _blob(tree, anchor, path)
    if result is None:
        feature = manifest.match_removal(path)
        if feature:
            return f"dropped [{feature}]"
        reloc = manifest.match_relocation(path)
        if reloc:
            target = _relocation_target(reloc[0], reloc[1], path)
            if " " in target:
                return f"relocated: {target}"
            # Batch-level signal only: whether the fork-side target changed
            # anywhere in this merge. It cannot attribute that change to
            # THIS commit (another batch commit, or a pre-batch fork fix,
            # may account for it) - use p/l on the file to see how.
            changed = _blob(tree, merge, target) != _blob(tree, main_parent,
                                                          target)
            state = ("target changed in batch" if changed
                     else "target unchanged in batch")
            return f"relocated -> {target} ({state})"
        return "absent (not in fork)"
    if result == upstream:
        if _blob(tree, main_parent, path) == upstream:
            return "already present"
        return "adopted verbatim"
    if result == _blob(tree, main_parent, path):
        # For a cleanly auto-merged file this can only mean the upstream
        # delta was already in our tree (round-tripped or independently
        # made); for a conflicted file it can also mean we declined it.
        return "kept ours (already covered, or declined in resolution)"
    if path.startswith("lib/I18n/translations/"):
        return "adapted (normalized)"
    return "adapted (resolved to LightPoint form)"


# ---------------------------------------------------------------------------
# walk: per-commit attribution of the fork-side landing
# ---------------------------------------------------------------------------
#
# A batch lands as one merge commit, so "LightPoint before -> after" for a
# file is the whole batch's net change plus resolution. To show only what one
# upstream commit contributed, each hunk of that diff is attributed:
#   blame  - an added line blames (through the merge's upstream parent) to
#            the commit, or to a commit inside it when it is a PR merge;
#   match  - a hunk with no blamed owner (pure deletion, or lines rewritten
#            by hand in resolution, which blame to the merge itself) deletes
#            or adds a non-trivial line, unique within the file, that the
#            commit also deleted/added (uniqueness keeps common idioms like
#            "return 0;" from tying unrelated hunks to a commit).
# Hunks nobody claims are the residue, listed at the end of the walk.

_LINE_RE = re.compile(rb"[^\n]*\n|[^\n]+\Z")
_HUNK_RE = re.compile(rb"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
_BLAME_RE = re.compile(rb"^([0-9a-f]{40}) \d+ (\d+)(?: \d+)?$")
_DIFF_OPTS = ("--no-ext-diff", "--no-textconv", "--no-color", "-U0")


def _git_bytes(tree: Path, *args: str,
               ok: Tuple[int, ...] = (0,)) -> Optional[bytes]:
    # ok=(0, 1) is for diff, where 1 just means "differences found"; for
    # show/blame any nonzero exit is a failure.
    result = subprocess.run(["git", "-C", str(tree)] + list(args),
                            capture_output=True)
    return result.stdout if result.returncode in ok else None


def _line_key(line: bytes) -> Optional[bytes]:
    """Whitespace-insensitive identity of a line, or None when the line is
    too generic (braces, blank, 'else') to tie a hunk to a commit."""
    key = b" ".join(line.split())
    return key if len(key) >= 6 else None


def _parse_u0(diff: bytes) -> List[Tuple[int, int, int, int]]:
    """-U0 hunk headers as 0-based half-open (a0, a1, b0, b1) ranges."""
    hunks = []
    for line in diff.split(b"\n"):
        m = _HUNK_RE.match(line)
        if not m:
            continue
        a, b = int(m.group(1)), int(m.group(2) or 1)
        c, d = int(m.group(3)), int(m.group(4) or 1)
        a0 = a if b == 0 else a - 1
        b0 = c if d == 0 else c - 1
        hunks.append((a0, a0 + b, b0, b0 + d))
    return hunks


class WalkSlicer:
    def __init__(self, tree: Path, manifest: "Manifest", merge: str,
                 main_parent: str, prev: str,
                 commit_files: Dict[str, List[str]]):
        self.tree = tree
        self.merge = merge
        self.main_parent = main_parent
        self.prev = prev
        self.order = list(commit_files)
        # blamed sha -> the first-parent batch commit that carries it
        self.member: Dict[str, str] = {}
        # fork path -> [(batch commit, upstream path)]
        self.touching: Dict[str, List[Tuple[str, str]]] = {}
        for sha, files in commit_files.items():
            inner = git(tree, "rev-list", f"{sha}^..{sha}",
                        check=False).split()
            for c in inner or [sha]:
                self.member.setdefault(c, sha)
            for f in files:
                target = _result_path(tree, manifest, merge, f)
                self.touching.setdefault(target, []).append((sha, f))
        self._files: Dict[str, Optional[Dict]] = {}
        self._upstream: Dict[Tuple[str, str], Tuple[set, set]] = {}

    def _upstream_lines(self, sha: str, path: str) -> Tuple[set, set]:
        key = (sha, path)
        if key not in self._upstream:
            removed, added = set(), set()
            diff = _git_bytes(self.tree, "diff", *_DIFF_OPTS, f"{sha}^", sha,
                              "--", path, ok=(0, 1)) or b""
            for line in diff.split(b"\n"):
                if line.startswith((b"---", b"+++")):
                    continue
                k = _line_key(line[1:]) if line[:1] in (b"-", b"+") else None
                if k:
                    (removed if line[:1] == b"-" else added).add(k)
            self._upstream[key] = (removed, added)
        return self._upstream[key]

    def attribution(self, path: str) -> Optional[Dict]:
        """Hunks of main_parent -> merge for a fork path, each with the
        batch commits that own it. None when the file cannot be sliced
        (absent from the result, or binary)."""
        if path in self._files:
            return self._files[path]
        self._files[path] = None
        if _blob(self.tree, self.merge, path) is None:
            return None
        after_raw = _git_bytes(self.tree, "show", f"{self.merge}:{path}")
        has_before = _blob(self.tree, self.main_parent, path) is not None
        before_raw = _git_bytes(
            self.tree, "show", f"{self.main_parent}:{path}") \
            if has_before else b""
        if after_raw is None or before_raw is None \
                or b"\0" in after_raw or b"\0" in before_raw:
            return None
        before = _LINE_RE.findall(before_raw)
        after = _LINE_RE.findall(after_raw)
        if has_before:
            diff = _git_bytes(self.tree, "diff", *_DIFF_OPTS,
                              f"{self.main_parent}:{path}",
                              f"{self.merge}:{path}", ok=(0, 1))
            if diff is None:
                return None
            ranges = _parse_u0(diff)
        else:
            ranges = [(0, 0, 0, len(after))] if after else []

        blamed: List[Optional[str]] = [None] * len(after)
        if ranges:
            out = _git_bytes(self.tree, "blame", "--porcelain", self.merge,
                             f"^{self.main_parent}", f"^{self.prev}",
                             "--", path) or b""
            for line in out.split(b"\n"):
                m = _BLAME_RE.match(line)
                if m:
                    n = int(m.group(2)) - 1
                    if 0 <= n < len(blamed):
                        blamed[n] = m.group(1).decode()

        # Only lines unique within their file can tie a hunk to a commit;
        # repeated idioms ("return 0;") would match unrelated hunks.
        before_counts = Counter(k for k in map(_line_key, before) if k)
        after_counts = Counter(k for k in map(_line_key, after) if k)

        hunks = []
        for a0, a1, b0, b1 in ranges:
            shas = blamed[b0:b1]
            owners = {self.member[c]: "blame" for c in shas
                      if c in self.member}
            if not owners:
                gone = {k for k in map(_line_key, before[a0:a1])
                        if k and before_counts[k] == 1}
                new = {k for k in map(_line_key, after[b0:b1])
                       if k and after_counts[k] == 1}
                for sha, upath in self.touching.get(path, []):
                    removed, added = self._upstream_lines(sha, upath)
                    if gone & removed or new & added:
                        owners[sha] = "match"
            hunks.append({"a0": a0, "a1": a1, "b0": b0, "b1": b1,
                          "owners": owners,
                          "resolution": self.merge in shas})
        self._files[path] = {"before": before, "after": after,
                             "hunks": hunks, "new": not has_before}
        return self._files[path]

    def _synth(self, info: Dict, keep) -> bytes:
        out: List[bytes] = []
        pos = 0
        for h in info["hunks"]:
            out += info["before"][pos:h["a0"]]
            out += info["after"][h["b0"]:h["b1"]] if keep(h) \
                else info["before"][h["a0"]:h["a1"]]
            pos = h["a1"]
        out += info["before"][pos:]
        return b"".join(out)

    def slice(self, sha: Optional[str], path: str):
        """(before, after, stats) where after is the fork's pre-merge file
        with only *sha*'s hunks applied; sha=None selects the residue."""
        info = self.attribution(path)
        if info is None:
            return None
        if sha is None:
            mine = [h for h in info["hunks"] if not h["owners"]]
        else:
            mine = [h for h in info["hunks"] if sha in h["owners"]]
        shared = {o for h in mine for o in h["owners"] if o != sha}
        stats = {
            "total": len(info["hunks"]),
            "mine": len(mine),
            "matched": sum(1 for h in mine if h["owners"].get(sha) == "match"),
            "resolution": sum(1 for h in mine if h["resolution"]),
            "shared": [c for c in self.order if c in shared],
            "residue": sum(1 for h in info["hunks"] if not h["owners"]),
        }
        chosen = {id(h) for h in mine}
        return (b"".join(info["before"]),
                self._synth(info, lambda h: id(h) in chosen), stats)

    def describe(self, stats: Dict) -> str:
        parts = [f"{stats['mine']}/{stats['total']} hunks"]
        if stats["matched"]:
            parts.append(f"{stats['matched']} by line match")
        if stats["resolution"]:
            parts.append(f"{stats['resolution']} with resolution edits")
        if stats["shared"]:
            parts.append("shared with " +
                         ", ".join(c[:8] for c in stats["shared"]))
        if stats["residue"]:
            parts.append(f"{stats['residue']} unattributed in file")
        return ", ".join(parts)

    def text_diff(self, sha: Optional[str], paths: List[str]) -> str:
        import difflib
        chunks = []
        for path in paths:
            sliced = self.slice(sha, path)
            if not sliced or not sliced[2]["mine"]:
                continue
            before, after, _ = sliced
            chunks.append("".join(difflib.unified_diff(
                before.decode("utf-8", "replace").splitlines(True),
                after.decode("utf-8", "replace").splitlines(True),
                f"a/{path}", f"b/{path}")))
        return "\n".join(chunks)

    def residue_paths(self) -> List[Tuple[str, int]]:
        changed = git(self.tree, "diff", "--name-only", self.main_parent,
                      self.merge, check=False).split("\n")
        out = []
        for path in filter(None, changed):
            info = self.attribution(path)
            if info:
                n = sum(1 for h in info["hunks"] if not h["owners"])
                if n:
                    out.append((path, n))
        return out

    def write_pair(self, sha: Optional[str], path: str, outdir: str,
                   flat: bool = True):
        """Materialize a slice as (before_file, after_file) for a GUI diff:
        flat names for a file comparison, tree layout for a folder one."""
        sliced = self.slice(sha, path)
        if not sliced:
            return None
        before, after, stats = sliced
        tag = sha[:8] if sha else "RESIDUE"
        base = Path(path)
        if flat:
            fb = Path(outdir) / f"{base.stem}.LIGHTPOINT-BEFORE{base.suffix}"
            fa = Path(outdir) / \
                f"{base.stem}.LIGHTPOINT-AFTER-{tag}-ONLY{base.suffix}"
        else:
            fb = Path(outdir) / "before" / path
            fa = Path(outdir) / f"after-{tag}-only" / path
        for f, data in ((fb, before), (fa, after)):
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(data)
        return str(fb), str(fa), stats


def _find_compare() -> Optional[str]:
    for base in (os.path.expanduser("~/Applications"), "/Applications"):
        app = base + "/Araxis Merge.app/Contents/Utilities/compare"
        if os.path.exists(app):
            return app
    # PATH fallback: ImageMagick also ships a 'compare' binary, so only
    # accept a PATH hit that resolves inside an Araxis install.
    path = shutil.which("compare")
    if path and "araxis" in os.path.realpath(path).lower():
        return path
    return None


def _extract_blob(tree: Path, commit: str, path: str, outdir: str,
                  role: str) -> Optional[str]:
    if _blob(tree, commit, path) is None:
        return None
    # Bytes, not text: blobs can be binary (fonts, images) and Araxis
    # compares those fine.
    result = subprocess.run(
        ["git", "-C", str(tree), "show", f"{commit}:{path}"],
        capture_output=True)
    if result.returncode != 0:
        return None
    base = Path(path)
    out = Path(outdir) / f"{base.stem}.{role}{base.suffix}"
    out.write_bytes(result.stdout)
    return str(out)


def _result_path(tree: Path, manifest: Manifest, merge: str,
                 path: str) -> str:
    """The upstream path mapped to where the fork keeps the file, when
    the file was relocated and the target exists in the merge result."""
    if _blob(tree, merge, path) is None:
        reloc = manifest.match_relocation(path)
        if reloc:
            target = _relocation_target(reloc[0], reloc[1], path)
            if " " not in target and _blob(tree, merge, target) is not None:
                return target
    return path


def _launch_three_pane(tree: Path, manifest: Manifest, sha: str, path: str,
                       merge: str) -> None:
    """
    Araxis three-pane for one file of one batch commit: upstream's edit on
    the left, the pre-edit base in the middle, LightPoint's final form on
    the right (relocation-mapped when the fork moved the file).
    """
    compare = _find_compare()
    if not compare:
        print("  Araxis 'compare' CLI not found")
        return
    result_path = _result_path(tree, manifest, merge, path)
    outdir = tempfile.mkdtemp(prefix="upstream-walk-")
    upstream = _extract_blob(tree, sha, path, outdir, "UPSTREAM")
    base = _extract_blob(tree, f"{sha}^", path, outdir, "BASE")
    ours = _extract_blob(tree, merge, result_path, outdir, "LIGHTPOINT")
    labeled = [(role, p) for role, p in
               (("upstream", upstream), ("base", base), ("lightpoint", ours))
               if p]
    if len(labeled) < 2:
        print(f"  not enough versions of {path} to compare")
        return
    # Araxis opens a two-way by default even with three file arguments;
    # -3 is required for a three-pane (same flag git's araxis mergetool
    # passes).
    nway = "-3" if len(labeled) == 3 else "-2"
    subprocess.Popen([compare, nway] + [p for _, p in labeled],
                     stdout=subprocess.DEVNULL)
    note = f" (result: {result_path})" if result_path != path else ""
    roles = "|".join(role for role, _ in labeled)
    print(f"  Araxis: {Path(path).name} {roles}{note}")


def _launch_pair_diffs(tree: Path, manifest: Manifest, sha: str, path: str,
                       merge: str, main_parent: str,
                       prev: Optional[str] = None,
                       anchor: Optional[str] = None,
                       slicer: Optional["WalkSlicer"] = None) -> None:
    """
    Dual two-way for one file: upstream before -> after beside LightPoint
    before -> after. Unlike the three-pane, this separates "what upstream
    changed" from "how the fork's copy moved", which the base|result diff
    conflates with pre-existing fork divergence.

    The upstream pair is per-commit. With a slicer, the LightPoint pair is
    cut down to the hunks attributed to this commit (see WalkSlicer): the
    "after" side is the fork's pre-merge file with only those hunks
    applied. Without one (or for a file that cannot be sliced) it is the
    whole batch's net change, and with prev/anchor given a note lists the
    other batch commits folded into it.
    """
    compare = _find_compare()
    if not compare:
        print("  Araxis 'compare' CLI not found")
        return
    result_path = _result_path(tree, manifest, merge, path)
    outdir = tempfile.mkdtemp(prefix="upstream-walk-")
    sliced = slicer.write_pair(sha, result_path, outdir) if slicer else None
    if sliced:
        ours_before, ours_after, stats = sliced
        if not stats["mine"]:
            print(f"  lightpoint pair skipped for {Path(result_path).name} "
                  f"(no hunks attributed to {sha[:8]}: "
                  f"{slicer.describe(stats)}; b<file#> for the whole batch)")
            ours_before = ours_after = None
    else:
        ours_before = _extract_blob(tree, main_parent, result_path, outdir,
                                    "LIGHTPOINT-BEFORE")
        ours_after = _extract_blob(tree, merge, result_path, outdir,
                                   "LIGHTPOINT-AFTER")
    pairs = (
        ("upstream", path,
         _extract_blob(tree, f"{sha}^", path, outdir, "UPSTREAM-BEFORE"),
         _extract_blob(tree, sha, path, outdir, "UPSTREAM-AFTER")),
        ("lightpoint", result_path, ours_before, ours_after),
    )
    launched = 0
    for name, shown, before, after in pairs:
        if before and after:
            if launched:
                time.sleep(0.4)  # keep comparison (tab) order deterministic
            subprocess.Popen([compare, "-2", before, after],
                             stdout=subprocess.DEVNULL)
            launched += 1
            print(f"  Araxis: {Path(shown).name} {name} before|after")
            if name == "lightpoint" and sliced:
                print(f"    this commit only: {slicer.describe(stats)}")
            elif name == "lightpoint" and prev and anchor:
                others = [c for c in git(
                    tree, "log", "--first-parent", "--format=%h",
                    f"{prev}..{anchor}", "--", path,
                    check=False).split() if not sha.startswith(c)]
                if others:
                    print(f"    note: this pair is the whole batch's net "
                          f"change (plus resolution); also folded in: "
                          f"{', '.join(reversed(others))}")
        elif not (name == "lightpoint" and sliced):
            have = "after only" if after else ("before only" if before
                                               else "neither version")
            print(f"  {name} pair skipped for {Path(shown).name} ({have})")


def _launch_all_views(tree: Path, manifest: Manifest, sha: str, path: str,
                      merge: str, main_parent: str,
                      prev: Optional[str] = None,
                      anchor: Optional[str] = None,
                      slicer: Optional["WalkSlicer"] = None) -> None:
    """
    The full triptych for one file: both before|after pairs, then the
    three-pane. The compare CLI has no tab/window placement options, so
    whether these stack as tabs in one window or open separately follows
    the Merge application preference; the staggered launches keep the
    ordering deterministic either way.
    """
    _launch_pair_diffs(tree, manifest, sha, path, merge, main_parent,
                       prev, anchor, slicer)
    time.sleep(0.4)
    _launch_three_pane(tree, manifest, sha, path, merge)


def _launch_dir_diff(tree: Path, a: str, b: str, paths: List[str]) -> None:
    compare = _find_compare()
    if not compare:
        print("  Araxis 'compare' CLI not found")
        return
    # git's built-in araxis tool takes 'compare' from PATH, where another
    # tool's binary can shadow the Araxis one; pin the real CLI. stderr is
    # left attached to the terminal so tool failures are visible.
    cmd = ["git", "-C", str(tree),
           "-c", f"difftool.araxis.path={compare}",
           "difftool", "-d", "--tool=araxis", "--no-prompt", a, b]
    if paths:
        cmd += ["--"] + paths
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL)
    la = a[:10] + "^" if a.endswith("^") else a[:10]
    lb = b[:10] + "^" if b.endswith("^") else b[:10]
    print(f"  Araxis folder comparison: {la} vs {lb}")


def _launch_slice_dir_diff(slicer: "WalkSlicer", sha: Optional[str],
                           paths: List[str]) -> None:
    """Araxis folder comparison of the fork-side hunks attributed to one
    batch commit (sha=None: the unattributed residue)."""
    compare = _find_compare()
    if not compare:
        print("  Araxis 'compare' CLI not found")
        return
    outdir = tempfile.mkdtemp(prefix="upstream-walk-")
    dirs = None
    for path in paths:
        # Check before writing: a no-hunk pair would leave empty parent
        # dirs behind, which Araxis shows as empty folders.
        sliced = slicer.slice(sha, path)
        if not (sliced and sliced[2]["mine"]):
            continue
        pair = slicer.write_pair(sha, path, outdir, flat=False)
        if pair:
            dirs = (str(Path(pair[0]).parents[len(Path(path).parts) - 1]),
                    str(Path(pair[1]).parents[len(Path(path).parts) - 1]))
    if not dirs:
        print("  (no fork-side hunks attributed)")
        return
    subprocess.Popen([compare, "-2", dirs[0], dirs[1]],
                     stdout=subprocess.DEVNULL)
    print(f"  Araxis folder comparison: {Path(dirs[0]).name} vs "
          f"{Path(dirs[1]).name}")


def _walk_residue(args, slicer: "WalkSlicer", interactive: bool) -> None:
    residue = slicer.residue_paths()
    if not residue:
        print("Residue: none - every fork-side hunk is attributed to a "
              "batch commit.")
        return
    print("Residue: fork-side hunks no batch commit accounts for "
          "(hand resolution, reaper/normalizer edits, unmatched deletions):")
    width = max(len(p) for p, _ in residue)
    for n, (path, count) in enumerate(residue, 1):
        print(f"  {n:>2}. {path:{width}}  {count} hunk(s)")
    if not interactive:
        return
    gui = " <file#>=Araxis" if args.difftool else " <file#>=diff"
    while True:
        try:
            choice = input(f"[Enter]=done  l=all residue{gui}  q=quit > ") \
                .strip().lower()
        except EOFError:
            return
        if choice in ("", "q"):
            return
        paths = [p for p, _ in residue]
        if choice == "l":
            if args.difftool:
                _launch_slice_dir_diff(slicer, None, paths)
            else:
                print(slicer.text_diff(None, paths))
        elif choice.isdigit() and 1 <= int(choice) <= len(residue):
            path = paths[int(choice) - 1]
            if args.difftool:
                compare = _find_compare()
                pair = slicer.write_pair(
                    None, path, tempfile.mkdtemp(prefix="upstream-walk-"))
                if compare and pair:
                    subprocess.Popen([compare, "-2", pair[0], pair[1]],
                                     stdout=subprocess.DEVNULL)
                    print(f"  Araxis: {Path(path).name} residue before|after")
            else:
                print(slicer.text_diff(None, [path]))


def cmd_walk(args) -> int:
    tree = Path(args.tree).resolve() if args.tree else REPO_ROOT
    manifest = Manifest(load_manifest())
    merge = git(tree, "rev-parse", "--verify", args.merge + "^{commit}").strip()
    resolved = _resolve_sync_merge(tree, merge)
    if not resolved:
        print(f"Error: {args.merge} does not look like a sync merge "
              "(need a merge with one upstream parent)")
        return 1
    merge, main_parent, anchor = resolved
    prev = git(tree, "merge-base", main_parent, anchor).strip()
    log = git(tree, "log", "--first-parent", "--reverse",
              "--format=%H|%h|%ad|%s", "--date=short",
              f"{prev}..{anchor}").strip().splitlines()
    print(f"Batch {prev[:8]}..{anchor[:8]} applied by merge "
          f"{merge[:8]} ({len(log)} commits)\n")

    commit_files = {
        line.split("|", 1)[0]: git(
            tree, "diff", "--name-only", f"{line.split('|', 1)[0]}^",
            line.split("|", 1)[0], check=False).split()
        for line in log}
    slicer = WalkSlicer(tree, manifest, merge, main_parent, prev,
                        commit_files)

    interactive = args.interactive and sys.stdin.isatty()
    for idx, line in enumerate(log, 1):
        sha, short, date, subject = line.split("|", 3)
        if args.commit and not sha.startswith(args.commit) \
                and not short.startswith(args.commit):
            continue
        files = commit_files[sha]
        # Fork-side views (landed/remerge) must also cover the relocated
        # targets, where the upstream paths no longer exist.
        landing = list(files)
        for f in files:
            target = _result_path(tree, manifest, merge, f)
            if target != f and target not in landing:
                landing.append(target)
        print(f"[{idx}/{len(log)}] {short} {date} {subject}")
        width = max((len(f) for f in files), default=0)
        for n, f in enumerate(files, 1):
            d = _disposition(tree, manifest, f, merge, main_parent, anchor)
            print(f"  {n:>2}. {f:{width}}  {d}")
            sliced = slicer.slice(sha, _result_path(tree, manifest, merge, f))
            if sliced and sliced[2]["total"]:
                print(f"      {'':{width}}  landed: "
                      f"{slicer.describe(sliced[2])}")
        print()

        if args.difftool and args.commit:
            _launch_dir_diff(tree, f"{sha}^", sha, [])
            for f in files:
                _launch_three_pane(tree, manifest, sha, f, merge)
            print()

        if args.commit:
            print("--- upstream change ---")
            print(git(tree, "diff", f"{sha}^", sha, check=False))
            print("--- landing in fork attributed to this commit ---")
            landed = slicer.text_diff(sha, landing)
            print(landed if landed.strip()
                  else "(no fork-side hunks attributed to this commit)")
            if args.batch_net:
                print("--- net landing in fork (these files, whole batch) ---")
                landed = git(tree, "diff", main_parent, merge, "--",
                             *landing, check=False)
                print(landed if landed.strip()
                      else "(no net change to these files)")
            print("--- manual intervention (remerge-diff slice) ---")
            rr = git(tree, "show", "--remerge-diff", "--format=", merge,
                     "--", *landing, check=False)
            print(rr if rr.strip() else "(none - handled mechanically)")

        if interactive and not args.commit:
            gui = (" <file#>=3-pane p<file#>=dual 2-way a<file#>=all "
                   "b<file#>=dual 2-way, whole batch") \
                if args.difftool else ""
            prompt = (f"[Enter]=next  d=upstream diff  l=landed (this "
                      f"commit)  b=landed (whole batch)  "
                      f"r=remerge slice{gui}  q=quit > ")
            while True:
                try:
                    choice = input(prompt).strip().lower()
                except EOFError:
                    return 0
                if choice == "":
                    break
                if choice == "q":
                    return 0
                if choice == "d":
                    if args.difftool:
                        _launch_dir_diff(tree, f"{sha}^", sha, [])
                    else:
                        print(git(tree, "diff", f"{sha}^", sha, check=False))
                elif choice == "l":
                    if args.difftool:
                        _launch_slice_dir_diff(slicer, sha, landing)
                    else:
                        out = slicer.text_diff(sha, landing)
                        print(out if out.strip() else
                              "(no fork-side hunks attributed to this commit)")
                elif choice == "b":
                    if args.difftool:
                        _launch_dir_diff(tree, main_parent, merge, landing)
                    else:
                        out = git(tree, "diff", main_parent, merge, "--",
                                  *landing, check=False)
                        print(out if out.strip() else "(no net change)")
                elif choice == "r":
                    out = git(tree, "show", "--remerge-diff", "--format=",
                              merge, "--", *landing, check=False)
                    print(out if out.strip() else "(none - handled mechanically)")
                elif args.difftool and choice.isdigit() \
                        and 1 <= int(choice) <= len(files):
                    _launch_three_pane(tree, manifest, sha,
                                       files[int(choice) - 1], merge)
                elif args.difftool and choice[:1] in ("p", "a", "b") \
                        and choice[1:].isdigit() \
                        and 1 <= int(choice[1:]) <= len(files):
                    fn = _launch_all_views if choice[0] == "a" \
                        else _launch_pair_diffs
                    fn(tree, manifest, sha, files[int(choice[1:]) - 1],
                       merge, main_parent, prev, anchor,
                       None if choice[0] == "b" else slicer)
                else:
                    hint = "" if args.difftool else \
                        " (file-number views need --difftool)"
                    print(f"  unrecognized: {choice!r}{hint}")
    if not args.commit:
        _walk_residue(args, slicer, interactive)
    return 0


# ---------------------------------------------------------------------------
# reap
# ---------------------------------------------------------------------------


def reap(tree: Path, check_only: bool, verbose: bool = True) -> Tuple[int, int]:
    """
    Enforce the manifest on *tree*. Returns (removed_count, problem_count)
    where problems are relocation warnings plus banned-symbol hits.
    """
    manifest = Manifest(load_manifest())
    tracked = set(git(tree, "ls-files").splitlines())

    removed = 0
    problems = 0
    to_remove: Dict[str, List[str]] = {}
    for path in sorted(tracked):
        feature = manifest.match_removal(path)
        if feature:
            to_remove.setdefault(feature, []).append(path)

    for feature, paths in sorted(to_remove.items()):
        for p in paths:
            action = "would remove" if check_only else "removing"
            print(f"  {action} [{feature}]: {p}")
        if not check_only:
            git(tree, "rm", "-f", "-q", "--", *paths)
        removed += len(paths)

    for path in sorted(tracked):
        reloc = manifest.match_relocation(path)
        if reloc:
            print(f"  WARNING [relocated]: {path}")
            print(f"    port upstream changes to: {reloc[1]}, then delete manually")
            problems += 1

    if manifest.symbols:
        pattern = "|".join(manifest.symbols)
        hits = git(
            tree, "grep", "-nwE", pattern, "--", *SOURCE_EXTS, check=False
        ).strip()
        if hits:
            # Symbol hits inside files being removed are expected; filter them.
            removed_paths = {p for paths in to_remove.values() for p in paths}
            for line in hits.splitlines():
                hit_path = line.split(":", 1)[0]
                if hit_path in removed_paths:
                    continue
                print(f"  SYMBOL HIT: {line[:160]}")
                problems += 1

    if verbose:
        if removed == 0 and problems == 0:
            print("  reap: clean (no resurrected paths, no banned symbols)")
        else:
            print(f"  reap: {removed} path(s) {'flagged' if check_only else 'removed'}, "
                  f"{problems} problem(s) needing hands")
    return removed, problems


def cmd_reap(args) -> int:
    tree = Path(args.tree).resolve() if args.tree else REPO_ROOT
    removed, problems = reap(tree, check_only=args.check)
    if args.check and (removed or problems):
        return 1
    return 1 if problems else 0


# ---------------------------------------------------------------------------
# start
# ---------------------------------------------------------------------------


def sync_local_main(tree: Path) -> Optional[str]:
    """
    Fast-forward local main from the push remote. Returns an error string
    if local main has diverged (never auto-resolve that), else None.
    """
    git(tree, "fetch", PUSH_REMOTE)
    diverged = git(tree, "rev-list", "--count", f"{PUSH_REMOTE}/main..main").strip()
    if diverged != "0":
        return (f"local main has {diverged} commit(s) not on {PUSH_REMOTE}/main - "
                "reconcile before starting a sync")
    behind = git(tree, "rev-list", "--count", f"main..{PUSH_REMOTE}/main").strip()
    if behind != "0":
        if git(tree, "branch", "--show-current").strip() == "main":
            # main is checked out here; fetch refuses to move it, so
            # fast-forward via merge (refuses on local-change overlap).
            result = subprocess.run(
                ["git", "-C", str(tree), "merge", "--ff-only",
                 f"{PUSH_REMOTE}/main"],
                capture_output=True, text=True,
            )
            if result.returncode != 0:
                return (f"could not fast-forward checked-out main:\n"
                        f"{result.stderr.strip()}")
        else:
            git(tree, "fetch", PUSH_REMOTE, "main:main")
        print(f"  fast-forwarded local main by {behind} commit(s) from {PUSH_REMOTE}")
    return None


def cmd_start(args) -> int:
    tree = REPO_ROOT
    print("Syncing local main from remote...")
    err = sync_local_main(tree)
    if err:
        print(f"Error: {err}")
        return 1
    anchor = git(tree, "rev-parse", "--verify", args.anchor + "^{commit}").strip()
    anchor_desc = git(
        tree, "log", "-1", "--format=%h %s", anchor
    ).strip()

    worktree = Path(args.worktree).resolve() if args.worktree else (
        REPO_ROOT.parent / "lightpoint-merge"
    )
    if worktree.exists():
        print(f"Error: worktree path already exists: {worktree}")
        return 1
    branches = git(tree, "branch", "--list", SYNC_BRANCH).strip()
    if branches:
        print(f"Error: branch '{SYNC_BRANCH}' already exists; finish or delete it")
        return 1

    print(f"Anchor:   {anchor_desc}")
    print(f"Worktree: {worktree}")
    print(f"Branch:   {SYNC_BRANCH} (from main; merging anchor in, ours=LightPoint)")
    git(tree, "worktree", "add", "-b", SYNC_BRANCH, str(worktree), "main")
    out = git(tree, "-C", str(worktree), "submodule", "update", "--init", check=False)
    if out:
        print("  submodules initialized")

    if args.reject:
        commits = git(
            tree,
            "log",
            "--first-parent",
            "--reverse",
            "--format=- %h %s",
            f"main..{anchor}",
            check=False,
        ).strip()
        message = (
            "chore: Merge upstream (rejected)\n\n"
            "Rejected upstream commits (ancestry advanced, no content adopted):\n"
            f"{commits}\n\n"
            "Reason: TODO - fill in before PR.\n"
        )
        git(worktree, "merge", "-s", "ours", anchor, "-m", message)
        print("\nRecorded -s ours merge. Tree is byte-identical to main.")
        print("Next: edit the commit message reasons (git commit --amend),")
        print("cherry-pick any keepers, then: upstream_sync.py pr")
        return 0

    print("\nMerging anchor...")
    merge_result = subprocess.run(
        ["git", "merge", "--no-ff", anchor], cwd=worktree,
        capture_output=True, text=True,
    )
    print("\n".join(f"  {ln}"
                    for ln in merge_result.stdout.strip().splitlines()[-12:]))

    unmerged = git(worktree, "diff", "--name-only", "--diff-filter=U").split()
    if not unmerged:
        # No conflicts must mean the new merge commit exists: HEAD's second
        # parent is the anchor. Checking mere existence of HEAD^2 is not
        # enough (main's tip is often itself a merge commit), and a fatal
        # error or an already-up-to-date no-op also leaves no conflicts.
        merged = git(worktree, "rev-parse", "-q", "--verify", "HEAD^2",
                     check=False).strip()
        if merge_result.returncode != 0 or merged != anchor:
            print("\nError: merge did not complete (and left no conflicts "
                  "to resolve):")
            for ln in merge_result.stderr.strip().splitlines()[-8:]:
                print(f"  {ln}")
            return 1
        print("\nMerge is clean (no conflicts). Commit created by git merge.")
    else:
        print(f"\n{len(unmerged)} conflicted file(s). Running mechanical pre-resolution:")

        # Submodule pointer and .gitmodules: keep ours, loudly.
        for sub in SUBMODULE_PATHS:
            if sub in unmerged:
                git(worktree, "checkout", "--ours", "--", sub, check=False)
                git(worktree, "add", "--", sub)
                print(f"  kept LightPoint {sub} (upstream SDK bumps go through the")
                print(f"    znelson/open-x4-sdk fork first - see process doc)")

        reap(worktree, check_only=False)

        # Normalize translation files the merge brought in cleanly.
        unmerged = set(git(worktree, "diff", "--name-only", "--diff-filter=U").split())
        # --diff-filter=d: a file upstream deleted has nothing to
        # normalize, and a missing path aborts the normalizer's whole run.
        tx_changed = [
            f
            for f in git(worktree, "diff", "--name-only", "--diff-filter=d",
                          "HEAD", "--", "lib/I18n/translations").split()
            if f not in unmerged
        ]
        if tx_changed:
            print(f"  normalizing {len(tx_changed)} translation file(s) (--strip-identical):")
            norm_out = run(
                [sys.executable, str(NORMALIZER),
                 "--dir", str(worktree / "lib/I18n/translations"),
                 "--strip-identical"]
                + [str(worktree / f) for f in tx_changed],
                check=False,
            )
            for ln in norm_out.strip().splitlines():
                print(f"    {ln}")
            git(worktree, "add", "--", *tx_changed)

    remaining = git(worktree, "diff", "--name-only", "--diff-filter=U").split()
    print()
    if remaining:
        print(f"{len(remaining)} conflict(s) left for judgment:")
        for f in remaining:
            print(f"  {f}")
        print("\nResolve in the worktree (zdiff3 recommended), git add each file,")
        print("then 'git commit' (default merge message), then: upstream_sync.py verify")
    else:
        if git(worktree, "rev-parse", "-q", "--verify", "MERGE_HEAD", check=False).strip():
            print("All conflicts auto-resolved. Review 'git status', then 'git commit'.")
        print(f"Then: upstream_sync.py verify --tree {worktree}")
    return 0


# ---------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------


def cmd_verify(args) -> int:
    tree = Path(args.tree).resolve() if args.tree else REPO_ROOT
    failures = []

    unmerged = git(tree, "diff", "--name-only", "--diff-filter=U").split()
    if unmerged:
        print(f"FAIL: {len(unmerged)} unmerged file(s) remain")
        return 1

    print("i18n: regenerating...")
    out = run(
        [sys.executable, "scripts/gen_i18n.py", "lib/I18n/translations", "lib/I18n/"],
        cwd=tree,
        check=False,
    )
    if "Code generation complete" not in out:
        failures.append("i18n generation failed")
        print("  FAIL:\n" + "\n".join(f"    {ln}" for ln in out.splitlines()[-6:]))
    else:
        print("  ok")

    print("normalize_locales --check:")
    result = subprocess.run(
        [sys.executable, str(NORMALIZER), "--check",
         "--dir", str(tree / "lib/I18n/translations")],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        failures.append("locale normalization needed")
        print("  FAIL:\n" + "\n".join(f"    {ln}" for ln in result.stdout.splitlines()[-8:]))
    else:
        print("  ok")

    print("reap --check:")
    removed, problems = reap(tree, check_only=True, verbose=False)
    if removed or problems:
        failures.append("manifest violations (see above)")
    else:
        print("  ok")

    # Host-side gtest suite (seconds once built; see test/README). Runs even
    # with --quick - a fork-side test asserting upstream-changed behavior is
    # exactly what a sync batch breaks.
    print("host tests (ctest)...")
    test_ok = True
    for cmd in (["cmake", "-S", "test", "-B", "build/test"],
                ["cmake", "--build", "build/test", "-j"]):
        result = subprocess.run(cmd, cwd=tree, capture_output=True, text=True)
        if result.returncode != 0:
            failures.append("host test build failed")
            print("  FAIL (build):\n" + "\n".join(
                f"    {ln}" for ln in
                (result.stdout + result.stderr).splitlines()[-8:]))
            test_ok = False
            break
    if test_ok:
        result = subprocess.run(
            ["ctest", "--test-dir", "build/test", "--output-on-failure",
             "-j4"],
            cwd=tree, capture_output=True, text=True,
        )
        if result.returncode != 0:
            failures.append("host tests failed")
            print("  FAIL:\n" + "\n".join(
                f"    {ln}" for ln in result.stdout.splitlines()[-12:]))
        else:
            print("  " + next((ln.strip() for ln in
                               result.stdout.splitlines()
                               if "tests passed" in ln), "ok"))

    # Same exclusions as bin/clang-format-fix: script-generated and vendored
    # trees are never formatted.
    format_excluded = ("lib/EpdFont/builtinFonts/", "lib/Hyphenation/generated/",
                       "lib/uzlib/")
    changed = [
        f for f in git(tree, "diff", "--name-only", "main...HEAD").split()
        if f.endswith((".cpp", ".h", ".c")) and (tree / f).is_file()
        and not f.startswith(format_excluded)
    ]
    if changed:
        print(f"clang-format ({len(changed)} changed source file(s)):")
        result = subprocess.run(
            ["clang-format", "--dry-run", "--Werror"] + changed,
            cwd=tree, capture_output=True, text=True,
        )
        if result.returncode != 0:
            failures.append("clang-format violations (run bin/clang-format-fix)")
            print("  FAIL (first lines):\n"
                  + "\n".join(f"    {ln}" for ln in result.stderr.splitlines()[:6]))
        else:
            print("  ok")

    layout_touched = any(
        f.startswith(("lib/Typesetter/", "lib/Epub/")) for f in changed
    )
    if layout_touched:
        ours = git(tree, "grep", "-h", "FILE_VERSION = ",
                   "HEAD", "--", "lib/Typesetter/Typesetter/Section.h", check=False)
        mains = git(tree, "grep", "-h", "FILE_VERSION = ",
                    "main", "--", "lib/Typesetter/Typesetter/Section.h", check=False)
        if ours.strip() == mains.strip():
            print("NOTE: batch touches layout/serialization but Section FILE_VERSION")
            print("      is unchanged - decide whether cached sections need a bump.")

    if not args.quick:
        print("pio run (this takes a while)...")
        result = subprocess.run(
            ["pio", "run"], cwd=tree, capture_output=True, text=True
        )
        if result.returncode != 0:
            failures.append("pio run failed")
            print("  FAIL (last lines):\n"
                  + "\n".join(f"    {ln}" for ln in result.stdout.splitlines()[-15:]))
        else:
            print("  ok")
    else:
        print("pio run: SKIPPED (--quick)")

    print()
    if failures:
        print("VERIFY FAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("VERIFY PASSED" + (" (build skipped)" if args.quick else ""))
    return 0


# ---------------------------------------------------------------------------
# pr
# ---------------------------------------------------------------------------


def cmd_pr(args) -> int:
    tree = Path(args.tree).resolve() if args.tree else REPO_ROOT
    if git(tree, "rev-parse", "-q", "--verify", "MERGE_HEAD", check=False).strip():
        print("Error: merge in progress - commit it first")
        return 1
    branch = git(tree, "branch", "--show-current").strip()
    if branch != SYNC_BRANCH:
        print(f"Error: expected branch '{SYNC_BRANCH}', found '{branch}'")
        return 1

    merge_head = git(tree, "rev-parse", "HEAD^2", check=False).strip()
    prev_anchor = git(tree, "merge-base", "main", merge_head or "HEAD").strip()
    commits = git(
        tree, "log", "--first-parent", "--reverse", "--format=- %h %s",
        f"{prev_anchor}..{merge_head}" if merge_head else "main..HEAD",
        check=False,
    ).strip()
    # QUIET REFERENCES: this repo is a GitHub fork with issues disabled, so
    # a plain #N in a PR body that matches no local PR resolves against the
    # PARENT repo and posts a permanent "mentioned this" event on the
    # upstream thread. Upstream PR numbers ride along in every upstream
    # commit subject; wrap all of them in code spans, which are never
    # autolinked. (Commit messages pushed to forks do not create upstream
    # cross-references - only issue/PR bodies and comments do.)
    commits = re.sub(r"#(\d+)", r"`#\1`", commits)

    subject_body = git(tree, "log", "-1", "--format=%B", "HEAD").strip()
    rejected = "(rejected)" in subject_body.splitlines()[0]

    body = "## Upstream sync\n\n"
    body += f"Upstream commits {'REJECTED (-s ours)' if rejected else 'merged'}:\n\n"
    body += commits + "\n\n"
    if rejected:
        body += "### Rejection reasons\n\nSee the merge commit body.\n\n"
    body += (
        "### Resolutions\n\n"
        # The reminder avoids any literal number-sign-plus-digits itself;
        # "N" is a letter, which GitHub never autolinks.
        "<!-- QUIET REFERENCES: backtick upstream numbers like the list "
        "above does - a bare one in this body posts a permanent 'mentioned "
        "this' event on the upstream thread. -->\n"
        "- (fill in: conflicts resolved and judgment calls made)\n\n"
        "### Verification\n\n"
        "- [ ] upstream_sync.py verify passed (includes pio run)\n"
    )

    if args.dry_run:
        print("--- PR body (dry run) ---")
        print(body)
        return 0

    print(f"Pushing {SYNC_BRANCH} to {PUSH_REMOTE}...")
    git(tree, "push", PUSH_REMOTE, SYNC_BRANCH)
    out = run(
        ["gh", "pr", "create", "--repo", PR_REPO, "--base", "main",
         "--head", SYNC_BRANCH, "--title", "chore: Merge upstream",
         "--body", body],
        cwd=tree,
    )
    print(out.strip())
    print("\nREMINDER: merge with a merge commit - NEVER SQUASH.")
    print("To audit the batch commit by commit in Araxis before merging:")
    print(f"  python3 scripts/upstream_sync.py walk --tree {tree} -i --difftool")
    return 0


# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[1],
        epilog="Runbook (workflow, conventions, iron rules): "
               ".claude/skills/upstream-sync/SKILL.md. Rationale/history: "
               "Chromanaut/docs/plans/upstream-merge-process.md in the "
               "Obsidian vault.")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("status", help="anchor, drift, annotated candidates")
    p.add_argument("--fetch", action="store_true", help="git fetch upstream first")
    p.add_argument("--limit", type=int, default=30, help="candidates to show")
    p.set_defaults(fn=cmd_status)

    p = sub.add_parser("reap", help="enforce the removed-features manifest")
    p.add_argument("--tree", help="tree to operate on (default: this repo)")
    p.add_argument("--check", action="store_true", help="report only, change nothing")
    p.set_defaults(fn=cmd_reap)

    p = sub.add_parser("start", help="begin a sync batch in a worktree")
    p.add_argument("anchor", help="upstream commit to sync to")
    p.add_argument("--reject", action="store_true",
                   help="record an -s ours merge (adopt nothing)")
    p.add_argument("--worktree", help="worktree path (default: ../lightpoint-merge)")
    p.set_defaults(fn=cmd_start)

    p = sub.add_parser("verify", help="post-resolution checks + build")
    p.add_argument("--tree", help="merge worktree (default: this repo)")
    p.add_argument("--quick", action="store_true", help="skip pio run")
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("walk", help="per-commit walkthrough of how a batch landed")
    p.add_argument("merge", nargs="?", default="HEAD",
                   help="sync merge commit, inner or PR merge (default: HEAD)")
    p.add_argument("--tree", help="repo/worktree to read (default: this repo)")
    p.add_argument("--commit", help="show full diffs for this one batch commit")
    p.add_argument("--interactive", "-i", action="store_true",
                   help="step commit by commit with diffs on demand")
    p.add_argument("--batch-net", action="store_true",
                   help="with --commit, also print the whole batch's net "
                        "change to the commit's files (the fork-side view "
                        "is otherwise cut down to hunks attributed to that "
                        "commit)")
    p.add_argument("--difftool", action="store_true",
                   help="open diffs in Araxis Merge: d/l launch folder "
                        "comparisons, a file number launches a three-pane "
                        "(upstream | base | lightpoint), p+number launches "
                        "dual two-ways (upstream before|after beside "
                        "LightPoint before|after, cut down to the hunks "
                        "attributed to that commit; b+number for the whole "
                        "batch's net change), a+number launches all "
                        "three (tabs in one window if Merge is set to open "
                        "comparisons in tabs); with --commit, opens the "
                        "three-panes for that commit")
    p.set_defaults(fn=cmd_walk)

    p = sub.add_parser("pr", help="push branch and open the sync PR")
    p.add_argument("--tree", help="merge worktree (default: this repo)")
    p.add_argument("--dry-run", action="store_true", help="print body, do nothing")
    p.set_defaults(fn=cmd_pr)

    args = parser.parse_args()
    try:
        return args.fn(args)
    except (RuntimeError, ValueError, FileNotFoundError) as e:
        print(f"\nError: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

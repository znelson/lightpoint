---
name: upstream-sync
description: Drive an upstream (CrossPoint) catch-up sync batch using scripts/upstream_sync.py. Use when asked to sync with upstream, catch up with upstream/develop, merge upstream changes, plan the next sync batch, or reject an upstream span. Covers batch selection, the merge/rejection mechanics, conflict-resolution conventions, verification, and the never-squash invariant.
---

# Upstream Sync

LightPoint periodically catches up with upstream CrossPoint in small,
reviewable batches. Full process rationale and history:
`Vault/Chromanaut/docs/plans/upstream-merge-process.md` (Obsidian vault).
The mechanics live in `scripts/upstream_sync.py`; the removed-features
manifest is `.upstream-sync.yaml` at the repo root.

## Iron rules

1. **The sync PR is merged with a MERGE COMMIT - never squash.** Squashing
   flattens upstream's graph out of main and poisons every future sync.
2. **A batch is homogeneous**: all-adopt (normal merge) or all-reject
   (`--reject`, which uses `-s ours`). For a mostly-reject span with a few
   keepers: `--reject`, then cherry-pick the keepers onto the same branch.
3. **Never commit or push without the user's go-ahead**, per repo rules.
4. **Quiet references.** This repo is a GitHub fork with issues disabled:
   a plain `#N` in any PR body or comment that matches no local PR
   resolves against the PARENT repo (crosspoint-reader) and posts a
   permanent, undeletable "mentioned this" event on that upstream thread.
   In PR bodies and comments, wrap every upstream number in backticks
   (code spans are never autolinked) - the `pr` subcommand does this for
   the commit list, but hand-written Resolutions text must follow the same
   rule. Never write `owner/repo#N`, paste upstream URLs, or `@handles`.
   Commit messages are exempt (fork commit pushes do not create upstream
   cross-references - twenty history-carrying syncs confirm this).

## Workflow

1. **Plan**: `python3 scripts/upstream_sync.py status --fetch --limit 40`.
   Pick a batch of roughly 3-13 first-parent commits. Do not split related
   commits (a feature and its fix); prefer ending before a MANIFEST/SDK
   flagged commit unless the whole batch shares its verdict. Release tags
   shown by status are checkpoints a campaign converges to, never a reason
   to stretch a batch across an adopt/reject boundary.
2. **Start**: `python3 scripts/upstream_sync.py start <anchor>` (add
   `--reject` for an all-reject span). This creates the `upstream-merge`
   branch from main in a sibling worktree, merges the anchor in
   (ours = LightPoint), then auto-resolves the mechanical part: manifest
   path re-deletion, locale normalization, submodule pointer kept ours.
3. **Resolve** the remaining conflicts in the worktree. Conventions:
   - Removed features stay removed; strip new references upstream added to
     surviving files (menus, settings, main.cpp wiring, fontIds).
   - Relocated files (see manifest `relocations`): port upstream's change
     to the LightPoint path, then delete the old path.
   - LightPoint refactors win on form (TextAlign enum, HAL names,
     signed/unsigned, tr() strings); upstream wins on semantic fixes -
     keep both by expressing upstream's fix in LightPoint's form.
   - LightPoint-authored fixes round-trip from upstream; verify the
     existing port covered every file upstream touched, then keep ours.
   - If the batch changes layout/serialization behavior, consider bumping
     `Section::FILE_VERSION` (lib/Typesetter/Typesetter/Section.h) even if
     upstream did not - stale caches must regenerate.
   - Follow CLAUDE.md rules in resolutions: makeUniqueNoThrow over new,
     HalFile without explicit close, tr() for user-facing text.
4. **Commit the merge** (plain `git commit`, default merge message). For
   rejections, `--amend` the TODO reasons in the message body first.
5. **Verify**: `python3 scripts/upstream_sync.py verify --tree <worktree>`.
   Must pass including the host gtest suite and `pio run`. Use `--quick`
   (skips only the firmware build) for iteration, never for the final
   check. Fork-side tests assert behavior upstream may have changed - a
   test failure here usually means the test needs updating to the adopted
   behavior, not that the port is wrong; judge which.
6. **PR**: `python3 scripts/upstream_sync.py pr --tree <worktree>` (use
   `--dry-run` first; fill in the Resolutions section of the body with the
   actual judgment calls). The user merges the PR - with a merge commit.
   After opening it, give the user the audit command for their own
   terminal (the interactive Araxis walkthrough needs their TTY and
   cannot be run through a Claude session):
   `python3 scripts/upstream_sync.py walk --tree <worktree> -i --difftool`
7. **After merge**: delete the remote branch, remove the worktree, delete
   the local `upstream-merge` branch. Update the state section of the
   process doc in the vault (anchor, drift, next batch).

## Reviewing a batch

`python3 scripts/upstream_sync.py walk [merge]` walks a sync merge commit
(current or historical; PR merge commits auto-descend to the inner merge)
and classifies every touched file per upstream commit: adopted verbatim /
adapted / relocated / dropped per manifest / kept ours. For relocated
files the annotation says only whether the batch changed the fork-side
target at all; the "landed:" line under it and the p/l views say what
this commit contributed there.

Fork-side views are cut down to the commit being walked. The batch lands
as one merge, so each hunk of "LightPoint before -> after" is attributed
to batch commits: by blame (an added line traces through the merge's
upstream parent to that commit), or, for hunks blame cannot place (pure
deletions, lines retyped in resolution), by matching a non-trivial line,
unique within the file, that the commit itself added or removed. Each file's "landed:" line reports
the count, e.g. `3/7 hunks, 1 by line match, 1 with resolution edits,
shared with <sha>`. Hunks no commit accounts for (hand resolution,
reaper/normalizer edits, dropped fork divergence) are the residue,
listed once at the end of the walk - review it there instead of
cross-referencing every per-commit diff. Line-match attribution is a
heuristic; the whole-batch views (b) remain the ground truth.

Add `-i` to step interactively with diffs on demand: d = upstream diff,
l = landing attributed to this commit, b = whole-batch net landing,
r = remerge slice. `--commit <sha>` prints one commit's full detail
(upstream diff, attributed landing, remerge slice; `--batch-net` adds the
whole-batch landing).
`--difftool` opens Araxis Merge instead of printing: folder comparisons
for d/l/b, a file number opens a relocation-aware three-pane
(upstream edit | base | LightPoint result), and p+number opens dual
two-ways - upstream before|after beside LightPoint before|after, the
LightPoint "after" being the pre-merge file with only this commit's hunks
applied - which separates "what upstream changed" from "how our copy
moved". b+number is the same pair with the whole batch's net change.
a+number opens all three views at once; set Merge to open comparisons in
tabs and they stack as one window per file.

(This file doubles as the human runbook: the commands above are the
complete manual workflow.)
For the raw judgment layer alone, `git show --remerge-diff <merge>` shows
exactly what manual resolution changed versus the mechanical merge.

## Current campaign context

- Anchor moves along upstream's old master line first (the pre-switch
  runway, ending at `2754a5ff`). All of it predates upstream's
  open-x4-sdk -> freeink-sdk migration.
- Do NOT start any batch past `2754a5ff` without the user deciding the
  freeink-sdk question (follow the migration vs reject and own the SDK
  divergence). The migration commit is `ebebc6f2`, the first commit on
  upstream/develop.
- If a resolution removes a feature or rejects upstream work not yet in
  `.upstream-sync.yaml`, add a manifest entry in the same PR.

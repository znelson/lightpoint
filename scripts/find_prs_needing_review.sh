#!/usr/bin/env bash
# Lists open PRs with exactly one approval (needing one more to merge).
# Requires: gh CLI (https://cli.github.com/) authenticated with `gh auth login`

set -euo pipefail

REPO="${1:-crosspoint-reader/crosspoint-reader}"

echo "Fetching open PRs for $REPO..."

gh pr list \
  --repo "$REPO" \
  --state open \
  --json number,title,reviews \
  --limit 200 \
| python3 -c "
import json, sys

prs = json.load(sys.stdin)
results = []
for pr in prs:
    latest = {}
    for r in pr.get('reviews', []):
        author = r.get('author', {}).get('login', '')
        latest[author] = r.get('state')
    approvals = sum(1 for s in latest.values() if s == 'APPROVED')
    if approvals == 1:
        results.append((pr['number'], pr['title']))

if not results:
    print('No PRs found with exactly 1 approval.')
else:
    print(f'Found {len(results)} PR(s) with exactly 1 approval:\n')
    for num, title in results:
        print(f'  #{num}: {title}')
"

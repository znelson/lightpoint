#!/usr/bin/env bash
# Lists open PRs with exactly one approval (needing one more to merge).
# Requires: gh CLI (https://cli.github.com/) authenticated with `gh auth login`

set -euo pipefail

REPO="${1:-crosspoint-reader/crosspoint-reader}"

echo "Fetching open PRs for $REPO..."

gh pr list \
  --repo "$REPO" \
  --state open \
  --json number,title,author,reviews \
  --limit 200 \
| python3 -c "
import json, sys

MY_USER = '${GH_USER:-}'
if not MY_USER:
    import subprocess
    try:
        MY_USER = subprocess.check_output(
            ['gh', 'api', 'user', '--jq', '.login'],
            text=True
        ).strip()
    except Exception:
        print('Error: could not determine your GitHub username.', file=sys.stderr)
        print('Set GH_USER env var or ensure gh is authenticated.', file=sys.stderr)
        sys.exit(1)

prs = json.load(sys.stdin)
needs_my_review = []
my_prs = []
approved_by_me = []

for pr in prs:
    pr_author = pr.get('author', {}).get('login', '')
    latest = {}
    for r in pr.get('reviews', []):
        reviewer = r.get('author', {}).get('login', '')
        latest[reviewer] = r.get('state')
    approvers = [a for a, s in latest.items() if s == 'APPROVED']
    num_approvals = len(approvers)

    if pr_author == MY_USER:
        approver_names = [f'@{a}' for a in approvers]
        my_prs.append((pr['number'], pr['title'], num_approvals, approver_names))
    elif num_approvals == 1:
        if approvers[0] == MY_USER:
            approved_by_me.append((pr['number'], pr['title']))
        else:
            needs_my_review.append((pr['number'], pr['title'], approvers[0]))

# --- Your PRs ---
if my_prs:
    print(f'Your PRs ({len(my_prs)}):\n')
    for num, title, count, names in sorted(my_prs, key=lambda x: x[2]):
        if count == 0:
            status = 'no approvals'
        elif count == 1:
            status = f'1 approval ({names[0]})'
        else:
            sep = ', '
            status = f'{count} approvals ({sep.join(names)})'
        print(f'  #{num}: {title}  [{status}]')
else:
    print('You have no open PRs.')

print()

# --- PRs you can review ---
if needs_my_review:
    print(f'Needs your review ({len(needs_my_review)} PR(s) approved by someone else):\n')
    for num, title, approver in needs_my_review:
        print(f'  #{num}: {title}  (approved by @{approver})')
else:
    print('No PRs awaiting your review.')

print()

# --- PRs you already approved ---
if approved_by_me:
    print(f'Needs another reviewer ({len(approved_by_me)} PR(s) you already approved):\n')
    for num, title in approved_by_me:
        print(f'  #{num}: {title}')
else:
    print('No PRs where you are the sole approver.')
"

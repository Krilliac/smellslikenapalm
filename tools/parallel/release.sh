#!/usr/bin/env bash
#
# tools/parallel/release.sh — mark a subsystem claim complete and push, for
# concurrent Claude Code sessions. See CLAUDE_PARALLEL.md for the full protocol.
#
# Usage:
#   tools/parallel/release.sh <subsystem>            # push the session branch only
#   tools/parallel/release.sh <subsystem> --merge    # push AND merge into main
#
# What it does:
#   1. Flips this subsystem's most recent ACTIVE block in PARALLEL_WORK.md to ✅.
#   2. Commits that change.
#   3. Pushes the session branch with --force-with-lease (refuses to clobber
#      commits you haven't seen — fetch+rebase and retry if it's rejected).
#   4. With --merge: fast-forwards/merges into main after the push. This is the
#      explicit opt-in required before touching main — CI must be green first.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <subsystem> [--merge]" >&2
    exit 2
fi

SUB="$1"
MERGE=0
[[ "${2:-}" == "--merge" ]] && MERGE=1

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"
COORD="${REPO_ROOT}/PARALLEL_WORK.md"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"

if [[ -f "${COORD}" ]]; then
    # Flip the LAST active block matching this subsystem to completed.
    awk -v sub="${SUB}" '
        BEGIN { }
        { lines[NR]=$0 }
        /^- Subsystem: / { if ($3==sub) cur_is_sub=1; else cur_is_sub=0 }
        /^- Status: 🟢/ && cur_is_sub==1 { last_active=NR }
        END {
            for (i=1;i<=NR;i++) {
                if (i==last_active) print "- Status: ✅ completed"
                else print lines[i]
            }
        }
    ' "${COORD}" > "${COORD}.tmp" && mv "${COORD}.tmp" "${COORD}"
    git add "${COORD}"
    git commit --quiet -m "parallel: release ${SUB}" || echo "[release] nothing to commit"
fi

echo "[release] pushing ${BRANCH} with --force-with-lease…"
attempt=0
until git push --force-with-lease -u origin "${BRANCH}"; do
    attempt=$((attempt+1))
    if [[ ${attempt} -ge 4 ]]; then
        echo "[release] push failed after ${attempt} attempts." >&2
        echo "[release] if rejected by --force-with-lease, run: git fetch origin && git rebase origin/main, then retry." >&2
        exit 1
    fi
    sleep $((2 ** attempt))
    echo "[release] retry ${attempt}…"
done

if [[ ${MERGE} -eq 1 ]]; then
    echo "[release] --merge requested: merging ${BRANCH} into main."
    echo "[release] Ensure CI is green on ${BRANCH} before this lands."
    git fetch origin main --quiet
    git checkout main
    git pull --ff-only origin main
    git merge --no-ff "${BRANCH}" -m "Merge ${BRANCH} into main (${SUB})"
    git push origin main
    git checkout "${BRANCH}"
    echo "[release] ✅ merged ${SUB} into main."
else
    echo "[release] ✅ pushed ${BRANCH}. A human or another session does the merge."
fi

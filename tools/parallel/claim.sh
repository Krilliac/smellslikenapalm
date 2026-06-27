#!/usr/bin/env bash
#
# tools/parallel/claim.sh — claim a subsystem before editing, for concurrent
# Claude Code sessions. See CLAUDE_PARALLEL.md for the full protocol.
#
# Usage:
#   tools/parallel/claim.sh <subsystem-name> "<files/dirs>" "<brief description>"
#
# Example:
#   tools/parallel/claim.sh net-control "src/Network/* src/Protocol/*" "handshake"
#
# What it does:
#   1. Rebases your branch on the latest origin/main (so existing claims are visible).
#   2. Ensures you're on a claude/* session branch (creates claude/<subsystem> if not).
#   3. Warns if any target file is already claimed by another ACTIVE session.
#   4. Appends a session block to PARALLEL_WORK.md and commits it.
#
# Do not hand-edit PARALLEL_WORK.md.

set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "usage: $0 <subsystem-name> \"<files/dirs>\" \"<description>\"" >&2
    exit 2
fi

SUB="$1"
FILES="$2"
DESC="$3"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"
COORD="${REPO_ROOT}/PARALLEL_WORK.md"

# Session id: short, stable-ish, unique per invocation.
SID="${SUB}-$(git rev-parse --short HEAD 2>/dev/null || echo nogit)-$$"

echo "[claim] fetching + rebasing on origin/main…"
git fetch origin main --quiet || echo "[claim] warning: fetch failed (offline?) — continuing with local state"
if git rev-parse --verify --quiet origin/main >/dev/null; then
    git rebase origin/main || {
        echo "[claim] rebase hit conflicts — resolve, 'git add', 'git rebase --continue', then re-run." >&2
        exit 1
    }
fi

# Ensure we're on a claude/* session branch.
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [[ "${CURRENT_BRANCH}" != claude/* ]]; then
    TARGET="claude/${SUB}"
    echo "[claim] switching to session branch ${TARGET}"
    git checkout -B "${TARGET}"
    CURRENT_BRANCH="${TARGET}"
fi

# Seed the coordinator if missing.
if [[ ! -f "${COORD}" ]]; then
    {
        echo "# Parallel Work Coordinator"
        echo
        echo "Auto-managed by tools/parallel/. Do not hand-edit."
        echo
        echo "## Sessions"
        echo
    } > "${COORD}"
fi

# Conflict warning: is any target path already under an ACTIVE claim?
for path in ${FILES}; do
    if awk -v p="${path}" '
        /^## Session: / { active=0 }
        /^- Status: 🟢/ { active=1 }
        /^- Status: ✅/ { active=0 }
        /^- Files: / && active==1 { if (index($0, p)) { found=1 } }
        END { exit(found?0:1) }
    ' "${COORD}"; then
        echo "[claim] ⚠️  WARNING: '${path}' appears in an ACTIVE claim already."
        echo "[claim]    Check tools/parallel/status.sh and coordinate before editing."
    fi
done

# Append the claim block.
TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
{
    echo "## Session: ${SID}"
    echo "- Status: 🟢 active"
    echo "- Subsystem: ${SUB}"
    echo "- Branch: ${CURRENT_BRANCH}"
    echo "- Files: ${FILES}"
    echo "- Description: ${DESC}"
    echo "- Claimed: ${TS}"
    echo
} >> "${COORD}"

git add "${COORD}"
git commit --quiet -m "parallel: claim ${SUB} (${FILES})" || echo "[claim] nothing to commit"

echo "[claim] ✅ claimed '${SUB}' on ${CURRENT_BRANCH}."
echo "[claim]    Files: ${FILES}"
echo "[claim]    Run tools/parallel/release.sh ${SUB} when done."

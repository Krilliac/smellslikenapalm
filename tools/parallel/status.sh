#!/usr/bin/env bash
#
# tools/parallel/status.sh — show the state of concurrent Claude Code sessions.
#
# What it does:
#   - Prints every session block recorded in the coordinator (PARALLEL_WORK.md).
#   - Counts active (🟢) vs completed (✅) sessions.
#   - Runs a conflict check: flags any file/dir path claimed by more than one
#     ACTIVE session at the same time.
#
# Usage:
#   tools/parallel/status.sh
#
# See CLAUDE_PARALLEL.md for the full protocol. Do not hand-edit PARALLEL_WORK.md;
# use claim.sh / release.sh.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COORD="${REPO_ROOT}/PARALLEL_WORK.md"

if [[ ! -f "${COORD}" ]]; then
    echo "No PARALLEL_WORK.md yet — no active parallel sessions."
    echo "Run tools/parallel/claim.sh to register the first claim."
    exit 0
fi

active=$(grep -c '^- Status: 🟢' "${COORD}" 2>/dev/null || true)
done=$(grep -c '^- Status: ✅' "${COORD}" 2>/dev/null || true)

echo "==================================================================="
echo " Parallel session status   (active: ${active:-0}, completed: ${done:-0})"
echo "==================================================================="
echo

# Print the coordinator body (session blocks) as-is for the operator to read.
sed -n '/^## Sessions/,$p' "${COORD}" || cat "${COORD}"

echo
echo "------------------------------------------------------------------"
echo " Conflict check (files claimed by >1 ACTIVE session)"
echo "------------------------------------------------------------------"

# Collect "Files:" lines that belong to active session blocks. A block is active
# when its most recent Status line is 🟢. We emit one path per line, then look for
# paths that appear under more than one distinct active session id.
awk '
    /^## Session: / { sid=$3; active=0 }
    /^- Status: 🟢/ { active=1 }
    /^- Status: ✅/ { active=0 }
    /^- Files: / && active==1 {
        sub(/^- Files: /, "")
        n=split($0, parts, /[[:space:]]+/)
        for (i=1;i<=n;i++) if (parts[i] != "") print parts[i] "\t" sid
    }
' "${COORD}" | sort | awk -F'\t' '
    { if ($1==prevpath && $2!=prevsid) { print "  CONFLICT: " $1 "  (" prevsid " vs " $2 ")"; found=1 }
      prevpath=$1; prevsid=$2 }
    END { if (!found) print "  none" }
'

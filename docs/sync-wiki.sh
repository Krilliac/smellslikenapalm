#!/usr/bin/env bash
#
# docs/sync-wiki.sh — mirror the in-repo docs/ tree to the GitHub wiki.
#
# The canonical documentation home is docs/ in this repo. The GitHub wiki
# (https://github.com/Krilliac/smellslikenapalm/wiki) is a *mirror* generated from
# it, so docs and code stay reviewable in the same PR. This script pushes the repo
# docs into that wiki, or just checks they're in sync.
#
# Usage:
#   docs/sync-wiki.sh check     # report drift between docs/ and the wiki (no writes)
#   docs/sync-wiki.sh sync      # copy docs/ -> wiki clone, commit, and push
#
# Env:
#   WIKI_REMOTE   Override the wiki git URL (default derives from 'origin').
#   WIKI_TMP      Working clone dir (default: a mktemp dir).
#
# Notes:
#   - GitHub wiki page name == filename without ".md"; "_Sidebar.md" is the nav.
#   - Sub-directory pages (docs/re/, docs/hardening/) are mirrored with their paths;
#     GitHub wikis serve them as nested pages.
#   - Requires: git, and push access to the wiki repo for 'sync'.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCS_DIR="${REPO_ROOT}/docs"
MODE="${1:-check}"

derive_wiki_remote() {
    if [[ -n "${WIKI_REMOTE:-}" ]]; then
        echo "${WIKI_REMOTE}"
        return
    fi
    local origin
    origin="$(git -C "${REPO_ROOT}" remote get-url origin 2>/dev/null || true)"
    if [[ -z "${origin}" ]]; then
        echo "error: no 'origin' remote and WIKI_REMOTE unset" >&2
        exit 1
    fi
    # foo/bar.git -> foo/bar.wiki.git ; foo/bar -> foo/bar.wiki.git
    echo "${origin%.git}.wiki.git"
}

WIKI_REMOTE="$(derive_wiki_remote)"
WIKI_TMP="${WIKI_TMP:-$(mktemp -d)}"
cleanup() { [[ -n "${WIKI_TMP:-}" && -d "${WIKI_TMP}" ]] && rm -rf "${WIKI_TMP}"; }
trap cleanup EXIT

echo "[sync-wiki] wiki remote: ${WIKI_REMOTE}"
echo "[sync-wiki] cloning wiki…"
git clone --quiet --depth 1 "${WIKI_REMOTE}" "${WIKI_TMP}" || {
    echo "[sync-wiki] error: could not clone the wiki. Create the first wiki page on" >&2
    echo "[sync-wiki] GitHub once (so the .wiki repo exists), or set WIKI_REMOTE." >&2
    exit 1
}

# Copy all markdown (and the RE .txt notes) from docs/ into the wiki clone,
# preserving sub-directory layout.
copy_docs() {
    ( cd "${DOCS_DIR}" && find . \( -name '*.md' -o -name '*.txt' \) -print0 ) \
        | while IFS= read -r -d '' rel; do
            mkdir -p "${WIKI_TMP}/$(dirname "${rel}")"
            cp "${DOCS_DIR}/${rel}" "${WIKI_TMP}/${rel}"
        done
}

case "${MODE}" in
    check)
        copy_docs
        if git -C "${WIKI_TMP}" status --porcelain | grep -q .; then
            echo "[sync-wiki] DRIFT: the wiki is out of sync with docs/. Changes:"
            git -C "${WIKI_TMP}" status --short
            echo "[sync-wiki] run 'docs/sync-wiki.sh sync' to publish."
            exit 1
        fi
        echo "[sync-wiki] ✅ wiki is in sync with docs/."
        ;;
    sync)
        copy_docs
        if ! git -C "${WIKI_TMP}" status --porcelain | grep -q .; then
            echo "[sync-wiki] ✅ nothing to publish — already in sync."
            exit 0
        fi
        git -C "${WIKI_TMP}" add -A
        git -C "${WIKI_TMP}" commit --quiet -m "docs: sync from repo docs/ ($(git -C "${REPO_ROOT}" rev-parse --short HEAD))"
        git -C "${WIKI_TMP}" push --quiet origin HEAD
        echo "[sync-wiki] ✅ published docs/ to the wiki."
        ;;
    *)
        echo "usage: $0 {check|sync}" >&2
        exit 2
        ;;
esac

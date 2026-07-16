#!/usr/bin/env bash
# Git-Bash compatibility bridge for the authoritative PowerShell controller.
# This script never edits configuration, chooses a build, or owns process identity.

set -uo pipefail

usage() {
  cat <<'EOF'
Usage: server_watchdog.sh [--loop|--once|--watch]

  --loop   Re-run the controller's safe Ensure action (default).
  --once   Run Ensure once and preserve its exit code.
  --watch  Follow controller status and logs until interrupted.
EOF
}

case "$(uname -s 2>/dev/null || true)" in
  MINGW*|MSYS*|CYGWIN*) ;;
  *)
    printf '%s\n' "server_watchdog.sh requires Git Bash/MSYS/Cygwin on Windows" >&2
    exit 1
    ;;
esac

powershell_bin="$(command -v powershell.exe 2>/dev/null || true)"
if [[ -z "$powershell_bin" || ! -f "$powershell_bin" || ! -x "$powershell_bin" ]]; then
  printf '%s\n' "powershell.exe is required but was not found on PATH" >&2
  exit 127
fi
if ! command -v cygpath >/dev/null 2>&1; then
  printf '%s\n' "cygpath is required to pass an exact Windows controller path" >&2
  exit 127
fi

script_dir="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || {
  printf '%s\n' "could not resolve the watchdog script directory" >&2
  exit 1
}
controller="$script_dir/server_control.ps1"
if [[ ! -f "$controller" || -L "$controller" ]]; then
  printf 'server controller not found: %s\n' "$controller" >&2
  exit 1
fi
controller_windows="$(cygpath -aw -- "$controller")" || {
  printf 'could not convert controller path for Windows: %s\n' "$controller" >&2
  exit 1
}
if [[ -z "$controller_windows" ]]; then
  printf 'cygpath returned an empty controller path: %s\n' "$controller" >&2
  exit 1
fi

run_controller() {
  local action="$1"
  "$powershell_bin" \
    -NoLogo \
    -NoProfile \
    -NonInteractive \
    -File "$controller_windows" \
    -Action "$action"
}

mode="${1:---loop}"
if (( $# > 1 )); then
  usage >&2
  exit 64
fi

case "$mode" in
  --once|once|ensure)
    run_controller Ensure
    exit $?
    ;;
  --watch|watch)
    run_controller Watch
    exit $?
    ;;
  --loop|loop)
    interval="${RS2V_WATCHDOG_INTERVAL_SECONDS:-12}"
    if [[ ! "$interval" =~ ^[1-9][0-9]{0,3}$ ]] || (( interval > 3600 )); then
      printf 'invalid RS2V_WATCHDOG_INTERVAL_SECONDS: %s\n' "$interval" >&2
      exit 64
    fi
    trap 'exit 130' INT
    trap 'exit 143' TERM
    while true; do
      run_controller Ensure
      result=$?
      if (( result != 0 )); then
        printf 'server Ensure failed with exit code %d; watchdog stopped\n' "$result" >&2
        exit "$result"
      fi
      sleep "$interval" || {
        result=$?
        printf 'watchdog sleep failed with exit code %d; watchdog stopped\n' \
          "$result" >&2
        exit "$result"
      }
    done
    ;;
  -h|--help|help)
    usage
    exit 0
    ;;
  *)
    printf 'unknown watchdog mode: %s\n' "$mode" >&2
    usage >&2
    exit 64
    ;;
esac

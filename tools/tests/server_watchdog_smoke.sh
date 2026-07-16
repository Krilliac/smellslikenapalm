#!/usr/bin/env bash

set -euo pipefail

source_watchdog="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/server_watchdog.sh"
temp_root="$(mktemp -d "${TMPDIR:-/tmp}/rs2v-watchdog smoke.XXXXXX")"

cleanup() {
  case "$temp_root" in
    "${TMPDIR:-/tmp}"/rs2v-watchdog\ smoke.*) rm -rf -- "$temp_root" ;;
    *) printf 'refusing unsafe smoke cleanup: %s\n' "$temp_root" >&2 ;;
  esac
}
trap cleanup EXIT

mkdir -p -- "$temp_root/tools" "$temp_root/mock-bin"
cp -- "$source_watchdog" "$temp_root/tools/server_watchdog.sh"
: > "$temp_root/tools/server_control.ps1"

cat > "$temp_root/mock-bin/powershell.exe" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$WATCHDOG_TEST_ARGUMENTS"
exit "${WATCHDOG_TEST_EXIT_CODE:-0}"
EOF
chmod +x "$temp_root/mock-bin/powershell.exe"

export PATH="$temp_root/mock-bin:$PATH"
export WATCHDOG_TEST_ARGUMENTS="$temp_root/powershell arguments.txt"
watchdog="$temp_root/tools/server_watchdog.sh"
expected_controller="$(cygpath -aw -- "$temp_root/tools/server_control.ps1")"
assertions=0

assert_equal() {
  local expected="$1"
  local actual="$2"
  local message="$3"
  if [[ "$expected" != "$actual" ]]; then
    printf 'ASSERTION FAILED: %s (expected=%q actual=%q)\n' \
      "$message" "$expected" "$actual" >&2
    exit 1
  fi
  assertions=$((assertions + 1))
}

assert_invocation() {
  local expected_action="$1"
  mapfile -t arguments < "$WATCHDOG_TEST_ARGUMENTS"
  assert_equal "7" "${#arguments[@]}" "controller invocation argument count"
  assert_equal "-NoLogo" "${arguments[0]}" "NoLogo switch"
  assert_equal "-NoProfile" "${arguments[1]}" "NoProfile switch"
  assert_equal "-NonInteractive" "${arguments[2]}" "NonInteractive switch"
  assert_equal "-File" "${arguments[3]}" "File switch"
  assert_equal "$expected_controller" "${arguments[4]}" "quoted controller path"
  assert_equal "-Action" "${arguments[5]}" "Action switch"
  assert_equal "$expected_action" "${arguments[6]}" "controller action"
}

WATCHDOG_TEST_EXIT_CODE=0 "$watchdog" --once
assert_invocation Ensure

WATCHDOG_TEST_EXIT_CODE=0 "$watchdog" --watch
assert_invocation Watch

set +e
WATCHDOG_TEST_EXIT_CODE=37 "$watchdog" --once >/dev/null 2>&1
once_result=$?
WATCHDOG_TEST_EXIT_CODE=23 RS2V_WATCHDOG_INTERVAL_SECONDS=1 \
  "$watchdog" --loop >/dev/null 2>&1
loop_result=$?
WATCHDOG_TEST_EXIT_CODE=29 RS2V_WATCHDOG_INTERVAL_SECONDS=1 \
  "$watchdog" >/dev/null 2>&1
default_result=$?
RS2V_WATCHDOG_INTERVAL_SECONDS=invalid "$watchdog" --loop >/dev/null 2>&1
interval_result=$?
"$watchdog" --unknown >/dev/null 2>&1
unknown_result=$?
set -e

assert_equal "37" "$once_result" "one-shot controller exit code"
assert_equal "23" "$loop_result" "loop stops on controller failure"
assert_equal "29" "$default_result" "default mode preserves the safe loop"
assert_equal "64" "$interval_result" "invalid interval fails closed"
assert_equal "64" "$unknown_result" "unknown mode fails closed"

if grep -Eiq 'Set-Content|build[\\/]Debug|tasklist|server_live' "$watchdog"; then
  printf '%s\n' "ASSERTION FAILED: legacy mutation/start logic remains" >&2
  exit 1
fi
assertions=$((assertions + 1))

printf 'server_watchdog smoke tests passed: %d assertions\n' "$assertions"

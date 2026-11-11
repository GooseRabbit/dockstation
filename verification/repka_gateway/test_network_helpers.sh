#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
source "$REPOSITORY_ROOT/deployment/repka/network_helpers.sh"

TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT
FAKE_BIN="$TEMP_ROOT/bin"
mkdir -p "$FAKE_BIN"

run_case() {
    local description="$1" expected_status="$2" expected_text="$3"
    shift 3
    local output actual_status
    if output="$("$@" 2>&1)"; then actual_status=0; else actual_status=$?; fi
    if [[ "$actual_status" -ne "$expected_status" || "$output" != *"$expected_text"* ]]; then
        printf 'FAIL: %s (expected=%s actual=%s)\n%s\n' "$description" "$expected_status" "$actual_status" "$output" >&2
        exit 1
    fi
    printf 'PASS: %s\n' "$description"
}

run_case "require_command accepts present command" 0 "доступна" require_command bash
run_case "require_command reports missing command" 2 "не найдена" require_command dockstation-command-that-does-not-exist
run_case "require_command requires arguments" 2 "хотя бы одну" require_command
run_case "require_command accepts an explicit executable path" 0 "доступна" require_command /bin/bash

cat > "$TEMP_ROOT/os-release" <<'OS_RELEASE'
NAME="Test Linux"
PRETTY_NAME="Test Linux 1.2"
ID=testlinux
VERSION_ID="1.2"
UNTRUSTED=$(touch "$TEMP_ROOT/should-not-exist")
OS_RELEASE
run_case "read_os_release parses expected fields" 0 "Test Linux 1.2 (ID=testlinux) (версия=1.2)" read_os_release "$TEMP_ROOT/os-release"
if [[ -e "$TEMP_ROOT/should-not-exist" ]]; then
    printf 'FAIL: read_os_release executed content from os-release.\n' >&2
    exit 1
fi
printf 'PASS: read_os_release does not execute file contents\n'
run_case "read_os_release handles missing file" 1 "недоступен для чтения" read_os_release "$TEMP_ROOT/missing"
printf 'NAME=\"\"\nID=test\n' > "$TEMP_ROOT/os-release-invalid"
run_case "read_os_release rejects incomplete file" 1 "не содержит NAME" read_os_release "$TEMP_ROOT/os-release-invalid"

cat > "$FAKE_BIN/uname" <<'UNAME'
#!/usr/bin/env bash
[[ "$1" == "-m" ]] || exit 64
printf '%s\n' "${FAKE_MACHINE:-aarch64}"
UNAME
cat > "$FAKE_BIN/getconf" <<'GETCONF'
#!/usr/bin/env bash
[[ "$1" == "LONG_BIT" ]] || exit 64
printf '%s\n' "${FAKE_WORD_SIZE:-64}"
GETCONF
chmod +x "$FAKE_BIN/uname" "$FAKE_BIN/getconf"
PATH="$FAKE_BIN:$PATH"
export PATH
run_case "check_architecture reads machine and word size" 0 "CPU=aarch64, разрядность=64" check_architecture "$FAKE_BIN/uname" "$FAKE_BIN/getconf"
FAKE_WORD_SIZE=128; export FAKE_WORD_SIZE
run_case "check_architecture rejects unknown bitness" 1 "неожиданное значение LONG_BIT" check_architecture "$FAKE_BIN/uname" "$FAKE_BIN/getconf"
unset FAKE_WORD_SIZE
run_case "check_architecture handles missing getconf" 1 "разрядность недоступна" check_architecture "$FAKE_BIN/uname" "$TEMP_ROOT/missing-getconf"
run_case "check_architecture handles missing uname" 2 "не найдена" check_architecture "$TEMP_ROOT/missing-uname" "$FAKE_BIN/getconf"

run_case "print report PASS" 0 "[PASS] Итоговая сетевая диагностика" print_diagnostics_report 0
run_case "print report WARN" 1 "[WARN] Итоговая сетевая диагностика" print_diagnostics_report 1
run_case "print report FAIL" 2 "[FAIL] Итоговая сетевая диагностика" print_diagnostics_report 2
run_case "print report rejects missing argument" 2 "ровно один аргумент" print_diagnostics_report
run_case "print report rejects extra argument" 2 "ровно один аргумент" print_diagnostics_report 0 extra
run_case "print report rejects unknown code" 2 "допустимы 0, 1 или 2" print_diagnostics_report 3
run_case "print report rejects nonnumeric code" 2 "некорректный общий код" print_diagnostics_report error
run_case "print report rejects leading zero" 2 "допустимы 0, 1 или 2" print_diagnostics_report 01

printf '\nAll network helper tests passed.\n'

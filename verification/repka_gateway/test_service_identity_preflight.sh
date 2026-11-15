#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CHECKER="$HERE/../../repka/check_service_identity.sh"
TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT

cat > "$TMP/getent" <<'SH'
#!/usr/bin/env bash
[[ "$#" -eq 2 ]] || exit 64
case "$1:$2" in
    group:dockstation)
        [[ "${FAKE_GROUP_MODE:-valid}" != missing ]] || exit 2
        printf 'dockstation:x:%s:\n' "${FAKE_GID:-995}"
        ;;
    passwd:dockstation)
        [[ "${FAKE_USER_MODE:-valid}" != missing ]] || exit 2
        printf 'dockstation:x:%s:%s:DockStation service:%s:%s\n' \
            "${FAKE_UID:-995}" "${FAKE_USER_GID:-995}" "${FAKE_HOME:-/nonexistent}" "${FAKE_SHELL:-/usr/sbin/nologin}"
        ;;
    *) exit 2 ;;
esac
SH
chmod +x "$TMP/getent"
export DOCKSTATION_GETENT_COMMAND="$TMP/getent"

expect_pass() {
    if bash "$CHECKER" >/dev/null; then return 0; fi
    echo "FAIL: expected identity preflight success ($*)" >&2
    exit 1
}
expect_fail() {
    if bash "$CHECKER" >/dev/null 2>&1; then
        echo "FAIL: expected identity preflight failure ($*)" >&2
        exit 1
    fi
}

FAKE_GROUP_MODE=missing FAKE_USER_MODE=missing expect_pass accounts-absent
FAKE_USER_MODE=missing expect_pass group-only
expect_pass valid-account-pair
FAKE_GID=0 expect_fail root-group
FAKE_UID=0 expect_fail root-user
FAKE_USER_GID=996 expect_fail mismatched-primary-group
FAKE_SHELL=/bin/bash expect_fail interactive-shell
FAKE_HOME=/root expect_fail unexpected-home
FAKE_GID=invalid expect_fail nonnumeric-group-id
FAKE_UID=invalid expect_fail nonnumeric-user-id
FAKE_USER_MODE=missing FAKE_GROUP_MODE=missing expect_pass missing-both

cat > "$TMP/getent-user-only" <<'SH'
#!/usr/bin/env bash
if [[ "$1:$2" == passwd:dockstation ]]; then
  printf 'dockstation:x:995:995:DockStation service:/nonexistent:/usr/sbin/nologin\n'
  exit 0
fi
exit 2
SH
chmod +x "$TMP/getent-user-only"
DOCKSTATION_GETENT_COMMAND="$TMP/getent-user-only" expect_fail user-without-group

cat > "$TMP/getent-error" <<'SH'
#!/usr/bin/env bash
exit 1
SH
chmod +x "$TMP/getent-error"
DOCKSTATION_GETENT_COMMAND="$TMP/getent-error" expect_fail getent-database-error

cat > "$TMP/getent-multiple" <<'SH'
#!/usr/bin/env bash
if [[ "$1:$2" == group:dockstation ]]; then
  printf 'dockstation:x:995:\ndockstation:x:996:\n'
  exit 0
fi
exit 2
SH
chmod +x "$TMP/getent-multiple"
DOCKSTATION_GETENT_COMMAND="$TMP/getent-multiple" expect_fail ambiguous-group-record

printf 'PASS: service identity preflight accepts only the expected non-root, non-login account layout.\n'

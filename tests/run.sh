#!/usr/bin/env bash
#
# tests/run.sh - build and run the curl package suites.
#
#   1. builds tests/fixture_server.c and starts it on a free loopback port;
#   2. builds tests/unit.ct and tests/chain.ct with -Wall -Wextra, failing on
#      any compiler warning;
#   3. runs both and requires a `SUMMARY passed=N failed=0` line from each;
#   4. statically checks that every public method name appears in a suite.
#
#   bash tests/run.sh
#   CT=/path/to/ct bash tests/run.sh
#
# The default compiler is the worktree build that carries the Phase 20 type
# alias (ct.pkg requires 0.20.3, which is not released yet). Point CT at any
# ct that understands `pub type`.
#
# All temporary files live under $TMPDIR and are removed on exit.
#
# License: MIT (see LICENSE).

set -u

here="$(cd "$(dirname "$0")" && pwd)"
pkg="$(cd "$here/.." && pwd)"
CT="${CT:-/tmp/ctbuild/ct}"
CC="${CC:-cc}"

if [ ! -x "$CT" ]; then
    echo "ct not found or not executable: $CT" >&2
    echo "set CT=/path/to/ct (ct >= 0.20.3, with type aliases)" >&2
    exit 1
fi

work="${TMPDIR:-/tmp}/ct-curl-test.$$"
rm -rf "$work"
mkdir -p "$work" || exit 1

srv_pid=""
cleanup() {
    if [ -n "$srv_pid" ]; then
        kill "$srv_pid" 2>/dev/null
        wait "$srv_pid" 2>/dev/null
    fi
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

pass=0
fail=0
ok()  { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %s: %s\n' "$1" "$2"; }

echo "ct: $CT ($("$CT" --version 2>/dev/null || true))"
echo "cc: $CC"
echo

echo "== fixture server =="
if ! "$CC" -O0 -Wall -Wextra -o "$work/fixture_server" \
        "$here/fixture_server.c" >"$work/server-build.log" 2>&1; then
    bad "fixture build" "see log"
    cat "$work/server-build.log" >&2
    printf 'curl: %d passed, %d failed\n' "$pass" "$fail"
    exit 1
fi

"$work/fixture_server" "$work/port" &
srv_pid=$!

i=0
while [ ! -s "$work/port" ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done

if [ ! -s "$work/port" ]; then
    bad "fixture start" "no port file after 5s"
    printf 'curl: %d passed, %d failed\n' "$pass" "$fail"
    exit 1
fi

port="$(cat "$work/port")"
CURL_BASE="http://127.0.0.1:$port"
export CURL_BASE
ok "fixture listening on $CURL_BASE"
echo

for suite in unit chain; do
    echo "== $suite =="
    if ! (
        cd "$work" || exit 1
        "$CT" build -I "$pkg" --ldflags '-lcurl' --cflags '-Wall -Wextra' \
            "$here/$suite.ct" -o "$work/$suite"
    ) >"$work/$suite.build.log" 2>&1; then
        bad "$suite build" "see log"
        cat "$work/$suite.build.log" >&2
        continue
    fi
    if grep -q 'warning:' "$work/$suite.build.log"; then
        bad "$suite warning" "$(grep -m1 'warning:' "$work/$suite.build.log")"
    else
        ok "$suite build (zero warnings)"
    fi

    "$work/$suite" >"$work/$suite.out" 2>"$work/$suite.err"
    run_rc=$?
    cat "$work/$suite.out"
    if [ -s "$work/$suite.err" ]; then
        cat "$work/$suite.err" >&2
    fi

    summary="$(sed -n 's/^SUMMARY passed=\([0-9][0-9]*\) failed=\([0-9][0-9]*\)$/\1 \2/p' "$work/$suite.out")"
    if [ -z "$summary" ]; then
        bad "$suite run" "no SUMMARY line (exit $run_rc)"
    else
        s_pass="${summary% *}"
        s_fail="${summary#* }"
        if [ "$run_rc" -eq 0 ] && [ "$s_fail" -eq 0 ]; then
            ok "$suite assertions ($s_pass passed)"
        else
            bad "$suite assertions" "exit $run_rc, $s_fail failed"
            grep '^FAIL ' "$work/$suite.out" || true
        fi
    fi
    echo
done

echo "== demo =="
if ! (
    cd "$work" || exit 1
    "$CT" build -I "$pkg" --ldflags '-lcurl' --cflags '-Wall -Wextra' \
        "$pkg/demo/main.ct" -o "$work/demo"
) >"$work/demo.build.log" 2>&1; then
    bad "demo build" "see log"
    cat "$work/demo.build.log" >&2
else
    if grep -q 'warning:' "$work/demo.build.log"; then
        bad "demo warning" "$(grep -m1 'warning:' "$work/demo.build.log")"
    else
        ok "demo build (zero warnings)"
    fi
    "$work/demo" "$CURL_BASE/hello" >"$work/demo.out" 2>&1
    sed -E 's/^url: .*/url: <url>/' "$work/demo.out" >"$work/demo.norm"
    if diff -u "$pkg/demo/expected.out" "$work/demo.norm" >"$work/demo.diff" 2>&1; then
        ok "demo output matches expected.out"
    else
        bad "demo output" "diff vs expected.out"
        cat "$work/demo.diff" >&2
    fi
fi
echo

echo "== method coverage (static: names called from a suite) =="
names="openCurl setUrl setRequest setUserAgent setUserPwd setFollow setMaxRedirs setTimeout setConnectTimeout setVerifyPeer setVerifyHost setCainfo addHeader setPostFields setBodyLimit setWriteFunction setHeaderFunction perform get post reset clearBody close destroy status message errorText responseCode effectiveUrl contentType totalTime sizeDownload body bodyLen globalInit globalCleanup version strerror"
total=0
missing=""
for m in $names; do
    total=$((total + 1))
    if ! grep -q "$m(" "$here/unit.ct" && ! grep -q "$m(" "$here/chain.ct"; then
        missing="$missing $m"
    fi
done
if [ -z "$missing" ]; then
    ok "$total/$total public method names exercised"
else
    bad "method coverage" "never called:$missing"
fi

echo
printf 'curl: %d passed, %d failed\n' "$pass" "$fail"
if [ "$fail" -ne 0 ]; then
    exit 1
fi
exit 0

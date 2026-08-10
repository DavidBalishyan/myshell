#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LSH=$ROOT/lsh
export LSH_TEST_ROOT=$ROOT/tests
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/lsh-tests.XXXXXX")
cleanup() {
    case $TEST_TMP in */lsh-tests.*) rm -rf -- "$TEST_TMP";; esac
}
trap cleanup EXIT HUP INT TERM

passed=0
failed=0
for test_file in "$ROOT"/tests/cases/*.sh; do
    name=${test_file##*/}
    work=$TEST_TMP/${name%.sh}
    mkdir -p "$work/cwd" "$work/results"

    set +e
    (cd "$work/cwd" && dash "$test_file" one "two three" '') \
        >"$work/results/dash.out" 2>"$work/results/dash.err"
    dash_status=$?
    (cd "$work/cwd" && "$LSH" "$test_file" one "two three" '') \
        >"$work/results/lsh.out" 2>"$work/results/lsh.err"
    lsh_status=$?
    set -e

    if [ "$dash_status" -eq "$lsh_status" ] && cmp -s "$work/results/dash.out" "$work/results/lsh.out"; then
        printf 'ok   %s\n' "$name"
        passed=$((passed + 1))
    else
        printf 'FAIL %s (dash=%s, lsh=%s)\n' "$name" "$dash_status" "$lsh_status"
        diff -u "$work/results/dash.out" "$work/results/lsh.out" || true
        if [ -s "$work/results/lsh.err" ]; then
            printf '%s\n' '--- lsh stderr ---'
            sed -n '1,120p' "$work/results/lsh.err"
        fi
        failed=$((failed + 1))
    fi
done

# Status-only diagnostics have implementation-specific wording.
set +e
"$LSH" -c 'a_command_that_does_not_exist' >/dev/null 2>/dev/null
not_found=$?
set -e
if [ "$not_found" -eq 127 ]; then
    printf 'ok   command-not-found status\n'; passed=$((passed + 1))
else
    printf 'FAIL command-not-found status (got %s)\n' "$not_found"; failed=$((failed + 1))
fi

printf '\n%s passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]

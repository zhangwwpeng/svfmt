#!/usr/bin/env bash
# Every input must format successfully (parse, format, re-parse, lexical
# equivalence and convergence checks all run inside svfmt), and its output must
# match the golden file in <expected dir>/<name>.sv.
# usage: run_format_inputs.sh <svfmt> <inputs dir> <expected dir>
set -u
svfmt="$1"
dir="$2"
expected="$3"
fail=0
err=$(mktemp)
out=$(mktemp)
trap 'rm -f "$err" "$out"' EXIT
for f in "$dir"/*.sv; do
  name=$(basename "$f")
  if ! "$svfmt" --failsafe_success=false --verify_convergence "$f" >"$out" 2>"$err"; then
    echo "FAIL: $f"; head -5 "$err"
    fail=1
    continue
  fi
  if [ ! -f "$expected/$name" ]; then
    echo "MISSING EXPECTED: $expected/$name (run tests/update_expected.sh)"
    fail=1
  elif ! cmp -s "$out" "$expected/$name"; then
    echo "CHANGED: $name differs from $expected/$name"
    diff "$expected/$name" "$out" | head -10
    fail=1
  fi
done
exit $fail

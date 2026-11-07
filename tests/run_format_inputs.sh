#!/usr/bin/env bash
# Every input must format successfully: parse, format, re-parse, lexical
# equivalence and convergence checks all run inside svfmt.
set -u
svfmt="$1"
dir="$2"
fail=0
for f in "$dir"/*.sv; do
  if ! "$svfmt" --failsafe_success=false --verify_convergence "$f" >/dev/null 2>/tmp/svfmt_err; then
    echo "FAIL: $f"; head -5 /tmp/svfmt_err
    fail=1
  fi
done
exit $fail

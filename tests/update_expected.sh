#!/usr/bin/env bash
# Regenerates the golden outputs for tests/inputs (run after an intended
# formatting change, or after adding an input; review the diff in git).
# usage: tests/update_expected.sh <svfmt>
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
svfmt="$1"
for f in "$root"/tests/inputs/*.sv; do
  name=$(basename "$f")
  "$svfmt" --failsafe_success=false --verify_convergence "$f" > "$root/tests/expected/$name"
done
echo "wrote $(ls "$root"/tests/expected | wc -l) files to tests/expected"

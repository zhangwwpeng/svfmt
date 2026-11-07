#!/usr/bin/env bash
# Differential test: compare svfmt output with verible-verilog-format.
# usage: run_diff.sh <svfmt> <dir> [-v]
svfmt="$1"; dir="$2"; verbose="$3"
verible="${VERIBLE_FORMAT:-verible-verilog-format}"
command -v "$verible" >/dev/null || { echo "$verible not found, skipping"; exit 0; }
tmp=$(mktemp -d)
pass=0; fail=0
for f in "$dir"/*.sv; do
  b=$(basename "$f")
  "$verible" "$f" > "$tmp/want_$b" 2>/dev/null
  "$svfmt" "$f" > "$tmp/got_$b" 2>"$tmp/err_$b"
  if cmp -s "$tmp/want_$b" "$tmp/got_$b"; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "DIFF $b"
    if [ "$verbose" = "-v" ]; then diff "$tmp/want_$b" "$tmp/got_$b" | head -40; head -c 600 "$tmp/err_$b"; fi
  fi
done
echo "verible-diff: $pass same, $fail different"
rm -rf "$tmp"
[ $fail -eq 0 ]

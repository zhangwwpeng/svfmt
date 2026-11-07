# Testing

`ctest --test-dir build` runs three suites:

| Test | What it checks |
| --- | --- |
| `format_inputs` | Every `tests/inputs/*.sv` formats with `--failsafe_success=false --verify_convergence`. That covers parsing, the lossless bridge, re-parse, lexical equivalence and idempotence. |
| `verible_diff` | Every `tests/diff/*.sv` formats byte-identical to `verible-verilog-format`. Skipped if Verible is not installed. |
| `verible_cases` | About 2100 inputs extracted from Verible's formatter tests (`tests/verible_cases.jsonl`). Fails if fewer than `--min-same` outputs match Verible. |

## Verible test corpus

`tools/extract_verible_tests.py` reads the `{input, expected}` tables in
Verible's `formatter_*test.cc` files. Re-run it against a newer Verible
checkout to refresh the corpus:

```sh
python3 tools/extract_verible_tests.py ~/workspace/ref/verible/verible/verilog/formatting \
    tests/verible_cases.jsonl
```

Many tests set a non-default `FormatStyle`, so the runner uses the Verible
binary with default flags as the oracle. The tables' `expected` values are
kept only for reference. Results fall into these buckets:

- `same`: identical output;
- `diff`: both tools succeed but the output differs;
- `rejected`: svfmt refused and echoed the input;
- `crash`;
- `verible_rejects`: Verible itself fails, so the case is ignored.

`--out DIR` writes every non-matching case to a file, and
`tools/show_cases.py` prints them.

Current result: 1593 same, 120 diff, 375 rejected, 0 crash.

## Built-in safety checks

These run on every format, as in Verible:

- the output must parse with slang;
- the output's non-whitespace token sequence must equal the input's;
- with `--verify_convergence` (the default), formatting the output again must
  change nothing.

If a check fails, the input is printed unchanged.

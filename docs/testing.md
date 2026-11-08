# Testing

`ctest --test-dir build` runs these suites. Configure the oracle with
`-DSVFMT_VERIBLE_FORMAT=<path>`, using the Verible release that matches the
ported commit (v0.0-4298-g085eef05). Older releases differ in places such as
body-level parameter alignment.

| Test | What it checks |
| --- | --- |
| `format_inputs` | Every `tests/inputs/*.sv` formats with `--failsafe_success=false --verify_convergence`. That covers parsing, the lossless bridge, re-parse, lexical equivalence and idempotence. |
| `corpus` | Only if `-DSVFMT_CORPUS_DIR=<dir>` is set. See below. |

## Corpus test (real code)

`tests/run_corpus.py <build> <dir>` runs every `.sv/.svh/.v/.vh` file under
`<dir>`. Semantic safety is checked first, and any failure here is a bug:

- **Formatted:** `svfmt-lexcmp` (`tools/lexcmp.cpp`) re-lexes input and output
  with slang's raw lexer, with no preprocessing and no code shared with the
  formatter. Only whitespace may differ. Formatting the output again must
  not change it.
- **Refused** (parse errors, unsupported constructs): the output must be the
  input, byte for byte. svfmt may refuse to format; it must never change the
  meaning of the code, including code with syntax errors.
- No crashes and no timeouts.

Parity with Verible is reported as statistics only. `--out DIR` lists every
failure and every file that differs from Verible. `--no-parity` skips Verible,
`--cases DIR` saves each failure (input, outputs, logs, `cmd.sh` repro), and
`--jsonl FILE` writes one result per file.

[corpus-safety-test.md](corpus-safety-test.md) is the task brief handed to a
helper agent to run the safety test on `../sv_test` and report root causes.

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

Current result (oracle v0.0-4298): 2064 same, 0 crash.

## Built-in safety checks

These run on every format, as in Verible:

- the output must parse with slang;
- the output's non-whitespace token sequence must equal the input's;
- with `--verify_convergence` (the default), formatting the output again must
  change nothing.

If a check fails, the input is printed unchanged.

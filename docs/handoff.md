# Handoff notes (2026-11-10)

State at commit time; read this first when resuming.

## Goals and results
- Safety first: formatting never changes the token stream. A file that
  cannot be formatted safely is echoed unchanged (exit 1 with
  --failsafe_success=false). Since 2026-11-08 there is no Verible
  comparison; the style is our own, starting from Verible's.
- Corpus ../sv_test (26.5k files, 01-09): 0 semantic_change /
  not_idempotent / crash / timeout, with default flags and with the nvim
  alignment flags (build-dev/full-nvim/run_all.sh). Remaining rejections in
  01-08 were checked by hand: illegal on purpose
  (tests/corpus_expected_rejects.txt), tool dialects, slang strictness, or
  multi-file. 09 (LRM excerpts) is mostly prose; fragment mode was dropped
  (2026-11-10).
- Style reviews: random corpus files formatted with the nvim flags and
  reviewed by vibe, findings checked and fixed by hand
  (reports/style-review-N, untracked; regression inputs
  tests/inputs/style_review*.sv, sv_constructs.sv).

## Environment
- Verible source (reference for porting): ~/workspace/ref/verible (085eef05);
  oracle binary ~/workspace/ref/verible-bin/.../verible-verilog-format
  (only to compare behavior by hand now).
- The editor uses ~/.local/bin/svfmt (copied from build-dev after a clean
  corpus run) through conform.nvim with --failsafe_success=false.

## Commands
```sh
cmake --build build -j && ctest --test-dir build        # golden tests
tests/update_expected.sh build/svfmt                     # after an intended change
sh build-dev/full/run_all.sh                             # corpus, default flags
sh build-dev/full-nvim/run_all.sh                        # corpus, nvim flags
build/svfmt --dump_cst x.sv; build/svfmt --show_token_partition_tree x.sv
```
Corpus runs use --jobs 4 (the user found 8 too heavy). Don't rebuild
build-dev while a corpus run uses it.

## Where things live
- src/svfmt/slang-analyzer.cc: the slang bridge (CST conversion, unknown
  macro stubs, conditional directives, tab expansion, parse-mode retries).
- src/verible/verilog/formatting/*.cc: the formatter (tree-unwrapper,
  token-annotator, align, formatter); edit directly.
- tools/lexcmp.cpp: the independent token-stream check used by the corpus
  runner.

## Decisions in force
- Macro arguments are not formatted inside (spacing and line breaks between
  top-level arguments are).
- Single-file formatting only (includes are placeholders).
- Tabs become spaces up to the next indentation stop.
- A parameter or port list with at most one element stays on one line when
  it fits; any longer list expands the whole header.
- Subroutine headers (2026-11-10): a function's ports pack like a C
  function's parameters (clang-format style: continuation aligned after "(",
  or all ports on the next line when that gives fewer lines); a task's ports
  go one per line with ");" on its own line, like a module's, when the header
  does not fit. Both apply to declarations and class prototypes.
- Search-limit rejections count as ok (default budget 300000).

## Open items
1. --try_wrap_long_lines is off by default (a style decision for the user).
   It wraps long single-partition lines (e.g. a long assign) that are
   otherwise kept as they are. As of 2026-11-10 it is convergence-clean on
   a 600-file sample (was 11 rejections): a trailing `define comment stays
   on its line, no line break is introduced before ',' or an inline block
   comment, and a line that overflows only because of its trailing comment,
   or that the search cannot make fit, keeps its original spacing.
   Cost: ~40% more formatting time on the sample.
2. Known non-convergent leftovers (safe: rejected, input echoed; Verible has
   the same): `endif in the middle of a wrapped expression (VeeR
   beh_lib.sv), comments after "`else // c" (rv_tracer te_reg.sv), `infer`
   alignment flips on leading-comma lists and continued parameter lists,
   a comment line between "end" and "else".
3. Style choices kept from Verible (reviewers keep reporting them): single
   statements on the line of if/always/repeat when they fit, "module tb ();",
   enum members one per line, leading-comma lists indent the first element
   one more level, "type#(T)" in typedefs vs "type #(T)" in declarations,
   dist lists with more than one item expand.

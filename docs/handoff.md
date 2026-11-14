# Handoff notes (2026-11-14)

State at pause time (2026-11-14, after the review-rounds and alignment
commit); read this first when resuming.

## Goals and results
- Safety first: formatting never changes the token stream. A file that
  cannot be formatted safely is echoed unchanged (exit 1 with
  --failsafe_success=false). Since 2026-11-08 there is no Verible
  comparison; the style is our own, starting from Verible's.
- Corpus ../sv_test (26.5k files, 01-09), last full run on 2026-11-13
  (prior to the two final `try_wrap_long_lines` edits in `formatter.cc`);
  `full` and `full-nvim` re-run on 2026-11-14 after the alignment fix in
  item 7: 0 semantic_change, 0 crash, 0 timeout, 0 not_idempotent in both.
  - `full-nvim` (`build-dev/full-nvim/run_all.sh`): 0 not_idempotent,
    0 timeout.
  - `full` (`build-dev/full/run_all.sh`): 0 not_idempotent, 0 timeout
    (the 3 `bp_cce*.sv` in `08-weird-syntax` are fixed, item 7).
  - `full-trywrap` (`build-dev/full-trywrap/run_all.sh`): not re-run after
    the item 7 fix; the previous run had 1 timeout and 22 not_idempotent
    (the 3 `bp_cce*.sv` in 08, 4 in 02, 2 in 03, 13 in 05). The 3 in 08
    and the 4 in 02 (`cv32e40p_decoder.sv`, `te_priority.sv`, two
    `prim_sha2.sv`) are fixed and verified individually; the remaining 15
    in 03/05 are diagnosed in item 1 below.
- Remaining rejections in 01-08 were checked by hand: illegal on purpose
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
- The editor uses ~/.local/bin/svfmt (installed from `83625fe` on
  2026-11-14, after the clean corpus runs) through conform.nvim with
  --failsafe_success=false.
- Working tree status: clean except review artifacts (`agy*.md`, `pi.md`,
  `vibe.md`, `reports/`), which are untracked and not part of the repo.
  `build-dev/svfmt` is built from `83625fe` and passes `ctest --test-dir
  build-dev` (`format_inputs`).

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
   otherwise kept as they are. Fixed on 2026-11-10..14: a trailing `define
   comment (EOL or block) stays on its line; no line break is introduced
   before ',' or an inline block comment; a line that overflows only
   because of its trailing comment, or that the search cannot make fit
   (unless it contains a multi-line token whose line widths shift), keeps
   its original spacing; and `formatted_lines_.back()` clears `kPreserve`
   before `ContinuationCommentAligner::HandleLine` (fixing `02-sv-designs`:
   `cv32e40p_decoder.sv`, `te_priority.sv`, and both `prim_sha2.sv`).
   Remaining 15 `not_idempotent` files (all safe: rejected; 03: 2, 05: 13)
   share one root cause: pass 1 wraps inside a single `UnwrappedLine` with
   `wrap_spaces`, and the introduced `\n` makes `TreeUnwrapper` split them
   into separate `UnwrappedLine`s at base indent on pass 2:
   - `/*...*/` followed on the same line by code (`nvdla/.../fp_format_cvt.v`)
     or `;` (`vtr/.../mkDelayWorker32B.v`);
   - `(* ... *)` followed on the same line by `wire ...` (12
     `vtr/.../vexriscv/*.v` files);
   - `;` followed on the same line by a `//...` EOL comment inside a
     `policy: wrap` partition (`nvdla/.../memory.v`, `$display(...)`, where
     `OverflowsOnlyByTrailingComment` only checks `kFitOnLineElseExpand`
     and `LayoutOptimizer` wraps inside the leaf).
   Cost: ~40% more formatting time.
2. Safety checks (2026-11-12): besides the re-parse token comparison,
   svfmt compares its final output with the original input using the raw
   lexer check of src/lexeq (the same code as svfmt-lexcmp): inactive
   `ifdef branches, `define/`pragma line ends, UDP tables. Before that, the
   inactive-branch pre-pass and `define line ends were only checked by the
   corpus runner.
3. Known non-convergent leftovers (safe: rejected, input echoed; Verible has
   the same): `endif in the middle of a wrapped expression (VeeR
   beh_lib.sv), comments after "`else // c" (rv_tracer te_reg.sv), `infer`
   alignment flips on leading-comma lists and continued parameter lists,
   a comment line between "end" and "else".
4. Style choices kept from Verible (reviewers keep reporting them): single
   statements on the line of if/always/repeat when they fit, "module tb ();",
   enum members one per line, leading-comma lists indent the first element
   one more level, "type#(T)" in typedefs vs "type #(T)" in declarations,
   dist lists with more than one item expand, "L : stmt" statement labels.
5. Review round agy_v2 / vibe v2 (2026-11-12, tests/inputs/review_agy_v2.sv):
   fixed goto repetition "b[->1:2]", "inside {", "with [", "s_eventually
   [1:3]", nested case / if-else in a case item (no longer flattened by
   alignment; indented below the label), rows with a forced line break
   ("f(1,  // c" then "2);") never take part in any alignment group,
   checker "rand" declarations after a comment, "L: if ... end else",
   "[$clog2(" return types with a comment-only port list, continuation
   comments after a multi-line block comment or a line outside --lines,
   comment lines before a macro statement, "`uvm_field_int (" keeps its space
   (the safety check reads "`M (" as a call only without the space).
   Left as they are (safe): `ifdef inside if/while/do-while conditions and
   inside macro arguments is laid out poorly; UDP "(0 1)" edges and a
   macro-closed attribute "`END_ATTR" are rejected.
6. Review rounds code-review / pi / agy_v3 (2026-11-13, tests/inputs/
   review_pi.sv, review_agy_v3*.sv, directive_same_line.sv). Safety check
   (src/lexeq) holes closed: line breaks inside macro arguments are
   compared; "\\F (x)" escaped macro names follow slang (one space before
   "(" still opens formals; "`\\F" and "`F" are one macro); a macro defined
   function-like in any branch is function-like; "table" is a UDP table only
   inside a primitive; "`line" / "`include" may move to the start of a line,
   not away; a `define line ending in "// c \" compares the next line as
   body (other tools continue there; slang does not). Also: `pragma keeps
   its tabs, inactive branches in "verilog_format: off" stay as they are and
   keep CRLF, "#1.5 'hff" keeps its space, directive lines take only their
   own macro arguments, alignment never pads between a macro name and "(",
   'begin' from a macro is not merged, ports with "$clog2(" stay whole,
   comment lines between uvm _begin/_end are indented.
   Known leftovers (all safe: rejected, input echoed): --define macros and
   macros defined both ways across branches can be rejected; "`ifdef (X)"
   inside macro arguments; some `define continuations followed by block
   comments. Hand-written scanners in slang-analyzer.cc (TabsToSpaces,
   ScanConditionalDirectives, StubDefinitions) and the heuristic lexeq
   check remain the main risk; the proposed next step is a differential
   check against slang's own preprocessor (expanded token streams under
   several define sets) plus mutation testing of corpus files.
7. Default-flag regression fixed (2026-11-14): 3 files in `08-weird-syntax`
   (`black-parrot` `bp_cce.sv`, `bp_cce_msg.sv`, `bp_cce_wrapper.sv`) were
   `not_idempotent` under `infer` alignment. Cause:
   `IgnoreWithinPortDeclarationPartitionGroup` (`src/verible/verilog/
   formatting/align.cc`) did not ignore `SyntaxKind::MacroUsage` (e.g.
   `` `declare_bp_proc_params(bp_params_p) `` inside `#(...)`), so
   `ParameterDeclarationColumnSchemaScanner` reserved the macro's `kParenGroup`
   as column `{1}`, padded the other rows, and `formatter.cc` stripped the
   alignment padding before `(`. On pass 2 `OriginalIsAligned()` saw
   mismatched spacing before `(` and `infer` flipped to `kFlushLeft`.
   Fixed: `MacroUsage` rows are ignored in
   `IgnoreWithinPortDeclarationPartitionGroup` and
   `IgnoreWithinStructUnionMemberPartitionGroup`, and
   `ParameterDeclarationColumnSchemaScanner::Visit` returns early for anything
   but `ParameterDeclaration` / `TypeParameterDeclaration` /
   `ParameterDeclarationStatement`. Regression input:
   `tests/inputs/param_macro_alignment.sv` (fails `--verify_convergence` on
   the old binary, passes now).

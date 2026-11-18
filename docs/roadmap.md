# Roadmap

## Done

- [x] slang + Abseil build; Verible's generic formatting core vendored
- [x] Bridge: lossless token stream, CST with list and group nodes, directive
      and macro nodes, generated typed conversion
- [x] Verible token enums generated from `verilog.y`
- [x] token-annotator, tree-unwrapper, align, formatter ported to the
      slang CST
- [x] Verible CLI and flags, plus `--incdir`, `--define`, `--dump_cst`
- [x] Verification: re-parse, lexical equivalence, convergence
- [x] Excerpt parse modes and automatic module-body retry
- [x] Undefined macros: tolerated, with stub re-parse
- [x] `` `ifdef ``: all-branches parse with fallback
- [x] v1.0: corpus `../sv_test` (26,522 files, 01-09) with default flags and
      with the nvim alignment flags: 0 semantic_change / not_idempotent /
      crash / timeout
- [x] v1.0: differential preprocessor check against slang's preprocessor
      (26,522 files, several define sets): 0 differences; mutation test of
      the safety checks (500-file sample): 0 failures

## Known limitations (v1.0)

1. `--try_wrap_long_lines` is off by default. With it, 15 files are
   `not_idempotent` and 1 times out; all are safe rejections (the input is
   echoed). The cause is diagnosed in
   [handoff.md](handoff.md) item 1; not being fixed by decision.
2. Known safe rejections (input echoed, no semantic change):
   - `` `endif `` in the middle of a wrapped expression, comments after
     `` `else // c ``, `infer` alignment flips on leading-comma and continued
     parameter lists, a comment line between `end` and `else`;
   - files that need `--define` macros, macros defined both ways across
     branches, `` `ifdef (X) `` inside macro arguments, some `` `define ``
     continuations followed by block comments.
3. The external differential preprocessor tool masks macro-produced values
   because it compares files with different paths. The formatter itself
   compares active macro expansions under the same source name and checks
   direct `` `__LINE__ `` locations across all branches. Indirect line-sensitive
   macro uses in inactive branches under a different define set remain a
   limitation. Mutation testing has run on a 500-file sample, not the full
   corpus.

## Later

- Library API (`svfmt::Format(text, style)`) for editor/LSP integration.
- Config file (`.svfmt.toml`).

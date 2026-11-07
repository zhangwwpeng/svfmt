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
- [x] Differential test infrastructure (Verible corpus, 1593/2088 identical)

## Next (ordered by test impact)

1. Remaining `diff` cases, grouped by source test file:
   - `formatter_disable_test` (mostly generic constructs)
   - `formatter_align_test`
   - `formatter_function_task_test`
   - `formatter_module_test`
   - `formatter_wrap_style_test`
   - `formatter_macro_test`
   - `formatter_class_package_test`
2. Unmapped constructs:
   - UDP primitives and tables
   - specify blocks
   - clocking blocks
   - covergroups, coverpoints and bins
   - assertion and property bodies
   - `randomize() with {}`
   - constraint blocks
   - DPI import/export
   - modports
   - `bind`
3. System and macro call stacking (`MacroCallReshaper`) for nested calls.
4. `--lines` partial formatting tests.
5. Corpus runs on real code (OpenTitan, Ibex, CVA6) and documenting the
   remaining deviations.

## Later

- Library API (`svfmt::Format(text, style)`) for editor/LSP integration.
- Config file (`.svfmt.toml`).

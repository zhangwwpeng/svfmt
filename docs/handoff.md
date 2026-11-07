# Handoff notes (2026-11-07)

State at commit time; read this first when resuming.

## Results
- Verible test corpus: 1656/2111 identical to verible-verilog-format
  v0.0-4298-g085eef05 (74 diff, 381 rejected, 0 crash).
- Local real-code corpus (206 files from ~/workspace/verilog-ethernet,
  solouvm, tree-sitter-systemverilog): 0 safety failures, 183/204 identical
  to Verible.

## Environment
- Verible source (reference for porting): ~/workspace/ref/verible (085eef05)
- Matching oracle binary:
  ~/workspace/ref/verible-bin/verible-v0.0-4298-g085eef05-macOS/bin/verible-verilog-format
  Configure: cmake -S . -B build -DSVFMT_VERIBLE_FORMAT=<that path>
- The system verible-verilog-format on PATH is an OLDER version; don't use it
  as the oracle.

## Commands
```sh
cmake --build build -j && ctest --test-dir build
VERIBLE_FORMAT=<oracle> python3 tests/run_verible_cases.py build/svfmt tests/verible_cases.jsonl --out build/cases_report
python3 tools/show_cases.py build/cases_report/diff.txt <test-file-substring> 5
python3 tests/run_corpus.py build <corpus dir> --verible <oracle> --out build/corpus_report
build/svfmt --dump_cst x.sv; build/svfmt --show_token_partition_tree x.sv
```

## Where things live
- Edit src/verible/verilog/formatting/*.cc directly. The one-off port scripts
  are gone, and the original Verible files are in ~/workspace/ref/verible.
- The bridge is src/svfmt/slang-analyzer.cc: ConvertSpecial() holds the
  synthetic groups and special cases. Lists come from the generated code
  (tools/gen_slang_cst.py).

## Open items / next steps
1. Changing IsVerilogBinaryExpressionKind (assignments are no longer
   binary) dropped 2 Verible cases (1646 -> 1644 before the macro-argument
   work). Investigate.
2. Remaining diffs by test file: function_task, align, wrap_style, macro,
   module, class_package (see build/cases_report after a run).
3. Known Verible quirks we don't mimic: "`define FOO_``BAR" spacing, and
   "interface class" indentation.
4. Corpus is at ../sv_test (26.5k files). The safety test is delegated to
   vibe/agy using docs/corpus-safety-test.md; they write
   reports/corpus-safety-<date>.md and must not change code. Fix the
   reported safety failures first (semantic_change / rejected_but_changed /
   crash / timeout / not_idempotent), then parity. A smoke run on
   09-lrm-examples found 1 crash (clause-21/ex-21-086.sv).
5. Not yet mapped: clocking blocks, covergroup bins details, assertion and
   property bodies, DPI, bind, constraint details, randomize-with spacing.
6. Known corpus leftovers (safe by default: --verify_convergence rejects
   them, input echoed): `endif in the middle of a wrapped expression
   (VeeR beh_lib.sv; Verible rejects it as a syntax error), and comments
   after "`else // c" re-indenting between passes (rv_tracer te_reg.sv;
   Verible has the same non-convergence). Reports: reports/round*/
   (untracked).
7. More known leftovers from 01-sv-compliance (all safe by default):
   enum alignment under `infer` and comments after a ")"-with-comment
   between parameter list and instance name do not converge (Verible: same);
   an `endif after unparsable disabled text gets glued to the next token
   (rejected by output verification); very long parameter lists hit the
   line-wrap search limit; macro arguments that build escaped identifiers
   via `` are rejected by re-parse (Verible formats them and changes the
   expansion). From 03-verilog-designs: leading-comma named port
   connections with EOL comments under `named_port_alignment=infer` flip
   from align to preserve on the second pass (Verible: same).

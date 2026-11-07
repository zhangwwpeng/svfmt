# Deviations from Verible

Intentional or structural differences. Porting bugs do not belong here; they
appear as `diff` cases in the test report.

## Inputs svfmt rejects

- **Constructs that are not legal SystemVerilog but that Verible's grammar
  accepts.** Examples are a bare `$foo(...);` or `` `MACRO(...) `` call at
  module scope, which slang reports as `expected member`. About 320 of
  Verible's test inputs are like this. svfmt prints them unchanged.
- **Unresolvable `` `include ``.** slang reports a missing include as an
  error. Use `--incdir`.

## Preprocessor

- If the all-branches parse fails (see preprocessor.md), inactive branches
  are copied verbatim. Verible formats them where its grammar allows.
- Macro calls in unusual positions may be partitioned differently from
  Verible. slang places the call where its expansion begins, and the
  expansion can start in a different syntactic position from where the call
  appears in Verible's CST.

## Parsing

svfmt accepts everything slang parses (IEEE 1800-2023). Constructs Verible
cannot parse are formatted with the generic rules.

# Style options

svfmt uses `verible-verilog-format`'s flag names and defaults, so existing
scripts and flagfiles keep working. Defaults below were taken from
`verible-verilog-format --helpfull` (v0.0-4080).

## Basic layout (`BasicFormatStyle`)

| Flag | Default | Meaning |
| --- | --- | --- |
| `--column_limit` | 100 | target maximum line length |
| `--indentation_spaces` | 2 | spaces per indentation level |
| `--wrap_spaces` | 4 | extra indentation for wrapped continuation lines |
| `--line_break_penalty` | 2 | search cost per introduced line break |
| `--over_column_limit_penalty` | 100 | base cost for exceeding the column limit (+1 per extra column) |
| `--line_terminator` | auto | `auto`, `LF` or `CRLF` for formatted line breaks. Verbatim text keeps its original line endings. |

## Verilog style (`FormatStyle`)

Alignment flags take `{align, flush-left, preserve, infer}`; the default for
all of them is `infer`, which chooses `align` or `flush-left` based on how the
original code is spaced.

| Flag | Default |
| --- | --- |
| `--port_declarations_alignment` | infer |
| `--named_port_alignment` | infer |
| `--named_parameter_alignment` | infer |
| `--formal_parameters_alignment` | infer |
| `--module_net_variable_alignment` | infer |
| `--assignment_statement_alignment` | infer |
| `--enum_assignment_statement_alignment` | infer |
| `--case_items_alignment` | infer |
| `--class_member_variable_alignment` | infer |
| `--struct_union_members_alignment` | infer |
| `--distribution_items_alignment` | infer |

Indentation flags take `{indent, wrap}`:

| Flag | Default |
| --- | --- |
| `--port_declarations_indentation` | wrap |
| `--named_port_indentation` | wrap |
| `--named_parameter_indentation` | wrap |
| `--formal_parameters_indentation` | wrap |

Other options:

| Flag | Default | Meaning |
| --- | --- | --- |
| `--alignment_group_boundary` | none | what splits alignment groups: `none`, `blank-lines`, `separator-comments`, `blank-lines-and-separator-comments` |
| `--compact_indexing_and_selections` | true | `a[i+1]` instead of `a[i + 1]` |
| `--expand_coverpoints` | false | always put each coverpoint item on its own line |
| `--port_declarations_right_align_packed_dimensions` | false | right-align packed dimensions in aligned port lists |
| `--port_declarations_right_align_unpacked_dimensions` | false | same, for unpacked dimensions |
| `--try_wrap_long_lines` | false | run the line-wrap optimizer; otherwise long lines are left unformatted |
| `--wrap_end_else_clauses` | false | put `end` and `else` on separate lines |

## Tool flags

| Flag | Meaning |
| --- | --- |
| `--inplace` | replace each changed file atomically. Files with multiple hard links are rejected. |
| `--lines=N-M,...` | format only these line ranges |
| `--stdin_name` | file name to use in diagnostics when reading `-` |
| `--verify` | exit non-zero if the file would change (check mode) |
| `--verify_convergence` | also check that formatting is idempotent (default: true) |
| `--failsafe_success` | exit 0 even on errors; the original text is always echoed (default: true) |
| `--max_search_states` | cap on line-wrap search states (default 300000) |
| `--show_token_partition_tree`, `--show_largest_token_partitions`, `--show_inter_token_info`, `--show_equally_optimal_wrappings` | debug dumps, same format as Verible's where possible |

## svfmt-only flags

These are needed because slang preprocesses (see preprocessor.md):

| Flag | Meaning |
| --- | --- |
| `--incdir=a,b` | include search paths |
| `--define=N,M=1` | predefine macros |

## In-file controls

```verilog
// verilog_format: off
... left untouched ...
// verilog_format: on
```

`/* verilog_format: off */` also works, as in Verible.
| `--dump_cst` | print the slang-derived CST and exit |

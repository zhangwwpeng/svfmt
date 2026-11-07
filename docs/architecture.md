# Architecture

Verible's formatter needs three things from its front end, all in a
`verible::TextStructureView`:

1. the **full token stream** in source order, including whitespace and
   comments, with token text pointing into the original buffer;
2. a **CST** of `verible::SyntaxTreeNode`s whose leaves are those tokens;
3. the **original text**.

svfmt builds these from slang. Everything after that point is Verible's code.

```
file.sv ─► slang Preprocessor + Parser ─► slang SyntaxTree
                                              │
             src/svfmt/slang-analyzer.cc ◄────┘   (the bridge)
                │  token stream (verilog_tokentype enums, whitespace, comments)
                │  CST (tags = slang SyntaxKind | ListKind | GroupKind)
                ▼
 src/verible/verilog/formatting/formatter.cc        Formatter::Format()
   comment-controls   verilog_format: off/on, --lines
   token-annotator    spacing / break decisions per token pair
   tree-unwrapper     CST → TokenPartitionTree (indentation, policies)
   align              tabular alignment column scanners
 src/verible/common/formatting/*                    (unchanged Verible)
   line-wrap-searcher, layout-optimizer, align engine, partition tree ops
   ▼
 verification: re-parse output with slang, compare non-whitespace tokens,
 optional convergence check (format twice)
```

## Source layout

```
src/main.cpp                      CLI: Verible's verilog-format.cc + --incdir/--define/--dump_cst
src/svfmt/slang-analyzer.{h,cc}   the bridge (SlangAnalyzer)
src/svfmt/slang-cst-gen.inc       generated: typed member conversion per slang syntax class
src/svfmt/list-kinds.h            generated: ListKind tags (<Class>_<member>)
src/svfmt/group-kinds.h           GroupKind tags (synthetic Verible-like groups)
src/svfmt/syntax-context.h        context queries over the slang-tagged CST
src/verible/common/...            Verible generic code, copied as is
src/verible/verilog/formatting/   Verible Verilog formatter, node-type logic rewritten for slang
src/verible/verilog/parser/verilog-token-enum.h   generated from verilog.y
tools/gen_verilog_token_enum.py   regenerates verilog-token-enum.h
tools/gen_slang_cst.py            regenerates list-kinds.h and slang-cst-gen.inc
```

Ported code keeps the `verible` namespace and include paths. That keeps the
diff against upstream Verible small, so future upstream fixes can be merged.
Edits are marked `svfmt:`.

## The bridge (SlangAnalyzer)

### Consuming the source

The builder walks slang's tree in order and consumes the main buffer with a
byte cursor:

- A **token** is accepted only if its `SourceLocation` is in the main buffer
  at the cursor offset. Macro-expanded tokens and tokens from
  `` `include ``d files never are, so they are dropped automatically. slang
  returns static text for keywords and punctuation, so the token text is
  taken from the buffer, not from slang.
- **Trivia** has no location, so it is matched by content at the cursor.
  Whitespace is split into Verible's `TK_SPACE`/`TK_NEWLINE` tokens.
  Comments become `TK_EOL_COMMENT`/`TK_COMMENT_BLOCK`.
- **Directives** (slang `Directive` trivia) become CST nodes tagged with the
  directive's `SyntaxKind`. They are hoisted to the outermost node that starts
  with the same token, so an `` `ifdef `` lands between list items, as it
  does in Verible. A macro call becomes
  `MacroUsage[MacroCallId, kParenGroup['(', kMacroArgList[MacroArg, ',' ...], ')']]`,
  and each argument is a single `MacroArg` token, as in Verible.
- If the cursor does not reach the end of the buffer, analysis fails, and
  svfmt refuses to format and echoes the input. A successful analysis
  therefore always reproduces the input byte for byte.

Token enums are Verible's `verilog_tokentype`. The mapping is by token text,
using the aliases in `verilog.y`, plus special cases for identifiers, numbers
(base and digits are split, as in Verible) and directives. Because the token
enums match Verible's, `verilog-token.cc` (format token classes) and most of
`token-annotator.cc` run unchanged.

### CST shape

slang flattens list members into their parent node. Verible's formatter
depends on list nodes such as `kModuleItemList` or `kPortDeclarationList`. So
`tools/gen_slang_cst.py` generates typed conversion code from slang's
`syntax.txt`, which wraps every list member in a synthetic **list node**
tagged `ListKind::<Class>_<member>`. For example,
`ListKind::ModuleDeclaration_members` corresponds to Verible's
`kModuleItemList`.

Where slang's tree is flatter than Verible's, hand-written conversions in
`ConvertSpecial()` insert **group nodes** (`GroupKind`):

| Group | Inserted for | Verible counterpart |
| --- | --- | --- |
| `kIfClause`, `kIfHeader`, `kIfBody`, `kElseBody` | `ConditionalStatement`, `IfGenerate`, `ElseClause` | same names |
| `kBegin`, `kEnd` | begin/end (+ labels) of blocks and generate blocks | `kBegin`, `kEnd` |
| `kLoopHeader` | for / foreach / while / repeat / for-generate headers | `kLoopHeader` |
| `kSubroutineHeader` | function/task prototype + `;` (+ class method qualifiers) | `kFunctionHeader`, `kTaskHeader` |
| `kClassHeader` | class header up to `;` | `kClassHeader` |
| `kInstantiationType` | type (+ parameters) of data declarations and instances | `kInstantiationType` |
| `kParenGroup`, `kMacroArgList` | macro call arguments | same names |

Empty nodes are omitted, except list nodes, which are kept so that comments
in empty bodies get indented. `--dump_cst` prints the tree.

## The Verilog formatter layer

- **token-annotator.cc**: token rules are unchanged. Each of the roughly 45
  `NodeEnum` context checks was rewritten with the helpers in
  `svfmt/syntax-context.h`; `AncestorKind()` skips list nodes.
- **tree-unwrapper.cc**: the token scanning and comment attachment code is
  unchanged. `SetIndentationsAndCreatePartitions()` and
  `ReshapeTokenPartitions()` were rewritten case by case against slang kinds,
  following Verible's logic and comments.
- **align.cc**: grouping, ignore predicates and handler registry are
  unchanged. The column scanners were rewritten. They reserve columns with
  explicit semantic paths (e.g. ports: `{0}` direction, `{1}` type, `{2}`
  packed dimensions, `{3}` name, `{4}` unpacked dimensions) instead of
  positional tree paths, because our CST omits absent children.
- **formatter.cc**: parsing goes through `SlangAnalyzer`. Verification
  re-parses the output with slang and compares the non-whitespace token
  sequences.

## Dependencies

slang (submodule, built with `add_subdirectory`) and Abseil (used throughout
the Verible code: `absl::Status`, `StrCat`, flags, logging). The only other
copied file is `src/external_libs/editscript.h` (Apache-2.0).

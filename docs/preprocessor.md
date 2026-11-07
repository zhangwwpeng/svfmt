# Preprocessor handling

Verible never preprocesses. Its grammar accepts macro calls and conditional
directives in many positions. slang is a compiler front end and runs the
preprocessor first. This page describes how svfmt reconciles the two. The
code is in `src/svfmt/slang-analyzer.cc`.

## Macro calls

slang attaches a `MacroUsage` directive to the first token of the expansion.
The expansion tokens are in the tree but have macro locations. The bridge
drops expansion tokens, which are never at the cursor, and emits the call
itself as a `MacroUsage` node at the position where the expansion starts. The
annotator and unwrapper treat it like Verible's `kMacroCall`.

`` `define `` bodies are a single `PP_define_body` token, emitted verbatim, as
in Verible.

## Undefined macros

When a macro is used but its definition is not visible (it is typically in a
missing `` `include ``), slang reports `unknown macro`. It still skips the
argument list and expands the macro to nothing. svfmt handles this in two
steps:

1. Unknown-macro errors on their own are ignored. The empty expansion is
   correct for statement-level and item-level calls such as
   `` `uvm_info(...) `` or `` `uvm_object_utils(T) ``.
2. If the empty expansion breaks the parse (the macro is used inside an
   expression), svfmt parses again with **stub definitions**:
   - the arity comes from the call;
   - an object-like macro, or a call in expression position, expands to the
     placeholder `__svfmt_macro__`;
   - other calls expand to nothing.

   Expression position is decided by the previous character (`= ( , [ { ?`,
   operators, `return`).

`--incdir` and `--define` give slang the real definitions when available.

## `ifdef / `ifndef / `elsif / `else / `endif

Verible formats every branch. svfmt does the same with a two-step strategy:

1. **All-branches parse.** A lexical pre-scan finds the conditional
   directives, skipping comments, strings and `` `define `` bodies. In the
   copy given to slang, each directive is replaced by spaces of the same
   length. slang then parses all branches as one stream with offsets
   unchanged. The builder matches trivia against that copy and re-inserts
   each directive from the original text as a node (`IfDefDirective`, ...)
   containing `PP_ifdef` and `PP_Identifier` tokens. This works whenever the
   concatenated branches are syntactically valid, which is the case for
   conditionals between items, ports, statements or connections. Those are
   exactly the placements Verible's grammar supports.
2. **Fallback.** If the all-branches text does not parse (for example two
   alternative module headers, or an `` `ifdef `` inside an expression),
   svfmt uses normal preprocessing. Inactive branches then become one
   verbatim `TK_SVFMT_DISABLED_TEXT` token each.

## Excerpt parse modes

As in Verible, `// verilog_syntax: parse-as-module-body` (also `class-body`,
`package-body`, `statements`) in the leading comments makes svfmt parse the
file as that kind of excerpt. The wrapper is passed as separate slang source
buffers, so offsets are untouched. The CST is then re-rooted at the body
list. A plain parse that fails is retried as a module body, like Verible's
automatic mode.

// Checks that two SystemVerilog texts differ only in whitespace. Uses slang's
// raw lexer (no preprocessing, no parsing), so it shares no code path with the
// formatter's parse: inactive `ifdef branches are compared token by token too.
//
// Whitespace is significant in one place: `define and `pragma end at the end
// of their line, so the line end that terminates them and every "\" line
// continuation inside them are compared too. (Other directives take a fixed
// number of tokens; a line end after them means nothing.)
//
// Inside a UDP table (table ... endtable), symbols are single characters and
// whitespace between them means nothing, but the lexer reads "00" or "x0" as
// one token; table tokens are compared character by character.
//
// The indentation of a block comment's continuation lines is not compared:
// the formatter shifts them along with the comment.
//
// Used by svfmt (on its own output, against the original input) and by the
// svfmt-lexcmp tool of the corpus tests.

#ifndef SVFMT_LEXEQ_LEX_EQUIVALENCE_H_
#define SVFMT_LEXEQ_LEX_EQUIVALENCE_H_

#include <optional>
#include <string>
#include <string_view>

namespace svfmt {

// Returns nothing if `before` and `after` differ only in whitespace, else a
// description of the first difference.
std::optional<std::string> LexicalDifference(std::string_view before, std::string_view after);

}  // namespace svfmt

#endif  // SVFMT_LEXEQ_LEX_EQUIVALENCE_H_

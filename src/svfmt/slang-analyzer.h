// Bridge from slang's syntax tree to the data structures Verible's formatter
// consumes (verible::TextStructureView: contents, full token stream, CST).
//
// The produced CST is *not* shaped like Verible's yacc CST. Every slang
// SyntaxNode becomes a verible::SyntaxTreeNode tagged with its
// slang::syntax::SyntaxKind, and every token that physically appears in the
// main source buffer becomes a leaf whose token enum is a verilog_tokentype.
// Preprocessor directives and macro calls, which slang keeps as trivia, are
// lifted into nodes tagged with their directive SyntaxKind. Comments and
// whitespace are only in the token stream, as in Verible.
//
// Tokens produced by macro expansion and tokens of `include'd files are
// dropped: the bridge consumes the main buffer with a byte cursor and accepts
// a piece of text only if it starts exactly at the cursor. When the cursor
// fails to reach the end of the buffer, analysis fails, so a successful
// analysis always reproduces the input exactly.

#ifndef SVFMT_SLANG_ANALYZER_H_
#define SVFMT_SLANG_ANALYZER_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "slang/syntax/SyntaxKind.h"
#include "verible/common/text/text-structure.h"

namespace slang {
class BumpAllocator;
class SourceManager;
namespace syntax {
class SyntaxNode;
class SyntaxTree;
}  // namespace syntax
}  // namespace slang

namespace verible {
class Symbol;
}

namespace svfmt {

using slang::syntax::SyntaxKind;

// Token enums that have no verilog_tokentype counterpart.
enum SvfmtTokenEnum {
  // Raw text of an inactive `ifdef branch. Emitted verbatim.
  TK_SVFMT_DISABLED_TEXT = 20000,
  // `line continuation backslash outside a `define body.
  TK_SVFMT_LINE_CONTINUATION,
};

struct ParseMode;

struct AnalyzerOptions {
  std::vector<std::string> include_dirs;
  std::vector<std::string> defines;
  // Tabs (outside comments and strings) become spaces up to the next
  // multiple of this column; svfmt sets it to the indentation width.
  int tab_width = 1;
  // Set internally: stub `define lines generated for unknown macros, parsed
  // from a buffer ahead of the main one.
  std::vector<std::string> stubbed_macros;
  // Set internally: (absolute path, text) of `include files that are not on
  // disk, so they parse as a placeholder instead of failing.
  std::vector<std::pair<std::string, std::string>> placeholder_includes;
};

class SlangAnalyzer {
 public:
  ~SlangAnalyzer();

  // Parses `text` (named `filename` in diagnostics). Returns an error when
  // slang reports parse errors or the source cannot be represented
  // losslessly.
  static absl::StatusOr<std::unique_ptr<SlangAnalyzer>> Analyze(
      std::string_view text, std::string_view filename,
      const AnalyzerOptions &options = {});

  const verible::TextStructureView &Data() const { return *data_; }
  verible::TextStructureView &MutableData() { return *data_; }

  // The slang node a CST node was converted from (nullptr for nodes lifted
  // from trivia that have no single origin).
  const slang::syntax::SyntaxNode *Origin(const verible::Symbol *node) const;

  // Byte ranges [begin, end) of text that must be emitted verbatim
  // (inactive `ifdef branches, `define bodies).
  const std::vector<std::pair<int, int>> &VerbatimRanges() const {
    return verbatim_ranges_;
  }

  // Byte ranges [begin, end) of inactive `ifdef / `else branch text (a subset
  // of VerbatimRanges()): the formatter may format these on their own.
  const std::vector<std::pair<int, int>> &DisabledBranchRanges() const {
    return disabled_branch_ranges_;
  }

  // Formatted diagnostics from slang (errors and warnings).
  const std::string &Diagnostics() const { return diagnostics_; }

 private:
  class Builder;
  SlangAnalyzer() = default;
  static absl::StatusOr<std::unique_ptr<SlangAnalyzer>> AnalyzeWithMode(
      std::string_view text, std::string_view filename, const AnalyzerOptions &options,
      const ParseMode *mode, bool all_branches);

  std::string original_text_;
  // Keeps sub-parses of macro arguments alive (origins_ points into them).
  std::unique_ptr<slang::SourceManager> source_manager_;
  std::shared_ptr<slang::syntax::SyntaxTree> tree_;
  std::unique_ptr<verible::TextStructureView> data_;
  absl::flat_hash_map<const verible::Symbol *, const slang::syntax::SyntaxNode *>
      origins_;
  std::vector<std::pair<int, int>> verbatim_ranges_;
  std::vector<std::pair<int, int>> disabled_branch_ranges_;
  std::string diagnostics_;
};

// Preprocessor options used for every analysis in this process (set once by
// the command line driver; also used to re-parse formatted output).
AnalyzerOptions &GlobalAnalyzerOptions();

// Returns the verilog_tokentype / SvfmtTokenEnum name, for debugging.
std::string_view TokenEnumName(int token_enum);

// Returns a printable name for a CST node tag (a slang SyntaxKind).
std::string_view NodeTagName(int tag);

}  // namespace svfmt

#endif  // SVFMT_SLANG_ANALYZER_H_

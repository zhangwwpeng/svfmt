// Copyright 2017-2020 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "verible/verilog/formatting/formatter.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/die_if_null.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "verible/common/formatting/basic-format-style.h"
#include "verible/common/formatting/format-token.h"
#include "verible/common/formatting/layout-optimizer.h"
#include "verible/common/formatting/line-wrap-searcher.h"
#include "verible/common/formatting/token-partition-tree.h"
#include "verible/common/formatting/unwrapped-line.h"
#include "verible/common/formatting/verification.h"
#include "verible/common/strings/diff.h"
#include "verible/common/strings/line-column-map.h"
#include "verible/common/strings/position.h"
#include "verible/common/strings/range.h"
#include "verible/common/text/line-terminator.h"
#include "verible/common/text/symbol.h"
#include "verible/common/text/text-structure.h"
#include "verible/common/text/token-info.h"
#include "verible/common/text/tree-utils.h"
#include "verible/common/util/expandable-tree-view.h"
#include "verible/common/util/interval-set.h"
#include "verible/common/util/interval.h"
#include "verible/common/util/logging.h"
#include "verible/common/util/spacer.h"
#include "verible/common/util/tree-operations.h"
#include "verible/common/util/vector-tree-iterators.h"
#include "verible/common/util/vector-tree.h"
#include "svfmt/list-kinds.h"
#include "svfmt/slang-analyzer.h"
#include "verible/verilog/formatting/align.h"
#include "verible/verilog/formatting/comment-controls.h"
#include "verible/verilog/formatting/format-style.h"
#include "verible/verilog/parser/verilog-token-classifications.h"
#include "verible/verilog/formatting/token-annotator.h"
#include "verible/verilog/formatting/tree-unwrapper.h"
#include "verible/verilog/parser/verilog-token-enum.h"

namespace verilog {
namespace formatter {
using absl::Status;
using absl::StatusCode;

using verible::ByteOffsetSet;
using verible::ExpandableTreeView;
using verible::LineNumberSet;
using verible::PartitionPolicyEnum;
using verible::TokenPartitionTree;
using verible::TreeViewNodeInfo;
using verible::UnwrappedLine;
using verible::VectorTree;
using verible::VectorTreeLeavesIterator;

using partition_node_type = VectorTree<TreeViewNodeInfo<TokenPartitionTree>>;

// Takes a TextStructureView and FormatStyle, and formats UnwrappedLines.
class Formatter {
 public:
  Formatter(const verible::TextStructureView &text_structure,
            const FormatStyle &style)
      : text_structure_(text_structure), style_(style) {}

  // Formats the source code
  Status Format(const ExecutionControl &);

  Status Format() { return Format(ExecutionControl()); }

  void SelectLines(const LineNumberSet &lines);

  // Outputs all of the FormattedExcerpt lines to stream.
  // If "include_disabled" is false, does not contain the disabled ranges.
  void Emit(bool include_disabled, std::ostream &stream) const;

 private:
  // Contains structural information about the code to format, such as
  // TokenSequence from lexing, and ConcreteSyntaxTree from parsing
  const verible::TextStructureView &text_structure_;

  // The style configuration for the formatter
  FormatStyle style_;

  // Ranges of text where formatter is disabled (by comment directives).
  ByteOffsetSet disabled_ranges_;

  // Set of formatted lines, populated by calling Format().
  std::vector<verible::FormattedExcerpt> formatted_lines_;
};

// TODO(b/148482625): make this public/re-usable for general content comparison.
// Not declared in any header, but also used in formatter_test
extern Status VerifyFormatting(const verible::TextStructureView &text_structure,
                               std::string_view formatted_output,
                               std::string_view filename) {
  // Verify that the formatted output creates the same lexical
  // stream (filtered) as the original.  If any tokens were lost, fall back to
  // printing the original source unformatted.
  // Note: We cannot just Tokenize() and compare because Analyze()
  // performs additional transformations like expanding MacroArgs to
  // expression subtrees.
  // svfmt: re-parse with slang, then compare the non-whitespace token
  // sequences of input and output (Verible's FormatEquivalent).
  const auto reanalyzer =
      svfmt::SlangAnalyzer::Analyze(formatted_output, filename, svfmt::GlobalAnalyzerOptions());
  if (!reanalyzer.ok()) {
    return absl::DataLossError(
        absl::StrCat("Error lex/parsing-ing formatted output.  "
                     "Please file a bug.\nFirst error: ",
                     reanalyzer.status().message()));
  }
  {
    auto significant = [](const verible::TokenSequence &tokens) {
      std::vector<const verible::TokenInfo *> out;
      for (const auto &t : tokens) {
        const int e = t.token_enum();
        if (e == TK_SPACE || e == TK_NEWLINE || t.isEOF()) continue;
        out.push_back(&t);
      }
      return out;
    };
    const auto before = significant(text_structure.TokenStream());
    const auto after = significant((*reanalyzer)->Data().TokenStream());
    const size_t n = std::min(before.size(), after.size());
    for (size_t i = 0; i < n; ++i) {
      if (before[i]->token_enum() != after[i]->token_enum() ||
          before[i]->text() != after[i]->text()) {
        return absl::DataLossError(absl::StrCat(
            "Formatted output is lexically different from the input.    "
            "Please file a bug.  Details:\nFirst mismatched token [", i, "]: (",
            before[i]->text(), ") vs. (", after[i]->text(), ")"));
      }
    }
    if (before.size() != after.size()) {
      return absl::DataLossError(absl::StrCat(
          "Formatted output is lexically different from the input.    "
          "Please file a bug.  Details:\nToken count ",
          before.size(), " vs. ", after.size()));
    }
  }

  return absl::OkStatus();
}

static Status ReformatVerilogIncrementally(std::string_view original_text,
                                           std::string_view formatted_text,
                                           std::string_view filename,
                                           const FormatStyle &style,
                                           std::ostream &reformat_stream,
                                           const ExecutionControl &control) {
  // Differences from the first formatting.
  const verible::LineDiffs formatting_diffs(original_text, formatted_text);
  // Added lines will be re-applied to incremental re-formatting.
  LineNumberSet formatted_lines(
      verible::DiffEditsToAddedLineNumbers(formatting_diffs.edits));
  // Even if no line were changed by formatting, need to make sure that
  // reformatting does not accidentally reformat the whole file by
  // adding an out-of-range lines interval.  This effectively disables
  // re-formatting on the whole file unless line ranges are specified.
  formatted_lines.Add(formatting_diffs.after_lines.size() + 1);
  VLOG(1) << "formatted changed lines: " << formatted_lines;
  return FormatVerilog(formatted_text, filename, style, reformat_stream,
                       formatted_lines, control);
}

static Status ReformatVerilog(std::string_view original_text,
                              std::string_view formatted_text,
                              std::string_view filename,
                              const FormatStyle &style,
                              std::ostream &reformat_stream,
                              const LineNumberSet &lines,
                              const ExecutionControl &control) {
  // Disable reformat check to terminate recursion.
  ExecutionControl convergence_control(control);
  convergence_control.verify_convergence = false;

  if (lines.empty()) {
    // format whole file
    return FormatVerilog(formatted_text, filename, style, reformat_stream,
                         lines, convergence_control);
  }
  // reformat incrementally
  return ReformatVerilogIncrementally(original_text, formatted_text, filename,
                                      style, reformat_stream,
                                      convergence_control);
}

static absl::StatusOr<std::unique_ptr<svfmt::SlangAnalyzer>> ParseWithStatus(
    std::string_view text, std::string_view filename) {
  return svfmt::SlangAnalyzer::Analyze(text, filename, svfmt::GlobalAnalyzerOptions());
}

// svfmt: an inactive `ifdef / `else branch is kept verbatim when the branches
// cannot be parsed together (e.g. a missing ',' that is an error only once
// both branches are active). Such a branch is formatted on its own, inside a
// wrapper that parses it, and put back. Its lines get the indentation of the
// active code of the same conditional (the alternatives share it). Only
// whitespace changes; the wrapper is checked like any other input.
namespace {

std::string_view TrimSpaces(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

std::vector<std::string_view> SplitLines(std::string_view s) {
  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start <= s.size()) {
    size_t nl = s.find('\n', start);
    if (nl == std::string_view::npos) {
      lines.push_back(s.substr(start));
      break;
    }
    lines.push_back(s.substr(start, nl - start));
    start = nl + 1;
  }
  return lines;
}

size_t LeadingSpaces(std::string_view s) {
  size_t n = 0;
  while (n < s.size() && s[n] == ' ') ++n;
  return n;
}

// True if the trimmed line starts with the directive `word` (not a longer name).
bool StartsWithDirective(std::string_view trimmed, std::string_view word) {
  if (trimmed.size() < word.size() + 1 || trimmed[0] != '`') return false;
  if (trimmed.substr(1, word.size()) != word) return false;
  const char next = trimmed.size() > word.size() + 1 ? trimmed[word.size() + 1] : ' ';
  return !std::isalnum(static_cast<unsigned char>(next)) && next != '_';
}

bool IsConditionalOpen(std::string_view t) {
  return StartsWithDirective(t, "ifdef") || StartsWithDirective(t, "ifndef");
}

// Whether a line is a directive of any kind (a `define, `include, `else...).
bool IsAnyDirective(std::string_view t) {
  static constexpr std::string_view kWords[] = {
      "ifdef", "ifndef", "elsif", "else", "endif", "define", "undef", "include", "pragma"};
  for (std::string_view w : kWords) {
    if (StartsWithDirective(t, w)) return true;
  }
  return false;
}

struct Line {
  size_t begin;  // byte offset of the line in the text
  std::string_view text;
};

std::vector<Line> LinesWithOffsets(std::string_view text) {
  std::vector<Line> out;
  size_t offset = 0;
  for (std::string_view line : SplitLines(text)) {
    out.push_back({offset, line});
    offset += line.size() + 1;
  }
  return out;
}

// Indentation of the active code that shares the conditional group of
// `range`: the first code line of the group outside every disabled branch.
std::optional<size_t> GroupIndent(const std::vector<Line> &lines, std::pair<size_t, size_t> range,
                                  const std::vector<std::pair<size_t, size_t>> &disabled) {
  size_t at = 0;
  while (at + 1 < lines.size() && lines[at + 1].begin <= range.first) ++at;
  // The opening directive: walk back over nested groups.
  size_t opener = lines.size();
  int depth = 0;
  for (size_t j = at; j-- > 0;) {
    const std::string_view t = TrimSpaces(lines[j].text);
    if (StartsWithDirective(t, "endif")) {
      ++depth;
    } else if (IsConditionalOpen(t)) {
      if (depth == 0) {
        opener = j;
        break;
      }
      --depth;
    }
  }
  if (opener == lines.size()) return std::nullopt;
  // The matching `endif.
  size_t closer = lines.size();
  depth = 0;
  for (size_t j = opener + 1; j < lines.size(); ++j) {
    const std::string_view t = TrimSpaces(lines[j].text);
    if (IsConditionalOpen(t)) {
      ++depth;
    } else if (StartsWithDirective(t, "endif")) {
      if (depth == 0) {
        closer = j;
        break;
      }
      --depth;
    }
  }
  if (closer == lines.size()) return std::nullopt;
  for (size_t j = opener + 1; j < closer; ++j) {
    const size_t line_end = lines[j].begin + lines[j].text.size();
    bool disabled_line = false;
    for (const auto &d : disabled) {
      if (lines[j].begin < d.second && d.first < line_end) disabled_line = true;
    }
    const std::string_view t = TrimSpaces(lines[j].text);
    if (disabled_line || t.empty() || IsAnyDirective(t) || t.starts_with("//")) continue;
    return LeadingSpaces(lines[j].text);
  }
  return std::nullopt;
}

struct BranchWrapper {
  std::string_view prolog;      // ends with '\n'
  std::string_view second_line;  // expected start of the prolog's last line, if any
  std::string_view closing;     // one line before "endmodule", if any
};

// Module items; port connections of an instance; statements in a block.
constexpr BranchWrapper kBranchWrappers[] = {
    {"module __svfmt_branch__;\n", "", ""},
    // The trailing comment keeps the port list on several lines.
    {"module __svfmt_branch__;\nfoo __svfmt_branch_inst__ ( // svfmt\n", "foo __svfmt_branch_inst__ (", ");"},
    {"module __svfmt_branch__;\ninitial begin\n", "initial begin", "end"},
};

// Formats `branch` (the text of one inactive branch) and returns it with its
// lines at `indent`, or nothing if no wrapper parses it.
std::optional<std::string> FormatBranch(std::string_view branch, std::optional<size_t> indent,
                                        const FormatStyle &style,
                                        std::string_view filename) {
  // A multi-line block comment would have its inner lines shifted: keep it.
  if (branch.find("/*") != std::string_view::npos && branch.find('\n') != std::string_view::npos) {
    return std::nullopt;
  }
  for (const BranchWrapper &w : kBranchWrappers) {
    std::string wrapped = absl::StrCat(w.prolog, branch, "\n", w.closing,
                                       w.closing.empty() ? "" : "\n", "endmodule\n");
    const auto analyzer = ParseWithStatus(wrapped, filename);
    if (!analyzer.ok()) continue;
    std::ostringstream sink;
    ExecutionControl control;
    control.stream = &sink;
    control.verify_convergence = false;
    std::string formatted;
    if (!FormatVerilog(analyzer->get()->Data(), filename, style, &formatted, {}, control).ok()) {
      continue;
    }

    std::vector<std::string_view> lines = SplitLines(formatted);
    while (!lines.empty() && TrimSpaces(lines.back()).empty()) lines.pop_back();
    const size_t prolog_lines = static_cast<size_t>(std::count(w.prolog.begin(), w.prolog.end(), '\n'));
    const size_t closing_lines = w.closing.empty() ? 0 : 1;
    if (lines.size() < prolog_lines + closing_lines + 2) continue;
    if (TrimSpaces(lines.front()) != "module __svfmt_branch__;") continue;
    if (TrimSpaces(lines.back()) != "endmodule") continue;
    if (!w.second_line.empty() &&
        TrimSpaces(lines[1]).substr(0, w.second_line.size()) != w.second_line) {
      continue;
    }
    if (closing_lines && TrimSpaces(lines[lines.size() - 2]) != w.closing) continue;

    const size_t body_begin = prolog_lines;
    const size_t body_end = lines.size() - 1 - closing_lines;
    if (body_end <= body_begin) continue;
    // Shift the lines after the first from the wrapper's indentation to
    // `indent`. The first line is indented by the formatter itself.
    const size_t base = LeadingSpaces(lines[body_begin]);
    const size_t target = indent.value_or(base);
    const bool shift_right = target >= base;
    const size_t shift = shift_right ? target - base : base - target;

    std::string result;
    for (size_t i = body_begin; i < body_end; ++i) {
      if (i > body_begin) result += '\n';
      if (i == body_begin) {
        result += std::string(TrimSpaces(lines[i]));
      } else if (TrimSpaces(lines[i]).empty()) {
        // Blank line: nothing to shift.
      } else if (shift_right) {
        result.append(shift, ' ');
        result += std::string(lines[i]);
      } else {
        result += std::string(lines[i].substr(std::min(shift, LeadingSpaces(lines[i]))));
      }
    }
    return result;
  }
  return std::nullopt;
}

// Formats `text` without the branch pre-pass.
std::optional<std::string> FormatPlain(std::string_view text, std::string_view filename,
                                       const FormatStyle &style) {
  const auto analyzer = ParseWithStatus(text, filename);
  if (!analyzer.ok()) return std::nullopt;
  std::ostringstream sink;
  ExecutionControl control;
  control.stream = &sink;
  control.verify_convergence = false;
  std::string formatted;
  if (!FormatVerilog(analyzer->get()->Data(), filename, style, &formatted, {}, control).ok()) {
    return std::nullopt;
  }
  return formatted;
}

// Returns `text` with each inactive branch formatted. The file is formatted
// once first: the indentation of each branch comes from the formatted active
// code of its conditional, so the result does not depend on the input's
// indentation of the branches.
std::string PreformatDisabledBranchesOnce(std::string_view text, std::string_view filename,
                                          const FormatStyle &style) {
  const auto analyzer = ParseWithStatus(text, filename);
  if (!analyzer.ok()) return std::string(text);
  const auto &disabled_ranges = analyzer->get()->DisabledBranchRanges();
  if (disabled_ranges.empty()) return std::string(text);
  const auto formatted = FormatPlain(text, filename, style);
  if (!formatted) return std::string(text);

  const auto formatted_analyzer = ParseWithStatus(*formatted, filename);
  if (!formatted_analyzer.ok()) return std::string(text);
  std::vector<std::pair<size_t, size_t>> ranges;
  for (const auto &[begin, end] : formatted_analyzer->get()->DisabledBranchRanges()) {
    ranges.emplace_back(static_cast<size_t>(begin), static_cast<size_t>(end));
  }
  const auto lines = LinesWithOffsets(*formatted);
  std::vector<std::optional<size_t>> indents;
  for (const auto &range : ranges) indents.push_back(GroupIndent(lines, range, ranges));

  std::string out = *formatted;
  // Last first, so that earlier offsets stay valid.
  for (size_t i = ranges.size(); i-- > 0;) {
    const auto [begin, end] = ranges[i];
    if (begin >= end || end > out.size()) continue;
    const std::string branch = out.substr(begin, end - begin);
    if (auto replacement = FormatBranch(branch, indents[i], style, filename)) {
      out.replace(begin, end - begin, *replacement);
    }
  }
  return out;
}

// The branches' indentation depends on the formatted active code, which in
// turn depends on the branches' text (alignment), so repeat until stable.
std::string PreformatDisabledBranches(std::string_view text, std::string_view filename,
                                      const FormatStyle &style) {
  std::string current(text);
  for (int round = 0; round < 3; ++round) {
    std::string next = PreformatDisabledBranchesOnce(current, filename, style);
    if (next == current) break;
    current = std::move(next);
  }
  // Keep the change only if formatting the result is stable; otherwise the
  // branches stay as they were.
  const auto once = FormatPlain(current, filename, style);
  const auto twice = once ? FormatPlain(*once, filename, style) : std::nullopt;
  if (!once || !twice || *once != *twice) return std::string(text);
  // Formatting the result runs this pre-pass again: it must change nothing.
  if (PreformatDisabledBranchesOnce(current, filename, style) != current) {
    return std::string(text);
  }
  return current;
}

}  // namespace

absl::Status FormatVerilog(const verible::TextStructureView &text_structure,
                           std::string_view filename, const FormatStyle &style,
                           std::string *formatted_text,
                           const verible::LineNumberSet &lines,
                           const ExecutionControl &control) {
  Formatter fmt(text_structure, style);
  fmt.SelectLines(lines);

  // Format code.
  Status format_status = fmt.Format(control);
  if (!format_status.ok()) {
    if (format_status.code() != StatusCode::kResourceExhausted) {
      // Some more fatal error, halt immediately.
      return format_status;
    }
    // Else allow remainder of this function to execute, and print partially
    // formatted code, but force a non-zero exit status in the end.
  }

  // In any diagnostic mode, proceed no further.
  if (control.AnyStop()) {
    return absl::CancelledError("Halting for diagnostic operation.");
  }

  // Render formatted text to the output buffer.
  std::ostringstream output_buffer;
  fmt.Emit(true, output_buffer);
  *formatted_text = output_buffer.str();

  // For now, unconditionally verify.
  if (Status verify_status =
          VerifyFormatting(text_structure, *formatted_text, filename);
      !verify_status.ok()) {
    return verify_status;
  }

  return format_status;
}

Status FormatVerilog(std::string_view text, std::string_view filename,
                     const FormatStyle &style, std::ostream &formatted_stream,
                     const LineNumberSet &lines,
                     const ExecutionControl &control) {
  // Whole file only: line-restricted formatting keeps the text as it is.
  std::string prepared;
  if (lines.empty()) {
    prepared = PreformatDisabledBranches(text, filename, style);
    text = prepared;
  }
  const auto analyzer = ParseWithStatus(text, filename);
  if (!analyzer.ok()) return analyzer.status();

  const verible::TextStructureView &text_structure = analyzer->get()->Data();
  std::string formatted_text;
  Status format_status = FormatVerilog(text_structure, filename, style,
                                       &formatted_text, lines, control);
  // Commit formatted text to the output stream independent of status.
  formatted_stream << formatted_text;
  if (!format_status.ok()) return format_status;

  // When formatting whole-file (no --lines are specified), ensure that
  // the formatting transformation is convergent after one iteration.
  //   format(format(text)) == format(text)
  if (control.verify_convergence) {
    std::ostringstream reformat_stream;
    if (auto reformat_status =
            ReformatVerilog(text, formatted_text, filename, style,
                            reformat_stream, lines, control);
        !reformat_status.ok()) {
      return reformat_status;
    }
    const std::string &reformatted_text(reformat_stream.str());
    return verible::ReformatMustMatch(text, lines, formatted_text,
                                      reformatted_text);
  }
  return format_status;
}

absl::Status FormatVerilogRange(const verible::TextStructureView &structure,
                                const FormatStyle &style,
                                std::string *formatted_text,
                                const verible::Interval<int> &line_range,
                                const ExecutionControl &control) {
  if (line_range.empty()) {
    return absl::OkStatus();
  }

  Formatter fmt(structure, style);
  fmt.SelectLines({line_range});

  // Format code.
  Status format_status = fmt.Format(control);
  if (!format_status.ok()) return format_status;

  // In any diagnostic mode, proceed no further.
  if (control.AnyStop()) {
    return absl::CancelledError("Halting for diagnostic operation.");
  }

  std::ostringstream output_buffer;
  fmt.Emit(false, output_buffer);
  *formatted_text = output_buffer.str();

  // The range-format can output a spurious newline in the beginning (#1150).
  // Whitespace handling needs some rework in the formatter, and it is not
  // trivial in the current state to fix at the source.
  // However, we can easily see the effects and mitigate it here: if we see a
  // newline at the beginning of the formatted range, but no newline at the
  // beginning of the original range, erase it in the formatted output.
  // TODO(hzeller): This can go when whitespace handling is revisited.
  //                (Emit(), FormatWhitespaceWithDisabledByteRanges())
  const auto &text_lines = structure.Lines();
  const char unformatted_begin = *text_lines[line_range.min - 1].begin();
  if (!formatted_text->empty() && (*formatted_text)[0] == '\n' &&
      unformatted_begin != '\n') {
    formatted_text->erase(0, 1);  // possibly expensive.
  }

  // We don't have verification tests here as we only have a subset of code
  // so it would be more tricky. Since this output is used in interactive
  // settings such as editors, this is less of an issue.

  return absl::OkStatus();
}

absl::Status FormatVerilogRange(std::string_view full_content,
                                std::string_view filename,
                                const FormatStyle &style,
                                std::string *formatted_text,
                                const verible::Interval<int> &line_range,
                                const ExecutionControl &control) {
  const auto analyzer = ParseWithStatus(full_content, filename);
  if (!analyzer.ok()) return analyzer.status();
  return FormatVerilogRange(analyzer->get()->Data(), style, formatted_text,
                            line_range, control);
}

static verible::Interval<int> DisableByteOffsetRange(
    std::string_view substring, std::string_view superstring) {
  CHECK(!substring.empty());
  auto range = verible::SubstringOffsets(substring, superstring);
  // +1 so that formatting can still occur on the space before the start
  // of the disabled range, for example allowing for indentation adjustments.
  return {range.first + 1, range.second};
}

// svfmt: Verible's kNetVariableAssignment / kBlockingAssignmentStatement is
// an ExpressionStatement holding an AssignmentExpression ("x = y;"), but not a
// nonblocking assignment.
static bool IsBlockingAssignmentStatement(const verible::Symbol &origin) {
  if (origin.Kind() != verible::SymbolKind::kNode ||
      origin.Tag().tag != static_cast<int>(slang::syntax::SyntaxKind::ExpressionStatement)) {
    return false;
  }
  const auto &node = verible::SymbolCastToNode(origin);
  return !node.empty() && node.front() &&
         node.front()->Tag().tag ==
             static_cast<int>(slang::syntax::SyntaxKind::AssignmentExpression);
}

// Decided at each node in UnwrappedLine partition tree whether or not
// it should be expanded or unexpanded.
static void DeterminePartitionExpansion(
    partition_node_type *node,
    std::vector<verible::PreFormatToken> *preformatted_tokens,
    std::string_view full_text, const ByteOffsetSet &disabled_ranges,
    const FormatStyle &style) {
  auto &node_view = node->Value();
  const UnwrappedLine &uwline = node_view.Value();
  VLOG(3) << "unwrapped line: " << uwline;
  const verible::FormatTokenRange ftoken_range(uwline.TokensRange());
  const auto partition_policy = uwline.PartitionPolicy();

  const auto PreserveSpaces = [&ftoken_range, &full_text,
                               preformatted_tokens]() {
    const ByteOffsetSet new_disable_range{{DisableByteOffsetRange(
        verible::make_string_view_range(ftoken_range.front().Text().begin(),
                                        ftoken_range.back().Text().end()),
        full_text)}};
    verible::PreserveSpacesOnDisabledTokenRanges(preformatted_tokens,
                                                 new_disable_range, full_text);
  };

  // Expand or not, depending on partition policy and other conditions.

  // If this is a leaf partition, there is nothing to expand.
  if (is_leaf(*node)) {
    VLOG(3) << "No children to expand.";
    node_view.Unexpand();
    if (partition_policy == PartitionPolicyEnum::kFitOnLineElseExpand &&
        !style.try_wrap_long_lines &&
        !verible::FitsOnLine(uwline, style).fits) {
      // give-up early and preserve original spacing
      VLOG(3) << "Does not fit (leaf), preserving.";
      PreserveSpaces();
    }
    return;
  }

  // If any children are expanded, then this node must be expanded,
  // regardless of the UnwrappedLine's chosen policy.
  // Thus, this function must be executed with a post-order traversal.
  const auto &children = node->Children();
  if (std::any_of(children.begin(), children.end(),
                  [](const partition_node_type &child) {
                    return child.Value().IsExpanded();
                  })) {
    VLOG(3) << "Child forces parent to expand.";
    node_view.Expand();
    return;
  }

  {
    // If any part of the range is formatting-disabled, expand this partition so
    // that whitespace between subpartitions can be handled accordingly in
    // Formatter::Emit().
    const verible::FormatTokenRange range(uwline.TokensRange());
    const verible::IntervalSet<int> partition_byte_range{
        {range.front().token->left(full_text),
         range.back().token->right(full_text)}};
    // (Same as IntervalSet::Complement without temporary copy.)
    verible::IntervalSet<int> diff(partition_byte_range);
    diff.Difference(disabled_ranges);
    if (partition_byte_range != diff) {
      // Then some sub-interval was removed.
      VLOG(3) << "Partition @bytes " << partition_byte_range
              << " is partially format-disabled, so expand.";
      node_view.Expand();
      return;
    }
  }

  VLOG(3) << "partition policy: " << partition_policy;
  switch (partition_policy) {
    case PartitionPolicyEnum::kUninitialized: {
      LOG(FATAL) << "Got an uninitialized partition policy at: " << uwline;
      break;
    }
    case PartitionPolicyEnum::kAlwaysExpand: {
      if (children.size() > 1) {
        node_view.Expand();
      }
      break;
    }
    case PartitionPolicyEnum::kTabularAlignment: {
      if (uwline.Origin() && uwline.Origin()->Tag().tag ==
          static_cast<int>(svfmt::ListKind::ArgumentList_parameters)) {
        // Check whether the whole function call fits on one line. If possible,
        // unexpand and fit into one line. Otherwise expand argument list with
        // tabular alignment.
        auto &node_view_parent = node->Parent()->Value();
        const UnwrappedLine &uwline_parent = node_view_parent.Value();
        if (verible::FitsOnLine(uwline_parent, style).fits) {
          node_view.Unexpand();
          break;
        }
      }

      if (children.size() > 1) {
        node_view.Expand();
      }
      break;
    }

    case PartitionPolicyEnum::kAlreadyFormatted: {
      // All partitions with this policy should be leafs at this point, which
      // are handled above. Just in case - unexpand.
      node_view.Unexpand();
      break;
    }

    case PartitionPolicyEnum::kInline: {
      // All partitions with this policy should be already applied and removed
      // by calls to ApplyAlreadyFormattedPartitionPropertiesToTokens on their
      // parent partitions.
      LOG(FATAL) << "Unreachable. " << partition_policy;
      break;
    }

    // Try to fit kAppendFittingSubPartitions partition into single line.
    // If it doesn't fit expand to grouped nodes.
    case PartitionPolicyEnum::kAppendFittingSubPartitions: {
      // !style.try_wrap_long_lines was already handled above
      if (verible::FitsOnLine(uwline, style).fits) {
        VLOG(3) << "Fits, un-expanding.";
        node_view.Unexpand();
      } else {
        VLOG(3) << "Does not fit, expanding.";
        node_view.Expand();
      }
      break;
    }

    case PartitionPolicyEnum::kFitOnLineElseExpand: {
      if (uwline.Origin() &&
          (IsBlockingAssignmentStatement(*uwline.Origin()) ||
           uwline.Origin()->Tag().tag ==
               static_cast<int>(svfmt::ListKind::BlockStatement_items))) {
        // Align unnamed parameters in function call. Example:
        // always_comb begin
        //   value = function_name(8'hA, signal,
        //                         signal_1234);
        // end
        const auto &children_tmp = node->Children();
        auto look_for_arglist = [](const partition_node_type &child) {
          const auto &node_view_child = child.Value();
          const UnwrappedLine &uwline_child = node_view_child.Value();
          return (uwline_child.Origin() &&
                  uwline_child.Origin()->Kind() == verible::SymbolKind::kNode &&
                  verible::SymbolCastToNode(*uwline_child.Origin())
                      .MatchesTag(svfmt::ListKind::ArgumentList_parameters));
        };

        // Check if kNetVariableAssignment or kBlockItemStatementList contains
        // kArgumentList node
        if (std::any_of(children_tmp.begin(), children_tmp.end(),
                        look_for_arglist)) {
          node_view.Unexpand();
          break;
        }
      }

      if (verible::FitsOnLine(uwline, style).fits) {
        VLOG(3) << "Fits, un-expanding.";
        node_view.Unexpand();
      } else {
        VLOG(3) << "Does not fit, expanding.";
        node_view.Expand();
      }
      break;
    }

    case PartitionPolicyEnum::kJuxtapositionOrIndentedStack:
    case PartitionPolicyEnum::kJuxtaposition:
    case PartitionPolicyEnum::kStack:
    case PartitionPolicyEnum::kWrap: {
      // The policies are handled (and replaced) in Layout Optimizer.
      LOG(FATAL) << "Unreachable. " << partition_policy;
      break;
    }
  }
}

// Produce a worklist of independently formattable UnwrappedLines.
static std::vector<UnwrappedLine> MakeUnwrappedLinesWorklist(
    const FormatStyle &style, std::string_view full_text,
    const ByteOffsetSet &disabled_ranges,
    const TokenPartitionTree &format_tokens_partitions,
    std::vector<verible::PreFormatToken> *preformatted_tokens) {
  // Initialize a tree view that treats partitions as fully-expanded.
  ExpandableTreeView<TokenPartitionTree> format_tokens_partition_view(
      format_tokens_partitions);

  // For unwrapped lines that fit, don't bother expanding their partitions.
  // Post-order traversal: if a child doesn't 'fit' and needs to be expanded,
  // so must all of its parents (and transitively, ancestors).
  format_tokens_partition_view.ApplyPostOrder(
      [&full_text, &disabled_ranges, &style,
       preformatted_tokens](partition_node_type &node) {
        DeterminePartitionExpansion(&node, preformatted_tokens, full_text,
                                    disabled_ranges, style);
      });

  // Remove trailing blank lines.
  std::vector<UnwrappedLine> unwrapped_lines(
      format_tokens_partition_view.begin(), format_tokens_partition_view.end());
  while (!unwrapped_lines.empty() && unwrapped_lines.back().IsEmpty()) {
    unwrapped_lines.pop_back();
  }
  return unwrapped_lines;
}

static void PrintLargestPartitions(
    std::ostream &stream, const TokenPartitionTree &token_partitions,
    size_t max_partitions, const verible::LineColumnMap &line_column_map,
    std::string_view base_text) {
  stream << "Showing the " << max_partitions
         << " largest (leaf) token partitions:" << std::endl;
  const auto ranked_partitions =
      FindLargestPartitions(token_partitions, max_partitions);
  const verible::Spacer hline(80, '=');
  for (const auto &partition : ranked_partitions) {
    stream << hline << "\n[" << partition->Size() << " tokens";
    if (!partition->IsEmpty()) {
      stream << ", starting at line:col "
             << line_column_map.GetLineColAtOffset(
                    base_text,
                    partition->TokensRange().front().token->left(base_text));
    }
    stream << "]: " << *partition << std::endl;
  }
  stream << hline << std::endl;
}

std::ostream &ExecutionControl::Stream() const {
  return (stream != nullptr) ? *stream : std::cout;
}

void Formatter::SelectLines(const LineNumberSet &lines) {
  disabled_ranges_ = EnabledLinesToDisabledByteRanges(
      lines, text_structure_.GetLineColumnMap());
}

// Given control flags and syntax tree, selectively disable some ranges
// of text from formatting.  This provides an easy way to preserve spacing on
// selected syntax subtrees to reduce formatter harm while allowing
// development to progress.
static void DisableSyntaxBasedRanges(ByteOffsetSet *disabled_ranges,
                                     const verible::Symbol &root,
                                     const FormatStyle &style,
                                     std::string_view full_text) {
  /**
  // Basic template:
  if (!style.controlling_flag) {
    for (const auto& match : FindAllSyntaxTreeNodeTypes(root)) {
      // Refine search into specific subtrees, if applicable.
      // Convert the spanning string_views into byte offset ranges to disable.
      const auto inst_text = verible::StringSpanOfSymbol(*match.match);
      VLOG(4) << "disabled: " << inst_text;
      disabled_ranges->Add(DisableByteOffsetRange(inst_text, full_text));
    }
  }
  **/
}

// Keeps multi-line EOL comments aligned to the same column.
//
// When a line containing nothing else than a single EOL comment follows a line
// containing any tokens and an EOL comment, starting columns (from original,
// unformatted source code) of both comments are compared. If the columns differ
// no more than kMaxColumnDifference, the comment in the comment-only line is
// considered to be a continuation comment. The same check is performed on all
// following comment-only lines. The process of continuation detection ends when
// currently handled line has any tokens other than EOL comment, or when the
// comment's starting column differs too much from the first comment's column.
// All continuation comments are placed in the same column as their starting
// comment's column in formatted output.
class ContinuationCommentAligner {
  // Maximum accepted difference between continuation and starting comments'
  // starting columns
  static constexpr int kMaxColumnDifference = 1;

 public:
  ContinuationCommentAligner(const verible::LineColumnMap &line_column_map,
                             const std::string_view base_text)
      : line_column_map_(line_column_map), base_text_(base_text) {}

  // Takes the next line that has to be formatted and a vector of already
  // formatted lines.
  //
  // Continuation comment lines are formatted and appended to
  // already_formatted_lines. In case of all other lines neither the line nor
  // already_formatted_lines are modified.
  // Return value informs whether the line has been formatted and added
  // to already_formatted_lines.
  bool HandleLine(
      const UnwrappedLine &uwline,
      std::vector<verible::FormattedExcerpt> *already_formatted_lines) {
    VLOG(4) << __FUNCTION__ << ": " << uwline;

    if (already_formatted_lines->empty()) {
      VLOG(4) << "Not a continuation comment line: first line";
      return false;
    }

    if (uwline.Size() != 1 || uwline.TokensRange().back().TokenEnum() !=
                                  verilog_tokentype::TK_EOL_COMMENT) {
      VLOG(4) << "Not a continuation comment line: "
              << "does not consist of a single EOL comment.";
      formatted_column_ = kInvalidColumn;
      original_column_ = kInvalidColumn;
      return false;
    }

    const auto &previous_line = already_formatted_lines->back();
    VLOG(4) << __FUNCTION__ << ": previous line: " << previous_line;
    if (original_column_ == kInvalidColumn) {
      if (previous_line.Tokens().size() <= 1) {
        VLOG(4) << "Not a continuation comment line: "
                << "too few tokens in previous line.";
        return false;
      }
      // svfmt: a comment after a directive line ("`else  // c") does not
      // continue it; otherwise a comment that lands within a column of it
      // becomes a continuation on the next pass and moves (no convergence).
      if (IsPreprocessorKeyword(verilog_tokentype(
              previous_line.Tokens().front().token->token_enum()))) {
        VLOG(4) << "Not a continuation comment line: previous line is a directive.";
        return false;
      }
      const auto *previous_comment = previous_line.Tokens().back().token;
      if (previous_comment->token_enum() != verilog_tokentype::TK_EOL_COMMENT) {
        VLOG(4) << "Not a continuation comment line: "
                << "no EOL comment in previous line.";
        return false;
      }
      original_column_ = GetTokenColumn(previous_comment);
    }

    const auto *comment = uwline.TokensRange().back().token;
    const int comment_column = GetTokenColumn(comment);

    VLOG(4) << "Original column: " << original_column_ << " vs. "
            << comment_column;

    if (std::abs(original_column_ - comment_column) > kMaxColumnDifference) {
      // svfmt: if the comment would land within a column of the previous
      // EOL comment, the next pass sees a continuation and moves it; align
      // it now so that formatting converges.
      const int previous_column = CalculateEolCommentColumn(previous_line);
      if (formatted_column_ != kInvalidColumn ||
          std::abs(uwline.IndentationSpaces() - previous_column) > kMaxColumnDifference) {
        VLOG(4) << "Not a continuation comment line: "
                << "starting column difference is too big";
        original_column_ = kInvalidColumn;
        return false;
      }
      original_column_ = comment_column;
    }

    VLOG(4) << "Continuation comment line - finalizing formatting";
    if (formatted_column_ == kInvalidColumn) {
      formatted_column_ = CalculateEolCommentColumn(previous_line);
    }
    UnwrappedLine aligned_uwline(uwline);
    aligned_uwline.SetIndentationSpaces(formatted_column_);
    already_formatted_lines->emplace_back(aligned_uwline);

    return true;
  }

 private:
  int GetTokenColumn(const verible::TokenInfo *token) {
    CHECK_NOTNULL(token);
    const int column =
        line_column_map_.GetLineColAtOffset(base_text_, token->left(base_text_))
            .column;
    CHECK_GE(column, 0);
    return column;
  }

  static void AdjustColumnUsingTokenSpacing(
      const verible::FormattedToken &token, int *column) {
    switch (token.before.action) {
      case verible::SpacingDecision::kPreserve: {
        if (token.before.preserved_space_start !=
            verible::string_view_null_iterator()) {
          const std::string_view leading = token.OriginalLeadingSpaces();
          const auto last_nl = leading.find_last_of('\n');
          if (last_nl == std::string_view::npos) {
            *column += leading.length();
          } else {
            // Reset column after the last newline, same as FormattedToken
            // emit (GitHub issue 2542).
            *column = static_cast<int>(leading.length() - last_nl - 1);
          }
        } else {
          *column += token.before.spaces;
        }
        break;
      }
      case verible::SpacingDecision::kWrap:
        // Newline then only the wrap indent (same as FormattedToken emit).
        *column = token.before.spaces;
        break;
      case verible::SpacingDecision::kAlign:
      case verible::SpacingDecision::kAppend:
        *column += token.before.spaces;
        break;
    }
  }

  static int CalculateEolCommentColumn(const verible::FormattedExcerpt &line) {
    // Compute the starting column of the trailing EOL comment the same way
    // FormattedExcerpt::FormattedText emits spaces, including:
    //   * wrap indents (SpacingDecision::kWrap), and
    //   * preserved leading whitespace that may contain newlines (common when
    //     an original line break is kept). Counting those newlines as width
    //     made continuation comments land on the wrong column and fail to
    //     converge on re-format (GitHub issue 2542).
    if (line.Tokens().empty()) return 0;

    int column = 0;
    const auto &front = line.Tokens().front();

    if (front.before.action != verible::SpacingDecision::kPreserve) {
      column += line.IndentationSpaces();
    }
    if (front.before.action == verible::SpacingDecision::kAlign) {
      column += front.before.spaces;
    }
    column += front.token->text().length();

    const auto &tokens = line.Tokens();
    for (size_t i = 1; i < tokens.size(); ++i) {
      const auto &ftoken = tokens[i];
      AdjustColumnUsingTokenSpacing(ftoken, &column);
      // Do not add the last token's length: that is the EOL comment whose
      // starting column we want.
      if (i + 1 < tokens.size()) {
        column += ftoken.token->text().length();
      }
    }

    CHECK_GE(column, 0);
    return column;
  }

  const verible::LineColumnMap &line_column_map_;
  const std::string_view base_text_;

  // Used when the most recenly handled line can't have a continuation comment.
  static constexpr int kInvalidColumn = -1;

  // Starting column of current comment group in original source code.
  int original_column_ = kInvalidColumn;
  // Starting column of current comment group in formatted source code.
  int formatted_column_ = kInvalidColumn;
};

Status Formatter::Format(const ExecutionControl &control) {
  const std::string_view full_text(text_structure_.Contents());
  const auto &token_stream(text_structure_.TokenStream());

  // Initialize auxiliary data needed for TreeUnwrapper.
  UnwrapperData unwrapper_data(token_stream);

  // Partition input token stream into hierarchical set of UnwrappedLines.
  TreeUnwrapper tree_unwrapper(text_structure_, style_,
                               unwrapper_data.preformatted_tokens);

  const TokenPartitionTree *format_tokens_partitions = nullptr;
  // TODO(fangism): The following block could be parallelized because
  // full-partitioning does not depend on format annotations.
  {
    // Annotate inter-token information between all adjacent PreFormatTokens.
    // This must be done before any decisions about ExpandableTreeView
    // can be made because they depend on minimum-spacing, and must-break.
    AnnotateFormattingInformation(style_, text_structure_,
                                  &unwrapper_data.preformatted_tokens);

    // Determine ranges of disabling the formatter, based on comment controls.
    disabled_ranges_.Union(DisableFormattingRanges(full_text, token_stream));

    // Find disabled formatting ranges for specific syntax tree node types.
    // These are typically temporary workarounds for sections that users
    // habitually prefer to format themselves.
    if (const auto &root = text_structure_.SyntaxTree()) {
      DisableSyntaxBasedRanges(&disabled_ranges_, *root, style_, full_text);
    }

    // Disable formatting ranges.
    verible::PreserveSpacesOnDisabledTokenRanges(
        &unwrapper_data.preformatted_tokens, disabled_ranges_, full_text);

    // Partition PreFormatTokens into candidate unwrapped lines.
    format_tokens_partitions = tree_unwrapper.Unwrap();
  }

  {
    // For debugging only: identify largest leaf partitions, and stop.
    if (control.show_token_partition_tree) {
      control.Stream() << "Full token partition tree:\n"
                       << verible::TokenPartitionTreePrinter(
                              *format_tokens_partitions,
                              control.show_inter_token_info)
                       << std::endl;
    }
    if (control.show_largest_token_partitions != 0) {
      PrintLargestPartitions(control.Stream(), *format_tokens_partitions,
                             control.show_largest_token_partitions,
                             text_structure_.GetLineColumnMap(), full_text);
    }
    if (control.AnyStop()) {
      return absl::OkStatus();
    }
  }

  {  // In this pass, perform additional modifications to the partitions and
     // spacings.
    tree_unwrapper.ApplyPreOrder([&](TokenPartitionTree &node) {
      const auto &uwline = node.Value();
      const auto partition_policy = uwline.PartitionPolicy();

      switch (partition_policy) {
        case PartitionPolicyEnum::kAppendFittingSubPartitions:
          // Reshape partition tree with kAppendFittingSubPartitions policy
          verible::ReshapeFittingSubpartitions(style_, &node);
          break;
        case PartitionPolicyEnum::kJuxtaposition:
        case PartitionPolicyEnum::kStack:
        case PartitionPolicyEnum::kWrap:
        case PartitionPolicyEnum::kJuxtapositionOrIndentedStack:
          verible::OptimizeTokenPartitionTree(style_, &node);
          break;
        case PartitionPolicyEnum::kTabularAlignment:
          // TODO(b/145170750): Adjust inter-token spacing to achieve alignment,
          // but leave partitioning intact.
          // This relies on inter-token spacing having already been annotated.
          TabularAlignTokenPartitions(style_, full_text, disabled_ranges_,
                                      &node);
          break;
        default:
          break;
      }
    });
  }

  // Apply token spacing from partitions to tokens. This is permanent, so it
  // must be done after all reshaping is done.
  {
    auto *root = tree_unwrapper.CurrentTokenPartition();
    auto node_iter = VectorTreeLeavesIterator(&LeftmostDescendant(*root));
    const auto end = ++VectorTreeLeavesIterator(&RightmostDescendant(*root));

    // Iterate over leaves. kAlreadyFormatted partitions are either leaves
    // themselves or parents of leaf partitions with kInline policy.
    for (; node_iter != end; ++node_iter) {
      const auto partition_policy = node_iter->Value().PartitionPolicy();

      if (partition_policy == PartitionPolicyEnum::kAlreadyFormatted) {
        verible::ApplyAlreadyFormattedPartitionPropertiesToTokens(
            &(*node_iter), &unwrapper_data.preformatted_tokens);
      } else if (partition_policy == PartitionPolicyEnum::kInline) {
        auto *parent = node_iter->Parent();
        CHECK_NOTNULL(parent);
        CHECK_EQ(parent->Value().PartitionPolicy(),
                 PartitionPolicyEnum::kAlreadyFormatted);
        // This removes the node pointed to by node_iter (and all other
        // siblings)
        verible::ApplyAlreadyFormattedPartitionPropertiesToTokens(
            parent, &unwrapper_data.preformatted_tokens);
        // Move to the parent which is now a leaf
        node_iter = verible::VectorTreeLeavesIterator(parent);
      }
    }
  }

  // Produce sequence of independently operable UnwrappedLines.
  const auto unwrapped_lines = MakeUnwrappedLinesWorklist(
      style_, full_text, disabled_ranges_, *format_tokens_partitions,
      &unwrapper_data.preformatted_tokens);

  // For each UnwrappedLine: minimize total penalty of wrap/break decisions.
  // TODO(fangism): This could be parallelized if results are written
  // to their own 'slots'.
  std::vector<const UnwrappedLine *> partially_formatted_lines;
  formatted_lines_.reserve(unwrapped_lines.size());
  ContinuationCommentAligner continuation_comment_aligner(
      text_structure_.GetLineColumnMap(), text_structure_.Contents());
  for (const auto &uwline : unwrapped_lines) {
    // TODO(fangism): Use different formatting strategies depending on
    // uwline.PartitionPolicy().
    if (continuation_comment_aligner.HandleLine(uwline, &formatted_lines_)) {
    } else if (uwline.PartitionPolicy() ==
               PartitionPolicyEnum::kAlreadyFormatted) {
      // For partitions that were successfully aligned, do not search
      // line-wrapping, but instead accept the adjusted padded spacing.
      formatted_lines_.emplace_back(uwline);
    } else {
      // In other case, default to searching for optimal line wrapping.
      const auto optimal_solutions =
          verible::SearchLineWraps(uwline, style_, control.max_search_states);
      if (control.show_equally_optimal_wrappings &&
          optimal_solutions.size() > 1) {
        verible::DisplayEquallyOptimalWrappings(control.Stream(), uwline,
                                                optimal_solutions);
      }
      // Arbitrarily choose the first solution, if there are multiple.
      formatted_lines_.push_back(optimal_solutions.front());
      if (!formatted_lines_.back().CompletedFormatting()) {
        // Copy over any lines that did not finish wrap searching.
        partially_formatted_lines.push_back(&uwline);
      }
    }
  }

  // svfmt: nothing may follow an EOL comment on its line; otherwise it
  // becomes comment text. Some layouts (e.g. comments between a case label
  // and its statement, merged by append-fitting) can still do that, as in
  // Verible; force a line break there.
  for (auto &line : formatted_lines_) {
    auto &tokens = line.MutableTokens();
    for (size_t i = 1; i < tokens.size(); ++i) {
      if (tokens[i - 1].token->token_enum() != verilog_tokentype::TK_EOL_COMMENT) continue;
      auto &before = tokens[i].before;
      if (before.action == verible::SpacingDecision::kWrap) continue;
      if (before.action == verible::SpacingDecision::kPreserve &&
          tokens[i].OriginalLeadingSpaces().find('\n') != std::string_view::npos) {
        continue;
      }
      before.action = verible::SpacingDecision::kWrap;
      before.spaces = line.IndentationSpaces();
    }
  }
  // svfmt: an escaped identifier ends at whitespace; "\\a ," must not
  // become "\\a,". Alignment can drop that space.
  for (auto &line : formatted_lines_) {
    auto &tokens = line.MutableTokens();
    for (size_t i = 1; i < tokens.size(); ++i) {
      const auto text = tokens[i - 1].token->text();
      if (text.empty() || text.front() != '\\') continue;
      auto &before = tokens[i].before;
      if ((before.action == verible::SpacingDecision::kAppend ||
           before.action == verible::SpacingDecision::kAlign) &&
          before.spaces == 0) {
        before.spaces = 1;
      }
    }
  }

  // Report any unwrapped lines that failed to complete wrap searching.
  if (!partially_formatted_lines.empty()) {
    std::ostringstream err_stream;
    err_stream << "*** Some token partitions failed to complete within the "
                  "search limit:"
               << std::endl;
    for (const auto *line : partially_formatted_lines) {
      err_stream << *line << std::endl;
    }
    err_stream << "*** end of partially formatted partition list" << std::endl;
    // Treat search state limit like a limited resource.
    return absl::ResourceExhaustedError(err_stream.str());
  }

  return absl::OkStatus();
}

// From options, extract the line terminator style. If 'auto' was chosen,
// attempt to determine from text.
static verible::LineTerminatorStyle DetermineOutputLineTerminator(
    verible::LineTerminatorOptionStyle from_options, std::string_view text) {
  static constexpr int32_t kCountAtMost = 100;  // sufficient stats
  switch (from_options) {
    case verible::LineTerminatorOptionStyle::kCRLF:
      return verible::LineTerminatorStyle::kCRLF;
    case verible::LineTerminatorOptionStyle::kLF:
      return verible::LineTerminatorStyle::kLF;
    case verible::LineTerminatorOptionStyle::kAuto:
      return verible::GuessLineTerminator(text, kCountAtMost);
  }
  return verible::LineTerminatorStyle::kLF;
}

void Formatter::Emit(bool include_disabled, std::ostream &stream) const {
  const std::string_view full_text(text_structure_.Contents());
  std::function<bool(const verible::TokenInfo &)> include_token_p;
  if (include_disabled) {
    include_token_p = [](const verible::TokenInfo &) { return true; };
  } else {
    include_token_p = [this, &full_text](const verible::TokenInfo &tok) {
      return !disabled_ranges_.Contains(tok.left(full_text));
    };
  }

  const verible::LineTerminatorStyle out_terminator =
      DetermineOutputLineTerminator(style_.line_terminator, full_text);
  int position = 0;  // tracks with the position in the original full_text
  for (const verible::FormattedExcerpt &line : formatted_lines_) {
    // TODO(fangism): The handling of preserved spaces before tokens is messy:
    // some of it is handled here, some of it is inside FormattedToken.
    // TODO(mglb): Test empty line handling when this method becomes testable.
    const auto front_offset =
        line.Tokens().empty() ? position
                              : line.Tokens().front().token->left(full_text);
    const std::string_view leading_whitespace(
        full_text.substr(position, front_offset - position));
    FormatWhitespaceWithDisabledByteRanges(full_text, leading_whitespace,
                                           disabled_ranges_, include_disabled,
                                           stream, out_terminator);

    // When front of first token is format-disabled, the previous call will
    // already cover the space up to the front token, in which case,
    // the left-indentation for this line should be suppressed to avoid
    // being printed twice.
    if (!line.Tokens().empty()) {
      line.FormattedText(stream, !disabled_ranges_.Contains(front_offset),
                         include_token_p);
      position = line.Tokens().back().token->right(full_text);
    }
  }

  // Handle trailing spaces after last token.
  const std::string_view trailing_whitespace(full_text.substr(position));
  FormatWhitespaceWithDisabledByteRanges(full_text, trailing_whitespace,
                                         disabled_ranges_, include_disabled,
                                         stream, out_terminator);
}

}  // namespace formatter
}  // namespace verilog

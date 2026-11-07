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

#include "verible/verilog/formatting/align.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <string_view>
#include <vector>

#include "absl/log/die_if_null.h"
#include "absl/strings/ascii.h"
#include "verible/common/formatting/align.h"
#include "verible/common/formatting/format-token.h"
#include "verible/common/formatting/token-partition-tree.h"
#include "verible/common/formatting/unwrapped-line.h"
#include "verible/common/strings/position.h"
#include "verible/common/text/concrete-syntax-leaf.h"
#include "verible/common/text/concrete-syntax-tree.h"
#include "verible/common/text/symbol.h"
#include "verible/common/text/token-info.h"
#include "verible/common/text/tree-context-visitor.h"
#include "verible/common/text/tree-utils.h"
#include "verible/common/util/logging.h"
#include "verible/common/util/value-saver.h"
#include "slang/syntax/SyntaxKind.h"
#include "svfmt/group-kinds.h"
#include "svfmt/list-kinds.h"
#include "verible/verilog/formatting/format-style.h"
#include "verible/verilog/parser/verilog-token-classifications.h"
#include "verible/verilog/parser/verilog-token-enum.h"

namespace verilog {
namespace formatter {

using verible::AlignablePartitionGroup;
using verible::AlignedPartitionClassification;
using verible::AlignmentCellScannerGenerator;
using verible::AlignmentColumnProperties;
using verible::AlignmentGroupAction;
using verible::ByteOffsetSet;
using verible::ColumnSchemaScanner;
using verible::ExtractAlignmentGroupsFunction;
using verible::FormatTokenRange;
using verible::PreFormatToken;
using verible::Symbol;
using verible::SyntaxTreeLeaf;
using verible::SyntaxTreeNode;
using verible::SyntaxTreePath;
using verible::TaggedTokenPartitionRange;
using verible::TokenPartitionRange;
using verible::TokenPartitionTree;
using verible::TreePathFormatter;
using verible::ValueSaver;
using slang::syntax::SyntaxKind;
using svfmt::GroupKind;
using svfmt::ListKind;

static constexpr AlignmentColumnProperties FlushLeft(true);
static constexpr AlignmentColumnProperties FlushRight(false);

static const SyntaxTreePath::value_type kLeadingNonTreeTokenPathIndex = -1;
static const SyntaxTreePath::value_type kTrailingNonTreeTokenPathIndex =
    std::numeric_limits<SyntaxTreePath::value_type>::max();

// Maximum SyntaxTreePath index available for tree tokens
static const SyntaxTreePath::value_type kMaxPathIndex =
    std::numeric_limits<SyntaxTreePath::value_type>::max() - 1;

template <class T>
static bool TokensAreAllCommentsOrAttributes(const T &tokens) {
  return std::all_of(
      tokens.begin(), tokens.end(), [](const typename T::value_type &token) {
        const auto tag = static_cast<verilog_tokentype>(token.TokenEnum());
        return IsComment(verilog_tokentype(tag)) ||
               tag == verilog_tokentype::TK_ATTRIBUTE;
      });
}

template <class T>
static bool TokensHaveParenthesis(const T &tokens) {
  return std::any_of(tokens.begin(), tokens.end(),
                     [](const typename T::value_type &token) {
                       return token.TokenEnum() == '(';
                     });
}

// Returns true if the partition contains a separator comment line.
// A separator comment is an EOL comment whose body contains a run of 4 or
// more consecutive identical "divider" characters, where a divider character
// is any non-alphanumeric, non-whitespace character (e.g. '-', '=', '*',
// '#', '/').  The run may be surrounded by other text, so both a bare rule
// and a captioned one are recognised.  Examples: "// ----", "// ====",
// "/////////////////////", "// ------ section heading ------".
static bool IsSeparatorComment(const TokenPartitionTree &partition) {
  const auto &uwline = partition.Value();
  const auto token_range = uwline.TokensRange();
  if (token_range.empty()) return false;

  constexpr int kMinRunLength = 4;
  for (const auto &ftoken : token_range) {
    if (ftoken.TokenEnum() !=
        static_cast<int>(verilog_tokentype::TK_EOL_COMMENT)) {
      continue;
    }

    std::string_view text = ftoken.Text();
    // Strip leading "//"
    if (text.size() < 2 || text[0] != '/' || text[1] != '/') continue;
    const std::string_view body = text.substr(2);

    // Look for a run of kMinRunLength or more consecutive identical divider
    // characters anywhere in the comment body.
    int run = 0;
    char prev = '\0';
    for (const char ch : body) {
      const bool is_divider =
          !absl::ascii_isalnum(ch) && !absl::ascii_isspace(ch);
      if (is_divider && ch == prev) {
        if (++run >= kMinRunLength) return true;
      } else {
        run = is_divider ? 1 : 0;
        prev = ch;
      }
    }
  }
  return false;
}

static bool BlankLinesBreakGroups(AlignmentGroupBoundary b) {
  return b == AlignmentGroupBoundary::kBlankLines ||
         b == AlignmentGroupBoundary::kBlankLinesAndSeparatorComments;
}

static bool SeparatorCommentsBreakGroups(AlignmentGroupBoundary b) {
  return b == AlignmentGroupBoundary::kSeparatorComments ||
         b == AlignmentGroupBoundary::kBlankLinesAndSeparatorComments;
}

// True when non-tree tokens (e.g. // comments, line-continuation `\`) precede
// the origin, and the first origin token must start a new line. Aligning such
// partitions would glue the leading tokens onto the origin line via kInline
// cells (GitHub issue 2539). Leave them out of alignment instead.
static bool PartitionHasLeadingTokensBeforeForcedWrap(
    const TokenPartitionTree &partition) {
  const auto &uwline = partition.Value();
  const verible::Symbol *origin = uwline.Origin();
  if (origin == nullptr) return false;

  const auto ftokens = uwline.TokensRange();
  if (ftokens.empty()) return false;

  const verible::SyntaxTreeLeaf *first_leaf = verible::GetLeftmostLeaf(*origin);
  if (first_leaf == nullptr) return false;

  const verible::TokenInfo &first_tree_token = first_leaf->get();
  auto ftoken_it = ftokens.begin();
  while (ftoken_it != ftokens.end() &&
         *(ftoken_it->token) != first_tree_token) {
    ++ftoken_it;
  }
  if (ftoken_it == ftokens.begin() || ftoken_it == ftokens.end()) return false;
  return ftoken_it->before.break_decision == verible::SpacingOptions::kMustWrap;
}

static bool IgnoreCommentsAndPreprocessingDirectives(
    const TokenPartitionTree &partition) {
  const auto &uwline = partition.Value();

  // ignore partitions with only non-tree tokens (comments, comma-only lines)
  if (!uwline.Origin()) return true;

  const auto token_range = uwline.TokensRange();
  CHECK(!token_range.empty());
  // ignore lines containing only comments
  if (TokensAreAllCommentsOrAttributes(token_range)) return true;

  if (PartitionHasLeadingTokensBeforeForcedWrap(partition)) return true;

  // ignore partitions belonging to preprocessing directives
  return IsPreprocessorKeyword(
      verilog_tokentype(token_range.front().TokenEnum()));
}

static bool IgnoreWithinPortDeclarationPartitionGroup(
    const TokenPartitionTree &partition) {
  const auto &uwline = partition.Value();
  const auto token_range = uwline.TokensRange();
  CHECK(!token_range.empty());
  if (IgnoreCommentsAndPreprocessingDirectives(partition)) return true;

  // Ignore .x or .x(x) port declarations.
  // These can appear in a list_of_port_or_port_declarations.
  CHECK_NOTNULL(uwline.Origin());
  // svfmt: non-ANSI port references and explicit ports (".a(b)").
  const auto tag = uwline.Origin()->Tag().tag;
  return uwline.Origin()->Kind() == verible::SymbolKind::kNode &&
         (tag == static_cast<int>(SyntaxKind::ImplicitNonAnsiPort) ||
          tag == static_cast<int>(SyntaxKind::ExplicitNonAnsiPort) ||
          tag == static_cast<int>(SyntaxKind::ExplicitAnsiPort));
}

static bool IgnoreWithinStructUnionMemberPartitionGroup(
    const TokenPartitionTree &partition) {
  const auto &uwline = partition.Value();
  const auto token_range = uwline.TokensRange();
  CHECK(!token_range.empty());
  // TODO(mglb): Verify whether `IgnoreCommentsAndPreprocessingDirectives` can
  // be used instead of a direct comments/preprocessor tests (like in other
  // IgnoreWithin* functions). If so, use it.

  // ignore lines containing only comments
  if (TokensAreAllCommentsOrAttributes(token_range)) {
    return true;
  }

  // ignore partitions belonging to preprocessing directives
  if (IsPreprocessorKeyword(
          verilog_tokentype(token_range.front().TokenEnum()))) {
    return true;
  }

  if (PartitionHasLeadingTokensBeforeForcedWrap(partition)) return true;

  // ignore nested structs/unions
  if (verible::FindFirstSubtree(
          partition.Value().Origin(), [](const Symbol &symbol) {
            return symbol.Tag() ==
                   verible::NodeTag(ListKind::StructUnionType_members);
          }) != nullptr) {
    return true;
  }

  return false;
}

static bool IgnoreWithinActualNamedParameterPartitionGroup(
    const TokenPartitionTree &partition) {
  if (IgnoreCommentsAndPreprocessingDirectives(partition)) return true;

  // ignore everything that isn't passing a parameter by name
  const auto &uwline = partition.Value();
  CHECK_NOTNULL(uwline.Origin());
  return !(uwline.Origin()->Kind() == verible::SymbolKind::kNode &&
           verible::SymbolCastToNode(*uwline.Origin())
               .MatchesTag(SyntaxKind::NamedParamAssignment));
}

static bool IgnoreWithinActualNamedPortPartitionGroup(
    const TokenPartitionTree &partition) {
  if (IgnoreCommentsAndPreprocessingDirectives(partition)) return true;

  const auto &uwline = partition.Value();
  const auto token_range = uwline.TokensRange();

  // ignore wildcard connections .*
  if (verilog_tokentype(token_range.front().TokenEnum()) ==
      verilog_tokentype::TK_DOTSTAR) {
    return true;
  }

  CHECK_NOTNULL(uwline.Origin());
  if (uwline.Origin()->Kind() != verible::SymbolKind::kNode) return true;

  // ignore implicit connections .aaa
  if (verible::SymbolCastToNode(*uwline.Origin())
          .MatchesTag(SyntaxKind::NamedPortConnection) &&
      !TokensHaveParenthesis(token_range)) {
    return true;
  }

  // ignore positional port connections
  if (verible::SymbolCastToNode(*uwline.Origin())
          .MatchesTag(SyntaxKind::OrderedPortConnection)) {
    return true;
  }

  return false;
}

static bool TokenForcesLineBreak(const PreFormatToken &ftoken) {
  switch (ftoken.TokenEnum()) {
    case verilog_tokentype::TK_begin:
    case verilog_tokentype::TK_fork:
      return true;
    default:
      return false;
  }
}

static bool IgnoreMultilineCaseStatements(const TokenPartitionTree &partition) {
  if (IgnoreCommentsAndPreprocessingDirectives(partition)) return true;

  const auto &uwline = partition.Value();
  const auto token_range = uwline.TokensRange();

  // Scan for any tokens that would force a line break.
  return std::any_of(token_range.begin(), token_range.end(),
                     &TokenForcesLineBreak);
}

// svfmt: the column scanners below are rewritten for the slang-derived CST.
// Verible's scanners rely on positional tree paths, which are stable in
// Verible's CST (absent optional children are null placeholders). The slang
// bridge omits absent children, so here every column is reserved with an
// explicit "semantic" path that is the same in every row of a group.

static const SyntaxTreeNode *AsNode(const verible::Symbol *s) {
  return (s && s->Kind() == verible::SymbolKind::kNode) ? &verible::SymbolCastToNode(*s)
                                                        : nullptr;
}
static const verible::SyntaxTreeLeaf *AsLeaf(const verible::Symbol *s) {
  return (s && s->Kind() == verible::SymbolKind::kLeaf) ? &verible::SymbolCastToLeaf(*s)
                                                        : nullptr;
}
static SyntaxKind KindOfSym(const verible::Symbol &s) {
  return static_cast<SyntaxKind>(s.Tag().tag);
}
static bool HasTag(const verible::Symbol *s, int tag) {
  return s && s->Kind() == verible::SymbolKind::kNode && s->Tag().tag == tag;
}
static bool HasTag(const verible::Symbol *s, SyntaxKind k) {
  return HasTag(s, static_cast<int>(k));
}
static bool HasTag(const verible::Symbol *s, ListKind k) {
  return HasTag(s, static_cast<int>(k));
}
static bool HasTag(const verible::Symbol *s, GroupKind k) {
  return HasTag(s, static_cast<int>(k));
}

// First direct child node with the given tag.
template <typename TagT>
static const SyntaxTreeNode *ChildWithTag(const SyntaxTreeNode &node, TagT tag) {
  for (const auto &child : node.children()) {
    if (HasTag(child.get(), tag)) return AsNode(child.get());
  }
  return nullptr;
}

// First descendant (pre-order) with the given tag.
template <typename TagT>
static const SyntaxTreeNode *DescendantWithTag(const verible::Symbol &root, TagT tag) {
  const verible::Symbol *found = verible::FindFirstSubtree(
      &root, [tag](const verible::Symbol &s) { return HasTag(&s, tag); });
  return AsNode(found);
}

static const verible::SyntaxTreeLeaf *FirstLeafWithEnum(const verible::Symbol &root,
                                                        int token_enum) {
  const verible::Symbol *found = verible::FindFirstSubtree(
      &root, [token_enum](const verible::Symbol &s) {
        return s.Kind() == verible::SymbolKind::kLeaf && s.Tag().tag == token_enum;
      });
  return AsLeaf(found);
}

static size_t CountChildNodes(const SyntaxTreeNode *node) {
  if (!node) return 0;
  size_t n = 0;
  for (const auto &child : node->children()) {
    if (child && child->Kind() == verible::SymbolKind::kNode) ++n;
  }
  return n;
}

static bool IsDimensionsList(int tag) {
  return tag == static_cast<int>(ListKind::IntegerType_dimensions) ||
         tag == static_cast<int>(ListKind::ImplicitType_dimensions) ||
         tag == static_cast<int>(ListKind::EnumType_dimensions) ||
         tag == static_cast<int>(ListKind::StructUnionType_dimensions) ||
         tag == static_cast<int>(ListKind::Declarator_dimensions);
}

static bool IsNetTypeKeyword(int e) {
  switch (e) {
    case TK_wire:
    case TK_tri:
    case TK_tri1:
    case TK_supply0:
    case TK_wand:
    case TK_triand:
    case TK_tri0:
    case TK_supply1:
    case TK_wor:
    case TK_trior:
    case TK_wone:
    case TK_uwire:
      return true;
    default:
      return false;
  }
}

class VerilogColumnSchemaScanner : public ColumnSchemaScanner {
 public:
  explicit VerilogColumnSchemaScanner(const FormatStyle &style) : style_(style) {}

 protected:
  using Path = SyntaxTreePath;

  // Reserves subcolumns for each VariableDimension of a dimensions list:
  // '[' (left), value (right), ']' (left). `right_align` right-aligns the set
  // of dimensions (the last dimensions share columns across rows).
  void ReserveDimensions(verible::ColumnPositionTree *group, const SyntaxTreeNode &dims,
                         const Path &base, bool right_align) {
    const size_t count = CountChildNodes(&dims);
    size_t i = 0;
    for (const auto &child : dims.children()) {
      const SyntaxTreeNode *dim = AsNode(child.get());
      if (!dim || KindOfSym(*dim) != SyntaxKind::VariableDimension) continue;
      Path dim_path = base;
      dim_path.push_back(right_align ? static_cast<Path::value_type>(kMaxPathIndex - (count - i))
                                     : static_cast<Path::value_type>(i));
      ++i;
      const verible::AlignmentColumnProperties no_border(false, 0);
      auto *column = group ? ReserveNewColumn(group, *dim, right_align ? no_border : FlushLeft,
                                              dim_path)
                           : ReserveNewColumn(*dim, FlushLeft, dim_path);
      if (column == nullptr) continue;
      // children: '[' specifier ']'
      const verible::Symbol *open = dim->size() > 0 ? (*dim)[0].get() : nullptr;
      const verible::Symbol *value = dim->size() > 2 ? (*dim)[1].get() : nullptr;
      const verible::Symbol *close = dim->size() > 0 ? dim->back().get() : nullptr;
      if (open) {
        ReserveNewColumn(column, *open, right_align ? no_border : FlushLeft,
                         GetSubpath(column->Value().path, {0}));
      }
      if (value) {
        ReserveNewColumn(column, *value, FlushRight, GetSubpath(column->Value().path, {1}));
      }
      if (close && close != open) {
        ReserveNewColumn(column, *close, FlushLeft, GetSubpath(column->Value().path, {2}));
      }
    }
  }

  // The dimensions list of a data type node (packed dimensions), if any.
  static const SyntaxTreeNode *PackedDimensions(const SyntaxTreeNode *type) {
    if (!type) return nullptr;
    for (const auto &child : type->children()) {
      if (child && child->Kind() == verible::SymbolKind::kNode &&
          IsDimensionsList(child->Tag().tag)) {
        return AsNode(child.get());
      }
    }
    return nullptr;
  }

  const FormatStyle &style_;
};

template <class ScannerType>
static std::function<verible::AlignmentCellScannerFunction(const FormatStyle &)>
UnstyledAlignmentCellScannerGenerator() {
  return [](const FormatStyle &vstyle) {
    return AlignmentCellScannerGenerator<ScannerType>(
        [vstyle] { return ScannerType(vstyle); });
  };
}

template <class ScannerType>
static std::function<verible::AlignmentCellScannerFunction(const FormatStyle &)>
UnstyledAlignmentCellScannerGenerator(
    const verible::NonTreeTokensScannerFunction &non_tree_column_scanner) {
  return [non_tree_column_scanner](const FormatStyle &vstyle) {
    return AlignmentCellScannerGenerator<ScannerType>(
        [vstyle] { return ScannerType(vstyle); }, non_tree_column_scanner);
  };
}

// Named parameter assignments and named port connections:
// ".name(value)" -> columns at '.' and at '('.
class NamedConnectionColumnSchemaScanner : public VerilogColumnSchemaScanner {
 public:
  explicit NamedConnectionColumnSchemaScanner(const FormatStyle &style)
      : VerilogColumnSchemaScanner(style) {}

 protected:
  void Visit(const SyntaxTreeNode &node) final {
    ReserveNewColumn(node, FlushLeft, {0});
    for (const auto &child : node.children()) {
      if (const auto *leaf = AsLeaf(child.get()); leaf && leaf->get().token_enum() == '(') {
        ReserveNewColumn(*leaf, FlushLeft, {1});
        break;
      }
    }
  }
  void Visit(const SyntaxTreeLeaf &) final {}
};

// ANSI port declarations, e.g. "input wire [3:0] clk [2],".
// Columns: {0} direction, {1} net/var keyword or data type, {2} packed
// dimensions, {3} identifier, {4} unpacked dimensions.
class PortDeclarationColumnSchemaScanner : public VerilogColumnSchemaScanner {
 public:
  explicit PortDeclarationColumnSchemaScanner(const FormatStyle &style)
      : VerilogColumnSchemaScanner(style) {}

 protected:
  void Visit(const SyntaxTreeNode &node) final {
    const SyntaxTreeNode *header = nullptr;
    const SyntaxTreeNode *declarator = nullptr;
    for (const auto &child : node.children()) {
      const auto *n = AsNode(child.get());
      if (!n) continue;
      if (KindOfSym(*n) == SyntaxKind::Declarator) {
        declarator = n;
      } else if (!header) {
        header = n;
      }
    }
    if (header) {
      bool have_type_column = false;
      for (const auto &child : header->children()) {
        if (const auto *leaf = AsLeaf(child.get())) {
          const int e = leaf->get().token_enum();
          if (e == TK_input || e == TK_output || e == TK_inout || e == TK_ref) {
            ReserveNewColumn(*leaf, FlushLeft, {0});
          } else if (!have_type_column &&
                     (IsNetTypeKeyword(e) || e == TK_var || e == SymbolIdentifier ||
                      e == TK_interface)) {
            ReserveNewColumn(*leaf, FlushLeft, {1});
            have_type_column = true;
          }
          continue;
        }
        const auto *type = AsNode(child.get());
        if (!type) continue;
        if (!have_type_column && KindOfSym(*type) != SyntaxKind::ImplicitType) {
          ReserveNewColumn(*type, FlushLeft, {1});
          have_type_column = true;
        }
        if (const auto *dims = PackedDimensions(type)) {
          static const verible::AlignmentColumnProperties single_left_border(true, 1);
          auto *group = ReserveNewColumn(*dims, single_left_border, {2});
          ReserveDimensions(group, *dims, {2},
                            style_.port_declarations_right_align_packed_dimensions);
        }
      }
    }
    if (declarator && !declarator->empty()) {
      ReserveNewColumn(*declarator->front(), FlushLeft, {3});
      if (const auto *dims = ChildWithTag(*declarator, ListKind::Declarator_dimensions)) {
        auto *group = ReserveNewColumn(*dims, FlushLeft, {4});
        ReserveDimensions(group, *dims, {4},
                          style_.port_declarations_right_align_unpacked_dimensions);
      }
    }
  }
  void Visit(const SyntaxTreeLeaf &) final {}
};

// Struct/union members, e.g. "bit [31:0] member_name = 1;".
// Columns: {0} member start, {1} identifier, {2} '='.
class StructUnionMemberColumnSchemaScanner : public VerilogColumnSchemaScanner {
 public:
  explicit StructUnionMemberColumnSchemaScanner(const FormatStyle &style)
      : VerilogColumnSchemaScanner(style) {}

 protected:
  void Visit(const SyntaxTreeNode &node) final {
    ReserveNewColumn(node, FlushLeft, {0});
    const auto *list = ChildWithTag(node, ListKind::StructUnionMember_declarators);
    if (!list || list->empty()) return;
    const auto *declarator = AsNode(list->front().get());
    if (!declarator || declarator->empty()) return;
    ReserveNewColumn(*declarator->front(), FlushLeft, {1});
    if (const auto *init = ChildWithTag(*declarator, SyntaxKind::EqualsValueClause)) {
      ReserveNewColumn(*init, FlushLeft, {2});
    }
  }
  void Visit(const SyntaxTreeLeaf &) final {}
};

static const SyntaxTreeNode *UnwrapClassProperty(const SyntaxTreeNode &node) {
  if (KindOfSym(node) == SyntaxKind::ClassPropertyDeclaration) {
    return ChildWithTag(node, SyntaxKind::DataDeclaration);
  }
  return &node;
}

static const SyntaxTreeNode *DeclaratorList(const SyntaxTreeNode &decl) {
  if (const auto *l = ChildWithTag(decl, ListKind::DataDeclaration_declarators)) return l;
  return ChildWithTag(decl, ListKind::NetDeclaration_declarators);
}

static bool IsAlignableDeclaration(const SyntaxTreeNode &origin) {
  // A data/net/variable declaration is alignable if:
  // * it is not a module instance
  // * it declares exactly one identifier
  const SyntaxTreeNode *node = UnwrapClassProperty(origin);
  if (!node) return false;
  switch (KindOfSym(*node)) {
    case SyntaxKind::DataDeclaration:
    case SyntaxKind::NetDeclaration:
      return CountChildNodes(DeclaratorList(*node)) == 1;
    default:
      return false;
  }
}

// Data, net and class property declarations, e.g. "logic [3:0] a [2] = 0;".
// Columns: {0} start, {1,i} packed dimensions, {2} identifier, {3,i} unpacked
// dimensions, {4} '=' (class properties only).
class DataDeclarationColumnSchemaScanner : public VerilogColumnSchemaScanner {
 public:
  explicit DataDeclarationColumnSchemaScanner(const FormatStyle &style,
                                              bool align_assignment = false)
      : VerilogColumnSchemaScanner(style), align_assignment_(align_assignment) {}

 protected:
  void Visit(const SyntaxTreeNode &origin) final {
    ReserveNewColumn(origin, FlushLeft, {0});
    const SyntaxTreeNode *node = UnwrapClassProperty(origin);
    if (!node) return;
    // Packed dimensions of the declared type.
    const SyntaxTreeNode *type_holder = ChildWithTag(*node, GroupKind::kInstantiationType);
    const SyntaxTreeNode *type = type_holder ? AsNode(type_holder->front().get()) : nullptr;
    if (!type) {
      // NetDeclaration: the data type is a direct child.
      for (const auto &child : node->children()) {
        const auto *n = AsNode(child.get());
        if (n && !svfmt::IsListTag(n->Tag().tag) &&
            KindOfSym(*n) != SyntaxKind::Declarator) {
          type = n;
        }
      }
    }
    if (const auto *dims = PackedDimensions(type)) {
      ReserveDimensions(nullptr, *dims, {1}, false);
    }
    const auto *list = DeclaratorList(*node);
    if (!list || list->empty()) return;
    const auto *declarator = AsNode(list->front().get());
    if (!declarator || declarator->empty()) return;
    ReserveNewColumn(*declarator, FlushLeft, {2});
    if (const auto *dims = ChildWithTag(*declarator, ListKind::Declarator_dimensions)) {
      ReserveDimensions(nullptr, *dims, {3}, false);
    }
    if (align_assignment_) {
      if (const auto *init = ChildWithTag(*declarator, SyntaxKind::EqualsValueClause)) {
        ReserveNewColumn(*init, FlushLeft, {4});
      }
    }
  }
  void Visit(const SyntaxTreeLeaf &) final {}

 private:
  bool align_assignment_;
};

class ClassPropertyColumnSchemaScanner : public DataDeclarationColumnSchemaScanner {
 public:
  explicit ClassPropertyColumnSchemaScanner(const FormatStyle &style)
      : DataDeclarationColumnSchemaScanner(style, /*align_assignment=*/true) {}
};

// Formal and body-level parameter declarations, e.g.
// "localparam int [3:0] Width = 5;", "parameter type T = logic".
// Columns: {0} keyword, {1} type / 'type', {2,i} packed dimensions,
// {3} identifier, {4} '='.
class ParameterDeclarationColumnSchemaScanner : public VerilogColumnSchemaScanner {
 public:
  explicit ParameterDeclarationColumnSchemaScanner(const FormatStyle &style)
      : VerilogColumnSchemaScanner(style) {}

 protected:
  void Visit(const SyntaxTreeNode &origin) final {
    const SyntaxTreeNode *node = &origin;
    if (KindOfSym(origin) == SyntaxKind::ParameterDeclarationStatement) {
      node = AsNode(origin.front().get());
      if (!node) return;
    }
    bool have_type = false;
    const SyntaxTreeNode *declarators = nullptr;
    for (const auto &child : node->children()) {
      if (const auto *leaf = AsLeaf(child.get())) {
        const int e = leaf->get().token_enum();
        if (e == TK_parameter || e == TK_localparam) {
          ReserveNewColumn(*leaf, FlushLeft, {0});
        } else if (e == TK_type && !have_type) {
          ReserveNewColumn(*leaf, FlushLeft, {1});
          have_type = true;
        }
        continue;
      }
      const auto *n = AsNode(child.get());
      if (!n) continue;
      if (n->Tag().tag == static_cast<int>(ListKind::ParameterDeclaration_declarators) ||
          n->Tag().tag == static_cast<int>(ListKind::TypeParameterDeclaration_declarators)) {
        declarators = n;
        continue;
      }
      if (!have_type && KindOfSym(*n) != SyntaxKind::ImplicitType) {
        ReserveNewColumn(*n, FlushLeft, {1});
        have_type = true;
      }
      if (const auto *dims = PackedDimensions(n)) {
        ReserveDimensions(nullptr, *dims, {2}, false);
      }
    }
    if (!declarators || declarators->empty()) return;
    const auto *declarator = AsNode(declarators->front().get());
    if (!declarator || declarator->empty()) return;
    ReserveNewColumn(*declarator->front(), FlushLeft, {3});
    for (const auto &child : declarator->children()) {
      const auto *n = AsNode(child.get());
      if (n && (KindOfSym(*n) == SyntaxKind::EqualsValueClause ||
                KindOfSym(*n) == SyntaxKind::EqualsTypeClause)) {
        ReserveNewColumn(*n, FlushLeft, {4});
      }
    }
  }
  void Visit(const SyntaxTreeLeaf &) final {}
};

// Case items, e.g. "value1, value2: x = f(y);".
// Columns: {0} item start, {1} statement after ':'.
class CaseItemColumnSchemaScanner : public VerilogColumnSchemaScanner {
 public:
  explicit CaseItemColumnSchemaScanner(const FormatStyle &style)
      : VerilogColumnSchemaScanner(style) {}

 protected:
  void Visit(const SyntaxTreeNode &node) final {
    ReserveNewColumn(node, FlushLeft, {0});
    bool after_colon = false;
    for (size_t i = 0; i < node.size(); ++i) {
      const verible::Symbol *child = node[i].get();
      if (!child) continue;
      if (after_colon) {
        ReserveNewColumn(*child, FlushLeft, {1});
        break;
      }
      const auto *leaf = AsLeaf(child);
      if (!leaf) continue;
      if (leaf->get().token_enum() == ':') {
        after_colon = true;
      } else if (leaf->get().token_enum() == TK_default) {
        // "default" may omit the ':'.
        const auto *next = i + 1 < node.size() ? AsLeaf(node[i + 1].get()) : nullptr;
        after_colon = !(next && next->get().token_enum() == ':');
      }
    }
  }
  void Visit(const SyntaxTreeLeaf &) final {}
};

// Assignments, e.g. "assign foo = bar;", "foo = bar;", "foo <= bar;".
// Columns: {0} start, {1} assignment operator.
class AssignmentColumnSchemaScanner : public VerilogColumnSchemaScanner {
 public:
  explicit AssignmentColumnSchemaScanner(const FormatStyle &style)
      : VerilogColumnSchemaScanner(style) {}

 protected:
  void Visit(const SyntaxTreeNode &node) final {
    ReserveNewColumn(node, FlushLeft, {0});
    const SyntaxTreeNode *assignment = nullptr;
    if (KindOfSym(node) == SyntaxKind::ContinuousAssign) {
      const auto *list = ChildWithTag(node, ListKind::ContinuousAssign_assignments);
      if (list && !list->empty()) assignment = AsNode(list->front().get());
    } else {
      assignment = AsNode(node.front().get());
    }
    if (!assignment) return;
    for (const auto &child : assignment->children()) {
      if (const auto *leaf = AsLeaf(child.get())) {
        const int e = leaf->get().token_enum();
        if (e == '=' || e == TK_LE) {
          ReserveNewColumn(*leaf, FlushLeft, {1});
          break;
        }
      }
    }
  }
  void Visit(const SyntaxTreeLeaf &) final {}
};

// Enum members with assignments: "foo = 42" -> columns at name and '='.
class EnumWithAssignmentsColumnSchemaScanner : public VerilogColumnSchemaScanner {
 public:
  explicit EnumWithAssignmentsColumnSchemaScanner(const FormatStyle &style)
      : VerilogColumnSchemaScanner(style) {}

 protected:
  void Visit(const SyntaxTreeNode &node) final {
    ReserveNewColumn(node, FlushLeft, {0});
    if (const auto *init = ChildWithTag(node, SyntaxKind::EqualsValueClause)) {
      ReserveNewColumn(*init, FlushLeft, {1});
    }
  }
  void Visit(const SyntaxTreeLeaf &) final {}
};

// Distribution items align on the := and :/ operators, and on value ranges.
class DistItemColumnSchemaScanner : public VerilogColumnSchemaScanner {
 public:
  explicit DistItemColumnSchemaScanner(const FormatStyle &style)
      : VerilogColumnSchemaScanner(style) {}

 protected:
  void Visit(const SyntaxTreeNode &node) final {
    auto *item_column = ReserveNewColumn(node, FlushLeft, {0});
    const auto *range = ChildWithTag(node, SyntaxKind::ValueRangeExpression);
    if (range && range->size() == 5 && item_column) {
      static constexpr AlignmentColumnProperties kProps[5] = {FlushLeft, FlushRight, FlushLeft,
                                                              FlushRight, FlushLeft};
      for (int i = 0; i < 5; ++i) {
        if ((*range)[i]) {
          ReserveNewColumn(item_column, *(*range)[i], kProps[i],
                           GetSubpath(item_column->Value().path, {i}));
        }
      }
    }
    for (const auto &child : node.children()) {
      const auto *weight = AsNode(child.get());
      if (!weight || KindOfSym(*weight) != SyntaxKind::DistWeight) continue;
      if (const auto *op = AsLeaf(weight->front().get())) {
        ReserveNewColumn(*op, FlushLeft, {1});
      }
    }
  }
  void Visit(const SyntaxTreeLeaf &) final {}
};

// These enums classify alignable groups of token partitions by their syntax
// structure, which then map to different alignment handler routines.
// These need not have a 1:1 correspondence to verilog::NodeEnum syntax tree
// enums, a single value here could apply to a group of syntax tree node types.
enum class AlignableSyntaxSubtype {
  kDontCare = 0,
  kNamedActualParameters,
  kNamedActualPorts,
  kParameterDeclaration,      // formal parameter list (#(...))
  kBodyParameterDeclaration,  // parameter/localparam in module/generate/
                              //   package/interface body
  kPortDeclaration,
  kStructUnionMember,
  kDataDeclaration,  // net/variable declarations
  kClassMemberVariables,
  kCaseLikeItems,
  kContinuousAssignment,
  kEnumListAssignment,  // Constants aligned in enums.
  kBlockingAssignment,
  kNonBlockingAssignment,
  kDistItem,  // Distribution items.
};

static AlignedPartitionClassification AlignClassify(
    AlignmentGroupAction match,
    AlignableSyntaxSubtype subtype = AlignableSyntaxSubtype::kDontCare) {
  if (match == AlignmentGroupAction::kMatch) {
    CHECK(subtype != AlignableSyntaxSubtype::kDontCare);
  }
  return {match, static_cast<int>(subtype)};
}

static std::vector<TaggedTokenPartitionRange> GetConsecutiveModuleItemGroups(
    const TokenPartitionRange &partitions, AlignmentGroupBoundary boundary) {
  VLOG(2) << __FUNCTION__;
  return GetPartitionAlignmentSubranges(
      partitions,  //
      [boundary](const TokenPartitionTree &partition)
          -> AlignedPartitionClassification {
        const Symbol *origin = partition.Value().Origin();
        if (origin == nullptr) {
          if (SeparatorCommentsBreakGroups(boundary) &&
              IsSeparatorComment(partition)) {
            return {AlignmentGroupAction::kNoMatch};
          }
          return {AlignmentGroupAction::kIgnore};
        }
        const verible::SymbolTag symbol_tag = origin->Tag();
        if (symbol_tag.kind != verible::SymbolKind::kNode) {
          return AlignClassify(AlignmentGroupAction::kIgnore);
        }
        const SyntaxTreeNode &node = verible::SymbolCastToNode(*origin);
        // Align body-level parameter/localparam declarations.
        if (node.MatchesTagAnyOf({SyntaxKind::ParameterDeclarationStatement,
                                  SyntaxKind::ParameterDeclaration})) {
          return AlignClassify(
              AlignmentGroupAction::kMatch,
              AlignableSyntaxSubtype::kBodyParameterDeclaration);
        }

        // Align net/variable declarations.
        if (IsAlignableDeclaration(node)) {
          return AlignClassify(AlignmentGroupAction::kMatch,
                               AlignableSyntaxSubtype::kDataDeclaration);
        }
        // Align continuous assignment, like "assign foo = bar;"
        if (node.MatchesTag(SyntaxKind::ContinuousAssign) &&
            CountChildNodes(ChildWithTag(node, ListKind::ContinuousAssign_assignments)) == 1) {
          return AlignClassify(AlignmentGroupAction::kMatch,
                               AlignableSyntaxSubtype::kContinuousAssignment);
        }
        return AlignClassify(AlignmentGroupAction::kNoMatch);
      });
}

static std::vector<TaggedTokenPartitionRange> GetConsecutiveClassItemGroups(
    const TokenPartitionRange &partitions, AlignmentGroupBoundary boundary) {
  VLOG(2) << __FUNCTION__;
  return GetPartitionAlignmentSubranges(
      partitions,  //
      [boundary](const TokenPartitionTree &partition)
          -> AlignedPartitionClassification {
        const Symbol *origin = partition.Value().Origin();
        if (origin == nullptr) {
          if (SeparatorCommentsBreakGroups(boundary) &&
              IsSeparatorComment(partition)) {
            return {AlignmentGroupAction::kNoMatch};
          }
          return {AlignmentGroupAction::kIgnore};
        }
        const verible::SymbolTag symbol_tag = origin->Tag();
        if (symbol_tag.kind != verible::SymbolKind::kNode) {
          return {AlignmentGroupAction::kIgnore};
        }
        const SyntaxTreeNode &node = verible::SymbolCastToNode(*origin);
        // Align class member variables.
        return AlignClassify(IsAlignableDeclaration(node)
                                 ? AlignmentGroupAction::kMatch
                                 : AlignmentGroupAction::kNoMatch,
                             AlignableSyntaxSubtype::kClassMemberVariables);
      });
}

static std::vector<TaggedTokenPartitionRange> GetAlignableStatementGroups(
    const TokenPartitionRange &partitions, AlignmentGroupBoundary boundary) {
  VLOG(2) << __FUNCTION__;
  return GetPartitionAlignmentSubranges(
      partitions,  //
      [boundary](const TokenPartitionTree &partition)
          -> AlignedPartitionClassification {
        const Symbol *origin = partition.Value().Origin();
        if (origin == nullptr) {
          if (SeparatorCommentsBreakGroups(boundary) &&
              IsSeparatorComment(partition)) {
            return {AlignmentGroupAction::kNoMatch};
          }
          return {AlignmentGroupAction::kIgnore};
        }
        const verible::SymbolTag symbol_tag = origin->Tag();
        if (symbol_tag.kind != verible::SymbolKind::kNode) {
          return AlignClassify(AlignmentGroupAction::kIgnore);
        }
        const SyntaxTreeNode &node = verible::SymbolCastToNode(*origin);
        // Align local variable declarations.
        if (IsAlignableDeclaration(node)) {
          return AlignClassify(AlignmentGroupAction::kMatch,
                               AlignableSyntaxSubtype::kDataDeclaration);
        }
        // Align blocking assignments.
        const SyntaxTreeNode *expr =
            node.MatchesTag(SyntaxKind::ExpressionStatement) ? AsNode(node.front().get())
                                                             : nullptr;
        if (expr && expr->MatchesTag(SyntaxKind::AssignmentExpression)) {
          return AlignClassify(AlignmentGroupAction::kMatch,
                               AlignableSyntaxSubtype::kBlockingAssignment);
        }
        // Align nonblocking assignments.
        if (expr && expr->MatchesTag(SyntaxKind::NonblockingAssignmentExpression)) {
          return AlignClassify(AlignmentGroupAction::kMatch,
                               AlignableSyntaxSubtype::kNonBlockingAssignment);
        }
        return AlignClassify(AlignmentGroupAction::kNoMatch);
      });
}

static std::function<
    std::vector<TaggedTokenPartitionRange>(const TokenPartitionRange &)>
PartitionBetweenBlankLines(AlignableSyntaxSubtype subtype) {
  return [subtype](const TokenPartitionRange &range) {
    return verible::GetSubpartitionsBetweenBlankLinesSingleTag(
        range, static_cast<int>(subtype));
  };
}

// Each alignment group subtype maps to a set of functions.
struct AlignmentGroupHandlers {
  std::function<verible::AlignmentCellScannerFunction(
      const FormatStyle &vstyle)>
      column_scanner_func;
  std::function<verible::AlignmentPolicy(const FormatStyle &vstyle)>
      policy_func;
};

// Convert a pointer-to-member to a function/lambda that accesses that member.
// Returns the referenced member by value.
// TODO(fangism): move this to an STL-style util/functional library
template <typename MemberType, typename StructType>
static std::function<MemberType(const StructType &)>
function_from_pointer_to_member(MemberType StructType::*member) {
  return [member](const StructType &obj) { return obj.*member; };
}

using AlignmentHandlerMapType =
    std::map<AlignableSyntaxSubtype, AlignmentGroupHandlers>;

static void non_tree_column_scanner(
    verible::FormatTokenRange leading_tokens,
    verible::FormatTokenRange trailing_tokens,
    verible::ColumnPositionTree *column_entries) {
  static const SyntaxTreePath kLeadingTokensPath = {
      kLeadingNonTreeTokenPathIndex};
  static const SyntaxTreePath kTrailingCommaPath = {
      kTrailingNonTreeTokenPathIndex, 0};
  static const SyntaxTreePath kTrailingCommentPath = {
      kTrailingNonTreeTokenPathIndex, 1};

  VLOG(4) << __FUNCTION__ << "\nleading tokens: "
          << verible::StringSpanOfTokenRange(leading_tokens)
          << "\ntrailing tokens: "
          << verible::StringSpanOfTokenRange(trailing_tokens);

  if (!leading_tokens.empty()) {
    column_entries->Children().emplace_back(verible::ColumnPositionEntry{
        kLeadingTokensPath, *leading_tokens.front().token, FlushLeft});
  }

  if (trailing_tokens.empty()) return;

  const auto separator_it =
      std::find_if(trailing_tokens.begin(), trailing_tokens.end(),
                   [](const PreFormatToken &tok) {
                     return tok.TokenEnum() == ',' || tok.TokenEnum() == ':';
                   });

  auto comment_it = trailing_tokens.begin();

  if (separator_it != trailing_tokens.end()) {
    AlignmentColumnProperties prop;
    prop.contains_delimiter = true;
    const verible::ColumnPositionEntry column{kTrailingCommaPath,
                                              *separator_it->token, prop};
    column_entries->Children().emplace_back(column);

    comment_it = separator_it + 1;
  }
  if (comment_it != trailing_tokens.end() &&
      (comment_it->token->token_enum() == TK_COMMENT_BLOCK ||
       comment_it->token->token_enum() == TK_EOL_COMMENT)) {
    const verible::ColumnPositionEntry column{kTrailingCommentPath,
                                              *comment_it->token, FlushLeft};
    column_entries->Children().emplace_back(column);
  }
}

// Global registry of all known alignment handlers for Verilog.
// This organization lets the same handlers be re-used in multiple
// syntactic contexts, e.g. data declarations can be module items and
// generate items and block statement items.
static const AlignmentHandlerMapType &AlignmentHandlerLibrary() {
  static const auto *handler_map = new AlignmentHandlerMapType{
      {AlignableSyntaxSubtype::kDataDeclaration,
       {UnstyledAlignmentCellScannerGenerator<
            DataDeclarationColumnSchemaScanner>(),
        function_from_pointer_to_member(
            &FormatStyle::module_net_variable_alignment)}},
      {AlignableSyntaxSubtype::kNamedActualParameters,
       {UnstyledAlignmentCellScannerGenerator<
            NamedConnectionColumnSchemaScanner>(non_tree_column_scanner),
        function_from_pointer_to_member(
            &FormatStyle::named_parameter_alignment)}},
      {AlignableSyntaxSubtype::kNamedActualPorts,
       {UnstyledAlignmentCellScannerGenerator<
            NamedConnectionColumnSchemaScanner>(non_tree_column_scanner),
        function_from_pointer_to_member(&FormatStyle::named_port_alignment)}},
      {AlignableSyntaxSubtype::kParameterDeclaration,
       {UnstyledAlignmentCellScannerGenerator<
            ParameterDeclarationColumnSchemaScanner>(non_tree_column_scanner),
        function_from_pointer_to_member(
            &FormatStyle::formal_parameters_alignment)}},
      {AlignableSyntaxSubtype::kBodyParameterDeclaration,
       {UnstyledAlignmentCellScannerGenerator<
            ParameterDeclarationColumnSchemaScanner>(non_tree_column_scanner),
        function_from_pointer_to_member(
            &FormatStyle::parameter_declaration_alignment)}},
      {AlignableSyntaxSubtype::kPortDeclaration,
       {UnstyledAlignmentCellScannerGenerator<
            PortDeclarationColumnSchemaScanner>(non_tree_column_scanner),
        function_from_pointer_to_member(
            &FormatStyle::port_declarations_alignment)}},
      {AlignableSyntaxSubtype::kStructUnionMember,
       {UnstyledAlignmentCellScannerGenerator<
            StructUnionMemberColumnSchemaScanner>(non_tree_column_scanner),
        function_from_pointer_to_member(
            &FormatStyle::struct_union_members_alignment)}},
      {AlignableSyntaxSubtype::kClassMemberVariables,
       {UnstyledAlignmentCellScannerGenerator<
            ClassPropertyColumnSchemaScanner>(),
        function_from_pointer_to_member(
            &FormatStyle::class_member_variable_alignment)}},
      {AlignableSyntaxSubtype::kCaseLikeItems,
       {UnstyledAlignmentCellScannerGenerator<CaseItemColumnSchemaScanner>(),
        function_from_pointer_to_member(&FormatStyle::case_items_alignment)}},
      {AlignableSyntaxSubtype::kContinuousAssignment,
       {UnstyledAlignmentCellScannerGenerator<AssignmentColumnSchemaScanner>(),
        function_from_pointer_to_member(
            &FormatStyle::assignment_statement_alignment)}},
      {AlignableSyntaxSubtype::kBlockingAssignment,
       {UnstyledAlignmentCellScannerGenerator<AssignmentColumnSchemaScanner>(),
        function_from_pointer_to_member(
            &FormatStyle::assignment_statement_alignment)}},
      {AlignableSyntaxSubtype::kNonBlockingAssignment,
       {UnstyledAlignmentCellScannerGenerator<AssignmentColumnSchemaScanner>(),
        function_from_pointer_to_member(
            &FormatStyle::assignment_statement_alignment)}},
      {AlignableSyntaxSubtype::kEnumListAssignment,
       {UnstyledAlignmentCellScannerGenerator<
            EnumWithAssignmentsColumnSchemaScanner>(non_tree_column_scanner),
        function_from_pointer_to_member(
            &FormatStyle::enum_assignment_statement_alignment)}},
      {AlignableSyntaxSubtype::kDistItem,
       {UnstyledAlignmentCellScannerGenerator<DistItemColumnSchemaScanner>(),
        function_from_pointer_to_member(
            &FormatStyle::distribution_items_alignment)}},
  };
  return *handler_map;
}

static verible::AlignmentCellScannerFunction AlignmentColumnScannerSelector(
    const FormatStyle &vstyle, int subtype) {
  static const auto &handler_map = AlignmentHandlerLibrary();
  const auto iter = handler_map.find(AlignableSyntaxSubtype(subtype));
  CHECK(iter != handler_map.end()) << "subtype: " << subtype;
  return iter->second.column_scanner_func(vstyle);
}

static verible::AlignmentPolicy AlignmentPolicySelector(
    const FormatStyle &vstyle, int subtype) {
  static const auto &handler_map = AlignmentHandlerLibrary();
  const auto iter = handler_map.find(AlignableSyntaxSubtype(subtype));
  CHECK(iter != handler_map.end()) << "subtype: " << subtype;
  return iter->second.policy_func(vstyle);
}

static std::vector<AlignablePartitionGroup> ExtractAlignablePartitionGroups(
    const std::function<std::vector<TaggedTokenPartitionRange>(
        const TokenPartitionRange &)> &group_extractor,
    const verible::IgnoreAlignmentRowPredicate &ignore_group_predicate,
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  const std::vector<TaggedTokenPartitionRange> ranges(
      group_extractor(full_range));
  std::vector<AlignablePartitionGroup> groups;
  groups.reserve(ranges.size());
  for (const auto &range : ranges) {
    // Use the alignment scanner and policy that correspond to the
    // match_subtype.  This supports aligning a heterogenous collection of
    // alignable partition groups from the same parent partition (full_range).
    groups.emplace_back(
        FilterAlignablePartitions(range.range, ignore_group_predicate),
        AlignmentColumnScannerSelector(vstyle, range.match_subtype),
        AlignmentPolicySelector(vstyle, range.match_subtype));
    if (groups.back().IsEmpty()) groups.pop_back();
  }
  return groups;
}

using AlignSyntaxGroupsFunction =
    std::function<std::vector<AlignablePartitionGroup>(
        const TokenPartitionRange &range, const FormatStyle &style)>;

static std::vector<AlignablePartitionGroup> AlignPortDeclarations(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  return ExtractAlignablePartitionGroups(
      PartitionBetweenBlankLines(AlignableSyntaxSubtype::kPortDeclaration),
      &IgnoreWithinPortDeclarationPartitionGroup, full_range, vstyle);
}

static std::vector<AlignablePartitionGroup> AlignStructUnionMembers(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  return ExtractAlignablePartitionGroups(
      PartitionBetweenBlankLines(AlignableSyntaxSubtype::kStructUnionMember),
      &IgnoreWithinStructUnionMemberPartitionGroup, full_range, vstyle);
}

static std::vector<AlignablePartitionGroup> AlignActualNamedParameters(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  return ExtractAlignablePartitionGroups(
      PartitionBetweenBlankLines(
          AlignableSyntaxSubtype::kNamedActualParameters),
      &IgnoreWithinActualNamedParameterPartitionGroup, full_range, vstyle);
}

static std::vector<AlignablePartitionGroup> AlignActualNamedPorts(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  return ExtractAlignablePartitionGroups(
      PartitionBetweenBlankLines(AlignableSyntaxSubtype::kNamedActualPorts),
      &IgnoreWithinActualNamedPortPartitionGroup, full_range, vstyle);
}

// Extracts alignable partition groups, optionally pre-splitting the range at
// blank lines when the alignment_group_boundary style setting requires it.
static std::vector<AlignablePartitionGroup>
ExtractAlignablePartitionGroupsWithBoundary(
    const std::function<std::vector<TaggedTokenPartitionRange>(
        const TokenPartitionRange &)> &group_extractor,
    const verible::IgnoreAlignmentRowPredicate &ignore_group_predicate,
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  if (!BlankLinesBreakGroups(vstyle.alignment_group_boundary)) {
    return ExtractAlignablePartitionGroups(
        group_extractor, ignore_group_predicate, full_range, vstyle);
  }
  // Pre-split at blank lines, then apply grouping to each sub-range.
  std::vector<AlignablePartitionGroup> all_groups;
  const auto sub_ranges =
      verible::GetSubpartitionsBetweenBlankLines(full_range);
  for (const auto &sub_range : sub_ranges) {
    auto groups = ExtractAlignablePartitionGroups(
        group_extractor, ignore_group_predicate, sub_range, vstyle);
    for (const auto &group : groups) {
      all_groups.push_back(group);
    }
  }
  return all_groups;
}

static std::vector<AlignablePartitionGroup> AlignModuleItems(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  // Applies to module/interface, generate, and package item lists.
  // Handles data/net/variable declarations, parameter/localparam
  // declarations, and continuous assignment statements.
  auto group_extractor = [&vstyle](const TokenPartitionRange &range) {
    return GetConsecutiveModuleItemGroups(range,
                                          vstyle.alignment_group_boundary);
  };
  return ExtractAlignablePartitionGroupsWithBoundary(
      group_extractor, &IgnoreCommentsAndPreprocessingDirectives, full_range,
      vstyle);
}

static std::vector<AlignablePartitionGroup> AlignClassItems(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  // TODO(fangism): align other class items besides member variables.
  auto group_extractor = [&vstyle](const TokenPartitionRange &range) {
    return GetConsecutiveClassItemGroups(range,
                                         vstyle.alignment_group_boundary);
  };
  return ExtractAlignablePartitionGroupsWithBoundary(
      group_extractor, &IgnoreCommentsAndPreprocessingDirectives, full_range,
      vstyle);
}

static std::vector<AlignablePartitionGroup> AlignCaseItems(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  return ExtractAlignablePartitionGroups(
      PartitionBetweenBlankLines(AlignableSyntaxSubtype::kCaseLikeItems),
      &IgnoreMultilineCaseStatements, full_range, vstyle);
}

static std::vector<AlignablePartitionGroup> AlignEnumItems(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  return ExtractAlignablePartitionGroups(
      PartitionBetweenBlankLines(AlignableSyntaxSubtype::kEnumListAssignment),
      &IgnoreCommentsAndPreprocessingDirectives, full_range, vstyle);
}

// Aligns formal parameters in #(...) headers (module/interface/class port
// parameter lists).  Body-level parameter/localparam declarations are
// handled by AlignModuleItems which dispatches through
// GetConsecutiveModuleItemGroups.
static std::vector<AlignablePartitionGroup> AlignParameterDeclarations(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  return ExtractAlignablePartitionGroups(
      PartitionBetweenBlankLines(AlignableSyntaxSubtype::kParameterDeclaration),
      &IgnoreWithinPortDeclarationPartitionGroup, full_range, vstyle);
}

static std::vector<AlignablePartitionGroup> AlignStatements(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  auto group_extractor = [&vstyle](const TokenPartitionRange &range) {
    return GetAlignableStatementGroups(range, vstyle.alignment_group_boundary);
  };
  return ExtractAlignablePartitionGroupsWithBoundary(
      group_extractor, &IgnoreCommentsAndPreprocessingDirectives, full_range,
      vstyle);
}

static std::vector<AlignablePartitionGroup> AlignDistItems(
    const TokenPartitionRange &full_range, const FormatStyle &vstyle) {
  return ExtractAlignablePartitionGroups(
      PartitionBetweenBlankLines(AlignableSyntaxSubtype::kDistItem),
      &IgnoreCommentsAndPreprocessingDirectives, full_range, vstyle);
}

void TabularAlignTokenPartitions(const FormatStyle &style,
                                 std::string_view full_text,
                                 const ByteOffsetSet &disabled_byte_ranges,
                                 TokenPartitionTree *partition_ptr) {
  VLOG(1) << __FUNCTION__;
  auto &partition = *partition_ptr;
  auto &uwline = partition.Value();
  const auto *origin = uwline.Origin();
  VLOG(2) << "origin is nullptr? " << (origin == nullptr);
  if (origin == nullptr) return;
  const SyntaxTreeNode *node = verible::MaybeNode(origin);
  VLOG(2) << "origin is node? " << (node != nullptr);
  if (node == nullptr) return;
  // Dispatch aligning function based on syntax tree node type.

  static const auto *const kAlignHandlers =
      new std::map<int, AlignSyntaxGroupsFunction>{
          {static_cast<int>(ListKind::AnsiPortList_ports), &AlignPortDeclarations},
          {static_cast<int>(ListKind::NonAnsiPortList_ports), &AlignPortDeclarations},
          {static_cast<int>(ListKind::StructUnionType_members), &AlignStructUnionMembers},
          {static_cast<int>(ListKind::ParameterValueAssignment_parameters),
           &AlignActualNamedParameters},
          {static_cast<int>(ListKind::HierarchicalInstance_connections), &AlignActualNamedPorts},
          // module/interface/package bodies, generate blocks
          {static_cast<int>(ListKind::ModuleDeclaration_members), &AlignModuleItems},
          {static_cast<int>(ListKind::GenerateBlock_members), &AlignModuleItems},
          {static_cast<int>(ListKind::GenerateRegion_members), &AlignModuleItems},
          {static_cast<int>(ListKind::ParameterPortList_declarations),
           &AlignParameterDeclarations},
          {static_cast<int>(ListKind::ClassDeclaration_items), &AlignClassItems},
          // various case-like constructs:
          {static_cast<int>(ListKind::CaseStatement_items), &AlignCaseItems},
          {static_cast<int>(ListKind::CaseGenerate_items), &AlignCaseItems},
          {static_cast<int>(ListKind::EnumType_members), &AlignEnumItems},
          // align various statements, like assignments
          {static_cast<int>(ListKind::BlockStatement_items), &AlignStatements},
          {static_cast<int>(ListKind::FunctionDeclaration_items), &AlignStatements},
          {static_cast<int>(ListKind::DistConstraintList_items), &AlignDistItems},
      };
  const auto handler_iter = kAlignHandlers->find(node->Tag().tag);
  if (handler_iter == kAlignHandlers->end()) return;

  const AlignSyntaxGroupsFunction &alignment_partitioner = handler_iter->second;
  const ExtractAlignmentGroupsFunction extract_alignment_groups =
      std::bind(alignment_partitioner, std::placeholders::_1, style);

  verible::TabularAlignTokens(style.column_limit, full_text,
                              disabled_byte_ranges, extract_alignment_groups,
                              &partition);

  VLOG(1) << "end of " << __FUNCTION__;
}

}  // namespace formatter
}  // namespace verilog

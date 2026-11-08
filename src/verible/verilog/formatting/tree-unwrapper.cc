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

#include "verible/verilog/formatting/tree-unwrapper.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/log/die_if_null.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "verible/common/formatting/format-token.h"
#include "verible/common/formatting/token-partition-tree.h"
#include "verible/common/formatting/tree-unwrapper.h"
#include "verible/common/formatting/unwrapped-line.h"
#include "verible/common/strings/display-utils.h"
#include "verible/common/strings/range.h"
#include "verible/common/text/concrete-syntax-leaf.h"
#include "verible/common/text/concrete-syntax-tree.h"
#include "verible/common/text/constants.h"
#include "verible/common/text/symbol.h"
#include "verible/common/text/syntax-tree-context.h"
#include "verible/common/text/text-structure.h"
#include "verible/common/text/token-info.h"
#include "verible/common/text/token-stream-view.h"
#include "verible/common/text/tree-utils.h"
#include "verible/common/util/container-iterator-range.h"
#include "verible/common/util/enum-flags.h"
#include "verible/common/util/iterator-range.h"
#include "verible/common/util/logging.h"
#include "verible/common/util/tree-operations.h"
#include "slang/syntax/AllSyntax.h"
#include "svfmt/group-kinds.h"
#include "svfmt/list-kinds.h"
#include "svfmt/slang-analyzer.h"
#include "svfmt/syntax-context.h"
#include "verible/verilog/formatting/format-style.h"
#include "verible/verilog/formatting/verilog-token.h"
#include "verible/verilog/parser/verilog-token-classifications.h"
#include "verible/verilog/parser/verilog-token-enum.h"

namespace verilog {
namespace formatter {

using ::verible::iterator_range;
using ::verible::NodeTag;
using ::verible::PartitionPolicyEnum;
using ::verible::PreFormatToken;
using ::verible::SpacingOptions;
using ::verible::SymbolTag;
using ::verible::SyntaxTreeNode;
using ::verible::TokenInfo;
using ::verible::TokenPartitionTree;
using ::verible::TokenWithContext;
using ::verilog::IsComment;

// Used to filter the TokenStreamView by discarding space-only tokens.
static bool KeepNonWhitespace(const TokenInfo &t) {
  if (t.token_enum() == verible::TK_EOF) return false;  // omit the EOF token
  return !IsWhitespace(verilog_tokentype(t.token_enum()));
}

// svfmt: the CST is tagged with slang SyntaxKinds, synthetic list kinds
// (svfmt::ListKind) and synthetic group kinds (svfmt::GroupKind), see
// svfmt/slang-analyzer.h. All Verible NodeEnum logic below was rewritten in
// those terms.
using slang::syntax::SyntaxKind;
using svfmt::AncestorKind;
using svfmt::G;
using svfmt::GroupKind;
using svfmt::ListKind;
using svfmt::IsInsideFirstIf;

static SyntaxKind Kind(const verible::Symbol &node) {
  return static_cast<SyntaxKind>(node.Tag().tag);
}
static int Tag(ListKind k) { return static_cast<int>(k); }
static bool IsList(const SyntaxTreeNode &node, ListKind k) {
  return node.Tag().tag == Tag(k);
}

// Innermost enclosing node tag, including list and group nodes.
static int DirectParentTag(const verible::SyntaxTreeContext &context) {
  return context.empty() ? -1 : context.top().Tag().tag;
}

static const SyntaxTreeNode *FindChildNode(const SyntaxTreeNode &node, int tag) {
  for (const auto &child : node.children()) {
    if (child && child->Kind() == verible::SymbolKind::kNode &&
        child->Tag().tag == tag) {
      return &verible::SymbolCastToNode(*child);
    }
  }
  return nullptr;
}

static bool NodeIsBeginEndBlock(const SyntaxTreeNode &node) {
  return Kind(node) == SyntaxKind::SequentialBlockStatement ||
         Kind(node) == SyntaxKind::GenerateBlock;
}

static const SyntaxTreeNode &GetBlockEnd(const SyntaxTreeNode &block) {
  CHECK(NodeIsBeginEndBlock(block));
  return *ABSL_DIE_IF_NULL(FindChildNode(block, static_cast<int>(GroupKind::kEnd)));
}

static const verible::Symbol *GetEndLabel(const SyntaxTreeNode &end_node) {
  return end_node.size() > 1 ? end_node[1].get() : nullptr;
}

static bool NodeIsConditionalConstruct(const SyntaxTreeNode &node) {
  return Kind(node) == SyntaxKind::ConditionalStatement ||
         Kind(node) == SyntaxKind::IfGenerate;
}

static bool NodeIsConditionalOrBlock(const SyntaxTreeNode &node) {
  return NodeIsBeginEndBlock(node) || NodeIsConditionalConstruct(node);
}

// Returns the only node child of a body group (kIfBody/kElseBody), or of a
// kIfClause / ElseClause (via its body group).
static const SyntaxTreeNode *GetAnyControlStatementBody(const SyntaxTreeNode &node) {
  const SyntaxTreeNode *body = nullptr;
  if (node.Tag().tag == static_cast<int>(GroupKind::kIfClause)) {
    body = FindChildNode(node, static_cast<int>(GroupKind::kIfBody));
  } else if (Kind(node) == SyntaxKind::ElseClause) {
    body = FindChildNode(node, static_cast<int>(GroupKind::kElseBody));
  } else {
    body = &node;
  }
  if (body == nullptr) return nullptr;
  for (const auto &child : body->children()) {
    if (child && child->Kind() == verible::SymbolKind::kNode) {
      return &verible::SymbolCastToNode(*child);
    }
  }
  return nullptr;
}

static const SyntaxTreeNode *GetAnyConditionalIfClause(const SyntaxTreeNode &node) {
  return FindChildNode(node, static_cast<int>(GroupKind::kIfClause));
}

static const SyntaxTreeNode *GetAnyConditionalElseClause(const SyntaxTreeNode &node) {
  return FindChildNode(node, static_cast<int>(SyntaxKind::ElseClause));
}

// Last node child (e.g. the body statement of a loop or always block).
static const SyntaxTreeNode *LastChildNode(const SyntaxTreeNode &node) {
  for (size_t i = node.size(); i > 0; --i) {
    const auto &child = node[i - 1];
    if (child && child->Kind() == verible::SymbolKind::kNode) {
      return &verible::SymbolCastToNode(*child);
    }
  }
  return nullptr;
}

static bool IsProceduralBlockKind(SyntaxKind k) {
  switch (k) {
    case SyntaxKind::AlwaysBlock:
    case SyntaxKind::AlwaysCombBlock:
    case SyntaxKind::AlwaysFFBlock:
    case SyntaxKind::AlwaysLatchBlock:
      return true;
    default:
      return false;
  }
}

static bool IsCaseItemKind(SyntaxKind k) {
  return k == SyntaxKind::StandardCaseItem || k == SyntaxKind::DefaultCaseItem ||
         k == SyntaxKind::PatternCaseItem;
}

// Statement kinds that Verible handles under search-anchor STATEMENT_TYPES.
static bool IsStatementKind(SyntaxKind k) {
  switch (k) {
    case SyntaxKind::EmptyStatement:
    case SyntaxKind::ExpressionStatement:
    case SyntaxKind::ProceduralAssignStatement:
    case SyntaxKind::ProceduralForceStatement:
    case SyntaxKind::ProceduralDeassignStatement:
    case SyntaxKind::ProceduralReleaseStatement:
    case SyntaxKind::JumpStatement:
    case SyntaxKind::ReturnStatement:
    case SyntaxKind::DisableStatement:
    case SyntaxKind::DisableForkStatement:
    case SyntaxKind::WaitStatement:
    case SyntaxKind::WaitForkStatement:
    case SyntaxKind::WaitOrderStatement:
    case SyntaxKind::ImmediateAssertStatement:
    case SyntaxKind::ImmediateAssumeStatement:
    case SyntaxKind::ImmediateCoverStatement:
    case SyntaxKind::AssertPropertyStatement:
    case SyntaxKind::AssumePropertyStatement:
    case SyntaxKind::CoverPropertyStatement:
    case SyntaxKind::CoverSequenceStatement:
    case SyntaxKind::ExpectPropertyStatement:
    case SyntaxKind::RestrictPropertyStatement:
    case SyntaxKind::BlockingEventTriggerStatement:
    case SyntaxKind::NonblockingEventTriggerStatement:
    case SyntaxKind::TimingControlStatement:
    case SyntaxKind::CaseStatement:
    case SyntaxKind::RandCaseStatement:
    case SyntaxKind::ForLoopStatement:
    case SyntaxKind::ForeverStatement:
    case SyntaxKind::LoopStatement:
    case SyntaxKind::DoWhileStatement:
    case SyntaxKind::ForeachLoopStatement:
    case SyntaxKind::ConditionalStatement:
      return true;
    default:
      return false;
  }
}

// Verible: kOpenRangeList directly in a kConcatenationExpression that is not
// part of a coverage bin, a ternary, or a binary expression.
static bool IsTopLevelConcatenation(const verible::SyntaxTreeContext &context) {
  if (svfmt::IsInsideAnyOf(context, {SyntaxKind::CoverageBins,
                                     SyntaxKind::ConditionalExpression})) {
    return false;
  }
  return !svfmt::IsInsideIf(context, svfmt::IsVerilogBinaryExpressionKind);
}

static bool IsAssignmentExpressionKind(SyntaxKind k) {
  switch (k) {
    case SyntaxKind::AssignmentExpression:
    case SyntaxKind::NonblockingAssignmentExpression:
    case SyntaxKind::AddAssignmentExpression:
    case SyntaxKind::SubtractAssignmentExpression:
    case SyntaxKind::MultiplyAssignmentExpression:
    case SyntaxKind::DivideAssignmentExpression:
    case SyntaxKind::ModAssignmentExpression:
    case SyntaxKind::AndAssignmentExpression:
    case SyntaxKind::OrAssignmentExpression:
    case SyntaxKind::XorAssignmentExpression:
    case SyntaxKind::LogicalLeftShiftAssignmentExpression:
    case SyntaxKind::LogicalRightShiftAssignmentExpression:
    case SyntaxKind::ArithmeticLeftShiftAssignmentExpression:
    case SyntaxKind::ArithmeticRightShiftAssignmentExpression:
      return true;
    default:
      return false;
  }
}

// List tags of statement/item lists (Verible kBlockItemStatementList etc.).
static bool IsBlockItemList(int tag) {
  return tag == Tag(ListKind::BlockStatement_items) ||
         tag == Tag(ListKind::FunctionDeclaration_items);
}

// True if a function/system call is a system task call ($display(...)).
static bool IsSystemCall(const SyntaxTreeNode &invocation) {
  if (invocation.empty() || !invocation.front()) return false;
  const verible::Symbol *first = invocation.front().get();
  while (first && first->Kind() == verible::SymbolKind::kNode) {
    const auto &n = verible::SymbolCastToNode(*first);
    if (n.empty()) return false;
    first = n.front().get();
  }
  return first && first->Tag().tag == SystemTFIdentifier;
}

// Creates a PreFormatToken given a TokenInfo and returns it
static PreFormatToken CreateFormatToken(const TokenInfo &token) {
  PreFormatToken format_token(&token);
  format_token.format_token_enum =
      GetFormatTokenType(verilog_tokentype(format_token.TokenEnum()));
  switch (format_token.format_token_enum) {
    case FormatTokenType::open_group:
      format_token.balancing = verible::GroupBalancing::kOpen;
      break;
    case FormatTokenType::close_group:
      format_token.balancing = verible::GroupBalancing::kClose;
      break;
    default:
      format_token.balancing = verible::GroupBalancing::kNone;
      break;
  }
  return format_token;
}

UnwrapperData::UnwrapperData(const verible::TokenSequence &tokens) {
  // Create a TokenStreamView that removes spaces, but preserves comments.
  {
    verible::InitTokenStreamView(tokens, &tokens_view_no_whitespace);
    verible::FilterTokenStreamViewInPlace(KeepNonWhitespace,
                                          &tokens_view_no_whitespace);
  }

  // Create an array of PreFormatTokens.
  {
    preformatted_tokens.reserve(tokens_view_no_whitespace.size());
    std::transform(tokens_view_no_whitespace.begin(),
                   tokens_view_no_whitespace.end(),
                   std::back_inserter(preformatted_tokens),
                   [=](verible::TokenSequence::const_iterator iter) {
                     return CreateFormatToken(*iter);
                   });
  }
}

// Represents a state of scanning tokens between syntax tree leaves.
enum TokenScannerState {
  // Initial state: immediately after a leaf token
  kStart,

  // While encountering any string of consecutive newlines
  kHaveNewline,

  // Transition from newline to non-newline
  kNewPartition,

  // Reached next leaf token, stop.  Preserve existing newline.
  kEndWithNewline,

  // Reached next leaf token, stop.
  kEndNoNewline,
};

static const verible::EnumNameMap<TokenScannerState> &
TokenScannerStateStrings() {
  static const verible::EnumNameMap<TokenScannerState>
      kTokenScannerStateStringMap({
          {"kStart", TokenScannerState::kStart},
          {"kHaveNewline", TokenScannerState::kHaveNewline},
          {"kNewPartition", TokenScannerState::kNewPartition},
          {"kEndWithNewline", TokenScannerState::kEndWithNewline},
          {"kEndNoNewline", TokenScannerState::kEndNoNewline},
      });
  return kTokenScannerStateStringMap;
}

// Conventional stream printer (declared in header providing enum).
static std::ostream &operator<<(std::ostream &stream, TokenScannerState p) {
  return TokenScannerStateStrings().Unparse(p, stream);
}

// This finite state machine class is used to determine the placement of
// non-whitespace and non-syntax-tree-node tokens such as comments in
// UnwrappedLines.  The input to this FSM is a Verilog-specific token enum.
// This class is an internal implementation detail of the TreeUnwrapper class.
// TODO(fangism): handle attributes.
class TreeUnwrapper::TokenScanner {
 public:
  TokenScanner() = default;

  // Deleted standard interfaces:
  TokenScanner(const TokenScanner &) = delete;
  TokenScanner(TokenScanner &&) = delete;
  TokenScanner &operator=(const TokenScanner &) = delete;
  TokenScanner &operator=(TokenScanner &&) = delete;

  // Re-initializes state.
  void Reset() {
    current_state_ = State::kStart;
    seen_any_nonspace_ = false;
  }

  // Calls TransitionState using current_state_ and the tranition token_Type
  void UpdateState(verilog_tokentype token_type) {
    current_state_ = TransitionState(current_state_, token_type);
    seen_any_nonspace_ |= IsComment(token_type);
  }

  // Returns true if this is a state that should start a new token partition.
  bool ShouldStartNewPartition() const {
    return current_state_ == State::kNewPartition ||
           (current_state_ == State::kEndWithNewline && seen_any_nonspace_);
  }

 protected:
  using State = TokenScannerState;

  // The current state of the TokenScanner.
  State current_state_ = kStart;
  bool seen_any_nonspace_ = false;

  // Transitions the TokenScanner given a TokenState and a verilog_tokentype
  // transition
  static State TransitionState(const State &old_state,
                               verilog_tokentype token_type);
};

static bool IsNewlineOrEOF(verilog_tokentype token_type) {
  return token_type == verilog_tokentype::TK_NEWLINE ||
         token_type == verible::TK_EOF;
}

/*
 * TransitionState computes the next state in the state machine given
 * a current TokenScanner::State and token_type transition.
 * This state machine is only expected to handle tokens that can appear
 * between syntax tree leaves (whitespace, comments, attributes).
 *
 * Transitions are expected to take place across 3 phases of inter-leaf
 * scanning:
 *   TreeUnwrapper::LookAheadBeyondCurrentLeaf()
 *   TreeUnwrapper::LookAheadBeyondCurrentNode()
 *   TreeUnwrapper::CatchUpToCurrentLeaf()
 */
TokenScannerState TreeUnwrapper::TokenScanner::TransitionState(
    const State &old_state, verilog_tokentype token_type) {
  VLOG(4) << "state transition on: " << old_state
          << ", token: " << svfmt::TokenEnumName(token_type);
  State new_state = old_state;
  switch (old_state) {
    case kStart: {
      if (IsNewlineOrEOF(token_type)) {
        new_state = kHaveNewline;
      } else if (IsComment(token_type)) {
        new_state = kStart;  // same state
      } else {
        new_state = kEndNoNewline;
      }
      break;
    }
    case kHaveNewline: {
      if (IsNewlineOrEOF(token_type)) {
        new_state = kHaveNewline;  // same state
      } else if (IsComment(token_type)) {
        new_state = kNewPartition;
      } else {
        new_state = kEndWithNewline;
      }
      break;
    }
    case kNewPartition: {
      if (IsNewlineOrEOF(token_type)) {
        new_state = kHaveNewline;
      } else if (IsComment(token_type)) {
        new_state = kStart;
      } else {
        new_state = kEndNoNewline;
      }
      break;
    }
    case kEndWithNewline:
    case kEndNoNewline: {
      // Terminal states.  Must Reset() next.
      break;
    }
    default:
      break;
  }
  VLOG(4) << "new state: " << new_state;
  return new_state;
}

void TreeUnwrapper::EatSpaces() {
  // TODO(fangism): consider a NextNonSpaceToken() method that combines
  // NextUnfilteredToken with EatSpaces.
  const auto before = NextUnfilteredToken();
  SkipUnfilteredTokens(
      [](const TokenInfo &token) { return token.token_enum() == TK_SPACE; });
  const auto after = NextUnfilteredToken();
  VLOG(4) << __FUNCTION__ << " ate " << std::distance(before, after)
          << " space tokens";
}

TreeUnwrapper::TreeUnwrapper(const verible::TextStructureView &view,
                             const FormatStyle &style,
                             const preformatted_tokens_type &ftokens)
    : verible::TreeUnwrapper(view, ftokens),
      style_(style),
      inter_leaf_scanner_(new TokenScanner),
      token_context_(FullText(), [](std::ostream &stream, int e) {
        stream << svfmt::TokenEnumName(e);
      }) {
  // Verify that unfiltered token stream is properly EOF terminated,
  // so that stream scanners (inter_leaf_scanner_) know when to stop.
  const auto &tokens = view.TokenStream();
  CHECK(!tokens.empty());
  const auto &back(tokens.back());
  CHECK(back.isEOF());
  CHECK(back.text().empty());
}

TreeUnwrapper::~TreeUnwrapper() = default;

static bool IsPreprocessorClause(int tag) {
  // svfmt: `ifdef clauses are not tree nodes that contain items; directives
  // are standalone nodes. Nothing to suppress here.
  (void)tag;
  return false;
}

// These are constructs where it is permissible to fit on one line, but in the
// event that the statement body is split, we need to ensure it is properly
// indented, even if it is a single statement.
// Keep this list in sync below where the same function name appears in comment.
static bool ShouldIndentRelativeToDirectParent(
    const verible::SyntaxTreeContext &context) {
  switch (AncestorKind(context)) {
    case SyntaxKind::LoopGenerate:
    case SyntaxKind::CaseStatement:
    case SyntaxKind::RandCaseStatement:
    case SyntaxKind::ForLoopStatement:
    case SyntaxKind::ForeverStatement:
    case SyntaxKind::LoopStatement:
    case SyntaxKind::DoWhileStatement:
    case SyntaxKind::ForeachLoopStatement:
    case SyntaxKind::ConditionalStatement:
    case SyntaxKind::ImmediateAssertStatement:
    case SyntaxKind::ImmediateAssumeStatement:
    case SyntaxKind::ImmediateCoverStatement:
    case SyntaxKind::TimingControlStatement:
    case SyntaxKind::AssertPropertyStatement:
    case SyntaxKind::AssumePropertyStatement:
    case SyntaxKind::ExpectPropertyStatement:
    case SyntaxKind::CoverPropertyStatement:
    case SyntaxKind::CoverSequenceStatement:
    case SyntaxKind::WaitStatement:
    case SyntaxKind::InitialBlock:
    case SyntaxKind::AlwaysBlock:
    case SyntaxKind::AlwaysCombBlock:
    case SyntaxKind::AlwaysFFBlock:
    case SyntaxKind::AlwaysLatchBlock:
    case SyntaxKind::FinalBlock:
      return true;
    default:
      // kIfClause in Verible.
      return AncestorKind(context) == G(GroupKind::kIfClause);
  }
}

void TreeUnwrapper::UpdateInterLeafScanner(verilog_tokentype token_type) {
  VLOG(4) << __FUNCTION__ << ", token: " << svfmt::TokenEnumName(token_type);
  inter_leaf_scanner_->UpdateState(token_type);
  if (inter_leaf_scanner_->ShouldStartNewPartition()) {
    VLOG(4) << "new partition";
    // interleaf-tokens like comments do not have corresponding syntax tree
    // nodes, so pass nullptr.
    StartNewUnwrappedLine(PartitionPolicyEnum::kFitOnLineElseExpand, nullptr);
  }
  VLOG(4) << "end of " << __FUNCTION__;
}

void TreeUnwrapper::AdvanceLastVisitedLeaf() {
  VLOG(4) << __FUNCTION__;
  EatSpaces();
  const auto &next_token = *NextUnfilteredToken();
  const verilog_tokentype token_enum =
      verilog_tokentype(next_token.token_enum());
  UpdateInterLeafScanner(token_enum);
  AdvanceNextUnfilteredToken();
  VLOG(4) << "end of " << __FUNCTION__;
}

verible::TokenWithContext TreeUnwrapper::VerboseToken(
    const TokenInfo &token) const {
  return TokenWithContext{token, token_context_};
}

static void VerilogOriginPrinter(
    std::ostream &stream, const verible::TokenInfo::Context &token_context,
    const verible::Symbol *symbol) {
  CHECK_NOTNULL(symbol);

  if (symbol->Kind() == verible::SymbolKind::kNode) {
    stream << svfmt::NodeTagName(symbol->Tag().tag);
  } else {
    stream << "#" << svfmt::TokenEnumName(verilog_tokentype(symbol->Tag().tag));
  }

  const auto *left = verible::GetLeftmostLeaf(*symbol);
  const auto *right = verible::GetRightmostLeaf(*symbol);
  if (left && right) {
    const int start = left->get().left(token_context.base);
    const int end = right->get().right(token_context.base);
    stream << "@" << start << "-" << end;
  }
  stream << " ";
  verible::UnwrappedLine::DefaultOriginPrinter(stream, symbol);
}

verible::TokenPartitionTreePrinter TreeUnwrapper::VerilogPartitionPrinter(
    const verible::TokenPartitionTree &partition) const {
  return verible::TokenPartitionTreePrinter(
      partition, false,
      [this](std::ostream &stream, const verible::Symbol *symbol) {
        VerilogOriginPrinter(stream, this->token_context_, symbol);
      });
}

void TreeUnwrapper::CatchUpToCurrentLeaf(const TokenInfo &leaf_token) {
  VLOG(4) << __FUNCTION__ << " to " << VerboseToken(leaf_token);
  // "Catch up" NextUnfilteredToken() to the current leaf.
  // Assigns non-whitespace tokens such as comments into UnwrappedLines.
  // Recall that SyntaxTreeLeaf has its own copy of TokenInfo, so we need to
  // compare a unique property of TokenInfo instead of its address.
  while (!NextUnfilteredToken()->isEOF()) {
    EatSpaces();
    // compare const char* addresses:
    if (NextUnfilteredToken()->text().begin() != leaf_token.text().begin()) {
      VLOG(4) << "token: " << VerboseToken(*NextUnfilteredToken());
      AdvanceLastVisitedLeaf();
    } else {
      break;
    }
  }
  VLOG(4) << "end of " << __FUNCTION__;
}

// Scan forward from the current leaf token, until some implementation-defined
// stopping condition.  This collects comments up until the first newline,
// and appends them to the most recent partition.
void TreeUnwrapper::LookAheadBeyondCurrentLeaf() {
  VLOG(4) << __FUNCTION__;
  // Re-initialize internal token-scanning state machine.
  inter_leaf_scanner_->Reset();

  // Consume trailing comments after the last leaf token.
  // Ignore spaces but stop at newline.
  // Advance until hitting a newline or catching up to the current leaf node,
  // whichever comes first.
  //
  // TODO(fangism): This might not adequately handle a block of //-style
  // comments that span multiple lines.  e.g.
  //
  //   ... some token;   // start of long comment ...
  //                     // continuation of long comment
  //   next tokens...
  //
  while (!NextUnfilteredToken()->isEOF()) {
    EatSpaces();
    VLOG(4) << "lookahead token: " << VerboseToken(*NextUnfilteredToken());
    if (auto next_tok = verilog_tokentype(NextUnfilteredToken()->token_enum());
        IsComment(next_tok) || next_tok == verilog_tokentype::TK_LINE_CONT) {
      // TODO(fangism): or IsAttribute().  Basically, any token that is not
      // in the syntax tree and not a space.
      AdvanceLastVisitedLeaf();
    } else {  // including newline
      // Don't advance NextUnfilteredToken() in this case.
      VLOG(4) << "no advance";
      break;
    }
  }
  VLOG(4) << "end of " << __FUNCTION__;
}

// Scan forward up to a syntax tree leaf token, but return (possibly) the
// position of the last newline *before* that leaf token.
// This allows prefix comments and attributes to stick with their
// intended token that immediately follows.
static verible::TokenSequence::const_iterator StopAtLastNewlineBeforeTreeLeaf(
    const verible::TokenSequence::const_iterator token_begin,
    const TokenInfo::Context &context) {
  VLOG(4) << __FUNCTION__;
  auto token_iter = token_begin;
  auto last_newline = token_begin;
  bool have_last_newline = false;

  // Find next syntax tree token or EOF.
  bool break_while = false;
  while (!token_iter->isEOF() && !break_while) {
    VLOG(4) << "scan: " << TokenWithContext{*token_iter, context};
    switch (token_iter->token_enum()) {
      // TODO(b/144653479): this token-case logic is redundant with other
      // places; plumb that through to here instead of replicating it.
      case TK_NEWLINE:
        have_last_newline = true;
        last_newline = token_iter;
        ABSL_FALLTHROUGH_INTENDED;
      case TK_SPACE:
      case TK_EOL_COMMENT:
      case TK_COMMENT_BLOCK:
      case TK_ATTRIBUTE:
        ++token_iter;
        break;
      default:
        break_while = true;
        break;
    }
  }

  const auto result = have_last_newline ? last_newline : token_iter;
  VLOG(4) << "end of " << __FUNCTION__ << ", advanced "
          << std::distance(token_begin, result) << " tokens";
  return result;
}

// Scan forward for comments between leaf tokens, and append them to a partition
// with the correct amount of indentation.
void TreeUnwrapper::LookAheadBeyondCurrentNode() {
  VLOG(4) << __FUNCTION__;
  // Scan until token is reached, or the last newline before token is reached.
  const auto token_begin = NextUnfilteredToken();
  const auto token_end =
      StopAtLastNewlineBeforeTreeLeaf(token_begin, token_context_);
  VLOG(4) << "stop before: " << VerboseToken(*token_end);
  while (NextUnfilteredToken() != token_end) {
    // Almost like AdvanceLastVisitedLeaf(), except suppress the last
    // advancement.
    EatSpaces();
    const auto next_token_iter = NextUnfilteredToken();
    const auto &next_token = *next_token_iter;
    VLOG(4) << "token: " << VerboseToken(next_token);
    const verilog_tokentype token_enum =
        verilog_tokentype(next_token.token_enum());
    UpdateInterLeafScanner(token_enum);
    if (next_token_iter != token_end) {
      AdvanceNextUnfilteredToken();
    } else {
      break;
    }
  }
  VLOG(4) << "end of " << __FUNCTION__;
}

// This hook is called between the children nodes of the handled node types.
// This is what allows partitions containing only comments to be properly
// indented to the same level that non-comment sub-partitions would.
void TreeUnwrapper::InterChildNodeHook(const SyntaxTreeNode &node) {
  const int tag = node.Tag().tag;
  VLOG(4) << __FUNCTION__ << " node type: " << svfmt::NodeTagName(tag);
  switch (tag) {
    case static_cast<int>(ListKind::ParameterPortList_declarations):
    case static_cast<int>(ListKind::AnsiPortList_ports):
    case static_cast<int>(ListKind::NonAnsiPortList_ports):
    case static_cast<int>(ListKind::ParameterValueAssignment_parameters):
    case static_cast<int>(ListKind::HierarchicalInstance_connections):
    case static_cast<int>(ListKind::ModuleDeclaration_members):
    case static_cast<int>(ListKind::GenerateBlock_members):
    case static_cast<int>(ListKind::GenerateRegion_members):
    case static_cast<int>(ListKind::ClassDeclaration_items):
    case static_cast<int>(ListKind::EnumType_members):
    case static_cast<int>(ListKind::CompilationUnit_members):
    case static_cast<int>(ListKind::BlockStatement_items):
    case static_cast<int>(ListKind::FunctionDeclaration_items):
    case static_cast<int>(ListKind::CaseStatement_items):
    case static_cast<int>(ListKind::CaseGenerate_items):
    case static_cast<int>(ListKind::ConstraintBlock_items):
    case static_cast<int>(ListKind::DistConstraintList_items):
    case static_cast<int>(ListKind::StructUnionType_members):
    case static_cast<int>(ListKind::CovergroupDeclaration_members):
    case static_cast<int>(ListKind::SpecifyBlock_items):
    case static_cast<int>(ListKind::ClockingDeclaration_items):
    case static_cast<int>(ListKind::UdpBody_entries):
      LookAheadBeyondCurrentNode();
      break;
    case static_cast<int>(ListKind::ArgumentList_parameters):
    case static_cast<int>(GroupKind::kMacroArgList):
      // Put each argument, separator, and comment in a new unwrapped line
      StartNewUnwrappedLine(PartitionPolicyEnum::kFitOnLineElseExpand, &node);
      // Catch comments after last argument
      LookAheadBeyondCurrentNode();
      break;
    default:
      break;
  }
}

void TreeUnwrapper::CollectLeadingFilteredTokens() {
  VLOG(4) << __FUNCTION__;
  // inter_leaf_scanner_ is already initialized to kStart state
  LookAheadBeyondCurrentNode();
  VLOG(4) << "end of " << __FUNCTION__;
}

void TreeUnwrapper::CollectTrailingFilteredTokens() {
  VLOG(4) << __FUNCTION__;
  // This should emulate ::Visit(leaf == EOF token).

  // A newline means there are no comments to add to this UnwrappedLine
  // TODO(fangism): fold this logic into CatchUpToCurrentLeaf()
  if (IsNewlineOrEOF(verilog_tokentype(NextUnfilteredToken()->token_enum()))) {
    // Filtered tokens may include comments, which do not correspond to syntax
    // tree nodes, so pass nullptr.
    StartNewUnwrappedLine(PartitionPolicyEnum::kFitOnLineElseExpand, nullptr);
  }

  // "Catch up" to EOF.
  // The very last unfiltered token scanned should be the EOF.
  // The byte offset of the EOF token is used as the stop-condition.
  CatchUpToCurrentLeaf(EOFToken());
  VLOG(4) << "end of " << __FUNCTION__;
}

enum class ContextHint {
  kInsideStandaloneMacroCall,
};

static const verible::EnumNameMap<ContextHint> &ContextHintStrings() {
  static const verible::EnumNameMap<ContextHint> kContextHintStringMap({
      {"kInsideStandaloneMacroCall", ContextHint::kInsideStandaloneMacroCall},
  });
  return kContextHintStringMap;
}

static std::ostream &operator<<(std::ostream &stream, ContextHint p) {
  return ContextHintStrings().Unparse(p, stream);
}

static std::ostream &operator<<(std::ostream &stream,
                                const std::vector<ContextHint> &f) {
  return stream << verible::SequenceFormatter(f);
}

// Visitor to determine which node enum function to call
void TreeUnwrapper::Visit(const SyntaxTreeNode &node) {
  VLOG(3) << __FUNCTION__ << " node: " << svfmt::NodeTagName(node.Tag().tag);

  const auto context_hints_size = context_hints_.size();

  // This phase is only concerned with creating token partitions (during tree
  // recursive descent) and setting correct indentation values.  It is ok to
  // have excessive partitioning during this phase.
  SetIndentationsAndCreatePartitions(node);

  // This phase is only concerned with reshaping operations on token partitions,
  // such as merging, flattening, hoisting.  Reshaping should only occur on the
  // return path of tree traversal (here).

  auto *partition = PreviousSibling(*CurrentTokenPartition());
  if (partition != nullptr) {
    ReshapeTokenPartitions(node, style_, partition);
  }

  CHECK_GE(context_hints_.size(), context_hints_size);
  // Remove hints from traversed context
  context_hints_.erase(context_hints_.begin() + context_hints_size,
                       context_hints_.end());
}

// CST-descending phase that creates partitions with correct indentation.
// TODO(mglb): This function is > 600 lines and should probably be split
//             up into multiple.
// svfmt: true if an empty list node encloses no comments (then Verible's CST
// would have no list node, so no partition is created for it).
bool TreeUnwrapper::IsEmptyListWithoutComments(const SyntaxTreeNode &node) const {
  if (!node.empty()) return false;
  for (auto it = NextUnfilteredToken(); !it->isEOF(); ++it) {
    const int e = it->token_enum();
    if (e == TK_SPACE || e == TK_NEWLINE) continue;
    return !IsComment(verilog_tokentype(e));
  }
  return true;
}

void TreeUnwrapper::SetIndentationsAndCreatePartitions(
    const SyntaxTreeNode &node) {
  const int tag = node.Tag().tag;
  const SyntaxKind kind = Kind(node);
  VLOG(3) << __FUNCTION__ << " node: " << svfmt::NodeTagName(tag);

  // svfmt: this is Verible's SetIndentationsAndCreatePartitions() with each
  // NodeEnum case mapped to the corresponding slang SyntaxKind, synthetic
  // list node (ListKind) or synthetic group node (GroupKind).

  // Suppress additional indentation when:
  // - at the syntax tree root
  // - at the first level of macro argument expansion
  const bool suppress_indentation =
      Context().empty() ||
      DirectParentTag(Context()) == static_cast<int>(GroupKind::kMacroArgList);

  // ---- List nodes (Verible's *List nonterminals) ----
  if (svfmt::IsListTag(tag)) {
    if (IsEmptyListWithoutComments(node) &&
        tag != Tag(ListKind::CompilationUnit_members) &&
        tag != Tag(ListKind::SpecifyBlock_items)) {
      return;  // absent in Verible's CST; nothing to visit
    }
    switch (static_cast<ListKind>(tag)) {
      case ListKind::CompilationUnit_members:
        // kDescriptionList: the base class already created this partition.
        VisitNewUnwrappedLine(node);
        return;

      // kBlockItemStatementList, kStatementList, kFunctionItemList,
      // kCaseItemList, kGenerateCaseItemList, kClassItems, kModuleItemList,
      // kGenerateItemList, kPackageItemList, kDistributionItemList,
      // kEnumNameList, kStructUnionMemberList
      case ListKind::BlockStatement_items:
      case ListKind::FunctionDeclaration_items:
      case ListKind::CaseStatement_items:
        // kCasePatternItemList ("case ... matches") is not aligned.
        if (std::any_of(node.children().begin(), node.children().end(),
                        [](const verible::SymbolPtr &p) {
                          return p && Kind(*p) == SyntaxKind::PatternCaseItem;
                        })) {
          VisitIndentedSection(node, suppress_indentation ? 0 : style_.indentation_spaces,
                               PartitionPolicyEnum::kAlwaysExpand);
          return;
        }
        [[fallthrough]];
      case ListKind::RandCaseStatement_items:
      case ListKind::CaseGenerate_items:
      case ListKind::ClassDeclaration_items:
      case ListKind::ModuleDeclaration_members:
      case ListKind::GenerateBlock_members:
      case ListKind::GenerateRegion_members:
      case ListKind::DistConstraintList_items:
      case ListKind::EnumType_members:
      case ListKind::StructUnionType_members: {
        const int indent = suppress_indentation ? 0 : style_.indentation_spaces;
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kTabularAlignment);
        return;
      }

      case ListKind::UdpBody_portDecls:
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kAlwaysExpand);
        return;
      // kOpenRangeList of a concatenation that is a whole expression
      case ListKind::ConcatenationExpression_expressions: {
        if (IsTopLevelConcatenation(Context())) {
          const int indent = suppress_indentation ? 0 : style_.indentation_spaces;
          VisitIndentedSection(node, indent, PartitionPolicyEnum::kFitOnLineElseExpand);
          return;
        }
        break;
      }
      // kPackageImportList in a module header
      case ListKind::ModuleHeader_imports: {
        if (node.empty()) break;
        const int indent = suppress_indentation ? 0 : style_.indentation_spaces;
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kAlwaysExpand);
        return;
      }
      case ListKind::UdpBody_entries:
        VisitIndentedSection(node, suppress_indentation ? 0 : style_.indentation_spaces,
                             PartitionPolicyEnum::kAlwaysExpand);
        return;

      // kConstraintBlockItemList, kCoverageSpecOptionList, ...
      case ListKind::ConstraintBlock_items:
      case ListKind::CovergroupDeclaration_members:
      case ListKind::ClockingDeclaration_items:
      case ListKind::SpecifyBlock_items: {
        const int indent = suppress_indentation ? 0 : style_.indentation_spaces;
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kAlwaysExpand);
        return;
      }

      // kActualParameterByNameList (only for named parameter assignments)
      case ListKind::ParameterValueAssignment_parameters: {
        const bool named = std::any_of(
            node.children().begin(), node.children().end(),
            [](const verible::SymbolPtr &p) {
              return p && Kind(*p) == SyntaxKind::NamedParamAssignment;
            });
        if (!named) break;
        const int indent =
            suppress_indentation ? 0 : style_.NamedParameterIndentation();
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kTabularAlignment);
        return;
      }

      // kPortActualList (named and positional ports of an instance)
      case ListKind::HierarchicalInstance_connections: {
        const int indent = suppress_indentation ? 0 : style_.NamedPortIndentation();
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kTabularAlignment);
        return;
      }

      // kPortDeclarationList (non-ANSI module ports are also a
      // kPortDeclarationList in Verible's CST)
      case ListKind::NonAnsiPortList_ports:
        if (!svfmt::IsInsideAnyOf(Context(), {SyntaxKind::ModuleHeader,
                                              SyntaxKind::InterfaceHeader,
                                              SyntaxKind::ProgramHeader})) {
          break;
        }
        [[fallthrough]];
      case ListKind::AnsiPortList_ports: {
        const int indent =
            suppress_indentation ? 0 : style_.PortDeclarationsIndentation();
        if (svfmt::IsInsideAnyOf(Context(), {SyntaxKind::ModuleHeader,
                                             SyntaxKind::InterfaceHeader,
                                             SyntaxKind::ProgramHeader})) {
          VisitIndentedSection(node, indent, PartitionPolicyEnum::kTabularAlignment);
        } else {
          VisitIndentedSection(node, indent, PartitionPolicyEnum::kFitOnLineElseExpand);
        }
        return;
      }

      // kFormalParameterList
      case ListKind::ParameterPortList_declarations: {
        const int indent =
            suppress_indentation ? 0 : style_.FormalParametersIndentation();
        if (svfmt::IsInsideAnyOf(Context(), {SyntaxKind::ModuleHeader,
                                             SyntaxKind::InterfaceHeader,
                                             SyntaxKind::ProgramHeader}) ||
            svfmt::IsInside(Context(), G(GroupKind::kClassHeader))) {
          VisitIndentedSection(node, indent, PartitionPolicyEnum::kTabularAlignment);
        } else {
          VisitIndentedSection(node, indent, PartitionPolicyEnum::kFitOnLineElseExpand);
        }
        return;
      }

      // kPortList (task/function ports)
      case ListKind::FunctionPortList_ports: {
        if (svfmt::IsInsideAnyOf(Context(),
                                 {SyntaxKind::DPIImport, SyntaxKind::DPIExport})) {
          const int indent = suppress_indentation ? 0 : style_.indentation_spaces;
          VisitIndentedSection(node, indent, PartitionPolicyEnum::kTabularAlignment);
        } else {
          const int indent = suppress_indentation ? 0 : style_.wrap_spaces;
          VisitIndentedSection(node, indent, PartitionPolicyEnum::kFitOnLineElseExpand);
        }
        return;
      }

      // kGateInstanceRegisterVariableList, kVariableDeclarationAssignmentList,
      // kMacroFormalParameterList, modport port lists
      case ListKind::DataDeclaration_declarators:
      case ListKind::HierarchyInstantiation_instances:
      case ListKind::PrimitiveInstantiation_instances:
      case ListKind::MacroFormalArgumentList_args: {
        const int indent = suppress_indentation ? 0 : style_.wrap_spaces;
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kFitOnLineElseExpand);
        return;
      }

      // kArgumentList of standalone system task calls
      case ListKind::ArgumentList_parameters: {
        if (HasContextHint(ContextHint::kInsideStandaloneMacroCall) &&
            AncestorKind(Context()) == SyntaxKind::ArgumentList &&
            AncestorKind(Context(), 1) == SyntaxKind::InvocationExpression) {
          VisitIndentedSection(node, style_.wrap_spaces, PartitionPolicyEnum::kWrap);
          return;
        }
        if (AncestorKind(Context()) == SyntaxKind::ArgumentList &&
            AncestorKind(Context(), 1) == SyntaxKind::InvocationExpression) {
          VisitIndentedSection(node, style_.wrap_spaces,
                               PartitionPolicyEnum::kFitOnLineElseExpand);
          return;
        }
        break;
      }

      // kExpressionList inside an assignment pattern
      case ListKind::SimpleAssignmentPattern_items:
      case ListKind::StructuredAssignmentPattern_items: {
        VisitIndentedSection(node, style_.wrap_spaces,
                             PartitionPolicyEnum::kFitOnLineElseExpand);
        return;
      }

      default:
        break;
    }
    TraverseChildren(node);
    return;
  }

  // ---- Synthetic group nodes ----
  if (svfmt::IsGroupTag(tag)) {
    switch (static_cast<GroupKind>(tag)) {
      case GroupKind::kCovergroupHeader:
      case GroupKind::kIfClause:
      case GroupKind::kIfHeader:
      case GroupKind::kLoopHeader:
      case GroupKind::kInstantiationType:
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
        return;
      case GroupKind::kBegin:
      case GroupKind::kEnd:
      case GroupKind::kClassHeader:
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kAlwaysExpand);
        return;
      // Verible's kAssertionVariableDeclarationList and kPropertySpec /
      // kSequenceDeclarationFinalExpr: one level in, one item per line.
      case GroupKind::kAssertionDeclBody:
        VisitIndentedSection(node, style_.indentation_spaces, PartitionPolicyEnum::kAlwaysExpand);
        return;
      case GroupKind::kSubroutineHeader:
        if (svfmt::IsInsideAnyOf(Context(),
                                 {SyntaxKind::DPIImport, SyntaxKind::DPIExport})) {
          VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
        } else {
          VisitIndentedSection(node, 0, PartitionPolicyEnum::kAppendFittingSubPartitions);
        }
        return;
      case GroupKind::kIfBody: {
        // In the case of if-begin, let the 'begin'-'end' block indent its own
        // section.  Othewise for single statements/items, indent here.
        const auto *subnode = GetAnyControlStatementBody(node);
        const auto next_indent = (subnode != nullptr && NodeIsBeginEndBlock(*subnode))
                                     ? 0
                                     : style_.indentation_spaces;
        VisitIndentedSection(node, next_indent, PartitionPolicyEnum::kFitOnLineElseExpand);
        return;
      }
      case GroupKind::kElseBody: {
        // In the case of else-begin, let the 'begin'-'end' block indent its
        // own section, otherwise, indent single statements here.
        // In the case of else-if, suppress further indentation, deferring to
        // the conditional construct.
        const auto *subnode = GetAnyControlStatementBody(node);
        const auto next_indent = subnode && NodeIsConditionalOrBlock(*subnode)
                                     ? 0
                                     : style_.indentation_spaces;
        VisitIndentedSection(node, next_indent, PartitionPolicyEnum::kFitOnLineElseExpand);
        return;
      }
      case GroupKind::kParenGroup: {
        // ( ... ) of a macro call.
        if (HasContextHint(ContextHint::kInsideStandaloneMacroCall)) {
          const bool is_nested_call = IsInsideFirstIf(
              Context(),
              [](SyntaxKind k) {
                return k == SyntaxKind::ArgumentList || k == G(GroupKind::kMacroArgList) ||
                       k == G(GroupKind::kParenGroup);
              },
              [](SyntaxKind k) { return IsStatementKind(k); });
          VisitIndentedSection(node, is_nested_call ? 0 : style_.wrap_spaces,
                               PartitionPolicyEnum::kWrap);
        } else {
          TraverseChildren(node);
        }
        return;
      }
      case GroupKind::kMacroArgList: {
        if (HasContextHint(ContextHint::kInsideStandaloneMacroCall) &&
            AncestorKind(Context()) == G(GroupKind::kParenGroup) &&
            AncestorKind(Context(), 1) == SyntaxKind::MacroUsage) {
          VisitIndentedSection(node, style_.wrap_spaces, PartitionPolicyEnum::kWrap);
          return;
        }
        const int indent = suppress_indentation ? 0 : style_.wrap_spaces;
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kFitOnLineElseExpand);
        return;
      }
      default:
        TraverseChildren(node);
        return;
    }
  }

  // ---- slang syntax nodes ----
  switch (kind) {
    // The following constructs are flushed-left, not indented:
    case SyntaxKind::IfDefDirective:
    case SyntaxKind::IfNDefDirective:
    case SyntaxKind::ElsIfDirective:
    case SyntaxKind::ElseDirective:
    case SyntaxKind::EndIfDirective:
    case SyntaxKind::TimeScaleDirective:
    case SyntaxKind::DefaultNetTypeDirective:
    case SyntaxKind::ResetAllDirective:
    case SyntaxKind::CellDefineDirective:
    case SyntaxKind::EndCellDefineDirective:
    case SyntaxKind::BeginKeywordsDirective:
    case SyntaxKind::EndKeywordsDirective:
    case SyntaxKind::PragmaDirective:
    case SyntaxKind::LineDirective:
    case SyntaxKind::UnconnectedDriveDirective:
    case SyntaxKind::NoUnconnectedDriveDirective:
    case SyntaxKind::DefaultDecayTimeDirective:
    case SyntaxKind::DefaultTriregStrengthDirective:
    case SyntaxKind::DelayModeDistributedDirective:
    case SyntaxKind::DelayModePathDirective:
    case SyntaxKind::DelayModeUnitDirective:
    case SyntaxKind::DelayModeZeroDirective:
    case SyntaxKind::UndefineAllDirective: {
      VisitNewUnindentedUnwrappedLine(node);
      break;
    }

    // The following constructs start a new UnwrappedLine indented to the
    // current level.
    case SyntaxKind::GenerateRegion:
    case SyntaxKind::CaseGenerate:
    case SyntaxKind::LoopGenerate:
    case SyntaxKind::PackageImportDeclaration:
    case SyntaxKind::IncludeDirective:
    case SyntaxKind::UndefDirective:
    case SyntaxKind::TypedefDeclaration:
    case SyntaxKind::ForwardTypedefDeclaration:
    case SyntaxKind::LetDeclaration:
    case SyntaxKind::NetTypeDeclaration:
    case SyntaxKind::ConstraintDeclaration:
    case SyntaxKind::ExpressionConstraint:
    case SyntaxKind::ImplicationConstraint:
    case SyntaxKind::ConditionalConstraint:
    case SyntaxKind::UniquenessConstraint:
    case SyntaxKind::SolveBeforeConstraint:
    case SyntaxKind::DisableConstraint:
    case SyntaxKind::LoopConstraint:
    case SyntaxKind::CoverageOption:
    case SyntaxKind::CoverageBins:
    case SyntaxKind::CoverCross:
    case SyntaxKind::BinsSelection:
    case SyntaxKind::DistItem:
    case SyntaxKind::StructUnionMember:
    case SyntaxKind::NetDeclaration:
    case SyntaxKind::PortDeclaration:
    case SyntaxKind::AlwaysBlock:
    case SyntaxKind::AlwaysCombBlock:
    case SyntaxKind::AlwaysFFBlock:
    case SyntaxKind::AlwaysLatchBlock:
    case SyntaxKind::InitialBlock:
    case SyntaxKind::FinalBlock:
    case SyntaxKind::NamedParamAssignment:
    case SyntaxKind::NamedPortConnection:
    case SyntaxKind::OrderedPortConnection:
    case SyntaxKind::WildcardPortConnection:
    case SyntaxKind::MacroFormalArgument:
    case SyntaxKind::PropertyDeclaration:
    case SyntaxKind::SequenceDeclaration:
    case SyntaxKind::ImplicitAnsiPort:
    case SyntaxKind::ExplicitAnsiPort:
    case SyntaxKind::ImplicitNonAnsiPort:
    case SyntaxKind::ExplicitNonAnsiPort:
    case SyntaxKind::ParameterDeclarationStatement:
    case SyntaxKind::ClockingDeclaration:
    case SyntaxKind::GenvarDeclaration:
    case SyntaxKind::IfGenerate:
    case SyntaxKind::ModuleHeader:
    case SyntaxKind::InterfaceHeader:
    case SyntaxKind::ProgramHeader:
    case SyntaxKind::PackageHeader:
    case SyntaxKind::BindDirective:
    case SyntaxKind::DataDeclaration:
    case SyntaxKind::HierarchyInstantiation:
    case SyntaxKind::PrimitiveInstantiation:
    case SyntaxKind::ModportDeclaration:
    case SyntaxKind::StandardCaseItem:
    case SyntaxKind::DefaultCaseItem:
    case SyntaxKind::PatternCaseItem:
    case SyntaxKind::RandCaseItem:
    case SyntaxKind::HierarchicalInstance:
    case SyntaxKind::ElseClause:
    case SyntaxKind::ClassPropertyDeclaration:
    case SyntaxKind::ClassMethodPrototype:
    case SyntaxKind::ContinuousAssign: {
      VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
      break;
    }

    // kParamDeclaration, kRegisterVariable, kEnumName: only as list elements
    // (parameter port list, declarator lists, enum members).
    case SyntaxKind::ParameterDeclaration:
    case SyntaxKind::TypeParameterDeclaration: {
      if (AncestorKind(Context()) == SyntaxKind::ParameterPortList) {
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
      } else {
        TraverseChildren(node);
      }
      break;
    }
    case SyntaxKind::Declarator: {
      const int parent = DirectParentTag(Context());
      if (parent == Tag(ListKind::DataDeclaration_declarators) ||
          parent == Tag(ListKind::EnumType_members)) {
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
      } else {
        TraverseChildren(node);
      }
      break;
    }

    // The following cases will always expand into their constituent
    // partitions:
    case SyntaxKind::CovergroupDeclaration:
    case SyntaxKind::ModuleDeclaration:
    case SyntaxKind::ProgramDeclaration:
    case SyntaxKind::PackageDeclaration:
    case SyntaxKind::InterfaceDeclaration:
    case SyntaxKind::FunctionDeclaration:
    case SyntaxKind::TaskDeclaration:
    case SyntaxKind::ClassDeclaration:
    case SyntaxKind::ParallelBlockStatement:
    case SyntaxKind::SequentialBlockStatement:
    case SyntaxKind::GenerateBlock:
    case SyntaxKind::DPIImport:
    case SyntaxKind::DPIExport: {
      VisitIndentedSection(node, 0, PartitionPolicyEnum::kAlwaysExpand);
      break;
    }

    // For the following items, start a new unwrapped line only if they are
    // *direct* descendants of list elements.  This effectively suppresses
    // starting a new line when single statements are found to extend other
    // statements, delayed assignments, single-statement if/for loops.
    // search-anchor: STATEMENT_TYPES
    case SyntaxKind::EmptyStatement:
    case SyntaxKind::ExpressionStatement:
    case SyntaxKind::ProceduralAssignStatement:
    case SyntaxKind::ProceduralForceStatement:
    case SyntaxKind::ProceduralDeassignStatement:
    case SyntaxKind::ProceduralReleaseStatement:
    case SyntaxKind::JumpStatement:
    case SyntaxKind::ReturnStatement:
    case SyntaxKind::DisableStatement:
    case SyntaxKind::DisableForkStatement:
    case SyntaxKind::WaitStatement:
    case SyntaxKind::WaitForkStatement:
    case SyntaxKind::WaitOrderStatement:
    case SyntaxKind::ImmediateAssertStatement:
    case SyntaxKind::ImmediateAssumeStatement:
    case SyntaxKind::ImmediateCoverStatement:
    case SyntaxKind::AssertPropertyStatement:
    case SyntaxKind::AssumePropertyStatement:
    case SyntaxKind::CoverPropertyStatement:
    case SyntaxKind::CoverSequenceStatement:
    case SyntaxKind::ExpectPropertyStatement:
    case SyntaxKind::RestrictPropertyStatement:
    case SyntaxKind::BlockingEventTriggerStatement:
    case SyntaxKind::NonblockingEventTriggerStatement:
    case SyntaxKind::TimingControlStatement:
    case SyntaxKind::CaseStatement:
    case SyntaxKind::RandCaseStatement:
    case SyntaxKind::ForLoopStatement:
    case SyntaxKind::ForeverStatement:
    case SyntaxKind::LoopStatement:
    case SyntaxKind::DoWhileStatement:
    case SyntaxKind::ForeachLoopStatement:
    case SyntaxKind::ConditionalStatement: {
      // Single statements directly inside a flow-control construct
      // should be properly indented one level.
      // Verible: statements anywhere inside a begin-end block's item list
      // (kBlockItemStatementList) get no extra indentation.
      const bool in_block_list = IsInsideFirstIf(
          Context(),
          [](SyntaxKind k) { return static_cast<int>(k) == Tag(ListKind::BlockStatement_items); },
          [](SyntaxKind) { return false; });
      const int indent = in_block_list ? 0
                         : ShouldIndentRelativeToDirectParent(Context())
                             ? style_.indentation_spaces
                             : 0;
      VisitIndentedSection(node, indent, PartitionPolicyEnum::kFitOnLineElseExpand);
      break;
    }

    // UDPs: kUdpPrimitive, kUdpBody, port declarations, table entries.
    case SyntaxKind::UdpDeclaration:
    case SyntaxKind::SpecifyBlock:
      VisitIndentedSection(node, 0, PartitionPolicyEnum::kAlwaysExpand);
      break;
    // kSpecifyItem
    case SyntaxKind::SystemTimingCheck:
    case SyntaxKind::PathDeclaration:
    case SyntaxKind::ConditionalPathDeclaration:
    case SyntaxKind::IfNonePathDeclaration:
    case SyntaxKind::PulseStyleDeclaration:
    case SyntaxKind::SpecparamDeclaration:
      if (svfmt::IsInside(Context(), SyntaxKind::SpecifyBlock) &&
          DirectParentTag(Context()) == Tag(ListKind::SpecifyBlock_items)) {
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
      } else {
        TraverseChildren(node);
      }
      break;
    // kModportSimplePortsDeclaration, kModportTFPortsDeclaration
    case SyntaxKind::ModportSimplePortList:
    case SyntaxKind::ModportSubroutinePortList:
    case SyntaxKind::ModportClockingPort:
      VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
      break;
    case SyntaxKind::UdpBody:
      VisitIndentedSection(node, suppress_indentation ? 0 : style_.indentation_spaces,
                           PartitionPolicyEnum::kAlwaysExpand);
      break;
    case SyntaxKind::UdpOutputPortDecl:
    case SyntaxKind::UdpInputPortDecl:
    case SyntaxKind::UdpInitialStmt:
    case SyntaxKind::UdpEntry:
      VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
      break;

    // kExpression elements of a top-level concatenation are their own lines.
    default:
      if (DirectParentTag(Context()) == Tag(ListKind::ConcatenationExpression_expressions) &&
          IsTopLevelConcatenation(Context())) {
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
        break;
      }
      SetIndentationsForOtherKinds(node, kind, suppress_indentation);
      break;
  }
}

void TreeUnwrapper::SetIndentationsForOtherKinds(const SyntaxTreeNode &node,
                                                  SyntaxKind kind,
                                                  bool suppress_indentation) {
  switch (kind) {
    // kNetVariableAssignment inside a continuous assignment
    case SyntaxKind::AssignmentExpression: {
      if (DirectParentTag(Context()) == Tag(ListKind::ContinuousAssign_assignments)) {
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
      } else {
        TraverseChildren(node);
      }
      break;
    }

    // kFunctionCall / kSystemTFCall
    case SyntaxKind::InvocationExpression: {
      const bool is_statement = AncestorKind(Context()) == SyntaxKind::ExpressionStatement;
      // A task call module item (only from a reshaped module-body excerpt).
      const bool is_module_item =
          DirectParentTag(Context()) == Tag(ListKind::ModuleDeclaration_members);
      if ((is_statement || is_module_item) && IsSystemCall(node)) {
        PushContextHint(ContextHint::kInsideStandaloneMacroCall);
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kStack);
      } else if (is_statement) {
        VisitIndentedSection(node, 0, PartitionPolicyEnum::kAppendFittingSubPartitions);
      } else {
        TraverseChildren(node);
      }
      break;
    }

    case SyntaxKind::MacroUsage: {
      // Single statements directly inside a controlled construct
      // should be properly indented one level.
      const int indent =
          ShouldIndentRelativeToDirectParent(Context()) ? style_.indentation_spaces : 0;
      const int parent = DirectParentTag(Context());
      const bool is_nested_call = parent == static_cast<int>(GroupKind::kMacroArgList) ||
                                  parent == Tag(ListKind::ArgumentList_parameters);
      const bool is_standalone_call =
          (parent == Tag(ListKind::ModuleDeclaration_members) ||
           parent == Tag(ListKind::ClassDeclaration_items) ||
           parent == Tag(ListKind::CompilationUnit_members) ||
           parent == Tag(ListKind::GenerateBlock_members) ||
           IsBlockItemList(parent)) &&
          !svfmt::IsInside(Context(), SyntaxKind::VariableDimension);
      if (is_nested_call && HasContextHint(ContextHint::kInsideStandaloneMacroCall)) {
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kStack);
      } else if (is_standalone_call) {
        PushContextHint(ContextHint::kInsideStandaloneMacroCall);
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kStack);
      } else {
        VisitIndentedSection(node, indent, PartitionPolicyEnum::kAppendFittingSubPartitions);
      }
      break;
    }

    // The following constructs wish to use the partition policy of appending
    // trailing subpartitions greedily as long as they fit, wrapping as
    // needed.
    case SyntaxKind::DefineDirective: {
      VisitIndentedSection(node, 0, PartitionPolicyEnum::kAppendFittingSubPartitions);
      break;
    }

    case SyntaxKind::Coverpoint: {
      VisitIndentedSection(node, 0,
                           style_.expand_coverpoints
                               ? PartitionPolicyEnum::kAlwaysExpand
                               : PartitionPolicyEnum::kFitOnLineElseExpand);
      break;
    }

    case SyntaxKind::AssignmentPatternExpression: {
      VisitIndentedSection(node, 0, PartitionPolicyEnum::kFitOnLineElseExpand);
      break;
    }

    // kParenGroup of a system task / function call
    case SyntaxKind::ArgumentList: {
      if (HasContextHint(ContextHint::kInsideStandaloneMacroCall) &&
          AncestorKind(Context()) == SyntaxKind::InvocationExpression) {
        const bool is_nested_call = IsInsideFirstIf(
            Context(),
            [](SyntaxKind k) {
              return static_cast<int>(k) == Tag(ListKind::ArgumentList_parameters) ||
                     k == G(GroupKind::kMacroArgList);
            },
            [](SyntaxKind k) { return IsStatementKind(k); });
        VisitIndentedSection(node, is_nested_call ? 0 : style_.wrap_spaces,
                             PartitionPolicyEnum::kWrap);
      } else {
        TraverseChildren(node);
      }
      break;
    }

    default: {
      TraverseChildren(node);
    }
  }
}

static bool PartitionStartsWithSemicolon(const TokenPartitionTree &partition) {
  const auto &uwline = partition.Value();
  if (uwline.IsEmpty()) return false;
  const auto first_token = uwline.TokensRange().front().TokenEnum();
  return (first_token == ';' ||
          first_token ==
              verilog_tokentype::SemicolonEndOfAssertionVariableDeclarations);
}

static bool PartitionIsCloseParenSemi(const TokenPartitionTree &partition) {
  const auto ftokens = partition.Value().TokensRange();
  if (ftokens.size() < 2) return false;
  if (ftokens.front().TokenEnum() != ')') return false;
  return ftokens.back().TokenEnum() == ';';
}

static bool PartitionStartsWithCloseParen(const TokenPartitionTree &partition) {
  const auto ftokens = partition.Value().TokensRange();
  if (ftokens.empty()) return false;
  const auto token_enum = ftokens.front().TokenEnum();
  return ((token_enum == ')') || (token_enum == MacroCallCloseToEndLine));
}

static bool PartitionEndsWithOpenParen(const TokenPartitionTree &partition) {
  const auto ftokens = partition.Value().TokensRange();
  if (ftokens.empty()) return false;
  const auto token_enum = ftokens.back().TokenEnum();
  return token_enum == '(';
}

static bool PartitionIsCloseBrace(const TokenPartitionTree &partition) {
  const auto ftokens = partition.Value().TokensRange();
  if (ftokens.size() != 1) return false;
  const auto token_enum = ftokens.front().TokenEnum();
  return token_enum == '}';
}

static bool PartitionEndsWithOpenBrace(const TokenPartitionTree &partition) {
  const auto ftokens = partition.Value().TokensRange();
  if (ftokens.empty()) return false;
  const auto token_enum = ftokens.back().TokenEnum();
  return token_enum == '{';
}

static bool PartitionStartsWithCloseBrace(const TokenPartitionTree &partition) {
  const auto ftokens = partition.Value().TokensRange();
  if (ftokens.empty()) return false;
  const auto token_enum = ftokens.front().TokenEnum();
  return token_enum == '}';
}

// Returns true if the partition is forced to start in a new line.
static bool PartitionIsForcedIntoNewLine(const TokenPartitionTree &partition) {
  const auto policy = partition.Value().PartitionPolicy();
  if (policy == PartitionPolicyEnum::kAlreadyFormatted) return true;
  if (policy == PartitionPolicyEnum::kInline) return false;
  if (!is_leaf(partition)) {
    auto *first_leaf = &LeftmostDescendant(partition);
    const auto leaf_policy = first_leaf->Value().PartitionPolicy();
    if (leaf_policy == PartitionPolicyEnum::kAlreadyFormatted ||
        leaf_policy == PartitionPolicyEnum::kInline) {
      return true;
    }
  }

  const auto ftokens = partition.Value().TokensRange();
  if (ftokens.empty()) return false;
  return ftokens.front().before.break_decision == SpacingOptions::kMustWrap;
}

// Joins partition containing only a ',' (and optionally comments) with
// a partition preceding or following it.
static void AttachSeparatorToPreviousOrNextPartition(
    TokenPartitionTree *partition) {
  CHECK_NOTNULL(partition);
  VLOG(5) << __FUNCTION__ << ": subpartition:\n" << *partition;

  if (!is_leaf(*partition)) {
    VLOG(5) << "  skip: not a leaf.";
    return;
  }

  // Find a separator and make sure this is the only non-comment token
  const verible::PreFormatToken *separator = nullptr;
  for (const auto &token : partition->Value().TokensRange()) {
    switch (token.TokenEnum()) {
      case verilog_tokentype::TK_COMMENT_BLOCK:
      case verilog_tokentype::TK_EOL_COMMENT:
      case verilog_tokentype::TK_ATTRIBUTE:
        break;
      case ',':
      case ':':
        if (separator == nullptr) {
          separator = &token;
          break;
        }
        [[fallthrough]];
      default:
        VLOG(5) << "  skip: contains tokens other than separator and comments.";
        return;
    }
  }
  if (separator == nullptr) {
    VLOG(5) << "  skip: separator token not found.";
    return;
  }

  // Merge with previous partition if both partitions are in the same line in
  // original text
  const auto *previous_partition = PreviousLeaf(*partition);
  if (previous_partition != nullptr) {
    if (!previous_partition->Value().TokensRange().empty()) {
      const auto &previous_token =
          previous_partition->Value().TokensRange().back();
      std::string_view original_text_between = verible::make_string_view_range(
          previous_token.Text().end(), separator->Text().begin());
      if (!absl::StrContains(original_text_between, '\n')) {
        VLOG(5) << "  merge into previous partition.";
        verible::MergeLeafIntoPreviousLeaf(partition);
        return;
      }
    }
  }

  // Merge with next partition if both partitions are in the same line in
  // original text
  const auto *next_partition = NextLeaf(*partition);
  if (next_partition != nullptr) {
    if (!next_partition->Value().TokensRange().empty()) {
      const auto &next_token = next_partition->Value().TokensRange().front();
      std::string_view original_text_between = verible::make_string_view_range(
          separator->Text().end(), next_token.Text().begin());
      if (!absl::StrContains(original_text_between, '\n')) {
        VLOG(5) << "  merge into next partition.";
        verible::MergeLeafIntoNextLeaf(partition);
        return;
      }
    }
  }

  // If there are no comments (separator is the only token)...
  if (partition->Value().TokensRange().size() == 1) {
    // Try merging with previous partition
    if (!PartitionIsForcedIntoNewLine(*partition)) {
      VLOG(5) << "  merge into previous partition.";
      verible::MergeLeafIntoPreviousLeaf(partition);
      return;
    }

    // Try merging with next partition
    if (next_partition != nullptr &&
        !PartitionIsForcedIntoNewLine(*next_partition)) {
      VLOG(5) << "  merge into next partition.";
      verible::MergeLeafIntoNextLeaf(partition);
      return;
    }
  }

  // Leave the separator in its own line and clear its Origin() to not confuse
  // tabular alignment.
  VLOG(5) << "  keep in separate line, remove origin.";
  partition->Value().SetOrigin(nullptr);
}

static void AttachSeparatorsToListElementPartitions(
    TokenPartitionTree *partition) {
  CHECK_NOTNULL(partition);
  // Skip the first partition, it can't contain just a separator.
  for (int i = 1; i < static_cast<int>(partition->Children().size()); ++i) {
    auto &subpartition = partition->Children()[i];
    // This can change children count
    AttachSeparatorToPreviousOrNextPartition(&subpartition);
  }
}

// True when a leaf partition contains only a binary/infix operator (and
// optional comments/attributes). Used to normalize wrapping of expressions
// like `A + B + C` so operators always stay with the following operand.
static bool PartitionIsInfixOperatorOnly(const TokenPartitionTree &partition) {
  if (!is_leaf(partition)) return false;
  const auto tokens = partition.Value().TokensRange();
  if (tokens.empty()) return false;

  const verible::PreFormatToken *op = nullptr;
  for (const auto &token : tokens) {
    switch (token.TokenEnum()) {
      case verilog_tokentype::TK_COMMENT_BLOCK:
      case verilog_tokentype::TK_EOL_COMMENT:
      case verilog_tokentype::TK_ATTRIBUTE:
        break;
      default:
        if (GetFormatTokenType(static_cast<verilog_tokentype>(
                token.TokenEnum())) != FormatTokenType::binary_operator ||
            op != nullptr) {
          return false;
        }
        op = &token;
        break;
    }
  }
  return op != nullptr;
}

// Always attach infix-operator-only partitions to the following operand.
// Attachment based on original newlines is unstable for macro sums:
//   `A\n+\n`B  vs  `A\n+ `B  produce different partition shapes and oscillate
// under re-format (GitHub issue 2547).
static void AttachInfixOperatorsToFollowingOperands(
    TokenPartitionTree *partition) {
  // Iterate by index; merges invalidate sibling pointers.
  for (int i = 0; i < static_cast<int>(partition->Children().size()); ++i) {
    auto &child = partition->Children()[i];
    if (!PartitionIsInfixOperatorOnly(child)) continue;
    if (NextLeaf(child) == nullptr) continue;
    VLOG(4) << "Attaching infix operator partition to following operand:\n"
            << child;
    verible::MergeLeafIntoNextLeaf(&child);
    // Children shifted; re-check current index.
    --i;
  }
}

static void AttachTrailingSemicolonToPreviousPartition(
    TokenPartitionTree *partition) {
  // TODO(mglb): Replace this function with
  // AttachSeparatorToPreviousOrNextPartition().

  // Attach the trailing ';' partition to the previous sibling leaf.
  // VisitIndentedSection() finished by starting a new partition,
  // so we need to back-track to the previous sibling partition.

  // In some cases where macros are involved, there may not necessarily
  // be a semicolon where one is grammatically expected.
  // In those cases, do nothing.
  auto *semicolon_partition = &RightmostDescendant(*partition);
  if (PartitionStartsWithSemicolon(*semicolon_partition)) {
    // When the semicolon is forced to wrap (e.g. when previous partition ends
    // with EOL comment), wrap previous partition with a group and append the
    // semicolon partition to it.
    if (PartitionIsForcedIntoNewLine(*semicolon_partition)) {
      auto *group = verible::GroupLeafWithPreviousLeaf(semicolon_partition);
      // Update invalidated pointer
      semicolon_partition = &RightmostDescendant(*group);
      group->Value().SetPartitionPolicy(
          verible::PartitionPolicyEnum::kAlwaysExpand);
      // Set indentation of partition with semicolon to match indentation of the
      // partition it has been grouped with
      semicolon_partition->Value().SetIndentationSpaces(
          group->Value().IndentationSpaces());
    } else {
      verible::MergeLeafIntoPreviousLeaf(semicolon_partition);
    }
    VLOG(4) << "after moving semicolon:\n" << *partition;
  }
}

static bool PartitionIsAllComments(const TokenPartitionTree &partition) {
  const auto tokens = partition.Value().TokensRange();
  return !tokens.empty() && std::all_of(tokens.begin(), tokens.end(), [](const PreFormatToken &t) {
    return IsComment(verilog_tokentype(t.TokenEnum()));
  });
}

// svfmt: `first` skips leading partitions (comment lines before the
// statement).
static void AdjustSubsequentPartitionsIndentation(TokenPartitionTree *partition,
                                                  int indentation, size_t first = 0) {
  // Adjust indentation of subsequent partitions
  const auto npartitions = partition->Children().size();
  if (npartitions > first + 1) {
    const auto &first_partition = partition->Children()[first];
    const auto &last_partition = partition->Children().back();

    // Do not indent intentionally wrapped partitions, e.g.
    // { (>>[assign foo = {] }
    // { (>>>>[<auto>]
    //   { (>>>>[a ,] }
    //   { (>>>>[b ,] }
    //   { (>>>>[c ,] }
    //   { (>>>>[d] }
    // }
    // { (>>[} ;] }
    if (!(PartitionEndsWithOpenBrace(first_partition) &&
          PartitionStartsWithCloseBrace(last_partition)) &&
        !(PartitionEndsWithOpenParen(first_partition) &&
          PartitionIsCloseParenSemi(last_partition))) {
      for (size_t idx = first + 1; idx < npartitions; ++idx) {
        AdjustIndentationRelative(&partition->Children()[idx], indentation);
      }
    }
  }
}

// Merges first found subpartition ending with "=" or ":" with "'{" or "{"
// from the subpartition following it.
static void AttachOpeningBraceToDeclarationsAssignmentOperator(
    TokenPartitionTree *partition) {
  const int children_count = partition->Children().size();
  if (children_count <= 1) return;

  for (int i = 0; i < children_count - 1; ++i) {
    auto &current = RightmostDescendant(partition->Children()[i]);
    const auto tokens = current.Value().TokensRange();
    if (tokens.empty()) continue;

    auto rbegin = std::make_reverse_iterator(tokens.end());
    auto rend = std::make_reverse_iterator(tokens.begin());
    auto last_non_comment_it =
        std::find_if_not(rbegin, rend, [](const PreFormatToken &token) {
          // Lines with EOL Comment are non-mergable, so don't skip it here.
          return token.TokenEnum() == verilog_tokentype::TK_COMMENT_BLOCK;
        });
    if (last_non_comment_it == rend ||
        !(last_non_comment_it->TokenEnum() == '=' ||
          last_non_comment_it->TokenEnum() == ':')) {
      continue;
    }

    const auto &next = partition->Children()[i + 1];
    if (next.Value().IsEmpty()) continue;

    const auto &next_token = next.Value().TokensRange().front();
    if (!(next_token.TokenEnum() == verilog_tokentype::TK_LP ||
          next_token.TokenEnum() == '{')) {
      continue;
    }

    if (!PartitionIsForcedIntoNewLine(next)) {
      verible::MergeLeafIntoNextLeaf(&current);
      // There shouldn't be more matching partitions
      return;
    }
  }
}

static void ReshapeIfClause(const SyntaxTreeNode &node,
                            TokenPartitionTree *partition_ptr) {
  auto &partition = *partition_ptr;
  const SyntaxTreeNode *body = GetAnyControlStatementBody(node);
  if (body == nullptr || !NodeIsBeginEndBlock(*body)) {
    VLOG(4) << "if-body was not a begin-end block.";
    // If body is a null statement, attach it to the previous partition.
    AttachTrailingSemicolonToPreviousPartition(partition_ptr);
    return;
  }

  // The 'if (...)' header may have come from a macro expansion (no tokens of
  // its own); then there is nothing in this clause to fuse 'begin' with.
  if (partition.Children().size() < 2) return;
  // Then fuse the 'begin' partition with the preceding 'if (...)'
  auto &if_body_partition = partition.Children().back();
  auto &begin_partition = if_body_partition.Children().front();
  verible::MergeLeafIntoPreviousLeaf(&begin_partition);
  partition.Value().SetPartitionPolicy(
      if_body_partition.Value().PartitionPolicy());
  // if seq_block body was empty, that leaves only 'end', so hoist.
  // if if-header was flat, hoist that too.
  FlattenOneChild(partition, verible::BirthRank(if_body_partition));
}

static void ReshapeElseClause(const SyntaxTreeNode &node,
                              TokenPartitionTree *partition_ptr) {
  auto &partition = *partition_ptr;
  const SyntaxTreeNode &else_body_subnode =
      *ABSL_DIE_IF_NULL(GetAnyControlStatementBody(node));
  if (!NodeIsConditionalOrBlock(else_body_subnode)) {
    VLOG(4) << "else-body was neither a begin-end block nor if-conditional.";
    // If body is a null statement, attach it to the previous partition.
    AttachTrailingSemicolonToPreviousPartition(partition_ptr);
    return;
  }

  // Then fuse 'else' and 'begin' partitions together
  // or fuse the 'else' and 'if' (header) partitions together
  auto &children = partition.Children();
  auto else_partition_iter = std::find_if(
      children.begin(), children.end(), [](const TokenPartitionTree &n) {
        const auto *origin = n.Value().Origin();
        return origin &&
               origin->Tag() == verible::LeafTag(verilog_tokentype::TK_else);
      });
  if (else_partition_iter == children.end()) return;

  auto &else_partition = *else_partition_iter;
  auto *next_leaf = NextLeaf(else_partition);
  if (!next_leaf || PartitionIsForcedIntoNewLine(*next_leaf)) return;

  const auto *next_origin = next_leaf->Value().Origin();
  if (!next_origin ||
      !(next_origin->Tag() == verible::NodeTag(GroupKind::kBegin) ||
        next_origin->Tag() == verible::LeafTag(verilog_tokentype::TK_begin) ||
        next_origin->Tag() == verible::NodeTag(GroupKind::kIfHeader) ||
        next_origin->Tag() == verible::LeafTag(verilog_tokentype::TK_if))) {
    return;
  }

  verible::MergeLeafIntoNextLeaf(&else_partition);
}

static void HoistOnlyChildPartition(TokenPartitionTree *partition) {
  // kWrap uses relative child indentation as a hanging
  // if (partition->Value().PartitionPolicy() == PartitionPolicyEnum::kWrap) {
  // TODO(mglb): implement or remove.
  // }
  const auto *origin = partition->Value().Origin();
  if (HoistOnlyChild(*partition)) {
    VLOG(4) << "reshape: hoisted, using child partition policy, parent origin";
    // Preserve source origin
    partition->Value().SetOrigin(origin);
  }
}

// True if any token in this leaf partition is an EOL comment.
static bool PartitionContainsEOLComment(const TokenPartitionTree &partition) {
  for (const auto &token : partition.Value().TokensRange()) {
    if (token.TokenEnum() == verilog_tokentype::TK_EOL_COMMENT) return true;
  }
  return false;
}

static void PushEndIntoElsePartition(TokenPartitionTree *partition_ptr) {
  // Then combine 'end' with the following 'else' ...
  // Do not flatten, so that if- and else- clauses can make formatting
  // decisions independently from each other.
  auto &partition = *partition_ptr;
  auto &if_clause_partition = partition.Children().front();
  auto *end_partition = &RightmostDescendant(if_clause_partition);
  // When 'end' carries a trailing EOL comment, 'else' must start on the next
  // line (see token annotator: comment before else => MustWrap). Merging
  // end+comment into the else-if header makes fit-else-expand treat the
  // header as wider than the eventual formatted line, which wraps the
  // else-if body on re-format and fails convergence (GitHub issue 2540).
  if (PartitionContainsEOLComment(*end_partition)) {
    VLOG(4) << "end has EOL comment, skip merge into else";
    return;
  }
  auto *end_parent = verible::MergeLeafIntoNextLeaf(end_partition);
  // if moving leaf results in any singleton partitions, hoist.
  if (end_parent != nullptr) {
    HoistOnlyChildPartition(end_parent);
  }
}

static void MergeEndElseWithoutLabel(const SyntaxTreeNode &conditional,
                                     TokenPartitionTree *partition_ptr) {
  auto &partition = *partition_ptr;
  // Handle merging of 'else' partition with (possibly)
  // a previous 'end' partition.
  // Do not flatten, so that if- and else- clauses can make formatting
  // decisions independently from each other.
  const auto *if_body_subnode =
      GetAnyControlStatementBody(*GetAnyConditionalIfClause(conditional));
  if (if_body_subnode == nullptr || !NodeIsBeginEndBlock(*if_body_subnode)) {
    VLOG(4) << "if-body was not begin-end block";
    return;
  }
  VLOG(4) << "if body was a begin-end block";
  const auto *end_label = GetEndLabel(GetBlockEnd(*if_body_subnode));
  if (end_label != nullptr) {
    VLOG(4) << "'end' came with label, no merge";
    return;
  }
  VLOG(4) << "No 'end' label, merge 'end' and 'else...' partitions";
  PushEndIntoElsePartition(&partition);
}

static void FlattenElseIfElse(const SyntaxTreeNode &else_clause,
                              TokenPartitionTree *partition_ptr) {
  // Keep chained else-if-else conditionals in a flat structure.
  auto &partition = *partition_ptr;
  const auto &else_body_subnode = *GetAnyControlStatementBody(else_clause);
  if (NodeIsConditionalConstruct(else_body_subnode) &&
      GetAnyConditionalElseClause(else_body_subnode) != nullptr) {
    FlattenOneChild(partition, partition.Children().size() - 1);
  }
}

static void ReshapeConditionalConstruct(const SyntaxTreeNode &conditional,
                                        TokenPartitionTree *partition_ptr,
                                        const FormatStyle &style) {
  const auto *else_clause = GetAnyConditionalElseClause(conditional);
  if (else_clause == nullptr) {
    VLOG(4) << "there was no else clause";
    return;
  }
  // svfmt: "if (...) x;" may come from a macro expansion, leaving only the
  // else clause in this file.
  if (GetAnyConditionalIfClause(conditional) == nullptr) return;
  VLOG(4) << "there was an else clause";
  if (!style.wrap_end_else_clauses) {
    MergeEndElseWithoutLabel(conditional, partition_ptr);
  }
  FlattenElseIfElse(*else_clause, partition_ptr);
}

static bool TokenIsComment(const PreFormatToken &t) {
  return IsComment(verilog_tokentype(t.TokenEnum()));
}

static void SetCommentLinePartitionAsAlreadyFormatted(
    TokenPartitionTree *partition) {
  for (auto &child : partition->Children()) {
    if (!is_leaf(child)) continue;
    const auto tokens = child.Value().TokensRange();
    if (std::all_of(tokens.begin(), tokens.end(), TokenIsComment)) {
      child.Value().SetPartitionPolicy(PartitionPolicyEnum::kAlreadyFormatted);
    }
  }
}

using TokenPartitionPredicate = std::function<bool(const TokenPartitionTree &)>;

// TokenPartitionPredicate returning true for partitions whose Origin's tag is
// equal to any tag from `tags` list.
struct OriginTagIs {
  OriginTagIs(std::initializer_list<SymbolTag> tags) : tags(tags) {}

  std::initializer_list<SymbolTag> tags;

  bool operator()(const TokenPartitionTree &partition) const {
    if (!partition.Value().Origin()) return false;
    for (const auto &tag : tags) {
      if (partition.Value().Origin()->Tag() == tag) return true;
    }
    return false;
  }
};

// TokenPartitionPredicate returning true for partitions containing token with a
// type equal to any value from `token_types` list.
struct ContainsToken {
  ContainsToken(std::initializer_list<int> token_types)
      : token_types(token_types) {}

  std::initializer_list<int> token_types;

  bool operator()(const TokenPartitionTree &partition) const {
    for (const auto &token : partition.Value().TokensRange()) {
      for (const auto token_type : token_types) {
        if (token.TokenEnum() == token_type) return true;
      }
    }
    return false;
  }
};

// Finds direct child of the `parent` node satisfying the `predicate`. Returns
// nullptr when child has not been found.
static const TokenPartitionTree *FindDirectChild(
    const TokenPartitionTree *parent, TokenPartitionPredicate predicate) {
  if (!parent) return nullptr;
  auto iter = std::find_if(parent->Children().begin(), parent->Children().end(),
                           std::move(predicate));
  if (iter == parent->Children().end()) return nullptr;
  return &(*iter);
}

// Finds direct child of the `parent` node satisfying the `predicate`. Returns
// nullptr when child has not been found.
static TokenPartitionTree *FindDirectChild(TokenPartitionTree *parent,
                                           TokenPartitionPredicate predicate) {
  const TokenPartitionTree *const_parent = parent;
  return const_cast<TokenPartitionTree *>(
      FindDirectChild(const_parent, std::move(predicate)));
}

static bool LineBreaksInsidePartitionBeforeChild(
    const TokenPartitionTree &parent, const TokenPartitionTree &child) {
  for (auto &node : parent.Children()) {
    if (PartitionIsForcedIntoNewLine(node)) return true;
    if (&node == &child) break;
  }
  return false;
}

class MacroCallReshaper {
 public:
  explicit MacroCallReshaper(const FormatStyle &style,
                             TokenPartitionTree *main_node)
      : style_(style),
        main_node_(main_node),
        is_nested_(IsNested(*main_node)) {}

  void Reshape() {
    const auto *const main_node_origin = main_node_->Value().Origin();

    if (!FindInitialPartitions()) {
      // Something went wrong, leave partitions intact. Log message has been
      // printed.
      return;
    }

    if (semicolon_) {
      if (semicolon_ == NextSibling(*paren_group_)) {
        if (!PartitionIsForcedIntoNewLine(*semicolon_)) {
          // Merge with ')'
          verible::MergeLeafIntoPreviousLeaf(semicolon_);
          semicolon_ = nullptr;
        }
      }
    }

    if (ReshapeEmptyParenGroup()) {
      auto *identifier_with_paren_group = main_node_;
      if (!IsLastChild(*paren_group_)) {
        // There is a semicolon preceded by comment line(s).
        identifier_with_paren_group =
            verible::GroupLeafWithPreviousLeaf(paren_group_);
        identifier_with_paren_group->Value().SetPartitionPolicy(
            PartitionPolicyEnum::kJuxtapositionOrIndentedStack);
        main_node_->Value().SetPartitionPolicy(PartitionPolicyEnum::kStack);
      }

      if (main_node_origin &&
          main_node_origin->Tag() == NodeTag(SyntaxKind::MacroUsage)) {
        // '(' must follow macro identifier in the same line
        CHECK(!PartitionIsForcedIntoNewLine(*paren_group_));
        identifier_with_paren_group->Children().clear();
        identifier_with_paren_group->Value().SetPartitionPolicy(
            PartitionPolicyEnum::kAlreadyFormatted);
      }

      return;
    }

    if ((!argument_list_ || is_leaf(*argument_list_)) &&
        (BirthRank(*r_paren_) - BirthRank(*l_paren_) > 1)) {
      // Group partitions between parentheses (comments and stuff)
      CreateArgumentList();
    }

    // TODO(jbylicki): This part causes mismatches in the syntax tree of the
    // formatted code.
    if (argument_list_ &&
        (is_nested_ || !PartitionIsForcedIntoNewLine(*r_paren_))) {
      // Format like a part of argument list.
      MoveRightParenToArgumentList();
    }

    if (argument_list_ && style_.try_wrap_long_lines) {
      for (auto &arg : argument_list_->Children()) {
        const auto tokens = arg.Value().TokensRange();
        // Hack: in order to avoid wrapping just before comment, do not enable
        // wrapping on partitions containing comments.
        if (is_leaf(arg) &&
            arg.Value().PartitionPolicy() !=
                PartitionPolicyEnum::kAlreadyFormatted &&
            std::none_of(tokens.begin(), tokens.end(),
                         [](const PreFormatToken &t) {
                           return IsComment(verilog_tokentype(t.TokenEnum()));
                         })) {
          arg.Value().SetPartitionPolicy(PartitionPolicyEnum::kWrap);
        }
      }
    }

    if (main_node_origin &&
        main_node_origin->Tag() == NodeTag(SyntaxKind::MacroUsage)) {
      // '(' must follow macro identifier in the same line
      MergeIdentifierAndLeftParen();
    } else if (!LineBreaksInsidePartitionBeforeChild(*paren_group_,
                                                     *l_paren_)) {
      GroupIdentifierCommentsAndLeftParen();
    } else if (l_paren_ != &paren_group_->Children().front()) {
      GroupCommentsAndLeftParen();
    }

    if (argument_list_) {
      HoistOnlyChildPartition(argument_list_);
      if (paren_group_->Children().size() == 3) {
        // Children: '(', argument_list, ')'
        GroupLeftParenAndArgumentList();
      }

      if (!PartitionIsForcedIntoNewLine(paren_group_->Children().back())) {
        paren_group_->Value().SetPartitionPolicy(
            PartitionPolicyEnum::kJuxtapositionOrIndentedStack);
      } else {
        paren_group_->Value().SetPartitionPolicy(PartitionPolicyEnum::kStack);
      }
    }
  }

 private:
  static bool IsLeftParenPartition(const TokenPartitionTree &node) {
    const auto tokens = node.Value().TokensRange();
    bool found = false;
    auto token = tokens.begin();
    for (; token != tokens.end(); ++token) {
      if (token->TokenEnum() == '(') {
        found = true;
        ++token;
        break;
      }
      if (token->TokenEnum() == verilog_tokentype::TK_COMMENT_BLOCK) continue;
      return false;
    }
    for (; token != tokens.end(); ++token) {
      if (IsComment(verilog_tokentype(token->TokenEnum()))) continue;
      if (found && token->TokenEnum() == ')') continue;
      return false;
    }
    return found;
  }

  // Report a bug/untested situation, but don't abort.
#define LOG_PARTITION_BUG(reason)                               \
  LOG(ERROR) << "formatting of macro call failed: " << (reason) \
             << "\n*** Please file a bug. ***";

  bool FindInitialPartitions() {
    // Note: identifier can contain tokens for macro name + EOL comment
    identifier_ = FindDirectChild(
        main_node_, ContainsToken{verilog_tokentype::SystemTFIdentifier,
                                  verilog_tokentype::MacroCallId});
    if (!identifier_) {
      LOG_PARTITION_BUG("identifier not found.");
      return false;
    }

    paren_group_ = FindDirectChild(main_node_,
                                   OriginTagIs{NodeTag(GroupKind::kParenGroup), NodeTag(SyntaxKind::ArgumentList)});

    // Macro can be used without parentheses
    if (!paren_group_) {
      return false;
    }

    // Make sure there's nothing between identifier and paren group partitions.
    // Optional comment partitions are expected to be inside paren group.
    if (NextSibling(*identifier_) != paren_group_) {
      LOG_PARTITION_BUG(
          "Unexpected partitions between identifier and paren_group.");
      return false;
    }

    if (!IsLastChild(*paren_group_)) {
      semicolon_ = &main_node_->Children().back();
      if (!ContainsToken{';'}(*semicolon_)) {
        LOG_PARTITION_BUG("Unexpected partition(s) after the call.");
        LOG(ERROR) << "\n" << *main_node_;
        return false;
      }
    }

    if (is_leaf(*paren_group_)) {
      l_paren_ = paren_group_;
      r_paren_ = paren_group_;
    } else {
      l_paren_ = FindDirectChild(paren_group_, IsLeftParenPartition);
      if (!l_paren_) {
        LOG_PARTITION_BUG("'(' not found.");
        return false;
      }
      r_paren_ = &paren_group_->Children().back();
      if (!ContainsToken{
              ')', verilog_tokentype::MacroCallCloseToEndLine}(*r_paren_)) {
        LOG_PARTITION_BUG("')' not found.");
        return false;
      }
      const auto l_paren_index = BirthRank(*l_paren_);
      const auto r_paren_index = BirthRank(*r_paren_);
      CHECK_LE(l_paren_index, r_paren_index);
      for (std::size_t i = l_paren_index + 1; i < r_paren_index; ++i) {
        auto &child = paren_group_->Children()[i];
        if (OriginTagIs{NodeTag(ListKind::ArgumentList_parameters),
                        NodeTag(GroupKind::kMacroArgList)}(child)) {
          argument_list_ = &child;
          break;
        }
      }
    }
    return true;
  }

#undef LOG_PARTITION_BUG

  bool ReshapeEmptyParenGroup() {
    if (paren_group_->Children().size() == 2 && l_paren_ != r_paren_ &&
        l_paren_->Value().TokensRange().end() ==
            r_paren_->Value().TokensRange().begin() &&
        !PartitionIsForcedIntoNewLine(*r_paren_)) {
      VLOG(6) << "Flatten paren group.";
      paren_group_->Children().clear();
    }
    if (is_leaf(*paren_group_)) {
      if (!PartitionIsForcedIntoNewLine(*paren_group_)) {
        main_node_->Value().SetPartitionPolicy(
            PartitionPolicyEnum::kJuxtapositionOrIndentedStack);
      }
      VLOG(6) << "Empty paren group.";
      return true;
    }

    return false;
  }

  void CreateArgumentList() {
    const int arguments_indentation =
        paren_group_->Value().IndentationSpaces() + style_.wrap_spaces;
    auto group = TokenPartitionTree(verible::UnwrappedLine(
        arguments_indentation, l_paren_->Value().TokensRange().end(),
        PartitionPolicyEnum::kWrap));
    group.Value().SpanUpToToken(r_paren_->Value().TokensRange().begin());

    const auto paren_group_begin = paren_group_->Children().begin();
    const auto args_begin = paren_group_begin + BirthRank(*l_paren_) + 1;
    const auto args_end = paren_group_begin + BirthRank(*r_paren_);
    // Move partitions into the group.
    group.Children().assign(std::make_move_iterator(args_begin),
                            std::make_move_iterator(args_end));
    for (auto &node : group.Children()) {
      verible::AdjustIndentationAbsolute(&node, arguments_indentation);
    }
    // Remove leftover entries of all grouped partitions except the first.
    paren_group_->Children().erase(args_begin + 1, args_end);

    // Move the group into first grouped partition's place.
    *args_begin = std::move(group);

    argument_list_ = &*args_begin;
    r_paren_ = NextSibling(*argument_list_);
  }

  void MoveRightParenToArgumentList() {
    {
      r_paren_->Value().SetIndentationSpaces(
          argument_list_->Value().IndentationSpaces());
      const auto r_paren_iter =
          paren_group_->Children().begin() + BirthRank(*r_paren_);
      argument_list_->Value().SpanUpToToken(
          r_paren_->Value().TokensRange().end());
      argument_list_->Children().push_back(std::move(*r_paren_));
      r_paren_ = &argument_list_->Children().back();
      paren_group_->Children().erase(r_paren_iter);
    }
    if (!PartitionIsForcedIntoNewLine(*r_paren_)) {
      // We want to avoid wrapping just before `)`. It would be best to use
      // token's break_penalty in Layout Optimizer, but that's a task for
      // future.
      // So far, use hacky heuristic: merge short partitions (things
      // like `);`) and group longer ones (anything with comments).
      const auto r_paren_tokens = r_paren_->Value().TokensRange();
      if (is_leaf(*r_paren_) &&
          std::none_of(r_paren_tokens.begin(), r_paren_tokens.end(),
                       [](const PreFormatToken &t) {
                         return IsComment(verilog_tokentype(t.TokenEnum()));
                       })) {
        // Merge
        verible::MergeLeafIntoPreviousLeaf(r_paren_);
        r_paren_ = &argument_list_->Children().back();
      } else {
        // Group
        auto &last_argument = *PreviousSibling(*r_paren_);
        auto group = TokenPartitionTree(
            verible::UnwrappedLine(last_argument.Value().IndentationSpaces(),
                                   last_argument.Value().TokensRange().begin(),
                                   PartitionPolicyEnum::kJuxtaposition));
        group.Value().SpanUpToToken(r_paren_->Value().TokensRange().end());
        group.Children().reserve(2);
        group.Children().push_back(std::move(last_argument));
        group.Children().push_back(std::move(*r_paren_));
        argument_list_->Children().erase(argument_list_->Children().end() - 1);
        argument_list_->Children().back() = std::move(group);
        r_paren_ = &argument_list_->Children().back();
      }
    }
  }

  void GroupIdentifierCommentsAndLeftParen() {
    const bool r_paren_is_in_arg_list = (r_paren_->Parent() == argument_list_);
    auto group = TokenPartitionTree(
        verible::UnwrappedLine(main_node_->Value().IndentationSpaces(),
                               identifier_->Value().TokensRange().begin(),
                               PartitionPolicyEnum::kJuxtaposition));
    group.Value().SpanUpToToken(l_paren_->Value().TokensRange().end());

    verible::AdjustIndentationAbsolute(paren_group_,
                                       main_node_->Value().IndentationSpaces());

    paren_group_->Value().SpanBackToToken(group.Value().TokensRange().begin());

    const auto old_identifier_iter =
        identifier_->Parent()->Children().begin() + BirthRank(*identifier_);
    const auto l_paren_index = BirthRank(*l_paren_);
    auto nodes_to_group = verible::iterator_range(
        paren_group_->Children().begin(),
        paren_group_->Children().begin() + l_paren_index + 1);

    const auto nodes_count = l_paren_index + 1 + 1;  // + 1 for identifier
    group.Children().reserve(nodes_count);
    // Move partitions into the group.
    group.Children().push_back(std::move(*identifier_));
    group.Children().insert(group.Children().end(),
                            std::make_move_iterator(nodes_to_group.begin()),
                            std::make_move_iterator(nodes_to_group.end()));
    // Remove leftover entries of all grouped partitions except:
    // * First paren_group's child: will be reused as a group node.
    // * The identifier: removing it will invalidate iterators; done later.
    paren_group_->Children().erase(nodes_to_group.begin() + 1,
                                   nodes_to_group.end());

    // Move the group into first grouped partition's place.
    *nodes_to_group.begin() = std::move(group);

    // Remove identifier
    main_node_->Children().erase(old_identifier_iter);

    HoistOnlyChildPartition(main_node_);

    paren_group_ = main_node_;
    identifier_ = nullptr;
    l_paren_ = &paren_group_->Children().front();
    if (argument_list_) {
      argument_list_ = NextSibling(*l_paren_);
      r_paren_ = r_paren_is_in_arg_list && !argument_list_->Children().empty()
                     ? &argument_list_->Children().back()
                     : NextSibling(*argument_list_);
    } else {
      r_paren_ = &paren_group_->Children().back();
    }
  }

  void MergeIdentifierAndLeftParen() {
    const bool r_paren_is_in_arg_list = (r_paren_->Parent() == argument_list_);
    CHECK_EQ(NextLeaf(*identifier_), l_paren_);
    CHECK(!PartitionIsForcedIntoNewLine(*l_paren_));
    verible::AdjustIndentationAbsolute(paren_group_,
                                       main_node_->Value().IndentationSpaces());
    verible::MergeLeafIntoNextLeaf(identifier_);
    HoistOnlyChildPartition(main_node_);

    paren_group_ = main_node_;
    identifier_ = nullptr;
    l_paren_ = &paren_group_->Children().front();
    if (argument_list_) {
      argument_list_ = NextSibling(*l_paren_);
      r_paren_ = r_paren_is_in_arg_list ? &argument_list_->Children().back()
                                        : NextSibling(*argument_list_);
    } else {
      r_paren_ = &paren_group_->Children().back();
    }
  }

  void GroupCommentsAndLeftParen() {
    // Group comments + '('
    auto group = TokenPartitionTree(
        verible::UnwrappedLine(paren_group_->Value().IndentationSpaces(),
                               paren_group_->Value().TokensRange().begin(),
                               PartitionPolicyEnum::kWrap));
    group.Value().SpanUpToToken(l_paren_->Value().TokensRange().end());

    const auto l_paren_index = BirthRank(*l_paren_);
    const auto paren_group_begin = paren_group_->Children().begin();
    auto nodes_to_group = verible::iterator_range(
        paren_group_begin, paren_group_begin + l_paren_index + 1);

    // Move partitions into the group.
    group.Children().assign(std::make_move_iterator(nodes_to_group.begin()),
                            std::make_move_iterator(nodes_to_group.end()));

    // Remove leftover entries of all grouped partitions except:
    // * First paren_group's child: will be reused as a group node.
    paren_group_->Children().erase(nodes_to_group.begin() + 1,
                                   nodes_to_group.end());

    // Move the group into first grouped partition's place.
    *nodes_to_group.begin() = std::move(group);

    l_paren_ = &(*nodes_to_group.begin());

    if (argument_list_) {
      argument_list_ = NextSibling(*l_paren_);
      r_paren_ = is_nested_ ? &argument_list_->Children().back()
                            : NextSibling(*argument_list_);
    } else {
      r_paren_ = &paren_group_->Children().back();
    }
  }

  void GroupLeftParenAndArgumentList() {
    auto old_argument_list_iter =
        paren_group_->Children().begin() + BirthRank(*argument_list_);

    const auto group_policy =
        PartitionIsForcedIntoNewLine(*argument_list_)
            ? PartitionPolicyEnum::kStack
            : PartitionPolicyEnum::kJuxtapositionOrIndentedStack;

    auto group = TokenPartitionTree(verible::UnwrappedLine(
        l_paren_->Value().IndentationSpaces(),
        l_paren_->Value().TokensRange().begin(), group_policy));
    group.Value().SpanUpToToken(argument_list_->Value().TokensRange().end());
    AdoptSubtree(group, std::move(*l_paren_), std::move(*argument_list_));

    *l_paren_ = std::move(group);
    paren_group_->Children().erase(old_argument_list_iter);

    l_paren_ = nullptr;
    argument_list_ = nullptr;
  }

  const FormatStyle &style_;
  TokenPartitionTree *const main_node_;

  // Subnodes of main_node_. Initialized by FindInitialPartitions().
  // Tree modifications invalidate pointers, so keeping pointers here instead
  // of passing them to each method allows for more readable code.
  TokenPartitionTree *identifier_ = nullptr;
  TokenPartitionTree *paren_group_ = nullptr;
  TokenPartitionTree *argument_list_ = nullptr;
  TokenPartitionTree *l_paren_ = nullptr;
  TokenPartitionTree *r_paren_ = nullptr;
  TokenPartitionTree *semicolon_ = nullptr;

  static bool IsNested(const TokenPartitionTree &macro_call_node) {
    auto *paren_group = FindDirectChild(
        &macro_call_node, OriginTagIs{NodeTag(GroupKind::kParenGroup), NodeTag(SyntaxKind::ArgumentList)});
    if (!paren_group) return false;
    return paren_group->Value().IndentationSpaces() ==
           macro_call_node.Value().IndentationSpaces();
  }

  const bool is_nested_;
};

static void IndentBetweenUVMBeginEndMacros(TokenPartitionTree *partition_ptr,
                                           int indentation_spaces) {
  auto &partition = *partition_ptr;
  // Indent elements between UVM begin/end macro calls
  unsigned int uvm_level = 1;
  std::vector<TokenPartitionTree *> uvm_range;

  // Search backwards for matching _begin.
  for (auto *itr = PreviousSibling(partition); itr;
       itr = PreviousSibling(*itr)) {
    VLOG(4) << "Scanning previous sibling:\n" << *itr;
    const auto macroId =
        LeftmostDescendant(*itr).Value().TokensRange().front().Text();
    VLOG(4) << "macro id: " << macroId;

    // Indent only uvm macros
    if (!absl::StartsWith(macroId, "`uvm_")) {
      continue;
    }

    // Count uvm indentation level
    if (absl::EndsWith(macroId, "_end")) {
      uvm_level++;
    } else if (absl::EndsWith(macroId, "_begin")) {
      uvm_level--;
    }

    // Break before indenting matching _begin macro
    if (uvm_level == 0) {
      break;
    }

    uvm_range.push_back(itr);
  }

  // Found matching _begin-_end macros
  if ((uvm_level == 0) && !uvm_range.empty()) {
    VLOG(4) << "Found matching uvm-begin/end macros";
    for (auto *itr : uvm_range) {
      // Indent uvm macros inside `uvm.*begin - `uvm.*end
      verible::AdjustIndentationRelative(itr, +indentation_spaces);
    }
  }
}

// Returns the initializer expression of the first declarator, if any.
static const SyntaxTreeNode *GetAssignedExpressionFromDataDeclaration(
    const SyntaxTreeNode &data_declaration) {
  const auto *list = FindChildNode(data_declaration,
                                   Tag(ListKind::DataDeclaration_declarators));
  if (!list || list->empty() || !list->front() ||
      Kind(*list->front()) != SyntaxKind::Declarator) {
    return nullptr;
  }
  const auto &declarator = verible::SymbolCastToNode(*list->front());
  const auto *init = FindChildNode(declarator, static_cast<int>(SyntaxKind::EqualsValueClause));
  if (!init) return nullptr;
  return LastChildNode(*init);
}

static size_t InstanceCount(const SyntaxTreeNode &node) {
  for (const auto &child : node.children()) {
    if (!child || child->Kind() != verible::SymbolKind::kNode) continue;
    const int tag = child->Tag().tag;
    if (tag == Tag(ListKind::DataDeclaration_declarators) ||
        tag == Tag(ListKind::HierarchyInstantiation_instances)) {
      size_t count = 0;
      for (const auto &item : verible::SymbolCastToNode(*child).children()) {
        if (item && item->Kind() == verible::SymbolKind::kNode) ++count;
      }
      return count;
    }
  }
  return 0;
}

static void HandleDataDeclaration(const SyntaxTreeNode &node,
                                  const FormatStyle &style,
                                  TokenPartitionTree *partition) {
  AttachTrailingSemicolonToPreviousPartition(partition);
  auto &data_declaration_partition = *partition;
  auto &children = data_declaration_partition.Children();
  // svfmt: a declaration expanded from a macro leaves only its ';' here.
  if (children.empty()) return;

  // The instances/variable declaration list is always in last position.
  auto &instance_list_partition = children.back();
  if (verible::BirthRank(instance_list_partition) == 0) {
    VLOG(4) << "No qualifiers and type is implicit";
    verible::AdjustIndentationAbsolute(
        &instance_list_partition,
        data_declaration_partition.Value().IndentationSpaces());
    HoistOnlyChildPartition(&instance_list_partition);
  } else if (InstanceCount(node) == 1) {
    VLOG(4) << "Instance list has only one child, singleton.";
    // svfmt: the list partition still wraps the single instance partition
    // (plus possibly an empty trailing partition); hoist it so the flattening
    // below reaches the instance's children.
    {
      auto &list_children = instance_list_partition.Children();
      while (list_children.size() > 1 && is_leaf(list_children.back()) &&
             list_children.back().Value().IsEmpty()) {
        list_children.pop_back();
      }
      HoistOnlyChildPartition(&instance_list_partition);
    }
    verible::AdjustIndentationRelative(&instance_list_partition,
                                       -style.wrap_spaces);
    auto *instance_type_partition =
        ABSL_DIE_IF_NULL(PreviousSibling(children.back()));
    if (instance_type_partition == &children.front()) {
      std::vector<size_t> offsets;
      FlattenOnlyChildrenWithChildren(data_declaration_partition, &offsets);
      const size_t fuse_position = offsets.back() - 1;
      MergeConsecutiveSiblings(&data_declaration_partition, fuse_position);
    } else {
      FlattenOnlyChildrenWithChildren(data_declaration_partition);
    }
  } else {
    VLOG(4) << "None of the special cases apply.";
  }

  if (Kind(node) != SyntaxKind::DataDeclaration) return;
  const auto *assigned_expr = GetAssignedExpressionFromDataDeclaration(node);
  if (!assigned_expr || assigned_expr->empty()) return;

  AttachOpeningBraceToDeclarationsAssignmentOperator(partition);

  // Special handling for assignment of a string concatenation
  if (Kind(*assigned_expr) != SyntaxKind::ConcatenationExpression) return;
  const auto *items =
      FindChildNode(*assigned_expr, Tag(ListKind::ConcatenationExpression_expressions));
  if (!items) return;
  if (std::all_of(items->children().begin(), items->children().end(),
                  [](const verible::SymbolPtr &sym) {
                    if (!sym) return true;
                    if (sym->Kind() == verible::SymbolKind::kLeaf) return true;
                    const auto &expr = verible::SymbolCastToNode(*sym);
                    return Kind(expr) == SyntaxKind::StringLiteralExpression;
                  })) {
    if (data_declaration_partition.Children().size() < 2) return;
    auto &assigned_value_partition = data_declaration_partition.Children()[1];
    partition->Value().SetPartitionPolicy(
        PartitionPolicyEnum::kAppendFittingSubPartitions);
    assigned_value_partition.Value().SetPartitionPolicy(
        PartitionPolicyEnum::kAlwaysExpand);
  }
}

// This phase is strictly concerned with reshaping token partitions,
// and occurs on the return path of partition tree construction.
// TODO: this method is long; possibly break out functionality similar to
// HandleDataDeclaration().
void TreeUnwrapper::ReshapeTokenPartitions(
    const SyntaxTreeNode &node, const FormatStyle &style,
    TokenPartitionTree *recent_partition) {
  const int tag = node.Tag().tag;
  const SyntaxKind kind = Kind(node);
  VLOG(3) << __FUNCTION__ << " node: " << svfmt::NodeTagName(tag);
  TokenPartitionTree &partition = *recent_partition;
  VLOG(4) << "before reshaping " << svfmt::NodeTagName(tag) << ":\n"
          << VerilogPartitionPrinter(partition);

  // Few node types that need to skip hoisting at the end of this function set
  // this flag to true.
  bool skip_singleton_hoisting = false;

  // svfmt: Verible's ReshapeTokenPartitions() with NodeEnum cases mapped to
  // slang kinds and svfmt list/group kinds.
  if (svfmt::IsListTag(tag)) {
    switch (static_cast<ListKind>(tag)) {
      case ListKind::ArgumentList_parameters:
        SetCommentLinePartitionAsAlreadyFormatted(&partition);
        AttachSeparatorsToListElementPartitions(&partition);
        break;
      case ListKind::ConstraintBlock_items: {
        HoistOnlyChildPartition(&partition);
        // Always expand constraint(s) blocks with braces inside them
        const auto &ftokens = partition.Value().TokensRange();
        auto found = std::find_if(ftokens.begin(), ftokens.end(),
                                  [](const verible::PreFormatToken &token) {
                                    return token.TokenEnum() == '{';
                                  });
        if (found != ftokens.end()) {
          partition.Value().SetPartitionPolicy(PartitionPolicyEnum::kAlwaysExpand);
        }
        break;
      }
      case ListKind::DataDeclaration_declarators:
      case ListKind::HierarchyInstantiation_instances:
      case ListKind::PrimitiveInstantiation_instances:
        // parent: data declaration / instantiation
        skip_singleton_hoisting = true;
        AttachSeparatorsToListElementPartitions(&partition);
        break;
      case ListKind::ParameterValueAssignment_parameters:
      case ListKind::DistConstraintList_items:
      case ListKind::EnumType_members:
      case ListKind::ParameterPortList_declarations:
      case ListKind::HierarchicalInstance_connections:
      case ListKind::AnsiPortList_ports:
      case ListKind::NonAnsiPortList_ports:
      case ListKind::MacroFormalArgumentList_args:
      case ListKind::ModportDeclaration_items:
        AttachSeparatorsToListElementPartitions(&partition);
        break;
      case ListKind::FunctionPortList_ports:
        // Verible: kParenGroup.
        SetCommentLinePartitionAsAlreadyFormatted(&partition);
        AttachSeparatorsToListElementPartitions(&partition);
        break;
      case ListKind::UdpBody_portDecls:
        // ';' separators belong to the preceding declaration.
        for (size_t i = partition.Children().size(); i > 1; --i) {
          auto &child = partition.Children()[i - 1];
          if (is_leaf(child) && PartitionStartsWithSemicolon(child)) {
            verible::MergeLeafIntoPreviousLeaf(&child);
          }
        }
        break;
      case ListKind::ConcatenationExpression_expressions:
      case ListKind::SimpleAssignmentPattern_items:
      case ListKind::StructuredAssignmentPattern_items:
        AttachSeparatorsToListElementPartitions(&partition);
        skip_singleton_hoisting = true;
        break;
      default:
        break;
    }
  } else if (svfmt::IsGroupTag(tag)) {
    switch (static_cast<GroupKind>(tag)) {
      case GroupKind::kMacroArgList:
        SetCommentLinePartitionAsAlreadyFormatted(&partition);
        AttachSeparatorsToListElementPartitions(&partition);
        break;
      case GroupKind::kParenGroup:
        SetCommentLinePartitionAsAlreadyFormatted(&partition);
        break;
      case GroupKind::kClassHeader: {
        // Allow empty parameters to appear as "#();"
        if (partition.Children().size() >= 2) {
          auto &last = partition.Children().back();
          auto &last_prev = *ABSL_DIE_IF_NULL(PreviousSibling(last));
          if (PartitionStartsWithCloseParen(last) &&
              PartitionEndsWithOpenParen(last_prev)) {
            verible::MergeLeafIntoPreviousLeaf(&last);
          }
        }
        break;
      }
      case GroupKind::kSubroutineHeader: {
        // May end up with a trailing ");" subpartition; attach it to the item
        // that preceded it.
        auto &last = RightmostDescendant(partition);
        if (PartitionIsCloseParenSemi(last)) {
          verible::MergeLeafIntoPreviousLeaf(&last);
        }
        break;
      }
      case GroupKind::kIfHeader: {
        // Fix indentation in case of e.g. function calls inside if headers
        if (partition.Children().size() > 1) {
          auto if_header_partition_iter = std::find_if(
              partition.Children().begin(), partition.Children().end(),
              [](const TokenPartitionTree &n) {
                const auto *origin = n.Value().Origin();
                return origin &&
                       origin->Tag() == verible::LeafTag(verilog_tokentype::TK_if);
              });
          if (if_header_partition_iter == partition.Children().end()) break;
          for (auto &child :
               make_range(if_header_partition_iter + 1, partition.Children().end())) {
            verible::AdjustIndentationRelative(&child, style.wrap_spaces);
          }
        }
        break;
      }
      case GroupKind::kIfClause:
        ReshapeIfClause(node, &partition);
        break;
      default:
        break;
    }
  } else {
    switch (kind) {
      // Note: this is also being applied to variable declaration lists,
      // which may want different handling than instantiations.
      case SyntaxKind::DataDeclaration:
      case SyntaxKind::HierarchyInstantiation:
        HandleDataDeclaration(node, style, &partition);
        break;

      case SyntaxKind::DPIImport:
      case SyntaxKind::DPIExport: {
        if (partition.Children().size() > 1) {
          verible::MergeLeafIntoNextLeaf(&partition.Children().front());
          AttachTrailingSemicolonToPreviousPartition(&partition);
        }
        break;
      }
      case SyntaxKind::BindDirective: {
        AttachTrailingSemicolonToPreviousPartition(&partition);
        // Verible: "bind target" leads the instantiation's first line
        // ("bind target type #(").
        auto &children = partition.Children();
        if (children.size() == 2 && is_leaf(children.front()) && !is_leaf(children.back())) {
          verible::MergeLeafIntoNextLeaf(&children.front());
          HoistOnlyChildPartition(&partition);
        }
        break;
      }
      case SyntaxKind::ModuleHeader:
      case SyntaxKind::InterfaceHeader:
      case SyntaxKind::ProgramHeader: {
        // Allow empty ports to appear as "();"
        if (partition.Children().size() >= 2) {
          auto &last = partition.Children().back();
          auto &last_prev = *ABSL_DIE_IF_NULL(PreviousSibling(last));
          if (PartitionStartsWithCloseParen(last) &&
              PartitionEndsWithOpenParen(last_prev)) {
            verible::MergeLeafIntoPreviousLeaf(&last);
          }
        }
        // If there were any parameters or ports at all, expand.
        if (partition.Children().size() > 2) {
          partition.Value().SetPartitionPolicy(PartitionPolicyEnum::kAlwaysExpand);
        }
        break;
      }

      case SyntaxKind::InvocationExpression: {
        const auto policy = partition.Value().PartitionPolicy();
        if (policy == PartitionPolicyEnum::kAppendFittingSubPartitions) {
          auto &last = RightmostDescendant(partition);
          if (PartitionStartsWithCloseParen(last) || PartitionStartsWithSemicolon(last)) {
            auto &token = last.Value().TokensRange().front();
            if (token.before.break_decision == verible::SpacingOptions::kMustWrap) {
              auto *group = verible::GroupLeafWithPreviousLeaf(&last);
              group->Value().SetPartitionPolicy(PartitionPolicyEnum::kStack);
            } else {
              verible::MergeLeafIntoPreviousLeaf(&last);
            }
          }
        } else if (policy == PartitionPolicyEnum::kStack) {
          MacroCallReshaper(style, &partition).Reshape();
        }
        break;
      }

      case SyntaxKind::MacroUsage: {
        const auto policy = partition.Value().PartitionPolicy();
        if (policy == PartitionPolicyEnum::kStack &&
            FindChildNode(node, static_cast<int>(GroupKind::kParenGroup)) != nullptr) {
          MacroCallReshaper(style, &partition).Reshape();
        } else {
          const auto *paren = FindChildNode(node, static_cast<int>(GroupKind::kParenGroup));
          const bool no_args =
              paren == nullptr ||
              FindChildNode(*paren, static_cast<int>(GroupKind::kMacroArgList)) == nullptr;
          if (no_args) {
            // If there are no call args, join the name, '(' and ')' together.
            partition.Children().clear();
          } else {
            // Merge closing parenthesis into last argument partition
            auto &last = RightmostDescendant(partition);
            if (PartitionStartsWithCloseParen(last) || PartitionIsCloseParenSemi(last)) {
              auto &token = last.Value().TokensRange().front();
              if (token.before.break_decision == verible::SpacingOptions::kMustWrap) {
                auto *group = verible::GroupLeafWithPreviousLeaf(&last);
                group->Value().SetPartitionPolicy(PartitionPolicyEnum::kStack);
              } else {
                verible::MergeLeafIntoPreviousLeaf(&last);
              }
            }
          }
        }
        // `uvm_*_end macros indent the items since the matching _begin.
        if (!node.empty() && node.front() &&
            node.front()->Kind() == verible::SymbolKind::kLeaf) {
          const auto text = verible::SymbolCastToLeaf(*node.front()).get().text();
          if (absl::StartsWith(text, "`uvm_") && absl::EndsWith(text, "_end") &&
              (DirectParentTag(Context()) == Tag(ListKind::ClassDeclaration_items) ||
               DirectParentTag(Context()) == Tag(ListKind::ModuleDeclaration_members) ||
               DirectParentTag(Context()) == Tag(ListKind::CompilationUnit_members))) {
            IndentBetweenUVMBeginEndMacros(&partition, style.indentation_spaces);
          }
        }
        break;
      }

      case SyntaxKind::ElseClause:
        ReshapeElseClause(node, &partition);
        break;

      case SyntaxKind::ConditionalStatement:
      case SyntaxKind::IfGenerate:
      case SyntaxKind::ImmediateAssertStatement:
      case SyntaxKind::ImmediateAssumeStatement:
      case SyntaxKind::ImmediateCoverStatement:
      case SyntaxKind::AssertPropertyStatement:
      case SyntaxKind::AssumePropertyStatement:
      case SyntaxKind::CoverPropertyStatement:
      case SyntaxKind::CoverSequenceStatement:
      case SyntaxKind::ExpectPropertyStatement:
      case SyntaxKind::RestrictPropertyStatement:
        ReshapeConditionalConstruct(node, &partition, style_);
        break;

      case SyntaxKind::DefineDirective: {
        auto &last = RightmostDescendant(partition);
        if (PartitionStartsWithCloseParen(last)) {
          verible::MergeLeafIntoPreviousLeaf(&last);
        }
        break;
      }

      case SyntaxKind::ConstraintDeclaration: {
        if (partition.Children().size() == 2) {
          auto &last = RightmostDescendant(partition);
          if (PartitionIsCloseBrace(last)) {
            verible::MergeLeafIntoPreviousLeaf(&last);
          }
        }
        break;
      }

      // kNetVariableAssignment, kBlockingAssignmentStatement,
      // kNonblockingAssignmentStatement, kAssignModifyStatement, kStatement
      case SyntaxKind::ExpressionStatement:
      case SyntaxKind::ProceduralAssignStatement:
      case SyntaxKind::ProceduralForceStatement: {
        AttachTrailingSemicolonToPreviousPartition(&partition);
        // RHS may have been further partitioned, e.g. a macro call.
        auto &children = partition.Children();
        if (children.size() == 2 && verible::is_leaf(children.front()) &&
            !PartitionIsForcedIntoNewLine(children.back())) {
          verible::MergeLeafIntoNextLeaf(&children.front());
        }
        break;
      }

      case SyntaxKind::TimingControlStatement: {
        std::vector<size_t> offsets;
        FlattenOnlyChildrenWithChildren(partition, &offsets);
        AttachTrailingSemicolonToPreviousPartition(&partition);
        // Check body, for a begin-end block, merge 'begin' with previous sibling
        if (const auto *tbody = LastChildNode(node);
            tbody != nullptr && NodeIsBeginEndBlock(*tbody) && offsets.size() > 1) {
          // A comment line before 'begin' makes the body a group; leave it.
          const auto &children = partition.Children();
          const size_t pos = offsets[1] - 1;
          if (pos + 1 < children.size() &&
              children[pos].Children().empty() == children[pos + 1].Children().empty()) {
            verible::MergeConsecutiveSiblings(&partition, pos);
          }
        }
        break;
      }

      case SyntaxKind::ContinuousAssign: {  // e.g. assign a=0, b=2;
        FlattenOnlyChildrenWithChildren(partition);
        AttachTrailingSemicolonToPreviousPartition(&partition);
        AttachSeparatorsToListElementPartitions(&partition);
        // Merge the 'assign' keyword with the (first) x=y assignment.
        // svfmt: comment lines before 'assign' are partitions of their own.
        size_t keyword = 0;
        while (keyword + 1 < partition.Children().size() &&
               is_leaf(partition.Children()[keyword]) &&
               PartitionIsAllComments(partition.Children()[keyword])) {
          ++keyword;
        }
        if (partition.Children().size() > keyword + 1) {
          verible::MergeLeafIntoNextLeaf(&partition.Children()[keyword]);
        }
        AdjustSubsequentPartitionsIndentation(&partition, style.wrap_spaces, keyword);
        break;
      }

      case SyntaxKind::LoopGenerate:
      case SyntaxKind::ForLoopStatement:
      case SyntaxKind::ForeverStatement:
      case SyntaxKind::LoopStatement:
      case SyntaxKind::ForeachLoopStatement:
      case SyntaxKind::StandardCaseItem:
      case SyntaxKind::DefaultCaseItem:
      case SyntaxKind::PatternCaseItem:
      case SyntaxKind::InitialBlock:
      case SyntaxKind::FinalBlock: {
        // Check if 'initial' is separated from 'begin' by EOL_COMMENT. If so,
        // adjust indentation and do not merge leaf into previous leaf.
        const verible::UnwrappedLine &unwrapped_line = partition.Value();
        verible::FormatTokenRange tokenrange = unwrapped_line.TokensRange();
        if (tokenrange.size() >= 3) {
          auto token_tmp = tokenrange.begin();
          if (token_tmp->TokenEnum() == TK_initial &&
              (++token_tmp)->TokenEnum() == TK_EOL_COMMENT &&
              (++token_tmp)->TokenEnum() == TK_begin) {
            AdjustIndentationRelative(
                &partition.Children()[(partition.Children().size() - 1)],
                style.indentation_spaces);
            break;
          }
        }

        // In these cases, merge the 'begin' partition of the statement block
        // with the preceding keyword or header partition.
        const auto *body = LastChildNode(node);
        if (partition.Children().size() > 1 && !verible::is_leaf(partition.Children().back()) &&
            body != nullptr && NodeIsBeginEndBlock(*body)) {
          auto &seq_block_partition = partition.Children().back();
          auto &begin_partition = LeftmostDescendant(seq_block_partition);
          CHECK(is_leaf(begin_partition));
          verible::MergeLeafIntoPreviousLeaf(&begin_partition);
          // Flatten only the statement block so that the control partition
          // can retain its own partition policy.
          FlattenOneChild(partition, verible::BirthRank(seq_block_partition));
        }
        break;
      }

      case SyntaxKind::DoWhileStatement: {
        const auto *body = FindChildNode(node, static_cast<int>(SyntaxKind::SequentialBlockStatement));
        if (body != nullptr && partition.Children().size() > 1) {
          // between do... and while (...);
          auto &seq_block_partition = partition.Children()[1];
          auto &begin_partition = LeftmostDescendant(seq_block_partition);
          verible::MergeLeafIntoPreviousLeaf(&begin_partition);
          auto &end_partition = RightmostDescendant(seq_block_partition);
          verible::MergeLeafIntoNextLeaf(&end_partition);
          FlattenOneChild(partition, verible::BirthRank(seq_block_partition));
        }
        break;
      }

      case SyntaxKind::AlwaysBlock:
      case SyntaxKind::AlwaysCombBlock:
      case SyntaxKind::AlwaysFFBlock:
      case SyntaxKind::AlwaysLatchBlock: {
        const auto *body = LastChildNode(node);
        if (body != nullptr && partition.Children().size() > 1 &&
            (Kind(*body) == SyntaxKind::TimingControlStatement ||
             Kind(*body) == SyntaxKind::SequentialBlockStatement)) {
          // Merge 'always' keyword with next sibling, and adjust subtree indent.
          verible::MergeLeafIntoNextLeaf(&partition.Children().front());
          verible::AdjustIndentationAbsolute(&partition.Children().front(),
                                             partition.Value().IndentationSpaces());
        }
        break;
      }

      case SyntaxKind::ModportDeclaration: {
        AttachSeparatorsToListElementPartitions(&partition);
        break;
      }

      case SyntaxKind::ParameterDeclaration:
      case SyntaxKind::TypeParameterDeclaration:
      case SyntaxKind::ParameterDeclarationStatement: {
        AttachTrailingSemicolonToPreviousPartition(&partition);
        AttachOpeningBraceToDeclarationsAssignmentOperator(&partition);
        AttachInfixOperatorsToFollowingOperands(&partition);
        break;
      }

      case SyntaxKind::AssignmentPatternExpression: {
        FlattenOnlyChildrenWithChildren(partition);
        break;
      }

      default:
        break;
    }
  }

  // In the majority of cases, automatically hoist singletons.
  // A few node types, however, will wish to delay the hoisting change,
  // so that the parent node can make reshaping decisions based on the
  // child node's original unhoisted form.
  if (!skip_singleton_hoisting) {
    HoistOnlyChildPartition(&partition);
  }

  VLOG(4) << "after reshaping " << svfmt::NodeTagName(tag) << ":\n"
          << VerilogPartitionPrinter(partition);
}

// Visitor to determine which leaf enum function to call
void TreeUnwrapper::Visit(const verible::SyntaxTreeLeaf &leaf) {
  VLOG(3) << __FUNCTION__ << " leaf: " << VerboseToken(leaf.get());
  const verilog_tokentype tag = verilog_tokentype(leaf.Tag().tag);

  // Catch up on any non-whitespace tokens that fall between the syntax tree
  // leaves, such as comments and attributes, stopping at the current leaf.
  CatchUpToCurrentLeaf(leaf.get());
  VLOG(4) << "Visit leaf: after CatchUp";

  // Possibly start new partition (without advancing token iterator).
  UpdateInterLeafScanner(tag);

  // Sanity check that NextUnfilteredToken() is aligned to the current leaf.
  CHECK(NextUnfilteredToken()->text().begin() == leaf.get().text().begin());

  // Start a new partition in the following cases.
  // In most other cases, do nothing.
  if (IsPreprocessorControlFlow(tag)) {
    VLOG(4) << "handling preprocessor control flow token";
    StartNewUnwrappedLine(PartitionPolicyEnum::kFitOnLineElseExpand, &leaf);
    CurrentUnwrappedLine().SetIndentationSpaces(0);
  } else if (IsPreprocessorKeyword(tag)) {
    // Compiler directives (DR_* tokens) that don't have parent nodes
    VLOG(4) << "handling compiler directive leaf token";
    StartNewUnwrappedLine(PartitionPolicyEnum::kFitOnLineElseExpand, &leaf);
    // Only unindent if at top level (context is empty) or inside preprocessor
    // clauses
    if (Context().empty() ||
        IsPreprocessorClause(Context().top().Tag().tag)) {
      CurrentUnwrappedLine().SetIndentationSpaces(0);
    }
  } else if (IsEndKeyword(tag)) {
    VLOG(4) << "handling end* keyword";
    StartNewUnwrappedLine(PartitionPolicyEnum::kAlwaysExpand, &leaf);
  }

  auto &partition = *ABSL_DIE_IF_NULL(CurrentTokenPartition());
  VLOG(4) << "before adding token " << VerboseToken(leaf.get()) << ":\n"
          << VerilogPartitionPrinter(partition);

  // Advances NextUnfilteredToken(), and extends CurrentUnwrappedLine().
  // Should be equivalent to AdvanceNextUnfilteredToken().
  AddTokenToCurrentUnwrappedLine();

  // Make sure that newly extended unwrapped line has origin symbol
  // FIXME: Pretty sure that this should be handled in an other way,
  //    e.g. in common/formatting/tree_unwrapper.cc similarly
  //    to StartNewUnwrappedLine()
  if (CurrentUnwrappedLine().Origin() == nullptr &&
      CurrentUnwrappedLine().TokensRange().size() == 1) {
    CurrentUnwrappedLine().SetOrigin(&leaf);
  }

  // Look-ahead to any trailing comments that are associated with this leaf,
  // up to a newline.
  LookAheadBeyondCurrentLeaf();

  VLOG(4) << "before reshaping " << VerboseToken(leaf.get()) << ":\n"
          << VerilogPartitionPrinter(partition);

  // Post-token-handling token partition adjustments:
  switch (leaf.Tag().tag) {
    case PP_else: {
      // Do not allow non-comment tokens on the same line as `else
      // (comments were handled above)
      StartNewUnwrappedLine(PartitionPolicyEnum::kFitOnLineElseExpand, &leaf);
      break;
    }
    default:
      break;
  }

  VLOG(4) << "after reshaping " << VerboseToken(leaf.get()) << ":\n"
          << VerilogPartitionPrinter(partition);
  VLOG(3) << "end of " << __FUNCTION__ << " leaf: " << VerboseToken(leaf.get());
}

// Specialized node visitors

void TreeUnwrapper::VisitNewUnwrappedLine(const SyntaxTreeNode &node) {
  StartNewUnwrappedLine(PartitionPolicyEnum::kFitOnLineElseExpand, &node);
  TraverseChildren(node);
}

void TreeUnwrapper::VisitNewUnindentedUnwrappedLine(
    const SyntaxTreeNode &node) {
  StartNewUnwrappedLine(PartitionPolicyEnum::kFitOnLineElseExpand, &node);
  // Force the current line to be unindented without losing track of where
  // the current indentation level is for children.
  CurrentUnwrappedLine().SetIndentationSpaces(0);
  TraverseChildren(node);
}

}  // namespace formatter
}  // namespace verilog

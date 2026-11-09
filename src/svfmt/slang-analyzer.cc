#include "svfmt/slang-analyzer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/ParserDiags.h"
#include "slang/diagnostics/PreprocessorDiags.h"
#include "slang/parsing/LexerFacts.h"
#include "slang/parsing/Parser.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/parsing/Token.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"
#include "slang/util/BumpAllocator.h"
#include "verible/common/text/concrete-syntax-leaf.h"
#include "verible/common/text/concrete-syntax-tree.h"
#include "verible/common/text/token-info.h"
#include "verible/common/text/token-stream-view.h"
#include "svfmt/group-kinds.h"
#include "svfmt/list-kinds.h"
#include "verible/verilog/parser/verilog-token-enum.h"

namespace svfmt {

using slang::parsing::LexerFacts;
using slang::parsing::Token;
using slang::parsing::TokenKind;
using slang::parsing::Trivia;
using slang::parsing::TriviaKind;
using slang::syntax::SyntaxNode;
using verible::SymbolPtr;
using verible::SyntaxTreeLeaf;
using verible::SyntaxTreeNode;
using verible::TokenInfo;

namespace {

const absl::flat_hash_map<std::string_view, int> &AliasMap() {
  static const auto *map = [] {
    auto *m = new absl::flat_hash_map<std::string_view, int>;
    for (const auto &entry : verilog::kVerilogTokenTable) {
      if (entry.alias != nullptr) m->emplace(entry.alias, entry.value);
    }
    m->emplace("^~", TK_NXOR);  // Verible lexes both spellings as TK_NXOR
    return m;
  }();
  return *map;
}

int BaseEnum(std::string_view base_text) {
  switch (std::tolower(static_cast<unsigned char>(base_text.back()))) {
    case 'b':
      return TK_BinBase;
    case 'o':
      return TK_OctBase;
    case 'h':
      return TK_HexBase;
    default:
      return TK_DecBase;
  }
}

int DigitsEnumForBase(int base_enum) {
  switch (base_enum) {
    case TK_BinBase:
      return TK_BinDigits;
    case TK_OctBase:
      return TK_OctDigits;
    case TK_HexBase:
      return TK_HexDigits;
    default:
      return TK_DecDigits;
  }
}

// Maps a slang token (outside directives) to a verilog_tokentype.
int MapToken(const Token &tok, int previous_enum) {
  switch (tok.kind) {
    case TokenKind::Identifier:
      return tok.rawText().starts_with('\\') ? EscapedIdentifier
                                              : SymbolIdentifier;
    case TokenKind::SystemIdentifier:
      return SystemTFIdentifier;
    case TokenKind::StringLiteral:
      return TK_StringLiteral;
    case TokenKind::IntegerBase:
      return BaseEnum(tok.rawText());
    case TokenKind::IntegerLiteral:
      if (previous_enum == TK_BinBase || previous_enum == TK_OctBase ||
          previous_enum == TK_DecBase || previous_enum == TK_HexBase) {
        return DigitsEnumForBase(previous_enum);
      }
      return TK_DecNumber;
    case TokenKind::UnbasedUnsizedLiteral:
      return TK_UnBasedNumber;
    case TokenKind::RealLiteral:
      return TK_RealTime;
    case TokenKind::TimeLiteral:
      return TK_TimeLiteral;
    case TokenKind::IncludeFileName:
      return tok.rawText().starts_with('<') ? TK_AngleBracketInclude
                                             : TK_StringLiteral;
    case TokenKind::Directive:
    case TokenKind::MacroUsage: {
      const auto &aliases = AliasMap();
      if (auto it = aliases.find(tok.rawText()); it != aliases.end()) {
        return it->second;
      }
      return MacroIdentifier;
    }
    case TokenKind::LineContinuation:
      return TK_LINE_CONT;
    default:
      break;
  }
  // Digits following a base that slang lexed as something else (e.g. 'hx).
  if (previous_enum == TK_BinBase || previous_enum == TK_OctBase ||
      previous_enum == TK_DecBase || previous_enum == TK_HexBase) {
    if (tok.kind != TokenKind::OpenParenthesis) {
      return DigitsEnumForBase(previous_enum);
    }
  }
  std::string_view text = LexerFacts::getTokenKindText(tok.kind);
  if (text.empty()) text = tok.rawText();
  const auto &aliases = AliasMap();
  if (auto it = aliases.find(text); it != aliases.end()) return it->second;
  if (text.size() == 1) return static_cast<unsigned char>(text[0]);
  return TK_OTHER;
}

}  // namespace

// A conditional compilation directive (`ifdef NAME, `else, ...) found by a
// lexical pre-scan of the source.
struct CondDirective {
  size_t begin, end;            // whole directive text
  size_t name_begin, name_end;  // macro name / expression, if any
  int keyword_enum;
  SyntaxKind kind;
};

// Finds conditional directives outside comments, strings and `define bodies.
std::vector<CondDirective> ScanConditionalDirectives(std::string_view text) {
  std::vector<CondDirective> out;
  size_t i = 0;
  const size_t n = text.size();
  auto ident_end = [&](size_t p) {
    while (p < n && (std::isalnum(static_cast<unsigned char>(text[p])) || text[p] == '_' ||
                     text[p] == '$')) {
      ++p;
    }
    return p;
  };
  while (i < n) {
    const char c = text[i];
    if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      while (i < n && text[i] != '\n') ++i;
    } else if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      size_t e = text.find("*/", i + 2);
      i = e == std::string_view::npos ? n : e + 2;
    } else if (c == '"') {
      ++i;
      while (i < n && text[i] != '"' && text[i] != '\n') i += text[i] == '\\' ? 2 : 1;
      ++i;
    } else if (c == '`') {
      const size_t kw_end = ident_end(i + 1);
      const std::string_view kw = text.substr(i + 1, kw_end - i - 1);
      if (kw == "define") {
        // Skip the macro body, including line continuations.
        size_t p = kw_end;
        while (p < n && !(text[p] == '\n' && text[p - 1] != '\\')) ++p;
        i = p;
        continue;
      }
      struct { std::string_view name; int e; SyntaxKind k; bool has_name; } static const kKinds[] = {
          {"ifdef", PP_ifdef, SyntaxKind::IfDefDirective, true},
          {"ifndef", PP_ifndef, SyntaxKind::IfNDefDirective, true},
          {"elsif", PP_elsif, SyntaxKind::ElsIfDirective, true},
          {"else", PP_else, SyntaxKind::ElseDirective, false},
          {"endif", PP_endif, SyntaxKind::EndIfDirective, false},
      };
      bool matched = false;
      for (const auto &k : kKinds) {
        if (kw != k.name) continue;
        CondDirective d{i, kw_end, kw_end, kw_end, k.e, k.k};
        if (k.has_name) {
          size_t p = kw_end;
          while (p < n && (text[p] == ' ' || text[p] == '\t')) ++p;
          if (p < n && text[p] == '(') {
            int depth = 0;
            size_t q = p;
            for (; q < n && text[q] != '\n'; ++q) {
              if (text[q] == '(') ++depth;
              if (text[q] == ')' && --depth == 0) { ++q; break; }
            }
            d.name_begin = p;
            d.name_end = d.end = q;
          } else {
            d.name_begin = p;
            d.name_end = d.end = ident_end(p);
          }
          if (d.name_end == d.name_begin) break;  // malformed; leave to slang
        }
        out.push_back(d);
        matched = true;
        i = d.end;
        break;
      }
      if (!matched) i = kw_end;
    } else {
      ++i;
    }
  }
  return out;
}

// Walks the slang tree once, building the token stream and the CST together.
class SlangAnalyzer::Builder {
 public:
  // `match_text` is the text slang parsed: identical to `contents` except
  // that the conditional directives in `directives` were blanked out (see
  // SlangAnalyzer::AnalyzeWithMode); they are re-inserted here.
  Builder(SlangAnalyzer &analyzer, std::string_view contents, std::string_view match_text,
          std::vector<CondDirective> directives, slang::BufferID buffer)
      : analyzer_(analyzer), contents_(contents), match_text_(match_text),
        directives_(std::move(directives)), buffer_(buffer), cursor_(contents.data()) {}

  absl::Status Build(const SyntaxNode &root, verible::TokenSequence &tokens,
                     SymbolPtr &tree) {
    auto node = std::make_unique<SyntaxTreeNode>(static_cast<int>(root.kind));
    analyzer_.origins_[node.get()] = &root;
    parent_first_.push_back(Token());
    ConvertMembers(root, *node);
    if (!status_.ok()) return status_;
    if (cursor_ != contents_.data() + contents_.size()) {
      return absl::InternalError(absl::StrCat(
          "svfmt: could not account for source text at byte offset ",
          Offset(cursor_),
          " (unsupported preprocessor construct); refusing to format",
          DebugTail()));
    }
    tokens = std::move(tokens_);
    tokens.push_back(TokenInfo::EOFToken(contents_));
    tree = std::move(node);
    return absl::OkStatus();
  }

 private:
  int Offset(const char *p) const { return static_cast<int>(p - contents_.data()); }

  // slang tokens do not always point into the source buffer (keywords and
  // punctuation return static text), so tokens are matched by location.
  // Returns the token's text in the main buffer if it starts at the cursor,
  // else an empty view (macro expansion, included file, missing token).
  std::string_view TokenAtCursor(const Token &tok) const {
    if (tok.rawText().empty()) return {};
    auto loc = tok.location();
    if (loc.buffer() != buffer_ ||
        loc.offset() + base_offset_ != static_cast<size_t>(Offset(cursor_))) {
      return {};
    }
    return contents_.substr(loc.offset() + base_offset_, tok.rawText().size());
  }

  // Trivia text is sometimes a copy (e.g. inside directives), so trivia is
  // matched by content. Returns the matching view of the main buffer, or an
  // empty view.
  std::string_view TriviaAtCursor(std::string_view text) const {
    const size_t offset = static_cast<size_t>(Offset(cursor_));
    if (text.empty() || match_text_.size() - offset < text.size()) return {};
    // Trivia that points into a source buffer can be placed exactly: text of
    // the main buffer must start at the cursor, text of another file (e.g.
    // the trailing trivia of an included file) is never ours.
    if (text.data() >= match_text_.data() &&
        text.data() < match_text_.data() + match_text_.size()) {
      if (static_cast<size_t>(text.data() - match_text_.data()) != offset) return {};
    } else if (InForeignBuffer(text.data())) {
      return {};
    }
    if (match_text_.substr(offset, text.size()) != text) return {};
    return contents_.substr(offset, text.size());
  }

  bool InForeignBuffer(const char *p) const {
    if (foreign_.empty()) {
      const auto &sm = *analyzer_.source_manager_;
      for (slang::BufferID id : sm.getAllBuffers()) {
        if (id == buffer_) continue;
        std::string_view t = sm.getSourceText(id);
        foreign_.emplace_back(t.data(), t.data() + t.size());
      }
      std::sort(foreign_.begin(), foreign_.end());
      if (foreign_.empty()) foreign_.emplace_back(nullptr, nullptr);
    }
    auto it = std::upper_bound(foreign_.begin(), foreign_.end(), p,
                               [](const char *q, const auto &range) { return q < range.first; });
    if (it == foreign_.begin()) return false;
    --it;
    return p >= it->first && p < it->second;
  }

  // Emits whitespace trivia text, re-inserting blanked conditional
  // directives as CST nodes appended to `parent`.
  void EmitWhitespaceRange(std::string_view text, SyntaxTreeNode &parent) {
    const size_t end = static_cast<size_t>(Offset(text.data())) + text.size();
    while (static_cast<size_t>(Offset(cursor_)) < end) {
      const size_t pos = static_cast<size_t>(Offset(cursor_));
      while (next_directive_ < directives_.size() && directives_[next_directive_].begin < pos) {
        ++next_directive_;
      }
      if (next_directive_ < directives_.size() && directives_[next_directive_].begin == pos) {
        const CondDirective &d = directives_[next_directive_++];
        auto node = std::make_unique<SyntaxTreeNode>(static_cast<int>(d.kind));
        size_t kw_end = d.begin + 1;
        while (kw_end < d.end && std::isalpha(static_cast<unsigned char>(contents_[kw_end]))) {
          ++kw_end;
        }
        node->AppendChild(std::make_unique<SyntaxTreeLeaf>(
            Emit(d.keyword_enum, contents_.substr(d.begin, kw_end - d.begin))));
        if (d.name_end > d.name_begin) {
          if (d.name_begin > kw_end) EmitWhitespace(contents_.substr(kw_end, d.name_begin - kw_end));
          node->AppendChild(std::make_unique<SyntaxTreeLeaf>(
              Emit(PP_Identifier, contents_.substr(d.name_begin, d.name_end - d.name_begin))));
        }
        parent.AppendChild(std::move(node));
        continue;
      }
      size_t stop = end;
      if (next_directive_ < directives_.size()) stop = std::min(stop, directives_[next_directive_].begin);
      EmitWhitespace(contents_.substr(pos, stop - pos));
    }
  }

  // Appends a token covering `text`, which must start at the cursor.
  const TokenInfo &Emit(int token_enum, std::string_view text) {
    tokens_.emplace_back(token_enum, text);
    cursor_ = text.data() + text.size();
    if (token_enum != TK_SPACE && token_enum != TK_NEWLINE &&
        token_enum != TK_EOL_COMMENT && token_enum != TK_COMMENT_BLOCK) {
      previous_enum_ = token_enum;
    }
    return tokens_.back();
  }


  void EmitWhitespace(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
      size_t j = i;
      if (text[i] == '\n') {
        j = i + 1;
        Emit(TK_NEWLINE, text.substr(i, 1));
      } else if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
        // Verible keeps '\r' as part of the preceding space run.
        j = i + 1;
        Emit(TK_SPACE, text.substr(i, 1));
      } else {
        while (j < text.size() && text[j] != '\n' &&
               !(text[j] == '\r' && j + 1 < text.size() && text[j + 1] == '\n')) {
          j++;
        }
        Emit(TK_SPACE, text.substr(i, j - i));
      }
      i = j;
    }
  }

  // Emits leading trivia of a token. Directives are converted into CST nodes
  // and appended to `parent`.
  void EmitTrivia(const Token &tok, SyntaxTreeNode &parent) {
    // On a token expanded from a macro, trivia after the last directive (the
    // macro usage itself) comes from the macro body, not from this file.
    auto trivia_list = tok.trivia();
    size_t count = trivia_list.size();
    if (analyzer_.source_manager_->isMacroLoc(tok.location())) {
      while (count > 0 && trivia_list[count - 1].kind != TriviaKind::Directive) --count;
    }
    for (size_t i = 0; i < count; ++i) {
      const Trivia &trivia = trivia_list[i];
      switch (trivia.kind) {
        case TriviaKind::Whitespace:
        case TriviaKind::EndOfLine:
          if (auto text = TriviaAtCursor(trivia.getRawText()); !text.empty()) {
            EmitWhitespaceRange(text, parent);
          }
          break;
        case TriviaKind::LineComment:
          if (auto text = TriviaAtCursor(trivia.getRawText()); !text.empty()) {
            Emit(TK_EOL_COMMENT, text);
          }
          break;
        case TriviaKind::BlockComment:
          if (auto text = TriviaAtCursor(trivia.getRawText()); !text.empty()) {
            Emit(TK_COMMENT_BLOCK, text);
          }
          break;
        case TriviaKind::DisabledText:
          if (auto text = TriviaAtCursor(trivia.getRawText()); !text.empty()) {
            EmitVerbatim(text);
          }
          break;
        case TriviaKind::Directive: {
          // EmitTrivia may run more than once for a token. Skip a directive
          // that was already consumed (its whitespace trivia, matched by
          // content, could otherwise match later text), and directives from
          // other buffers (stub definitions, excerpt wrappers).
          const Token first = trivia.syntax()->getFirstToken();
          if (first.location().buffer() != buffer_ ||
              first.location().offset() + base_offset_ < static_cast<size_t>(Offset(cursor_))) {
            break;
          }
          ConvertDirective(*trivia.syntax(), parent);
          break;
        }
        case TriviaKind::SkippedTokens:
        case TriviaKind::SkippedSyntax:
          if (status_.ok()) {
            status_ = absl::InvalidArgumentError("svfmt: source has syntax errors");
          }
          break;
        default:
          break;
      }
    }
  }

  void EmitVerbatim(std::string_view text) {
    analyzer_.verbatim_ranges_.emplace_back(Offset(text.data()),
                                            Offset(text.data()) + text.size());
    Emit(TK_SVFMT_DISABLED_TEXT, text);
  }

  // Emits a token that is part of a directive and appends it as a leaf.
  void EmitDirectiveToken(const Token &tok, int token_enum, SyntaxTreeNode &node) {
    EmitTrivia(tok, node);
    std::string_view text = TokenAtCursor(tok);
    if (text.empty()) return;
    node.AppendChild(std::make_unique<SyntaxTreeLeaf>(Emit(token_enum, text)));
  }

  // Emits a run of tokens (macro argument, `define body) as one token.
  void EmitMerged(std::span<const Token> toks, int token_enum, SyntaxTreeNode &node,
                  bool verbatim) {
    if (toks.empty()) return;
    EmitTrivia(toks.front(), node);
    // Leading trivia of later tokens is part of the span.
    if (TokenAtCursor(toks.front()).empty()) return;
    const auto &last = toks.back();
    if (last.location().buffer() != buffer_) return;
    const size_t begin = static_cast<size_t>(Offset(cursor_));
    const size_t end = last.location().offset() + base_offset_ + last.rawText().size();
    if (end <= begin) return;
    std::string_view text = contents_.substr(begin, end - begin);
    if (verbatim) {
      analyzer_.verbatim_ranges_.emplace_back(Offset(text.data()),
                                              Offset(text.data()) + text.size());
    }
    node.AppendChild(std::make_unique<SyntaxTreeLeaf>(Emit(token_enum, text)));
  }

  // Tokens of an inactive `ifdef branch become one verbatim token.
  void EmitDisabled(const slang::syntax::TokenList &toks, SyntaxTreeNode &node) {
    if (toks.empty()) return;
    std::vector<Token> list(toks.begin(), toks.end());
    // The list may end with a zero-width token whose trivia (line ends,
    // comments) follows the last real token; that trivia is emitted as usual.
    std::optional<Token> trailing;
    if (list.back().rawText().empty()) {
      trailing = list.back();
      list.pop_back();
    }
    EmitDisabledTokens(list, node);
    if (trailing) EmitTrivia(*trailing, node);
  }

  void EmitDisabledTokens(const std::vector<Token> &list, SyntaxTreeNode &node) {
    if (list.empty()) return;
    // Whitespace before the first disabled token stays ordinary whitespace.
    EmitTrivia(list.front(), node);
    if (TokenAtCursor(list.front()).empty()) return;
    const auto &last = list.back();
    const size_t begin = static_cast<size_t>(Offset(cursor_));
    const size_t end = last.location().offset() + base_offset_ + last.rawText().size();
    if (last.location().buffer() != buffer_ || end <= begin) return;
    std::string_view text = contents_.substr(begin, end - begin);
    analyzer_.verbatim_ranges_.emplace_back(begin, begin + text.size());
    analyzer_.disabled_branch_ranges_.emplace_back(begin, begin + text.size());
    // Its own group, so that the formatter can indent it apart from the
    // directive line.
    auto group = std::make_unique<SyntaxTreeNode>(static_cast<int>(GroupKind::kDisabledBranch));
    group->AppendChild(std::make_unique<SyntaxTreeLeaf>(Emit(TK_SVFMT_DISABLED_TEXT, text)));
    node.AppendChild(std::move(group));
  }

  void ConvertDirective(const SyntaxNode &syntax, SyntaxTreeNode &parent) {
    using namespace slang::syntax;
    // Leading trivia (which may contain re-inserted conditional directives)
    // goes before the directive, not into it.
    EmitTrivia(syntax.getFirstToken(), parent);
    auto node = std::make_unique<SyntaxTreeNode>(static_cast<int>(syntax.kind));
    analyzer_.origins_[node.get()] = &syntax;
    switch (syntax.kind) {
      case SyntaxKind::MacroUsage: {
        const auto &usage = syntax.as<MacroUsageSyntax>();
        EmitDirectiveToken(usage.directive, usage.args ? MacroCallId : MacroIdentifier,
                           *node);
        if (usage.args) {
          const auto &args = *usage.args;
          Group(GroupKind::kParenGroup, *node, [&](SyntaxTreeNode &paren) {
            EmitDirectiveToken(args.openParen, '(', paren);
            Group(GroupKind::kMacroArgList, paren, [&](SyntaxTreeNode &list) {
              for (size_t i = 0; i < args.args.getChildCount(); i++) {
                auto child = args.args.getChild(i);
                if (child.isNode()) {
                  const auto &toks = child.node()->as<MacroActualArgumentSyntax>().tokens;
                  std::vector<Token> arg(toks.begin(), toks.end());
                  ConvertMacroArg(arg, list);
                } else {
                  EmitDirectiveToken(child.token(), ',', list);
                }
              }
            });
            EmitDirectiveToken(args.closeParen, ')', paren);
          });
        }
        break;
      }
      case SyntaxKind::DefineDirective: {
        const auto &define = syntax.as<DefineDirectiveSyntax>();
        EmitDirectiveToken(define.directive, PP_define, *node);
        EmitDirectiveToken(define.name, PP_Identifier, *node);
        if (define.formalArguments) {
          // Formal arguments (Verible keeps them in the macro name line
          // untouched). With comments or line breaks inside, keep them as
          // one verbatim token: their layout is part of the definition.
          std::vector<Token> formals;
          CollectTokens(*define.formalArguments, formals);
          bool complex = false;
          for (size_t i = 1; i < formals.size(); ++i) {
            for (const auto &t : formals[i].trivia()) {
              if (t.kind != TriviaKind::Whitespace) complex = true;
            }
          }
          if (complex) {
            EmitMerged(formals, '(', *node, /*verbatim=*/true);
          } else {
            ConvertDirectiveTokens(*define.formalArguments, *node);
          }
        }
        std::vector<Token> body(define.body.begin(), define.body.end());
        EmitMerged(body, PP_define_body, *node, /*verbatim=*/true);
        break;
      }
      case SyntaxKind::IfDefDirective:
      case SyntaxKind::IfNDefDirective:
      case SyntaxKind::ElsIfDirective: {
        const auto &branch = syntax.as<ConditionalBranchDirectiveSyntax>();
        EmitDirectiveToken(branch.directive, MapToken(branch.directive, 0), *node);
        ConvertDirectiveTokens(*branch.expr, *node, /*name_enum=*/PP_Identifier);
        EmitDisabled(branch.disabledTokens, *node);
        break;
      }
      case SyntaxKind::ElseDirective:
      case SyntaxKind::EndIfDirective: {
        const auto &branch = syntax.as<UnconditionalBranchDirectiveSyntax>();
        EmitDirectiveToken(branch.directive, MapToken(branch.directive, 0), *node);
        EmitDisabled(branch.disabledTokens, *node);
        break;
      }
      case SyntaxKind::PragmaDirective: {
        // `pragma ends at the end of the line and its contents are free-form
        // ("`pragma protect end"): keep everything after `pragma verbatim.
        std::vector<Token> toks;
        CollectTokens(syntax, toks);
        if (toks.empty()) break;
        EmitDirectiveToken(toks.front(), MapToken(toks.front(), 0), *node);
        toks.erase(toks.begin());
        EmitMerged(toks, PP_define_body, *node, /*verbatim=*/true);
        break;
      }
      case SyntaxKind::UndefDirective:
        ConvertDirectiveTokens(syntax, *node, /*name_enum=*/PP_Identifier);
        break;
      default:
        ConvertDirectiveTokens(syntax, *node);
        break;
    }
    if (!node->empty()) parent.AppendChild(std::move(node));
  }

  static void CollectTokens(const SyntaxNode &syntax, std::vector<Token> &out) {
    for (size_t i = 0; i < syntax.getChildCount(); i++) {
      if (auto *child = syntax.childNode(i)) {
        CollectTokens(*child, out);
      } else if (Token tok = syntax.childToken(i)) {
        out.push_back(tok);
      }
    }
  }

  // Generic directive conversion: every token becomes a leaf.
  void ConvertDirectiveTokens(const SyntaxNode &syntax, SyntaxTreeNode &node,
                              int name_enum = 0) {
    for (size_t i = 0; i < syntax.getChildCount(); i++) {
      if (auto *child = syntax.childNode(i)) {
        ConvertDirectiveTokens(*child, node, name_enum);
        continue;
      }
      Token tok = syntax.childToken(i);
      if (!tok) continue;
      int token_enum = MapToken(tok, previous_enum_);
      if (name_enum != 0 && tok.kind == TokenKind::Identifier) token_enum = name_enum;
      if (tok.kind == TokenKind::Directive) {
        if (auto it = AliasMap().find(tok.rawText()); it != AliasMap().end()) {
          token_enum = it->second;
        }
      }
      EmitDirectiveToken(tok, token_enum, node);
    }
  }

  // Macro arguments are never formatted: the text between the commas is kept
  // verbatim as one token (whitespace inside a macro call is part of the
  // macro's text).
  void ConvertMacroArg(std::span<const Token> toks, SyntaxTreeNode &list) {
    EmitMerged(toks, MacroArg, list, /*verbatim=*/false);
  }

  // slang has separate '.' and '*' tokens; Verible lexes ".*" as one.
  void EmitDotStar(const Token &dot, const Token &star, SyntaxTreeNode &out) {
    EmitTrivia(dot, out);
    if (!TokenAtCursor(dot).empty() && star.trivia().empty() &&
        star.location().buffer() == buffer_ &&
        star.location().offset() == dot.location().offset() + 1) {
      out.AppendChild(std::make_unique<SyntaxTreeLeaf>(
          Emit(TK_DOTSTAR, contents_.substr(dot.location().offset() + base_offset_, 2))));
    } else {
      Tok(dot, out);
      Tok(star, out);
    }
  }

  // Runs `fill` to populate a synthetic group node appended to `out`.
  template <typename Fill>
  void Group(GroupKind kind, SyntaxTreeNode &out, Fill fill) {
    auto group = std::make_unique<SyntaxTreeNode>(static_cast<int>(kind));
    parent_first_.push_back(Token());
    groups_.emplace_back(group.get(), &out);
    fill(*group);
    groups_.pop_back();
    parent_first_.pop_back();
    if (!group->empty()) out.AppendChild(std::move(group));
  }

  template <typename ListT>
  static bool LabelBeforeAttributes(const slang::syntax::NamedLabelSyntax *label,
                                    const ListT &attributes) {
    if (label == nullptr || attributes.empty()) return false;
    return label->getFirstToken().location() < attributes[0]->getFirstToken().location();
  }

  // Hand-written conversions that insert Verible-like grouping nodes.
  bool ConvertSpecial(const SyntaxNode &node, SyntaxTreeNode &out) {
    using namespace slang::syntax;
    switch (node.kind) {
      case SyntaxKind::ConditionalStatement: {
        const auto &n = node.as<ConditionalStatementSyntax>();
        Child(n.label, out);
        List(n.attributes, static_cast<int>(ListKind::ConditionalStatement_attributes), out);
        Group(GroupKind::kIfClause, out, [&](SyntaxTreeNode &clause) {
          Group(GroupKind::kIfHeader, clause, [&](SyntaxTreeNode &g) {
            Tok(n.uniqueOrPriority, g);
            Tok(n.ifKeyword, g);
            Tok(n.openParen, g);
            Child(n.predicate.get(), g);
            Tok(n.closeParen, g);
          });
          Group(GroupKind::kIfBody, clause,
                [&](SyntaxTreeNode &g) { Child(n.statement.get(), g); });
        });
        Child(n.elseClause, out);
        return true;
      }
      case SyntaxKind::IfGenerate: {
        const auto &n = node.as<IfGenerateSyntax>();
        List(n.attributes, static_cast<int>(ListKind::IfGenerate_attributes), out);
        Group(GroupKind::kIfClause, out, [&](SyntaxTreeNode &clause) {
          Group(GroupKind::kIfHeader, clause, [&](SyntaxTreeNode &g) {
            Tok(n.keyword, g);
            Tok(n.openParen, g);
            Child(n.condition.get(), g);
            Tok(n.closeParen, g);
          });
          Group(GroupKind::kIfBody, clause,
                [&](SyntaxTreeNode &g) { Child(n.block.get(), g); });
        });
        Child(n.elseClause, out);
        return true;
      }
      // Verible's kAssertionClause, kAssertPropertyClause, kWaitStatement...:
      // grouped like an if clause, with the action's else clause after it.
      case SyntaxKind::ImmediateAssertStatement:
      case SyntaxKind::ImmediateAssumeStatement:
      case SyntaxKind::ImmediateCoverStatement: {
        const auto &n = node.as<ImmediateAssertionStatementSyntax>();
        Child(n.label, out);
        List(n.attributes, static_cast<int>(ListKind::ImmediateAssertionStatement_attributes),
             out);
        Group(GroupKind::kIfClause, out, [&](SyntaxTreeNode &clause) {
          Group(GroupKind::kIfHeader, clause, [&](SyntaxTreeNode &g) {
            Tok(n.keyword, g);
            Child(n.delay, g);
            Child(n.expr.get(), g);
          });
          Group(GroupKind::kIfBody, clause,
                [&](SyntaxTreeNode &g) { Child(n.action->statement, g); });
        });
        Child(n.action->elseClause, out);
        return true;
      }
      case SyntaxKind::AssertPropertyStatement:
      case SyntaxKind::AssumePropertyStatement:
      case SyntaxKind::CoverPropertyStatement:
      case SyntaxKind::CoverSequenceStatement:
      case SyntaxKind::ExpectPropertyStatement:
      case SyntaxKind::RestrictPropertyStatement: {
        const auto &n = node.as<ConcurrentAssertionStatementSyntax>();
        Child(n.label, out);
        List(n.attributes, static_cast<int>(ListKind::ConcurrentAssertionStatement_attributes),
             out);
        Group(GroupKind::kIfClause, out, [&](SyntaxTreeNode &clause) {
          Group(GroupKind::kIfHeader, clause, [&](SyntaxTreeNode &g) {
            Tok(n.keyword, g);
            Tok(n.propertyOrSequence, g);
            Tok(n.openParen, g);
            Child(n.propertySpec.get(), g);
            Tok(n.closeParen, g);
          });
          Group(GroupKind::kIfBody, clause,
                [&](SyntaxTreeNode &g) { Child(n.action->statement, g); });
        });
        Child(n.action->elseClause, out);
        return true;
      }
      case SyntaxKind::PropertyDeclaration: {
        const auto &n = node.as<PropertyDeclarationSyntax>();
        List(n.attributes, static_cast<int>(ListKind::PropertyDeclaration_attributes), out);
        Tok(n.keyword, out);
        Tok(n.name, out);
        Child(n.portList, out);
        Tok(n.semi, out);
        Group(GroupKind::kAssertionDeclBody, out, [&](SyntaxTreeNode &g) {
          List(n.variables, static_cast<int>(ListKind::PropertyDeclaration_variables), g);
          Child(n.propertySpec.get(), g);
          Tok(n.optionalSemi, g);
        });
        Tok(n.end, out);
        Child(n.endBlockName, out);
        return true;
      }
      case SyntaxKind::SequenceDeclaration: {
        const auto &n = node.as<SequenceDeclarationSyntax>();
        List(n.attributes, static_cast<int>(ListKind::SequenceDeclaration_attributes), out);
        Tok(n.keyword, out);
        Tok(n.name, out);
        Child(n.portList, out);
        Tok(n.semi, out);
        Group(GroupKind::kAssertionDeclBody, out, [&](SyntaxTreeNode &g) {
          List(n.variables, static_cast<int>(ListKind::SequenceDeclaration_variables), g);
          Child(n.seqExpr.get(), g);
          Tok(n.optionalSemi, g);
        });
        Tok(n.end, out);
        Child(n.endBlockName, out);
        return true;
      }
      case SyntaxKind::WithFunctionSample: {
        // Verible lexes "sample" here as the keyword TK_sample.
        const auto &n = node.as<WithFunctionSampleSyntax>();
        Tok(n.with, out);
        Tok(n.function, out);
        EmitDirectiveToken(n.sample, TK_sample, out);
        Child(n.portList, out);
        return true;
      }
      case SyntaxKind::WaitStatement: {
        const auto &n = node.as<WaitStatementSyntax>();
        Child(n.label, out);
        List(n.attributes, static_cast<int>(ListKind::WaitStatement_attributes), out);
        Group(GroupKind::kIfClause, out, [&](SyntaxTreeNode &clause) {
          Group(GroupKind::kIfHeader, clause, [&](SyntaxTreeNode &g) {
            Tok(n.wait, g);
            Tok(n.openParen, g);
            Child(n.expr.get(), g);
            Tok(n.closeParen, g);
          });
          Group(GroupKind::kIfBody, clause,
                [&](SyntaxTreeNode &g) { Child(n.statement.get(), g); });
        });
        return true;
      }
      case SyntaxKind::ElseClause: {
        const auto &n = node.as<ElseClauseSyntax>();
        Tok(n.elseKeyword, out);
        Group(GroupKind::kElseBody, out,
              [&](SyntaxTreeNode &g) { Child(n.clause.get(), g); });
        return true;
      }
      case SyntaxKind::SequentialBlockStatement:
      case SyntaxKind::ParallelBlockStatement: {
        const auto &n = node.as<BlockStatementSyntax>();
        // "L: (* a *) begin": the label may come before the attributes.
        const bool label_first = LabelBeforeAttributes(n.label, n.attributes);
        if (!label_first) List(n.attributes, static_cast<int>(ListKind::BlockStatement_attributes), out);
        Group(GroupKind::kBegin, out, [&](SyntaxTreeNode &g) {
          Child(n.label, g);
          if (label_first) List(n.attributes, static_cast<int>(ListKind::BlockStatement_attributes), g);
          Tok(n.begin, g);
          Child(n.blockName, g);
        });
        List(n.items, static_cast<int>(ListKind::BlockStatement_items), out);
        Group(GroupKind::kEnd, out, [&](SyntaxTreeNode &g) {
          Tok(n.end, g);
          Child(n.endBlockName, g);
        });
        return true;
      }
      case SyntaxKind::GenerateBlock: {
        const auto &n = node.as<GenerateBlockSyntax>();
        // "L: (* a *) begin": the label may come before the attributes.
        const bool label_first = LabelBeforeAttributes(n.label, n.attributes);
        if (!label_first) List(n.attributes, static_cast<int>(ListKind::GenerateBlock_attributes), out);
        Group(GroupKind::kBegin, out, [&](SyntaxTreeNode &g) {
          Child(n.label, g);
          if (label_first) List(n.attributes, static_cast<int>(ListKind::GenerateBlock_attributes), g);
          Tok(n.begin, g);
          Child(n.beginName, g);
        });
        List(n.members, static_cast<int>(ListKind::GenerateBlock_members), out);
        Group(GroupKind::kEnd, out, [&](SyntaxTreeNode &g) {
          Tok(n.end, g);
          Child(n.endName, g);
        });
        return true;
      }
      case SyntaxKind::ForLoopStatement: {
        const auto &n = node.as<ForLoopStatementSyntax>();
        Child(n.label, out);
        List(n.attributes, static_cast<int>(ListKind::ForLoopStatement_attributes), out);
        Group(GroupKind::kLoopHeader, out, [&](SyntaxTreeNode &g) {
          Tok(n.forKeyword, g);
          Tok(n.openParen, g);
          List(n.initializers, static_cast<int>(ListKind::ForLoopStatement_initializers), g);
          Tok(n.semi1, g);
          Child(n.stopExpr, g);
          Tok(n.semi2, g);
          List(n.steps, static_cast<int>(ListKind::ForLoopStatement_steps), g);
          Tok(n.closeParen, g);
        });
        Child(n.statement.get(), out);
        return true;
      }
      case SyntaxKind::ForeachLoopStatement: {
        const auto &n = node.as<ForeachLoopStatementSyntax>();
        Child(n.label, out);
        List(n.attributes, static_cast<int>(ListKind::ForeachLoopStatement_attributes), out);
        Group(GroupKind::kLoopHeader, out, [&](SyntaxTreeNode &g) {
          Tok(n.keyword, g);
          Child(n.loopList.get(), g);
        });
        Child(n.statement.get(), out);
        return true;
      }
      case SyntaxKind::LoopStatement: {
        const auto &n = node.as<LoopStatementSyntax>();
        Child(n.label, out);
        List(n.attributes, static_cast<int>(ListKind::LoopStatement_attributes), out);
        Group(GroupKind::kLoopHeader, out, [&](SyntaxTreeNode &g) {
          Tok(n.repeatOrWhile, g);
          Tok(n.openParen, g);
          Child(n.expr.get(), g);
          Tok(n.closeParen, g);
        });
        Child(n.statement.get(), out);
        return true;
      }
      case SyntaxKind::LoopGenerate: {
        const auto &n = node.as<LoopGenerateSyntax>();
        List(n.attributes, static_cast<int>(ListKind::LoopGenerate_attributes), out);
        Group(GroupKind::kLoopHeader, out, [&](SyntaxTreeNode &g) {
          Tok(n.keyword, g);
          Tok(n.openParen, g);
          Tok(n.genvar, g);
          Tok(n.identifier, g);
          Tok(n.equals, g);
          Child(n.initialExpr.get(), g);
          Tok(n.semi1, g);
          Child(n.stopExpr.get(), g);
          Tok(n.semi2, g);
          Child(n.iterationExpr.get(), g);
          Tok(n.closeParen, g);
        });
        Child(n.block.get(), out);
        return true;
      }
      case SyntaxKind::ParameterPortList: {
        // slang groups "#(parameter int A = 1, B = 2)" into one declaration
        // with two declarators; Verible has one kParamDeclaration per
        // parameter. Split them so that each parameter is a list element.
        const auto &n = node.as<ParameterPortListSyntax>();
        Tok(n.hash, out);
        Tok(n.openParen, out);
        auto list = std::make_unique<SyntaxTreeNode>(
            static_cast<int>(ListKind::ParameterPortList_declarations));
        parent_first_.push_back(Token());
        for (size_t i = 0; i < n.declarations.getChildCount(); ++i) {
          auto child = n.declarations.getChild(i);
          if (!child.isNode()) {
            Tok(child.token(), *list);
            continue;
          }
          const SyntaxNode &decl = *child.node();
          if (decl.kind == SyntaxKind::ParameterDeclaration) {
            SplitParameterDeclaration(decl.as<ParameterDeclarationSyntax>(), *list);
          } else {
            Child(&decl, *list);
          }
        }
        parent_first_.pop_back();
        out.AppendChild(std::move(list));
        Tok(n.closeParen, out);
        return true;
      }
      case SyntaxKind::UdpSimpleField: {
        // Verible lexes each UDP table symbol separately ("00" -> "0" "0").
        const auto &n = node.as<UdpSimpleFieldSyntax>();
        EmitTrivia(n.field, out);
        std::string_view text = TokenAtCursor(n.field);
        for (size_t i = 0; i < text.size(); ++i) {
          const char c = text[i];
          const int e = std::isdigit(static_cast<unsigned char>(c)) ? TK_DecNumber
                                                                    : static_cast<unsigned char>(c);
          out.AppendChild(std::make_unique<SyntaxTreeLeaf>(Emit(e, text.substr(i, 1))));
        }
        return true;
      }
      case SyntaxKind::ClassMethodDeclaration: {
        // Qualifiers ("virtual", "static", ...) belong to the method header.
        const auto &n = node.as<ClassMethodDeclarationSyntax>();
        List(n.attributes, static_cast<int>(ListKind::ClassMethodDeclaration_attributes), out);
        pending_qualifiers_ = &n.qualifiers;
        Child(n.declaration.get(), out);
        pending_qualifiers_ = nullptr;
        return true;
      }
      case SyntaxKind::WildcardPortConnection: {
        const auto &n = node.as<WildcardPortConnectionSyntax>();
        List(n.attributes, static_cast<int>(ListKind::WildcardPortConnection_attributes), out);
        EmitDotStar(n.dot, n.star, out);
        return true;
      }
      case SyntaxKind::WildcardPattern: {
        const auto &n = node.as<WildcardPatternSyntax>();
        EmitDotStar(n.dot, n.star, out);
        return true;
      }
      case SyntaxKind::FunctionDeclaration:
      case SyntaxKind::TaskDeclaration: {
        const auto &n = node.as<FunctionDeclarationSyntax>();
        const slang::syntax::TokenList *qualifiers = pending_qualifiers_;
        pending_qualifiers_ = nullptr;
        List(n.attributes, static_cast<int>(ListKind::FunctionDeclaration_attributes), out);
        Group(GroupKind::kSubroutineHeader, out, [&](SyntaxTreeNode &g) {
          if (qualifiers) Toks(*qualifiers, g);
          Child(n.prototype.get(), g);
          Tok(n.semi, g);
        });
        List(n.items, static_cast<int>(ListKind::FunctionDeclaration_items), out);
        Tok(n.end, out);
        Child(n.endBlockName, out);
        return true;
      }
      case SyntaxKind::CovergroupDeclaration: {
        const auto &n = node.as<CovergroupDeclarationSyntax>();
        List(n.attributes, static_cast<int>(ListKind::CovergroupDeclaration_attributes), out);
        Group(GroupKind::kCovergroupHeader, out, [&](SyntaxTreeNode &g) {
          Tok(n.covergroup, g);
          Tok(n.extends, g);
          Tok(n.name, g);
          Child(n.portList, g);
          Child(n.event, g);
          Tok(n.semi, g);
        });
        List(n.members, static_cast<int>(ListKind::CovergroupDeclaration_members), out);
        Tok(n.endgroup, out);
        Child(n.endBlockName, out);
        return true;
      }
      case SyntaxKind::ClassDeclaration: {
        const auto &n = node.as<ClassDeclarationSyntax>();
        List(n.attributes, static_cast<int>(ListKind::ClassDeclaration_attributes), out);
        Group(GroupKind::kClassHeader, out, [&](SyntaxTreeNode &g) {
          Tok(n.virtualOrInterface, g);
          Tok(n.classKeyword, g);
          Child(n.finalSpecifier, g);
          Tok(n.name, g);
          Child(n.parameters, g);
          Child(n.extendsClause, g);
          Child(n.implementsClause, g);
          Tok(n.semi, g);
        });
        List(n.items, static_cast<int>(ListKind::ClassDeclaration_items), out);
        Tok(n.endClass, out);
        Child(n.endBlockName, out);
        return true;
      }
      case SyntaxKind::HierarchyInstantiation: {
        const auto &n = node.as<HierarchyInstantiationSyntax>();
        List(n.attributes, static_cast<int>(ListKind::HierarchyInstantiation_attributes), out);
        Group(GroupKind::kInstantiationType, out, [&](SyntaxTreeNode &g) {
          Tok(n.type, g);
          Child(n.parameters, g);
        });
        List(n.instances, static_cast<int>(ListKind::HierarchyInstantiation_instances), out);
        Tok(n.semi, out);
        return true;
      }
      case SyntaxKind::DataDeclaration: {
        const auto &n = node.as<DataDeclarationSyntax>();
        List(n.attributes, static_cast<int>(ListKind::DataDeclaration_attributes), out);
        Toks(n.modifiers, out);
        Group(GroupKind::kInstantiationType, out,
              [&](SyntaxTreeNode &g) { Child(n.type.get(), g); });
        List(n.declarators, static_cast<int>(ListKind::DataDeclaration_declarators), out);
        Tok(n.semi, out);
        return true;
      }
      default:
        return false;
    }
  }

  void SplitParameterDeclaration(const slang::syntax::ParameterDeclarationSyntax &n,
                                 SyntaxTreeNode &list) {
    using namespace slang::syntax;
    const int decl_tag = static_cast<int>(SyntaxKind::ParameterDeclaration);
    const int declarators_tag = static_cast<int>(ListKind::ParameterDeclaration_declarators);
    // First parameter: keyword, type, first declarator.
    Token first = n.getFirstToken();
    EmitTrivia(first, list);
    auto node = std::make_unique<SyntaxTreeNode>(decl_tag);
    analyzer_.origins_[node.get()] = &n;
    parent_first_.push_back(first);
    Tok(n.keyword, *node);
    Child(n.type.get(), *node);
    parent_first_.pop_back();
    auto declarators = std::make_unique<SyntaxTreeNode>(declarators_tag);
    parent_first_.push_back(Token());
    for (size_t i = 0; i < n.declarators.getChildCount(); ++i) {
      auto child = n.declarators.getChild(i);
      if (!child.isNode()) {
        // ',' between parameters: close the current one, start the next.
        node->AppendChild(std::move(declarators));
        list.AppendChild(std::move(node));
        Tok(child.token(), list);
        node = std::make_unique<SyntaxTreeNode>(decl_tag);
        declarators = std::make_unique<SyntaxTreeNode>(declarators_tag);
        continue;
      }
      Child(child.node(), *declarators);
    }
    parent_first_.pop_back();
    node->AppendChild(std::move(declarators));
    list.AppendChild(std::move(node));
  }

  // Converts the members of `syntax` into `node` (generated per syntax class,
  // see tools/gen_slang_cst.py). The callbacks below are used by the
  // generated code.
  void ConvertMembers(const SyntaxNode &node, SyntaxTreeNode &out) {
    using namespace slang::syntax;
    auto &b = *this;
    if (ConvertSpecial(node, out)) return;
#include "svfmt/slang-cst-gen.inc"
    // Kinds without a generated case: generic child iteration.
    for (size_t i = 0; i < node.getChildCount(); i++) {
      if (const SyntaxNode *child = node.childNode(i)) {
        Child(child, out);
      } else {
        Tok(node.childToken(i), out);
      }
    }
  }

 public:
  void Tok(const Token &tok, SyntaxTreeNode &out) {
    if (!tok) return;
    // Directives right before a list terminator (`endif before endmodule, or
    // a macro call that expanded to nothing before "end") belong to the end
    // of the preceding list, as in Verible.
    SyntaxTreeNode *target = &out;
    // The first token of a synthetic group (e.g. "end" in kEnd) looks at the
    // group's parent instead.
    SyntaxTreeNode *holder = &out;
    if (out.empty() && !groups_.empty() && groups_.back().first == &out) {
      holder = groups_.back().second;
    }
    if (!holder->empty() && holder->back() &&
        holder->back()->Kind() == verible::SymbolKind::kNode &&
        IsListTag(holder->back()->Tag().tag)) {
      target = &verible::SymbolCastToNode(*holder->back());
    }
    EmitTrivia(tok, *target);
    std::string_view text = TokenAtCursor(tok);
    if (text.empty()) return;  // macro expansion, include, missing
    int token_enum = MapToken(tok, previous_enum_);
    out.AppendChild(std::make_unique<SyntaxTreeLeaf>(Emit(token_enum, text)));
  }

  void Toks(const slang::syntax::TokenList &toks, SyntaxTreeNode &out) {
    for (const Token &tok : toks) Tok(tok, out);
  }

  void Child(const SyntaxNode *child, SyntaxTreeNode &out) {
    if (child == nullptr) return;
    // Hoist directives in front of a child up to the outermost node that
    // starts with the same token, so that e.g. `ifdef lands between module
    // items rather than inside the first item. List nodes never share a first
    // token with their elements here, so directives stay inside lists.
    Token first = child->getFirstToken();
    // "`A, `B: ..." : a macro that expands to the first case item expression
    // stays with the expressions; "`W'b0": the size stays with the literal.
    Token macro_name;
    // "`TYPE [7:0] x;", "`CELL u (...)", "`MODE property ...": a type or
    // keyword macro stays with its declaration.
    const bool declares = child->kind == SyntaxKind::DataDeclaration ||
                          child->kind == SyntaxKind::NetDeclaration ||
                          child->kind == SyntaxKind::HierarchyInstantiation ||
                          child->kind == SyntaxKind::ConcurrentAssertionMember ||
                          slang::syntax::ConcurrentAssertionStatementSyntax::isKind(child->kind);
    const bool keep_inside = (child->kind == SyntaxKind::StandardCaseItem || declares ||
                              slang::syntax::ExpressionSyntax::isKind(child->kind)) &&
                             first && first.location().buffer() != buffer_ &&
                             OnlyMacroUsageBefore(first, macro_name);
    if (keep_inside) {
      // What precedes the macro call ("`ifdef X", comments) stays outside.
      // Only once, at the case item: nested expressions share the macro.
      if ((child->kind == SyntaxKind::StandardCaseItem || declares) && macro_name &&
          macro_name.location().buffer() == buffer_) {
        EmitTrivia(macro_name, out);
      }
      first = Token();
    }
    if (first && !SameToken(first, parent_first_.back())) EmitTrivia(first, out);
    // "`MACRO(...);" where the macro expanded to nothing: slang sees an empty
    // item/statement. Keep the ';' with the macro call.
    // "foreach (x) `M(a);": the macro is the end of the previous statement.
    if (child->kind == SyntaxKind::EmptyStatement && !out.empty() && out.back() &&
        out.back()->Kind() == verible::SymbolKind::kNode &&
        out.back()->Tag().tag != static_cast<int>(SyntaxKind::MacroUsage)) {
      if (SyntaxTreeNode *usage = TrailingMacroUsage(verible::SymbolCastToNode(*out.back()))) {
        Token semi = child->getLastToken();
        EmitTrivia(semi, *usage);
        bool same_line = true;
        for (auto it = tokens_.rbegin(); it != tokens_.rend(); ++it) {
          const int e = it->token_enum();
          if (e == TK_NEWLINE) same_line = false;
          if (e != TK_NEWLINE && e != TK_SPACE && e != TK_EOL_COMMENT && e != TK_COMMENT_BLOCK) break;
        }
        if (std::string_view text = TokenAtCursor(semi); !text.empty() && same_line) {
          usage->AppendChild(std::make_unique<SyntaxTreeLeaf>(Emit(';', text)));
          return;
        }
      }
    }
    if ((child->kind == SyntaxKind::EmptyMember || child->kind == SyntaxKind::EmptyStatement) &&
        !out.empty() && out.back() && out.back()->Kind() == verible::SymbolKind::kNode &&
        out.back()->Tag().tag == static_cast<int>(SyntaxKind::MacroUsage)) {
      Token semi = child->getLastToken();
      EmitTrivia(semi, out);
      bool same_line = true;
      for (auto it = tokens_.rbegin(); it != tokens_.rend(); ++it) {
        const int e = it->token_enum();
        if (e == TK_NEWLINE) same_line = false;
        if (e != TK_NEWLINE && e != TK_SPACE && e != TK_EOL_COMMENT && e != TK_COMMENT_BLOCK) break;
      }
      if (std::string_view text = TokenAtCursor(semi); !text.empty() && same_line &&
          out.back()->Tag().tag == static_cast<int>(SyntaxKind::MacroUsage)) {
        verible::SymbolCastToNode(*out.back())
            .AppendChild(std::make_unique<SyntaxTreeLeaf>(Emit(';', text)));
        return;
      }
    }
    auto converted = std::make_unique<SyntaxTreeNode>(static_cast<int>(child->kind));
    analyzer_.origins_[converted.get()] = child;
    parent_first_.push_back(first);
    ConvertMembers(*child, *converted);
    parent_first_.pop_back();
    // A node whose tokens all come from a macro expansion has no leaves
    // (only empty lists); drop it. If only its ';' is in this file
    // ("`DECL(a);" where the macro expands to a declaration), the ';' goes
    // with the macro call.
    if (!HasLeaf(*converted)) return;
    if (SymbolPtr *only = SoleLeafIn(*converted); only != nullptr &&
        (*only)->Tag().tag == ';' && !NewlineBeforeLastToken() && !out.empty() && out.back() &&
        out.back()->Kind() == verible::SymbolKind::kNode &&
        out.back()->Tag().tag == static_cast<int>(SyntaxKind::MacroUsage)) {
      verible::SymbolCastToNode(*out.back()).AppendChild(std::move(*only));
      return;
    }
    out.AppendChild(std::move(converted));
  }

  // Whether the only directives in front of `tok` are macro calls (other
  // directives are hoisted out of the node as usual). Sets `name` to the
  // first macro call's name token.
  bool OnlyMacroUsageBefore(const Token &tok, Token &name) const {
    for (const slang::parsing::Trivia &t : tok.trivia()) {
      if (t.kind == slang::parsing::TriviaKind::DisabledText) return false;
      if (t.kind != slang::parsing::TriviaKind::Directive) continue;
      if (t.syntax()->kind != SyntaxKind::MacroUsage) return false;
      if (!name) name = t.syntax()->getFirstToken();
    }
    return true;
  }

  // The macro call that ends `node` ("foreach (x) `M(a)"), if any.
  static SyntaxTreeNode *TrailingMacroUsage(SyntaxTreeNode &node) {
    SyntaxTreeNode *n = &node;
    while (true) {
      if (n->Tag().tag == static_cast<int>(SyntaxKind::MacroUsage)) return n;
      verible::Symbol *last = nullptr;
      for (auto &c : n->mutable_children()) {
        if (c && HasLeaf(*c)) last = c.get();
      }
      if (last == nullptr || last->Kind() != verible::SymbolKind::kNode) return nullptr;
      n = &verible::SymbolCastToNode(*last);
    }
  }

  // Whether a newline separates the last emitted token from the previous
  // non-whitespace, non-comment token.
  bool NewlineBeforeLastToken() const {
    if (tokens_.empty()) return false;
    for (auto it = tokens_.rbegin() + 1; it != tokens_.rend(); ++it) {
      const int e = it->token_enum();
      if (e == TK_NEWLINE) return true;
      if (e != TK_SPACE && e != TK_EOL_COMMENT && e != TK_COMMENT_BLOCK) return false;
    }
    return false;
  }

  // Returns the slot holding the only leaf under `symbol`, or nullptr if
  // there are none or several.
  static SymbolPtr *SoleLeafIn(SyntaxTreeNode &node) {
    SymbolPtr *found = nullptr;
    for (auto &c : node.mutable_children()) {
      if (!c || !HasLeaf(*c)) continue;
      if (found != nullptr) return nullptr;
      found = c->Kind() == verible::SymbolKind::kLeaf
                  ? &c
                  : SoleLeafIn(verible::SymbolCastToNode(*c));
      if (found == nullptr) return nullptr;
    }
    return found;
  }

  static bool HasLeaf(const verible::Symbol &symbol) {
    if (symbol.Kind() == verible::SymbolKind::kLeaf) return true;
    for (const auto &c : verible::SymbolCastToNode(symbol).children()) {
      if (c && HasLeaf(*c)) return true;
    }
    return false;
  }

  template <typename ListT>
  void List(const ListT &list, int list_tag, SyntaxTreeNode &out) {
    // Empty lists are kept (Verible keeps empty item lists too): comments in
    // an empty body are indented relative to the list.
    if (ListKindName(list_tag).ends_with("_attributes")) {
      // Verible lexes "(* ... *)" as one TK_ATTRIBUTE token that, like a
      // comment, is not part of the syntax tree.
      for (size_t i = 0; i < list.getChildCount(); ++i) {
        auto child = list.getChild(i);
        if (!child.isNode()) continue;
        const SyntaxNode &attr = *child.node();
        Token first = attr.getFirstToken();
        Token last = attr.getLastToken();
        EmitTrivia(first, out);
        if (TokenAtCursor(first).empty() || last.location().buffer() != buffer_) continue;
        const size_t begin = static_cast<size_t>(Offset(cursor_));
        const size_t end = last.location().offset() + base_offset_ + last.rawText().size();
        Emit(TK_ATTRIBUTE, contents_.substr(begin, end - begin));
      }
      return;
    }
    if (list.empty()) {
      {
        out.AppendChild(std::make_unique<SyntaxTreeNode>(list_tag));
      }
      return;
    }
    auto converted = std::make_unique<SyntaxTreeNode>(list_tag);
    parent_first_.push_back(Token());
    for (size_t i = 0; i < list.getChildCount(); i++) {
      auto child = list.getChild(i);
      if (child.isNode()) {
        Child(child.node(), *converted);
      } else {
        Tok(child.token(), *converted);
      }
    }
    parent_first_.pop_back();
    if (!converted->empty()) out.AppendChild(std::move(converted));
  }

 private:
  std::string DebugTail() const {
    std::string out = "\n  last tokens:";
    size_t start = tokens_.size() > 6 ? tokens_.size() - 6 : 0;
    for (size_t i = start; i < tokens_.size(); i++) {
      absl::StrAppend(&out, " ", TokenEnumName(tokens_[i].token_enum()), "'",
                      tokens_[i].text(), "'");
    }
    return out;
  }

  static bool SameToken(const Token &a, const Token &b) {
    return a.kind == b.kind && a.location() == b.location() &&
           a.rawText().data() == b.rawText().data();
  }

  SlangAnalyzer &analyzer_;
  std::string_view contents_;
  std::string_view match_text_;
  std::vector<CondDirective> directives_;
  size_t next_directive_ = 0;
  slang::BufferID buffer_;
  // Added to token offsets when converting a sub-parse (macro arguments).
  size_t base_offset_ = 0;
  const char *cursor_;
  // Text ranges of the other source buffers (lazily built).
  mutable std::vector<std::pair<const char *, const char *>> foreign_;
  int previous_enum_ = 0;
  verible::TokenSequence tokens_;
  absl::Status status_;
  // Synthetic groups being filled, with their parent.
  std::vector<std::pair<SyntaxTreeNode *, SyntaxTreeNode *>> groups_;
  // Qualifiers of a ClassMethodDeclaration, moved into its subroutine header.
  const slang::syntax::TokenList *pending_qualifiers_ = nullptr;
  // First token of each node being converted (null Token for roots/lists).
  std::vector<Token> parent_first_;
};

SlangAnalyzer::~SlangAnalyzer() = default;

// Verible's excerpt parse modes ("// verilog_syntax: parse-as-module-body"):
// the text is wrapped in a prolog/epilog and the CST is re-rooted at the list
// that holds the excerpt.
struct ParseMode {
  std::string_view name;
  std::string_view prolog;
  std::string_view epilog;
  ListKind body;
};

namespace {

constexpr ParseMode kParseModes[] = {
    {"parse-as-module-body", "module __svfmt_excerpt__;", "\nendmodule\n",
     ListKind::ModuleDeclaration_members},
    {"parse-as-class-body", "class __svfmt_excerpt__;", "\nendclass\n",
     ListKind::ClassDeclaration_items},
    {"parse-as-package-body", "package __svfmt_excerpt__;", "\nendpackage\n",
     ListKind::ModuleDeclaration_members},
    {"parse-as-statements", "function void __svfmt_excerpt__();", "\nendfunction\n",
     ListKind::FunctionDeclaration_items},
    // Internal (no directive can name it): a module body that holds task
    // calls, parsed as statements and reshaped into module items.
    {"(module-body-statements)", "function void __svfmt_excerpt__();", "\nendfunction\n",
     ListKind::FunctionDeclaration_items},
};

const ParseMode *FindParseMode(std::string_view name) {
  for (const auto &mode : kParseModes) {
    if (mode.name == name) return &mode;
  }
  return nullptr;
}

// Returns the mode named by a "verilog_syntax: <mode>" directive in the
// comments that precede the first code token, as Verible does.
std::string_view ScanParseModeDirective(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    if (std::isspace(static_cast<unsigned char>(text[i]))) {
      ++i;
      continue;
    }
    std::string_view comment;
    if (text.substr(i).starts_with("//")) {
      size_t end = text.find('\n', i);
      if (end == std::string_view::npos) end = text.size();
      comment = text.substr(i, end - i);
      i = end;
    } else if (text.substr(i).starts_with("/*")) {
      size_t end = text.find("*/", i);
      end = end == std::string_view::npos ? text.size() : end + 2;
      comment = text.substr(i, end - i);
      i = end;
    } else {
      break;
    }
    size_t pos = comment.find("verilog_syntax:");
    if (pos == std::string_view::npos) continue;
    std::string_view rest = comment.substr(pos + 15);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    size_t len = 0;
    while (len < rest.size() && (std::isalnum(static_cast<unsigned char>(rest[len])) ||
                                 rest[len] == '-')) {
      ++len;
    }
    return rest.substr(0, len);
  }
  return {};
}

// Turns statements parsed from a module body back into module items, the way
// Verible's grammar sees them: the list becomes a module member list, and a
// task call statement "$f(...);" becomes the call with its ';' inside.
void ReshapeAsModuleItems(SymbolPtr &tree) {
  auto items = std::make_unique<SyntaxTreeNode>(
      static_cast<int>(ListKind::ModuleDeclaration_members));
  for (auto &child : verible::SymbolCastToNode(*tree).mutable_children()) {
    if (child && child->Tag().tag == static_cast<int>(SyntaxKind::ExpressionStatement)) {
      auto &statement = verible::SymbolCastToNode(*child);
      SymbolPtr *call = nullptr;
      SymbolPtr *semi = nullptr;
      bool other = false;
      for (auto &part : statement.mutable_children()) {
        if (!part) continue;
        if (part->Tag().tag == static_cast<int>(SyntaxKind::InvocationExpression) && !call) {
          call = &part;
        } else if (part->Kind() == verible::SymbolKind::kLeaf && call && !semi) {
          semi = &part;
        } else {
          other = true;
        }
      }
      if (call && semi && !other) {
        verible::SymbolCastToNode(**call).AppendChild(std::move(*semi));
        child = std::move(*call);
      }
    }
    items->AppendChild(std::move(child));
  }
  tree = std::move(items);
}

// Finds the first node (pre-order) with the given tag.
SymbolPtr *FindNodeWithTag(SymbolPtr &root, int tag) {
  if (!root || root->Kind() != verible::SymbolKind::kNode) return nullptr;
  if (root->Tag().tag == tag) return &root;
  auto &node = verible::SymbolCastToNode(*root);
  for (auto &child : node.mutable_children()) {
    if (auto *found = FindNodeWithTag(child, tag)) return found;
  }
  return nullptr;
}

bool IsIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// Whether the '{' or ';' at `pos` is inside the braces of a struct or union
// ("struct packed {", "union soft {").
bool InStructBody(std::string_view text, size_t pos) {
  int depth = 0;
  size_t brace = pos;
  for (;; --brace) {
    const char c = text[brace];
    if (c == '}') ++depth;
    if (c == '{' && depth-- == 0) break;
    if (brace == 0) return false;
  }
  // The words before the brace, skipping qualifiers.
  size_t end = brace;
  while (true) {
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    size_t begin = end;
    while (begin > 0 && IsIdentChar(text[begin - 1])) --begin;
    const std::string_view word = text.substr(begin, end - begin);
    if (word == "struct" || word == "union") return true;
    if (word != "packed" && word != "signed" && word != "unsigned" && word != "soft" &&
        word != "tagged") {
      return false;
    }
    end = begin;
  }
}

// Whether `pos` is inside a constraint block ("constraint c { ... }"),
// possibly nested in "foreach (...) {" / "if (...) {" braces.
bool InConstraintBody(std::string_view text, size_t pos) {
  constexpr size_t kMaxScan = 20000;  // keep the backward scans bounded
  const size_t floor = pos > kMaxScan ? pos - kMaxScan : 0;
  int depth = 0;
  for (size_t i = pos; i-- > floor;) {
    const char c = text[i];
    if (c == '}') {
      ++depth;
    } else if (c == '{' && depth-- == 0) {
      // The text of the construct that opens this brace.
      size_t start = i;
      while (start > floor && text[start - 1] != ';' && text[start - 1] != '{' &&
             text[start - 1] != '}') {
        --start;
      }
      const std::string_view head = text.substr(start, i - start);
      for (size_t at = head.find("constraint"); at != std::string_view::npos;
           at = head.find("constraint", at + 1)) {
        const bool left = at == 0 || !IsIdentChar(head[at - 1]);
        const bool right = at + 10 >= head.size() || !IsIdentChar(head[at + 10]);
        if (left && right) return true;
      }
      depth = 0;  // look further out (foreach / if / implication braces)
    }
  }
  return false;
}

// Whether `pos` is in a covergroup body: the nearest keyword before it is
// "covergroup", not "endgroup".
bool InCovergroup(std::string_view text, size_t pos) {
  const auto last_word = [&](std::string_view word) -> size_t {
    for (size_t at = text.rfind(word, pos); at != std::string_view::npos;
         at = at == 0 ? std::string_view::npos : text.rfind(word, at - 1)) {
      const bool left = at == 0 || !IsIdentChar(text[at - 1]);
      const bool right = at + word.size() >= text.size() || !IsIdentChar(text[at + word.size()]);
      // Not in a "// ..." comment.
      const size_t line = text.rfind('\n', at);
      const size_t line_start = line == std::string_view::npos ? 0 : line + 1;
      const bool commented =
          text.substr(line_start, at - line_start).find("//") != std::string_view::npos;
      if (left && right && !commented) return at;
    }
    return std::string_view::npos;
  };
  const size_t open = last_word("covergroup");
  if (open == std::string_view::npos) return false;
  const size_t close = last_word("endgroup");
  return close == std::string_view::npos || close < open;
}

// End of the code before `pos`: skips whitespace, "// ..." line comments and
// "/* ... */" block comments (a '/' in "////" is not a division).
size_t PreviousCodeEnd(std::string_view text, size_t pos) {
  size_t p = pos;
  while (true) {
    while (p > 0 && std::isspace(static_cast<unsigned char>(text[p - 1]))) --p;
    if (p >= 2 && text[p - 1] == '/' && text[p - 2] == '*') {
      const size_t open = text.rfind("/*", p - 2);
      if (open == std::string_view::npos) return p;
      p = open;
      continue;
    }
    // A "//" on the same line before p makes the rest of that line a comment.
    size_t line_start = text.rfind('\n', p == 0 ? 0 : p - 1);
    line_start = line_start == std::string_view::npos ? 0 : line_start + 1;
    const size_t slashes = text.substr(line_start, p - line_start).find("//");
    if (slashes == std::string_view::npos) return p;
    p = line_start + slashes;
  }
}

// Start of the code after `pos`: skips whitespace and comments.
size_t NextCodeStart(std::string_view text, size_t pos) {
  while (pos < text.size()) {
    if (std::isspace(static_cast<unsigned char>(text[pos]))) {
      ++pos;
    } else if (text.substr(pos, 2) == "//") {
      pos = text.find('\n', pos);
      if (pos == std::string_view::npos) return text.size();
    } else if (text.substr(pos, 2) == "/*") {
      pos = text.find("*/", pos + 2);
      if (pos == std::string_view::npos) return text.size();
      pos += 2;
    } else {
      break;
    }
  }
  return pos;
}

// Whether the code after an argument-less macro ending at `end` declares a
// name with it as the type: "`T name (", "`T name;", "`T name [", ...
// Decided from the code, not from line breaks (formatting moves those).
bool DeclaresName(std::string_view text, size_t end) {
  size_t p = NextCodeStart(text, end);
  // "`T [7:0] name;", "`T #(8) name (": a type with dimensions/parameters.
  if (p < text.size() && (text[p] == '[' || text[p] == '#')) return true;
  if (p >= text.size() || !(std::isalpha(static_cast<unsigned char>(text[p])) ||
                            text[p] == '_' || text[p] == '\\')) {
    return false;
  }
  if (text[p] == '\\') {
    while (p < text.size() && !std::isspace(static_cast<unsigned char>(text[p]))) ++p;
  } else {
    while (p < text.size() && IsIdentChar(text[p])) ++p;
  }
  p = NextCodeStart(text, p);
  return p < text.size() && std::strchr("(;,[=#", text[p]) != nullptr;
}

// Builds stub definitions for macros that slang could not find (typically
// defined in an `include that is not available). A macro used with
// arguments in statement/item position expands to nothing, or to an empty
// statement ';' if some use is not followed by ';' (e.g. "else `M(x)",
// where the macro supplies the whole statement); one used in an expression
// position, or without arguments (constants, types), expands to a
// placeholder identifier ("0" right after a base specifier, as in
// "32'h`ADDR"). With `items_empty`, a macro used without arguments outside
// expressions (e.g. "`uvm_object_new" in a class body) expands to nothing
// instead. Expansions never reach the output: only the macro call text is
// formatted.
// Whether the code ending at `p` heads a statement body: "foreach (...)",
// "for (...)", "while (...)", "repeat (...)", "else", "forever". Not "if (...)"
// or "do": "if (c) `M(a); else" needs the ';' to end the body.
bool ControlsStatement(std::string_view text, size_t p) {
  const auto word_before = [&](size_t end) {
    size_t w = end;
    while (w > 0 && IsIdentChar(text[w - 1])) --w;
    return text.substr(w, end - w);
  };
  if (IsIdentChar(text[p - 1])) {
    const std::string_view word = word_before(p);
    // Not the directive "`else".
    const size_t w = p - word.size();
    if (w > 0 && text[w - 1] == '`') return false;
    return word == "else" || word == "forever";
  }
  if (text[p - 1] != ')') return false;
  int depth = 0;
  for (size_t i = p; i-- > 0;) {
    if (text[i] == ')') ++depth;
    else if (text[i] == '(' && --depth == 0) {
      const size_t q = PreviousCodeEnd(text, i);
      const std::string_view word = word_before(q);
      return word == "foreach" || word == "for" || word == "while" || word == "repeat";
    }
  }
  return false;
}

// Whether the ')' ending at `p` closes "if (...)".
bool IfHeaderEnd(std::string_view text, size_t p) {
  int depth = 0;
  for (size_t i = p; i-- > 0;) {
    if (text[i] == ')') ++depth;
    else if (text[i] == '(' && --depth == 0) {
      const size_t q = PreviousCodeEnd(text, i);
      return q >= 2 && text.substr(q - 2, 2) == "if" && (q == 2 || !IsIdentChar(text[q - 3]));
    }
  }
  return false;
}

// Whether `pos` is directly inside "@( ... )".
bool InEventControl(std::string_view text, size_t pos) {
  int depth = 0;
  for (size_t i = pos; i-- > 0;) {
    if (text[i] == ')') ++depth;
    else if (text[i] == '(' && depth-- == 0) {
      const size_t q = PreviousCodeEnd(text, i);
      return q > 0 && text[q - 1] == '@';
    } else if (text[i] == ';') {
      return false;
    }
  }
  return false;
}

std::vector<std::string> StubDefinitions(std::string_view text,
                                         const std::vector<size_t> &usage_offsets,
                                         bool items_empty) {
  struct Info {
    int arity = -1;  // -1: used without parentheses
    bool expression = false;
    bool no_semicolon = false;  // some use is not followed by ';'
    bool literal = false;       // used as the digits of a based literal
    bool member = false;        // used as a struct/union member
    bool constraint = false;    // used in a constraint block
    bool not_alone = false;     // some argument-less use shares its line
    bool operand = false;       // some use is followed by an operator
    bool size = false;          // used as the size of a based literal
    bool assertion = false;     // followed by "property" / "sequence"
    bool body = false;          // the body of a loop / else
    bool if_body = false;       // "if (c) `M(a); else": the ';' ends the body
    bool trailing = false;      // follows a complete list element
  };
  std::map<std::string, Info> infos;
  // Ends of macro uses that complete an operand, in source order.
  std::set<size_t> operand_ends;
  std::vector<size_t> offsets = usage_offsets;
  std::sort(offsets.begin(), offsets.end());
  for (size_t offset : offsets) {
    if (offset >= text.size() || text[offset] != '`') continue;
    bool after_operand = false;
    size_t end = offset + 1;
    while (end < text.size() && IsIdentChar(text[end])) ++end;
    std::string name(text.substr(offset + 1, end - offset - 1));
    if (name.empty()) continue;
    Info &info = infos[name];
    // Arity: count top-level commas inside the argument list.
    // slang accepts whitespace between a macro name and its arguments.
    while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) ++end;
    size_t call_end = end;
    if (end < text.size() && text[end] == '(') {
      int depth = 0, commas = 0;
      bool in_string = false;
      for (size_t i = end; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
          if (c == '\\') ++i;
          else if (c == '"') in_string = false;
          continue;
        }
        if (c == '"') in_string = true;
        else if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') {
          if (--depth == 0) {
            call_end = i + 1;
            break;
          }
        } else if (c == ',' && depth == 1) {
          ++commas;
        }
      }
      info.arity = std::max(info.arity, commas + 1);
      size_t q = call_end;
      while (q < text.size() && std::isspace(static_cast<unsigned char>(text[q]))) ++q;
      if (q >= text.size() || text[q] != ';') info.no_semicolon = true;
      // "`M(x) < y": an operand of an operator.
      // "`M(x)\n -> ev;" is an event trigger statement, not an operator.
      if (q < text.size() && std::strchr("<>=!+-*/%&|^?.[", text[q]) &&
          text.substr(q, 2) != "//" && text.substr(q, 2) != "/*" && text.substr(q, 2) != "->") {
        info.expression = true;
        info.operand = true;
      }
    }
    // Context: the previous non-space character outside comments.
    size_t p = PreviousCodeEnd(text, offset);
    // An argument-less macro that starts an item: after ';', ')', '{', '}'
    // or a begin/end keyword, and nothing else on its line. Line breaks
    // alone must not decide it: formatting may move the macro to its own line.
    if (call_end == end) {
      bool at_item_start = p == 0 || std::strchr(";){}", text[p - 1]) != nullptr;
      if (!at_item_start && IsIdentChar(text[p - 1])) {
        size_t w = p;
        while (w > 0 && IsIdentChar(text[w - 1])) --w;
        const std::string_view word = text.substr(w, p - w);
        // ... or another macro ("`uvm_object_utils_end" then "`uvm_object_new").
        at_item_start = word == "begin" || word.starts_with("end") ||
                        (w > 0 && text[w - 1] == '`');
      }
      // "`M;" is a statement: an identifier keeps "assert (x) `M; else ...".
      const size_t next = NextCodeStart(text, end);
      // "`A, `B:" (case items) are values.
      if (!at_item_start || DeclaresName(text, end) ||
          (next < text.size() && std::strchr(";,:", text[next]))) {
        info.not_alone = true;
      }
    }
    // A whole constraint item ("{ `M(x) ... }"), not part of an expression.
    if (p > 0 && std::strchr("{;}", text[p - 1]) && InConstraintBody(text, offset)) {
      info.constraint = true;
    }
    if (p > 0) {
      const char c = text[p - 1];
      static constexpr std::string_view kExprChars = "=(,[{?+-*/%&|^~!<>#'";
      if (kExprChars.find(c) != std::string_view::npos) info.expression = true;
      if (p >= 6 && text.substr(p - 6, 6) == "return") info.expression = true;
      // "[0:`W-1]": after ':' an argument-less macro is a value; one with
      // arguments may be a statement ("1: `uvm_info(...)").
      if (c == ':' && (end >= text.size() || text[end] != '(')) info.expression = true;
      // "[i+:`M(a, b)]", "[`M(a, b):0]".
      if (c == ':' && p >= 2 && (text[p - 2] == '+' || text[p - 2] == '-')) info.expression = true;
      const size_t after = NextCodeStart(text, call_end);
      if (call_end != end && after < text.size() && text[after] == ']') info.expression = true;
      if ((c == '{' || c == ';') && InStructBody(text, p - 1)) info.member = true;
      // "parameter `M(x)": the macro is the declared name.
      if (IsIdentChar(c)) {
        size_t w = p;
        while (w > 0 && IsIdentChar(text[w - 1])) --w;
        const std::string_view word = text.substr(w, p - w);
        if (word == "parameter" || word == "localparam") info.expression = true;
        // "= e_cfg `M(x) `N(y) , ...": the macro adds list elements after a
        // complete operand, so it expands to nothing here.
        const bool keyword = word == "begin" || word == "else" || word.starts_with("end");
        if (!keyword && (w == 0 || text[w - 1] != '`')) after_operand = true;
        // "@(posedge clk `OR_RST)": an argument-less macro that adds to an
        // event list after a plain name.
        static const std::set<std::string_view> kEdges = {"posedge", "negedge", "edge", "or"};
        if (call_end == end && !keyword && !kEdges.count(word) && (w == 0 || text[w - 1] != '`') &&
            InEventControl(text, offset)) {
          const size_t next = NextCodeStart(text, end);
          if (next < text.size() && text[next] == ')') info.trailing = true;
        }
      }
      if (operand_ends.count(p)) after_operand = true;
      // "- (a + b) `M(x)": a parenthesized operand.
      if (c == ')') {
        int depth = 0;
        for (size_t i = p; i-- > 0;) {
          if (text[i] == ')') ++depth;
          else if (text[i] == '(' && --depth == 0) {
            const size_t q = PreviousCodeEnd(text, i);
            if (q > 0 && kExprChars.find(text[q - 1]) != std::string_view::npos) after_operand = true;
            break;
          }
        }
      }
      // A value ("= `CFG", "= `CLOG2(n)") ends an operand too.
      if (kExprChars.find(c) != std::string_view::npos) operand_ends.insert(call_end);
    }
    // "foreach (x[i]) `M(a);": the body of a control statement must be a
    // statement, so the macro is one.
    if (p > 0 && ControlsStatement(text, p)) info.body = true;
    if (p > 0 && text[p - 1] == ')' && IfHeaderEnd(text, p)) {
      const size_t semi = NextCodeStart(text, call_end);
      if (semi < text.size() && text[semi] == ';') {
        const size_t after = NextCodeStart(text, semi + 1);
        if (text.substr(after, 4) == "else" &&
            (after + 4 >= text.size() || !IsIdentChar(text[after + 4]))) {
          info.if_body = true;
        }
      }
    }
    // A covergroup item ("`SEEN(a, b)" between coverpoints): ';' is not a
    // covergroup member, nothing is.
    if (call_end != end && (p == 0 || std::strchr(";)}{", text[p - 1])) &&
        InCovergroup(text, offset)) {
      info.trailing = true;
    }
    if (after_operand && call_end != end) {
      const size_t next = NextCodeStart(text, call_end);
      if (next < text.size() && std::strchr(",)`", text[next])) {
        info.trailing = true;
        operand_ends.insert(call_end);
      }
    }
    // "`MODE property (...)": the macro is assert / assume / cover.
    if (call_end == end) {
      const size_t next = NextCodeStart(text, end);
      for (std::string_view kw : {"property", "sequence"}) {
        if (text.substr(next, kw.size()) == kw &&
            (next + kw.size() >= text.size() || !IsIdentChar(text[next + kw.size()]))) {
          info.assertion = true;
        }
      }
    }
    // "`W'h0": the size of a based literal must be a number.
    if (call_end == end && end + 1 < text.size() && text[end] == '\'' &&
        std::strchr("sShHdDbBoO", text[end + 1])) {
      info.size = true;
    }
    if (offset >= 2 && p == offset && std::strchr("hHdDbBoO", text[offset - 1]) &&
        (text[offset - 2] == '\'' ||
         (offset >= 3 && std::strchr("sS", text[offset - 2]) && text[offset - 3] == '\''))) {
      info.literal = true;
    }
  }
  std::vector<std::string> defs;
  for (const auto &[name, info] : infos) {
    std::string def = "`define " + name;
    if (info.arity >= 0) {
      def += '(';
      for (int i = 0; i < info.arity; ++i) {
        if (i) def += ',';
        def += "a" + std::to_string(i) + "=";  // empty default: uses may pass fewer
      }
      def += ')';
    }
    if (info.assertion) def += " assert";
    else if (info.body && !info.if_body && !info.expression && !info.member) def += " begin end";
    else if (info.member) def += info.no_semicolon ? " int __svfmt_macro__;" : " int __svfmt_macro__";
    else if (info.literal || info.size) def += info.size ? " 8" : " 0";
    // A constraint item: a constant expression is a valid one.
    else if (info.constraint && !info.operand) def += info.no_semicolon ? " 1;" : " 1";
    else if (info.trailing && !info.expression) {}
    // An argument-less macro alone on its line is an item ("`uvm_object_utils_end").
    else if (info.arity < 0 && !info.expression && (items_empty || !info.not_alone)) {}
    else if (info.arity < 0 || info.expression) def += " __svfmt_macro__";
    else if (info.no_semicolon) def += " ;";
    defs.push_back(std::move(def));
  }
  return defs;
}

}  // namespace

namespace {

// Every `include "name" is replaced by a placeholder buffer registered under
// the path slang would read it from. The formatter works on one file: include
// files are never read, so their contents (and their macros) are unknown.
// Returns (absolute path, text) pairs with `content` as the text.
std::vector<std::pair<std::string, std::string>> IncludePlaceholders(
    std::string_view text, std::string_view filename, std::string_view content) {
  namespace fs = std::filesystem;
  std::vector<std::pair<std::string, std::string>> out;
  std::set<std::string> seen;
  const fs::path dir = fs::path(filename).parent_path();
  for (size_t pos = text.find("`include"); pos != std::string_view::npos;
       pos = text.find("`include", pos)) {
    size_t p = pos + 8;
    pos = p;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    if (p >= text.size() || text[p] != '"') continue;
    const size_t close = text.find('"', p + 1);
    if (close == std::string_view::npos) continue;
    const std::string_view name_text = text.substr(p + 1, close - p - 1);
    if (name_text.empty() || name_text.find('\n') != std::string_view::npos) continue;
    const fs::path local = dir / std::string(name_text);
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(local, ec);
    std::string path = ec ? local.string() : canonical.string();
    if (seen.insert(path).second) out.emplace_back(std::move(path), std::string(content));
  }
  return out;
}

}  // namespace

// Replaces each tab with a space, except inside comments, string literals and
// "verilog_format: off" regions (kept verbatim). Only whitespace changes.
std::string TabsToSpaces(std::string_view text) {
  std::string out(text);
  size_t i = 0;
  const size_t n = text.size();
  bool off = false;
  while (i < n) {
    const char c = text[i];
    if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      const size_t begin = i;
      while (i < n && text[i] != '\n') ++i;
      const std::string_view comment = text.substr(begin, i - begin);
      if (comment.find("verilog_format: off") != std::string_view::npos) off = true;
      if (comment.find("verilog_format: on") != std::string_view::npos) off = false;
    } else if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      const size_t e = text.find("*/", i + 2);
      const size_t end = e == std::string_view::npos ? n : e + 2;
      const std::string_view comment = text.substr(i, end - i);
      if (comment.find("verilog_format: off") != std::string_view::npos) off = true;
      if (comment.find("verilog_format: on") != std::string_view::npos) off = false;
      i = end;
    } else if (c == '"') {
      ++i;
      while (i < n && text[i] != '"' && text[i] != '\n') i += text[i] == '\\' ? 2 : 1;
      ++i;
    } else {
      if (c == '\t' && !off) out[i] = ' ';
      ++i;
    }
  }
  return out;
}

absl::StatusOr<std::unique_ptr<SlangAnalyzer>> SlangAnalyzer::Analyze(
    std::string_view text, std::string_view filename, const AnalyzerOptions &options) {
  // Tabs are formatted as spaces (outside comments and strings).
  const std::string spaced = TabsToSpaces(text);
  text = spaced;
  const ParseMode *mode = FindParseMode(ScanParseModeDirective(text));
  // Prefer parsing every `ifdef branch (like Verible); fall back to normal
  // preprocessing, which leaves inactive branches verbatim.
  const bool has_conditionals = !ScanConditionalDirectives(text).empty();
  // Include files are placeholders from the start (see IncludePlaceholders).
  AnalyzerOptions base = options;
  base.placeholder_includes = IncludePlaceholders(text, filename, "");
  auto attempt = [&](const ParseMode *m) {
    auto r = AnalyzeWithMode(text, filename, base, m, /*all_branches=*/true);
    if (!r.ok() && has_conditionals) {
      r = AnalyzeWithMode(text, filename, base, m, /*all_branches=*/false);
    }
    if (!r.ok()) {
      // An empty include supplies items; a port needs a ',' after it, or
      // none if it is the last port: try the forms in turn.
      // Ports and parameters: an empty include in a list leaves a stray ','.
      for (const char *content :
           {"input __svfmt_include__,", "input __svfmt_include__",
            "parameter __svfmt_include__ = 0", "parameter __svfmt_include__ = 0,"}) {
        AnalyzerOptions with = base;
        with.placeholder_includes = IncludePlaceholders(text, filename, content);
        auto retry = AnalyzeWithMode(text, filename, with, m, /*all_branches=*/true);
        if (retry.ok()) return retry;
      }
    }
    return r;
  };
  const ParseMode *module_body = &kParseModes[0];
  const ParseMode *statements = &kParseModes[4];
  auto result = attempt(mode);
  if (!result.ok() && mode == nullptr && absl::IsInvalidArgument(result.status())) {
    // Like Verible's automatic mode: retry module items (e.g. a bare
    // "assign" or "always") as a module body.
    auto retry = attempt(module_body);
    if (retry.ok()) return retry;
    mode = module_body;
  }
  if (!result.ok() && mode == module_body && absl::IsInvalidArgument(result.status())) {
    // Verible's module items include task calls ("$display(...);"), which
    // slang only accepts as statements.
    auto retry = attempt(statements);
    if (retry.ok()) return retry;
  }
  return result;
}

absl::StatusOr<std::unique_ptr<SlangAnalyzer>> SlangAnalyzer::AnalyzeWithMode(
    std::string_view text, std::string_view filename, const AnalyzerOptions &options,
    const ParseMode *mode, bool all_branches) {
  std::unique_ptr<SlangAnalyzer> analyzer(new SlangAnalyzer);
  analyzer->source_manager_ = std::make_unique<slang::SourceManager>();
  auto &sm = *analyzer->source_manager_;

  slang::parsing::PreprocessorOptions pp_options;
  pp_options.predefines = options.defines;
  for (const auto &dir : options.include_dirs) {
    pp_options.additionalIncludePaths.emplace_back(dir);
  }
  // SV-2023 is a superset for formatting purposes (e.g. "union soft").
  pp_options.languageVersion = slang::LanguageVersion::v1800_2023;
  slang::parsing::LexerOptions lex_options;
  lex_options.languageVersion = slang::LanguageVersion::v1800_2023;
  slang::parsing::ParserOptions parse_options;
  parse_options.languageVersion = slang::LanguageVersion::v1800_2023;
  slang::Bag bag;
  bag.set(pp_options);
  bag.set(lex_options);
  bag.set(parse_options);

  // In all-branches mode, conditional directives are replaced by spaces (same
  // length, so offsets are unchanged) and every branch is parsed; the
  // Builder re-inserts the directives from the original text.
  std::vector<CondDirective> directives;
  analyzer->original_text_ = std::string(text);
  std::string parse_text(text);
  if (all_branches) {
    directives = ScanConditionalDirectives(text);
    for (const auto &d : directives) {
      std::fill(parse_text.begin() + d.begin, parse_text.begin() + d.end, ' ');
    }
  }
  for (const auto &[path, content] : options.placeholder_includes) {
    sm.assignText(path, content);
  }
  slang::SourceBuffer buffer = sm.assignText(filename, parse_text);
  if (mode != nullptr || !options.stubbed_macros.empty()) {
    // Stub definitions and excerpt wrappers go in separate buffers; only
    // main-buffer tokens become part of the token stream, so offsets are
    // unaffected.
    std::vector<slang::SourceBuffer> buffers;
    if (!options.stubbed_macros.empty()) {
      buffers.push_back(
          sm.assignText("<svfmt-stubs>", absl::StrJoin(options.stubbed_macros, "\n") + "\n"));
    }
    if (mode != nullptr) buffers.push_back(sm.assignText("<svfmt-prolog>", mode->prolog));
    buffers.push_back(buffer);
    if (mode != nullptr) buffers.push_back(sm.assignText("<svfmt-epilog>", mode->epilog));
    analyzer->tree_ = slang::syntax::SyntaxTree::fromBuffers(buffers, sm, bag);
  } else {
    analyzer->tree_ = slang::syntax::SyntaxTree::fromBuffer(buffer, sm, bag);
  }
  const auto &diagnostics = analyzer->tree_->diagnostics();
  if (!diagnostics.empty()) {
    analyzer->diagnostics_ = slang::DiagnosticEngine::reportAll(sm, diagnostics);
  }
  // Checks that leave the parse tree intact (Verible accepts them).
  auto is_error = [](const slang::Diagnostic &diag) {
    return diag.isError() && diag.code != slang::diag::EndNameMismatch &&
           diag.code != slang::diag::DeclModifierOrdering;
  };
  bool has_errors = false;
  for (const auto &diag : diagnostics) {
    if (is_error(diag)) has_errors = true;
  }
  if (has_errors) {
    // Unknown macros alone are fine: slang skips their arguments and expands
    // them to nothing. If they broke the parse, retry once with stubs.
    // Missing `include files are fine too: the formatter never needs their
    // contents (macros they define become unknown macros).
    std::vector<size_t> unknown_macros;
    bool other_errors = false;
    for (const auto &diag : diagnostics) {
      if (!is_error(diag)) continue;
      if (diag.code == slang::diag::CouldNotOpenIncludeFile) continue;
      // An unknown macro used in the body of a macro defined in this file
      // is reported at the expansion; its text is in the `define body.
      const slang::SourceLocation original =
          diag.code == slang::diag::UnknownDirective && sm.isMacroLoc(diag.location)
              ? sm.getFullyOriginalLoc(diag.location)
              : diag.location;
      if (diag.code == slang::diag::UnknownDirective && original.buffer() == buffer.id) {
        unknown_macros.push_back(original.offset());
      } else {
        other_errors = true;
      }
    }
    if (other_errors && (unknown_macros.empty() || !options.stubbed_macros.empty())) {
      return absl::InvalidArgumentError(analyzer->diagnostics_);
    }
    // An empty expansion can also parse *wrongly* without an error, e.g.
    // "f(`M(1) + 8)" becomes "f(+8)". Re-parse with stubs whenever a stub
    // would expand to something.
    std::vector<std::string> stubs =
        options.stubbed_macros.empty() ? StubDefinitions(text, unknown_macros, false)
                                       : std::vector<std::string>{};
    const bool nonempty_stub = std::any_of(stubs.begin(), stubs.end(),
                                           [](const std::string &d) { return d.back() != ')'; });
    // No stub could be made (e.g. an escaped macro name): retrying would
    // parse the same text again.
    if (stubs.empty() && other_errors) return absl::InvalidArgumentError(analyzer->diagnostics_);
    if (!stubs.empty() && (other_errors || nonempty_stub)) {
      AnalyzerOptions stubbed = options;
      stubbed.stubbed_macros = stubs;
      auto retry = AnalyzeWithMode(text, filename, stubbed, mode, all_branches);
      if (!retry.ok()) {
        // Try again with argument-less item macros expanding to nothing.
        std::vector<std::string> alt = StubDefinitions(text, unknown_macros, true);
        if (alt != stubs) {
          stubbed.stubbed_macros = std::move(alt);
          auto retry2 = AnalyzeWithMode(text, filename, stubbed, mode, all_branches);
          if (retry2.ok()) return retry2;
        }
        // A loop body must be a statement: a stub ";" (or nothing) fails in
        // "foreach (x) M(...);", while "begin end" parses (the expansion never
        // reaches the output, only the macro call text does).
        if (!retry.ok()) {
          std::vector<std::string> blocks = stubs;
          for (auto &def : blocks) {
            if (def.size() >= 2 && def.compare(def.size() - 2, 2, " ;") == 0) {
              def.replace(def.size() - 2, 2, " begin end");
            } else if (def.back() == ')') {
              def += " begin end";  // an argument list with an empty body
            }
          }
          if (blocks != stubs) {
            stubbed.stubbed_macros = std::move(blocks);
            auto retry3 = AnalyzeWithMode(text, filename, stubbed, mode, all_branches);
            if (retry3.ok()) return retry3;
          }
        }
      }
      if (retry.ok() || other_errors) return retry;
      // Stubs made things worse; keep the empty expansion.
    }
  }

  std::string_view match_text = sm.getSourceText(buffer.id);
  // slang null-terminates its buffers.
  if (!match_text.empty() && match_text.back() == '\0') match_text.remove_suffix(1);
  std::string_view contents = analyzer->original_text_;
  analyzer->data_ = std::make_unique<verible::TextStructureView>(contents);

  Builder builder(*analyzer, contents, match_text, std::move(directives), buffer.id);
  auto status = builder.Build(analyzer->tree_->root(),
                              analyzer->data_->MutableTokenStream(),
                              analyzer->data_->MutableSyntaxTree());
  if (!status.ok()) return status;

  if (mode != nullptr) {
    // Re-root at the excerpt's list (Verible's FocusOnSubtreeSpanningSubstring).
    auto &tree = analyzer->data_->MutableSyntaxTree();
    if (SymbolPtr *body = FindNodeWithTag(tree, static_cast<int>(mode->body))) {
      SymbolPtr new_root = std::move(*body);
      tree = std::move(new_root);
    } else {
      tree = std::make_unique<SyntaxTreeNode>(static_cast<int>(mode->body));
    }
    if (mode == &kParseModes[4]) ReshapeAsModuleItems(tree);
  }

  // The filtered view holds what a parser would see: no whitespace/comments.
  auto &view = analyzer->data_->MutableTokenStreamView();
  verible::InitTokenStreamView(analyzer->data_->TokenStream(), &view);
  analyzer->data_->FilterTokens([](const TokenInfo &t) {
    const int e = t.token_enum();
    return e != TK_SPACE && e != TK_NEWLINE && e != TK_EOL_COMMENT &&
           e != TK_COMMENT_BLOCK && e != TK_LINE_CONT && e != TK_ATTRIBUTE;
  });
  return analyzer;
}

const SyntaxNode *SlangAnalyzer::Origin(const verible::Symbol *node) const {
  auto it = origins_.find(node);
  return it == origins_.end() ? nullptr : it->second;
}

AnalyzerOptions &GlobalAnalyzerOptions() {
  static AnalyzerOptions options;
  return options;
}

std::string_view TokenEnumName(int token_enum) {
  static const auto *names = [] {
    auto *m = new absl::flat_hash_map<int, std::string_view>;
    for (const auto &entry : verilog::kVerilogTokenTable) m->emplace(entry.value, entry.name);
    return m;
  }();
  if (token_enum == TK_SVFMT_DISABLED_TEXT) return "TK_SVFMT_DISABLED_TEXT";
  if (auto it = names->find(token_enum); it != names->end()) return it->second;
  static const auto *chars = [] {
    auto *v = new std::vector<std::string>;
    for (int c = 0; c < 256; c++) v->push_back(std::string("'") + char(c) + "'");
    return v;
  }();
  if (token_enum >= 0 && token_enum < 256) return (*chars)[token_enum];
  return {};
}

std::string_view NodeTagName(int tag) {
  if (IsListTag(tag)) return ListKindName(tag);
  if (IsGroupTag(tag)) return GroupKindName(tag);
  return slang::syntax::toString(static_cast<SyntaxKind>(tag));
}

}  // namespace svfmt

#include "lexeq/lex-equivalence.h"

#include <sstream>
#include <string>
#include <vector>

#include "slang/diagnostics/Diagnostics.h"
#include "slang/parsing/Lexer.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/text/SourceManager.h"
#include "slang/util/BumpAllocator.h"

namespace svfmt {
namespace {

using namespace slang;
using namespace slang::parsing;

struct Item {
  std::string kind;
  std::string text;
  size_t line;
};

std::string WithoutLineIndents(std::string_view text) {
  std::string out;
  bool line_start = false;
  for (char c : text) {
    if (line_start && (c == ' ' || c == '\t')) continue;
    line_start = c == '\n';
    out += c;
  }
  return out;
}

std::vector<Item> Lex(SourceManager &sm, std::string_view text, std::string_view name) {
  BumpAllocator alloc;
  Diagnostics diags;
  SourceBuffer buffer = sm.assignText(name, text);
  Lexer lexer(buffer, alloc, diags, sm);
  std::vector<Item> items;
  bool in_directive = false;
  bool continued = false;  // the next line end follows a "\" continuation
  bool in_table = false;
  auto end_directive = [&] {
    if (in_directive) items.push_back({"DirectiveEnd", "", 0});
    in_directive = false;
  };
  while (true) {
    Token tok = lexer.lex();
    for (const auto &trivia : tok.trivia()) {
      switch (trivia.kind) {
        case TriviaKind::EndOfLine:
          if (continued) {
            continued = false;
          } else {
            end_directive();
          }
          break;
        case TriviaKind::BlockComment:
          items.push_back({std::string(toString(trivia.kind)),
                           WithoutLineIndents(trivia.getRawText()), 0});
          break;
        case TriviaKind::LineComment:
        case TriviaKind::DisabledText:
        case TriviaKind::SkippedTokens:
          items.push_back({std::string(toString(trivia.kind)), std::string(trivia.getRawText()),
                           0});
          break;
        default:
          break;  // whitespace, newlines, line continuations
      }
    }
    if (tok.kind == TokenKind::EndOfFile) {
      end_directive();
      break;
    }
    if (tok.kind == TokenKind::EndTableKeyword) in_table = false;
    if (in_table) {
      for (char c : tok.rawText()) {
        items.push_back({"UdpSymbol", std::string(1, c), sm.getLineNumber(tok.location())});
      }
      continue;
    }
    items.push_back({std::string(toString(tok.kind)), std::string(tok.rawText()),
                     sm.getLineNumber(tok.location())});
    if (tok.kind == TokenKind::TableKeyword) in_table = true;
    if (tok.kind == TokenKind::LineContinuation) {
      continued = true;
      continue;
    }
    continued = false;
    if (tok.kind == TokenKind::Directive &&
        (tok.directiveKind() == slang::syntax::SyntaxKind::DefineDirective ||
         tok.directiveKind() == slang::syntax::SyntaxKind::PragmaDirective)) {
      end_directive();
      in_directive = true;
    }
    // A block comment may hide a line end inside a directive.
    for (const auto &trivia : tok.trivia()) {
      if (trivia.kind == TriviaKind::BlockComment &&
          trivia.getRawText().find('\n') != std::string_view::npos) {
        end_directive();
      }
    }
  }
  return items;
}

}  // namespace

std::optional<std::string> LexicalDifference(std::string_view before, std::string_view after) {
  SourceManager sm;
  const auto a = Lex(sm, before, "<before>");
  const auto b = Lex(sm, after, "<after>");
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    if (a[i].kind != b[i].kind || a[i].text != b[i].text) {
      std::ostringstream out;
      out << "token " << i << " differs: before line " << a[i].line << " " << a[i].kind << " '"
          << a[i].text << "' vs after line " << b[i].line << " " << b[i].kind << " '"
          << b[i].text << "'";
      return out.str();
    }
  }
  if (a.size() != b.size()) {
    std::ostringstream out;
    out << "token count differs: " << a.size() << " vs " << b.size();
    return out.str();
  }
  return std::nullopt;
}

}  // namespace svfmt

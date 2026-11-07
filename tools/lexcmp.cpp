// svfmt-lexcmp: independent check that two SystemVerilog files differ only
// in whitespace. Uses slang's raw lexer (no preprocessing, no parsing), so it
// shares no code path with the formatter.
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
// usage: svfmt-lexcmp <before.sv> <after.sv>
// exit 0 if equivalent; 1 with a description of the first difference.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "slang/diagnostics/Diagnostics.h"
#include "slang/parsing/Lexer.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/text/SourceManager.h"
#include "slang/util/BumpAllocator.h"

using namespace slang;
using namespace slang::parsing;

namespace {

struct Item {
  std::string kind;
  std::string text;
  size_t line;
};

std::string ReadFile(const char *path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "svfmt-lexcmp: cannot read %s\n", path);
    std::exit(2);
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<Item> Lex(SourceManager &sm, const std::string &text, const char *name) {
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
        case TriviaKind::LineComment:
        case TriviaKind::BlockComment:
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

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: svfmt-lexcmp <before.sv> <after.sv>\n");
    return 2;
  }
  SourceManager sm;
  const auto a = Lex(sm, ReadFile(argv[1]), argv[1]);
  const auto b = Lex(sm, ReadFile(argv[2]), argv[2]);
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    if (a[i].kind != b[i].kind || a[i].text != b[i].text) {
      std::cout << "token " << i << " differs: before line " << a[i].line << " " << a[i].kind
                << " '" << a[i].text << "' vs after line " << b[i].line << " " << b[i].kind
                << " '" << b[i].text << "'\n";
      return 1;
    }
  }
  if (a.size() != b.size()) {
    std::cout << "token count differs: " << a.size() << " vs " << b.size() << "\n";
    return 1;
  }
  return 0;
}

#include "lexeq/lex-equivalence.h"

#include <map>
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

bool IsOpen(TokenKind k) {
  return k == TokenKind::OpenParenthesis || k == TokenKind::OpenBracket ||
         k == TokenKind::OpenBrace || k == TokenKind::ApostropheOpenBrace;
}

bool IsClose(TokenKind k) {
  return k == TokenKind::CloseParenthesis || k == TokenKind::CloseBracket ||
         k == TokenKind::CloseBrace;
}

// Where a token is inside a `define: the whitespace that is part of the
// definition is compared exactly; the rest is formatted freely.
enum class DefineState {
  kNone,
  kName,       // the next token is the macro name
  kAfterName,  // "(" right after the name opens the formal arguments
  kFormals,    // inside the formal argument list
  kBodyStart,  // the next token starts the body
  kBody,       // inside the body
};

// The name a macro is known by: "`\\FOO " and "`FOO" are the same macro.
std::string MacroKey(std::string_view raw) {
  if (!raw.empty() && raw.front() == '`') raw.remove_prefix(1);
  if (!raw.empty() && raw.front() == '\\') raw.remove_prefix(1);
  while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) raw.remove_suffix(1);
  return std::string(raw);
}

// "`define F(x)" has formal arguments; "`define F (x)" has a body. An escaped
// name ends at whitespace, so it may be followed by one space (as in slang).
bool OpensFormals(const Token &paren, bool escaped_name) {
  const auto trivia = paren.trivia();
  return trivia.empty() ||
         (escaped_name && trivia.size() == 1 && trivia[0].getRawText() == " ");
}

std::vector<Item> Lex(SourceManager &sm, std::string_view text, std::string_view name) {
  BumpAllocator alloc;
  Diagnostics diags;
  SourceBuffer buffer = sm.assignText(name, text);
  Lexer lexer(buffer, alloc, diags, sm);
  std::vector<Item> items;
  // A `define, `pragma or `line ends at the end of its line.
  bool in_directive = false;
  DefineState define = DefineState::kNone;
  int formal_depth = 0;
  bool continued = false;  // the next line end follows a "\" continuation
  bool in_table = false;
  bool in_primitive = false;  // a "table" is a UDP table only in a primitive
  bool escaped_define = false;  // the `define name is escaped ("\\F")
  bool line_start = true;       // no token yet on this line
  // Macro call arguments (outside `define): the whitespace inside an argument
  // is compared (the argument may be stringified); the whitespace around it
  // (after "(" or "," and before "," or ")") is not part of the argument.
  bool macro_pending = false;  // the last token was a macro name
  bool macro_takes_args = false;  // ... of a function-like (or unknown) macro
  bool macro_known = false;       // ... defined in this text
  bool macro_escaped = false;     // ... escaped ("`\\M (x)")
  int arg_depth = 0;           // bracket depth inside the argument list
  bool at_arg_start = false;   // the last token opened the list or was a ','
  bool arg_empty = false;      // no argument text yet since "(" or ","
  bool cond_name_next = false;  // the last token was "`ifdef" / "`elsif"
  std::vector<size_t> tentative;  // items: whitespace next to "`endif" & co.
  // Macros defined in this text: true if function-like ("`define F(x)").
  std::map<std::string, bool> defined;
  std::string define_name;
  auto end_directive = [&] {
    if (in_directive) items.push_back({"DirectiveEnd", "", 0});
    in_directive = false;
    define = DefineState::kNone;
  };
  while (true) {
    Token tok = lexer.lex();
    const bool arg_separator = arg_depth == 1 && (tok.kind == TokenKind::Comma ||
                                                  tok.kind == TokenKind::CloseParenthesis);
    // A conditional directive in the arguments ("`ifdef A" on its own line)
    // is no argument text. The whitespace before it (and before its name) is
    // around the argument when nothing of the argument precedes it, or when
    // the argument ends after it ("1" then "`endif" then ")"); in the middle
    // of an argument it is compared ("a `ifdef X b `endif c" may be
    // stringified).
    const auto dkind = tok.kind == TokenKind::Directive ? tok.directiveKind()
                                                        : slang::syntax::SyntaxKind::Unknown;
    const bool cond_directive = dkind == slang::syntax::SyntaxKind::IfDefDirective ||
                                dkind == slang::syntax::SyntaxKind::IfNDefDirective ||
                                dkind == slang::syntax::SyntaxKind::ElsIfDirective ||
                                dkind == slang::syntax::SyntaxKind::ElseDirective ||
                                dkind == slang::syntax::SyntaxKind::EndIfDirective;
    const bool cond_part = cond_directive || cond_name_next;
    cond_name_next = dkind == slang::syntax::SyntaxKind::IfDefDirective ||
                     dkind == slang::syntax::SyntaxKind::IfNDefDirective ||
                     dkind == slang::syntax::SyntaxKind::ElsIfDirective;
    const bool keep_arg_space =
        arg_depth > 0 && !at_arg_start && !arg_separator && !arg_empty;
    if (arg_separator) {
      // The argument ends: whitespace held next to its trailing directives
      // was around it.
      for (auto it = tentative.rbegin(); it != tentative.rend(); ++it) {
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(*it));
      }
    }
    if (arg_separator || (keep_arg_space && !cond_part)) tentative.clear();
    const size_t trivia_begin = items.size();
    // "`define F(x)" (formal arguments) differs from "`define F (x)" (a body).
    if (define == DefineState::kAfterName && tok.kind == TokenKind::OpenParenthesis) {
      items.push_back({"DefineNameSpace", OpensFormals(tok, escaped_define) ? "" : " ", 0});
    }
    // "`line" and "`include" stand at the start of their line.
    for (const auto &trivia : tok.trivia()) {
      if (trivia.kind == TriviaKind::EndOfLine) line_start = true;
    }
    if (tok.kind == TokenKind::Directive &&
        (tok.directiveKind() == slang::syntax::SyntaxKind::LineDirective ||
         tok.directiveKind() == slang::syntax::SyntaxKind::IncludeDirective)) {
      items.push_back({"DirectiveLineStart", line_start ? "yes" : "no", 0});
    }
    line_start = false;
    std::string pending;  // whitespace not yet known to be compared
    bool keep = define == DefineState::kBody || (define == DefineState::kNone && keep_arg_space);
    auto flush = [&] {
      if (keep && !pending.empty()) items.push_back({"Whitespace", pending, 0});
      pending.clear();
    };
    // The whitespace before a block comment is held until it is known not to
    // end the define: "... \" then "  /* c */" as its last line is not body.
    std::string held;
    size_t held_at = 0;
    auto release = [&] {
      if (!held.empty()) {
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(held_at),
                     Item{"Whitespace", held, 0});
      }
      held.clear();
    };
    for (const auto &trivia : tok.trivia()) {
      switch (trivia.kind) {
        case TriviaKind::EndOfLine:
          if (continued) {
            release();
            pending += '\n';
            continued = false;
          } else {
            // Whitespace at the end of a line is not part of anything; the
            // line break itself is, inside a macro argument.
            held.clear();
            pending.clear();
            if (define != DefineState::kNone) keep = false;
            if (keep) pending = "\n";
            end_directive();
          }
          break;
        case TriviaKind::Whitespace:
          release();
          pending += trivia.getRawText();
          break;
        case TriviaKind::BlockComment:
          release();
          if (keep && define == DefineState::kBody && !pending.empty()) {
            held = pending;
            held_at = items.size();
            pending.clear();
          }
          flush();
          items.push_back({std::string(toString(trivia.kind)),
                           define != DefineState::kNone ? std::string(trivia.getRawText())
                                                        : WithoutLineIndents(trivia.getRawText()),
                           0});
          break;
        case TriviaKind::LineComment:
          held.clear();     // a comment then an end-of-line comment: line end
          pending.clear();  // whitespace before an end-of-line comment
          // "`define M a // c \": slang ends the comment (and the define)
          // at the line end, other tools continue the body; compare the
          // next line as body either way.
          if (define != DefineState::kNone && trivia.getRawText().ends_with('\\')) {
            continued = true;
          }
          [[fallthrough]];
        case TriviaKind::DisabledText:
        case TriviaKind::SkippedTokens:
          release();
          items.push_back({std::string(toString(trivia.kind)), std::string(trivia.getRawText()),
                           0});
          break;
        default:
          break;  // line continuations are tokens of their own, below
      }
    }
    release();
    flush();
    if (keep_arg_space && cond_part && define == DefineState::kNone) {
      for (size_t i = trivia_begin; i < items.size(); ++i) {
        if (items[i].kind == "Whitespace") tentative.push_back(i);
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
    // A UDP table, not the word in a `define body ("`define T table").
    if (tok.kind == TokenKind::PrimitiveKeyword && define == DefineState::kNone) in_primitive = true;
    if (tok.kind == TokenKind::EndPrimitiveKeyword) in_primitive = false;
    if (tok.kind == TokenKind::TableKeyword && define == DefineState::kNone && in_primitive &&
        arg_depth == 0) {
      in_table = true;
    }

    // `define states.
    switch (define) {
      case DefineState::kName:
        define = DefineState::kAfterName;
        // A name defined in several branches ("`ifdef A `define F(x) ...
        // `else `define F ...") is function-like if any definition is: its
        // arguments are then compared.
        escaped_define = !tok.rawText().empty() && tok.rawText().front() == '\\';
        define_name = MacroKey(tok.rawText());
        defined.emplace(define_name, false);
        break;
      case DefineState::kAfterName:
        if (tok.kind == TokenKind::OpenParenthesis && OpensFormals(tok, escaped_define)) {
          define = DefineState::kFormals;
          formal_depth = 1;
          defined[define_name] = true;
        } else {
          define = DefineState::kBody;  // this token starts the body
        }
        break;
      case DefineState::kFormals:
        if (tok.kind == TokenKind::OpenParenthesis) ++formal_depth;
        if (tok.kind == TokenKind::CloseParenthesis && --formal_depth == 0) {
          define = DefineState::kBodyStart;
        }
        break;
      case DefineState::kBodyStart:
        if (tok.kind != TokenKind::LineContinuation) define = DefineState::kBody;
        break;
      default:
        break;
    }

    // Macro argument lists.
    if (define == DefineState::kNone) {
      if (arg_depth > 0) {
        if (IsOpen(tok.kind)) ++arg_depth;
        if (IsClose(tok.kind)) --arg_depth;
        at_arg_start = arg_depth == 1 && tok.kind == TokenKind::Comma;
        if (!cond_part) arg_empty = at_arg_start;
        if (arg_depth == 0) tentative.clear();
      } else if (macro_pending && tok.kind == TokenKind::OpenParenthesis && macro_takes_args &&
                 (macro_known || OpensFormals(tok, macro_escaped))) {
        // A macro defined here takes arguments only if it is function-like;
        // one defined elsewhere is taken to be called when "(" follows its
        // name directly ("`uvm_info(", not "module `NAME (").
        arg_depth = 1;
        at_arg_start = true;
        arg_empty = true;
      }
      macro_pending = tok.kind == TokenKind::Directive &&
                      tok.directiveKind() == slang::syntax::SyntaxKind::MacroUsage;
      if (macro_pending) {
        macro_escaped = tok.rawText().size() > 1 && tok.rawText()[1] == '\\';
        const auto it = defined.find(MacroKey(tok.rawText()));
        macro_known = it != defined.end();
        macro_takes_args = macro_known ? it->second : true;
      }
    }

    if (tok.kind == TokenKind::LineContinuation) {
      continued = true;
      continue;
    }
    continued = false;
    if (tok.kind == TokenKind::Directive) {
      const auto kind = tok.directiveKind();
      if (kind == slang::syntax::SyntaxKind::DefineDirective ||
          kind == slang::syntax::SyntaxKind::PragmaDirective ||
          kind == slang::syntax::SyntaxKind::LineDirective) {
        end_directive();
        in_directive = true;
        if (kind == slang::syntax::SyntaxKind::DefineDirective) define = DefineState::kName;
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
    // A "`line" / "`include" may move to the start of its line, not away.
    if (a[i].kind == "DirectiveLineStart" && b[i].kind == a[i].kind && a[i].text == "no") {
      continue;
    }
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

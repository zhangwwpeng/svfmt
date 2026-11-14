// svfmt-preproc-cmp: compares the preprocessed token streams of two
// SystemVerilog files (before/after formatting) under the same defines.
//
// usage: svfmt-preproc-cmp [--define NAME[=VALUE]]... <before.sv> <after.sv>
// exit 0 if the expanded token streams agree; 1 with the first difference;
// 2 on usage/read errors.
//
// This is a test tool, not part of the formatter: it checks that formatting
// did not change what slang's preprocessor sees (macro expansion, conditional
// branches, macro arguments). Whitespace and comments are not part of the
// stream; svfmt-lexcmp covers those. Values produced by a macro (numbers and
// strings) are masked: `__LINE__ / `__FILE__ and a moved macro call change
// them, and svfmt-lexcmp already compares the macro body and its arguments.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "slang/diagnostics/Diagnostics.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"
#include "slang/util/BumpAllocator.h"

namespace {

struct Item {
  int kind;
  std::string text;
};

std::string ReadFile(const char *path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "svfmt-preproc-cmp: cannot read %s\n", path);
    std::exit(2);
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool IsIntrinsicName(std::string_view text) {
  return text.find("__FILE__") != std::string_view::npos ||
         text.find("__LINE__") != std::string_view::npos ||
         text.find("__DATE__") != std::string_view::npos ||
         text.find("__TIME__") != std::string_view::npos;
}

// Macros whose expansion contains an intrinsic (`__LINE__ etc.), directly or
// through another macro. Their string/number tokens change when a line moves
// or the file name changes, so they are masked.
std::set<std::string> TaintedMacros(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> defs;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    std::string_view line = text.substr(pos, eol - pos);
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string_view::npos && line.substr(first, 7) == "`define") {
      size_t q = first + 7;
      while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
      const size_t name_start = q;
      while (q < line.size() &&
             (std::isalnum(static_cast<unsigned char>(line[q])) || line[q] == '_' ||
              line[q] == '$' || line[q] == '\\')) {
        ++q;
      }
      std::string name(line.substr(name_start, q - name_start));
      while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
      std::string body;
      if (q < line.size() && line[q] == '(') {
        // The formal argument list may span lines (slang accepts it without
        // a "\" continuation); the body starts after the closing ")".
        int depth = 0;
        size_t r = q;
        for (;;) {
          for (; r < line.size(); ++r) {
            if (line[r] == '(') {
              ++depth;
            } else if (line[r] == ')' && --depth == 0) {
              ++r;
              break;
            }
          }
          if (depth == 0) {
            body.assign(line.substr(r));
            break;
          }
          if (eol >= text.size()) break;
          pos = eol + 1;
          eol = text.find('\n', pos);
          if (eol == std::string_view::npos) eol = text.size();
          line = text.substr(pos, eol - pos);
          r = 0;
        }
      } else {
        body.assign(line.substr(q));
      }
      while (!line.empty() && line.back() == '\\' && eol < text.size()) {
        pos = eol + 1;
        eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        line = text.substr(pos, eol - pos);
        body += '\n';
        body += line;
      }
      if (!name.empty()) defs.emplace_back(std::move(name), std::move(body));
    }
    pos = eol + 1;
  }

  std::set<std::string> tainted;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[name, body] : defs) {
      if (tainted.count(name)) continue;
      bool tainted_body = IsIntrinsicName(body);
      if (!tainted_body) {
        for (const auto &macro : tainted) {
          if (body.find("`" + macro) != std::string::npos) {
            tainted_body = true;
            break;
          }
        }
      }
      if (tainted_body) {
        tainted.insert(name);
        changed = true;
      }
    }
  }
  return tainted;
}

// UDP table rows are character-level ("00" is two input symbols, not the
// number zero); the preprocessor tokenizes them as numbers, so the same row
// can compare differently after formatting ("00" vs "0 0"). The raw lexer
// check compares UDP tables itself; blank the table bodies here.
std::string BlankUdpTables(std::string text) {
  const auto has_word = [](std::string_view line, std::string_view word) {
    for (size_t p = line.find(word); p != std::string_view::npos;
         p = line.find(word, p + word.size())) {
      const bool left_ok = p == 0 ||
          !(std::isalnum(static_cast<unsigned char>(line[p - 1])) || line[p - 1] == '_');
      const size_t end = p + word.size();
      const bool right_ok = end >= line.size() ||
          !(std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_');
      if (left_ok && right_ok) return true;
    }
    return false;
  };
  bool in_primitive = false;
  bool in_table = false;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    const std::string_view line(text.data() + pos, eol - pos);
    if (has_word(line, "endprimitive")) {
      in_primitive = false;
      in_table = false;
    } else {
      if (has_word(line, "primitive")) in_primitive = true;
      if (in_primitive) {
        if (has_word(line, "endtable")) {
          if (in_table) {
            in_table = false;
          } else if (has_word(line, "table")) {
            // "table 0 : 1; endtable" on one line: blank the body.
            const size_t t = line.find("table");
            const size_t et = line.find("endtable");
            for (size_t i = pos + t + 5; i < pos + et; ++i) text[i] = ' ';
          }
        } else if (has_word(line, "table")) {
          in_table = true;
          const size_t after = pos + line.find("table") + 5;
          for (size_t i = after; i < eol; ++i) text[i] = ' ';
        } else if (in_table) {
          for (size_t i = pos; i < eol; ++i) text[i] = ' ';
        }
      }
    }
    pos = eol + 1;
  }
  return text;
}

std::vector<Item> Preprocess(std::string_view text, std::string_view path,
                             const std::vector<std::string> &defines,
                             const std::vector<std::string> &include_dirs) {
  slang::SourceManager sm;
  slang::BumpAllocator alloc;
  slang::Diagnostics diags;
  slang::Bag bag;
  slang::parsing::PreprocessorOptions options;
  options.languageVersion = slang::LanguageVersion::v1800_2023;
  options.predefines = defines;
  for (const auto &dir : include_dirs) {
    options.additionalIncludePaths.emplace_back(dir);
  }
  bag.set(options);
  slang::parsing::Preprocessor pp(sm, alloc, diags, bag);
  const std::string blanked = BlankUdpTables(std::string(text));
  text = blanked;
  pp.pushSource(text, path);

  const std::set<std::string> tainted = TaintedMacros(text);
  std::vector<Item> items;
  for (;;) {
    const slang::parsing::Token tok = pp.next();
    if (tok.kind == slang::parsing::TokenKind::EndOfFile) break;
    std::string item(tok.rawText());
    const slang::SourceLocation loc = tok.location();
    if (sm.isMacroLoc(loc)) {
      const std::string macro_name(sm.getMacroName(loc));
      const bool value_token =
          tok.kind == slang::parsing::TokenKind::StringLiteral ||
          tok.kind == slang::parsing::TokenKind::IntegerLiteral ||
          tok.kind == slang::parsing::TokenKind::UnbasedUnsizedLiteral;
      // Values produced by a macro can depend on `__LINE__ / `__FILE__ or on
      // the line a macro call moved to; the raw lexer check compares the
      // macro body and its arguments, so mask the expanded value here.
      bool masked = value_token;
      // "foo_`__LINE__" pastes the line number into an identifier.
      if (!masked && tok.kind == slang::parsing::TokenKind::Identifier &&
          tainted.count(macro_name) != 0 &&
          std::any_of(item.begin(), item.end(),
                      [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        masked = true;
      }
      if (masked) item = "<intrinsic>";
    }
    items.push_back({static_cast<int>(tok.kind), std::move(item)});
  }
  return items;
}

}  // namespace

int main(int argc, char **argv) {
  std::vector<std::string> defines;
  std::vector<std::string> include_dirs;
  std::vector<const char *> files;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--define") {
      if (++i >= argc) {
        std::fprintf(stderr, "svfmt-preproc-cmp: --define needs a value\n");
        return 2;
      }
      defines.emplace_back(argv[i]);
    } else if (arg.starts_with("--define=")) {
      defines.emplace_back(arg.substr(9));
    } else if (arg == "--include-dir") {
      if (++i >= argc) {
        std::fprintf(stderr, "svfmt-preproc-cmp: --include-dir needs a value\n");
        return 2;
      }
      include_dirs.emplace_back(argv[i]);
    } else {
      files.push_back(argv[i]);
    }
  }
  if (files.size() != 2) {
    std::fprintf(stderr,
                 "usage: svfmt-preproc-cmp [--define NAME[=VALUE]]... "
                 "<before.sv> <after.sv>\n");
    return 2;
  }

  const std::string before = ReadFile(files[0]);
  const std::string after = ReadFile(files[1]);
  const auto before_items = Preprocess(before, files[0], defines, include_dirs);
  const auto after_items = Preprocess(after, files[1], defines, include_dirs);

  const size_t common = std::min(before_items.size(), after_items.size());
  for (size_t i = 0; i < common; ++i) {
    if (before_items[i].kind != after_items[i].kind ||
        before_items[i].text != after_items[i].text) {
      std::printf("token %zu: kind %d \"%s\" vs kind %d \"%s\"\n", i,
                  before_items[i].kind, before_items[i].text.c_str(),
                  after_items[i].kind, after_items[i].text.c_str());
      return 1;
    }
  }
  if (before_items.size() != after_items.size()) {
    std::printf("token count %zu vs %zu\n", before_items.size(), after_items.size());
    return 1;
  }
  return 0;
}

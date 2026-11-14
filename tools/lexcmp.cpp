// Copyright 2026 The svfmt Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// svfmt-lexcmp: checks that two SystemVerilog files differ only in
// whitespace (see src/lexeq/lex-equivalence.h for what is compared). svfmt
// runs the same check on its own output; the corpus tests run this tool on
// the files svfmt wrote.
//
// usage: svfmt-lexcmp <before.sv> <after.sv>
// exit 0 if equivalent; 1 with a description of the first difference.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "lexeq/lex-equivalence.h"

namespace {

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

}  // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: svfmt-lexcmp <before.sv> <after.sv>\n");
    return 2;
  }
  if (const auto difference = svfmt::LexicalDifference(ReadFile(argv[1]), ReadFile(argv[2]))) {
    std::cout << *difference << "\n";
    return 1;
  }
  return 0;
}

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

#include "verible/common/strings/split.h"

#include <cstddef>
#include <string_view>
#include <vector>

#include "absl/strings/str_split.h"

namespace verible {

std::vector<std::string_view> SplitLines(std::string_view text) {
  if (text.empty()) return {};
  std::vector<std::string_view> lines(absl::StrSplit(text, absl::ByChar('\n')));
  // If text ends cleanly with a \n, omit the last blank split,
  // otherwise treat it as if the trailing text ends with a \n.
  if (text.back() == '\n') {
    lines.pop_back();
  }
  return lines;
}

std::vector<std::string_view> SplitLinesKeepLineTerminator(
    std::string_view text) {
  std::vector<std::string_view> lines;
  for (size_t start = 0; start < text.size();) {
    size_t end = text.find_first_of("\r\n", start);
    if (end == std::string_view::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    if (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n') ++end;
    lines.push_back(text.substr(start, end + 1 - start));
    start = end + 1;
  }
  return lines;
}

}  // namespace verible

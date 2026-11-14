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

// Helpers for querying a verible::SyntaxTreeContext whose node tags are
// slang::syntax::SyntaxKind values (see slang-analyzer.h).

#ifndef SVFMT_SYNTAX_CONTEXT_H_
#define SVFMT_SYNTAX_CONTEXT_H_

#include <cstddef>
#include <initializer_list>

#include "slang/syntax/SyntaxKind.h"
#include "svfmt/list-kinds.h"
#include "verible/common/text/concrete-syntax-tree.h"
#include "verible/common/text/symbol.h"
#include "verible/common/text/syntax-tree-context.h"

namespace svfmt {

using slang::syntax::SyntaxKind;

inline SyntaxKind KindOf(const verible::Symbol &symbol) {
  return static_cast<SyntaxKind>(symbol.Tag().tag);
}

// Kind of the ancestor `up` levels above the innermost one (0 = direct
// parent), or SyntaxKind::Unknown. Synthetic list nodes are skipped, so the
// parent of a port is its AnsiPortList, not AnsiPortList_ports.
inline SyntaxKind AncestorKind(const verible::SyntaxTreeContext &context,
                               size_t up = 0) {
  for (auto it = context.rbegin(); it != context.rend(); ++it) {
    if (IsListTag((*it)->Tag().tag)) continue;
    if (up == 0) return KindOf(**it);
    --up;
  }
  return SyntaxKind::Unknown;
}

// Tag of the innermost enclosing list node, or 0.
inline int EnclosingListTag(const verible::SyntaxTreeContext &context) {
  for (auto it = context.rbegin(); it != context.rend(); ++it) {
    if (IsListTag((*it)->Tag().tag)) return (*it)->Tag().tag;
  }
  return 0;
}

// True if the innermost node (list or not) has tag `list`.
inline bool DirectParentIsList(const verible::SyntaxTreeContext &context, ListKind list) {
  return !context.empty() && (*context.rbegin())->Tag().tag == static_cast<int>(list);
}

inline bool DirectParentIs(const verible::SyntaxTreeContext &context,
                           SyntaxKind kind) {
  return AncestorKind(context) == kind;
}

inline bool DirectParentIsOneOf(const verible::SyntaxTreeContext &context,
                                std::initializer_list<SyntaxKind> kinds) {
  const SyntaxKind parent = AncestorKind(context);
  for (SyntaxKind k : kinds) {
    if (k == parent) return true;
  }
  return false;
}

// Like SyntaxTreeContext::DirectParentsAre: innermost first.
inline bool DirectParentsAre(const verible::SyntaxTreeContext &context,
                             std::initializer_list<SyntaxKind> kinds) {
  size_t up = 0;
  for (SyntaxKind k : kinds) {
    if (AncestorKind(context, up++) != k) return false;
  }
  return true;
}

// The innermost ancestor of kind `kind`, or nullptr.
inline const verible::Symbol *NearestOfKind(const verible::SyntaxTreeContext &context,
                                            SyntaxKind kind) {
  for (auto it = context.rbegin(); it != context.rend(); ++it) {
    if (KindOf(**it) == kind) return *it;
  }
  return nullptr;
}

// Walks outward from the innermost ancestor. Returns true if `include`
// matches before `exclude` does.
template <typename Include, typename Exclude>
bool IsInsideFirstIf(const verible::SyntaxTreeContext &context, Include include,
                     Exclude exclude) {
  for (auto it = context.rbegin(); it != context.rend(); ++it) {
    const SyntaxKind kind = KindOf(**it);
    if (include(kind)) return true;
    if (exclude(kind)) return false;
  }
  return false;
}

template <typename Include>
bool IsInsideIf(const verible::SyntaxTreeContext &context, Include include) {
  return IsInsideFirstIf(context, include, [](SyntaxKind) { return false; });
}

inline bool IsInside(const verible::SyntaxTreeContext &context, SyntaxKind kind) {
  return IsInsideIf(context, [kind](SyntaxKind k) { return k == kind; });
}

inline bool IsInsideAnyOf(const verible::SyntaxTreeContext &context,
                          std::initializer_list<SyntaxKind> kinds) {
  return IsInsideIf(context, [kinds](SyntaxKind k) {
    for (SyntaxKind x : kinds) {
      if (x == k) return true;
    }
    return false;
  });
}

// slang models assignments ("a = b", "a <= b", "a += b") as binary
// expressions; Verible does not (kNetVariableAssignment, ...).
bool IsAssignmentKind(SyntaxKind kind);

// A binary expression in Verible's sense: slang binary expression kinds
// except assignments.
bool IsVerilogBinaryExpressionKind(SyntaxKind kind);

}  // namespace svfmt

#endif  // SVFMT_SYNTAX_CONTEXT_H_

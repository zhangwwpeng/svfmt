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

#include "svfmt/syntax-context.h"

#include "slang/syntax/AllSyntax.h"

namespace svfmt {

bool IsAssignmentKind(SyntaxKind kind) {
  switch (kind) {
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

bool IsVerilogBinaryExpressionKind(SyntaxKind kind) {
  return slang::syntax::BinaryExpressionSyntax::isKind(kind) && !IsAssignmentKind(kind);
}

}  // namespace svfmt

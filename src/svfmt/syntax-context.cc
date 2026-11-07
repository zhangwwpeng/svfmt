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

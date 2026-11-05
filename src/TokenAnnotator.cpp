#include "svfmt/TokenAnnotator.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/syntax/AllSyntax.h"

using namespace slang::parsing;
using namespace slang::syntax;

namespace svfmt {

bool TokenAnnotator::isAssignmentOperator(Token token) {
    switch (token.kind) {
        case TokenKind::Equals:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
        case TokenKind::PercentEqual:
        case TokenKind::AndEqual:
        case TokenKind::OrEqual:
        case TokenKind::XorEqual:
        case TokenKind::LeftShiftEqual:
        case TokenKind::RightShiftEqual:
        case TokenKind::TripleLeftShiftEqual:
        case TokenKind::TripleRightShiftEqual:
        case TokenKind::LessThanEquals: // In procedural block `a <= b`
            return true;
        default:
            return false;
    }
}

bool TokenAnnotator::isUnaryOperator(Token token, const SyntaxNode* parent) {
    if (parent) {
        if (parent->as_if<PrefixUnaryExpressionSyntax>() ||
            parent->kind == SyntaxKind::TimingControlExpression ||
            parent->kind == SyntaxKind::EventControl) {
            return true;
        }
    }
    // Also '@', '#', '##' are prefix timing controls
    if (token.kind == TokenKind::At || token.kind == TokenKind::Hash || token.kind == TokenKind::DoubleHash) {
        return true;
    }
    return false;
}

bool TokenAnnotator::isBinaryOperator(Token token, const SyntaxNode* parent) {
    if (parent) {
        if (parent->kind == SyntaxKind::AddExpression ||
            parent->kind == SyntaxKind::SubtractExpression ||
            parent->kind == SyntaxKind::MultiplyExpression ||
            parent->kind == SyntaxKind::DivideExpression ||
            parent->kind == SyntaxKind::ModExpression ||
            parent->kind == SyntaxKind::BinaryAndExpression ||
            parent->kind == SyntaxKind::BinaryOrExpression ||
            parent->kind == SyntaxKind::BinaryXorExpression ||
            parent->kind == SyntaxKind::BinaryXnorExpression ||
            parent->kind == SyntaxKind::LogicalAndExpression ||
            parent->kind == SyntaxKind::LogicalOrExpression ||
            parent->kind == SyntaxKind::LogicalShiftLeftExpression ||
            parent->kind == SyntaxKind::LogicalShiftRightExpression ||
            parent->kind == SyntaxKind::ArithmeticShiftLeftExpression ||
            parent->kind == SyntaxKind::ArithmeticShiftRightExpression ||
            parent->kind == SyntaxKind::EqualityExpression ||
            parent->kind == SyntaxKind::InequalityExpression ||
            parent->kind == SyntaxKind::CaseEqualityExpression ||
            parent->kind == SyntaxKind::CaseInequalityExpression ||
            parent->kind == SyntaxKind::WildcardEqualityExpression ||
            parent->kind == SyntaxKind::WildcardInequalityExpression ||
            parent->kind == SyntaxKind::LessThanExpression ||
            parent->kind == SyntaxKind::LessThanEqualExpression ||
            parent->kind == SyntaxKind::GreaterThanExpression ||
            parent->kind == SyntaxKind::GreaterThanEqualExpression) {
            return true;
        }
    }
    switch (token.kind) {
        case TokenKind::DoubleStar:
        case TokenKind::DoubleAnd:
        case TokenKind::DoubleOr:
        case TokenKind::MinusArrow:
        case TokenKind::MinusDoubleArrow:
        case TokenKind::EqualsArrow:
        case TokenKind::InsideKeyword:
        case TokenKind::DistKeyword:
            return true;
        default:
            return false;
    }
}

bool TokenAnnotator::isControlFlowKeyword(TokenKind kind) {
    switch (kind) {
        case TokenKind::IfKeyword:
        case TokenKind::ElseKeyword:
        case TokenKind::ForKeyword:
        case TokenKind::ForeachKeyword:
        case TokenKind::ForeverKeyword:
        case TokenKind::RepeatKeyword:
        case TokenKind::WhileKeyword:
        case TokenKind::DoKeyword:
        case TokenKind::CaseKeyword:
        case TokenKind::CaseXKeyword:
        case TokenKind::CaseZKeyword:
        case TokenKind::ReturnKeyword:
        case TokenKind::WaitKeyword:
        case TokenKind::AssertKeyword:
        case TokenKind::AssumeKeyword:
        case TokenKind::CoverKeyword:
            return true;
        default:
            return false;
    }
}

bool TokenAnnotator::isInsideRangeOrSlice(const SyntaxNode* node) {
    const SyntaxNode* curr = node;
    while (curr) {
        if (curr->kind == SyntaxKind::SimpleRangeSelect ||
            curr->kind == SyntaxKind::AscendingRangeSelect ||
            curr->kind == SyntaxKind::DescendingRangeSelect ||
            curr->kind == SyntaxKind::ElementSelect ||
            curr->kind == SyntaxKind::RangeDimensionSpecifier ||
            curr->kind == SyntaxKind::BitSelect) {
            return true;
        }
        // Don't search beyond statement or expression boundary
        if (curr->kind == SyntaxKind::SequentialBlockStatement ||
            curr->kind == SyntaxKind::ContinuousAssign) {
            break;
        }
        curr = curr->parent;
    }
    return false;
}

bool TokenAnnotator::isCaseItemColon(Token token, const SyntaxNode* parent) {
    if (token.kind != TokenKind::Colon) return false;
    const SyntaxNode* curr = parent;
    while (curr) {
        if (curr->kind == SyntaxKind::StandardCaseItem ||
            curr->kind == SyntaxKind::PatternCaseItem ||
            curr->kind == SyntaxKind::DefaultCaseItem) {
            return true;
        }
        if (curr->kind == SyntaxKind::CaseStatement) break;
        curr = curr->parent;
    }
    return false;
}

bool TokenAnnotator::isLabelColon(Token token, const SyntaxNode* parent) {
    if (token.kind != TokenKind::Colon) return false;
    if (parent && (parent->kind == SyntaxKind::NamedBlockClause ||
                   parent->kind == SyntaxKind::NamedLabel)) {
        return true;
    }
    return false;
}

int TokenAnnotator::spacesRequiredBetween(
    Token left,
    Token right,
    const SyntaxNode* leftParent,
    const SyntaxNode* rightParent,
    const FormatStyle& style
) {
    // Escaped identifier must have whitespace after it
    if (left.kind == TokenKind::Identifier && !left.valueText().empty() && left.valueText().front() == '\\') {
        return 1;
    }

    // Line continuation
    if (left.kind == TokenKind::LineContinuation || right.kind == TokenKind::LineContinuation) {
        return 0;
    }

    // Scope resolution operator ::
    if (left.kind == TokenKind::DoubleColon || right.kind == TokenKind::DoubleColon) {
        return 0;
    }

    // Dot . (hierarchical access or named port connection)
    if (left.kind == TokenKind::Dot) {
        return 0;
    }
    if (right.kind == TokenKind::Dot) {
        return 0;
    }

    // Open delimiters: (, [, {
    if (left.kind == TokenKind::OpenParenthesis ||
        left.kind == TokenKind::OpenBracket ||
        left.kind == TokenKind::OpenBrace ||
        left.kind == TokenKind::ApostropheOpenBrace) {
        return 0;
    }

    // Close delimiters: ), ], }
    if (right.kind == TokenKind::CloseParenthesis ||
        right.kind == TokenKind::CloseBracket ||
        right.kind == TokenKind::CloseBrace) {
        return 0;
    }

    // After close bracket ']' or close parenthesis ')'
    if (left.kind == TokenKind::CloseBracket) {
        if (right.kind == TokenKind::OpenBracket ||
            right.kind == TokenKind::Dot ||
            right.kind == TokenKind::Comma ||
            right.kind == TokenKind::Semicolon ||
            right.kind == TokenKind::CloseParenthesis ||
            right.kind == TokenKind::CloseBracket ||
            right.kind == TokenKind::CloseBrace) {
            return 0;
        }
        return 1;
    }
    if (left.kind == TokenKind::CloseParenthesis) {
        if (right.kind == TokenKind::Dot ||
            right.kind == TokenKind::Comma ||
            right.kind == TokenKind::Semicolon ||
            right.kind == TokenKind::CloseParenthesis ||
            right.kind == TokenKind::CloseBracket ||
            right.kind == TokenKind::CloseBrace) {
            return 0;
        }
        return 1;
    }

    // Comma ,
    if (right.kind == TokenKind::Comma) {
        return 0;
    }
    if (left.kind == TokenKind::Comma) {
        return 1;
    }

    // Semicolon ;
    if (right.kind == TokenKind::Semicolon) {
        if (left.kind == TokenKind::Colon) {
            return 1; // e.g. "default: ;"
        }
        return 0;
    }
    if (left.kind == TokenKind::Semicolon) {
        return 1;
    }

    // Colon :
    if (left.kind == TokenKind::Colon) {
        if (isCaseItemColon(left, leftParent) || isLabelColon(left, leftParent)) {
            return 1;
        }
        if (leftParent && leftParent->kind == SyntaxKind::ConditionalExpression) {
            return 1; // ternary ?:
        }
        if (isInsideRangeOrSlice(leftParent)) {
            return 0; // [7:0]
        }
        return 1;
    }
    if (right.kind == TokenKind::Colon) {
        if (isCaseItemColon(right, rightParent)) {
            return 0;
        }
        if (isLabelColon(right, rightParent)) {
            return 1; // begin : label
        }
        if (rightParent && rightParent->kind == SyntaxKind::ConditionalExpression) {
            return 1; // ternary ?:
        }
        if (isInsideRangeOrSlice(rightParent)) {
            return 0; // [7:0]
        }
        return 0;
    }

    // Ternary ?
    if (left.kind == TokenKind::Question || right.kind == TokenKind::Question) {
        return 1;
    }

    // Unary prefix operator
    if (isUnaryOperator(left, leftParent)) {
        if (left.kind == right.kind) {
            return 1;
        }
        return 0;
    }

    // Event control @
    if (left.kind == TokenKind::At) {
        return 0; // @(posedge clk)
    }
    if (right.kind == TokenKind::At) {
        return 1; // always @(...)
    }

    // Delay #
    if (left.kind == TokenKind::Hash) {
        return 0; // #10, #(parameter)
    }

    // Parameter list #(
    if (right.kind == TokenKind::Hash) {
        if (rightParent && (rightParent->kind == SyntaxKind::ModuleHeader ||
                            rightParent->kind == SyntaxKind::HierarchicalInstance)) {
            return 1;
        }
        if (left.kind == TokenKind::Identifier || LexerFacts::isKeyword(left.kind)) {
            return style.class_parameter_space ? 1 : 0;
        }
        return 1;
    }

    // Assignment operators
    if (isAssignmentOperator(left) || isAssignmentOperator(right)) {
        return 1;
    }

    // Binary operators
    if (isBinaryOperator(left, leftParent) || isBinaryOperator(right, rightParent)) {
        if (style.compact_indexing_and_selections &&
            (isInsideRangeOrSlice(leftParent) || isInsideRangeOrSlice(rightParent))) {
            return 0;
        }
        return 1;
    }

    // Keywords before (
    if (right.kind == TokenKind::OpenParenthesis) {
        if (isControlFlowKeyword(left.kind)) {
            return 1; // if (a), for (..), case (..)
        }
        if (left.kind == TokenKind::SystemIdentifier) {
            return 0; // $display(...)
        }
        if (left.kind == TokenKind::Identifier) {
            if (rightParent && (rightParent->kind == SyntaxKind::HierarchicalInstance ||
                                rightParent->kind == SyntaxKind::ModuleHeader ||
                                rightParent->kind == SyntaxKind::AnsiPortList)) {
                return 1;
            }
            return 0;
        }
    }

    // Two alphanumeric / keyword tokens cannot be merged without space
    bool leftWord = left.kind == TokenKind::Identifier ||
                    left.kind == TokenKind::SystemIdentifier ||
                    left.kind == TokenKind::IntegerLiteral ||
                    left.kind == TokenKind::RealLiteral ||
                    left.kind == TokenKind::TimeLiteral ||
                    LexerFacts::isKeyword(left.kind);

    bool rightWord = right.kind == TokenKind::Identifier ||
                     right.kind == TokenKind::SystemIdentifier ||
                     right.kind == TokenKind::IntegerLiteral ||
                     right.kind == TokenKind::RealLiteral ||
                     right.kind == TokenKind::TimeLiteral ||
                     LexerFacts::isKeyword(right.kind);

    if (leftWord && rightWord) {
        return 1;
    }

    // Keyword followed by punctuation or vice-versa
    if (LexerFacts::isKeyword(left.kind)) {
        return 1;
    }

    return 0;
}

} // namespace svfmt

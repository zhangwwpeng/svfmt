#pragma once

#include <string_view>
#include "slang/parsing/Token.h"
#include "slang/parsing/LexerFacts.h"
#include "slang/syntax/SyntaxNode.h"
#include "svfmt/FormatStyle.h"

namespace svfmt {

enum class SpacingDecision {
    None,       // 0 spaces
    One,        // 1 space
    TwoOrMore,  // 2 spaces (e.g. before comments)
    Custom      // custom spaces
};

class TokenAnnotator {
public:
    // Determine the minimum required spaces between left and right token on the same line
    static int spacesRequiredBetween(
        slang::parsing::Token left,
        slang::parsing::Token right,
        const slang::syntax::SyntaxNode* leftParent,
        const slang::syntax::SyntaxNode* rightParent,
        const FormatStyle& style
    );

    static bool isUnaryOperator(slang::parsing::Token token, const slang::syntax::SyntaxNode* parent);
    static bool isBinaryOperator(slang::parsing::Token token, const slang::syntax::SyntaxNode* parent);
    static bool isAssignmentOperator(slang::parsing::Token token);
    static bool isControlFlowKeyword(slang::parsing::TokenKind kind);
    static bool isInsideRangeOrSlice(const slang::syntax::SyntaxNode* node);
    static bool isCaseItemColon(slang::parsing::Token token, const slang::syntax::SyntaxNode* parent);
    static bool isLabelColon(slang::parsing::Token token, const slang::syntax::SyntaxNode* parent);
};

} // namespace svfmt

#include "svfmt/Formatter.h"
#include "svfmt/TokenAnnotator.h"
#include "svfmt/Alignment.h"
#include "svfmt/CommentControls.h"

#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "slang/syntax/SyntaxPrinter.h"
#include "slang/parsing/Token.h"
#include "slang/parsing/LexerFacts.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>

using namespace slang;
using namespace slang::syntax;
using namespace slang::parsing;

namespace svfmt {

namespace {

struct FormattedToken {
    Token token;
    const SyntaxNode* parentNode = nullptr;
    std::string text;
    int spacesBefore = 0;
    bool startsNewLine = false;
    int lineIndent = 0;
    std::vector<std::string> leadingComments;
    std::string trailingComment;
    int blankLinesBefore = 0;
};

struct TokenCollector : public SyntaxVisitor<TokenCollector> {
    std::vector<FormattedToken>& tokens;
    const SyntaxNode* currentNode = nullptr;

    explicit TokenCollector(std::vector<FormattedToken>& t) : tokens(t) {}

    void handle(const SyntaxNode& node) {
        const SyntaxNode* prev = currentNode;
        currentNode = &node;
        visitDefault(node);
        currentNode = prev;
    }

    void visitToken(Token tok) {
        if (!tok || tok.kind == TokenKind::EndOfFile || tok.kind == TokenKind::Placeholder) {
            return;
        }

        FormattedToken ft;
        ft.token = tok;
        ft.parentNode = currentNode;
        ft.text = std::string(tok.valueText());
        if (ft.text.empty() && tok.rawText().size() > 0) {
            ft.text = std::string(tok.rawText());
        }

        int consecutiveEols = 0;
        for (const auto& trivia : tok.trivia()) {
            if (trivia.kind == TriviaKind::EndOfLine) {
                consecutiveEols++;
            } else if (trivia.kind == TriviaKind::Whitespace) {
                // Ignore whitespace
            } else if (trivia.kind == TriviaKind::LineComment || trivia.kind == TriviaKind::BlockComment) {
                std::string commentText(trivia.getRawText());
                while (!commentText.empty() && (commentText.back() == '\r' || commentText.back() == '\n')) {
                    commentText.pop_back();
                }
                if (consecutiveEols > 0) {
                    ft.leadingComments.push_back(commentText);
                } else {
                    if (!tokens.empty()) {
                        tokens.back().trailingComment = commentText;
                    } else {
                        ft.leadingComments.push_back(commentText);
                    }
                }
                consecutiveEols = 0;
            } else if (trivia.kind == TriviaKind::Directive) {
                std::string dirText(trivia.getRawText());
                while (!dirText.empty() && (dirText.back() == '\r' || dirText.back() == '\n')) {
                    dirText.pop_back();
                }
                ft.leadingComments.push_back(dirText);
                consecutiveEols = 0;
            }
        }

        if (consecutiveEols > 1) {
            ft.blankLinesBefore = std::min(consecutiveEols - 1, 1);
        }
        if (consecutiveEols > 0 && !tokens.empty()) {
            ft.startsNewLine = true;
        }

        tokens.push_back(std::move(ft));
    }
};

} // anonymous namespace

class FormatterImpl {
public:
    explicit FormatterImpl(const FormatStyle& style) : style_(style) {}

    std::string format(std::string_view sourceText) {
        if (sourceText.empty()) return "";

        auto disabledRanges = CommentControls::findDisabledRanges(sourceText);

        auto tree = SyntaxTree::fromText(sourceText);
        const auto& root = tree->root();

        std::vector<FormattedToken> tokens;
        TokenCollector collector(tokens);
        root.visit(collector);

        if (tokens.empty()) {
            return std::string(sourceText);
        }

        annotateTree(root, tokens);
        processAlignments(tokens);

        return assembleLines(tokens, sourceText, disabledRanges);
    }

private:
    FormatStyle style_;

    void annotateTree(const SyntaxNode& root, std::vector<FormattedToken>& tokens) {
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& ft = tokens[i];
            const SyntaxNode* parent = ft.parentNode;

            if (i > 0) {
                const auto& prev = tokens[i - 1];

                // Rule 1: Semicolon ends declaration/statement
                if (prev.token.kind == TokenKind::Semicolon) {
                    bool inForHeader = false;
                    const SyntaxNode* p = prev.parentNode;
                    while (p) {
                        if (p->kind == SyntaxKind::ForLoopStatement) {
                            inForHeader = true;
                            break;
                        }
                        if (p->kind == SyntaxKind::SequentialBlockStatement) break;
                        p = p->parent;
                    }
                    if (!inForHeader) {
                        ft.startsNewLine = true;
                    }
                }
                // Rule 2: begin / fork
                else if (prev.token.kind == TokenKind::BeginKeyword ||
                         prev.token.kind == TokenKind::ForkKeyword) {
                    if (ft.token.kind != TokenKind::Colon) {
                        ft.startsNewLine = true;
                    }
                }
                // Rule 3: begin : label
                else if (i >= 2 && tokens[i - 2].token.kind == TokenKind::Colon &&
                         i >= 3 && (tokens[i - 3].token.kind == TokenKind::BeginKeyword || tokens[i - 3].token.kind == TokenKind::ForkKeyword)) {
                    ft.startsNewLine = true;
                }

                // Rule 4: Block end keywords
                if (ft.token.kind == TokenKind::EndKeyword ||
                    ft.token.kind == TokenKind::EndModuleKeyword ||
                    ft.token.kind == TokenKind::EndClassKeyword ||
                    ft.token.kind == TokenKind::EndFunctionKeyword ||
                    ft.token.kind == TokenKind::EndTaskKeyword ||
                    ft.token.kind == TokenKind::EndPackageKeyword ||
                    ft.token.kind == TokenKind::EndInterfaceKeyword ||
                    ft.token.kind == TokenKind::EndCaseKeyword ||
                    ft.token.kind == TokenKind::EndGenerateKeyword ||
                    ft.token.kind == TokenKind::JoinKeyword ||
                    ft.token.kind == TokenKind::JoinAnyKeyword ||
                    ft.token.kind == TokenKind::JoinNoneKeyword) {
                    ft.startsNewLine = true;
                }
            }

            int nodeIndent = calculateIndentForToken(ft.token, parent);

            if (i == 0 || ft.startsNewLine) {
                ft.lineIndent = nodeIndent;
                ft.spacesBefore = 0;
            } else {
                const auto& prev = tokens[i - 1];
                ft.spacesBefore = TokenAnnotator::spacesRequiredBetween(
                    prev.token, ft.token, prev.parentNode, ft.parentNode, style_
                );
            }
        }
    }

    int calculateIndentForToken(Token tok, const SyntaxNode* parent) {
        int indent = 0;
        const SyntaxNode* curr = parent;

        while (curr) {
            SyntaxKind kind = curr->kind;

            if (kind == SyntaxKind::ModuleDeclaration ||
                kind == SyntaxKind::InterfaceDeclaration ||
                kind == SyntaxKind::ClassDeclaration ||
                kind == SyntaxKind::ProgramDeclaration ||
                kind == SyntaxKind::PackageDeclaration) {
                if (tok.kind == TokenKind::ModuleKeyword ||
                    tok.kind == TokenKind::EndModuleKeyword ||
                    tok.kind == TokenKind::InterfaceKeyword ||
                    tok.kind == TokenKind::EndInterfaceKeyword ||
                    tok.kind == TokenKind::ClassKeyword ||
                    tok.kind == TokenKind::EndClassKeyword ||
                    tok.kind == TokenKind::ProgramKeyword ||
                    tok.kind == TokenKind::EndProgramKeyword ||
                    tok.kind == TokenKind::PackageKeyword ||
                    tok.kind == TokenKind::EndPackageKeyword) {
                    // Outer level
                } else {
                    indent += style_.indentation_spaces;
                }
            }
            else if (kind == SyntaxKind::FunctionDeclaration ||
                     kind == SyntaxKind::TaskDeclaration) {
                if (tok.kind == TokenKind::FunctionKeyword ||
                    tok.kind == TokenKind::EndFunctionKeyword ||
                    tok.kind == TokenKind::TaskKeyword ||
                    tok.kind == TokenKind::EndTaskKeyword) {
                    // Outer level
                } else {
                    indent += style_.indentation_spaces;
                }
            }
            else if (kind == SyntaxKind::SequentialBlockStatement) {
                if (tok.kind == TokenKind::EndKeyword || tok.kind == TokenKind::BeginKeyword) {
                    // Outer level
                } else {
                    indent += style_.indentation_spaces;
                }
            }
            else if (kind == SyntaxKind::ParallelBlockStatement) {
                if (tok.kind == TokenKind::JoinKeyword ||
                    tok.kind == TokenKind::JoinAnyKeyword ||
                    tok.kind == TokenKind::JoinNoneKeyword ||
                    tok.kind == TokenKind::ForkKeyword) {
                    // Outer level
                } else {
                    indent += style_.indentation_spaces;
                }
            }
            else if (kind == SyntaxKind::CaseStatement) {
                if (tok.kind == TokenKind::EndCaseKeyword ||
                    tok.kind == TokenKind::CaseKeyword ||
                    tok.kind == TokenKind::CaseXKeyword ||
                    tok.kind == TokenKind::CaseZKeyword) {
                    // Outer level
                } else {
                    indent += style_.indentation_spaces;
                }
            }
            else if (kind == SyntaxKind::GenerateRegion ||
                     kind == SyntaxKind::GenerateBlock) {
                if (tok.kind == TokenKind::GenerateKeyword ||
                    tok.kind == TokenKind::EndGenerateKeyword) {
                    // Outer level
                } else {
                    indent += style_.indentation_spaces;
                }
            }
            else if (kind == SyntaxKind::AnsiPortList ||
                     kind == SyntaxKind::ParameterPortList ||
                     kind == SyntaxKind::HierarchicalInstance) {
                if (tok.kind == TokenKind::CloseParenthesis ||
                    tok.kind == TokenKind::OpenParenthesis) {
                    // Outer level
                } else {
                    indent += style_.wrap_spaces;
                }
            }
            else if (kind == SyntaxKind::ConditionalStatement) {
                auto cond = curr->as_if<ConditionalStatementSyntax>();
                if (cond) {
                    if (cond->statement.get() == curr || (cond->elseClause && cond->elseClause->clause == curr)) {
                        if (curr->kind != SyntaxKind::SequentialBlockStatement) {
                            indent += style_.indentation_spaces;
                        }
                    }
                }
            }

            curr = curr->parent;
        }

        return indent;
    }

    std::vector<std::pair<size_t, size_t>> getLines(const std::vector<FormattedToken>& tokens) {
        std::vector<std::pair<size_t, size_t>> lines;
        if (tokens.empty()) return lines;
        size_t start = 0;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i].startsNewLine) {
                lines.push_back({start, i});
                start = i;
            }
        }
        lines.push_back({start, tokens.size()});
        return lines;
    }

    void processAlignments(std::vector<FormattedToken>& tokens) {
        auto lines = getLines(tokens);
        if (lines.empty()) return;

        // 1. Align Named Port Connections in instances: .port (expr)
        if (style_.named_port_alignment == AlignmentPolicy::kAlign) {
            size_t lineIdx = 0;
            while (lineIdx < lines.size()) {
                auto [start, end] = lines[lineIdx];
                if (start < end && tokens[start].token.kind == TokenKind::Dot) {
                    size_t grpStart = lineIdx;
                    size_t maxPortLen = 0;
                    while (lineIdx < lines.size()) {
                        auto [s, e] = lines[lineIdx];
                        if (s < e && tokens[s].token.kind == TokenKind::Dot) {
                            if (s + 1 < e && tokens[s + 1].token.kind == TokenKind::Identifier) {
                                maxPortLen = std::max(maxPortLen, tokens[s + 1].text.size());
                            }
                            lineIdx++;
                        } else {
                            break;
                        }
                    }
                    if (lineIdx - grpStart > 1 && maxPortLen > 0) {
                        for (size_t l = grpStart; l < lineIdx; ++l) {
                            size_t s = lines[l].first;
                            if (s + 2 < tokens.size() &&
                                tokens[s + 1].token.kind == TokenKind::Identifier &&
                                tokens[s + 2].token.kind == TokenKind::OpenParenthesis) {
                                int pad = static_cast<int>(maxPortLen - tokens[s + 1].text.size());
                                tokens[s + 2].spacesBefore = pad;
                            }
                        }
                    }
                } else {
                    lineIdx++;
                }
            }
        }

        // 2. Align Continuous Assignments: assign a = 1;
        if (style_.assignment_statement_alignment == AlignmentPolicy::kAlign) {
            size_t lineIdx = 0;
            while (lineIdx < lines.size()) {
                auto [start, end] = lines[lineIdx];
                if (start < end && tokens[start].token.kind == TokenKind::AssignKeyword) {
                    size_t grpStart = lineIdx;
                    size_t maxLhsLen = 0;
                    std::vector<size_t> equalsIndices;

                    while (lineIdx < lines.size()) {
                        auto [s, e] = lines[lineIdx];
                        if (s < e && tokens[s].token.kind == TokenKind::AssignKeyword) {
                            size_t eqIdx = s;
                            while (eqIdx < e && tokens[eqIdx].token.kind != TokenKind::Equals) {
                                eqIdx++;
                            }
                            if (eqIdx < e && tokens[eqIdx].token.kind == TokenKind::Equals) {
                                size_t lhsLen = 0;
                                for (size_t t = s + 1; t < eqIdx; ++t) {
                                    lhsLen += tokens[t].text.size() + tokens[t].spacesBefore;
                                }
                                maxLhsLen = std::max(maxLhsLen, lhsLen);
                                equalsIndices.push_back(eqIdx);
                            }
                            lineIdx++;
                        } else {
                            break;
                        }
                    }

                    if (equalsIndices.size() > 1 && maxLhsLen > 0) {
                        for (size_t eqIdx : equalsIndices) {
                            size_t s = eqIdx - 1;
                            while (s > 0 && tokens[s].token.kind != TokenKind::AssignKeyword) {
                                s--;
                            }
                            size_t lhsLen = 0;
                            for (size_t t = s + 1; t < eqIdx; ++t) {
                                lhsLen += tokens[t].text.size() + tokens[t].spacesBefore;
                            }
                            int pad = static_cast<int>(maxLhsLen - lhsLen);
                            tokens[eqIdx].spacesBefore = 1 + pad;
                        }
                    }
                } else {
                    lineIdx++;
                }
            }
        }

        // 3. Align Parameter Declarations: parameter int a = 1;
        if (style_.parameter_declaration_alignment == AlignmentPolicy::kAlign) {
            size_t lineIdx = 0;
            while (lineIdx < lines.size()) {
                auto [start, end] = lines[lineIdx];
                if (start < end && (tokens[start].token.kind == TokenKind::ParameterKeyword ||
                                    tokens[start].token.kind == TokenKind::LocalParamKeyword)) {
                    size_t grpStart = lineIdx;
                    size_t maxPrefixLen = 0;
                    std::vector<size_t> equalsIndices;

                    while (lineIdx < lines.size()) {
                        auto [s, e] = lines[lineIdx];
                        if (s < e && (tokens[s].token.kind == TokenKind::ParameterKeyword ||
                                      tokens[s].token.kind == TokenKind::LocalParamKeyword)) {
                            size_t eqIdx = s;
                            while (eqIdx < e && tokens[eqIdx].token.kind != TokenKind::Equals) {
                                eqIdx++;
                            }
                            if (eqIdx < e && tokens[eqIdx].token.kind == TokenKind::Equals) {
                                size_t prefixLen = 0;
                                for (size_t t = s; t < eqIdx; ++t) {
                                    prefixLen += tokens[t].text.size() + (t > s ? tokens[t].spacesBefore : 0);
                                }
                                maxPrefixLen = std::max(maxPrefixLen, prefixLen);
                                equalsIndices.push_back(eqIdx);
                            }
                            lineIdx++;
                        } else {
                            break;
                        }
                    }

                    if (equalsIndices.size() > 1 && maxPrefixLen > 0) {
                        for (size_t eqIdx : equalsIndices) {
                            size_t s = eqIdx - 1;
                            while (s > 0 && tokens[s].token.kind != TokenKind::ParameterKeyword &&
                                   tokens[s].token.kind != TokenKind::LocalParamKeyword) {
                                s--;
                            }
                            size_t prefixLen = 0;
                            for (size_t t = s; t < eqIdx; ++t) {
                                prefixLen += tokens[t].text.size() + (t > s ? tokens[t].spacesBefore : 0);
                            }
                            int pad = static_cast<int>(maxPrefixLen - prefixLen);
                            tokens[eqIdx].spacesBefore = 1 + pad;
                        }
                    }
                } else {
                    lineIdx++;
                }
            }
        }

        // 4. Align Case Items: ADD: res = a + b;
        if (style_.case_items_alignment == AlignmentPolicy::kAlign) {
            size_t lineIdx = 0;
            while (lineIdx < lines.size()) {
                auto [start, end] = lines[lineIdx];
                size_t colonIdx = start;
                while (colonIdx < end && tokens[colonIdx].token.kind != TokenKind::Colon) {
                    colonIdx++;
                }
                if (colonIdx < end && TokenAnnotator::isCaseItemColon(tokens[colonIdx].token, tokens[colonIdx].parentNode)) {
                    size_t grpStart = lineIdx;
                    size_t maxLabelLen = 0;
                    std::vector<size_t> colonIndices;

                    while (lineIdx < lines.size()) {
                        auto [s, e] = lines[lineIdx];
                        size_t cIdx = s;
                        while (cIdx < e && tokens[cIdx].token.kind != TokenKind::Colon) {
                            cIdx++;
                        }
                        if (cIdx < e && TokenAnnotator::isCaseItemColon(tokens[cIdx].token, tokens[cIdx].parentNode)) {
                            size_t labelLen = 0;
                            for (size_t t = s; t < cIdx; ++t) {
                                labelLen += tokens[t].text.size() + (t > s ? tokens[t].spacesBefore : 0);
                            }
                            maxLabelLen = std::max(maxLabelLen, labelLen);
                            colonIndices.push_back(cIdx);
                            lineIdx++;
                        } else {
                            break;
                        }
                    }

                    if (colonIndices.size() > 1 && maxLabelLen > 0) {
                        for (size_t cIdx : colonIndices) {
                            size_t s = cIdx;
                            while (s > 0 && !tokens[s].startsNewLine) {
                                s--;
                            }
                            size_t labelLen = 0;
                            for (size_t t = s; t < cIdx; ++t) {
                                labelLen += tokens[t].text.size() + (t > s ? tokens[t].spacesBefore : 0);
                            }
                            int pad = static_cast<int>(maxLabelLen - labelLen);
                            if (cIdx + 1 < tokens.size()) {
                                tokens[cIdx + 1].spacesBefore = 1 + pad;
                            }
                        }
                    }
                } else {
                    lineIdx++;
                }
            }
        }
    }

    std::string assembleLines(
        const std::vector<FormattedToken>& tokens,
        std::string_view originalSource,
        const std::vector<DisabledRange>& disabledRanges
    ) {
        std::ostringstream out;
        std::string currentLine;
        int currentLineIndent = 0;
        bool inNewLine = true;

        auto flushCurrentLine = [&]() {
            if (!currentLine.empty()) {
                while (!currentLine.empty() && currentLine.back() == ' ') {
                    currentLine.pop_back();
                }
                out << currentLine << "\n";
                currentLine.clear();
            }
            inNewLine = true;
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto& ft = tokens[i];

            if (ft.blankLinesBefore > 0) {
                flushCurrentLine();
                for (int b = 0; b < ft.blankLinesBefore; ++b) {
                    out << "\n";
                }
            }

            for (const auto& comment : ft.leadingComments) {
                flushCurrentLine();
                if (!comment.empty() && comment.front() == '`') {
                    out << comment << "\n";
                } else {
                    out << std::string(ft.lineIndent, ' ') << comment << "\n";
                }
            }

            if (ft.startsNewLine) {
                flushCurrentLine();
            }

            if (inNewLine) {
                currentLineIndent = ft.lineIndent;
                if (!ft.text.empty() && ft.text.front() == '`') {
                    currentLineIndent = 0;
                }
                currentLine.append(currentLineIndent, ' ');
                inNewLine = false;
            } else if (ft.spacesBefore > 0) {
                currentLine.append(ft.spacesBefore, ' ');
            }

            currentLine += ft.text;

            if (!ft.trailingComment.empty()) {
                currentLine += "  ";
                currentLine += ft.trailingComment;
                flushCurrentLine();
            }
        }

        flushCurrentLine();
        return out.str();
    }
};

std::string Formatter::format(std::string_view sourceText) {
    FormatterImpl impl(style_);
    return impl.format(sourceText);
}

bool Formatter::formatFile(const std::string& filePath, const FormatStyle& style, bool inPlace, std::string* output) {
    std::ifstream in(filePath);
    if (!in.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    Formatter formatter(style);
    std::string formatted = formatter.format(content);

    if (output) {
        *output = formatted;
    }

    if (inPlace) {
        std::ofstream out(filePath, std::ios::trunc);
        if (!out.is_open()) return false;
        out << formatted;
        out.close();
    }
    return true;
}

} // namespace svfmt

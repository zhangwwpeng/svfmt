#include "svfmt/FormatStyle.h"

namespace svfmt {

bool parseAlignmentPolicy(std::string_view str, AlignmentPolicy& policy) {
    if (str == "align") {
        policy = AlignmentPolicy::kAlign;
        return true;
    }
    if (str == "flush-left" || str == "flush_left") {
        policy = AlignmentPolicy::kFlushLeft;
        return true;
    }
    if (str == "preserve") {
        policy = AlignmentPolicy::kPreserve;
        return true;
    }
    if (str == "infer") {
        policy = AlignmentPolicy::kInfer;
        return true;
    }
    return false;
}

std::string_view toString(AlignmentPolicy policy) {
    switch (policy) {
        case AlignmentPolicy::kAlign: return "align";
        case AlignmentPolicy::kFlushLeft: return "flush-left";
        case AlignmentPolicy::kPreserve: return "preserve";
        case AlignmentPolicy::kInfer: return "infer";
    }
    return "align";
}

bool parseIndentationStyle(std::string_view str, IndentationStyle& style) {
    if (str == "wrap") {
        style = IndentationStyle::kWrap;
        return true;
    }
    if (str == "indent" || str == "indentation") {
        style = IndentationStyle::kIndentation;
        return true;
    }
    return false;
}

std::string_view toString(IndentationStyle style) {
    switch (style) {
        case IndentationStyle::kWrap: return "wrap";
        case IndentationStyle::kIndentation: return "indent";
    }
    return "wrap";
}

bool parseAlignmentGroupBoundary(std::string_view str, AlignmentGroupBoundary& boundary) {
    if (str == "none") {
        boundary = AlignmentGroupBoundary::kNone;
        return true;
    }
    if (str == "blank-lines" || str == "blank_lines") {
        boundary = AlignmentGroupBoundary::kBlankLines;
        return true;
    }
    if (str == "separator-comments" || str == "separator_comments") {
        boundary = AlignmentGroupBoundary::kSeparatorComments;
        return true;
    }
    if (str == "blank-lines-and-separator-comments" || str == "all") {
        boundary = AlignmentGroupBoundary::kBlankLinesAndSeparatorComments;
        return true;
    }
    return false;
}

std::string_view toString(AlignmentGroupBoundary boundary) {
    switch (boundary) {
        case AlignmentGroupBoundary::kNone: return "none";
        case AlignmentGroupBoundary::kBlankLines: return "blank-lines";
        case AlignmentGroupBoundary::kSeparatorComments: return "separator-comments";
        case AlignmentGroupBoundary::kBlankLinesAndSeparatorComments: return "blank-lines-and-separator-comments";
    }
    return "none";
}

} // namespace svfmt

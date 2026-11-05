#pragma once

#include <string>
#include <string_view>
#include <ostream>

namespace svfmt {

enum class AlignmentPolicy {
    kAlign,
    kFlushLeft,
    kPreserve,
    kInfer
};

enum class IndentationStyle {
    kWrap,
    kIndentation
};

enum class AlignmentGroupBoundary {
    kNone,
    kBlankLines,
    kSeparatorComments,
    kBlankLinesAndSeparatorComments
};

struct FormatStyle {
    int indentation_spaces = 2;
    int wrap_spaces = 4;
    int column_limit = 100;
    int over_column_limit_penalty = 100;
    int line_break_penalty = 2;

    // Alignment policies
    AlignmentPolicy port_declarations_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy named_port_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy named_parameter_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy parameter_declaration_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy module_net_variable_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy assignment_statement_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy case_items_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy struct_union_members_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy enum_assignment_statement_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy formal_parameters_alignment = AlignmentPolicy::kAlign;
    AlignmentPolicy distribution_items_alignment = AlignmentPolicy::kAlign;

    // Indentation styles
    IndentationStyle port_declarations_indentation = IndentationStyle::kWrap;
    IndentationStyle formal_parameters_indentation = IndentationStyle::kWrap;
    IndentationStyle named_parameter_indentation = IndentationStyle::kWrap;
    IndentationStyle named_port_indentation = IndentationStyle::kWrap;

    // Boundary settings
    AlignmentGroupBoundary alignment_group_boundary = AlignmentGroupBoundary::kBlankLinesAndSeparatorComments;

    // Options
    bool compact_indexing_and_selections = true;
    bool class_parameter_space = false;
    bool wrap_end_else_clauses = false;
    bool try_wrap_long_lines = true;

    // Helper to apply uniform alignment policy
    void applyToAllAlignmentPolicies(AlignmentPolicy policy) {
        port_declarations_alignment = policy;
        named_port_alignment = policy;
        named_parameter_alignment = policy;
        parameter_declaration_alignment = policy;
        module_net_variable_alignment = policy;
        assignment_statement_alignment = policy;
        case_items_alignment = policy;
        struct_union_members_alignment = policy;
        enum_assignment_statement_alignment = policy;
        formal_parameters_alignment = policy;
        distribution_items_alignment = policy;
    }

    int portDeclarationsIndentation() const {
        return port_declarations_indentation == IndentationStyle::kWrap ? wrap_spaces : indentation_spaces;
    }

    int formalParametersIndentation() const {
        return formal_parameters_indentation == IndentationStyle::kWrap ? wrap_spaces : indentation_spaces;
    }

    int namedParameterIndentation() const {
        return named_parameter_indentation == IndentationStyle::kWrap ? wrap_spaces : indentation_spaces;
    }

    int namedPortIndentation() const {
        return named_port_indentation == IndentationStyle::kWrap ? wrap_spaces : indentation_spaces;
    }
};

bool parseAlignmentPolicy(std::string_view str, AlignmentPolicy& policy);
std::string_view toString(AlignmentPolicy policy);

bool parseIndentationStyle(std::string_view str, IndentationStyle& style);
std::string_view toString(IndentationStyle style);

bool parseAlignmentGroupBoundary(std::string_view str, AlignmentGroupBoundary& boundary);
std::string_view toString(AlignmentGroupBoundary boundary);

} // namespace svfmt

#include "svfmt/Alignment.h"
#include <algorithm>
#include <sstream>

namespace svfmt {

std::vector<std::string> AlignmentTable::render(const FormatStyle& style) const {
    std::vector<std::string> output;
    if (rows.empty()) return output;

    // Split into groups based on alignment_group_boundary
    std::vector<std::vector<AlignmentRow>> groups;
    std::vector<AlignmentRow> current_group;

    bool break_on_blank = (style.alignment_group_boundary == AlignmentGroupBoundary::kBlankLines ||
                           style.alignment_group_boundary == AlignmentGroupBoundary::kBlankLinesAndSeparatorComments);
    bool break_on_sep = (style.alignment_group_boundary == AlignmentGroupBoundary::kSeparatorComments ||
                         style.alignment_group_boundary == AlignmentGroupBoundary::kBlankLinesAndSeparatorComments);

    for (const auto& r : rows) {
        if ((r.is_blank_line && break_on_blank) || (r.is_separator_comment && break_on_sep)) {
            if (!current_group.empty()) {
                groups.push_back(std::move(current_group));
                current_group.clear();
            }
            // Put separator/blank line in its own single-item group or as boundary
            groups.push_back({r});
        } else {
            current_group.push_back(r);
        }
    }
    if (!current_group.empty()) {
        groups.push_back(std::move(current_group));
    }

    // Now render each group
    for (const auto& grp : groups) {
        if (grp.size() == 1 && (grp[0].is_blank_line || grp[0].is_separator_comment)) {
            if (grp[0].is_blank_line) {
                output.emplace_back("");
            } else {
                std::string line(grp[0].indent, ' ');
                line += grp[0].trailing_comment.empty() && !grp[0].cells.empty() ? grp[0].cells[0].text : grp[0].trailing_comment;
                output.push_back(std::move(line));
            }
            continue;
        }

        // Calculate max width for each column in this group
        size_t max_cols = 0;
        for (const auto& row : grp) {
            max_cols = std::max(max_cols, row.cells.size());
        }

        std::vector<size_t> col_widths(max_cols, 0);
        for (const auto& row : grp) {
            for (size_t c = 0; c < row.cells.size(); ++c) {
                // If this is the last column in the row and there's no trailing comment or delimiter,
                // we don't strictly need to pad it, but to measure width:
                col_widths[c] = std::max(col_widths[c], row.cells[c].text.size());
            }
        }

        for (const auto& row : grp) {
            if (row.is_blank_line) {
                output.emplace_back("");
                continue;
            }

            std::string line(row.indent, ' ');
            for (size_t c = 0; c < row.cells.size(); ++c) {
                const auto& cell = row.cells[c];
                if (c > 0 && cell.min_spaces_before > 0) {
                    line.append(cell.min_spaces_before, ' ');
                }

                bool is_last_col = (c + 1 == row.cells.size());

                if (policy == AlignmentPolicy::kAlign) {
                    if (cell.align_right) {
                        if (col_widths[c] > cell.text.size()) {
                            line.append(col_widths[c] - cell.text.size(), ' ');
                        }
                        line += cell.text;
                    } else {
                        line += cell.text;
                        if (!is_last_col && col_widths[c] > cell.text.size()) {
                            line.append(col_widths[c] - cell.text.size(), ' ');
                        }
                    }
                } else {
                    // Flush left or preserve: no extra padding
                    line += cell.text;
                }
            }

            if (!row.trailing_comment.empty()) {
                // Verible style: at least 2 spaces before trailing comments
                if (!row.cells.empty()) {
                    line += "  ";
                }
                line += row.trailing_comment;
            }

            output.push_back(std::move(line));
        }
    }

    return output;
}

} // namespace svfmt

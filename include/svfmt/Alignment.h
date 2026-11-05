#pragma once

#include <string>
#include <vector>
#include <string_view>
#include "svfmt/FormatStyle.h"

namespace svfmt {

struct AlignmentCell {
    std::string text;
    int min_spaces_before = 1;
    bool align_right = false;

    AlignmentCell() = default;
    AlignmentCell(std::string t, int spaces = 1, bool right = false)
        : text(std::move(t)), min_spaces_before(spaces), align_right(right) {}
};

struct AlignmentRow {
    int indent = 0;
    std::vector<AlignmentCell> cells;
    std::string trailing_comment;
    bool is_blank_line = false;
    bool is_separator_comment = false;
};

class AlignmentTable {
public:
    AlignmentPolicy policy = AlignmentPolicy::kAlign;
    std::vector<AlignmentRow> rows;

    void addRow(AlignmentRow row) {
        rows.push_back(std::move(row));
    }

    bool empty() const { return rows.empty(); }
    void clear() { rows.clear(); }

    // Renders the table into lines of text according to the alignment policy and column widths
    std::vector<std::string> render(const FormatStyle& style) const;
};

} // namespace svfmt

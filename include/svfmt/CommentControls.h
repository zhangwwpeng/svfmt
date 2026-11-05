#pragma once

#include <string_view>
#include <vector>
#include <utility>
#include <cstdint>

namespace svfmt {

struct DisabledRange {
    size_t begin_offset;
    size_t end_offset;
};

class CommentControls {
public:
    static std::vector<DisabledRange> findDisabledRanges(std::string_view text);
    static bool isOffsetDisabled(size_t offset, const std::vector<DisabledRange>& ranges);
    static bool isLineDisabled(size_t lineStart, size_t lineEnd, const std::vector<DisabledRange>& ranges);
};

} // namespace svfmt

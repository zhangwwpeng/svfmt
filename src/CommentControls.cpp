#include "svfmt/CommentControls.h"
#include <algorithm>
#include <cctype>

namespace svfmt {

static bool startsWith(std::string_view sv, std::string_view prefix) {
    return sv.size() >= prefix.size() && sv.substr(0, prefix.size()) == prefix;
}

static std::string_view trimWhitespace(std::string_view sv) {
    while (!sv.empty() && (std::isspace(sv.front()) || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (std::isspace(sv.back()) || sv.back() == '\t')) {
        sv.remove_suffix(1);
    }
    return sv;
}

std::vector<DisabledRange> CommentControls::findDisabledRanges(std::string_view text) {
    std::vector<DisabledRange> ranges;
    size_t pos = 0;
    bool in_disabled_range = false;
    size_t disabled_start = 0;

    while (pos < text.size()) {
        // Line comment
        if (pos + 1 < text.size() && text[pos] == '/' && text[pos + 1] == '/') {
            size_t comment_start = pos;
            size_t line_end = text.find('\n', pos);
            if (line_end == std::string_view::npos) line_end = text.size();

            std::string_view comment_content = text.substr(pos + 2, line_end - (pos + 2));
            comment_content = trimWhitespace(comment_content);

            if (startsWith(comment_content, "verilog_format:")) {
                std::string_view cmd = trimWhitespace(comment_content.substr(15));
                if (startsWith(cmd, "off")) {
                    if (!in_disabled_range) {
                        in_disabled_range = true;
                        // Disabling starts after this comment line
                        disabled_start = (line_end < text.size()) ? line_end + 1 : text.size();
                    }
                } else if (startsWith(cmd, "on")) {
                    if (in_disabled_range) {
                        in_disabled_range = false;
                        // Disabling ends before this comment line
                        ranges.push_back({disabled_start, comment_start});
                    }
                }
            }
            pos = line_end;
        }
        // Block comment
        else if (pos + 1 < text.size() && text[pos] == '/' && text[pos + 1] == '*') {
            size_t comment_start = pos;
            size_t block_end = text.find("*/", pos + 2);
            if (block_end == std::string_view::npos) {
                pos = text.size();
                break;
            }
            std::string_view comment_content = text.substr(pos + 2, block_end - (pos + 2));
            comment_content = trimWhitespace(comment_content);

            if (startsWith(comment_content, "verilog_format:")) {
                std::string_view cmd = trimWhitespace(comment_content.substr(15));
                if (startsWith(cmd, "off")) {
                    if (!in_disabled_range) {
                        in_disabled_range = true;
                        disabled_start = block_end + 2;
                    }
                } else if (startsWith(cmd, "on")) {
                    if (in_disabled_range) {
                        in_disabled_range = false;
                        ranges.push_back({disabled_start, comment_start});
                    }
                }
            }
            pos = block_end + 2;
        }
        // Strings
        else if (text[pos] == '"') {
            pos++;
            while (pos < text.size()) {
                if (text[pos] == '\\' && pos + 1 < text.size()) {
                    pos += 2;
                } else if (text[pos] == '"') {
                    pos++;
                    break;
                } else {
                    pos++;
                }
            }
        } else {
            pos++;
        }
    }

    if (in_disabled_range) {
        ranges.push_back({disabled_start, text.size()});
    }

    return ranges;
}

bool CommentControls::isOffsetDisabled(size_t offset, const std::vector<DisabledRange>& ranges) {
    for (const auto& r : ranges) {
        if (offset >= r.begin_offset && offset < r.end_offset) {
            return true;
        }
    }
    return false;
}

bool CommentControls::isLineDisabled(size_t lineStart, size_t lineEnd, const std::vector<DisabledRange>& ranges) {
    for (const auto& r : ranges) {
        if (std::max(lineStart, r.begin_offset) < std::min(lineEnd, r.end_offset)) {
            return true;
        }
    }
    return false;
}

} // namespace svfmt

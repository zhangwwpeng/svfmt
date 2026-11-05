#pragma once

#include <string>
#include <string_view>
#include "svfmt/FormatStyle.h"

namespace svfmt {

class Formatter {
public:
    explicit Formatter(FormatStyle style = FormatStyle()) : style_(style) {}

    // Formats SystemVerilog source code according to the style rules
    std::string format(std::string_view sourceText);

    // Formats a file in-place or returns formatted text
    static bool formatFile(const std::string& filePath, const FormatStyle& style, bool inPlace, std::string* output = nullptr);

    const FormatStyle& getStyle() const { return style_; }
    void setStyle(const FormatStyle& style) { style_ = style; }

private:
    FormatStyle style_;
};

} // namespace svfmt

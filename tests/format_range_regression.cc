#include <iostream>
#include <string>
#include <string_view>

#include "verible/verilog/formatting/formatter.h"

int main() {
  using verilog::formatter::FormatVerilogRange;
  const std::string_view source = "module m;\nwire a;\nendmodule\n";
  std::string output = "old output";
  const auto beyond_eof = FormatVerilogRange(
      source, "range.sv", {}, &output, {1000000000, 1000000001});
  if (!beyond_eof.ok() || !output.empty()) {
    std::cerr << "Expected an empty result for a range past EOF: " << beyond_eof << '\n';
    return 1;
  }
  const auto zero = FormatVerilogRange(source, "range.sv", {}, &output, {0, 1});
  if (!absl::IsInvalidArgument(zero)) {
    std::cerr << "Expected a validation error for a zero-based line: " << zero << '\n';
    return 1;
  }
  const auto normal = FormatVerilogRange(source, "range.sv", {}, &output, {2, 3});
  if (!normal.ok() || output != "  wire a;\n") {
    std::cerr << "Unexpected output for the second line: " << normal << '\n' << output;
    return 1;
  }
}

#include <iostream>
#include <cassert>
#include <string_view>
#include "svfmt/Formatter.h"
#include "svfmt/CommentControls.h"

using namespace svfmt;

static void assertEqual(std::string_view actual, std::string_view expected, std::string_view testName) {
    if (actual != expected) {
        std::cerr << "FAILED test [" << testName << "]:\n"
                  << "--- EXPECTED ---\n" << expected
                  << "\n--- ACTUAL ---\n" << actual << "\n";
        std::exit(1);
    } else {
        std::cout << "PASSED test [" << testName << "]\n";
    }
}

void testBasicModule() {
    FormatStyle style;
    style.indentation_spaces = 2;
    Formatter fmt(style);

    std::string input = "module m;wire w;assign w=1;endmodule\n";
    std::string expected =
        "module m;\n"
        "  wire w;\n"
        "  assign w = 1;\n"
        "endmodule\n";

    std::string output = fmt.format(input);
    assertEqual(output, expected, "BasicModule");
}

void testNamedPortAlignment() {
    FormatStyle style;
    style.indentation_spaces = 2;
    style.wrap_spaces = 4;
    style.named_port_alignment = AlignmentPolicy::kAlign;
    Formatter fmt(style);

    std::string input =
        "module m;\n"
        "  foo bar (\n"
        "    .a(a),\n"
        "    .aa(aa),\n"
        "    .aaa(aaa)\n"
        "  );\n"
        "endmodule\n";

    std::string expected =
        "module m;\n"
        "  foo bar (\n"
        "      .a  (a),\n"
        "      .aa (aa),\n"
        "      .aaa(aaa)\n"
        "  );\n"
        "endmodule\n";

    std::string output = fmt.format(input);
    assertEqual(output, expected, "NamedPortAlignment");
}

void testContinuousAssignmentAlignment() {
    FormatStyle style;
    style.indentation_spaces = 2;
    style.assignment_statement_alignment = AlignmentPolicy::kAlign;
    Formatter fmt(style);

    std::string input =
        "module m;\n"
        "  assign a = 1;\n"
        "  assign long_name = 2;\n"
        "endmodule\n";

    std::string expected =
        "module m;\n"
        "  assign a         = 1;\n"
        "  assign long_name = 2;\n"
        "endmodule\n";

    std::string output = fmt.format(input);
    assertEqual(output, expected, "ContinuousAssignmentAlignment");
}

void testParameterAlignment() {
    FormatStyle style;
    style.indentation_spaces = 2;
    style.parameter_declaration_alignment = AlignmentPolicy::kAlign;
    Formatter fmt(style);

    std::string input =
        "module m;\n"
        "  parameter int a = 1;\n"
        "  parameter int aaa = 100;\n"
        "endmodule\n";

    std::string expected =
        "module m;\n"
        "  parameter int a   = 1;\n"
        "  parameter int aaa = 100;\n"
        "endmodule\n";

    std::string output = fmt.format(input);
    assertEqual(output, expected, "ParameterAlignment");
}

void testUnaryAndBinarySpacing() {
    FormatStyle style;
    style.indentation_spaces = 2;
    Formatter fmt(style);

    std::string input =
        "module m;\n"
        "  assign a = -1;\n"
        "  assign b = ~c;\n"
        "  assign d = !e;\n"
        "  assign f = g+h;\n"
        "  assign x = y[7:0];\n"
        "endmodule\n";

    std::string expected =
        "module m;\n"
        "  assign a = -1;\n"
        "  assign b = ~c;\n"
        "  assign d = !e;\n"
        "  assign f = g + h;\n"
        "  assign x = y[7:0];\n"
        "endmodule\n";

    std::string output = fmt.format(input);
    assertEqual(output, expected, "UnaryAndBinarySpacing");
}

void testCaseAlignment() {
    FormatStyle style;
    style.indentation_spaces = 2;
    style.case_items_alignment = AlignmentPolicy::kAlign;
    Formatter fmt(style);

    std::string input =
        "module m;\n"
        "  always_comb begin\n"
        "    case (op)\n"
        "      ADD: res = a + b;\n"
        "      SUBT: res = a - b;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";

    std::string expected =
        "module m;\n"
        "  always_comb begin\n"
        "    case (op)\n"
        "      ADD:  res = a + b;\n"
        "      SUBT: res = a - b;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n";

    std::string output = fmt.format(input);
    assertEqual(output, expected, "CaseAlignment");
}

void testTrailingCommentSpacing() {
    FormatStyle style;
    style.indentation_spaces = 2;
    Formatter fmt(style);

    std::string input =
        "module m;\n"
        "  wire a; // my comment\n"
        "endmodule\n";

    std::string expected =
        "module m;\n"
        "  wire a;  // my comment\n"
        "endmodule\n";

    std::string output = fmt.format(input);
    assertEqual(output, expected, "TrailingCommentSpacing");
}

void testCommentControls() {
    std::string text =
        "module m;\n"
        "  // verilog_format: off\n"
        "  wire   a   =   1;\n"
        "  // verilog_format: on\n"
        "  wire b = 2;\n"
        "endmodule\n";

    auto disabled = CommentControls::findDisabledRanges(text);
    assert(disabled.size() == 1);
    std::cout << "PASSED test [CommentControls]\n";
}

int main() {
    std::cout << "Running svfmt unit test suite...\n";
    testBasicModule();
    testNamedPortAlignment();
    testContinuousAssignmentAlignment();
    testParameterAlignment();
    testUnaryAndBinarySpacing();
    testCaseAlignment();
    testTrailingCommentSpacing();
    testCommentControls();
    std::cout << "\nALL TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}

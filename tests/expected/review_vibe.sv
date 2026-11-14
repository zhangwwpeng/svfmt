// Bugs from an external review (vibe.md).
// An object-like macro followed by "(": ordinary code, formatted.
`define ICG my_gate
module `ICG (
    input  logic clk,
    input  logic en,
    output logic clk_cg
);
endmodule

// An escaped macro name ends at whitespace.
`define A "aaa"
`define \B "bbb"
module escaped_macro;
  initial begin
    $display(`A);
    $display(`\B );
  end
endmodule

// `line with macro calls as arguments stays one line.
`define foo(filename) `"filename.vh`"
`define new_line_num 200
module line_args;
  initial begin
`line `new_line_num `foo(line) 1
    $display(`__FILE__, `__LINE__);
  end
endmodule

// A triple-quoted string with a directive-looking line inside.
module triple_quoted;
  string s = """
`ifdef FOO
""";
`ifdef BAR
  wire a;
`endif
endmodule

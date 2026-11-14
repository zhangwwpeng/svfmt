// Bugs from an external review (agy.md): crashes, silent changes, rejected
// output and layout. Each case formatted (and was checked) on its own.
`define END_BLOCK end
`define END_BLK blk end
`define MY_TYPE(T, W) T [W-1:0]
`define END_WHILE end while (0);
`define DECL(x) int x
`define STR(x) `"x`"
`define x(a, b) a /* comment */// * b
`ifdef MODE_A
typedef int u;
`define M(v) `"line1 \
  line2 v`"
`else
typedef int t;
`endif

// a block 'end' from a macro before 'else'
module crash_1_1;
  initial begin
    if (a) begin
      x = 1;
      `END_BLOCK
    else x = 2;
  end
endmodule

// a block label and 'end' from a macro
module crash_1_2;
  initial begin
    if (a) begin : `END_BLK
  end
endmodule

// a type from a macro call with arguments
module crash_1_3;
  `MY_TYPE(logic, 8) a;
endmodule

// a comment line after 'do', 'end while' from a macro
module crash_1_4;
  initial begin
    do
    // comment
    begin
      x = 1;
      `END_WHILE
  end
endmodule

// a declaration from a macro, ';' on the next line
class crash_1_6 extends uvm_object;
  `uvm_object_utils_begin(crash_1_6)
    `DECL(a)
    ;
  `uvm_object_utils_end
endclass

// a tab in a stringified macro argument is content
module silent_2_2;
  string s = `STR(hello	world);
endmodule

// `line keeps its own line (it numbers the next line)
module silent_2_3;
  initial begin
    y = a +
`line 100 "foo.sv" 0
        `__LINE__;
  end
endmodule

// a directive inside an aligned row is not joined with the row
module silent_2_4 (
    input logic [7:0]
`ifdef FOO
`endif
        a,
    input logic [3:0] bbb
);
endmodule

// ';' after a ')' with an end-of-line comment is kept
module rejected_3_1;
  function void f(
      input int a
  )  // c
  ;
  endfunction
endmodule

// operators inside [] are not glued into other operators
module rejected_3_2;
  assign y1 = a[b+ +c];
  assign y2 = a[b- -c];
  assign y3 = a[b& &c];
  assign y4 = a[b&& &c];
  assign y5 = a[b^ ~c];
endmodule

// a tab inside a triple-quoted string is content
module rejected_3_5;
  string s = """
	hello
""";
endmodule

// non-ASCII text before a continued end-of-line comment
module rejected_3_8;
  string s = "中文";  // c1
                    // c2
endmodule

// a comment inside an SV-2023 `ifdef expression
`ifdef (A && /* ) */ B)
module layout_4_1;
  wire a = 1;
endmodule
`endif

checker layout_4_2;
  bit x;
  rand bit a;
endchecker

class layout_4_3;
  rand bit mode, x;
  constraint c {
    mode -> {
      x == 1;
      x == 1;
    }
    mode -> x == 1;
  }
endclass

module layout_4_4;
  assign y = {<<8{a + b, c ? d : e}};
endmodule

module layout_4_5 (
    input pkg::word_t [7:0] a,
    input logic       [3:0] bb
);
endmodule

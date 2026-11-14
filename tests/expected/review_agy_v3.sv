// Review round agy_v3.
// An escaped macro name with arguments: the tab in the argument is content.
`define \STR (x) `"x`"
`define IF_BEGIN(c) if (c) begin
`define INIT_BEGIN initial begin
`define ASSERT assert
module m1;
  string s = `\STR (hello	world);
endmodule

// A delay before an unsized literal keeps its space.
module m2;
  initial begin
    a <= #1.5 'hff;
    b <= #10ns 'hff;
  end
endmodule

// A label kept on its own line before "if ... end else".
module m3;
  initial begin
    L :  // c1
    if (a) begin
      x = 1;
    end else begin
      x = 2;
    end
  end
endmodule

// A declaration after `include on the same line is not part of it.
module m4;
  `include "foo.svh"
  import pkg::*;
endmodule

// Function headers with $clog2 in a port or in the return type.
module m5;
  function automatic logic [7:0] my_very_long_func_name_1234567890(
      input logic [$clog2(PARAM_WIDTH)-1:0] port_a);
    return 0;
  endfunction
  function automatic logic [$clog2(PARAM_WIDTH_LONG_NAME)-1:0] my_very_long_function_name_without_any_parens;
    return 0;
  endfunction
endmodule

// A 'begin' from a macro.
module m6;
  `INIT_BEGIN
    `IF_BEGIN(a)
      x = 1;
      y = 2;
    end
    z = 3;
  end
endmodule

// An argument-less macro statement, and comments between uvm macros.
module m7;
  initial begin
    x = 1;
    // c
    `ASSERT ($stable(a));
    yy = 2;
  end
endmodule
class c8;
  int y;
  `uvm_object_utils_begin(c8)
    // c
    `uvm_field_int(x, UVM_ALL_ON)
  `uvm_object_utils_end
endclass

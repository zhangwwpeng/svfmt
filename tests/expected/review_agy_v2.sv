// agy_v2 / corpus regressions
`define ASSERT assert
module m1;
  always @(posedge i_clk)
    if (a) begin
      if (OPT_PIPELINED)
        // All but valid will be stable
        `ASSERT($stable(o_valid));
    end
endmodule

module m2;
  `FOO(
      `ifdef A
      1
  `endif
)
endmodule

module m3;
  int x =  /* l1
  l2 */ 1;  // c1
            // c2
endmodule

module m4;
  property p;
    @(posedge clk) a |-> b[->1:2] ##1 c[=1:2] ##1 d[*1:2];
  endproperty
endmodule

module m5;
  function automatic logic [$clog2(PARAM_WIDTH_LONG_NAME)-1:0] my_very_long_func_name(/* no args */);
    return 0;
  endfunction
endmodule

module m6;
  initial begin
    a = {<<8{x inside {1, 2}}};
    b = {<<byte{payload with [0+:len]}};
  end
  property p;
    s_eventually [1:3] c;
  endproperty
endmodule

module m7;
  always_comb begin
    case (sel)
      2'd0: y = 0;
      2'd1:
        case (sub)
          1'b0: y = 1;
          1'b1: y = 2;
        endcase
    endcase
  end
endmodule

module m8;
  typedef struct {
    int a = foo(
        1,  // comment
        2
    );
    int bbb = 3;
  } s_t;
endmodule

checker chk(input logic clk);
  // comment
  rand bit [7:0] x;
endchecker

module m9;
  initial begin
    L : if (a) begin
      x = 1;
    end else begin
      x = 2;
    end
  end
endmodule

module m_case;
  always_comb begin
    case (sel)
      2:
        if (a) y = 1;
        else y = 2;
      3: for (int i = 0; i < 2; i++) y = i;
    endcase
  end
endmodule

module m_eol;
  int a = foo(
      1,  // comment
      2
  );
  int bbb = 3;
  initial begin
    a = foo(
        1,  // c
        2
    );
    bbb = 3;
  end
endmodule

class c_uvm;
  `uvm_object_utils_begin(c)
    `uvm_field_int (branch_addr, UVM_DEFAULT | UVM_HEX)
    `uvm_field_int (enable, UVM_DEFAULT)
  `uvm_object_utils_end
endclass

module m_gen;
  generate
    case (W)
      8:
        wire [7:0] aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa = bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb;
      9:
        logic xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx = yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy;
    endcase
  endgenerate
endmodule

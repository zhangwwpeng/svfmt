// Style fixes from a review of random corpus files.
class c;
  function void f();
    // void' casts are statements of their own
    void'(uvm_config_db#(uvma_axi_cfg_c)::get(this, "", "cfg", cfg));
    void'(foo(this, "", "cfg", cfg));
  endfunction
endclass

module m (
  input  logic [7:0] a,  // first
  input  logic       bb  // second
);
  // trailing comments of aligned declarations are aligned too
  logic [7:0]               cmd_q;        // CMD register
  logic                     cmd_qe;       // CMD register enable
  otbn_pkg::err_bits_t      err_bits;     // Error bits

  initial begin
    // a timing control and 'begin' stay on the loop header's line
    repeat (800) @(posedge clk, negedge clk) begin
      in <= $random;
    end
    forever @(posedge clk) begin
      in <= 1;
    end
  end

foo u (
    .clk(clk),

    /*
     * multi-line block comments move as a whole
     */
    .a(a)
);
	/* tab-indented continuation lines are left alone
	 * x */
  wire v;
endmodule

// packed dimensions after a user-defined type get their own column
module user_type_ports (
  input  logic                          clk_i,
  output axi_req_t    [NoMstPorts-1:0]  mst_reqs_o,
  input  axi_resp_t   [NoMstPorts-1:0]  mst_resps_i,
  input  logic        [7:0]             d
);
endmodule

// one declaration per line: constraint prototypes, timeunit, extern methods
class proto_c;
  extern constraint a_c;
  extern constraint b_c;
endclass

interface forkjoin_if;
  extern forkjoin task countSlaves();
  extern forkjoin task Read(input int a);
endinterface

module assertion_items;
  timeunit 1ns;
  timeprecision 1ps;
  // a short sequence keeps its endsequence on its own line
  sequence abc;
    @(posedge clk) a ##1 b;
  endsequence
  covergroup cg;
    cross a, b {
      ignore_bins ignore = binsof (a) intersect {5};
    }
  endgroup
  initial begin
    randsequence (main)
      main : first := 1 | second := 0;
      first : { some_long_variable_name = another_long_function_name(argument_one, argument_two); y = 2; };
      second: repeat (10) third;
    endsequence
  end
endmodule

// the dimensions of an assigned type stay with the type
module type_param_dims #(
  parameter int unsigned NoMstPorts  = 32'd2,
  parameter int unsigned SelectWidth = (NoMstPorts > 32'd1) ? $clog2(NoMstPorts) : 32'd1,
  parameter type         select_t    = logic [SelectWidth-1:0]
) ();
endmodule

// a row with a struct body is not aligned with its neighbours (that would
// put the body on one line)
module struct_param_rows;
  localparam type a_t = `A_T(Cfg);
  localparam type rvfi_probes_t = struct packed {
    logic               csr;
    rvfi_probes_instr_t instr;
  };
endmodule

// a parameter or port list with at most one element stays on one line when
// it fits; any longer list expands the whole header
class one_param_c #(type T=int);
endclass
class one_named_c extends cip_base_env_cfg #(.RAL_T(rram_ctrl_core_reg_block));
endclass
class two_param_c #(type T=int, int W=8) extends base #(T);
endclass
module one_each #(parameter W=8) (input a);
  foo #(.W(8)) u (.a(a));
  foo u2 (.a(a), .b(b));
endmodule
module one_param_two_ports #(parameter W=8) (input a, output b);
endmodule
module no_ports #(parameter W = 8) ();
endmodule
module commented_port (
  input clk  // the clock
);
endmodule
module long_one #(parameter int unsigned SOME_VERY_LONG_PARAMETER_NAME_THAT_IS_REALLY_LONG = 32'd12345678) (input logic clk_with_a_long_name);
endmodule

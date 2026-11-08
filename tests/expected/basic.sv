`timescale 1ns / 1ps
`define WIDTH 8
`define MAX(a, b) ((a) > (b) ? (a) : (b))

// verilog_format: off
module   keep_me ( input a ); endmodule
// verilog_format: on

module counter #(
    parameter int W = `WIDTH
) (
    input logic clk,
    rst_n,
    output logic [W-1:0] q
);
  logic [W-1:0] next;  // trailing comment
  assign next = `MAX(q + 1'b1, '0);
`ifdef SIM
  initial $display("sim");
`else
  initial begin
  end
`endif
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) q <= '0;
    else q <= next;
endmodule

`timescale 1ns/1ps
module counter #(parameter int W = 8, parameter X=2) (
  input  logic clk, rst_n,
  output logic [W-1:0] q
);
  logic [W-1:0] next;   // trailing comment
  wire a,b;
  assign next = q+1'b1;
  initial begin end
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) q <= '0;
    else q<=next;
endmodule

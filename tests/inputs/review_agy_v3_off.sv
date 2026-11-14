// verilog_format: off
module m(
`ifndef A
  input   a
`else
  input   a,
  input   b
`endif
);
`ifdef A
  logic     x   =   1;
`endif
endmodule
// verilog_format: on
module n;
`ifdef A
  logic     y   =   1;
`endif
endmodule

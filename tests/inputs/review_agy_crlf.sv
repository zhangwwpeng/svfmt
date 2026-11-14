// CRLF cases from an external review (agy.md).
`define M(a) `"a \
	b`"
module m;
  // verilog_format: off
  assign a=b;
  // verilog_format: on
  assign c = d;
endmodule

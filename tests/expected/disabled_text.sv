// An inactive branch that does not parse on its own (so every branch cannot
// be parsed together) stays verbatim; the `endif after it must be kept.
module m;
  initial begin
`ifdef A
    if (w) begin
`endif
  end
`ifdef B
  foo
`endif
endmodule

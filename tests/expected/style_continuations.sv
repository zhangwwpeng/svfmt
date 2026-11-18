module m;
  initial begin
    a = 1;
  end;
  function void f;
  endfunction;
  function void named;
  endfunction : named;
  initial begin
  end
  ;
  localparam X = (A || B) ? P : (C ? ((P + 1) / 2)  // half
      : 0);  // otherwise
  property p;
    logic tmp;
    int count;
    @(posedge clk) disable iff (!en) (en, tmp = q, count = 1) |=> q == tmp && count == 1;
  endproperty
  sequence s;
    logic sampled;
`ifdef EXTRA
    int tag;
`endif
    (en, sampled = q) ##1 sampled;
  endsequence
endmodule

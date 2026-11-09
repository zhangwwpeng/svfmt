// Style fixes from review round 1 (reports/style-review-1).
module m;
  covergroup cg;
    // a coverpoint or cross body with more than one member expands, one
    // level in; a single member still fits on one line
    cp_x: coverpoint b {
      option.weight = 1;  // c
      bins hit = {1};
    }
    cp_y: coverpoint b {option.weight = 1; bins hit = {1};}
    cp_z: coverpoint b {bins hit = {1};}
    cross a, b {
      option.weight = 1;
      ignore_bins i = binsof (a);
    }
  endgroup

  // qualifiers stay with the type of an expanded declaration
`ifdef DSIM
  privileged_reg_t implemented_csr[] = {
`else
  const privileged_reg_t implemented_csr[] = {
`endif
    A,  // c
    B
  };

  always @(posedge clk)
    if (r) x <= 0;
    // a concatenation stays in the case header
    else case ({w_write, w_read})
      2'b01: x <= 1;
      default: x <= 0;
    endcase

  always forever begin
    #5;
    v = v + 1;
  end

  initial begin
    if (a) begin
      x <= 0;
    end
    else if (b) begin
      x <= 1;
    end else if (c) begin
      x <= 2;
    end
  end
endmodule

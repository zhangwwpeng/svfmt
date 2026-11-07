module top;
  foo #(.W(8), .D(2)) u_foo (.clk(clk), .rst(rst), .data(data));
  bar u_bar(.*);
  baz u_baz (a, b, c);
  generate
    for (genvar i=0;i<4;i++) begin : g_loop
      assign y[i] = x[i];
    end
  endgenerate
endmodule

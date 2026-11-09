module m;
  specify
    $period(edge[01, x1, 1Z] clk, 42, notify);
    $setup(posedge a, edge[01, 0x] b, 1);
  endspecify
endmodule

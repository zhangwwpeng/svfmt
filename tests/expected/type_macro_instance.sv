module m;
  // cell
  `RV_CELL
  bank_hi0 (
      .CLK(clk),
      .D  (d)
  );
  initial
    assert (0) `stop;
    else $stop;
endmodule

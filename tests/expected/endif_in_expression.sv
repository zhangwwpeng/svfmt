module m;
  assign x = a +
    `ifdef F
    b ^
    `endif

    c;
endmodule

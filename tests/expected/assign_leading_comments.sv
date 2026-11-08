module m;
  generate
    if (A_WIDTH <= 4) begin
      if (A_WIDTH == 4) assign Ax = A;
      else
        // c1
        // c2
        // c3
        assign Ax = {
          A[1], A
        };
    end
  endgenerate
endmodule

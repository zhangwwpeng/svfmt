module m;
  initial begin
    L: (* keep = 1 *) begin
      x = 1;
    end : L
    L2: (* a *) x = 2;
  end
endmodule

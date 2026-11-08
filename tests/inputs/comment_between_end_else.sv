module m;
  always_ff @(posedge c) begin
    if (a) begin
      x <= 1;
    end
    // keep
    else
      x <= x;
  end
endmodule

module m;
  always_comb begin
    case (sel)
      A: begin
        x = 1;
      end
      `else  // narrow
      // FILTER
      B: begin
        x = 3;
      end
      `endif
    endcase
  end
endmodule

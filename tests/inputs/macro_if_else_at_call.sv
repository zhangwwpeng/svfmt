module m;
  `define CMP if (a !== b) e = 1;
  initial begin
    `CMP
    else z = 1;
  end
endmodule

`pragma protect key_keyowner="foo"	// t
module m;
endmodule

`define M(a) a
module m2;
  int x = `M
  (a +	b);
endmodule

module m3;
  wire [7:0] a;
  wire [3:0] #5 bbbb;
  wire [1:0] c;
endmodule

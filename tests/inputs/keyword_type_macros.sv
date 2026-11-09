module m;
  `MYMODE property (@(posedge clk) a |-> b);
  `ASSERT_KIND property (@(posedge clk) a) else $error("x");
  `uvm_info("a", "b", UVM_LOW)
  `TYPE_T [3:0] arr [2];
  wire `W w;
endmodule

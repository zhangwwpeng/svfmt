`define FOO(a, b) (a + b)
module m;
  initial begin
    `uvm_info("TAG", "msg", UVM_LOW)
    x = `FOO(1, 2);
  end
endmodule

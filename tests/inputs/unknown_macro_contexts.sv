`define MY_CHECK(x) if (!(x)) `uvm_fatal("chk", "failed")
class c extends uvm_object;
  int a;
  `uvm_object_utils_begin(c)
    `uvm_field_int(a, UVM_DEFAULT)
  `uvm_object_utils_end
  `uvm_object_new
  constraint k {
    `CLK_CONSTRAINT(a)
    `CALC(a, 2) < 2 ** 4;
  }
  function void f();
    ////////////////////////
    `uvm_info(`gfn, "x", UVM_LOW)
    `MY_CHECK(a == 1)
    wait (a inside `RANGE);
  endfunction
endclass

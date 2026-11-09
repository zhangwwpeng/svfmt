`include "vunit_defines.svh"

module tb_example;
   `TEST_SUITE begin
      `TEST_SUITE_SETUP begin
         $display("setup");
      end
      `TEST_CASE("Test that passes") begin
         $display("pass");
      end
   end
   `WATCHDOG(10ms);
endmodule

`define TRY(a, b) if (1 != $cast(a, b)) $stop
module `CELL_NAME
  (input a);
  `CELL_NAME u (.a(a));
  initial begin
    `TRY(x, y);
  end
  function void f();
    `uvm_info(`gfn, "start", UVM_LOW)

    // a block
    begin
      int i;
    end
  endfunction
endmodule

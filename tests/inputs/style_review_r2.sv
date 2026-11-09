// Style fixes from review round 2 (reports/style-review-2).
class c;
  rand bit [5:0] reserved_c;
  // the right side of "->" moved to its own line keeps the indentation
  // (the line is too long to wrap, so its other spacing is kept)
  constraint k {
    if (e) {
      (reserved_c == kReservedAddi16sp)  -> ((c_msb == 3'b011) && (c_op == 2'b01) &&
                                             (instr_bin[11:7] == 2) &&
                                             !instr_bin[12] && (instr_bin[6:2] == 0));
    }
  }
  // a short argument list stays on the line of "randomize(...) with {"
  function void f();
    void'(std::randomize(destination) with {
      destination.x inside {[0 : configuration.size_x - 1]};
      destination.y inside {[0 : configuration.size_y - 1]};
    });
    assert (req.randomize(a) with {
      a == 1;
      b == 2;
    });
    void'(this.randomize(addr, data) with {a == 1; b == 2;});
  endfunction
endclass

// tabs in a line kept as it was become spaces up to the next indentation
// stop (here 2 columns), not one space each
module tabs;
	always @(posedge clock) begin
		if (rvfi_valid[channel_idx] && rvfi_order[64*channel_idx +: 64] > insn_order_long_name &&
				(register_index == rvfi_rs1_addr[channel_idx*5 +: 5] ||
				register_index == rvfi_rs2_addr[channel_idx*5 +: 5])) begin
		end
	end
endmodule

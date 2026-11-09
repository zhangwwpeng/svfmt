module m;
  assign a = c ? {1'b0, b} : {d,  // data
      `W'b0,  // size
      `T,  // id
      `X'b0,  // thr
      e};
  always @(posedge clk `OR_ASYNC_RST)
    if (rst) q <= 0;
    else q <= d;
  function automatic logic [get_width(CONFIG)-1:0] pack_payload_with_a_long_name(
      packet_type_t packet_type);
    return 0;
  endfunction
  covergroup cg @(posedge clk);
    cp_a: coverpoint a;

    `SEEN(x, y)
    `SEEN(z, w)
  endgroup
endmodule

class c;
  task t();
    foreach (fifos[i]) `CHECK_EMPTY(fifos[i]);
    `uvm_info(`gfn, "trigger", UVM_DEBUG)
    -> item.sampled;
    if (c) `CHECK_EMPTY(x)
  endtask
endclass

module spi;
  always @(*)
    case (adr)
`ifdef WIDE
      `RX_0:   d = rx[31:0];
`else
      `RX_0:   d = rx[15:0];
`endif
      default: d = 0;
    endcase
endmodule

module widths;
  `rand_reg
  [63:0] o;
  reg [`XLEN   - 1:0] a;
  reg [`XLEN/8 - 1:0] b;
endmodule

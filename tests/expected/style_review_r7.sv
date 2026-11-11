// Style fixes from review round 7 (reports/style-review-7).
// The item of a generate case item on the line after its label is indented.
module m;
  case (x)
    32: assign a = b;
    default:
      initial
        assert (x == -1) else begin
          $error("bad");
        end
  endcase
  if (x) begin : g
  end else
    initial begin
      $display("x");
    end
endmodule

// "`M(...) else `M(...) else begin" where `M expands to an if: the chain
// stays flat, like an else-if chain.
`define bsg_mem_1rw_sync_macro(words,bits,lgEls,newBits,mux)    \
if (els_p == words && width_p == bits)                          \
  begin: macro                                                  \
     tsmc180_1rw_lg``lgEls``_w``newBits``_m``mux``_all mem      \
          (.Q(data_o)                                           \
           ,.CLK(clk_i)                                         \
           ,.CEN(~v_i)                                          \
           ,.WEN(~w_i)                                          \
           ,.A(addr_i)                                          \
           ,.D(data_i)                                          \
           // 1=tristate                                        \
           ,.OEN(1'b0)                                          \
           );                                                   \
  end

module m;
  `bsg_mem_1rw_sync_macro(2048, 32, 11, 32, 8)
  else `bsg_mem_1rw_sync_macro(1024, 32, 10, 32, 8)
  else `bsg_mem_1rw_sync_macro(256, 128, 8, 128, 4)
  else begin : z
    assign o = 0;
  end
endmodule

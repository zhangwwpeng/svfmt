module m
  import p::*;
#(parameter cfg_e cfg_p = e_default_cfg
  `declare_proc_params(cfg_p)
  `declare_if_widths(a_p, b_p)
  , parameter `INV_PARAM(width_p)
  , localparam lg_p = `SAFE_CLOG2(width_p)
  `declare_more(lg_p)
  , localparam w_lp = c_p - (d_p + e_p)
  `declare_cache(w_lp)
)
(input clk_i);
  logic [3:0] x = y[i+:`MAX(a_p, 1)];
  always_comb
    unique casez (op)
      `OP_A, `OP_B: x = 1;
      default: x = 0;
    endcase
endmodule

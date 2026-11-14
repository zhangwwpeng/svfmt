// A macro call between parameter declarations is not an alignment row: its
// argument list must not reserve a column (that padded the other rows and
// made `infer` alignment flip on the second pass).
module param_macro_alignment #(
    parameter bp_params_e bp_params_p = e_bp_default_cfg
    `declare_bp_proc_params(bp_params_p)

    // Derived parameters
    , localparam block_size_in_bytes_lp    = (bedrock_block_width_p / 8)
    , localparam lg_block_size_in_bytes_lp = `BSG_SAFE_CLOG2(block_size_in_bytes_lp)
) ();

  // The same for a macro call between struct members.
  typedef struct packed {
    logic       a;
    `DECL_FIELD(b)
    logic [3:0] c;
  } s_t;

endmodule

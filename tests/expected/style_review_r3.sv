// Style fixes from review round 3 (reports/style-review-3).
module m (
    input  logic [7:0] a,
    input  logic       bb,  // only commented port
    output logic       c
);
  // a lone trailing comment is not pushed to the end of the widest row
  wire tb_match;  // Verification
  wire tb_mismatch = ~tb_match;
  assign x = 1;

  // two or more trailing comments in a group are aligned
  logic                [7:0] cmd_q;     // CMD register
  logic                      cmd_qe;    // CMD register enable
  otbn_pkg::err_bits_t       err_bits;

  initial begin
    stim_cnt_limit = stim_cnt_limit_local;
    seq_runtime_us = 0;  // Disable timeout
  end

  // concatenations in a case header and its labels stay inline; the statement
  // of an item goes one level in when it is on the next line
  always_comb
    case ({lsu_op[2:0], addr})  // synopsys parallel_case
      {3'b010, 2'b00} :  // lbz
        sel_byte1 = `OR1200_M2R_ZERO;  // zero extend
      {3'b011, 2'b00} :  // lbs 0
        sel_byte1 = `OR1200_M2R_EXTB3;  // sign extend from byte 3
      2:
        y = {
          aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,
          bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,
          ccccccccccccccccccccccccccc
        };
    endcase
endmodule

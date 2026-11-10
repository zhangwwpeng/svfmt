// Style fixes from review round 5 (reports/style-review-5).
// Non-ANSI port declarations in a module body are aligned like ANSI ports.
module m (
    a,
    b,
    c
);
  output [31:0] jbi_sctag_req_d1;
  output [ 6:0] jbi_scbuf_ecc_d1;
  output        jbi_sctag_req_vld_d1;
  initial begin
    // a do-while block always expands; "end while (...)" stays one line
    do begin
      x = 1;
    end while (($system(command) == 1) && (max_count > 0));
  end
endmodule

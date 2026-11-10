// Style fixes from review round 6 (reports/style-review-6).
module m;
  // elaboration-time system tasks are formatted like calls
  if (SyncStages < 2) begin : gen_a
    $error("A minimum of 2 synchronizer stages is required for proper functionality.");
  end
  $error("A minimum of 2 synchronizer stages is required for proper functionality: %0d %0d", SyncStages, OtherParameter);
  // the dimensions after a typedef's name follow it with no space
  typedef int offsets_t[3:15];
  initial begin
    // the arguments of new(...) wrap like a call's
    automatic foo #(.A(int), .B(bit)) iti = new (
      a_long_argument_name, b_long_argument_name, c_long_argument_name, d_long_argument_name, e_long_argument_name
    );
    // a timing control and "if (...) begin" share a line
    @(posedge pck) if (rstn !== 1'b1) begin
      x = 1;
    end else begin
      x = 2;
    end
  end
endmodule

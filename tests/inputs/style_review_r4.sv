// Style fixes from review round 4 (reports/style-review-4).
// The extra names of an ANSI port ("input logic a, b") go under the names
// of the other ports when the ports are aligned.
module ports (
    input  wire logic [3:0] din_0,
                            din_1,
                            din_2,  // data in
    output logic      [3:0] dout_0,
    input                   clk
);
endmodule

// A lone unpacked dimension is not pushed to the end of the widest row.
class c;
  virtual task body;
    byte unsigned data[$];
    uint          data_bytes = ($urandom & 1) ? MaxPktSizeByte : 8;
    string        stim_desc;
    repeat (data_bytes) data.push_back(0);
  endtask
endclass

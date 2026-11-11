// A `define's trailing comment stays on its line whatever follows the
// define (another define, a blank line, an item or the end of the file).
`define GPIO_RGPIO_PTRIG  4'h4  // Address 0x10
`define GPIO_RGPIO_AUX    4'h5  // Address 0x14

module m;
  `define LOCAL_W 8  // width
  logic [`LOCAL_W-1:0] q;
endmodule
`define LAST 1  // last line
// A block comment at the end of a `define stays on the define's line too.
`define x(a, b) a /* comment */// * b
`define y(a, b) a /* comment *//*/ b*/

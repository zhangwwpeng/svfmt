interface foo; modport mp1(input  .a(sig), output  .b(sig)); endinterface
covergroup c with function sample(bit i); endgroup
module m;foo bar(.x(^~{a,b}));
clocking cb @(posedge clk); input a; output b; endclocking endmodule
class c; foo_pkg::bar_t#(.W(2)) baz; endclass

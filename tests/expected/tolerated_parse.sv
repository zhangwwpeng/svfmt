module foo #(
    parameter type baz_t = struct packed {`BAZ();},
    parameter type qux_t = struct packed {`QUX(a)}
) ();
endmodule : bar
typedef union soft packed {
  bit [3:0] a;
  bit [7:0] b;
} u_t;
module m;
  initial begin
    automatic const int a = 0;
  end
endmodule

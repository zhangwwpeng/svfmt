module foo; bar #(
`ifdef MM
.M(M)
`else
.N(N)
`endif
) bq (.bus(bus));endmodule

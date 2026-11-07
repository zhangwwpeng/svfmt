module m;
  always_comb begin
    case (sel)
      2'b00: y = a;
      2'b01: begin y = b; z = 1; end
      default: y = '0;
    endcase
    for (int i=0;i<4;i++) begin
      x[i] = i;
    end
    if (a) begin
      b = 1;
    end else if (c) begin
      b = 2;
    end else begin
      b = 3;
    end
  end
  function automatic int add(input int a, input int b);
    return a+b;
  endfunction
  task t; begin #10; $display("hi %d", a); end endtask
endmodule

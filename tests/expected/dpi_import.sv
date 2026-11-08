module mdi;
  import "DPI-C" function int add();
  import "DPI-C" function int sleep(input int secs);
  import "DPI-C" function int wake(
    input int secs,
    output bit [2:0] z
  );
endmodule

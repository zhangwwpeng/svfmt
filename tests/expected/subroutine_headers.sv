// Function headers wrap like a C function's (clang-format style): ports
// packed, continuation aligned after "(", or all ports on the next line when
// that gives fewer lines. Task headers that do not fit put one port per line
// with ");" on its own line, like a module's port list.
module m;
  function automatic void sv_dpi_aes_crypt_block(input bit impl_i, input bit op_i,
                                                 input bit [5:0] mode_i, input bit [3:0][31:0] iv_i,
                                                 input bit [2:0] key_len_i);
  endfunction
  function void f3(int aaaaaaaaaaaaaaaaaaaa, int bbbbbbbbbbbbbbbbbbbbbbbb,
                   int cccccccccccccccccccccccc, int dddddddddddddddddd);
  endfunction
  function int short_f(int a, int b);
    return a + b;
  endfunction
  task automatic sv_task_with_many_arguments(
      input bit impl_i,
      input bit op_i,
      input bit [5:0] mode_i,
      input bit [3:0][31:0] iv_i,
      input bit [2:0] key_len_i
  );
  endtask
  task t_short(int a, int b);
  endtask
  task t_one_long_port(
      input logic [SOME_PARAMETER_WIDTH-1:0] a_single_port_with_a_long_name_that_does_not_fit
  );
  endtask
endmodule
package p;
  function automatic void sv_dpi_aes_crypt_block(
      input bit impl_i, input bit op_i, input bit [5:0] mode_i, input bit [3:0][31:0] iv_i,
      input bit [2:0] key_len_i, input bit [7:0][31:0] key_i, input bit [3:0][31:0] data_i,
      output bit [3:0][31:0] data_o);
  endfunction
  import "DPI-C" function int c_add(
    input int a,
    input int b,
    input int ccccccccccccccccccc,
    input int dddddddddddddddddddd
  );
endpackage
class c;
  extern task run_phase_with_a_long_name(
      uvm_phase phase,
      int another_argument,
      int third_argument_here,
      int d
  );
  extern function void build_phase_with_a_long_name(uvm_phase phase, int another_argument,
                                                    int third_argument_here);
  pure virtual function void f_with_a_long_name(uvm_phase phase, int another_argument,
                                                int third_argument_here);
  extern function void short_proto(int a);
  extern task short_task_proto(int a, int b);
endclass

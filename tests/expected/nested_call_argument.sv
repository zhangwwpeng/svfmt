module nested_call_argument;
  initial begin
    printer.print_field("wait_count_write_address_channel",
                        axi4_cfg.wait_count_write_address_channel,
                        $size(axi4_cfg.wait_count_write_address_channel), UVM_DEC);
    printer.print_field("wait_count_write_address_channel",
                        axi4_cfg.wait_count_write_address_channel,
                        my_func(axi4_cfg.wait_count_write_address_channel), UVM_DEC);
  end
endmodule

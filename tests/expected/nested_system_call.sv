module nested_system_call;
  initial begin
    printer.print_field("wait_count_write_address_channel",
                        axi4_cfg.wait_count_write_address_channel,
                        $bits(axi4_cfg.wait_count_write_address_channel), UVM_DEC);
  end
endmodule

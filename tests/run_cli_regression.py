#!/usr/bin/env python3
"""Exercise CLI file handling, line selection, and formatting safety."""

import errno
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile


formatter = Path(sys.argv[1])
golden_runner = Path(sys.argv[2])


def format_stdin(source, *flags):
    return subprocess.run([formatter, "--failsafe_success=false", *flags, "-"],
                          input=source, capture_output=True, timeout=10)


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    target = root / "target.sv"
    link = root / "link.sv"
    original = b"module m;wire a;endmodule\n"
    expected = b"module m;\n  wire a;\nendmodule\n"
    target.write_bytes(original)
    target.chmod(0o640)
    link.symlink_to(target)

    result = subprocess.run([formatter, "--failsafe_success=false", "--inplace", link],
                            capture_output=True)
    assert result.returncode == 0, result.stderr
    assert target.read_bytes() == expected
    assert link.is_symlink()
    assert stat.S_IMODE(target.stat().st_mode) == 0o640
    assert not list(root.glob(".svfmt-*"))

    if hasattr(os, "geteuid") and os.geteuid() != 0:
        target.write_bytes(original)
        target.chmod(0o440)
        result = subprocess.run([formatter, "--failsafe_success=false", "--inplace", target],
                                capture_output=True)
        assert result.returncode != 0
        assert target.read_bytes() == original
        assert not list(root.glob(".svfmt-*"))

    long_path = root / ("a" * (os.pathconf(root, "PC_NAME_MAX") - 3) + ".sv")
    long_path.write_bytes(original)
    result = subprocess.run([formatter, "--inplace", long_path], capture_output=True)
    assert result.returncode == 0 and long_path.read_bytes() == expected, result

    hard_link = root / "hard_link.sv"
    shared = root / "shared.sv"
    shared.write_bytes(original)
    os.link(shared, hard_link)
    result = subprocess.run([formatter, "--inplace", shared], capture_output=True)
    assert result.returncode == 1 and b"hard links" in result.stderr, result
    assert os.path.samefile(shared, hard_link)
    assert shared.read_bytes() == original and hard_link.read_bytes() == original
    assert not list(root.glob(".svfmt-*"))

    metadata = root / "metadata.sv"
    metadata.write_bytes(original)
    if sys.platform == "darwin":
        name = "com.svfmt.test"
        subprocess.run(["xattr", "-w", name, "original", metadata], check=True)
        result = subprocess.run([formatter, "--inplace", metadata], capture_output=True)
        assert result.returncode == 0 and metadata.read_bytes() == expected, result
        assert subprocess.check_output(["xattr", "-p", name, metadata]).rstrip() == b"original"
    elif sys.platform == "linux":
        name = "user.svfmt.test"
        try:
            os.setxattr(metadata, name, b"original")
        except OSError as error:
            if error.errno != errno.ENOTSUP:
                raise
        else:
            result = subprocess.run([formatter, "--inplace", metadata], capture_output=True)
            assert result.returncode == 0 and metadata.read_bytes() == expected, result
            assert os.getxattr(metadata, name) == b"original"

    with target.open("rb") as readonly:
        result = subprocess.run([formatter, "--failsafe_success=false", "-"],
                                input=original, stdout=readonly, stderr=subprocess.PIPE)
    assert result.returncode == 1 and b"writing to stdout" in result.stderr, result

    inputs = root / "inputs"
    goldens = root / "goldens"
    inputs.mkdir()
    goldens.mkdir()
    (inputs / "one.sv").write_bytes(original)
    (goldens / "one.sv").write_bytes(expected)
    fake = root / "fake-formatter"
    fake.write_text("#!/bin/sh\nprintf 'module m;\\n  wire a;\\nendmodule\\n\\n'\n")
    fake.chmod(0o755)
    result = subprocess.run([golden_runner, fake, inputs, goldens],
                            capture_output=True)
    assert result.returncode != 0, "golden check accepted an extra final newline"

for source in (
    b'module m; initial $display("%d", `__LINE__); endmodule\n',
    b'`define LINE `__LINE__\nmodule m; initial $display("%d", `LINE); endmodule\n',
    b'`ifdef OFF\nmodule m; initial $display("%d", `__LINE__); endmodule\n'
    b'`else\nmodule m; endmodule\n`endif\n',
    b'`define LINE `__LINE__\n`define ALIAS(x) x + \\\n  `LINE\n'
    b'`ifdef OFF\nmodule m; initial $display("%d", `ALIAS(1)); endmodule\n'
    b'`else\nmodule m; endmodule\n`endif\n',
    b'`define ALIAS // continued \\\n  `__LINE__\n'
    b'`ifdef OFF\nmodule m; initial $display(`ALIAS); endmodule\n'
    b'`else\nmodule m; endmodule\n`endif\n',
):
    result = format_stdin(source)
    assert result.returncode == 1, result
    assert b"changes a macro expansion" in result.stderr, result
    assert result.stdout == source, "unsafe formatting was emitted"

source = (b'`define VALUE `__LINE__\n'
          b'`ifdef FIXED\n`undef VALUE\n`define VALUE 1\n`endif\n'
          b'module m; localparam int x = `VALUE; endmodule\n')
result = format_stdin(source, "--define=FIXED")
assert result.returncode == 1 and result.stdout == source, result
assert b"changes a macro expansion" in result.stderr, result

for source, expected in (
    (b"\xef\xbb\xbfmodule m;wire a;endmodule\n",
     b"\xef\xbb\xbfmodule m;\n  wire a;\nendmodule\n"),
    (b'`define VALUE `__LINE__\n`undef VALUE\n`define VALUE 1\n'
     b'module m; localparam int x = `VALUE; endmodule\n',
     b'`define VALUE `__LINE__\n`undef VALUE\n`define VALUE 1\n'
     b'module m;\n  localparam int x = `VALUE;\nendmodule\n'),
    (b'`define VALUE 1\nmodule m; localparam int x = `VALUE; endmodule\n'
     b'`undef VALUE\n`define VALUE `__LINE__\n',
     b'`define VALUE 1\nmodule m;\n  localparam int x = `VALUE;\nendmodule\n'
     b'`undef VALUE\n`define VALUE `__LINE__\n'),
    (b'module m;\ninitial $display("%d", `__LINE__);\nendmodule\n',
     b'module m;\n  initial $display("%d", `__LINE__);\nendmodule\n'),
    (b'module m; string msg = "`__LINE__"; endmodule\n',
     b'module m;\n  string msg = "`__LINE__";\nendmodule\n'),
    (b'module m; /* `__LINE__ */ endmodule\n',
     b'module m;  /* `__LINE__ */\nendmodule\n'),
):
    result = format_stdin(source)
    assert result.returncode == 0, result
    assert result.stdout == expected, result

result = format_stdin(b"\xef\xbb\xbfmodule m;\n  wire a;\nendmodule\n", "--verify")
assert result.returncode == 0 and result.stdout == b"", result
result = format_stdin(b"\xef\xbb\xbfmodule m;wire a;endmodule\n", "--dump_cst")
assert result.returncode == 0 and result.stdout.startswith(b"CompilationUnit"), result

for flag in ("--dump_cst", "--show_token_partition_tree",
             "--show_largest_token_partitions=1"):
    result = format_stdin(b"module m;wire a;endmodule\n", "--verify", flag)
    assert result.returncode == 1 and b"--verify cannot be combined" in result.stderr, result
    assert result.stdout == b"", result

source = b"module m;\nwire  a;\nendmodule"
for invalid in ("2147483647", "1-2147483647", "1-2-3", "0"):
    result = format_stdin(source, "--lines=" + invalid)
    assert result.returncode == 1 and b"--lines" in result.stderr, result
    assert result.stdout == b"", result

source = b"module m(input logic a, output logic b);\nendmodule\n"
for flag in ("--indentation_spaces", "--wrap_spaces"):
    result = format_stdin(source, flag + "=-1")
    assert result.returncode == 1 and flag.encode() in result.stderr, result
    assert result.stdout == b"", result

result = format_stdin(source, "--show_largest_token_partitions=-1")
assert result.returncode == 1 and b"--show_largest_token_partitions" in result.stderr, result
assert result.stdout == b"", result

for limit in ("0", "-1"):
    result = format_stdin(source, "--column_limit=" + limit)
    assert result.returncode == 1 and b"--column_limit must be positive" in result.stderr, result
    assert result.stdout == b"", result

for source, flags, expected in (
    (b"module sample;\n"
     b"  always_ff @(posedge clk) begin\n"
     b"    {first_destination, second_destination} <= {first_value, second_value} +\n"
     b"        (32 + SOME_WIDTH)'({input_high, input_low});\n"
     b"  end\n"
     b"endmodule\n",
     ('--column_limit=68',),
     b"module sample;\n"
     b"  always_ff @(posedge clk) begin\n"
     b"    {first_destination, second_destination} <=\n"
     b"        {first_value, second_value} +\n"
     b"        (32 + SOME_WIDTH)'({input_high, input_low});\n"
     b"  end\n"
     b"endmodule\n"),
    (b"package sample;\n"
     b"  typedef struct packed {int a; int b;} item;\n"
     b"  typedef item [6:0] items;\n"
     b"  function automatic items f(int n);\n"
     b"    case(n)\n"
     b"      1: return '{'{LongFirstIdentifier, 0}, '{LongSecondIdentifier, 1}, '{LongThirdIdentifier, 2},\n"
     b"                  '{LongFourthIdentifier, 3}, '{LongFifthIdentifier, 4}, '{LongSixthIdentifier, 5},\n"
     b"                  '{LongSeventhIdentifier, 6}};\n"
     b"      default: return '0;\n"
     b"    endcase\n"
     b"  endfunction\n"
     b"endpackage\n",
     ('--column_limit=85',),
     b"package sample;\n"
     b"  typedef struct packed {\n"
     b"    int a;\n"
     b"    int b;\n"
     b"  } item;\n"
     b"  typedef item [6:0] items;\n"
     b"  function automatic items f(int n);\n"
     b"    case (n)\n"
     b"      1:\n"
     b"        return '{\n"
     b"            '{LongFirstIdentifier, 0},\n"
     b"            '{LongSecondIdentifier, 1},\n"
     b"            '{LongThirdIdentifier, 2},\n"
     b"            '{LongFourthIdentifier, 3},\n"
     b"            '{LongFifthIdentifier, 4},\n"
     b"            '{LongSixthIdentifier, 5},\n"
     b"            '{LongSeventhIdentifier, 6}\n"
     b"        };\n"
     b"      default: return '0;\n"
     b"    endcase\n"
     b"  endfunction\n"
     b"endpackage\n"),
    (b"module m;\n"
     b"  initial begin\n"
     b"    assert (expected_value == read_response.data[0]) else\n"
     b"      `uvm_error(get_type_name(),\n"
     b"        $sformatf(\"A long error message with expected and actual values %0h %0h\",\n"
     b"          expected_value,read_response.data[0]));\n"
     b"  end\n"
     b"endmodule\n",
     ('--column_limit=80',),
     b"module m;\n"
     b"  initial begin\n"
     b"    assert (expected_value == read_response.data[0]) else\n"
     b"      `uvm_error(get_type_name(),\n"
     b"                 $sformatf(\"A long error message with expected and actual values %0h %0h\",\n"
     b"          expected_value,read_response.data[0]));\n"
     b"  end\n"
     b"endmodule\n"),
    (b"module m;\n"
     b"child bus[4]();\n"
     b"child #(.W(4)) bus2[4](.a(a));\n"
     b"virtual interface ifc endpoints[2];\n"
     b"endmodule\n",
     (),
     b"module m;\n"
     b"  child bus[4] ();\n"
     b"  child #(.W(4)) bus2[4] (.a(a));\n"
     b"  virtual interface ifc endpoints[2];\n"
     b"endmodule\n"),
    (b"module m (\n"
     b"  input logic a,\n"
     b"`ifdef A\n"
     b"`ifndef B\n"
     b"`ifndef C\n"
     b"  input logic b,\n"
     b"`endif\n"
     b"  input logic c,\n"
     b"`endif\n"
     b"`endif\n"
     b"`ifndef X\n"
     b"  output logic d\n"
     b"`else\n"
     b"  output logic e\n"
     b"`endif\n"
     b");\n"
     b"endmodule\n",
     (),
     b"module m (\n"
     b"    input  logic a,\n"
     b"`ifdef A\n"
     b"`ifndef B\n"
     b"`ifndef C\n"
     b"  input logic b,\n"
     b"`endif\n"
     b"  input logic c,\n"
     b"`endif\n"
     b"`endif\n"
     b"`ifndef X\n"
     b"    output logic d\n"
     b"`else\n"
     b"    output logic e\n"
     b"`endif\n"
     b");\n"
     b"endmodule\n"),
    (b"class sample;\n"
     b"task sample_op();\n"
     b"  record_event();\n"
     b"  if (control.get_auto_predict())\n"
     b"    // Update expected model.\n"
     b"    do_predict(rw, PREDICT_WRITE);\n"
     b"endtask\n"
     b"endclass\n",
     ('--indentation_spaces=4',),
     b"class sample;\n"
     b"    task sample_op();\n"
     b"        record_event();\n"
     b"        if (control.get_auto_predict())\n"
     b"            // Update expected model.\n"
     b"            do_predict(rw, PREDICT_WRITE);\n"
     b"    endtask\n"
     b"endclass\n"),
    (b"`define N 32\n"
     b"module m;\n"
     b"localparam int LANES=`N/8;\n"
     b"wire [`N/8-1:0] w;\n"
     b"initial for(i=0;i<`N/8;i++) one();\n"
     b"endmodule\n",
     (),
     b"`define N 32\n"
     b"module m;\n"
     b"  localparam int LANES = `N / 8;\n"
     b"  wire [`N/8-1:0] w;\n"
     b"  initial for (i = 0; i < `N / 8; i++) one();\n"
     b"endmodule\n"),
    (b"module m;\n"
     b"  l: assert property (ready)\n"
     b"    else `uvm_error (\"protocol\", \"long error message identifying the failed ready assertion\");\n"
     b"endmodule\n",
     ('--column_limit=80',),
     b"module m;\n"
     b"  l : assert property (ready) else\n"
     b"    `uvm_error (\"protocol\",\n"
     b"                \"long error message identifying the failed ready assertion\");\n"
     b"endmodule\n"),
    (b"`define WIDTH(a) $clog2(a)\n"
     b"module sample;\n"
     b"always_comb begin\n"
     b"  if (condition)\n"
     b"    out = (num_groups > 1) ? base_addr + pointer[LONG_HI_SYMBOL +: (num_groups > 1 ? `WIDTH(num_groups) : 1)] : base_addr;\n"
     b"  else\n"
     b"    out = '0;\n"
     b"end\n"
     b"endmodule\n",
     ('--column_limit=85',),
     b"`define WIDTH(a) $clog2(a)\n"
     b"module sample;\n"
     b"  always_comb begin\n"
     b"    if (condition)\n"
     b"      out = (num_groups > 1) ? base_addr + pointer[LONG_HI_SYMBOL+:(num_groups > 1\n"
     b"          ? `WIDTH(num_groups)\n"
     b"          : 1)] : base_addr;\n"
     b"    else out = '0;\n"
     b"  end\n"
     b"endmodule\n"),
    (b"class sample;\n"
     b"  rand int r;\n"
     b"  constraint c_small { r > 0; };\n"
     b"  constraint c_dist { r dist {1:=5, 2:=5}; };\n"
     b"endclass\n",
     (),
     b"class sample;\n"
     b"  rand int r;\n"
     b"  constraint c_small {r > 0;};\n"
     b"  constraint c_dist {\n"
     b"    r dist {\n"
     b"      1 := 5,\n"
     b"      2 := 5\n"
     b"    };\n"
     b"  };\n"
     b"endclass\n"),
    (b"`define EXT(a,b) (a)\n"
     b"module sample;\n"
     b"  always_comb begin\n"
     b"    out = (reason_code == 2)\n"
     b"               ? `EXT(first_signal_with_long_name, width_parameter_symbol)\n"
     b"               : `EXT(second_signal_with_long_name, width_parameter_symbol);\n"
     b"  end\n"
     b"endmodule\n",
     ('--column_limit=78',),
     b"`define EXT(a,b) (a)\n"
     b"module sample;\n"
     b"  always_comb begin\n"
     b"    out = (reason_code == 2)\n"
     b"        ? `EXT(first_signal_with_long_name, width_parameter_symbol)\n"
     b"        : `EXT(second_signal_with_long_name, width_parameter_symbol);\n"
     b"  end\n"
     b"endmodule\n"),
    (b"module sample;\n"
     b"enum logic [1:0] {A,B,C,D} state, next_state;\n"
     b"struct packed { logic a; logic b; } current_state, future_state;\n"
     b"enum {One, Two} single_state;\n"
     b"endmodule\n",
     (),
     b"module sample;\n"
     b"  enum logic [1:0] {\n"
     b"    A,\n"
     b"    B,\n"
     b"    C,\n"
     b"    D\n"
     b"  } state, next_state;\n"
     b"  struct packed {\n"
     b"    logic a;\n"
     b"    logic b;\n"
     b"  } current_state, future_state;\n"
     b"  enum {\n"
     b"    One,\n"
     b"    Two\n"
     b"  } single_state;\n"
     b"endmodule\n"),
    (b"module m;\n"
     b"  covergroup cg;\n"
     b"    cp: coverpoint (a.x()[15]);\n"
     b"  endgroup\n"
     b"endmodule\n",
     (),
     b"module m;\n"
     b"  covergroup cg;\n"
     b"    cp: coverpoint (a.x()[15]);\n"
     b"  endgroup\n"
     b"endmodule\n"),
    (b"module m(input logic clk,a,b);\n"
     b"  assert property (@(posedge clk) (a) [->1:2] |-> (b) [*2]);\n"
     b"endmodule\n",
     (),
     b"module m (\n"
     b"    input logic clk,\n"
     b"    a,\n"
     b"    b\n"
     b");\n"
     b"  assert property (@(posedge clk) (a)[->1:2] |-> (b)[*2]);\n"
     b"endmodule\n"),
    (b"module m;\ninitial begin\n"
     b"a = T '{x:1};\nb = pkg::T '{default:0};\nc = a + '{default:0};\n"
     b"end\nendmodule\n",
     (),
     b"module m;\n  initial begin\n"
     b"    a = T'{x: 1};\n    b = pkg::T'{default: 0};\n"
     b"    c = a + '{default: 0};\n  end\nendmodule\n"),
    (b"module m;\nalways_ff @(posedge clk) begin\n"
     b"@(negedge rst) // reset\nbegin\nx=0;\nend\nend\nendmodule\n",
     (),
     b"module m;\n  always_ff @(posedge clk) begin\n"
     b"    @(negedge rst)  // reset\n    begin\n      x = 0;\n    end\n"
     b"  end\nendmodule\n"),
    (b"module m;\ncovergroup cg;\n"
     b"coverpoint x { bins b = (1=>2[*3:5]=>3[=2]=>4[->2]); }\n"
     b"endgroup\nendmodule\n",
     (),
     b"module m;\n  covergroup cg;\n"
     b"    coverpoint x {bins b = (1 => 2[*3:5] => 3[=2] => 4[->2]);}\n"
     b"  endgroup\nendmodule\n"),
    (b"module m;\nalways @(posedge clk) fork\nx=0;\njoin_none\n"
     b"initial #5 // keep\nfork\nx=2;\njoin_any\nendmodule\n",
     (),
     b"module m;\n  always @(posedge clk) fork\n    x = 0;\n  join_none\n"
     b"  initial\n    #5  // keep\n    fork\n      x = 2;\n    join_any\nendmodule\n"),
    (b"module m;\nwire a; // first\n        // next\n\n"
     b"        // independent\nwire b;\nendmodule\n",
     (),
     b"module m;\n  wire a;  // first\n           // next\n\n"
     b"  // independent\n  wire b;\nendmodule\n"),
    (b"module m;\n`ifdef A\nlogic b; logic c;\n`endif\nendmodule\n",
     ("--line_terminator=CRLF",),
     b"module m;\r\n`ifdef A\r\n  logic b;\r\n  logic c;\r\n`endif\r\nendmodule\r\n"),
    (b'`define STR(x) `"x`"\nmodule m;\n'
     b'string s=`STR /* comment */ (hello\tworld);\nendmodule\n',
     (),
     b'`define STR(x) `"x`"\nmodule m;\n'
     b'  string s = `STR  /* comment */ (hello\tworld);\nendmodule\n'),
    (b'module m;\ntypedef struct {\nlogic [7:0] a;\n'
     b'`line 10 "very_long_filename_path.sv" 0\nlogic bbb;\n} s_t;\nendmodule\n',
     ("--struct_union_members_alignment=align",),
     b'module m;\n  typedef struct {\n    logic [7:0] a;\n'
     b'`line 10 "very_long_filename_path.sv" 0\n    logic       bbb;\n  } s_t;\nendmodule\n'),
    (b"module m;\n    /* l1\n       l2 */ int x = 1;  // c1\n"
     b"                         // c2\nendmodule\n",
     (),
     b"module m;\n  /* l1\n     l2 */ int x = 1;  // c1\n"
     b"                       // c2\nendmodule\n"),
    (b"module m (\ninput logic /* line 1\n"
     b"                 line 2 */ [7:0] a,\ninput logic [3:0] bbb\n);\nendmodule\n",
     ("--port_declarations_alignment=align",),
     b"module m (\n    input logic /* line 1\n"
     b"                 line 2 */ [7:0] a,\n    input logic [3:0] bbb\n);\nendmodule\n"),
    (b"module m;\r// verilog_format: off\rwire    a;\r"
     b"// verilog_format: on\rwire    b;\rendmodule\r",
     (),
     b"module m;\n  // verilog_format: off\nwire    a;\r"
     b"// verilog_format: on\n  wire    b;\nendmodule\n"),
    (b"module m;\r\rwire  b;\nendmodule\n", ("--lines=2",),
     b"module m;\r\r\nwire  b;\nendmodule\n"),
    (b"module m; assign abc = ddddddddddd + eeeeeeeeeeeee + ffffffffffff; endmodule\n",
     ("--line_terminator=CRLF", "--column_limit=28", "--try_wrap_long_lines"),
     b"module m;\r\n  assign abc = ddddddddddd +\r\n"
     b"      eeeeeeeeeeeee +\r\n      ffffffffffff;\r\nendmodule\r\n"),
    (b"module m;\nreg plain;\nreg [3:0] wide;\n"
     b"(* anyconst *) reg flag;\n(* anyconst *) reg [3:0] attr;\nreg tail;\nendmodule\n",
     ("--module_net_variable_alignment=align",),
     b"module m;\n  reg       plain;\n  reg [3:0] wide;\n"
     b"  (* anyconst *) reg       flag;\n  (* anyconst *) reg [3:0] attr;\n"
     b"  reg tail;\nendmodule\n"),
    (b"module m;\nwire  a;\nendmodule  ", ("--lines=2",),
     b"module m;\n  wire a;\nendmodule  "),
    (b"module m;\nwire  a;\nendmodule", ("--lines=2",),
     b"module m;\n  wire a;\nendmodule"),
    (b"module m;\nwire  a;\nendmodule", ("--lines=99",),
     b"module m;\nwire  a;\nendmodule"),
    (b"module m;\r\nwire  a;\r\nendmodule  \r\n", ("--lines=2",),
     b"module m;\r\n  wire a;\r\nendmodule  \r\n"),
    (b"module m;\rwire  a;\r\nwire  b;\nendmodule\n", ("--lines=2",),
     b"module m;\r  wire  a;\nwire  b;\nendmodule\n"),
    (b"module m;\rwire  a;\rendmodule\r", ("--lines=2",),
     b"module m;\r  wire a;\nendmodule\r"),
    (b"module m;\n/* comment\n * keep\n */\nendmodule\n", ("--lines=2",),
     b"module m;\n  /* comment\n * keep\n */\nendmodule\n"),
    (b"module m;\n  foo #(\n      .a(1)\n  )\n  // c\n  bar();\nendmodule\n",
     ("--lines=5",),
     b"module m;\n  foo #(\n      .a(1)\n  )\n  // c\n  bar();\nendmodule\n"),
):
    result = format_stdin(source, *flags)
    assert result.returncode == 0 and result.stdout == expected, result

# Check an untouched comment ending in bare CR inside a formatted excerpt
# that spans several lines of a parameter list.
fixture = (golden_runner.parent / "inputs" / "param_macro_alignment.sv").read_bytes()
terminators = (b"\r\n", b"\r", b"\n")
mixed = b"".join(line + terminators[i % 3] for i, line in enumerate(fixture.splitlines()))
lines = mixed.splitlines(keepends=True)
index = next(i for i, line in enumerate(lines) if line.strip() == b"} s_t;")
result = format_stdin(mixed, f"--lines={index + 1}")
assert result.returncode == 0, result
assert result.stdout.startswith(b"".join(lines[:index])), result
assert result.stdout.endswith(b"".join(lines[index + 1:])), result

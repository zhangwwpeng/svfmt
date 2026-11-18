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

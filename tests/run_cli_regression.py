#!/usr/bin/env python3
"""Exercise file replacement and byte-exact golden output comparison."""

import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile


formatter = Path(sys.argv[1])
golden_runner = Path(sys.argv[2])

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
    assert not list(root.glob("*.svfmt-*"))

    if hasattr(os, "geteuid") and os.geteuid() != 0:
        target.write_bytes(original)
        target.chmod(0o440)
        result = subprocess.run([formatter, "--failsafe_success=false", "--inplace", target],
                                capture_output=True)
        assert result.returncode != 0
        assert target.read_bytes() == original
        assert not list(root.glob("*.svfmt-*"))

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
    ):
        result = subprocess.run([formatter, "--failsafe_success=false", "-"],
                                input=source, capture_output=True)
        assert result.returncode != 0, result.stdout
        assert result.stdout == source, "unsafe formatting was emitted"

    stable_line = b'module m;\ninitial $display("%d", `__LINE__);\nendmodule\n'
    result = subprocess.run([formatter, "--failsafe_success=false", "-"],
                            input=stable_line, capture_output=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout != stable_line

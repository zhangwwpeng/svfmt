# svfmt

A SystemVerilog formatter: the [Verible](https://github.com/chipsalliance/verible)
formatter (`verible-verilog-format`) ported onto the
[slang](https://github.com/MikePopoloski/slang) front end.

Verible's formatting engine is kept: token partitioning, line-wrap search,
layout optimizer, tabular alignment, and comment controls. Its yacc parser is
replaced by slang, which supports all of IEEE 1800-2023. The command-line
flags are the same as `verible-verilog-format`'s.

## Status

- Formats modules, ports, parameters, instances, statements, functions and
  tasks, classes, packages, generate blocks, macros and `` `ifdef `` regions.
- Of 2111 inputs taken from Verible's own formatter tests, **1656 produce
  output byte-identical to `verible-verilog-format`**. 381 are rejected, most
  of them because they are not legal SystemVerilog (see
  [docs/deviations.md](docs/deviations.md)), and the rest differ. No input
  crashes.
- On failure, svfmt prints the input unchanged, like Verible. Failures
  include a parse error, a construct the bridge can't represent, or output
  that doesn't re-parse to the same tokens.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requirements:

- CMake ≥ 3.20 and a C++20 compiler.
- Abseil: an installed copy is used if found (e.g. `brew install abseil`);
  otherwise CMake fetches it.
- `verible-verilog-format` on `PATH`, only for the differential tests.

## Usage

```sh
build/svfmt file.sv                  # formatted output on stdout
build/svfmt --inplace a.sv b.sv
build/svfmt --verify file.sv         # exit 1 if the file would change
build/svfmt --column_limit=120 --port_declarations_alignment=align file.sv
build/svfmt --incdir=inc,rtl --define=SYNTHESIS file.sv   # svfmt-only
build/svfmt --helpfull               # all flags
```

Debug flags: `--dump_cst` (the slang-derived tree),
`--show_token_partition_tree`, `--show_inter_token_info`.

## Documentation

| Doc | Contents |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | Pipeline, source layout, and how slang's tree is turned into what Verible expects |
| [docs/preprocessor.md](docs/preprocessor.md) | Macros, `` `ifdef `` (all branches), undefined macros, excerpt parse modes |
| [docs/porting-guide.md](docs/porting-guide.md) | How Verible node types map to slang kinds; how to fix a formatting difference |
| [docs/testing.md](docs/testing.md) | Test suites and the differential workflow |
| [docs/deviations.md](docs/deviations.md) | Known, intentional differences from Verible |
| [docs/roadmap.md](docs/roadmap.md) | What's done, what's next |

## License

Code under `src/verible/` comes from Verible (Apache-2.0) and is modified;
see [NOTICE](NOTICE). slang is MIT.

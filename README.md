# svfmt

A SystemVerilog formatter: the [Verible](https://github.com/chipsalliance/verible)
formatter (`verible-verilog-format`) ported onto the
[slang](https://github.com/MikePopoloski/slang) front end.

Verible's formatting engine is kept: token partitioning, line-wrap search,
layout optimizer, tabular alignment, and comment controls. Its yacc parser is
replaced by slang, which supports all of IEEE 1800-2023. The command-line
flags are the same as `verible-verilog-format`'s.

## Status (v1.0)

- Formats modules, ports, parameters, instances, statements, functions and
  tasks, classes, packages, generate blocks, macros and `` `ifdef `` regions.
- Safety: every output is checked against the input with the
  re-parsed token stream and with the raw lexer (inactive `` `ifdef ``
  branches, `` `define ``/`` `pragma `` line ends, UDP tables). Macro
  expansions and `` `__LINE__ `` locations are also checked; whole-file
  formatting checks convergence. A file that cannot be formatted safely is
  printed unchanged.
- Corpus `../sv_test` (26,522 files, 01-09) with the default flags and with
  the nvim alignment flags: 0 semantic_change / not_idempotent / crash /
  timeout.
- `--try_wrap_long_lines` (off by default): 15 known not_idempotent files and
  1 timeout, all safe rejections (see
  [docs/handoff.md](docs/handoff.md) item 1).
- Differential preprocessor check against slang's own preprocessor (26,522
  files, several define sets): 0 differences. Mutation test of the safety
  checks (500-file sample): 0 failures.

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

svfmt is licensed under the Apache License 2.0; see [LICENSE](LICENSE) and
[NOTICE](NOTICE). Code under `src/verible/` comes from Verible (Apache-2.0)
and is modified; `src/external_libs/editscript.h` is from diff-match-patch
(Apache-2.0); slang is MIT.

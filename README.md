# svfmt

`svfmt` 是一个基于 **[slang](https://github.com/MikePopoloski/slang)** 具体语法树（CST）并完整复刻 **[Verible](https://github.com/chipsalliance/verible) (`verible-verilog-format`)** 规则体系的高性能 SystemVerilog 格式化工具。

---

## 特性与规则对标 (Verible Ruler in Slang)

1. **Token 间距规则 (`TokenAnnotator`)**:
   - 区分一元前缀操作符与二元操作符（如 `-1`, `~mask`, `!flag` 紧密贴合无空格；`a + b`, `x == y` 两侧保留空格）。
   - 括号/方括号/花括号内侧无多余空格 `(foo)`, `[0]`, `{a, b}`。
   - 逗号 `,` 前无空格、后加 1 空格；分号 `;` 前无空格、后换行。
   - 作用域操作符 `::` 与层级引用 `.` 无空格紧贴。
   - 紧凑位选切片模式（`compact_indexing_and_selections`），如 `[WIDTH-1:0]`, `[7:0]`。
   - 行尾注释前保证至少 2 个空格。

2. **纵向列对齐引擎 (`Alignment`)**:
   - **命名端口连接 (`named_port_alignment`)**: 对齐实例化中的端口名称与开括号，如 `.clk    (clk)`.
   - **连续赋值语句 (`assignment_statement_alignment`)**: 对齐 `assign` 语句中的 `=` 号。
   - **参数定义 (`parameter_declaration_alignment`)**: 对齐 `parameter`/`localparam` 中的类型、变量名与 `=` 赋值。
   - **分支选择项 (`case_items_alignment`)**: 对齐 `case` 表达式标签与冒号后的执行语句。
   - **对齐组边界 (`alignment_group_boundary`)**: 空行与分隔注释自动切分对齐块。

3. **语法树缩进驱动 (`TreeUnwrapper`)**:
   - 模块、类、接口、函数、任务、包、`begin...end`、`fork...join`、`case` 等语法块精确缩进。
   - 端口列表、参数列表使用 `wrap_spaces` 挂起缩进。
   - 连续空行压缩（最多保留 1 个空行）。

4. **格式化控制指令 (`CommentControls`)**:
   - 支持 `// verilog_format: off` 和 `// verilog_format: on`（以及 `/* ... */`），保护选定代码区域不被格式化。

---

## 编译与测试

### 环境依赖
- CMake >= 3.20
- C++20 兼容编译器（Clang / GCC）
- Python 3（slang 代码生成依赖）

### 编译构建
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 运行单元测试
```bash
./build/svfmt_test
```

---

## 命令行使用说明

```bash
# 格式化文件输出至控制台
./build/svfmt path/to/design.sv

# 原地覆写格式化
./build/svfmt -i path/to/design.sv

# 检查文件是否已规范格式化（CI 检查）
./build/svfmt --verify path/to/design.sv

# 从标准输入读取并格式化
cat design.sv | ./build/svfmt

# 自定义缩进与列宽
./build/svfmt --indentation_spaces 4 --wrap_spaces 4 --column_limit 120 -i design.sv
```

### 常用选项参数
| 参数 | 说明 | 默认值 |
|---|---|---|
| `-i, --inplace` | 直接覆写目标源文件 | `false` |
| `--indentation_spaces <n>` | 每层作用域基础缩进空格数 | `2` |
| `--wrap_spaces <n>` | 折行/端口列表附加悬挂缩进空格数 | `4` |
| `--column_limit <n>` | 目标行最大字符数限制 | `100` |
| `--assignment_statement_alignment` | 赋值语句对齐模式 (`align`, `flush-left`) | `align` |
| `--named_port_alignment` | 实例化端口连接对齐模式 | `align` |
| `--parameter_declaration_alignment` | 参数定义对齐模式 | `align` |
| `--case_items_alignment` | `case` 选项对齐模式 | `align` |
| `--compact_indexing_and_selections` | 切片表达式紧凑模式 | `true` |
| `--flagfile <path>` | 从配置文件读取参数 | 无 |
| `--verify` | 仅验证文件格式（未格式化则退出码为非 0） | `false` |

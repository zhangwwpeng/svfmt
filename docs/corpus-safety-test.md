# 任务书：语料语义安全测试

本文档写给执行测试的 agent（vibe / agy），可以直接当作 prompt 使用。工作目录为
仓库根目录 `/Users/wpzhang/workspace/svfmt`。

## 1. 目标

svfmt 是一个 SystemVerilog 格式化器。本轮**只测一件事：格式化前后代码的语义
是否改变**。格式是否美观、是否和 Verible 一致，本轮**不测**。

判定规则：

| svfmt 行为 | 结果 |
| --- | --- |
| 格式化了文件，且前后只有空白（空格、换行、缩进）不同 | 通过 |
| 拒绝格式化（退出码非 0），且输出与输入**逐字节相同** | 通过，即使输入本身有语法错误 |
| 其他任何情况 | **bug，必须写进报告** |

## 2. 硬性规则（必须遵守）

1. **禁止修改仓库内任何已有文件**，包括 `src/`、`tools/`、`tests/`、`docs/`、
   `CMakeLists.txt`，也禁止修改语料目录 `../sv_test`。
2. **禁止修复 bug**，也不要提交补丁。发现问题就写进报告。
3. **禁止 `git commit`、`git push`、`git checkout`、`git stash`** 等改变仓库状态
   的命令。
4. 你只能写这两处：`reports/` 目录（交付物），以及临时目录 `/tmp/svfmt-corpus/`。
5. 只要有错就写，不要因为"看起来不重要"就省略。不确定的结论标注 **(推测)**。
6. 任何命令失败（构建失败、脚本报错等），把命令和错误原文写进报告，然后继续
   做能做的部分。

## 3. 准备

```sh
cd /Users/wpzhang/workspace/svfmt
cmake --build build -j          # 生成 build/svfmt 和 build/svfmt-lexcmp
mkdir -p reports /tmp/svfmt-corpus
```

`build/` 目录已经配置好，不要删除或重新配置。

## 4. 运行测试

语料在 `../sv_test`，共 9 个类别、约 2.6 万个文件。逐类运行，每类耗时从几十秒
到几分钟不等：

```sh
T=/tmp/svfmt-corpus
for c in 01-sv-compliance 02-sv-designs 03-verilog-designs 04-verification-uvm \
         05-eda-tool-tests 06-uvm-libs 07-assertions 08-weird-syntax 09-lrm-examples; do
  python3 tests/run_corpus.py build ../sv_test/$c --no-parity \
      --out $T/$c/summary --cases $T/$c/cases --jsonl $T/$c/results.jsonl \
      > $T/$c/stdout.txt 2>&1
  echo "$c exit=$?"; tail -12 $T/$c/stdout.txt
done
```

脚本退出码非 0 表示该类别存在安全失败，这是预期情况，不是脚本错误。

### 输出说明

- `stdout.txt`：该类别各安全分类的计数。
- `results.jsonl`：每个文件一行，`{"path", "safety"}`。
- `cases/<分类>/<文件路径用 __ 连接>/`：每个失败文件一个目录，包含：
  - `input.sv`：原文件副本；
  - `output.sv`：svfmt 的输出；
  - `output2.sv`：对 output 再次格式化的结果（仅幂等检查时有）；
  - `lexcmp.txt`：词法比较器报告的差异（第一个不同的 token）；
  - `stderr.txt`：svfmt 的错误输出；
  - `cmd.sh`：一键复现脚本。

### 安全分类含义

| 分类 | 含义 | 是否 bug |
| --- | --- | --- |
| `ok_formatted` | 格式化成功，只有空白变化，且幂等 | 否 |
| `ok_rejected` | 拒绝格式化，原样输出 | 否 |
| `semantic_change` | 格式化后 token 或注释变了，**语义可能被改变** | **是，最严重** |
| `rejected_but_changed` | 报错退出，但输出和输入不同 | **是，严重** |
| `crash` | 崩溃（信号退出或 `Check failed`） | **是** |
| `timeout` | 单文件超过 60 秒 | **是** |
| `not_idempotent` | 对输出再格式化会再变，或者第二次格式化被拒绝 | **是** |

判定由 `build/svfmt-lexcmp input output` 完成。它用 slang 的原始词法器分别切分
两个文件，比较 token 序列和注释内容，忽略空白。它与格式化器不共享代码，可以作为
独立裁判。

## 5. 分析每个失败

对 `cases/` 下每个失败目录做以下分析：

1. **确认可复现**：运行 `sh cmd.sh`。
2. **定位改变**：
   - `semantic_change`：看 `lexcmp.txt`，再用 `diff input.sv output.sv` 找到
     对应位置，说明哪个 token 或注释被删除、增加、改变或挪动。
   - `crash`：看 `stderr.txt` 中的 `Check failed` 或断言位置（文件:行）。
   - `not_idempotent`：`diff output.sv output2.sv`，说明第二次又改了什么。
   - `rejected_but_changed`：`diff input.sv output.sv`。
3. **缩减到最小复现**：在 `/tmp/svfmt-corpus/min/` 下复制 input，反复删除无关
   代码，每次删除后用 `build/svfmt --failsafe_success=false --verify_convergence=false x.sv`
   和 `build/svfmt-lexcmp` 确认问题仍在，直到只剩几行。最小复现**必须**亲自
   跑过、确认能复现。
4. **推测原因**（可选，但尽量做）：
   - `build/svfmt --dump_cst x.sv`：查看 slang 生成的语法树。
   - `build/svfmt --show_token_partition_tree x.sv`：查看分区树。
   - 判断问题最可能出在哪一层：
     - 桥接/建树：`src/svfmt/slang-analyzer.cc`
     - 预处理、宏、`` `ifdef ``：同一文件中的 `ConvertDirective`、`MacroUsage` 部分
     - 空格与换行决策：`src/verible/verilog/formatting/token-annotator.cc`
     - 分区和缩进：`src/verible/verilog/formatting/tree-unwrapper.cc`
     - 对齐：`src/verible/verilog/formatting/align.cc`
     - 输出校验：`src/verible/verilog/formatting/formatter.cc`
   - 可以阅读源码，但**不要修改**。
5. **按根因合并**：最小复现相同、或明显是同一原因的文件合成一个条目，并记录受影响
   文件数。

失败文件很多时，先按 `lexcmp.txt` 首行或 `stderr.txt` 的断言位置粗分组，每组至少
深入分析一个代表文件，其余列入清单。

## 6. 交付物

写入 `reports/corpus-safety-<YYYYMMDD>.md`，格式如下：

```markdown
# 语料语义安全测试报告 <日期>

svfmt commit: <git rev-parse --short HEAD>
语料: ../sv_test (<文件数>)

## 总表

| 类别 | 文件数 | ok_formatted | ok_rejected | semantic_change | rejected_but_changed | crash | timeout | not_idempotent |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 01-sv-compliance | ... |
| ... |
| 合计 | ... |

## 问题清单（按严重程度排序：semantic_change > rejected_but_changed > crash > timeout > not_idempotent）

### P1. <一句话描述>
- 分类: semantic_change
- 受影响文件数: N
- 代表文件: ../sv_test/...
- 最小复现:
  ```systemverilog
  <输入>
  ```
  svfmt 输出:
  ```systemverilog
  <输出>
  ```
  改变的内容: <例如 "注释 // foo 被删除" / "token `,` 消失">
- 复现命令: `build/svfmt --failsafe_success=false --verify_convergence=false min.sv`
- 原因分析: <...>（推测）
- 可能涉及: src/...（推测）

### P2. ...

## 运行中遇到的其他问题
<脚本错误、构建问题等；没有就写"无">

## 附录：全部失败文件
<按分类列出全部路径，可以从 summary/*.txt 汇总>
```

另外把最小复现文件放到 `reports/min/P<编号>.sv`。

完成后用 `git status --short` 确认只有 `reports/` 下有新增文件，并把输出贴在报告
末尾。

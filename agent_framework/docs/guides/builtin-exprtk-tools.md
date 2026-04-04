# 内建 math_* / expr_* 表达式工具（ExprTk 建设指南）

本文档说明如何在 **Agent Framework** 中基于 **[ExprTk](https://github.com/ArashPartow/exprtk)**（单头文件、MIT 许可）**设计与实现**面向 LLM 调用的**本地数学/数据计算工具**，并与 **`ToolBus::register_local_tool`**、**`build_cli_agent_graph`** 等现有机制对齐。文档定位与 [builtin-fs-tools.md](./builtin-fs-tools.md)（本地文件监禁）、[builtin-web-tools.md](./builtin-web-tools.md)（受控出站 HTTP）并列：**`fs_*` 管磁盘边界，`web_*` 管网络边界，表达式工具管「可执行数学语言的计算边界」**。

**现状说明**：当 **`exprtk.hpp`** 存在且 **`AGENT_EXPR_ENABLE`** 未设为 **`0` / `false` / `off` / `no`** 时，`build_cli_agent_graph` 会幂等注册 **`expr_eval`**、**`expr_validate`**、**`expr_batch_eval`**（实现见 **`src/toolbus/expr_tools.cpp`**，声明 **`include/agent/expr_tools.hpp`**）。门闩与解析/循环上限等环境变量见下文 §10；**`AGENT_TOOL_ALLOWLIST`** 若启用须**同时**列入上述三名（与 `web_*` 相同，缺一会在注册阶段抛错）。离线回归：**`ctest -R expr_tools`**（**`test_expr_tools`**）。

**安全说明**：表达式求值 **不是** 操作系统沙箱。若允许模型提交**任意字符串**作为表达式，必须通过 **ExprTk 解析器限制**（深度、节点数、循环迭代、表达式长度、可选禁用控制流/赋值）约束 **CPU 与内存**；对不可信输入还应考虑 **禁用文件 I/O RTL**（若编译定义未关闭）及 **不在符号表注册危险自定义函数**。勿在无人监督场景下把表达式语言当作「通用脚本」。

---

## 1. ExprTk 适用场景（为何选它）

| 维度 | 说明 |
|------|------|
| **形态** | **Header-only**，主接口 **`exprtk.hpp`**， specialization 多为 **`double`**（亦支持 `float` / `long double` 等）。 |
| **典型用途** | 运行时编译算术/逻辑表达式，**重复求值**同一 AST（变量更新后 `expression.value()`），适合代理在对话中迭代调参。 |
| **能力** | 标量/向量/字符串类型、大量初等函数与比较逻辑、**控制流**（`if` / `switch` / `for` / `while`）、**`$fNN` 特殊函数**节点以减 AST 深度、可选数值积分/导数辅助函数（见上游文档）。 |
| **与本项目契合** | C++17/20 代码库；与 **`nlohmann::json`** 工具参数/返回值拼接自然；无额外链接依赖。 |

上游权威说明见：[ExprTk 官方页](https://www.partow.net/programming/exprtk/index.html) 与镜像仓库 [ArashPartow/exprtk](https://github.com/ArashPartow/exprtk)（**`readme.txt`** 为完整手册）。

---

## 2. 语法与能力摘要（写给 Agent 系统提示与 Schema 注释）

以下内容用于**压缩写入工具 `description`** 或 **系统提示**，使模型知道「能写什么、不能写什么」。详细语法以上游 **readme Section 08–15、30** 为准。

### 2.1 类型

- **标量**：实数（实现中多为 `double`）。
- **向量**：固定长度数值数组；支持按元素运算、`sum` / `min` / `max` / `dot` 等（见上游 **Section 14**）。
- **字符串**：单引号字面量、部分字符串运算与 `in` / `like` / `ilike`；**不能与标量混算**。

### 2.2 运算符（节选）

`+ - * / % ^`；赋值 `:=, +=, -=, *=, /=, %=`；比较 `= == != < > <= >=`；逻辑 `and or not xor ...`；`&` `|` 为带短路优化的与/或。

### 2.3 内置函数（数学与通用，节选）

含 **`abs` `min` `max` `clamp` `sgn` `floor` `ceil` `round` `trunc` `frac`**；**`exp` `log` `log10` `log2` `logn` `sqrt` `root` `pow`（或 `^`）**；**`sin` `cos` `tan` `asin` `acos` `atan` `atan2` `sinh` `cosh` `tanh` …**（弧度，除非注册替换函数）；**`erf` `erfc` `ncdf`** 等统计相关；**`equal` `not_equal`** 为带 epsilon 的比较。完整表格见上游 **Section 08**。

对「**特殊函数**」一词的两种含义：

1. **数学上的特殊函数**：ExprTk 已提供 **`erf` / `erfc` / `ncdf`** 等；更复杂的特殊函数需通过 **`ifunction` 自定义**或分段有理近似写入表达式。
2. **ExprTk 的 `$fNN` 编译期特殊节点**：将常见代数模式折叠为高效节点，如 **`$f12(x,y,z)`** 对应 **`(x / y) - z`**，见上游 **Section 12 (4)** 中完整 **`$f00`–`$f99`** 表。Agent **通常不手写** `$fNN`，而是由实现或离线生成的表达式使用；若开放给模型，须在文档中说明语法。

### 2.4 控制流与多语句

- 语句间用 **`;`** 分隔；**最后一条语句**决定返回值。
- **`if (cond, a, b)`** 函数式三目；**`if / else`** 块形式；**`switch`**；**`for` / `while` / `repeat-until`**；**`return [...]`**（需处理 `return_invoked()` 与 **`results_context`**，见上游 **Section 20**）。
- **注意**：多语句时若漏写分号，**隐含乘法**（commutative check）可能把两条语句拼成一条，产生非预期结果；建设工具时可在实现层对原始字符串做**括号称校验**或**拒绝歧义换行**。

### 2.5 外部变量与符号表

- 表达式中 **`x` `y`** 等需在 **`exprtk::symbol_table`** 注册 **`add_variable`** / **`add_vector`** / **`add_constant`**。
- **未知符号**：默认编译失败；开启 **unknown symbol resolver** 会把未识别标识符当成新变量，**可能把函数名拼写错误吞掉**（上游 **Section 18 Note25**）。**面向不可信模型时建议关闭默认 USR**，仅允许白名单变量名。

---

## 3. 依赖与构建（本仓库）

| 项 | 说明 |
|----|------|
| 子模块路径 | **`3rd-party/exprtk`**（含 **`exprtk.hpp`**）。 |
| 初始化 | `git submodule update --init 3rd-party/exprtk` |
| CMake | **`EXPRTK_DIR`** 逻辑位于 **`agent_framework/CMakeLists.txt`**；存在 **`exprtk.hpp`** 时 **`include_directories(${EXPRTK_DIR})`**。 |
| 包含方式 | `#include "exprtk.hpp"`（注意包含目录为 **`exprtk` 目录本身**）。 |
| 冒烟测试 | **`BUILD_TESTING=ON`** 时构建 **`test_exprtk_smoke`**（**`agent_framework/tests/test_exprtk_smoke.cpp`**）。 |

---

## 4. 与 ToolBus 的集成模式

与 **`fs_tools.cpp` / `web_tools.cpp`** 一致：

1. 实现 **`std::function<nlohmann::json(const nlohmann::json&)>`**，入参为工具 **arguments**（对象）。
2. 构造 **`ToolMeta`**：`name`、`description`（可嵌入**第 2 节**压缩语法说明）、**`schema`**（JSON Schema，`required` 明确）。
3. 调用 **`ToolBus::register_local_tool(name, func, meta)`**。
4. 在 **`build_cli_agent_graph`**（或自建图）中于构图前调用 **`register_builtin_*_if_configured(bus)`** 风格注册函数；**避免重复注册**可用 `bus.get_tool_info("tool_name")` 判断。
5. **`AGENT_TOOL_ALLOWLIST`**：若设置，须**同时**列入 **`expr_eval,expr_validate,expr_batch_eval`**（与 [builtin-fs-tools.md](./builtin-fs-tools.md)、[builtin-web-tools.md](./builtin-web-tools.md) 相同约定；仅列其中一部分会导致 `register_local_tool` 抛错）。

---

## 5. 建议工具形态（可一种或多种并存）

以下为**产品化拆分**建议；实现可合并为单工具 **`expr_eval`** + `mode` 字段。

| 工具名（示例） | 职责 | 说明 |
|----------------|------|------|
| **`expr_eval`** | 单次编译并求值，或「已知已编译句柄」求值 | 主路径；参数含 **`expression`** 字符串与 **`variables`** 对象（名→数）或 **`vectors`**（名→数组）。 |
| **`expr_validate`** | 仅编译不运行，或编译后返回依赖符号列表 | 利用 **`dependent_entity_collector`**（上游 **Section 16**）返回 **`variables` / `functions`** 列表，便于 Agent 自检。 |
| **`expr_batch_eval`** | 同一程序多组变量 | **同一条表达式**编译一次，对 **`rows`** 中每组变量多次 **`value()`**，减少解析开销。 |

**不推荐**让模型在单工具调用里上传「**预编译 AST 序列化**」：ExprTk 表达式应以**字符串**为 canonical 形式（上游 **Section 26 Note32**）。

---

## 6. JSON 参数与 Schema 建议

### 6.1 `expr_eval`（推荐字段）

| 字段 | 类型 | 说明 |
|------|------|------|
| **`expression`** | string | 必填；ExprTk 程序字符串（可为多语句；注意长度上限）。 |
| **`variables`** | object | 可选；键为变量名，值为 **number**（ IEEE 双精度）；缺失的注册变量可视为 0 或报错（需固定策略并写进 `description`）。 |
| **`vectors`** | object | 可选；值为 **number 数组**，注册为 `std::vector<double>` 或 **`exprtk::vector_view`**（需在多次求值间 **rebase** 时注意线程安全）。 |
| **`constants`** | object | 可选；只读常量 **`add_constant`**，避免模型误改。 |
| **`return_format`** | string | 可选；如 **`scalar`**（默认）、**`full`**（若使用 `return` 路径则附带 **`results`** 数组结构说明）。 |

**Schema 示例**（可按项目风格收紧 `maxLength`）：

```json
{
  "type": "object",
  "properties": {
    "expression": { "type": "string", "maxLength": 16384 },
    "variables": { "type": "object", "additionalProperties": { "type": "number" } },
    "vectors": {
      "type": "object",
      "additionalProperties": { "type": "array", "items": { "type": "number" } }
    },
    "constants": { "type": "object", "additionalProperties": { "type": "number" } }
  },
  "required": ["expression"]
}
```

### 6.2 建议返回（成功）

```json
{
  "value": 1.23456789012345,
  "expression": "...",
  "warnings": []
}
```

若表达式使用 **`return`**：

- **`expression.value()`** 可能为 **NaN**；须检查 **`expression.return_invoked()`** 并自 **`expression.results()`** 抽取多返回值（标量/向量/字符串），序列化为 JSON 数组 **`results`**。

失败时与 **`fs_*` / `web_*`** 一致，返回含 **`error.code`**、**`error.message`** 的对象（勿向模型暴露内部堆栈）。

---

## 7. 安全与资源限制（实现清单）

以下为 **ExprTk 手册**与工程经验结合的检查项；**应在编译前设置**（部分在 **`parser.settings()`**，部分为**独立校验**）。

### 7.1 字符串与解析预算

| 措施 | 建议 |
|------|------|
| **`expression` 最大字节数** | 入参硬性拒绝（如 **16–64 KiB** 按场景）。 |
| **`parser.settings().set_max_stack_depth(n)`** | 限制递归下降解析栈，如 **100–400**（默认 400，可按硬防护收紧）。 |
| **`set_max_node_depth`** | 限制 AST 深度，如 **≤ 512–2000**。 |
| **`set_max_total_local_symbol_size_bytes`** | 限制 **`var`** 局部变量总量，防止巨型局部向量定义。 |
| **`set_max_local_vector_size`** | 限制局部向量元素个数上限。 |

### 7.2 循环与运行时间

| 措施 | 建议 |
|------|------|
| **`loop_runtime_check`** | 注册 **`exprtk::loop_runtime_check`** 子类：限制 **`max_loop_iterations`**（如 **1e5**）或结合计时（上游 **Section 24 (3)**）。 |
| **禁用控制流** | 若仅需算术： **`parser.settings().disable_all_control_structures()`**（上游 **Section 19 (2)**）。 |

### 7.3 赋值与副作用

- 若仅需**纯函数式求值**：对托管变量的 **`symbol_table`** 使用 **`e_immutable`**（上游 **Section 10 Note04–05**），禁止表达式内 **`:=`** 修改外部状态。
- **多语句**中 **`return`**、字符串写侧效应等，按业务决定允许范围。

### 7.4 非法数值

- **`value()`** 结果可为 **NaN / Inf**；建议在返回 JSON 前用 **`std::isfinite`** 判断，并映射为 **`error.code`: `non_finite_result`** 或显式字符串 **`"nan"`** 策略（与数据科学 Agent 约定一致）。

### 7.5 线程与性能

- **`exprtk::parser`** **非线程安全**；多线程并发求值时 **每个线程独立 parser**，或 **单线程编译 + 多份 `expression`（只读求值）** 需上游文档与实例验证；**共享 `expression` 与并发 `value()`** 需谨慎。
- **推荐**：**编译一次，多次求值**；勿每条消息重新编译同一巨型程序。

### 7.6 RTL 与宏

ExprTk 可选 **RTL 包**（打印、文件、向量扩展等，上游 **Section 22**）。若不需要 **文件 I/O**，构建时可用 **`exprtk_disable_rtl_io_file`** 等宏（上游 **Section 28**）避免符号表里误注册 **`open`/`write`** 类能力。

---

## 8. 自定义函数与「特殊函数」扩展

| 方式 | 适用 |
|------|------|
| **`exprtk::ifunction<T>`** | 固定个数标量参数（如 **Gamma 近似、贝塞尔、领域公式**）。 |
| **`exprtk::ivararg_function<T>`** | 变长标量参数。 |
| **`exprtk::igeneric_function<T>`** | 标量/向量/字符串混合参数（带类型序列串 ** `"TTV"`** 等）。 |
| **`function_compositor`** | 用 ExprTk 语法组合可复用函数并挂到 **`symbol_table`**（上游 **Section 15 (6)**）。 |

注册名称勿与 **保留字** 冲突；若需替换 **`sin`** 等为「度」模式，用 **`add_reserved_function`**（上游 **Section 19 Note30**）。

---

## 9. 实现骨架（C++，与仓库风格一致）

以下的逻辑顺序可直接作为 **`expr_tools.cpp`** 中 **`invoke`** 的骨架（省略命名空间与错误封装）。

```cpp
#include "exprtk.hpp"
#include <nlohmann/json.hpp>

// 1) 校验 expression 长度、UTF-8 若需要
// 2) symbol_table.add_constants(); // 可选 pi, e 等
// 3) 按 JSON 填充 variables / vectors / constants
// 4) parser.settings()...; register_loop_runtime_check(...);
// 5) expression.register_symbol_table(symbol_table);
// 6) if (!parser.compile(expression_str, expression)) return parse_error(parser);
// 7) double v = expression.value();
// 8) if (expression.return_invoked()) { ... results ... }
```

**变量生存期**：**`symbol_table` 持有外部变量引用**；编译后的 **`expression`** 存活期间，被引用的 **`double&`** / **`std::vector<double>`** 必须有效（上游 **Section 10 Note02**）。

---

## 10. 建议环境变量（与 fs_*/web_* 并列）

实现新工具时，建议支持以下环境变量（名称可按项目前缀 **`AGENT_EXPR_`** 统一）：

| 变量 | 含义 | 示例默认 |
|------|------|----------|
| **`AGENT_EXPR_MAX_EXPR_BYTES`** | 表达式字符串最大长度 | `16384` |
| **`AGENT_EXPR_MAX_LOOP_ITERS`** | 循环 RTC 最大迭代 | `100000` |
| **`AGENT_EXPR_PARSER_STACK_DEPTH`** | `set_max_stack_depth` | `200` |
| **`AGENT_EXPR_PARSER_NODE_DEPTH`** | `set_max_node_depth` | `2000` |
| **`AGENT_EXPR_DISABLE_CONTROL_FLOW`** | 若为 `1`，`disable_all_control_structures` | `0` |
| **`AGENT_EXPR_ENABLE`** | 若为 `0`，不注册工具（类比 **`AGENT_WEB_ENABLE`**） | `1` |

---

## 11. 常见错误码（建议）

| `error.code` | 说明 |
|--------------|------|
| **`expr_too_large`** | 超过 **`AGENT_EXPR_MAX_EXPR_BYTES`** |
| **`parse_error`** | **`parser.compile` 失败**；**`message`** 含 **`parser.error()`**（可截断） |
| **`undefined_symbol`** | 未注册变量/函数且未启用 USR |
| **`loop_limit`** | 触发循环 RTC |
| **`non_finite_result`** | 结果为 NaN/Inf（若策略为拒绝） |
| **`timeout`** | 若将来增加求值 wall-clock 上限 |

---

## 12. 测试建议

| 项 | 说明 |
|----|------|
| **冒烟** | 现有 **`test_exprtk_smoke`**：验证子模块与包含路径。 |
| **单测** | 增加 **`test_expr_tools.cpp`**：**parse 失败**、**简单算术**、**clamp/sin**、**循环上限**、**immutable 符号表拒绝 `:=`** 等。 |
| **回归** | **`test_expr_tools`** / **`ctest -R expr_tools`**：幂等注册、**`AGENT_EXPR_ENABLE=0`** 门闑、**`expr_tools_builtin_allowlist_partial`**（仅 `expr_eval` 在 allowlist 时注册应抛错）。 |

---

## 13. Agent 使用提示（可放入系统提示模板）

- 优先把问题写成 **一条清晰的 ExprTk 程序**，显式 **`variables`** 数值；复用中间结果用 **`var`** 局部变量或 **`:=`**（在允许可变策略下）。
- **角度制**三角函数须用 **`deg2rad(x)`** 包装自变量，或依赖工具注册的 **`sin_deg`** 自定义函数。
- 需要 **分段定义** 时用 **`if` / `switch`**，避免在宿主语言里拆多次调用（在控制流未禁用前提下）。
- **大型表格逐行计算**：用 **`expr_batch_eval`** 模式减少重复编译。

---

## 14. 许可与合规

- **ExprTk** 采用 **MIT License**（上游 **`license.txt`**）。发行物中保留许可证副本；若 **静态链接或分发头文件**，遵循项目第三方清单惯例。
- 本文档不替代 ExprTk 官方手册；实现者以 **`readme.txt` / 官方站点** 为准。

---

## 15. 参考链接

- ExprTk 主页：<https://www.partow.net/programming/exprtk/index.html>  
- 源码镜像：<https://github.com/ArashPartow/exprtk>  
- 本仓库：**`3rd-party/exprtk/readme.txt`**（完整章节手册）  
- 并列文档：[builtin-fs-tools.md](./builtin-fs-tools.md) · [builtin-web-tools.md](./builtin-web-tools.md)

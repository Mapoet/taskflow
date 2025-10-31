# Key-Based API 与声明式构图设计

## ✅ 当前实现状态

### 已完成功能

1. **✅ Key-based 输出映射**
   - `TypedOutputs` 包含 `futures_map`，支持通过 key 访问输出
   - 所有输出都有明确的 key（构造函数指定或自动生成）
   - 同时保持类型安全和类型擦除两种访问方式
   - `TypedOutputs::get_typed<I>(key)` - 类型安全的 future 访问
   - `TypedOutputs::get_typed_by_key<T>(key)` - 运行时类型查找

2. **✅ 统一输出访问接口**
   - `INode::get_output_future(key)` - 获取 type-erased future（所有节点类型）
   - `INode::get_output_keys()` - 获取所有输出 key
   - 支持多态访问：`std::shared_ptr<INode>` 可以直接调用

3. **✅ 声明式构图 API**
   - `create_typed_source(name, values, output_keys)` - 创建源节点
   - `create_typed_node<Ins...>(name, input_specs, functor, output_keys)` - 创建节点
     - 输入类型：显式指定 `<Ins...>`
     - 输出类型：从 functor 返回类型自动推断
   - `create_any_source/node/sink()` - Any 节点创建
   - **自动依赖推断**：根据 `input_specs` 自动建立依赖关系

4. **✅ 输入辅助函数**
   - `get_input<T>(node_name, key)` - 获取类型化的输入（声明已添加）
   - `get_output(node_name, key)` - 获取 type-erased 输出

5. **✅ 适配器任务管理**
   - 自动创建适配器任务连接 Typed → Any
   - 适配器任务注册到 `adapter_tasks_` 映射
   - 依赖关系：优先使用 `adapter → target`，无适配器时使用 `source → target`

## 🎯 技术特点

### 1. Key-based I/O 系统

**数据结构**：

**TypedOutputs** (编译时类型安全)：
```cpp
template <typename... Outs>
struct TypedOutputs {
  // 类型化的 futures（索引访问）
  std::tuple<std::shared_ptr<std::promise<Outs>>...> promises;
  std::tuple<std::shared_future<Outs>...> futures;
  
  // Key-based 访问（类型擦除）
  std::unordered_map<std::string, std::shared_future<std::any>> futures_map;
  std::vector<std::string> output_keys;  // 有序 key 列表
  std::unordered_map<std::string, std::size_t> key_to_index_;  // key → 索引映射
  
  // Any promises（用于同步）
  std::unordered_map<std::size_t, std::shared_ptr<std::promise<std::any>>> any_promises_;
};
```

**AnyOutputs** (运行时类型擦除)：
```cpp
struct AnyOutputs {
  std::unordered_map<std::string, std::shared_ptr<std::promise<std::any>>> promises;
  std::unordered_map<std::string, std::shared_future<std::any>> futures;
};
```

**访问方式对比**：

```cpp
// 方式 1: 索引访问（类型安全，但需要知道顺序）
auto x_fut = std::get<0>(A->out.futures);  // double

// 方式 2: Key 访问 - 类型安全（需要知道索引）
auto x_fut = A->out.get_typed<0>("x");  // double

// 方式 3: Key 访问 - 类型安全（运行时查找）
auto x_fut = A->out.get_typed_by_key<double>("x");  // double

// 方式 4: Key 访问 - 类型擦除（统一接口）
auto x_any_fut = A->get_output_future("x");  // shared_future<any>
```

### 2. 声明式构图 API

**输入规范格式**：
```cpp
std::vector<std::pair<std::string, std::string>> input_specs;
// 格式：{{"source_node_name", "source_output_key"}, ...}

// 示例：
{{"A", "x"}}                    // 单输入
{{"B", "b"}, {"C", "c"}}        // 多输入
{{"D", "prod"}, {"G", "sum"}}    // 多输入（可来自不同源）
```

**自动依赖推断机制**：

```cpp
// 在 create_typed_node 中：
for (const auto& [source_node, source_key] : input_specs) {
  const std::string adapter_key = source_node + "::" + source_key;
  auto adapter_it = adapter_tasks_.find(adapter_key);
  
  if (adapter_it != adapter_tasks_.end()) {
    // 使用适配器任务作为依赖
    adapter_it->second.precede(task);
  } else {
    // 直接使用源节点作为依赖
    source_task_it->second.precede(task);
  }
}
```

**适配器任务创建**：

```cpp
// 在 get_typed_input_impl 中：
// 当需要从 Typed 节点获取类型化 future 时
auto any_fut = node->get_output_future(key);  // 获取 any future

// 创建适配器 promise/future
auto p_typed = std::make_shared<std::promise<T>>();
auto f_typed = p_typed->get_future().share();

// 创建适配器任务：any → typed
auto adapter_task = taskflow_.emplace([any_fut, p_typed]() {
  std::any value = any_fut.get();
  T typed_value = std::any_cast<T>(value);
  p_typed->set_value(std::move(typed_value));
}).name(node_name + "_to_" + key + "_adapter");

// 注册适配器
adapter_tasks_[node_name + "::" + key] = adapter_task;

// 建立依赖：source → adapter
source_task.precede(adapter_task);
```

### 3. 类型推断系统

**输入类型**：必须显式指定
```cpp
auto [B, _] = builder.create_typed_node<double>(  // <double> 是输入类型
  "B", {{"A", "x"}}, /*functor*/, {"b"}
);
```

**输出类型**：从 functor 返回类型自动推断
```cpp
// Functor 返回类型推断
using ReturnType = typename std::invoke_result<OpType, TestInput>::type;
// ReturnType = tuple<Outs...>

// 提取输出类型
using OutsTuple = ReturnType;
// 创建节点类型
using NodeType = TypedNode<InputsTuple, std::tuple_element_t<OutIndices, OutsTuple>...>;
```

## 📝 使用样例

### 样例 1: 基本声明式构图

**代码**：

```cpp
#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>

int main() {
  namespace wf = workflow;
  tf::Executor executor;
  wf::GraphBuilder builder("example");

  // 创建源节点
  auto [A, tA] = builder.create_typed_source("A",
    std::make_tuple(3.5, 7),
    {"x", "k"}
  );

  // 创建节点 B: A::x -> b (x+1)
  auto [B, tB] = builder.create_typed_node<double>("B",
    {{"A", "x"}},
    [](const std::tuple<double>& in) {
      return std::make_tuple(std::get<0>(in) + 1.0);
    },
    {"b"}
  );

  // 创建节点 C: A::x -> c (2*x)
  auto [C, tC] = builder.create_typed_node<double>("C",
    {{"A", "x"}},
    [](const std::tuple<double>& in) {
      return std::make_tuple(2.0 * std::get<0>(in));
    },
    {"c"}
  );

  // 创建节点 D: B::b, C::c -> prod (b*c)
  auto [D, tD] = builder.create_typed_node<double, double>("D",
    {{"B", "b"}, {"C", "c"}},
    [](const std::tuple<double, double>& in) {
      return std::make_tuple(std::get<0>(in) * std::get<1>(in));
    },
    {"prod"}
  );

  // 创建接收节点
  auto [H, tH] = builder.create_any_sink("H",
    {{"D", "prod"}}
  );

  // 执行（依赖自动建立）
  builder.run(executor);
  return 0;
}
```

**数据流图**：

```dot
digraph example {
  A -> A_to_x_adapter
  A_to_x_adapter -> B
  A_to_x_adapter -> C
  B -> B_to_b_adapter
  C -> C_to_c_adapter
  B_to_b_adapter -> D
  C_to_c_adapter -> D
  D -> H
}
```

**依赖关系**（自动推断）：
- `B` 依赖于 `A` (via `{"A", "x"}`)
- `C` 依赖于 `A` (via `{"A", "x"}`)
- `D` 依赖于 `B, C` (via `{{"B", "b"}, {"C", "c"}}`)
- `H` 依赖于 `D` (via `{"D", "prod"}`)

### 样例 2: 多输出节点

```cpp
// 节点 G: 三个输入，两个输出
auto [G, tG] = builder.create_typed_node<double, double, int>("G",
  {{"C", "c"}, {"B", "b"}, {"E", "ek"}},
  [](const std::tuple<double, double, int>& in) {
    double sum = std::get<0>(in) + std::get<1>(in);
    int parity = (std::get<2>(in) % 2 + 2) % 2;
    return std::make_tuple(sum, parity);  // 返回 tuple<double, int>
  },
  {"sum", "parity"}  // 两个输出 key
);

// 使用多个输出
auto [H, tH] = builder.create_any_sink("H",
  {{"G", "sum"}, {"G", "parity"}}  // 从同一节点获取多个输出
);
```

**数据细节**：
- `G` 的输出：`{"sum": shared_future<double>, "parity": shared_future<int>}`
- `H` 的输入：`{"sum": shared_future<any>, "parity": shared_future<any>}`

### 样例 3: Any-based 节点

```cpp
// Any 源节点
auto [A, tA] = builder.create_any_source("A",
  std::unordered_map<std::string, std::any>{
    {"x", std::any{3.5}},
    {"k", std::any{7}}
  }
);

// Any 节点
auto [B, tB] = builder.create_any_node("B",
  {{"A", "x"}},
  [](const std::unordered_map<std::string, std::any>& in) {
    double x = std::any_cast<double>(in.at("x"));
    return std::unordered_map<std::string, std::any>{
      {"b", std::any{x + 1.0}}
    };
  },
  {"b"}
);

// Any 接收节点
auto [H, tH] = builder.create_any_sink("H",
  {{"B", "b"}}
);
```

**数据细节**：
- 输入/输出都使用 `unordered_map<string, any>`
- 运行时类型转换：`std::any_cast<T>`
- 类型安全由程序员保证（运行时检查）

### 样例 4: 混合 Typed/Any 工作流

```cpp
// Typed 计算节点（性能优先）
auto [D, tD] = builder.create_typed_node<double, double>("D",
  {{"B", "b"}, {"C", "c"}},
  [](const std::tuple<double, double>& in) {
    return std::make_tuple(std::get<0>(in) * std::get<1>(in));
  },
  {"prod"}
);

// Any 接收节点（灵活性优先）
auto [H, tH] = builder.create_any_sink("H",
  {{"D", "prod"}, {"G", "sum"}, {"G", "parity"}}
);
```

**适配器机制**：
- 当 `H` 从 `D` 获取 `"prod"` 时：
  1. `D->get_output_future("prod")` 返回 `shared_future<any>`
  2. 适配器任务 `D_to_prod_adapter` 自动创建
  3. 适配器提取 `double` 值并包装为 `any`
  4. 依赖关系：`D → D_to_prod_adapter → H`

## 🔍 技术细节

### 适配器任务生命周期

1. **创建时机**：在 `get_typed_input_impl<T>()` 中，当需要从 Any 输出获取 Typed 输入时
2. **注册**：保存到 `adapter_tasks_["source::key"]`
3. **依赖建立**：
   - `source_task → adapter_task` (在创建时)
   - `adapter_task → target_task` (在 `create_typed_node` 中)

### 类型转换流程

**Typed → Any** (适配器任务)：
```
TypedSource emits: double (3.5)
  ↓
TypedOutputs sets: promise<double> + promise<any>
  ↓
Adapter task: any_fut.get() → any_cast<double> → promise<T>
  ↓
TypedNode receives: shared_future<T>
```

**Any → Typed** (直接访问)：
```
AnyNode emits: map<string, any>
  ↓
AnyOutputs sets: promise<any>
  ↓
TypedNode: any_fut.get() → any_cast<T> (via adapter)
```

### 依赖推断算法

```cpp
// 伪代码
for each input_spec in input_specs:
  source_node = input_spec.first
  source_key = input_spec.second
  
  adapter_key = source_node + "::" + source_key
  
  if adapter_tasks_.contains(adapter_key):
    // 适配器已存在，使用适配器
    adapter_tasks_[adapter_key].precede(target_task)
  else:
    // 无适配器，直接连接源节点
    tasks_[source_node].precede(target_task)
```

### 内存模型

**Typed 节点**：
- 每个输出：1个 `shared_ptr<promise<T>>` + 1个 `shared_future<T>` (类型化)
- 每个输出：1个 `shared_ptr<promise<any>>` + 1个 `shared_future<any>` (类型擦除)
- 总内存：`2 * sizeof(shared_ptr) * num_outputs` (约 32 bytes/输出)

**Any 节点**：
- 每个输出：1个 `shared_ptr<promise<any>>` + 1个 `shared_future<any>`
- 总内存：`2 * sizeof(shared_ptr) * num_outputs` (约 32 bytes/输出)

**适配器任务**：
- 1个 `shared_ptr<promise<T>>` + 1个 `shared_future<T>`
- 1个 Taskflow task 对象
- 总内存：约 100-200 bytes/适配器

## 📊 性能特性

### 类型化访问（零开销）

```cpp
// 编译时类型已知，零运行时开销
auto x_fut = A->out.get_typed<0>("x");  // 直接返回 tuple 元素
```

### 类型擦除访问（最小开销）

```cpp
// 运行时查找 key，类型擦除（~1-2ns）
auto x_any_fut = A->get_output_future("x");  // map 查找 + any 包装
```

### 适配器任务开销

- **创建开销**：1个额外任务对象（~100 bytes）
- **执行开销**：1个额外任务调度（~100ns）
- **内存开销**：适配器 promise/future 对（~32 bytes）

## 🎯 最佳实践

### 1. 优先使用声明式 API

**✅ 推荐**：
```cpp
auto [B, _] = builder.create_typed_node<double>("B",
  {{"A", "x"}}, /*functor*/, {"b"});
// 依赖自动推断
```

**❌ 不推荐**：
```cpp
auto B = std::make_shared<wf::TypedNode</*...*/>>(/*...*/);
auto tB = builder.add_typed_node(B);
builder.precede(tA, std::vector<tf::Task>{tB});  // 手动依赖
```

### 2. 使用有意义的输出 keys

**✅ 推荐**：
```cpp
{"x", "y", "sum", "product"}  // 语义清晰
```

**❌ 不推荐**：
```cpp
{"out0", "out1", "out2"}  // 自动生成，但缺少语义
```

### 3. 输入类型显式指定

**✅ 正确**：
```cpp
builder.create_typed_node<double, double>("D",  // 明确两个 double 输入
  {{"B", "b"}, {"C", "c"}}, /*functor*/, {"prod"});
```

**❌ 错误**：
```cpp
builder.create_typed_node("D",  // 缺少输入类型
  {{"B", "b"}, {"C", "c"}}, /*functor*/, {"prod"});
```

### 4. Functor 签名匹配

**✅ 正确**：
```cpp
builder.create_typed_node<double>("B",
  {{"A", "x"}},
  [](const std::tuple<double>& in) {  // 接收 tuple<double>
    return std::make_tuple(std::get<0>(in) + 1.0);  // 返回 tuple<...>
  },
  {"b"}
);
```

**❌ 错误**：
```cpp
builder.create_typed_node<double>("B",
  {{"A", "x"}},
  [](double x) { return x + 1.0; },  // 签名不匹配
  {"b"}
);
```

## 🔮 未来改进方向

### Phase 1: 输入类型自动推断 ⏳

```cpp
// 未来：从输入规范自动推断输入类型
auto [B, _] = builder.create_typed_node("B",  // 无需指定 <double>
  {{"A", "x"}},  // 从 A 的输出类型推断输入类型
  /*functor*/, {"b"});
```

### Phase 2: 完全类型无关的 Functor ⏳

```cpp
// 未来：完全基于 key 访问，无类型信息
auto [B, _] = builder.create_typed_node("B",
  {{"A", "x"}},
  [](const auto& inputs) {  // 接收 key-value map
    double x = inputs.at("x").get<double>();
    return std::make_tuple(x + 1.0);
  },
  {"b"}
);
```

### Phase 3: 声明式链式 API ⏳

```cpp
// 未来：完全声明式链式调用
builder
  .source("A", std::make_tuple(3.5, 7), {"x", "k"})
  .node("B", inputs("A::x"), [](auto x){return x+1;}, outputs("b"))
  .node("D", inputs("B::b", "C::c"), [](auto b, auto c){return b*c;}, outputs("prod"))
  .sink("H", inputs("D::prod"))
  .run(executor);
```

## 📈 对比总结

| 特性 | 传统 API | Key-based API | 声明式 API |
|------|---------|---------------|------------|
| **输入指定** | 手动获取 futures | Key 访问 | Input specs |
| **依赖管理** | 手动 `precede/succeed` | 手动 `precede/succeed` | 自动推断 ✅ |
| **代码量** | 多 | 中等 | 少 ✅ |
| **可读性** | 中 | 高 ✅ | 最高 ✅ |
| **类型安全** | 是 | 是 | 是 |
| **灵活性** | 中 | 高 | 最高 ✅ |

**推荐使用顺序**：
1. 🥇 **声明式 API** - 新代码首选
2. 🥈 **Key-based API** - 需要精细控制时
3. 🥉 **传统 API** - 向后兼容，已标记 deprecated

## 📚 相关文档

- `workflow/README.md` - 完整库文档
- `readme/guide_workflow.md` - 技术路线与实现细节
- `examples/declarative_example.cpp` - 声明式 API 完整示例
- `examples/unified_example.cpp` - Key-based API 示例

```dot
digraph Taskflow {
subgraph cluster_p0x7ffdc7cb6fb0 {
label="Taskflow: advanced_control_flow";
p0x626c0a7c3500[label="A" ];
p0x626c0a7c3500 -> p0x626c0a7c6930;
p0x626c0a7c3640[label="B" shape=diamond color=black fillcolor=aquamarine style=filled];
p0x626c0a7c70a0[label="E" ];
p0x626c0a7c70a0 -> p0x626c0a7ca000;
p0x626c0a7c71e0[label="F" shape=diamond color=black fillcolor=aquamarine style=filled];
p0x626c0a7ca7b0[shape=box3d, color=blue, label="Pipeline [m1]"];
p0x626c0a7ca9f0[label="Loop_body" ];
p0x626c0a7ca9f0 -> p0x626c0a7caad0;
p0x626c0a7cac40[label="Loop_exit" ];
p0x626c0a7c8070[shape=box3d, color=blue, label="G [m2]"];
p0x626c0a7c8fb0[shape=box3d, color=blue, label="H [m3]"];
p0x626c0a7c9f20[shape=box3d, color=blue, label="I [m4]"];
p0x626c0a7ca000[label="F_decl" shape=diamond color=black fillcolor=aquamarine style=filled];
p0x626c0a7ca000 -> p0x626c0a7c8070 [style=dashed label="0"];
p0x626c0a7ca000 -> p0x626c0a7c8fb0 [style=dashed label="1"];
p0x626c0a7ca000 -> p0x626c0a7c9f20 [style=dashed label="2"];
p0x626c0a7c6930[label="B_decl" shape=diamond color=black fillcolor=aquamarine style=filled];
p0x626c0a7c6930 -> p0x626c0a7c4fb0 [style=dashed label="0"];
p0x626c0a7c6930 -> p0x626c0a7c6850 [style=dashed label="1"];
p0x626c0a7c4fb0[shape=box3d, color=blue, label="C [m5]"];
p0x626c0a7caad0[label="Loop_condition" shape=diamond color=black fillcolor=aquamarine style=filled];
p0x626c0a7c6850[shape=box3d, color=blue, label="D [m6]"];
p0x626c0a7cad20[label="ManualLoop_body" ];
p0x626c0a7cad20 -> p0x626c0a7cae00;
p0x626c0a7cae00[label="ManualLoop_condition" shape=diamond color=black fillcolor=aquamarine style=filled];
p0x626c0a7cae00 -> p0x626c0a7cad20 [style=dashed label="0"];
p0x626c0a7cae00 -> p0x626c0a7caff0 [style=dashed label="1"];
p0x626c0a7caff0[label="ManualLoop_exit" ];
}
subgraph cluster_p0x626c0a7c50e0 {
label="m6";
p0x626c0a7c5b50[label="D_src" ];
p0x626c0a7c5b50 -> p0x626c0a7c5c30;
p0x626c0a7c5c30[label="D_src_to_x_adapter" ];
p0x626c0a7c5c30 -> p0x626c0a7c6580;
p0x626c0a7c6580[label="D_proc" ];
p0x626c0a7c6580 -> p0x626c0a7c6730;
p0x626c0a7c6730[label="D_sink" ];
}
subgraph cluster_p0x626c0a7c3790 {
label="m5";
p0x626c0a7c4290[label="C_src" ];
p0x626c0a7c4290 -> p0x626c0a7c4370;
p0x626c0a7c4370[label="C_src_to_x_adapter" ];
p0x626c0a7c4370 -> p0x626c0a7c4ce0;
p0x626c0a7c4ce0[label="C_proc" ];
p0x626c0a7c4ce0 -> p0x626c0a7c4e90;
p0x626c0a7c4e90[label="C_sink" ];
}
subgraph cluster_p0x626c0a7c9170 {
label="m4";
p0x626c0a7c9be0[label="Si" ];
p0x626c0a7c9be0 -> p0x626c0a7c9de0;
p0x626c0a7c9de0[label="Ki" ];
}
subgraph cluster_p0x626c0a7c81a0 {
label="m3";
p0x626c0a7c8c90[label="Sh" ];
p0x626c0a7c8c90 -> p0x626c0a7c8e90;
p0x626c0a7c8e90[label="Kh" ];
}
subgraph cluster_p0x626c0a7c7310 {
label="m2";
p0x626c0a7c7d30[label="Sg" ];
p0x626c0a7c7d30 -> p0x626c0a7c7f30;
p0x626c0a7c7f30[label="Kg" ];
}
subgraph cluster_p0x626c0a7ca130 {
label="m1";
p0x626c0a7ca2e0[label="cond" shape=diamond color=black fillcolor=aquamarine style=filled];
p0x626c0a7ca2e0 -> p0x626c0a7ca3c0 [style=dashed label="0"];
p0x626c0a7ca2e0 -> p0x626c0a7ca4c0 [style=dashed label="1"];
p0x626c0a7ca2e0 -> p0x626c0a7ca5a0 [style=dashed label="2"];
p0x626c0a7ca2e0 -> p0x626c0a7ca680 [style=dashed label="3"];
p0x626c0a7ca3c0[label="nprt-0" ];
p0x626c0a7ca4c0[label="nprt-1" ];
p0x626c0a7ca5a0[label="nprt-2" ];
p0x626c0a7ca680[label="nprt-3" ];
}
}
```
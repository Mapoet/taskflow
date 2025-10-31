# Workflow Library 实现技术路线与设计指南

## 一、项目需求与目标

### 1.1 核心需求

1. **定义与实现分离**：头文件仅声明，实现放在 `src/` 目录
2. **模板特化节点**：基于已知数据类型创建编译时类型安全的节点
3. **纯虚基类设计**：所有节点（已知类型/未知类型）派生自统一的 `INode` 基类
4. **构图管理器**：提供高级 API 管理节点增删、输入输出配置以及异步执行
5. **代码风格一致**：与现有 `nodeflow.hpp` 保持一致
6. **Key-based I/O**：所有输入输出通过字符串 key 访问，提供语义清晰的数据流
7. **声明式构图**：自动依赖推断，减少手动配置错误

### 1.2 设计目标

- **类型安全**：编译时类型检查（Typed Nodes）
- **运行时灵活**：动态类型处理（Any-based Nodes）
- **统一接口**：多态支持（INode 基类）
- **易用性**：声明式构图 API（GraphBuilder）
- **可读性**：Key-based I/O 提升代码可维护性

## 二、技术架构设计

### 2.1 分层架构

```
┌─────────────────────────────────────────────────────────┐
│              User Application Layer                     │
│  (Declarative API, Key-based I/O, Auto Dependencies)   │
└─────────────────────────────────────────────────────────┘
                      ▲
                      │
┌─────────────────────────────────────────────────────────┐
│              Workflow Abstraction Layer                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   INode      │  │ GraphBuilder │  │  Node Types  │  │
│  │ (Interface)  │  │  (Manager)    │  │  (Concrete)  │  │
│  │             │  │ + Adapters    │  │              │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
                      ▲
                      │
┌─────────────────────────────────────────────────────────┐
│              Taskflow Execution Layer                   │
│  (Executor, Taskflow, Tasks, Dependencies)              │
└─────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

#### 组件 1: 纯虚基类 `INode`

**设计意图**：提供统一的节点接口，支持多态操作和 key-based 访问

**接口定义**：
```cpp
class INode {
 public:
  virtual ~INode() = default;
  virtual std::string name() const = 0;
  virtual std::string type() const = 0;
  virtual std::function<void()> functor(const char* node_name) const = 0;
  virtual std::shared_future<std::any> get_output_future(const std::string& key) const = 0;
  virtual std::vector<std::string> get_output_keys() const = 0;
};
```

**设计要点**：
- 虚析构函数支持多态销毁
- `name()` 返回节点名称，便于调试和管理
- `type()` 返回类型标识符（"TypedSource", "AnyNode" 等）
- `functor()` 创建 Taskflow 兼容的可执行函数
- `get_output_future(key)` 统一输出访问接口（类型擦除）
- `get_output_keys()` 获取所有输出 key，支持动态查询

#### 组件 2: 模板特化节点（编译时类型安全）

**设计意图**：为已知类型提供零开销的类型安全节点

**节点类型**：
1. **`TypedSource<Outs...>`**：产生多个类型化的输出
2. **`TypedNode<InputsTuple, Outs...>`**：类型化的转换节点
3. **`TypedSink<Ins...>`**：消费多个类型化的输入

**关键设计决策**：

1. **输入类型表示**：使用 `std::tuple<std::shared_future<Ins>...>` 作为 `InputsTuple`
2. **类型萃取**：从 `InputsTuple` 提取值类型 `tuple<Ins...>` 用于操作函数
3. **操作函数签名**：`std::function<std::tuple<Outs...>(ValuesTuple)>`，接收解包后的值
4. **Key-based 输出**：`TypedOutputs` 同时提供索引访问和 key 访问
5. **类型存储**：使用 `std::any` 存储操作函数（类型擦除），在构造时转换为正确的 `std::function`

**数据模型**：

```cpp
template <typename... Outs>
struct TypedOutputs {
  // 类型化的 promises/futures（索引访问）
  std::tuple<std::shared_ptr<std::promise<Outs>>...> promises;
  std::tuple<std::shared_future<Outs>...> futures;
  
  // Key-based 访问（类型擦除）
  std::unordered_map<std::string, std::shared_future<std::any>> futures_map;
  std::vector<std::string> output_keys;
  std::unordered_map<std::string, std::size_t> key_to_index_;
  
  // Any promises（用于同步）
  std::unordered_map<std::size_t, std::shared_ptr<std::promise<std::any>>> any_promises_;
};
```

**实现技巧**：
```cpp
// 类型萃取辅助类
template <typename T>
struct FutureValueType;

template <typename T>
struct FutureValueType<std::shared_future<T>> {
  using type = T;
};

// 从 InputsTuple 提取值类型
template <typename... Futures>
struct ExtractValueTypesHelper<std::tuple<Futures...>> {
  using type = std::tuple<typename FutureValueType<Futures>::type...>;
};

// 使用
using InputsTuple = std::tuple<std::shared_future<double>, std::shared_future<int>>;
using ValuesTuple = ExtractValueTypesHelper<InputsTuple>::type;  // tuple<double, int>
```

#### 组件 3: 运行时类型擦除节点（Any-based）

**设计意图**：支持动态类型和异构数据处理

**节点类型**：
1. **`AnySource`**：使用 `unordered_map<string, any>` 产生初始值
2. **`AnyNode`**：使用 `unordered_map<string, any>` 进行输入输出
3. **`AnySink`**：消费 `unordered_map<string, any>`；可选回调用于结果收集/处理

**关键设计决策**：
- **数据传递**：使用 `std::shared_ptr<std::promise<std::any>>` + `std::shared_future<std::any>`
- **线程安全**：共享指针和共享 future 确保 lambdas 可复制（Taskflow 要求）
- **类型转换**：使用 `std::any_cast<T>` 在运行时提取值
- **Key-based 访问**：所有输入输出通过字符串 key 访问

**数据模型**：

```cpp
struct AnyOutputs {
  std::unordered_map<std::string, std::shared_ptr<std::promise<std::any>>> promises;
  std::unordered_map<std::string, std::shared_future<std::any>> futures;
};
```

#### 组件 4: GraphBuilder 管理器（声明式 API）

**设计意图**：简化构图、依赖配置和执行流程，支持自动依赖推断

**核心功能**：

1. **节点管理**
   - 自动名称生成和冲突检测
   - 统一添加接口（支持类型化/Any 节点）
   - 节点查找和迭代

2. **声明式构图**（推荐）
   - `create_typed_source(name, values, output_keys)` - 创建源节点
   - `create_typed_node<Ins...>(name, input_specs, functor, output_keys)` - 创建节点
     - 输入类型：显式指定 `<Ins...>`
     - 输出类型：从 functor 返回类型自动推断
   - `create_any_source/node/sink()` - Any 节点创建
   - **自动依赖推断**：根据 `input_specs` 自动建立依赖关系

3. **输入辅助函数**
   - `get_input<T>(node_name, key)` - 获取类型化的输入
   - `get_output(node_name, key)` - 获取 type-erased 输出

4. **适配器任务管理**
   - 自动创建适配器任务连接 Typed → Any
   - 适配器任务注册到 `adapter_tasks_` 映射
   - 依赖关系：优先使用 `adapter → target`，无适配器时使用 `source → target`

5. **执行管理**
   - `run(executor)` - 同步执行
   - `run_async(executor)` - 异步执行返回 Future
   - `dump(ostream)` - 输出 DOT 可视化
   - `create_subtask(name, builder_fn)` - 任务执行时构建并运行子图（适合循环体）

**实现要点**：
```cpp
class GraphBuilder {
 private:
  tf::Taskflow taskflow_;
  std::unordered_map<std::string, std::shared_ptr<INode>> nodes_;
  std::unordered_map<std::string, tf::Task> tasks_;
  std::unordered_map<std::string, tf::Task> adapter_tasks_;  // 适配器任务映射
  
 public:
  // 声明式 API（推荐）
  template <typename... Outs>
  std::pair<std::shared_ptr<TypedSource<Outs...>>, tf::Task>
  create_typed_source(const std::string& name, 
                      std::tuple<Outs...> values,
                      const std::vector<std::string>& output_keys);
  
  template <typename... Ins, typename OpType>
  auto create_typed_node(const std::string& name,
                        const std::vector<std::pair<std::string, std::string>>& input_specs,
                        OpType&& functor,
                        const std::vector<std::string>& output_keys);
  
  // 输入辅助函数
  template <typename T>
  std::shared_future<T> get_input(const std::string& node_name, 
                                  const std::string& key) const;
};
```

### 2.3 文件组织

```
workflow/
├── include/workflow/
│   ├── nodeflow.hpp          # 主要头文件（仅声明）
│   └── nodeflow_impl.hpp     # 模板实现（头文件）
├── src/
│   └── nodeflow.cpp          # 非模板代码实现
├── examples/
│   ├── keyed_example.cpp     # Any-based 示例
│   ├── unified_example.cpp  # Key-based API 示例
│   └── declarative_example.cpp # 声明式 API 示例
└── CMakeLists.txt
```

**分离原则**：
- **声明**：在 `nodeflow.hpp` 中，包含类定义、方法声明
- **模板实现**：在 `nodeflow_impl.hpp` 中（必须头文件）
- **非模板实现**：在 `src/nodeflow.cpp` 中（编译为对象文件）

## 三、实现技术路线

### 阶段 1: 基础架构（已完成）✅

✅ **步骤 1.1**: 重构头文件结构
- 将原 `nodeflow.hpp` 中的实现提取到 `src/nodeflow.cpp`
- 保留声明和接口定义在头文件中
- 创建 `nodeflow_impl.hpp` 用于模板实现

✅ **步骤 1.2**: 设计纯虚基类
- 定义 `INode` 接口
- 包含 `name()`, `type()`, `functor()` 纯虚方法
- 添加 `get_output_future(key)` 和 `get_output_keys()` 统一接口

✅ **步骤 1.3**: 更新现有节点继承
- `AnyNode`, `AnySource`, `AnySink` 继承 `INode`
- 实现所有纯虚方法

### 阶段 2: 模板节点开发（已完成）✅

✅ **步骤 2.1**: 实现类型萃取工具
```cpp
// 从 std::shared_future<T> 提取 T
template <typename T> struct FutureValueType<std::shared_future<T>>;

// 从 tuple<futures...> 提取 tuple<values...>
template <typename Tuple> struct ExtractValueTypesHelper;
```

✅ **步骤 2.2**: 实现 TypedSource
- 使用 `std::tuple<Outs...>` 存储初始值
- 使用 `TypedOutputs<Outs...>` 管理输出 promises/futures
- 在 `functor()` 中将值设置到 promises
- 添加 key-based 输出访问

✅ **步骤 2.3**: 实现 TypedNode
- 构造函数接收 `InputsTuple` 和操作函数
- 将操作函数转换为正确的 `std::function` 类型并存储在 `std::any` 中
- `functor()` 中从 futures 提取值，调用操作函数，设置输出 promises
- 添加 key-based 输入/输出访问

✅ **步骤 2.4**: 实现 TypedSink
- 接收 `std::tuple<std::shared_future<Ins>...>` 作为输入
- 在 `functor()` 中提取所有输入值并打印

### 阶段 3: Key-based I/O 系统（已完成）✅

✅ **步骤 3.1**: 扩展 TypedOutputs
- 添加 `futures_map: map<string, shared_future<any>>`
- 添加 `output_keys: vector<string>`
- 添加 `key_to_index_: map<string, size_t>`
- 添加 `any_promises_: map<size_t, shared_ptr<promise<any>>>`

✅ **步骤 3.2**: 实现 key-based 访问方法
- `get_typed<I>(key)` - 通过 key 和索引获取类型化 future
- `get_typed_by_key<T>(key)` - 通过 key 和类型获取类型化 future
- `get(key)` - 获取 type-erased future
- `keys()` - 获取所有输出 key

✅ **步骤 3.3**: 统一接口实现
- `INode::get_output_future(key)` - 所有节点类型统一接口
- `INode::get_output_keys()` - 获取所有输出 key

### 阶段 4: 声明式构图 API（已完成）✅

✅ **步骤 4.1**: 实现声明式节点创建
- `create_typed_source(name, values, output_keys)`
- `create_typed_node<Ins...>(name, input_specs, functor, output_keys)`
  - 输入类型：显式指定
  - 输出类型：从 functor 返回类型自动推断
- `create_any_source/node/sink()`

✅ **步骤 4.2**: 实现自动依赖推断
- 解析 `input_specs: vector<pair<source_node, source_key>>`
- 自动建立依赖关系：`source → target` 或 `adapter → target`
- 无需手动 `precede/succeed` 调用

✅ **步骤 4.3**: 实现适配器任务管理
- 在 `get_typed_input_impl<T>()` 中创建适配器任务
- 适配器任务注册到 `adapter_tasks_["source::key"]`
- 依赖关系：优先使用适配器，否则使用源节点

✅ **步骤 4.4**: 输入辅助函数
- `get_input<T>(node_name, key)` - 获取类型化输入
- `get_output(node_name, key)` - 获取 type-erased 输出

### 阶段 5: 示例与文档（已完成）✅

✅ **步骤 5.1**: 更新示例
- `keyed_example.cpp`：纯 Any-based 示例
- `unified_example.cpp`：Key-based API 示例
- `declarative_example.cpp`：声明式 API 完整示例

✅ **步骤 5.2**: 构建系统更新
- 更新 `CMakeLists.txt` 编译 `src/nodeflow.cpp`
- 添加示例目标

✅ **步骤 5.3**: 文档编写
- 更新 `README.md` - 完整库文档
- 更新 `KEY_BASED_API.md` - Key-based API 详细说明
- 更新 `guide_workflow.md` - 技术路线文档

## 四、关键技术难点与解决方案

### 难点 1: 模板参数包顺序限制

**问题**：`template <typename... Ins, typename... Outs>` 不合法

**解决**：使用 `InputsTuple` 作为单个参数
```cpp
// 不合法：
template <typename... Ins, typename... Outs> class TypedNode;

// 合法：
template <typename InputsTuple, typename... Outs> class TypedNode;
// 使用时：InputsTuple = tuple<shared_future<Ins>...>
```

### 难点 2: TypedNode 操作函数的类型存储

**问题**：需要存储任意可调用对象，但 `std::function` 需要完整类型签名

**解决**：在构造函数中转换为正确的 `std::function` 并存储在 `std::any` 中
```cpp
template <typename OpType>
TypedNode(InputsTuple fin, OpType&& fn, const std::string& name) {
  using ValuesTuple = typename ExtractValueTypesHelper<InputsTuple>::type;
  using OpFuncType = std::function<std::tuple<Outs...>(ValuesTuple)>;
  op_ = OpFuncType(std::forward<OpType>(fn));  // 类型转换并存储在 std::any
}
```

### 难点 3: Key-based 输出访问的实现

**问题**：需要同时支持索引访问（类型安全）和 key 访问（灵活）

**解决**：双重存储 + 映射表
```cpp
template <typename... Outs>
struct TypedOutputs {
  // 索引访问（类型安全）
  std::tuple<std::shared_future<Outs>...> futures;
  
  // Key 访问（类型擦除）
  std::unordered_map<std::string, std::shared_future<std::any>> futures_map;
  std::vector<std::string> output_keys;
  std::unordered_map<std::string, std::size_t> key_to_index_;
  
  // 同步用 Any promises
  std::unordered_map<std::size_t, std::shared_ptr<std::promise<std::any>>> any_promises_;
};
```

### 难点 4: 声明式 API 的输出类型推断

**问题**：如何从 functor 返回类型推断输出类型？

**解决**：使用 `std::invoke_result` 和类型萃取
```cpp
template <typename... Ins, typename OpType>
auto create_typed_node(const std::string& name, /*...*/, OpType&& functor, /*...*/) {
  // 创建测试输入类型
  using TestInput = std::tuple<Ins...>;
  
  // 推断返回类型
  using ReturnType = typename std::invoke_result<OpType, TestInput>::type;
  // ReturnType = std::tuple<Outs...>
  
  // 提取输出类型并创建节点
  using NodeType = TypedNode<InputsTuple, /* 从 ReturnType 提取 Outs... */>;
  // ...
}
```

### 难点 5: 自动依赖推断与适配器管理

**问题**：如何自动建立正确的依赖关系（包括适配器任务）？

**解决**：
1. 在 `create_typed_node` 中遍历 `input_specs`
2. 检查是否存在适配器：`adapter_tasks_["source::key"]`
3. 如果存在适配器：`adapter → target`
4. 否则：`source → target`

```cpp
for (const auto& [source_node, source_key] : input_specs) {
  const std::string adapter_key = source_node + "::" + source_key;
  auto adapter_it = adapter_tasks_.find(adapter_key);
  
  if (adapter_it != adapter_tasks_.end()) {
    adapter_it->second.precede(task);  // 使用适配器
  } else {
    tasks_[source_node].precede(task);  // 直接连接
  }
}
```

### 难点 6: GraphBuilder 中节点名称传递

**问题**：`node->functor(node_name.c_str())` 可能传递临时字符串指针

**解决**：在 lambda 中捕获 `std::string` 而非 `const char*`
```cpp
std::string task_name = node_name;
auto task = taskflow_.emplace([node, task_name]() {
  node->functor(task_name.c_str())();
});
```

### 难点 7: 类型化与 Any 节点互操作

**问题**：Typed 节点输出 `tuple<shared_future<T>...>`，Any 节点需要 `unordered_map<string, shared_future<any>>`

**解决**：使用适配器任务桥接
```cpp
// 在 get_typed_input_impl 中创建适配器
auto any_fut = node->get_output_future(key);  // 获取 any future
auto p_typed = std::make_shared<std::promise<T>>();
auto f_typed = p_typed->get_future().share();

// 适配器任务：从 any future 提取值并转换为 typed
auto adapter = taskflow_.emplace([any_fut, p_typed]() {
  std::any value = any_fut.get();
  T typed_value = std::any_cast<T>(value);
  p_typed->set_value(std::move(typed_value));
}).name(node_name + "_to_" + key + "_adapter");

// 注册并建立依赖
adapter_tasks_[node_name + "::" + key] = adapter;
source_task.precede(adapter);
```

## 五、使用模式与最佳实践

### 模式 1: 声明式构图（推荐）✅

**适用场景**：所有新代码

**特点**：
- 最小代码量
- 自动依赖推断
- Key-based I/O

```cpp
wf::GraphBuilder builder("workflow");
auto [A, _] = builder.create_typed_source("A",
  std::make_tuple(3.5, 7), {"x", "k"});

auto [B, _] = builder.create_typed_node<double>("B",
  {{"A", "x"}},  // 输入规范
  [](const std::tuple<double>& in) {
    return std::make_tuple(std::get<0>(in) + 1.0);
  },
  {"b"}  // 输出 key
);

// 依赖自动推断：B 依赖 A
builder.run(executor);
```

### 模式 2: 纯类型化工作流（性能优先）

**适用场景**：类型在编译时已知，需要最大性能

```cpp
wf::GraphBuilder builder("typed_workflow");
auto A = std::make_shared<wf::TypedSource<double>>(
  std::make_tuple(3.5), {"x"}, "A");
auto B = std::make_shared<wf::TypedNode</*...*/>>(/*...*/, {"b"}, "B");
builder.precede(tA, std::vector<tf::Task>{tB});  // 手动依赖（deprecated）
```

### 模式 3: 纯 Any-based 工作流（灵活性优先）

**适用场景**：类型在运行时确定，需要动态组合

```cpp
wf::GraphBuilder builder("any_workflow");
auto [A, _] = builder.create_any_source("A",
  {{"x", std::any{3.5}}});
auto [B, _] = builder.create_any_node("B",
  {{"A", "x"}},
  [](const auto& in) {
    double x = std::any_cast<double>(in.at("x"));
    return std::unordered_map<std::string, std::any>{{"b", x + 1.0}};
  },
  {"b"});
```

### 模式 4: 混合工作流（平衡性能与灵活性）

**适用场景**：核心计算路径使用类型化节点，接口层使用 Any 节点

```cpp
// 类型化核心计算（性能）
auto [D, _] = builder.create_typed_node<double, double>("D",
  {{"B", "b"}, {"C", "c"}},
  [](const std::tuple<double, double>& in) {
    return std::make_tuple(std::get<0>(in) * std::get<1>(in));
  },
  {"prod"});

// Any-based 接口（灵活性）
auto [H, _] = builder.create_any_sink("H",
  {{"D", "prod"}, {"G", "sum"}});
// 适配器任务自动创建：D::prod → H
```

## 六、高级控制流节点实现

### 6.1 控制流节点概述

Workflow 库在声明式 API 基础上，提供了四种高级控制流节点：

1. **条件节点 (Condition Node)**：基于条件的 if-else 分支
2. **多条件节点 (Multi-Condition Node)**：并行执行多个分支
3. **管道节点 (Pipeline Node)**：结构化流水线执行
4. **循环节点 (Loop Node)**：迭代执行直到条件满足

所有控制流节点都使用声明式 API 构建，支持在子图中使用 `gb` 创建执行结构体并传递参数。

### 6.2 子图创建机制

**核心 API**：
```cpp
tf::Task create_subgraph(const std::string& name,
                        const std::function<void(GraphBuilder&)>& builder_fn);
```

**设计要点**：
- 子图构建函数 `builder_fn` 在构图时调用一次
- 子图作为 `composed_of` 任务嵌入主图
- 子图内部可以使用完整的声明式 API
- 参数通过 lambda 捕获传递到子图内部

**示例**：
```cpp
auto module_task = builder.create_subgraph("Module", [&counter](wf::GraphBuilder& gb){
  // 在子图中使用声明式 API
  auto [src, _] = gb.create_typed_source("src", std::make_tuple(1.0), {"x"});
  auto [proc, _] = gb.create_typed_node<double>("proc", 
    {{"src", "x"}},
    [&counter](const std::tuple<double>& in) {
      // 使用捕获的参数
      return std::make_tuple(std::get<0>(in) + counter);
    },
    {"y"}
  );
  auto [sink, _] = gb.create_any_sink("sink", {{"proc", "y"}});
  // 依赖关系自动推断
});
```

### 6.3 条件节点实现

**API**：
```cpp
tf::Task create_condition_decl(const std::string& name,
                              const std::vector<std::string>& depend_on_nodes,
                              std::function<int()> condition_func,
                              const std::vector<tf::Task>& successors);
```

**实现机制**：
- 使用 Taskflow 的 `emplace` 创建条件任务
- 条件函数返回整数索引，选择后继节点
- 返回 0 执行第一个后继，返回 1 执行第二个后继，以此类推
- 依赖关系：前置节点 → 条件节点 → 后继节点

### 6.4 多条件节点实现

**API**：
```cpp
tf::Task create_multi_condition_decl(const std::string& name,
                                    const std::vector<std::string>& depend_on_nodes,
                                    std::function<tf::SmallVector<int>()> func,
                                    const std::vector<tf::Task>& successors);
```

**实现机制**：
- 条件函数返回 `tf::SmallVector<int>`，包含要执行的分支索引
- 多个分支并行执行（由 Taskflow 调度器管理）
- 用于需要同时执行多个路径的场景

### 6.5 管道节点实现

**API**：
```cpp
template <typename... Ps>
tf::Task create_pipeline_node(const std::string& name,
                              std::tuple<tf::Pipe<Ps>...>&& pipes,
                              size_t lines);
```

**实现机制**：
- 直接使用 Taskflow 的 `Pipeline` 算法
- 管道阶段通过 `tf::Pipe` 定义，支持 `SERIAL` 和 `PARALLEL` 类型
- `lines` 参数指定并行流水线数量
- 内部使用 `tf::Pipeline` 对象，通过 `composed_of` 嵌入主图

### 6.6 循环节点实现

**API**：
```cpp
tf::Task create_loop_decl(const std::string& name,
                         const std::vector<std::string>& depend_on_nodes,
                         tf::Task body_task,
                         std::function<int()> condition_func,
                         tf::Task exit_task);
```

**实现机制**：
1. **循环体**：推荐通过 `create_subtask` 创建，使用声明式 API 构建内部结构（避免模块任务仅运行一次的限制）
2. **条件任务**：创建条件任务连接循环体和退出任务
3. **依赖关系**：
   - 前置节点 → 循环体（首次执行）
   - 循环体 → 条件任务
   - 条件返回 0 → 循环体（继续循环）
   - 条件返回非 0 → 退出任务（退出循环）

**关键设计**：
- 循环体通过 `create_subtask` 创建，每次迭代重建并运行子图，确保可重复执行
- 参数（如 counter）通过 lambda 捕获传递
- 条件函数只读取状态，不修改（修改在循环体中完成）
- 支持可选的退出动作子图

**示例**：
```cpp
int counter = 0;

// 循环体使用声明式 API
auto loop_body_task = builder.create_subgraph("LoopBody", [&counter](wf::GraphBuilder& gb){
  auto [trigger, _] = gb.create_typed_source("trigger", std::make_tuple(0), {"t"});
  auto [process, _] = gb.create_typed_node<int>("process",
    {{"trigger", "t"}},
    [&counter](const std::tuple<int>&) {
      ++counter;  // 在循环体中修改
      return std::make_tuple(counter);
    },
    {"result"}
  );
  auto [sink, _] = gb.create_any_sink("sink", {{"process", "result"}});
});

// 循环条件
builder.create_loop_decl("Loop",
  {"A"},  // 依赖节点 A
  loop_body_task,
  [&counter]() -> int { 
    return (counter < 5) ? 0 : 1;  // 只读取，不修改
  },
  exit_task
);
```

### 6.7 控制流节点设计原则

1. **声明式优先**：所有控制流节点都支持声明式 API
2. **子图重用**：分支、循环体等通过子图创建，便于复用
3. **参数传递**：通过 lambda 捕获传递外部变量，支持复杂状态管理
4. **自动依赖**：子图内部依赖自动推断，无需手动配置
5. **类型灵活**：支持 Typed 和 Any 节点的混合使用

## 七、扩展方向

### 7.1 类型系统增强

- ✅ **类型适配**：自动生成类型化 ↔ Any 适配器（已实现）
- ⏳ **类型验证**：在 Any 节点中添加运行时类型检查
- ⏳ **类型推断增强**：从函数签名完全自动推断节点输入输出类型（部分实现）

### 7.2 构图能力增强

- ✅ **子图支持**：通过 `create_subgraph` 支持节点包含子工作流（已实现）
- ✅ **条件分支**：通过 `create_condition_decl` 支持基于条件的条件执行（已实现）
- ✅ **多条件分支**：通过 `create_multi_condition_decl` 支持并行分支执行（已实现）
- ✅ **循环支持**：通过 `create_loop_decl` 支持迭代执行子图（已实现）
- ✅ **管道执行**：通过 `create_pipeline_node` 支持结构化流水线（已实现）
- ⏳ **动态节点**：运行时添加/删除节点

### 7.3 执行优化

- ⏳ **调度策略**：优先级调度、资源感知调度
- ⏳ **数据局部性**：优化数据传递路径
- ⏳ **并行度控制**：限制并发任务数
- ⏳ **性能分析**：集成性能分析工具

### 7.4 工具支持

- **可视化工具**：图形界面构建和可视化工作流
- **调试工具**：数据流追踪、断点支持
- **测试框架**：节点单元测试、集成测试
- **文档生成**：自动生成 API 文档和使用示例

## 七、参考实现

### 7.1 关键文件

- `workflow/include/workflow/nodeflow.hpp`：主要接口定义（INode, 节点类型, GraphBuilder）
- `workflow/include/workflow/nodeflow_impl.hpp`：模板实现（Typed 节点模板）
- `workflow/src/nodeflow.cpp`：非模板实现（Any 节点, GraphBuilder, 控制流节点）
- `workflow/examples/declarative_example.cpp`：声明式 API 完整示例
- `workflow/examples/advanced_control_flow.cpp`：高级控制流节点示例

### 7.2 参考示例

**Workflow 库示例**（`workflow/examples/`）：
- ✅ `declarative_example.cpp`：声明式 API 完整示例（推荐）
- ✅ `unified_example.cpp`：Key-based API 演示
- ✅ `keyed_example.cpp`：纯 Any-based 工作流
- ✅ `advanced_control_flow.cpp`：高级控制流节点（condition, multi-condition, pipeline, loop）

**Taskflow 基础示例**（`examples/`）：
- `simple.cpp`：基础任务依赖
- `dataflow_arith.cpp`：数据流传递（promise/future）
- `condition.cpp`：条件任务示例
- `while_loop.cpp`：循环示例

## 八、总结

Workflow 库通过以下设计实现了灵活且类型安全的数据流编程：

1. **统一接口**：`INode` 基类提供多态支持
2. **双重类型系统**：编译时类型安全 + 运行时灵活性
3. **Key-based I/O**：字符串 key 访问提升可读性和可维护性
4. **声明式构图**：自动依赖推断减少手动配置错误
5. **简化 API**：`GraphBuilder` 降低使用复杂度
6. **清晰分离**：声明/实现分离，易于维护和扩展

该设计既满足了高性能需求（类型化节点），又保持了足够的灵活性（Any 节点），同时通过统一的接口和声明式 API 实现了良好的可扩展性和易用性。

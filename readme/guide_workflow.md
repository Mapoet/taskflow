# Workflow Library 实现技术路线与设计指南

## 一、项目需求与目标

### 1.1 核心需求

1. **定义与实现分离**：头文件仅声明，实现放在 `src/` 目录
2. **模板特化节点**：基于已知数据类型创建编译时类型安全的节点
3. **纯虚基类设计**：所有节点（已知类型/未知类型）派生自统一的 `INode` 基类
4. **构图管理器**：提供高级 API 管理节点增删、输入输出配置以及异步执行
5. **代码风格一致**：与现有 `nodeflow.hpp` 保持一致

### 1.2 设计目标

- **类型安全**：编译时类型检查（Typed Nodes）
- **运行时灵活**：动态类型处理（Any-based Nodes）
- **统一接口**：多态支持（INode 基类）
- **易用性**：简化的构图 API（GraphBuilder）

## 二、技术架构设计

### 2.1 分层架构

```
┌─────────────────────────────────────────────────────────┐
│              User Application Layer                     │
│  (GraphBuilder, Node Creation, Execution)              │
└─────────────────────────────────────────────────────────┘
                      ▲
                      │
┌─────────────────────────────────────────────────────────┐
│              Workflow Abstraction Layer                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   INode      │  │ GraphBuilder │  │  Node Types  │  │
│  │ (Interface)  │  │  (Manager)   │  │  (Concrete)  │  │
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

**设计意图**：提供统一的节点接口，支持多态操作

**接口定义**：
```cpp
class INode {
 public:
  virtual ~INode() = default;
  virtual std::string name() const = 0;
  virtual std::string type() const = 0;
  virtual std::function<void()> functor(const char* node_name) const = 0;
};
```

**设计要点**：
- 虚析构函数支持多态销毁
- `name()` 返回节点名称，便于调试和管理
- `type()` 返回类型标识符（"TypedNode", "AnyNode" 等）
- `functor()` 创建 Taskflow 兼容的可执行函数

#### 组件 2: 模板特化节点（编译时类型安全）

**设计意图**：为已知类型提供零开销的类型安全节点

**节点类型**：
1. **`TypedSource<Outs...>`**：产生多个类型化的输出
2. **`TypedNode<InputsTuple, Outs...>`**：类型化的转换节点
3. **`TypedSink<Ins...>`**：消费多个类型化的输入

**关键设计决策**：
- **输入类型表示**：使用 `std::tuple<std::shared_future<Ins>...>` 作为 `InputsTuple`
- **类型萃取**：从 `InputsTuple` 提取值类型 `tuple<Ins...>` 用于操作函数
- **操作函数签名**：`std::function<std::tuple<Outs...>(ValuesTuple)>`，接收解包后的值
- **类型存储**：使用 `std::any` 存储操作函数（类型擦除），在构造时转换为正确的 `std::function`

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
```

#### 组件 3: 运行时类型擦除节点（Any-based）

**设计意图**：支持动态类型和异构数据处理

**节点类型**：
1. **`AnySource`**：使用 `unordered_map<string, any>` 产生初始值
2. **`AnyNode`**：使用 `unordered_map<string, any>` 进行输入输出
3. **`AnySink`**：消费 `unordered_map<string, any>` 并打印结果

**关键设计决策**：
- **数据传递**：使用 `std::shared_ptr<std::promise<std::any>>` + `std::shared_future<std::any>`
- **线程安全**：共享指针和共享 future 确保 lambdas 可复制（Taskflow 要求）
- **类型转换**：使用 `std::any_cast<T>` 在运行时提取值

#### 组件 4: GraphBuilder 管理器

**设计意图**：简化构图、依赖配置和执行流程

**核心功能**：

1. **节点管理**
   - 自动名称生成和冲突检测
   - 统一添加接口（支持类型化/Any 节点）
   - 节点查找和迭代

2. **依赖配置**
   - `precede(from, to)`：设置 from → to 依赖
   - `succeed(to, from)`：设置 from → to 依赖（反向语法）
   - 支持单个任务或容器（vector/list）

3. **执行管理**
   - `run(executor)`：同步执行
   - `run_async(executor)`：异步执行返回 Future
   - `dump(ostream)`：输出 DOT 可视化

**实现要点**：
```cpp
class GraphBuilder {
 private:
  tf::Taskflow taskflow_;
  std::unordered_map<std::string, std::shared_ptr<INode>> nodes_;
  std::unordered_map<std::string, tf::Task> tasks_;
  
 public:
  tf::Task add_node(std::shared_ptr<INode> node);
  // ... template methods for typed nodes
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
│   └── unified_example.cpp    # 统一 API 示例
└── CMakeLists.txt
```

**分离原则**：
- **声明**：在 `nodeflow.hpp` 中，包含类定义、方法声明
- **模板实现**：在 `nodeflow_impl.hpp` 中（必须头文件）
- **非模板实现**：在 `src/nodeflow.cpp` 中（编译为对象文件）

## 三、实现技术路线

### 阶段 1: 基础架构（已完成）

✅ **步骤 1.1**: 重构头文件结构
- 将原 `nodeflow.hpp` 中的实现提取到 `src/nodeflow.cpp`
- 保留声明和接口定义在头文件中
- 创建 `nodeflow_impl.hpp` 用于模板实现

✅ **步骤 1.2**: 设计纯虚基类
- 定义 `INode` 接口
- 包含 `name()`, `type()`, `functor()` 纯虚方法
- 确保所有节点类型可多态使用

✅ **步骤 1.3**: 更新现有节点继承
- `AnyNode`, `AnySource`, `AnySink` 继承 `INode`
- 实现所有纯虚方法

### 阶段 2: 模板节点开发（已完成）

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

✅ **步骤 2.3**: 实现 TypedNode
- 构造函数接收 `InputsTuple` 和操作函数
- 将操作函数转换为正确的 `std::function` 类型并存储在 `std::any` 中
- `functor()` 中从 futures 提取值，调用操作函数，设置输出 promises

✅ **步骤 2.4**: 实现 TypedSink
- 接收 `std::tuple<std::shared_future<Ins>...>` 作为输入
- 在 `functor()` 中提取所有输入值并打印

### 阶段 3: GraphBuilder 开发（已完成）

✅ **步骤 3.1**: 节点管理
- `add_node()`：统一入口，处理名称冲突
- 模板方法 `add_typed_*()` 和 `add_any_*()` 便捷接口

✅ **步骤 3.2**: 依赖配置
- `precede()` / `succeed()` 单任务版本
- 模板版本支持容器（vector/list）

✅ **步骤 3.3**: 执行接口
- `run()` 同步执行
- `run_async()` 异步执行
- `dump()` 图可视化

### 阶段 4: 示例与测试（已完成）

✅ **步骤 4.1**: 更新示例
- `keyed_example.cpp`：纯 Any-based 示例
- `unified_example.cpp`：展示统一 API、多态、混合使用

✅ **步骤 4.2**: 构建系统更新
- 更新 `CMakeLists.txt` 编译 `src/nodeflow.cpp`
- 添加 `unified_example` 目标

✅ **步骤 4.3**: 修复编译错误
- 模板参数包顺序问题（使用 `InputsTuple` 替代参数包）
- `std::any` 存储操作函数并正确提取类型
- Future 返回类型（`tf::Future<void>`）

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
  op_ = OpFuncType(std::forward<OpType>(fn));  // 类型转换
}
```

### 难点 3: GraphBuilder 中节点名称传递

**问题**：`node->functor(node_name.c_str())` 可能传递临时字符串指针

**解决**：在 lambda 中捕获 `std::string` 而非 `const char*`
```cpp
std::string task_name = node_name;
auto task = taskflow_.emplace([node, task_name]() {
  node->functor(task_name.c_str())();
});
```

### 难点 4: 类型化与 Any 节点互操作

**问题**：Typed 节点输出 `tuple<shared_future<T>...>`，Any 节点需要 `unordered_map<string, shared_future<any>>`

**解决**：使用适配器任务桥接
```cpp
// 创建 promise/future 对
auto p_prod = std::make_shared<std::promise<std::any>>();
auto f_prod = p_prod->get_future().share();

// 适配器任务：从 typed future 提取值并转换为 any
auto adapter = taskflow.emplace([p_prod, typed_fut]() {
  p_prod->set_value(std::any{typed_fut.get()});
});
```

## 五、使用模式与最佳实践

### 模式 1: 纯类型化工作流（性能优先）

适用场景：类型在编译时已知，需要最大性能

```cpp
wf::GraphBuilder builder("typed_workflow");
auto A = std::make_shared<wf::TypedSource<double>>(std::make_tuple(3.5), "A");
auto B = std::make_shared<wf::TypedNode</*...*/>>(/*...*/);
builder.precede(tA, std::vector<tf::Task>{tB});
```

### 模式 2: 纯 Any-based 工作流（灵活性优先）

适用场景：类型在运行时确定，需要动态组合

```cpp
wf::GraphBuilder builder("any_workflow");
auto A = std::make_shared<wf::AnySource>({{"x", std::any{3.5}}}, "A");
auto B = std::make_shared<wf::AnyNode>(/*...*/);
```

### 模式 3: 混合工作流（平衡性能与灵活性）

适用场景：核心计算路径使用类型化节点，接口层使用 Any 节点

```cpp
// 类型化核心计算
auto compute = std::make_shared<wf::TypedNode</*...*/>>(/*...*/);

// 适配器：类型化 -> Any
auto adapter = /*...*/;

// Any-based 接口
auto sink = std::make_shared<wf::AnySink>(/*...*/);
```

## 六、扩展方向

### 6.1 类型系统增强

- **类型验证**：在 Any 节点中添加运行时类型检查
- **类型推断**：从函数签名自动推断节点输入输出类型
- **类型适配**：自动生成类型化 ↔ Any 适配器

### 6.2 构图能力增强

- **子图支持**：节点可以包含子工作流
- **条件分支**：基于数据值的条件执行
- **循环支持**：迭代执行子图
- **动态节点**：运行时添加/删除节点

### 6.3 执行优化

- **调度策略**：优先级调度、资源感知调度
- **数据局部性**：优化数据传递路径
- **并行度控制**：限制并发任务数
- **性能分析**：集成性能分析工具

### 6.4 工具支持

- **可视化工具**：图形界面构建和可视化工作流
- **调试工具**：数据流追踪、断点支持
- **测试框架**：节点单元测试、集成测试
- **文档生成**：自动生成 API 文档和使用示例

## 七、参考实现

### 7.1 关键文件

- `workflow/include/workflow/nodeflow.hpp`：主要接口定义
- `workflow/include/workflow/nodeflow_impl.hpp`：模板实现
- `workflow/src/nodeflow.cpp`：非模板实现
- `workflow/examples/unified_example.cpp`：完整示例

### 7.2 参考示例

参考 `examples/` 目录下的示例了解 Taskflow 的各种用法：
- `simple.cpp`：基础任务依赖
- `dataflow_arith.cpp`：数据流传递（promise/future）
- `nodeflow.cpp`：节点封装模式
- `nodeflow_variadic.cpp`：变长参数节点
- `varnodeflow.cpp`：Any 类型节点

## 八、总结

Workflow 库通过以下设计实现了灵活且类型安全的数据流编程：

1. **统一接口**：`INode` 基类提供多态支持
2. **双重类型系统**：编译时类型安全 + 运行时灵活性
3. **简化 API**：`GraphBuilder` 降低使用复杂度
4. **清晰分离**：声明/实现分离，易于维护和扩展

该设计既满足了高性能需求（类型化节点），又保持了足够的灵活性（Any 节点），同时通过统一的接口实现了良好的可扩展性。

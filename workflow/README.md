# Workflow: Unified Nodeflow Library for Taskflow

A high-level **declarative dataflow library** built on Taskflow, supporting both **compile-time type-safe nodes** and **runtime type-erased nodes** through a unified, key-based interface with **automatic dependency inference**.

## 🎯 Overview

The Workflow library provides a powerful abstraction for building dataflow graphs with:

- **🔑 Key-based I/O**: All inputs/outputs accessed via string keys for clarity and flexibility
- **🚀 Declarative API**: Create nodes with input specifications; dependencies auto-inferred
- **⚡ Type Safety**: Compile-time type-safe nodes (`TypedNode`) for zero-overhead performance
- **🔀 Runtime Flexibility**: Dynamic type handling (`AnyNode`) for heterogeneous data
- **🔗 Unified Interface**: Polymorphic `INode` base class for all node types
- **🎨 Graph Builder**: High-level API managing construction, execution, and visualization

## 🔄 What's New (Latest)

- **✨ NEW: Taskflow Algorithm Nodes** - Declarative API wrappers for Taskflow's parallel algorithms:
  - `create_for_each` - Parallel iteration over containers
  - `create_for_each_index` - Parallel iteration over index ranges with **return value collection**
  - `create_reduce` - Parallel reduction operations
  - `create_transform` - Parallel transformation operations
  - All algorithms support string-keyed inputs/outputs and automatic dependency inference
  - `create_loop_decl` (body_task version) uses master mode: `input_specs` for condition inputs, manual dependency setup
- Added `create_subtask(name, builder_fn)`: builds and runs a fresh subgraph at task execution time. Ideal for loop bodies to avoid module-task single-run semantics.
- Enhanced sinks with callbacks:
  - `create_any_sink(..., callback)` where callback receives `std::unordered_map<std::string, std::any>`
  - `create_typed_sink<Ins...>(..., callback)` where callback receives `std::tuple<Ins...>`
- Cleaned default console noise: removed builtin "emitted"/"done" prints from nodes; output is now user-controlled (e.g., via sink callbacks).
- Loop examples updated to use `create_subtask` for iterative execution; `advanced_control_flow.cpp` and `loop_only.cpp` verified.

## 📐 Architecture

### Design Principles

1. **Separation of Concerns**: Declarations in `include/`, implementations in `src/`
2. **Type Safety with Flexibility**: Typed nodes (compile-time) + Any nodes (runtime)
3. **Polymorphism**: Unified `INode` interface for all node types
4. **Declarative Composition**: `GraphBuilder` with automatic dependency inference
5. **Key-based Access**: String keys for inputs/outputs instead of tuple indices

### Component Overview

```
┌─────────────────────────────────────────────────────────┐
│                    INode (Base)                         │
│  - name(): string                                       │
│  - type(): string                                       │
│  - functor(): function<void()>                         │
│  - get_output_future(key): shared_future<any>         │
│  - get_output_keys(): vector<string>                   │
└─────────────────────────────────────────────────────────┘
          ▲                    ▲
          │                    │
    ┌─────┴─────┐      ┌──────┴──────┐
    │           │      │              │
┌───▼────┐ ┌───▼────┐ │  ┌─────────▼─┐
│ Typed  │ │ Typed  │ │  │ Any-based│
│ Nodes  │ │ Source │ │  │ Nodes    │
│        │ │ Sink   │ │  │ Source   │
└────────┘ └────────┘ │  │ Sink     │
                      └──┴──────────┘
```

## 📁 Directory Structure

```
workflow/
├── include/workflow/
│   ├── nodeflow.hpp          # Main header (declarations)
│   └── nodeflow_impl.hpp     # Template implementations
├── src/
│   └── nodeflow.cpp          # Implementation for non-template code
├── examples/
│   ├── keyed_example.cpp      # Any-based nodes example
│   ├── unified_example.cpp    # Key-based API demonstration
│   ├── declarative_example.cpp # Declarative API with auto-deps
│   ├── advanced_control_flow.cpp # Control flow (condition, loop, pipeline)
│   ├── loop_only.cpp         # Loop-only demonstration
│   └── algorithm_example.cpp # Taskflow algorithm nodes example
├── CMakeLists.txt
├── README.md
└── KEY_BASED_API.md
```

## 🏗️ Core Components

### 1. Pure Virtual Base Class: `INode`

All nodes inherit from `INode`, providing a unified, polymorphic interface:

```cpp
class INode {
 public:
  virtual std::string name() const = 0;
  virtual std::string type() const = 0;
  virtual std::function<void()> functor(const char* node_name) const = 0;
  virtual std::shared_future<std::any> get_output_future(const std::string& key) const = 0;
  virtual std::vector<std::string> get_output_keys() const = 0;
};
```

**Benefits**:
- Enables polymorphism: store nodes as `std::shared_ptr<INode>`
- Unified node management and inspection
- Easy iteration over all nodes regardless of type
- Key-based output access regardless of node type

### 2. Typed Nodes (Compile-time Type-safe)

Type-safe nodes for known input/output types with zero runtime overhead.

#### `TypedSource<Outs...>`

Produces initial values with compile-time type checking and key-based outputs:

```cpp
// Create source with explicit output keys
auto A = std::make_shared<wf::TypedSource<double, int>>(
  std::make_tuple(3.5, 7), 
  std::vector<std::string>{"x", "k"},  // Output keys
  "A"
);

// Access typed futures via key (type-safe)
auto x_fut = A->out.get_typed<0>("x");  // double
auto k_fut = A->out.get_typed<1>("k");  // int

// Or access via unified interface (type-erased)
auto x_any_fut = A->get_output_future("x");  // shared_future<any>
auto keys = A->get_output_keys();  // ["x", "k"]
```

**Data Structure**:
- `values: tuple<Outs...>` - Initial values
- `out: TypedOutputs<Outs...>` - Output management
  - `futures: tuple<shared_future<Outs>...>` - Typed futures (index-based)
  - `futures_map: map<string, shared_future<any>>` - Key-based access (type-erased)
  - `output_keys: vector<string>` - Ordered key names

#### `TypedNode<InputsTuple, Outs...>`

Transforms typed inputs to typed outputs with key-based I/O:

```cpp
using B_Inputs = std::tuple<std::shared_future<double>>;
auto B = std::make_shared<wf::TypedNode<B_Inputs, double>>(
  std::make_tuple(x_fut),  // Input futures
  [](const std::tuple<double>& in) {  // Functor receives unwrapped values
    double x = std::get<0>(in);
    return std::make_tuple(x + 1.0);
  },
  std::vector<std::string>{"b"},  // Output key
  "B"
);

// Access output via key
auto b_fut = B->out.get_typed<0>("b");
auto b_any_fut = B->get_output_future("b");
```

**Data Structure**:
- `inputs: InputsTuple` - `tuple<shared_future<Ins>...>` - Input futures
- `out: TypedOutputs<Outs...>` - Output management (same as `TypedSource`)
- `op_: std::any` - Type-erased operation function
  - Stored as: `std::function<std::tuple<Outs...>(ValuesTuple)>`
  - `ValuesTuple` = unwrapped value types (`tuple<Ins...>`)

**Type Extraction**:
```cpp
// Helper to extract value types from InputsTuple
template <typename T>
struct FutureValueType<std::shared_future<T>> {
  using type = T;
};

// Extract: tuple<shared_future<Ins>...> -> tuple<Ins...>
template <typename... Futures>
struct ExtractValueTypesHelper<std::tuple<Futures...>> {
  using type = std::tuple<typename FutureValueType<Futures>::type...>;
};
```

#### `TypedSink<Ins...>`

Consumes typed values:

```cpp
auto H = std::make_shared<wf::TypedSink<double, int>>(
  std::make_tuple(prod_fut, count_fut), "H"
);
```

**Advantages of Typed Nodes**:
- ✅ Compile-time type checking
- ✅ Zero runtime type overhead
- ✅ IDE autocomplete support
- ✅ Clear type contracts
- ✅ Full compiler optimization

### 3. Any-based Nodes (Runtime Type-erased)

Flexible nodes using `std::any` for heterogeneous types and dynamic workflows.

#### `AnySource`

```cpp
auto A = std::make_shared<wf::AnySource>(
  std::unordered_map<std::string, std::any>{
    {"x", std::any{3.5}},
    {"k", std::any{7}}
  },
  "A"
);

// Access outputs via key
auto x_fut = A->out.futures.at("x");  // shared_future<any>
auto keys = A->get_output_keys();  // ["x", "k"]
```

**Data Structure**:
- `values: map<string, any>` - Initial values
- `out: AnyOutputs` - Output management
  - `promises: map<string, shared_ptr<promise<any>>>` - Output promises
  - `futures: map<string, shared_future<any>>` - Output futures

#### `AnyNode`

```cpp
auto D = std::make_shared<wf::AnyNode>(
  std::unordered_map<std::string, std::shared_future<std::any>>{
    {"b", B->out.futures.at("b")},
    {"c", C->out.futures.at("c")}
  },
  {"prod"},  // Output keys
  [](const std::unordered_map<std::string, std::any>& in) {
    double b = std::any_cast<double>(in.at("b"));
    double c = std::any_cast<double>(in.at("c"));
    return std::unordered_map<std::string, std::any>{{"prod", b * c}};
  },
  "D"
);
```

**Data Structure**:
- `inputs: map<string, shared_future<any>>` - Input futures (keyed)
- `out: AnyOutputs` - Output management
- `op: function<map<string,any>(const map<string,any>&)>` - Operation function

#### `AnySink`

```cpp
auto H = std::make_shared<wf::AnySink>(
  std::unordered_map<std::string, std::shared_future<std::any>>{
    {"prod", D->get_output_future("prod")},
    {"sum", G->get_output_future("sum")},
    {"parity", G->get_output_future("parity")}
  },
  "H"
);
```

**Advantages of Any Nodes**:
- ✅ Dynamic type handling
- ✅ Easy mixing of different types
- ✅ String-keyed access for clarity
- ✅ Runtime flexibility

### 4. Graph Builder: Declarative API

High-level API for building and executing workflows with **automatic dependency inference**.

#### Traditional API (Manual Dependencies)

```cpp
wf::GraphBuilder builder("my_workflow");
tf::Executor executor;

// Add nodes
auto tA = builder.add_typed_source(A);
auto tB = builder.add_typed_node(B);
auto tH = builder.add_any_sink(H);

// Manually configure dependencies
builder.precede(tA, std::vector<tf::Task>{tB, tC});
builder.succeed(tH, std::vector<tf::Task>{tD, tG});

// Execute
builder.run(executor);
```

**Note**: The `precede`/`succeed` methods are now **deprecated**. Use declarative API instead.

#### 🎯 Declarative API (Recommended)

**Key Features**:
- ✅ Key-based input specifications: `{{"source_node", "source_key"}, ...}`
- ✅ Automatic dependency inference from input specs
- ✅ Output types inferred from functor return type
- ✅ No manual `precede`/`succeed` calls needed

**Creating Source Nodes**:

```cpp
auto [A, tA] = builder.create_typed_source("A",
  std::make_tuple(3.5, 7),
  std::vector<std::string>{"x", "k"}
);

// Or for Any-based
auto [A, tA] = builder.create_any_source("A",
  std::unordered_map<std::string, std::any>{
    {"x", std::any{3.5}},
    {"k", std::any{7}}
  }
);
```

**Creating Typed Nodes**:

```cpp
// Single input
auto [B, tB] = builder.create_typed_node<double>(
  "B",
  {{"A", "x"}},  // Input: from A's "x" output
  [](const std::tuple<double>& in) {
    return std::make_tuple(std::get<0>(in) + 1.0);
  },
  {"b"}  // Output key
);

// Multiple inputs
auto [D, tD] = builder.create_typed_node<double, double>(
  "D",
  {{"B", "b"}, {"C", "c"}},  // Multiple inputs via key specs
  [](const std::tuple<double, double>& in) {
    return std::make_tuple(std::get<0>(in) * std::get<1>(in));
  },
  {"prod"}  // Output key
);

// Multiple outputs (types inferred from functor return)
auto [G, tG] = builder.create_typed_node<double, double, int>(
  "G",
  {{"C", "c"}, {"B", "b"}, {"E", "ek"}},
  [](const std::tuple<double, double, int>& in) {
    double sum = std::get<0>(in) + std::get<1>(in);
    int parity = (std::get<2>(in) % 2 + 2) % 2;
    return std::make_tuple(sum, parity);  // Return type inferred
  },
  {"sum", "parity"}  // Output keys
);
```

**Template Parameters**:
- `<Ins...>` - Input types (must be explicitly specified)
- Output types - **Auto-inferred** from functor return type

**Creating Any Nodes**:

```cpp
auto [B, tB] = builder.create_any_node("B",
  {{"A", "x"}},  // Input specs
  [](const std::unordered_map<std::string, std::any>& in) {
    double x = std::any_cast<double>(in.at("x"));
    return std::unordered_map<std::string, std::any>{{"b", x + 1.0}};
  },
  {"b"}  // Output keys
);
```

**Creating Sinks**:

```cpp
auto [H, tH] = builder.create_any_sink("H",
  {{"D", "prod"}, {"G", "sum"}, {"G", "parity"}}  // Input specs
);
```

**Automatic Dependency Inference**:
- Dependencies are **automatically established** from input specifications
- For each input spec `{"source_node", "source_key"}`:
  - If adapter exists: `adapter_task → target_node`
  - Otherwise: `source_node → target_node`
- No manual dependency configuration needed!

**Adapter Tasks**:
- When connecting Typed nodes via key-based inputs, adapter tasks are automatically created
- Adapters convert `std::any` futures to typed futures
- Adapter tasks are linked: `source → adapter → target`
- Adapter names follow pattern: `"<source>_to_<key>_adapter"`

## 📚 Complete Examples

### Example 1: Declarative API (Recommended)

See `examples/declarative_example.cpp` for full code:

```cpp
#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>

int main() {
  namespace wf = workflow;
  tf::Executor executor;
  wf::GraphBuilder builder("declarative_workflow");

  // Create source
  auto [A, tA] = builder.create_typed_source("A",
    std::make_tuple(3.5, 7), {"x", "k"}
  );

  // Create nodes with automatic dependency inference
  auto [B, tB] = builder.create_typed_node<double>("B",
    {{"A", "x"}},
    [](const std::tuple<double>& in) {
      return std::make_tuple(std::get<0>(in) + 1.0);
    },
    {"b"}
  );

  auto [C, tC] = builder.create_typed_node<double>("C",
    {{"A", "x"}},
    [](const std::tuple<double>& in) {
      return std::make_tuple(2.0 * std::get<0>(in));
    },
    {"c"}
  );

  auto [D, tD] = builder.create_typed_node<double, double>("D",
    {{"B", "b"}, {"C", "c"}},
    [](const std::tuple<double, double>& in) {
      return std::make_tuple(std::get<0>(in) * std::get<1>(in));
    },
    {"prod"}
  );

  // Create sink
  auto [H, tH] = builder.create_any_sink("H",
    {{"D", "prod"}}
  );

  // No manual dependencies! Auto-inferred from input specs:
  // - B depends on A (via {"A", "x"})
  // - C depends on A (via {"A", "x"})
  // - D depends on B, C (via {{"B", "b"}, {"C", "c"}})
  // - H depends on D (via {"D", "prod"})

  builder.run(executor);
  builder.dump(std::cout);
  return 0;
}
```

Tip: default internal prints were removed; use sink callbacks to log final values.

### Example 2: Traditional API (Key-based)

See `examples/unified_example.cpp` for key-based access:

```cpp
// Create source with keys
auto A = std::make_shared<wf::TypedSource<double, int>>(
  std::make_tuple(3.5, 7), {"x", "k"}, "A"
);
auto tA = builder.add_typed_source(A);

// Access via key
auto x_fut = A->out.get_typed<0>("x");

// Create node
auto B = std::make_shared<wf::TypedNode<B_Inputs, double>>(
  std::make_tuple(x_fut),
  [](const std::tuple<double>& in) { ... },
  {"b"}, "B"
);

// Manual dependencies (deprecated but still works)
builder.precede(tA, std::vector<tf::Task>{tB});
```

### Example 3: Pure Any-based Workflow

See `examples/keyed_example.cpp`:

```cpp
auto A = std::make_shared<wf::AnySource>(
  std::unordered_map<std::string, std::any>{
    {"x", std::any{3.5}},
    {"k", std::any{7}}
  },
  "A"
);

auto B = std::make_shared<wf::AnyNode>(
  {{"x", A->out.futures.at("x")}},
  {"b"},
  [](const auto& in) {
    double x = std::any_cast<double>(in.at("x"));
    return std::unordered_map<std::string, std::any>{{"b", x + 1.0}};
  },
  "B"
);
```

## 🔧 API Reference

### GraphBuilder: Declarative API (Recommended)

#### Node Creation

```cpp
// Typed Source
template <typename... Outs>
std::pair<std::shared_ptr<TypedSource<Outs...>>, tf::Task>
create_typed_source(const std::string& name,
                     std::tuple<Outs...> values,
                     const std::vector<std::string>& output_keys);

// Typed Node
template <typename... Ins, typename OpType>
auto create_typed_node(const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& input_specs,
                      OpType&& functor,
                      const std::vector<std::string>& output_keys);
// Input types: Ins... (explicit)
// Output types: auto-inferred from functor return type

// Any Source
std::pair<std::shared_ptr<AnySource>, tf::Task>
create_any_source(const std::string& name,
                  std::unordered_map<std::string, std::any> values);

// Any Node
std::pair<std::shared_ptr<AnyNode>, tf::Task>
create_any_node(const std::string& name,
                const std::vector<std::pair<std::string, std::string>>& input_specs,
                std::function<std::unordered_map<std::string, std::any>(
                    const std::unordered_map<std::string, std::any>&)> functor,
                const std::vector<std::string>& output_keys);

// Any Sink
std::pair<std::shared_ptr<AnySink>, tf::Task>
create_any_sink(const std::string& name,
                const std::vector<std::pair<std::string, std::string>>& input_specs);

// Any Sink with callback
std::pair<std::shared_ptr<AnySink>, tf::Task>
create_any_sink(const std::string& name,
                const std::vector<std::pair<std::string, std::string>>& input_specs,
                std::function<void(const std::unordered_map<std::string, std::any>&)> callback);

// Typed Sink with and without callback
template <typename... Ins>
std::pair<std::shared_ptr<TypedSink<Ins...>>, tf::Task>
create_typed_sink(const std::string& name,
                  const std::vector<std::pair<std::string, std::string>>& input_specs);

template <typename... Ins>
std::pair<std::shared_ptr<TypedSink<Ins...>>, tf::Task>
create_typed_sink(const std::string& name,
                  const std::vector<std::pair<std::string, std::string>>& input_specs,
                  std::function<void(const std::tuple<Ins...>&)> callback);

// Build-and-run subgraph each execution (good for loop bodies)
tf::Task create_subtask(const std::string& name,
                        const std::function<void(GraphBuilder&)>& builder_fn);
```

#### Taskflow Algorithm Nodes

The Workflow library provides declarative wrappers for Taskflow's parallel algorithms, enabling seamless integration with the dataflow graph:

```cpp
// Parallel for_each - iterate over container elements
template <typename Container, typename C>
std::pair<std::shared_ptr<AnyNode>, tf::Task>
create_for_each(const std::string& name,
                const std::vector<std::pair<std::string, std::string>>& input_specs,
                C callable,
                const std::vector<std::string>& output_keys = {});
// Input: expects one input spec providing the container
// Callable: (element) -> void

// Parallel for_each_index - iterate over index range
template <typename B, typename E, typename S = int, typename C>
std::pair<std::shared_ptr<AnyNode>, tf::Task>
create_for_each_index(const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& input_specs,
                      C callable,
                      const std::vector<std::string>& output_keys = {});
// Input: expects "first", "last", "step" (step optional, defaults to 1)
// Callable: (index) -> void

// Parallel reduce - reduce container to single value
template <typename T, typename Container, typename BinaryOp>
std::pair<std::shared_ptr<AnyNode>, tf::Task>
create_reduce(const std::string& name,
              const std::vector<std::pair<std::string, std::string>>& input_specs,
              T& init,
              BinaryOp bop,
              const std::vector<std::string>& output_keys = {"result"});
// Input: expects one input spec providing the container
// BinaryOp: (T, element_type) -> T
// Result: stored in init (captured by reference) and exposed via output key

// Parallel transform - transform container elements
template <typename InputContainer, typename OutputContainer, typename UnaryOp>
std::pair<std::shared_ptr<AnyNode>, tf::Task>
create_transform(const std::string& name,
                const std::vector<std::pair<std::string, std::string>>& input_specs,
                UnaryOp unary_op,
                const std::vector<std::string>& output_keys = {"result"});
// Input: expects one input spec providing the input container
// UnaryOp: (input_element) -> output_element
// Output: new container with transformed elements
```

**Key Features**:
- ✅ String-keyed inputs/outputs (consistent with declarative API)
- ✅ Automatic dependency inference
- ✅ Parallel execution via Taskflow's efficient scheduler
- ✅ Seamless integration with existing workflow nodes

#### Input/Output Access

```cpp
// Get output future by key (type-erased, works for all node types)
std::shared_future<std::any> get_output(const std::string& node_name, 
                                        const std::string& key) const;

// Get typed input (for advanced use cases)
template <typename T>
std::shared_future<T> get_input(const std::string& node_name, 
                                const std::string& key) const;
```

#### Execution

```cpp
void run(tf::Executor& executor);  // Synchronous
tf::Future<void> run_async(tf::Executor& executor);  // Asynchronous
void dump(std::ostream& os = std::cout) const;  // DOT visualization
```

### GraphBuilder: Traditional API (Deprecated)

These methods are deprecated but kept for backward compatibility:

```cpp
// Deprecated: Use declarative API instead
[[deprecated]]
tf::Task add_typed_source(std::shared_ptr<TypedSource<Outs...>> node);
[[deprecated]]
tf::Task add_typed_node(std::shared_ptr<TypedNode<...>> node);
[[deprecated]]
void precede(tf::Task from, tf::Task to);
[[deprecated]]
void succeed(tf::Task to, tf::Task from);
```

### Node Interface (INode)

All nodes support:

```cpp
std::string name() const;  // Get node name
std::string type() const;  // Get node type ("TypedSource", "AnyNode", etc.)
std::function<void()> functor(const char* node_name) const;  // Create task functor
std::shared_future<std::any> get_output_future(const std::string& key) const;  // Get output by key
std::vector<std::string> get_output_keys() const;  // Get all output keys
```

### TypedOutputs Interface

For typed nodes, access outputs via:

```cpp
// Type-safe access (requires knowing output index)
template <std::size_t I>
std::shared_future<std::tuple_element_t<I, std::tuple<Outs...>>> 
get_typed(const std::string& key) const;

// Type-safe access via type (runtime lookup)
template <typename T>
std::shared_future<T> get_typed_by_key(const std::string& key) const;

// Type-erased access
std::shared_future<std::any> get(const std::string& key) const;

// List all keys
const std::vector<std::string>& keys() const;
```

## 🎨 Usage Patterns

### Pattern 1: Pure Declarative Workflow (Recommended)

**Best for**: New code, maximum simplicity

```cpp
wf::GraphBuilder builder("workflow");
auto [A, _] = builder.create_typed_source("A", std::make_tuple(3.5), {"x"});
auto [B, _] = builder.create_typed_node<double>("B", {{"A", "x"}},
  [](auto in) { return std::make_tuple(std::get<0>(in) + 1.0); }, {"b"});
builder.run(executor);  // Dependencies auto-inferred!
```

**Advantages**:
- ✅ Minimal boilerplate
- ✅ Dependencies auto-inferred
- ✅ Key-based, readable
- ✅ Type-safe with inference

### Pattern 2: Typed Workflow (Performance-critical)

**Best for**: Performance-critical paths with known types

```cpp
wf::GraphBuilder builder("typed_workflow");
auto A = std::make_shared<wf::TypedSource<double>>(std::make_tuple(3.5), {"x"}, "A");
auto B = std::make_shared<wf::TypedNode</*...*/>>(/*...*/, {"b"}, "B");
auto tA = builder.add_typed_source(A);
auto tB = builder.add_typed_node(B);
builder.precede(tA, std::vector<tf::Task>{tB});  // Deprecated but works
```

**Advantages**:
- ✅ Zero runtime type overhead
- ✅ Full compiler optimization
- ✅ Compile-time type checking

### Pattern 3: Any-based Workflow (Dynamic)

**Best for**: Dynamic types, runtime flexibility

```cpp
wf::GraphBuilder builder("any_workflow");
auto [A, _] = builder.create_any_source("A", {{"x", std::any{3.5}}});
auto [B, _] = builder.create_any_node("B", {{"A", "x"}},
  [](const auto& in) {
    double x = std::any_cast<double>(in.at("x"));
    return std::unordered_map<std::string, std::any>{{"b", x + 1.0}};
  },
  {"b"});
```

**Advantages**:
- ✅ Dynamic type handling
- ✅ Easy mixing of types
- ✅ Runtime flexibility

### Pattern 4: Mixed Workflow

**Best for**: Typed computation + Any-based interface

```cpp
// Typed computation (performance)
auto [D, _] = builder.create_typed_node<double, double>("D",
  {{"B", "b"}, {"C", "c"}}, /*...*/, {"prod"});

// Any-based interface (flexibility)
auto [H, _] = builder.create_any_sink("H", {{"D", "prod"}});
```

**Bridge**: Adapter tasks automatically created when needed

## 🏗️ Building

### As Subdirectory (Recommended)

From the taskflow root:

```bash
mkdir build && cd build
cmake .. -DTF_BUILD_WORKFLOW=ON
cmake --build . --target declarative_example
./workflow/declarative_example
```

### Standalone

From the workflow directory:

```bash
mkdir build && cd build
cmake ..
cmake --build . --target declarative_example
./declarative_example
```

## 🔬 Technical Details

### Data Passing Mechanism

**Typed Nodes**:
- Uses `std::shared_ptr<std::promise<T>>` + `std::shared_future<T>`
- `std::shared_ptr` makes promises copyable (required by Taskflow)
- `std::shared_future` allows multiple consumers

**Any Nodes**:
- Uses `std::shared_ptr<std::promise<std::any>>` + `std::shared_future<std::any>`
- Type erasure via `std::any`
- Runtime type conversion via `std::any_cast<T>`

**Adapter Tasks** (Typed → Any conversion):
- Created automatically when connecting Typed outputs to Any inputs
- Extracts typed value and wraps in `std::any`
- Named: `"<source>_to_<key>_adapter"`

### Dependency Management

**Automatic Inference** (Declarative API):
- For each input spec `{"source_node", "source_key"}`:
  1. Check if adapter exists: `adapter_tasks_["source::key"]`
  2. If adapter exists: `adapter → target`
  3. Otherwise: `source → target`

**Manual Configuration** (Deprecated):
- Use `precede()` / `succeed()` methods (marked deprecated)

### Type System

**Compile-time Types** (Typed Nodes):
- Input: `tuple<shared_future<Ins>...>`
- Output: `tuple<shared_future<Outs>...>`
- Operation: `tuple<Ins...> → tuple<Outs...>`

**Runtime Types** (Any Nodes):
- Input: `map<string, shared_future<any>>`
- Output: `map<string, shared_future<any>>`
- Operation: `map<string, any> → map<string, any>`

**Type Extraction**:
- From `shared_future<T>` extract `T` via `FutureValueType`
- From `tuple<shared_future<Ins>...>` extract `tuple<Ins...>` via `ExtractValueTypesHelper`

## 🎯 Design Decisions

### Why Key-based I/O?

- **Readability**: `{{"A", "x"}}` vs `std::get<0>(A->out.futures)`
- **Flexibility**: Easy to add/remove outputs without breaking code
- **Unified Interface**: Same API for Typed and Any nodes
- **Self-documenting**: Keys describe data semantics

### Why Declarative API?

- **Simplicity**: Less boilerplate code
- **Safety**: Dependencies automatically inferred (no manual errors)
- **Maintainability**: Changes to graph structure easier
- **Readability**: Input specs clearly show data flow

### Why Both Typed and Any Nodes?

- **Typed Nodes**: Best for performance-critical paths
- **Any Nodes**: Essential for dynamic workflows
- **Interoperability**: Adapter tasks bridge seamlessly

### Why Pure Virtual Base Class?

- Enables polymorphic node management
- Simplifies graph inspection and debugging
- Supports generic algorithms over node collections
- Maintains type information while allowing runtime dispatch

## 🚀 Performance Considerations

1. **Typed Nodes**: Zero runtime type overhead, full compiler optimization
2. **Any Nodes**: Minimal overhead from `std::any` type erasure (~1-2ns per access)
3. **Adapter Tasks**: One extra task per Typed→Any conversion
4. **Taskflow Integration**: Efficient work-stealing scheduler
5. **Data Passing**: Lock-free via `std::promise`/`std::future`

## 📖 Examples Reference

- **`declarative_example.cpp`**: 🎯 **Recommended** - Declarative API with auto-deps
- **`unified_example.cpp`**: Key-based API demonstration
- **`keyed_example.cpp`**: Pure Any-based workflow
- **`advanced_control_flow.cpp`**: Advanced control flow (condition, multi-condition, pipeline, loop)
- **`algorithm_example.cpp`**: Taskflow algorithm nodes (for_each, reduce, transform)
- **`loop_only.cpp`**: Loop control flow demonstration

## 🎮 Advanced Control Flow Nodes

The Workflow library provides powerful control flow constructs built on Taskflow's primitives:

### Condition Node (If-Else Branching)

Creates conditional execution paths based on a boolean function:

```cpp
// Create branches as subgraphs
auto C_task = builder.create_subgraph("C", [&](wf::GraphBuilder& gb){
  // Branch logic for condition true
});

auto D_task = builder.create_subgraph("D", [&](wf::GraphBuilder& gb){
  // Branch logic for condition false
});

// Create condition: returns 0 for C (true), 1 for D (false)
builder.create_condition_decl("B",
  {"A"},  // Depend on node A first
  [](int x) { return (x % 2 == 0) ? 0 : 1; },
  {C_task, D_task}  // Successors
);
```

### Multi-Condition Node (Parallel Branches)

Executes multiple branches based on a vector return value:

```cpp
builder.create_multi_condition_decl("F",
  {"E"},
  []() -> tf::SmallVector<int> {
    return {0, 2};  // Execute branches 0 and 2 in parallel
  },
  {G_task, H_task, I_task}  // Multiple successors
);
```

### Pipeline Node

Creates a structured pipeline with multiple stages and parallel lines:

```cpp
builder.create_pipeline_node("Pipeline",
  std::make_tuple(
    tf::Pipe{tf::PipeType::SERIAL, [](tf::Pipeflow& pf) { /* stage 1 */ }},
    tf::Pipe{tf::PipeType::PARALLEL, [](tf::Pipeflow& pf) { /* stage 2 */ }},
    tf::Pipe{tf::PipeType::SERIAL, [](tf::Pipeflow& pf) { /* stage 3 */ }}
  ),
  4  // 4 parallel lines
);
```

### Loop Node (Iterative Control Flow)

Creates iterative loops with condition-based exit:

```cpp
int counter = 0;

// Build loop body using create_subtask (rebuilds subgraph each iteration)
auto loop_body_task = builder.create_subtask("LoopBody", [&counter](wf::GraphBuilder& gb){
  auto [trigger, _] = gb.create_typed_source("loop_trigger",
    std::make_tuple(counter), {"trigger"}
  );
  auto [process, _] = gb.create_typed_node<int>("loop_iteration",
    {{"loop_trigger", "trigger"}},
    [&counter](const std::tuple<int>&) {
      ++counter;
      return std::make_tuple(counter);
    },
    {"result"}
  );
  auto [sink, _] = gb.create_any_sink("loop_complete",
    {{"loop_iteration", "result"}},
    [](const std::unordered_map<std::string, std::any>& values){
      if (auto it = values.find("result"); it != values.end()) {
        std::cout << "  Loop iteration completed: counter = "
                  << std::any_cast<int>(it->second) << "\n";
      }
    }
  );
});

// Optional exit action subgraph
auto loop_exit_task = builder.create_subgraph("LoopExit", [](wf::GraphBuilder& gb){
  // Exit logic
});

// Create loop: condition returns 0 to continue, non-zero to exit
// Note: Returns tf::Task (condition task) in master mode
// input_specs are used for condition_func inputs but dependencies are NOT auto-set
auto cond_task = builder.create_loop_decl(
  "Loop",
  {{"A", "value"}},  // input_specs: inputs available to condition_func (not auto-dependencies)
  loop_body_task,
  [&counter](const std::unordered_map<std::string, std::any>& inputs) -> int {
    // inputs from input_specs are available here
    (void)inputs;  // Can use inputs if needed
    return (counter < 5) ? 0 : 1;  // Continue if counter < 5
  },
  loop_exit_task
);

// Dependencies are automatically set by create_loop_decl:
// - All unique source nodes from input_specs -> body_task (initial trigger)
// - body_task -> cond_task -> body_task (when cond returns 0, continue loop)
// - body_task -> cond_task -> exit_task (when cond returns non-zero, exit loop)
// If input_specs is empty, an empty start task is created to trigger the loop
```

**Key Features**:
- ✅ Loop body uses declarative API for clean structure
- ✅ Parameter passing via lambda capture
- ✅ Condition function receives inputs from `input_specs`
- ✅ Subgraphs support nested declarative workflows
- ✅ For iterative execution, prefer `create_subtask` over `create_subgraph`
- ✅ **Automatic dependency setup**: source nodes from `input_specs` are automatically connected to `body_task`
- ✅ If `input_specs` is empty, an empty start task is created to trigger the loop
- ✅ `input_specs` provide inputs to `condition_func` and auto-connect initial dependencies

## 🔢 Taskflow Algorithm Nodes

The Workflow library provides declarative wrappers for Taskflow's parallel algorithms, enabling you to leverage Taskflow's efficient parallel execution within your dataflow graphs.

### Parallel Iteration: `create_for_each`

Iterate over container elements in parallel with support for shared parameters:

```cpp
std::vector<int> numbers = {1, 2, 3, 4, 5};

// Create source with container
auto [input, _] = builder.create_any_source("Input",
  {{"data", std::any{numbers}}}
);

// Optional: Create shared parameter source
auto [shared_params, _] = builder.create_any_source("SharedParams",
  {{"multiplier", std::any{2}}}
);

// Parallel for_each: apply function to each element
// Function signature: void(ElementType, shared_params&)
auto [for_each_node, for_each_task] = builder.create_for_each<std::vector<int>>(
  "PrintElements",
  {{"Input", "data"}, {"SharedParams", "multiplier"}},  // Container + shared params
  std::function<void(int, std::unordered_map<std::string, std::any>&)>(
    [](int value, std::unordered_map<std::string, std::any>& shared_params) {
      int multiplier = std::any_cast<int>(shared_params.at("multiplier"));
      std::cout << "Processing: " << value << " * " << multiplier 
                << " = " << (value * multiplier) << "\n";
      // shared_params is modifiable and shared across all iterations
    }
  ),
  {}  // No outputs
);
```

**API Signature**:
```cpp
template <typename Container>
std::pair<std::shared_ptr<AnyNode>, tf::Task>
create_for_each(const std::string& name,
                const std::vector<std::pair<std::string, std::string>>& input_specs,
                std::function<void(typename Container::value_type, 
                                  std::unordered_map<std::string, std::any>&)> callable,
                const std::vector<std::string>& output_keys = {});
```

**Parameters**:
- `input_specs`: First element is the container, rest are shared parameters (optional)
- `callable`: Function receiving `(element, shared_params)` where `shared_params` is modifiable
- `output_keys`: Optional output keys for chaining

**Use Cases**:
- Processing each element independently
- Printing/logging container elements
- Side-effect operations on elements
- Custom processing with shared state modification

### Parallel Iteration by Index: `create_for_each_index`

Iterate over index ranges in parallel with index range passed as function parameters and **return value collection**:

```cpp
// Optional: Create shared parameter source
auto [shared_params, _] = builder.create_any_source("SharedParams",
  {{"multiplier", std::any{2}}}
);

// Parallel for_each_index: iterate over indices and collect return values
// Index range is passed as function arguments, not from input_specs
auto [index_node, index_task] = builder.create_for_each_index<int, int>(
  "ProcessIndices",
  {{"SharedParams", "multiplier"}},  // Optional shared parameters
  0,   // first: beginning index (inclusive)
  20,  // last: ending index (exclusive)
  2,   // step: step size
  std::function<int(int, std::unordered_map<std::string, std::any>&)>(
    [](int index, std::unordered_map<std::string, std::any>& shared_params) -> int {
      int multiplier = std::any_cast<int>(shared_params.at("multiplier"));
      int result = index * multiplier;
      std::cout << "Index: " << index << ", multiplied: " << result << "\n";
      return result;  // Return value collected into vector
    }
  ),
  {"results"}  // Output key: stores std::vector<int> of collected results
);

// Access collected results from subsequent nodes
auto [display_node, display_task] = builder.create_any_sink("DisplayResults",
  {{"ProcessIndices", "results"}},
  [](const std::unordered_map<std::string, std::any>& inputs) {
    const std::vector<int>& results = std::any_cast<const std::vector<int>&>(inputs.at("results"));
    std::cout << "Collected " << results.size() << " results\n";
  }
);
```

**API Signature**:
```cpp
template <typename IndexType, typename ReturnType = std::any>
std::pair<std::shared_ptr<AnyNode>, tf::Task>
create_for_each_index(const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& input_specs,
                      IndexType first,      // Beginning index (inclusive)
                      IndexType last,       // Ending index (exclusive)
                      IndexType step,      // Step size
                      std::function<ReturnType(IndexType, 
                                        std::unordered_map<std::string, std::any>&)> callable,
                      const std::vector<std::string>& output_keys = {"result"});
```

**Parameters**:
- `input_specs`: Optional shared parameters (index range is NOT from input_specs)
- `first`, `last`, `step`: Index range parameters passed directly
- `callable`: Function receiving `(index, shared_params)` and **returning `ReturnType`**
  - Return values are collected into `std::vector<ReturnType>` and stored in the first `output_key`
  - Supports both `std::any` (default) and typed return values (e.g., `int`, `double`)
- `output_keys`: Output keys - first key (default: "result") stores the collected results vector

**Key Features**:
- ✅ Return value collection: all return values collected into `std::vector<ReturnType>`
- ✅ Thread-safe parallel collection using mutex protection
- ✅ Supports both `std::any` and typed return values
- ✅ Shared parameters modifiable across iterations

**Use Cases**:
- Numeric range processing with result collection
- Array index-based operations with return values
- Generating sequences in parallel and collecting results
- Custom processing with shared state modification and return value aggregation
- Parallel computation where each index produces a value to be collected

### Parallel Reduction: `create_reduce`

Reduce a container to a single value using a binary operator with support for shared parameters:

```cpp
std::vector<int> numbers = {1, 2, 3, 4, 5};

// Create source
auto [input, _] = builder.create_any_source("Input",
  {{"data", std::any{numbers}}}
);

// Optional: Create shared parameter source
auto [shared_params, _] = builder.create_any_source("SharedParams",
  {{"weight", std::any{2}}}
);

// Parallel reduce: compute weighted sum
// Function signature: T(T, ElementType, shared_params&)
int sum_result = 0;
auto [reduce_node, reduce_task] = builder.create_reduce<int, std::vector<int>>(
  "SumElements",
  {{"Input", "data"}, {"SharedParams", "weight"}},  // Container + shared params
  sum_result,  // Initial value (captured by reference - must remain alive)
  std::function<int(int, int, std::unordered_map<std::string, std::any>&)>(
    [](int acc, int val, std::unordered_map<std::string, std::any>& shared_params) -> int {
      int weight = std::any_cast<int>(shared_params.at("weight"));
      return acc + (val * weight);  // Weighted reduction
    }
  ),
  {"sum"}  // Output key
);

// Access result via output key or sum_result variable
```

**API Signature**:
```cpp
template <typename T, typename Container>
std::pair<std::shared_ptr<AnyNode>, tf::Task>
create_reduce(const std::string& name,
              const std::vector<std::pair<std::string, std::string>>& input_specs,
              T& init,  // Initial value (by reference - must remain alive)
              std::function<T(T, typename Container::value_type,
                             std::unordered_map<std::string, std::any>&)> bop,
              const std::vector<std::string>& output_keys = {"result"});
```

**Parameters**:
- `input_specs`: First element is the container, rest are shared parameters (optional)
- `init`: Initial value captured by reference - must remain alive during execution
- `bop`: Binary operator receiving `(accumulator, element, shared_params)` where `shared_params` is modifiable
- `output_keys`: Output key for the reduced result (default: "result")

**Important Notes**:
- `init` is captured **by reference** - must remain alive during execution
- Result is stored in `init` AND exposed via output key
- Binary operator must be associative and commutative for correctness
- Shared parameters are extracted once and passed to each reduction operation

**Use Cases**:
- Sum, product, min, max operations
- Aggregating container values
- Statistical computations
- Custom reduction with shared state

### Parallel Transformation: `create_transform`

Transform container elements producing a new container:

```cpp
std::vector<int> input = {1, 2, 3, 4, 5};

// Create source
auto [input_node, _] = builder.create_any_source("Input",
  {{"data", std::any{input}}}
);

// Parallel transform: square each element
// Function signature: OutputElement(InputElement)
auto [transform_node, transform_task] = builder.create_transform<
  std::vector<int>,      // Input container type
  std::vector<int>       // Output container type
>(
  "SquareElements",
  {{"Input", "data"}},
  std::function<int(int)>([](int x) -> int {
    return x * x;  // Unary operation
  }),
  {"squared"}  // Output key
);

// Use transformed container in subsequent nodes
auto [sink, _] = builder.create_any_sink("Sink",
  {{"SquareElements", "squared"}}
);
```

**API Signature**:
```cpp
template <typename InputContainer, typename OutputContainer>
std::pair<std::shared_ptr<AnyNode>, tf::Task>
create_transform(const std::string& name,
                const std::vector<std::pair<std::string, std::string>>& input_specs,
                std::function<typename OutputContainer::value_type(
                  typename InputContainer::value_type)> unary_op,
                const std::vector<std::string>& output_keys = {"result"});
```

**Parameters**:
- `input_specs`: Input specification for the container (exactly one input)
- `unary_op`: Unary operation receiving `InputElement` and returning `OutputElement`
- `output_keys`: Output key for the transformed container (default: "result")

**Use Cases**:
- Element-wise mathematical operations
- Data format conversions
- Filtering/mapping operations

### Complete Algorithm Workflow Example

See `examples/algorithm_example.cpp` for a complete example combining multiple algorithms:

```cpp
#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>

int main() {
  namespace wf = workflow;
  tf::Executor executor;
  wf::GraphBuilder builder("algorithm_workflow");

  // 1. Create input data
  std::vector<int> numbers = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  auto [input, _] = builder.create_any_source("Input",
    {{"data", std::any{numbers}}}
  );

  // 2. Parallel for_each: print elements
  auto [for_each, _] = builder.create_for_each<std::vector<int>>(
    "PrintElements",
    {{"Input", "data"}},
    std::function<void(int, std::unordered_map<std::string, std::any>&)>(
      [](int val, std::unordered_map<std::string, std::any>& shared_params) {
        (void)shared_params;  // Suppress unused parameter warning
        std::cout << "Element: " << val << "\n";
      }
    ),
    {}
  );

  // 3. Parallel transform: square elements
  auto [transform, _] = builder.create_transform<
    std::vector<int>, std::vector<int>
  >(
    "SquareElements",
    {{"Input", "data"}},
    std::function<int(int)>([](int x) -> int {
      return x * x;
    }),
    {"squared"}
  );

  // 4. Parallel reduce: sum squared elements
  int sum = 0;
  auto [reduce, _] = builder.create_reduce<int, std::vector<int>>(
    "SumSquares",
    {{"SquareElements", "squared"}},
    sum,
    std::function<int(int, int, std::unordered_map<std::string, std::any>&)>(
      [](int acc, int val, std::unordered_map<std::string, std::any>& shared_params) -> int {
        (void)shared_params;  // Suppress unused parameter warning
        return acc + val;
      }
    ),
    {"sum"}
  );

  // 5. Collect result
  auto [sink, _] = builder.create_any_sink("FinalSink",
    {{"SumSquares", "sum"}},
    [](const auto& values) {
      int result = std::any_cast<int>(values.at("sum"));
      std::cout << "Sum of squares: " << result << "\n";
    }
  );

  // All dependencies auto-inferred!
  builder.run(executor);
  return 0;
}
```

### Algorithm Node Integration Benefits

1. **Consistent API**: Same declarative pattern as regular nodes
2. **Automatic Dependencies**: Input specs automatically establish dependencies
3. **Parallel Execution**: Leverages Taskflow's efficient work-stealing scheduler
4. **Key-based I/O**: Outputs accessible via string keys for chaining
5. **Type Safety**: Template-based type checking for containers and operations

### Performance Considerations

- **Parallel Execution**: Algorithms utilize Taskflow's parallel scheduler for multi-core performance
- **Work-stealing**: Efficient load balancing across worker threads
- **Zero-copy**: Container references passed efficiently (no unnecessary copies)
- **Overhead**: Minimal wrapper overhead; execution is dominated by algorithm implementation

### Implementation Notes

Algorithm nodes are implemented as `AnyNode` wrappers that:
1. Extract container/data from input futures at execution time
2. Create temporary `tf::Taskflow` instances for algorithm execution
3. Use `executor_->run(taskflow).wait()` to execute algorithms synchronously
4. Expose results via string-keyed outputs for seamless chaining

This design allows algorithm nodes to:
- Work with both typed and any-based nodes via unified interface
- Leverage Taskflow's efficient parallel algorithms
- Maintain consistent declarative API patterns

## 🧪 Test Graphs (DOT Snapshots)

Below are DOT graph snippets from the verified examples (generated via `builder.dump`).

### loop_only

```
digraph Taskflow {
subgraph cluster_... {
label="Taskflow: loop_only";
Input -> LoopBody;
LoopBody -> Loop;
Loop [shape=diamond];
Loop -> LoopBody [style=dashed label="0"];  // continue
Loop -> LoopExit [style=dashed label="1"]; // exit
}
}
```

Behavior: Loop body executes 5 times (counter 1..5) then exits; logging is handled by the `loop_complete` sink callback.

### advanced_control_flow

```
digraph Taskflow {
subgraph cluster_a {
label="Taskflow: advanced_control_flow";
A -> B; A -> LoopBody;
E -> F;
B [shape=diamond] -> C [m2] | D [m6];
F [shape=diamond] -> G [m3] | H [m4] | I [m5];
Loop [shape=diamond] -> LoopBody (0) | LoopExit (1);
}
}
```

Behavior: condition selects branch C; multi-condition selects G and I; pipeline runs 3 stages over 4 lines; loop iterates 5 times; sink callbacks print concise results for each segment.

## 🛠️ Technical Notes (Updates)

- Module task vs subtask for loops:
  - `create_subgraph` creates a module task (single-run semantics). Not suitable as loop body since the same module cannot be dispatched repeatedly.
  - `create_subtask` creates a normal task whose callable builds and runs a fresh subgraph on each execution using the enclosing executor; this supports iterative loop semantics.
- Sink callbacks:
  - Any-based sink: `std::function<void(const std::unordered_map<std::string, std::any>&)>`
  - Typed sink: `std::function<void(const std::tuple<Ins...>&)>`
  - If no callback is provided, a simple default print is used.
- Noise reduction: removed internal per-node "emitted"/"done" prints to keep outputs focused on user intent (callbacks or explicit `std::cout`).

### Subgraph Creation

Create reusable workflow modules:

```cpp
auto module_task = builder.create_subgraph("ModuleName", [](wf::GraphBuilder& gb){
  // Use gb to build the subgraph with declarative API
  auto [A, _] = gb.create_typed_source("A", std::make_tuple(1.0), {"x"});
  auto [B, _] = gb.create_typed_node<double>("B", {{"A", "x"}}, /*...*/, {"y"});
  // Dependencies automatically inferred within subgraph
});

// Use module_task in main graph or as loop body
```

## 🔮 Future Enhancements

- **Algorithm Nodes**:
  - Additional algorithm wrappers: `sort`, `find`, `scan`, etc.
  - Support for custom partitioners in algorithm nodes
  - Nested algorithm execution within subgraphs
- **Type System**:
  - Type validation and schema checking for Any-based nodes
  - Input type auto-inference from source node types
- **Tooling**:
  - Visual graph representation with type/key annotations
  - Performance profiling and monitoring
- **Features**:
  - Support for optional inputs/outputs
  - Error handling and recovery mechanisms
  - Conditional execution based on data values

## 📄 License

See main Taskflow repository for license information.

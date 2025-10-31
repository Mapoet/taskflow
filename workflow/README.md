# Workflow: Unified Nodeflow Library for Taskflow

A high-level dataflow library built on Taskflow, supporting both **compile-time type-safe nodes** and **runtime type-erased nodes** through a unified interface.

## Overview

The Workflow library provides a powerful abstraction for building dataflow graphs with flexible type handling:

- **Pure Virtual Interface (`INode`)**: All nodes inherit from a common base class, enabling polymorphism
- **Typed Nodes**: Compile-time type-safe nodes using C++ templates (`TypedNode`, `TypedSource`, `TypedSink`)
- **Any-based Nodes**: Runtime type-erased nodes using `std::any` (`AnyNode`, `AnySource`, `AnySink`)
- **String-keyed I/O**: Flexible key-based data access via `std::unordered_map<std::string, std::any>`
- **Graph Builder**: High-level API for graph construction, dependency management, and execution
- **Taskflow Integration**: Built on Taskflow for efficient parallel execution

## Architecture

### Design Principles

1. **Separation of Concerns**: Header declarations in `include/`, implementations in `src/`
2. **Type Safety with Flexibility**: Support both typed (compile-time) and untyped (runtime) nodes
3. **Polymorphism**: Unified `INode` interface for all node types
4. **Easy Composition**: `GraphBuilder` simplifies graph construction and execution

### Component Overview

```
┌─────────────────────────────────────────────────────────┐
│                    INode (Base)                         │
│  - name(): string                                       │
│  - type(): string                                       │
│  - functor(): function<void()>                         │
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

## Directory Structure

```
workflow/
├── include/workflow/
│   ├── nodeflow.hpp          # Main header (declarations)
│   └── nodeflow_impl.hpp     # Template implementations
├── src/
│   └── nodeflow.cpp          # Implementation for non-template code
├── examples/
│   ├── keyed_example.cpp      # Any-based nodes example
│   └── unified_example.cpp    # Unified API with typed/any nodes
├── CMakeLists.txt
└── README.md
```

## Core Components

### 1. Pure Virtual Base Class: `INode`

All nodes inherit from `INode`, providing a unified interface:

```cpp
class INode {
 public:
  virtual std::string name() const = 0;
  virtual std::string type() const = 0;
  virtual std::function<void()> functor(const char* node_name) const = 0;
};
```

**Benefits**:
- Enables polymorphism: store nodes as `std::shared_ptr<INode>`
- Unified node management and inspection
- Easy iteration over all nodes regardless of type

### 2. Typed Nodes (Compile-time Type-safe)

Type-safe nodes for known input/output types:

#### `TypedSource<Outs...>`

Produces initial values with compile-time type checking:

```cpp
auto A = std::make_shared<wf::TypedSource<double, int>>(
  std::make_tuple(3.5, 7), "A"
);
auto [x_fut, k_fut] = A->out.futures;  // Type-safe access
```

#### `TypedNode<InputsTuple, Outs...>`

Transforms typed inputs to typed outputs:

```cpp
using B_Inputs = std::tuple<std::shared_future<double>>;
auto B = std::make_shared<wf::TypedNode<B_Inputs, double>>(
  std::make_tuple(x_fut),
  [](const std::tuple<double>& in) {
    double x = std::get<0>(in);
    return std::make_tuple(x + 1.0);
  },
  "B"
);
```

**Advantages**:
- Compile-time type checking
- No runtime type casting overhead
- IDE autocomplete support
- Clear type contracts

#### `TypedSink<Ins...>`

Consumes typed values:

```cpp
auto H = std::make_shared<wf::TypedSink<double, int>>(
  std::make_tuple(prod_fut, count_fut), "H"
);
```

### 3. Any-based Nodes (Runtime Type-erased)

Flexible nodes using `std::any` for heterogeneous types:

#### `AnySource`

```cpp
auto A = std::make_shared<wf::AnySource>(
  std::unordered_map<std::string, std::any>{
    {"x", std::any{3.5}},
    {"k", std::any{7}}
  },
  "A"
);
```

#### `AnyNode`

```cpp
auto D = std::make_shared<wf::AnyNode>(
  {{"b", B->out.futures.at("b")}, {"c", C->out.futures.at("c")}},
  {"prod"},
  [](const auto& in) {
    double b = std::any_cast<double>(in.at("b"));
    double c = std::any_cast<double>(in.at("c"));
    return std::unordered_map<std::string, std::any>{{"prod", b * c}};
  },
  "D"
);
```

#### `AnySink`

```cpp
auto H = std::make_shared<wf::AnySink>(
  {{"prod", f_prod}, {"sum", f_sum}, {"parity", f_parity}}, "H"
);
```

**Advantages**:
- Dynamic type handling
- Easy mixing of different types
- String-keyed access for clarity

### 4. Graph Builder

High-level API for building and executing workflows:

```cpp
wf::GraphBuilder builder("my_workflow");
tf::Executor executor;

// Add nodes
auto tA = builder.add_typed_source(A);
auto tB = builder.add_typed_node(B);
auto tH = builder.add_any_sink(H);

// Configure dependencies
builder.precede(tA, std::vector<tf::Task>{tB, tC});
builder.succeed(tH, std::vector<tf::Task>{tD, tG});

// Execute
builder.run(executor);
builder.dump(std::cout);  // Visualize graph
```

**Features**:
- Automatic node name management
- Duplicate name detection
- Support for both typed and any-based nodes
- Dependency configuration (single or multiple)
- Synchronous and asynchronous execution
- Graph visualization

## Complete Example

See `examples/unified_example.cpp` for a comprehensive demonstration:

```cpp
#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>

int main() {
  namespace wf = workflow;
  tf::Executor executor;
  wf::GraphBuilder builder("unified_workflow");

  // Typed source
  auto A = std::make_shared<wf::TypedSource<double, int>>(
    std::make_tuple(3.5, 7), "A"
  );
  auto tA = builder.add_typed_source(A);
  auto [x_fut, k_fut] = A->out.futures;

  // Typed nodes
  using B_Inputs = std::tuple<std::shared_future<double>>;
  auto B = std::make_shared<wf::TypedNode<B_Inputs, double>>(
    std::make_tuple(x_fut),
    [](const std::tuple<double>& in) {
      return std::make_tuple(std::get<0>(in) + 1.0);
    },
    "B"
  );
  auto tB = builder.add_typed_node(B);

  // Any-based sink
  auto H = std::make_shared<wf::AnySink>(/* ... */, "H");
  auto tH = builder.add_any_sink(H);

  // Dependencies
  builder.precede(tA, std::vector<tf::Task>{tB});
  builder.succeed(tH, std::vector<tf::Task>{tB});

  // Polymorphism demonstration
  std::vector<std::shared_ptr<wf::INode>> all_nodes = {A, B, H};
  for (auto& node : all_nodes) {
    std::cout << "Node: " << node->name() << ", Type: " << node->type() << '\n';
  }

  // Execute
  builder.run(executor);
  return 0;
}
```

## Usage Patterns

### Pattern 1: Pure Typed Workflow

Use typed nodes for maximum type safety:

```cpp
wf::GraphBuilder builder("typed_workflow");
auto A = std::make_shared<wf::TypedSource<double>>(std::make_tuple(3.5), "A");
auto B = std::make_shared<wf::TypedNode</* ... */>>(/* ... */, "B");
auto H = std::make_shared<wf::TypedSink<double>>(/* ... */, "H");
```

### Pattern 2: Pure Any-based Workflow

Use any-based nodes for maximum flexibility:

```cpp
wf::GraphBuilder builder("any_workflow");
auto A = std::make_shared<wf::AnySource>({{"x", std::any{3.5}}}, "A");
auto B = std::make_shared<wf::AnyNode>(/* ... */, "B");
auto H = std::make_shared<wf::AnySink>(/* ... */, "H");
```

### Pattern 3: Mixed Workflow

Combine typed and any-based nodes using adapter tasks:

```cpp
// Typed nodes for computation
auto D = std::make_shared<wf::TypedNode</* ... */>>(/* ... */);

// Bridge: typed -> any
auto p_prod = std::make_shared<std::promise<std::any>>();
auto adapter = builder.taskflow().emplace([p_prod, fut=/*typed future*/]() {
  p_prod->set_value(std::any{fut.get()});
});

// Any-based sink
auto H = std::make_shared<wf::AnySink>({{"prod", p_prod->get_future().share()}}, "H");
```

## Building

### As Subdirectory (Recommended)

From the taskflow root:

```bash
mkdir build && cd build
cmake .. -DTF_BUILD_WORKFLOW=ON
cmake --build . --target unified_example
./workflow/unified_example
```

### Standalone

From the workflow directory:

```bash
mkdir build && cd build
cmake ..
cmake --build . --target unified_example
./unified_example
```

## API Reference

### GraphBuilder Methods

- `add_node(std::shared_ptr<INode>)` - Add any node via base interface
- `add_typed_source/std::shared_ptr<TypedSource<Outs...>>)` - Add typed source
- `add_typed_node(std::shared_ptr<TypedNode<...>>)` - Add typed node
- `add_typed_sink(std::shared_ptr<TypedSink<Ins...>>)` - Add typed sink
- `add_any_source(std::shared_ptr<AnySource>)` - Add any-based source
- `add_any_node(std::shared_ptr<AnyNode>)` - Add any-based node
- `add_any_sink(std::shared_ptr<AnySink>)` - Add any-based sink
- `precede(tf::Task from, tf::Task to)` - Set dependency: from → to
- `precede(tf::Task from, const Container& to)` - Set multiple dependencies
- `succeed(tf::Task to, tf::Task from)` - Set dependency: from → to (alternative)
- `succeed(tf::Task to, const Container& from)` - Set multiple dependencies
- `run(executor)` - Synchronous execution
- `run_async(executor)` - Asynchronous execution (returns `tf::Future<void>`)
- `dump(ostream)` - Output DOT graph visualization
- `get_node(name)` - Retrieve node by name
- `nodes()` - Get all nodes

### Node Interface

All nodes support:

- `name()` - Get node name
- `type()` - Get node type identifier
- `functor(node_name)` - Create Taskflow-compatible functor

## Design Decisions

### Why Both Typed and Any-based Nodes?

- **Typed Nodes**: Best for performance-critical paths where types are known at compile time
- **Any-based Nodes**: Essential for dynamic workflows, heterogeneous data, and runtime flexibility
- **Interoperability**: Adapter tasks bridge between typed and any-based nodes when needed

### Why Pure Virtual Base Class?

- Enables polymorphic node management
- Simplifies graph inspection and debugging
- Supports generic algorithms over node collections
- Maintains type information while allowing runtime dispatch

### Why GraphBuilder?

- Simplifies common workflow patterns
- Reduces boilerplate code
- Automatic name management
- Unified execution interface
- Easy graph visualization

## Performance Considerations

1. **Typed Nodes**: Zero runtime type overhead, full compiler optimization
2. **Any-based Nodes**: Minimal overhead from `std::any` type erasure
3. **Taskflow Integration**: Efficient work-stealing scheduler
4. **Data Passing**: `std::shared_ptr<promise>` + `std::shared_future` for thread-safe, copyable lambdas

## Future Enhancements

- Type validation and schema checking for Any-based nodes
- Automatic adapter generation between typed and any-based nodes
- Node composition and subgraph support
- Visual graph representation with type/key annotations
- Performance profiling and monitoring
- Support for optional inputs/outputs
- Pipeline and loop constructs
- Error handling and recovery mechanisms

## Examples

- `keyed_example.cpp`: Pure any-based workflow demonstration
- `unified_example.cpp`: Mixed typed/any workflow with polymorphism

## License

See main Taskflow repository for license information.

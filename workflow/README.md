# Workflow: String-Keyed Nodeflow Library

A Taskflow-based dataflow library with string-keyed heterogeneous inputs/outputs using `std::unordered_map<std::string, std::any>`.

## Features

- **String-keyed I/O**: All node inputs/outputs are accessed via string keys in unordered_map, enabling flexible key-based access and template-friendly node functions.
- **Heterogeneous types**: Uses `std::any` for type-erased values, supporting any C++ type.
- **Taskflow integration**: Built on top of Taskflow for efficient parallel execution.

## Directory Structure

```
workflow/
├── include/workflow/
│   └── nodeflow.hpp       # Main library header
├── src/                   # (Reserved for future .cpp implementations)
├── examples/
│   └── keyed_example.cpp # Example usage
└── CMakeLists.txt        # Build configuration
```

## Usage

### Basic Example

```cpp
#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>

namespace wf = workflow;

int main() {
  tf::Executor executor;
  tf::Taskflow tf("my_workflow");

  // Source: emit initial values
  wf::AnySource A({{"x", std::any{3.5}}, {"k", std::any{7}}});

  // Node: transform inputs to outputs
  wf::AnyNode B(
      {{"x", A.out.futures.at("x")}},  // inputs
      {"b"},                            // output keys
      [](const std::unordered_map<std::string, std::any>& in) {
        double x = std::any_cast<double>(in.at("x"));
        return std::unordered_map<std::string, std::any>{{"b", x + 1.0}};
      }
  );

  // Sink: consume final values
  wf::AnySink H({{"b", B.out.futures.at("b")}});

  // Create tasks and dependencies
  auto tA = tf.emplace(A.functor("A")).name("A");
  auto tB = tf.emplace(B.functor("B")).name("B");
  auto tH = tf.emplace(H.functor("H")).name("H");

  tA.precede(tB);
  tB.precede(tH);

  executor.run(tf).wait();
  return 0;
}
```

## Building

### As Subdirectory (Recommended)

From the taskflow root:

```bash
mkdir build && cd build
cmake .. -DTF_BUILD_EXAMPLES=ON
cmake --build . --target keyed_example
./workflow/examples/keyed_example
```

### Standalone

From the workflow directory:

```bash
mkdir build && cd build
cmake ..
cmake --build . --target keyed_example
./examples/keyed_example
```

## Key Concepts

### AnySource

Produces initial values with string keys:
```cpp
wf::AnySource src({{"key1", std::any{value1}}, {"key2", std::any{value2}}});
```

### AnyNode

Transforms inputs to outputs:
```cpp
wf::AnyNode node(
    {{"input_key", future}},  // input map
    {"output_key1", "output_key2"},  // output keys
    [](const auto& in) {
      // Access inputs: in.at("input_key")
      // Return outputs: {{"output_key1", value1}, {"output_key2", value2}}
      return std::unordered_map<std::string, std::any>{...};
    }
);
```

### AnySink

Consumes final values and prints them:
```cpp
wf::AnySink sink({{"key1", future1}, {"key2", future2}});
```

## Benefits of String Keys

1. **Self-documenting**: Keys describe the data semantics
2. **Flexible composition**: Easy to wire nodes with matching keys
3. **Template-friendly**: Lambda functions can easily access named parameters
4. **Debugging**: Clear trace of data flow via key names

## Future Enhancements

- Type validation and schema checking
- Automatic key inference from function signatures
- Support for optional inputs/outputs
- Visual graph representation with key annotations


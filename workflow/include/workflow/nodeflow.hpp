// Nodeflow library: string-keyed heterogeneous dataflow nodes based on Taskflow
// Inputs/outputs are accessed via unordered_map<string, any> for flexible key-based access
// Supports both typed nodes (template-based) and untyped nodes (std::any-based)

#ifndef WORKFLOW_NODEFLOW_HPP
#define WORKFLOW_NODEFLOW_HPP

#include <taskflow/taskflow.hpp>
#include <taskflow/algorithm/pipeline.hpp>
#include <taskflow/utility/small_vector.hpp>
#include <future>
#include <memory>
#include <any>
#include <unordered_map>
#include <string>
#include <functional>
#include <vector>
#include <iostream>
#include <tuple>
#include <type_traits>

namespace workflow {

// Forward declarations
struct AnyOutputs;
class INode;
template <typename... Ts> struct TypedOutputs;
template <typename InputsTuple, typename... Outs> class TypedNode;
template <typename... Outs> class TypedSource;
template <typename... Ins> class TypedSink;
struct AnyNode;
struct AnySource;
struct AnySink;
class ConditionNode;
class MultiConditionNode;
class PipelineNode;
class LoopNode;
class GraphBuilder;

// ============================================================================
// Pure virtual base class for all nodes
// ============================================================================

/**
 * @brief Base interface for all node types (typed and untyped)
 * @details Provides a common interface for nodes with name and task creation
 */
class INode {
 public:
  virtual ~INode() = default;
  
  /**
   * @brief Get the name of this node
   * @return Node name string
   */
  virtual std::string name() const = 0;
  
  /**
   * @brief Create a task functor for Taskflow
   * @param node_name Name to use in the functor
   * @return Functor that can be used with tf::Taskflow::emplace
   */
  virtual std::function<void()> functor(const char* node_name) const = 0;
  
  /**
   * @brief Get node type identifier
   * @return String describing node type (e.g., "TypedNode", "AnyNode", "Source", "Sink")
   */
  virtual std::string type() const = 0;

  /**
   * @brief Get output future by key (type-erased)
   * @param key Output key
   * @return Shared future to the output value (as std::any)
   */
  virtual std::shared_future<std::any> get_output_future(const std::string& key) const = 0;
  
  /**
   * @brief Get all output keys
   * @return Vector of output key names
   */
  virtual std::vector<std::string> get_output_keys() const = 0;
};

// ============================================================================
// Any-based outputs (runtime type-erased)
// ============================================================================

struct AnyOutputs {
  std::unordered_map<std::string, std::shared_ptr<std::promise<std::any>>> promises;
  std::unordered_map<std::string, std::shared_future<std::any>> futures;

  AnyOutputs() = default;
  explicit AnyOutputs(const std::vector<std::string>& keys);
  
  void add(const std::string& key);
  void add(const std::vector<std::string>& keys);
};

// ============================================================================
// Template-based typed outputs (compile-time type-safe)
// ============================================================================

template <typename... Outs>
struct TypedOutputs {
  std::tuple<std::shared_ptr<std::promise<Outs>>...> promises;
  std::tuple<std::shared_future<Outs>...> futures;
  // Key-value mapping for outputs (type-erased for unified interface)
  std::unordered_map<std::string, std::shared_future<std::any>> futures_map;
  std::vector<std::string> output_keys;  // Ordered keys matching tuple order
  // Any promises to be set when typed values are available
  std::unordered_map<std::size_t, std::shared_ptr<std::promise<std::any>>> any_promises_;
  // Key to index mapping for typed future access
  std::unordered_map<std::string, std::size_t> key_to_index_;
  
  TypedOutputs();
  explicit TypedOutputs(const std::vector<std::string>& keys);
  
  // Get future by key (type-erased)
  std::shared_future<std::any> get(const std::string& key) const;
  
  // Get typed future by key (type-safe, requires knowing which output by index)
  template <std::size_t I>
  std::shared_future<std::tuple_element_t<I, std::tuple<Outs...>>> get_typed(const std::string& key) const {
    if (key_to_index_.at(key) != I) {
      throw std::runtime_error("Key index mismatch for typed access");
    }
    return std::get<I>(futures);
  }
  
  // Get typed future by key and type (runtime lookup, type-safe extraction)
  template <typename T>
  std::shared_future<T> get_typed_by_key(const std::string& key) const {
    auto it = key_to_index_.find(key);
    if (it == key_to_index_.end()) {
      throw std::runtime_error("Unknown output key: " + key);
    }
    std::size_t idx = it->second;
    // Use a helper to extract the typed future at runtime
    return get_typed_future_impl<T>(idx);
  }
  
  // Get all output keys
  const std::vector<std::string>& keys() const { return output_keys; }
  
 private:
  // Helper to extract typed future by index at runtime
  template <typename T>
  std::shared_future<T> get_typed_future_impl(std::size_t idx) const {
    // Use a visitor pattern or recursive template to find matching type
    return get_typed_future_impl_helper<T>(idx, std::index_sequence_for<Outs...>{});
  }
  
  template <typename T, std::size_t... Is>
  std::shared_future<T> get_typed_future_impl_helper(std::size_t idx, std::index_sequence<Is...>) const {
    // Check if idx matches and type matches at that index
    bool found = false;
    ((idx == Is && std::is_same_v<T, std::tuple_element_t<Is, std::tuple<Outs...>>> ? (found = true) : false), ...);
    if (found) {
      return std::get<idx>(futures);
    }
    throw std::runtime_error("Type mismatch: key maps to different type");
  }
};

// ============================================================================
// Typed Source Node (known types at compile time)
// ============================================================================

template <typename... Outs>
class TypedSource : public INode {
 public:
  std::tuple<Outs...> values;
  TypedOutputs<Outs...> out;
  std::string node_name_;

  // Constructor with explicit output keys
  explicit TypedSource(std::tuple<Outs...> vals, 
                       const std::vector<std::string>& output_keys,
                       const std::string& name = "");
  // Constructor with default keys (auto-generated: "out0", "out1", ...)
  explicit TypedSource(std::tuple<Outs...> vals, const std::string& name = "");
  std::string name() const override { return node_name_; }
  std::string type() const override { return "TypedSource"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
};

// ============================================================================
// Typed Node (known input/output types)
// ============================================================================

template <typename InputsTuple, typename... Outs>
class TypedNode : public INode {
 public:
  InputsTuple inputs;
  TypedOutputs<Outs...> out;
  // Store op as a type-erased callable
  // User provides op that receives unwrapped tuple values
  mutable std::any op_;  // Type-erased: callable(std::tuple<Values...>) -> std::tuple<Outs...>
  std::string node_name_;

  // Constructor with explicit output keys
  template <typename OpType>
  TypedNode(InputsTuple fin, OpType&& fn, 
            const std::vector<std::string>& output_keys,
            const std::string& name = "");
  // Constructor with default keys
  template <typename OpType>
  TypedNode(InputsTuple fin, OpType&& fn, const std::string& name = "");
  std::string name() const override { return node_name_; }
  std::string type() const override { return "TypedNode"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
  
 private:
  // Helper to extract value types from InputsTuple
  template <typename Tuple>
  struct ExtractValueTypes;
};

// ============================================================================
// Typed Sink Node (known types at compile time)
// ============================================================================

template <typename... Ins>
class TypedSink : public INode {
 public:
  std::tuple<std::shared_future<Ins>...> inputs;
  std::string node_name_;
  std::function<void(const std::tuple<Ins...>&)> callback_;

  explicit TypedSink(std::tuple<std::shared_future<Ins>...> fin, const std::string& name = "");
  explicit TypedSink(std::tuple<std::shared_future<Ins>...> fin,
                     std::function<void(const std::tuple<Ins...>&)> callback,
                     const std::string& name = "");
  std::string name() const override { return node_name_; }
  std::string type() const override { return "TypedSink"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
};

// ============================================================================
// Any-based Source Node (runtime type-erased)
// ============================================================================

struct AnySource : public INode {
  std::unordered_map<std::string, std::any> values;
  AnyOutputs out;
  std::string node_name_;

  AnySource() = default;
  explicit AnySource(std::unordered_map<std::string, std::any> vals, const std::string& name = "");
  std::string name() const override { return node_name_; }
  std::string type() const override { return "AnySource"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;

 private:
  static std::vector<std::string> extract_keys(const std::unordered_map<std::string, std::any>& m);
};

// ============================================================================
// Any-based Node (runtime type-erased)
// ============================================================================

struct AnyNode : public INode {
  std::unordered_map<std::string, std::shared_future<std::any>> inputs;
  AnyOutputs out;
  std::function<std::unordered_map<std::string, std::any>(
      const std::unordered_map<std::string, std::any>&)> op;
  std::string node_name_;

  AnyNode() = default;
  AnyNode(std::unordered_map<std::string, std::shared_future<std::any>> fin,
          const std::vector<std::string>& out_keys,
          std::function<std::unordered_map<std::string, std::any>(
              const std::unordered_map<std::string, std::any>&)> fn,
          const std::string& name = "");
  std::string name() const override { return node_name_; }
  std::string type() const override { return "AnyNode"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
};

// ============================================================================
// Any-based Sink Node (runtime type-erased)
// ============================================================================

struct AnySink : public INode {
  std::unordered_map<std::string, std::shared_future<std::any>> inputs;
  std::string node_name_;
  std::function<void(const std::unordered_map<std::string, std::any>&)> callback_;

  AnySink() = default;
  explicit AnySink(std::unordered_map<std::string, std::shared_future<std::any>> fin, const std::string& name = "");
  explicit AnySink(std::unordered_map<std::string, std::shared_future<std::any>> fin,
                    std::function<void(const std::unordered_map<std::string, std::any>&)> callback,
                    const std::string& name = "");
  std::string name() const override { return node_name_; }
  std::string type() const override { return "AnySink"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
};

// ============================================================================
// Condition Node: Returns an integer index to select which successor to execute
// ============================================================================

/**
 * @brief Condition node that returns an integer to select successor task
 * @details Returns an integer index (0, 1, 2...) indicating which successor task to execute
 *          Supports string-keyed input and output parameters like regular nodes
 */
class ConditionNode : public INode {
 public:
  using ConditionFunc = std::function<int(const std::unordered_map<std::string, std::any>&)>;
  
  ConditionNode() = default;
  explicit ConditionNode(const std::unordered_map<std::string, std::shared_future<std::any>>& inputs,
                         ConditionFunc func,
                         const std::vector<std::string>& output_keys,
                         const std::string& name = "");
  
  std::string name() const override;
  std::string type() const override { return "ConditionNode"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
  
  std::unordered_map<std::string, std::shared_future<std::any>> inputs;
  AnyOutputs out;
  ConditionFunc func_;
  std::string node_name_;
};

// ============================================================================
// Multi-Condition Node: Returns multiple indices to execute multiple successors
// ============================================================================

/**
 * @brief Multi-condition node that returns multiple indices for parallel execution
 * @details Returns tf::SmallVector<int> to select multiple successor tasks to execute
 *          Supports string-keyed input and output parameters like regular nodes
 */
class MultiConditionNode : public INode {
 public:
  using MultiConditionFunc = std::function<tf::SmallVector<int>(const std::unordered_map<std::string, std::any>&)>;
  
  MultiConditionNode() = default;
  explicit MultiConditionNode(const std::unordered_map<std::string, std::shared_future<std::any>>& inputs,
                               MultiConditionFunc func,
                               const std::vector<std::string>& output_keys,
                               const std::string& name = "");
  
  std::string name() const override;
  std::string type() const override { return "MultiConditionNode"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
  
  std::unordered_map<std::string, std::shared_future<std::any>> inputs;
  AnyOutputs out;
  MultiConditionFunc func_;
  std::string node_name_;
};

// ============================================================================
// Pipeline Node: Wraps a Taskflow Pipeline for pipeline scheduling
// ============================================================================

/**
 * @brief Pipeline node that wraps a Taskflow Pipeline
 * @details Encapsulates a pipeline scheduling framework with parallel lines and serial pipes
 */
class PipelineNode : public INode {
 public:
  PipelineNode() = default;
  
  // Create from Taskflow Pipeline
  template <typename... Ps>
  explicit PipelineNode(tf::Pipeline<Ps...>& pipeline, const std::string& name = "");
  
  std::string name() const override;
  std::string type() const override { return "PipelineNode"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
  
  // Store pipeline as type-erased wrapper
  std::any pipeline_wrapper_;
  std::string node_name_;
};

// ============================================================================
// Loop Node: Creates a loop using a condition node
// ============================================================================

/**
 * @brief Loop node that creates iterative control flow using a condition
 * @details Uses a condition function to decide whether to continue loop or exit
 *          Supports string-keyed input and output parameters like regular nodes
 */
class LoopNode : public INode {
 public:
  using LoopConditionFunc = std::function<int(const std::unordered_map<std::string, std::any>&)>;  // Returns 0 to continue, non-zero to exit
  
  LoopNode() = default;
  
  /**
   * @brief Create a loop node
   * @param inputs Input futures from other nodes
   * @param body_func Function to execute in each iteration (receives inputs)
   * @param condition_func Function that returns 0 to continue loop, non-zero to exit
   * @param output_keys Output key names
   * @param name Node name
   */
  explicit LoopNode(const std::unordered_map<std::string, std::shared_future<std::any>>& inputs,
                    std::function<void(const std::unordered_map<std::string, std::any>&)> body_func,
                    LoopConditionFunc condition_func,
                    const std::vector<std::string>& output_keys,
                    const std::string& name = "");
  
  std::string name() const override;
  std::string type() const override { return "LoopNode"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
  
  std::unordered_map<std::string, std::shared_future<std::any>> inputs;
  AnyOutputs out;
  std::function<void(const std::unordered_map<std::string, std::any>&)> body_func_;
  LoopConditionFunc condition_func_;
  std::string node_name_;
};

// ============================================================================
// Graph Builder: Manages graph construction and execution
// ============================================================================

/**
 * @brief Manages workflow graph construction, node lifecycle, and execution
 * @details Provides a high-level API for building and running nodeflow graphs
 */
class GraphBuilder {
 public:
  explicit GraphBuilder(const std::string& name = "workflow");
  
  ~GraphBuilder() = default;

  // Disable copy, allow move
  GraphBuilder(const GraphBuilder&) = delete;
  GraphBuilder& operator=(const GraphBuilder&) = delete;
  GraphBuilder(GraphBuilder&&) = default;
  GraphBuilder& operator=(GraphBuilder&&) = default;

  /**
   * @brief Add a node to the graph
   * @param node Shared pointer to INode
   * @return Task handle for dependency configuration
   */
  tf::Task add_node(std::shared_ptr<INode> node);

  /**
   * @brief Add a typed source node
   * @param node Shared pointer to typed source
   * @return Task handle
   */
  template <typename... Outs>
  tf::Task add_typed_source(std::shared_ptr<TypedSource<Outs...>> node);

  /**
   * @brief Add a typed node
   * @param node Shared pointer to typed node
   * @return Task handle
   */
  template <typename InputsTuple, typename... Outs>
  tf::Task add_typed_node(std::shared_ptr<TypedNode<InputsTuple, Outs...>> node);

  /**
   * @brief Add a typed sink node
   * @param node Shared pointer to typed sink
   * @return Task handle
   */
  template <typename... Ins>
  tf::Task add_typed_sink(std::shared_ptr<TypedSink<Ins...>> node);

  /**
   * @brief Add an any-based source node
   * @param node Shared pointer to any source
   * @return Task handle
   */
  tf::Task add_any_source(std::shared_ptr<AnySource> node);

  /**
   * @brief Add an any-based node
   * @param node Shared pointer to any node
   * @return Task handle
   */
  tf::Task add_any_node(std::shared_ptr<AnyNode> node);

  /**
   * @brief Add an any-based sink node
   * @param node Shared pointer to any sink
   * @return Task handle
   */
  tf::Task add_any_sink(std::shared_ptr<AnySink> node);

  // Deprecated: Use declarative API instead (dependencies auto-inferred from inputs)
  // These methods are kept for backward compatibility but should not be used
  // with the new declarative API
  
  /**
   * @deprecated Use declarative API (create_typed_node/create_any_node) instead.
   * Dependencies are automatically inferred from input specifications.
   */
  [[deprecated("Use declarative API instead - dependencies are auto-inferred")]]
  void precede(tf::Task from, tf::Task to);
  
  template <typename Container>
  [[deprecated("Use declarative API instead - dependencies are auto-inferred")]]
  void precede(tf::Task from, const Container& to);
  
  [[deprecated("Use declarative API instead - dependencies are auto-inferred")]]
  void succeed(tf::Task to, tf::Task from);
  
  template <typename Container>
  [[deprecated("Use declarative API instead - dependencies are auto-inferred")]]
  void succeed(tf::Task to, const Container& from);

  /**
   * @brief Run the graph asynchronously
   * @param executor Taskflow executor
   * @return Future handle
   */
  tf::Future<void> run_async(tf::Executor& executor);

  /**
   * @brief Run the graph synchronously
   * @param executor Taskflow executor
   */
  void run(tf::Executor& executor);

  /**
   * @brief Dump graph to DOT format
   * @param os Output stream
   */
  void dump(std::ostream& os = std::cout) const;

  /**
   * @brief Get the underlying Taskflow
   * @note Only valid when GraphBuilder owns the Taskflow (constructed from name)
   */
  tf::Taskflow& taskflow() { 
    return taskflow_; 
  }
  const tf::Taskflow& taskflow() const { 
    return taskflow_; 
  }
  
  // Internal mutable access (for const methods that need to create adapter tasks)
  tf::Taskflow& taskflow_mutable() { return taskflow_; }

  /**
   * @brief Get node by name
   */
  std::shared_ptr<INode> get_node(const std::string& name) const;

  /**
   * @brief Get all nodes
   */
  const std::unordered_map<std::string, std::shared_ptr<INode>>& nodes() const { return nodes_; }

  /**
   * @brief Get output future from a node by key (works for both typed and any nodes)
   */
  std::shared_future<std::any> get_output(const std::string& node_name, const std::string& key) const;

  /**
   * @brief Get typed input from a node by key (for TypedNode construction)
   * @tparam T Type of the value
   * @param node_name Source node name
   * @param key Output key from source node
   * @return Typed shared_future
   */
  template <typename T>
  std::shared_future<T> get_input(const std::string& node_name, const std::string& key) const;

  // ============================================================================
  // Declarative API: Create nodes with automatic dependency inference
  // ============================================================================

  /**
   * @brief Create and add a typed source node
   * @param name Node name
   * @param values Initial values
   * @param output_keys Output key names
   * @return Pair of (node_ptr, task_handle)
   */
  template <typename... Outs>
  std::pair<std::shared_ptr<TypedSource<Outs...>>, tf::Task> 
  create_typed_source(const std::string& name,
                       std::tuple<Outs...> values,
                       const std::vector<std::string>& output_keys);

  /**
   * @brief Create and add a typed node with key-based inputs
   * @tparam Ins... Input types (must be explicitly specified)
   * @tparam OpType Functor type (auto-deduced)
   * @param name Node name
   * @param input_specs Vector of {source_node_name, source_output_key} pairs
   * @param functor Operation function: tuple<Ins...> -> tuple<Outs...>
   * @param output_keys Output key names
   * @details Automatically fetches inputs, infers output types from functor, and establishes dependencies
   */
  template <typename... Ins, typename OpType>
  auto create_typed_node(const std::string& name,
                        const std::vector<std::pair<std::string, std::string>>& input_specs,
                        OpType&& functor,
                        const std::vector<std::string>& output_keys);

  /**
   * @brief Create and add an any-based source node
   */
  std::pair<std::shared_ptr<AnySource>, tf::Task>
  create_any_source(const std::string& name,
                    std::unordered_map<std::string, std::any> values);

  /**
   * @brief Create and add an any-based node with key-based inputs
   */
  std::pair<std::shared_ptr<AnyNode>, tf::Task>
  create_any_node(const std::string& name,
                  const std::vector<std::pair<std::string, std::string>>& input_specs,
                  std::function<std::unordered_map<std::string, std::any>(
                      const std::unordered_map<std::string, std::any>&)> functor,
                  const std::vector<std::string>& output_keys);

  /**
   * @brief Create and add an any-based sink with key-based inputs
   */
  std::pair<std::shared_ptr<AnySink>, tf::Task>
  create_any_sink(const std::string& name,
                  const std::vector<std::pair<std::string, std::string>>& input_specs);

  /**
   * @brief Create and add an any-based sink with key-based inputs and a callback function
   * @param name Node name
   * @param input_specs Vector of {source_node_name, source_output_key} pairs
   * @param callback Function to process the collected values: (values_map) -> void
   * @return Pair of (node_ptr, task_handle)
   */
  std::pair<std::shared_ptr<AnySink>, tf::Task>
  create_any_sink(const std::string& name,
                  const std::vector<std::pair<std::string, std::string>>& input_specs,
                  std::function<void(const std::unordered_map<std::string, std::any>&)> callback);

  /**
   * @brief Create and add a typed sink with key-based inputs
   * @tparam Ins... Input types (must be explicitly specified)
   * @param name Node name
   * @param input_specs Vector of {source_node_name, source_output_key} pairs
   * @return Pair of (node_ptr, task_handle)
   */
  template <typename... Ins>
  std::pair<std::shared_ptr<TypedSink<Ins...>>, tf::Task>
  create_typed_sink(const std::string& name,
                    const std::vector<std::pair<std::string, std::string>>& input_specs);

  /**
   * @brief Create and add a typed sink with key-based inputs and a callback function
   * @tparam Ins... Input types (must be explicitly specified)
   * @param name Node name
   * @param input_specs Vector of {source_node_name, source_output_key} pairs
   * @param callback Function to process the collected values: (values_tuple) -> void
   * @return Pair of (node_ptr, task_handle)
   */
  template <typename... Ins>
  std::pair<std::shared_ptr<TypedSink<Ins...>>, tf::Task>
  create_typed_sink(const std::string& name,
                    const std::vector<std::pair<std::string, std::string>>& input_specs,
                    std::function<void(const std::tuple<Ins...>&)> callback);

  // ============================================================================
  // Advanced Control Flow Nodes: Condition, Multi-Condition, Pipeline, Loop
  // ============================================================================

  /**
   * @brief Create and add a pipeline node
   * @tparam Ps Pipe types
   * @param name Node name
   * @param num_lines Number of parallel lines
   * @param pipes Variadic pipe arguments
   * @return Pair of (node_ptr, task_handle)
   * @details Creates a Taskflow Pipeline with specified number of lines and pipes
   */
  template <typename... Ps>
  std::pair<std::shared_ptr<PipelineNode>, tf::Task>
  create_pipeline_node(const std::string& name,
                       size_t num_lines,
                       Ps&&... pipes);

  // ----------------------------------------------------------------------------
  // Declarative helpers for control flow and subgraphs
  // ----------------------------------------------------------------------------

  /**
   * @brief Create a subgraph as a module task using a nested GraphBuilder
   * @param name Subgraph name
   * @param builder_fn Function that receives a nested GraphBuilder to define the subgraph
   * @return Task handle representing the subgraph module
   */
  tf::Task create_subgraph(const std::string& name,
                           const std::function<void(GraphBuilder&)>& builder_fn);

  /**
   * @brief Create a subgraph with string-keyed inputs and outputs
   * @param name Subgraph name
   * @param input_specs Vector of {source_node_name, source_output_key} pairs
   * @param builder_fn Function that receives a nested GraphBuilder and input values to define the subgraph
   * @param output_keys Output key names that the subgraph will produce
   * @return Pair of (subgraph_node_ptr, task_handle)
   * @details The subgraph receives inputs and can produce outputs. Automatically establishes dependencies.
   */
  std::pair<std::shared_ptr<AnyNode>, tf::Task>
  create_subgraph(const std::string& name,
                  const std::vector<std::pair<std::string, std::string>>& input_specs,
                  std::function<void(GraphBuilder&, const std::unordered_map<std::string, std::any>&)> builder_fn,
                  const std::vector<std::string>& output_keys);

  /**
   * @brief Create a task that builds and runs a fresh subgraph at execution time
   * @param name Task name
   * @param builder_fn Function to populate the subgraph using a provided GraphBuilder
   * @return Task handle representing the executable subtask
   * @details Unlike create_subgraph (which creates a composed_of module that runs once),
   *          this creates a normal task that, on each execution, constructs a new subgraph
   *          and runs it using the enclosing builder's executor. This is suitable for loop bodies.
   */
  tf::Task create_subtask(const std::string& name,
                          const std::function<void(GraphBuilder&)>& builder_fn);

  /**
   * @brief Create a subtask with string-keyed inputs and outputs
   * @param name Task name
   * @param input_specs Vector of {source_node_name, source_output_key} pairs
   * @param builder_fn Function to populate the subgraph using a provided GraphBuilder and input values
   * @param output_keys Output key names that the subtask will produce
   * @return Pair of (subtask_node_ptr, task_handle)
   * @details The subtask receives inputs and can produce outputs. Automatically establishes dependencies.
   */
  std::pair<std::shared_ptr<AnyNode>, tf::Task>
  create_subtask(const std::string& name,
                 const std::vector<std::pair<std::string, std::string>>& input_specs,
                 std::function<void(GraphBuilder&, const std::unordered_map<std::string, std::any>&)> builder_fn,
                 const std::vector<std::string>& output_keys);

  /**
   * @brief Declarative condition with string-keyed inputs and outputs
   * @param name Condition task name
   * @param input_specs Vector of {source_node_name, source_output_key} pairs
   * @param condition_func Function that receives inputs and returns index of successor
   * @param successors Successor tasks to wire
   * @param output_keys Output key names (e.g., {"result"})
   * @return Pair of (node_ptr, task_handle)
   */
  std::pair<std::shared_ptr<ConditionNode>, tf::Task>
  create_condition_decl(const std::string& name,
                        const std::vector<std::pair<std::string, std::string>>& input_specs,
                        std::function<int(const std::unordered_map<std::string, std::any>&)> condition_func,
                        const std::vector<tf::Task>& successors,
                        const std::vector<std::string>& output_keys = {"result"});

  /**
   * @brief Declarative multi-condition with string-keyed inputs and outputs
   * @param name Multi-condition task name
   * @param input_specs Vector of {source_node_name, source_output_key} pairs
   * @param func Function that receives inputs and returns indices of successors to run
   * @param successors Successor tasks to wire in order
   * @param output_keys Output key names (e.g., {"result"})
   * @return Pair of (node_ptr, task_handle)
   */
  std::pair<std::shared_ptr<MultiConditionNode>, tf::Task>
  create_multi_condition_decl(const std::string& name,
                              const std::vector<std::pair<std::string, std::string>>& input_specs,
                              std::function<tf::SmallVector<int>(const std::unordered_map<std::string, std::any>&)> func,
                              const std::vector<tf::Task>& successors,
                              const std::vector<std::string>& output_keys = {"result"});

  /**
   * @brief Declarative loop with string-keyed inputs and outputs
   * @param name Loop name
   * @param input_specs Vector of {source_node_name, source_output_key} pairs
   * @param body_builder_fn Function that receives a GraphBuilder and inputs to build the loop body
   * @param condition_func Function that receives inputs and returns 0 to continue (loop back), non-zero to exit
   * @param exit_builder_fn Optional function to build exit task (receives GraphBuilder and inputs)
   * @param output_keys Output key names
   * @return Pair of (node_ptr, task_handle for loop entry)
   */
  std::pair<std::shared_ptr<LoopNode>, tf::Task>
  create_loop_decl(const std::string& name,
                   const std::vector<std::pair<std::string, std::string>>& input_specs,
                   std::function<void(GraphBuilder&, const std::unordered_map<std::string, std::any>&)> body_builder_fn,
                   std::function<int(const std::unordered_map<std::string, std::any>&)> condition_func,
                   std::function<void(GraphBuilder&, const std::unordered_map<std::string, std::any>&)> exit_builder_fn = nullptr,
                   const std::vector<std::string>& output_keys = {});
  /**
   * @brief Declarative loop using an existing body task (e.g., from create_subgraph)
   * @param name Loop name
   * @param body_task Task representing the loop body
   * @param condition_func Returns 0 to continue (loop back), non-zero to exit
   * @param exit_task Optional task to run when exiting the loop (non-zero)
   * @return The created condition task controlling the loop
   */
  tf::Task create_loop_decl(const std::string& name,
                            tf::Task& body_task,
                            std::function<int()> condition_func,
                            tf::Task exit_task = tf::Task{});

  /**
   * @brief Declarative loop with auto predecessors by node names
   */
  tf::Task create_loop_decl(const std::string& name,
                            const std::vector<std::string>& depend_on_nodes,
                            tf::Task& body_task,
                            std::function<int()> condition_func,
                            tf::Task exit_task = tf::Task{});

  // ============================================================================
  // Taskflow Algorithm Nodes: Parallel Iterations, Reductions, Transforms
  // ============================================================================

  /**
   * @brief Create a parallel for_each node that iterates over a container
   * @tparam Container Container type (auto-deduced)
   * @tparam C Callable type
   * @param name Node name
   * @param input_specs Vector of {source_node_name, source_output_key} to get the container
   * @param callable Function to apply to each element: (element) -> void
   * @param output_keys Optional output keys (for chaining)
   * @return Pair of (node_ptr, task_handle)
   * @details The container is extracted from the input at execution time
   */
  template <typename Container, typename C>
  std::pair<std::shared_ptr<AnyNode>, tf::Task>
  create_for_each(const std::string& name,
                  const std::vector<std::pair<std::string, std::string>>& input_specs,
                  C callable,
                  const std::vector<std::string>& output_keys = {});

  /**
   * @brief Create a parallel for_each_index node that iterates over an index range
   * @tparam B Beginning index type (integral)
   * @tparam E Ending index type (integral)
   * @tparam S Step type (integral)
   * @tparam C Callable type
   * @param name Node name
   * @param input_specs Vector of {source_node_name, source_output_key} to get first, last, step
   * @param callable Function to apply to each index: (index) -> void
   * @param output_keys Optional output keys
   * @return Pair of (node_ptr, task_handle)
   * @details Expects inputs: "first", "last", "step" (optional, defaults to 1)
   */
  template <typename B, typename E, typename S = int, typename C>
  std::pair<std::shared_ptr<AnyNode>, tf::Task>
  create_for_each_index(const std::string& name,
                        const std::vector<std::pair<std::string, std::string>>& input_specs,
                        C callable,
                        const std::vector<std::string>& output_keys = {});

  /**
   * @brief Create a parallel reduce node
   * @tparam T Result type
   * @tparam Container Container type (auto-deduced)
   * @tparam BinaryOp Binary operation type
   * @param name Node name
   * @param input_specs Vector of {source_node_name, source_output_key} to get the container
   * @param init Initial value (captured by reference - must remain alive)
   * @param bop Binary operator: (T, element_type) -> T
   * @param output_keys Output key for the reduced result (default: "result")
   * @return Pair of (node_ptr, task_handle)
   * @details The reduced result is stored in init and also exposed via output key
   */
  template <typename T, typename Container, typename BinaryOp>
  std::pair<std::shared_ptr<AnyNode>, tf::Task>
  create_reduce(const std::string& name,
                const std::vector<std::pair<std::string, std::string>>& input_specs,
                T& init,
                BinaryOp bop,
                const std::vector<std::string>& output_keys = {"result"});

  /**
   * @brief Create a parallel transform node
   * @tparam InputContainer Input container type (auto-deduced)
   * @tparam OutputContainer Output container type (auto-deduced)
   * @tparam UnaryOp Unary operation type
   * @param name Node name
   * @param input_specs Vector of {source_node_name, source_output_key} to get the input container
   * @param unary_op Unary operation: (input_element) -> output_element
   * @param output_keys Output key for the transformed container (default: "result")
   * @return Pair of (node_ptr, task_handle)
   * @details Creates a new output container with transformed elements
   */
  template <typename InputContainer, typename OutputContainer, typename UnaryOp>
  std::pair<std::shared_ptr<AnyNode>, tf::Task>
  create_transform(const std::string& name,
                  const std::vector<std::pair<std::string, std::string>>& input_specs,
                  UnaryOp unary_op,
                  const std::vector<std::string>& output_keys = {"result"});

                   
 private:
  // Helper to extract typed future from source node (for create_typed_node)
  template <typename T>
  std::shared_future<T> get_typed_input_impl(const std::string& node_name, 
                                             const std::string& key);
  
  // Implementation helper for create_typed_node
  template <typename... Ins, typename OpType, std::size_t... OutIndices>
  auto create_typed_node_impl(const std::string& name,
                              std::tuple<std::shared_future<Ins>...> input_futures,
                              OpType&& functor,
                              const std::vector<std::string>& output_keys,
                              std::index_sequence<OutIndices...>);
  
 private:
  tf::Taskflow taskflow_;  // Used when constructed from name (owns graph)
  tf::Executor* executor_;
  std::unordered_map<std::string, std::shared_ptr<INode>> nodes_;
  std::unordered_map<std::string, tf::Task> tasks_;
  // Adapter tasks created for typed extraction: key format "<node>::<key>"
  mutable std::unordered_map<std::string, tf::Task> adapter_tasks_;
  // Hold nested subgraph builders to keep composed_of lifetimes valid
  std::vector<std::unique_ptr<GraphBuilder>> subgraph_builders_;
};

}  // namespace workflow

// Include template implementations (after namespace close - nodeflow_impl.hpp defines its own namespace)
#include <workflow/nodeflow_impl.hpp>

#endif  // WORKFLOW_NODEFLOW_HPP

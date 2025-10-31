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

  explicit TypedSink(std::tuple<std::shared_future<Ins>...> fin, const std::string& name = "");
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

  AnySink() = default;
  explicit AnySink(std::unordered_map<std::string, std::shared_future<std::any>> fin, const std::string& name = "");
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
 * @brief Condition node that returns an integer to select which branch subgraph to execute
 * @details Returns an integer index (0, 1, 2...) indicating which branch subgraph to execute
 *          Each branch is a GraphBuilder subgraph composed of workflow nodes
 */
class ConditionNode : public INode {
 public:
  using ConditionFunc = std::function<int()>;
  using BranchBuilder = std::function<void(GraphBuilder&)>;  // Function to build branch subgraph
  
  ConditionNode() = default;
  
  /**
   * @brief Create a condition node with multiple branch subgraphs
   * @param func Condition function that returns branch index
   * @param branches Vector of functions that build each branch subgraph
   * @param name Node name
   */
  explicit ConditionNode(ConditionFunc func,
                        std::vector<BranchBuilder> branches,
                        const std::string& name = "");
  
  std::string name() const override;
  std::string type() const override { return "ConditionNode"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
  
  ConditionFunc func_;
  std::vector<BranchBuilder> branches_;  // Branch subgraph builders
  std::vector<tf::Taskflow> branch_taskflows_;  // Taskflows for each branch
  std::string node_name_;
};

// ============================================================================
// Multi-Condition Node: Returns multiple indices to execute multiple successors
// ============================================================================

/**
 * @brief Multi-condition node that returns multiple indices for parallel execution
 * @details Returns tf::SmallVector<int> to select multiple successor tasks to execute
 */
class MultiConditionNode : public INode {
 public:
  using MultiConditionFunc = std::function<tf::SmallVector<int>()>;
  
  MultiConditionNode() = default;
  explicit MultiConditionNode(MultiConditionFunc func, const std::string& name = "");
  
  std::string name() const override;
  std::string type() const override { return "MultiConditionNode"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
  
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
// Loop Node: Creates a loop using a condition node with subgraph body
// ============================================================================

/**
 * @brief Loop node that creates iterative control flow using a condition
 * @details Uses a condition function to decide whether to continue loop or exit
 *          Loop body is a GraphBuilder subgraph composed of workflow nodes
 */
class LoopNode : public INode {
 public:
  using LoopConditionFunc = std::function<int()>;  // Returns 0 to continue, non-zero to exit
  using LoopBodyBuilder = std::function<void(GraphBuilder&)>;  // Function to build loop body subgraph
  
  LoopNode() = default;
  
  /**
   * @brief Create a loop node
   * @param body_builder Function that builds the loop body subgraph
   * @param condition_func Function that returns 0 to continue loop, non-zero to exit
   * @param name Node name
   */
  explicit LoopNode(LoopBodyBuilder body_builder,
                    LoopConditionFunc condition_func,
                    const std::string& name = "");
  
  std::string name() const override;
  std::string type() const override { return "LoopNode"; }
  std::function<void()> functor(const char* node_name) const override;
  std::shared_future<std::any> get_output_future(const std::string& key) const override;
  std::vector<std::string> get_output_keys() const override;
  
  LoopBodyBuilder body_builder_;
  LoopConditionFunc condition_func_;
  tf::Taskflow body_taskflow_;  // Taskflow for loop body
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
   */
  tf::Taskflow& taskflow() { return taskflow_; }
  const tf::Taskflow& taskflow() const { return taskflow_; }
  
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

  // ============================================================================
  // Advanced Control Flow Nodes: Condition, Multi-Condition, Pipeline, Loop
  // ============================================================================

  /**
   * @brief Create and add a condition node with branch subgraphs
   * @param name Node name
   * @param condition_func Function that returns an integer index (0, 1, 2...) to select branch
   * @param branches Vector of functions that build each branch subgraph using GraphBuilder
   * @return Pair of (node_ptr, task_handle)
   * @details The returned index selects which branch subgraph to execute
   *          Each branch is built as a separate GraphBuilder subgraph
   */
  std::pair<std::shared_ptr<ConditionNode>, tf::Task>
  create_condition_node(const std::string& name,
                       std::function<int()> condition_func,
                       std::vector<std::function<void(GraphBuilder&)>> branches);

  /**
   * @brief Create and add a multi-condition node
   * @param name Node name
   * @param multi_condition_func Function that returns tf::SmallVector<int> to select multiple successors
   * @return Pair of (node_ptr, task_handle)
   * @details Returns multiple indices to execute multiple successor tasks in parallel
   */
  std::pair<std::shared_ptr<MultiConditionNode>, tf::Task>
  create_multi_condition_node(const std::string& name,
                              std::function<tf::SmallVector<int>()> multi_condition_func);

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

  /**
   * @brief Create and add a loop node with subgraph body
   * @param name Node name
   * @param body_builder Function that builds the loop body subgraph using GraphBuilder
   * @param condition_func Function that returns 0 to continue loop, non-zero to exit
   * @return Pair of (node_ptr, task_handle)
   * @details Creates a loop using condition task. Loop body is a GraphBuilder subgraph.
   *          Loop continues while condition returns 0.
   */
  std::pair<std::shared_ptr<LoopNode>, tf::Task>
  create_loop_node(const std::string& name,
                   std::function<void(GraphBuilder&)> body_builder,
                   std::function<int()> condition_func);
  

 private:
  // Helper to extract typed future from source node (for create_typed_node)
  template <typename T>
  std::shared_future<T> get_typed_input_impl(const std::string& node_name, 
                                             const std::string& key) const;
  
  // Implementation helper for create_typed_node
  template <typename... Ins, typename OpType, std::size_t... OutIndices>
  auto create_typed_node_impl(const std::string& name,
                              std::tuple<std::shared_future<Ins>...> input_futures,
                              OpType&& functor,
                              const std::vector<std::string>& output_keys,
                              std::index_sequence<OutIndices...>);
  
 private:
  tf::Taskflow taskflow_;
  tf::Executor* executor_;
  std::unordered_map<std::string, std::shared_ptr<INode>> nodes_;
  std::unordered_map<std::string, tf::Task> tasks_;
  // Adapter tasks created for typed extraction: key format "<node>::<key>"
  mutable std::unordered_map<std::string, tf::Task> adapter_tasks_;
};

}  // namespace workflow

// Include template implementations
#include <workflow/nodeflow_impl.hpp>

#endif  // WORKFLOW_NODEFLOW_HPP

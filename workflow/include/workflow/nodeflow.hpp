// Nodeflow library: string-keyed heterogeneous dataflow nodes based on Taskflow
// Inputs/outputs are accessed via unordered_map<string, any> for flexible key-based access
// Supports both typed nodes (template-based) and untyped nodes (std::any-based)

#ifndef WORKFLOW_NODEFLOW_HPP
#define WORKFLOW_NODEFLOW_HPP

#include <taskflow/taskflow.hpp>
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
  
  TypedOutputs();
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

  explicit TypedSource(std::tuple<Outs...> vals, const std::string& name = "");
  std::string name() const override { return node_name_; }
  std::string type() const override { return "TypedSource"; }
  std::function<void()> functor(const char* node_name) const override;
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

  template <typename OpType>
  TypedNode(InputsTuple fin, OpType&& fn, const std::string& name = "");
  std::string name() const override { return node_name_; }
  std::string type() const override { return "TypedNode"; }
  std::function<void()> functor(const char* node_name) const override;
  
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

  /**
   * @brief Set execution dependency: task_from precedes task_to
   */
  void precede(tf::Task from, tf::Task to);
  template <typename Container>
  void precede(tf::Task from, const Container& to);
  void succeed(tf::Task to, tf::Task from);
  template <typename Container>
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

  /**
   * @brief Get node by name
   */
  std::shared_ptr<INode> get_node(const std::string& name) const;

  /**
   * @brief Get all nodes
   */
  const std::unordered_map<std::string, std::shared_ptr<INode>>& nodes() const { return nodes_; }

 private:
  tf::Taskflow taskflow_;
  tf::Executor* executor_;
  std::unordered_map<std::string, std::shared_ptr<INode>> nodes_;
  std::unordered_map<std::string, tf::Task> tasks_;
};

}  // namespace workflow

// Include template implementations
#include <workflow/nodeflow_impl.hpp>

#endif  // WORKFLOW_NODEFLOW_HPP

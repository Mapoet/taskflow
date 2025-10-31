// Template implementations for nodeflow.hpp

#ifndef WORKFLOW_NODEFLOW_IMPL_HPP
#define WORKFLOW_NODEFLOW_IMPL_HPP

#include <workflow/nodeflow.hpp>
#include <tuple>
#include <type_traits>

namespace workflow {

// Tuple utilities
namespace detail {
  template <typename Tuple, typename F, std::size_t... I>
  void tuple_for_each_impl(Tuple&& t, F&& f, std::index_sequence<I...>) {
    (f(std::get<I>(t), std::integral_constant<std::size_t, I>{}), ...);
  }

  template <typename... Ts, typename F>
  void tuple_for_each(std::tuple<Ts...>& t, F&& f) {
    tuple_for_each_impl(t, std::forward<F>(f), std::index_sequence_for<Ts...>{});
  }

  template <typename Tuple, typename F, std::size_t... I>
  auto tuple_transform_impl(const Tuple& t, F&& f, std::index_sequence<I...>) {
    return std::make_tuple(f(std::get<I>(t))...);
  }

  template <typename... Ts, typename F>
  auto tuple_transform(const std::tuple<Ts...>& t, F&& f) {
    return tuple_transform_impl(t, std::forward<F>(f), std::index_sequence_for<Ts...>{});
  }
}  // namespace detail

// ============================================================================
// TypedOutputs implementation
// ============================================================================

template <typename... Outs>
TypedOutputs<Outs...>::TypedOutputs() {
  promises = std::make_tuple(std::make_shared<std::promise<Outs>>()...);
  futures = detail::tuple_transform(promises, [](auto& p){ return p->get_future().share(); });
  
  // Generate default keys: "out0", "out1", ...
  output_keys.resize(sizeof...(Outs));
  [this]<std::size_t... Is>(std::index_sequence<Is...>) {
    ((output_keys[Is] = "out" + std::to_string(Is)), ...);
    ((key_to_index_[output_keys[Is]] = Is), ...);
  }(std::index_sequence_for<Outs...>{});
  
  // Create any promises for key-based access
  detail::tuple_for_each(futures, [this](auto& fut, auto I) {
    auto p_any = std::make_shared<std::promise<std::any>>();
    auto f_any = p_any->get_future().share();
    futures_map[output_keys[I.value]] = f_any;
    any_promises_[I.value] = p_any;
  });
}

template <typename... Outs>
TypedOutputs<Outs...>::TypedOutputs(const std::vector<std::string>& keys) {
  if (keys.size() != sizeof...(Outs)) {
    throw std::runtime_error("Number of keys must match number of outputs");
  }
  output_keys = keys;
  promises = std::make_tuple(std::make_shared<std::promise<Outs>>()...);
  futures = detail::tuple_transform(promises, [](auto& p){ return p->get_future().share(); });
  
  // Create key-to-index mapping and any promises
  detail::tuple_for_each(futures, [this, &keys](auto& fut, auto I) {
    key_to_index_[keys[I.value]] = I.value;
    auto p_any = std::make_shared<std::promise<std::any>>();
    auto f_any = p_any->get_future().share();
    futures_map[keys[I.value]] = f_any;
    any_promises_[I.value] = p_any;
  });
}

template <typename... Outs>
std::shared_future<std::any> TypedOutputs<Outs...>::get(const std::string& key) const {
  auto it = futures_map.find(key);
  if (it == futures_map.end()) {
    throw std::runtime_error("Unknown output key: " + key);
  }
  return it->second;
}

// ============================================================================
// TypedSource implementation
// ============================================================================

template <typename... Outs>
TypedSource<Outs...>::TypedSource(std::tuple<Outs...> vals, const std::string& name)
    : values(std::move(vals)), node_name_(name.empty() ? "TypedSource" : name) {
  out = TypedOutputs<Outs...>();
}

template <typename... Outs>
TypedSource<Outs...>::TypedSource(std::tuple<Outs...> vals, 
                                   const std::vector<std::string>& output_keys,
                                   const std::string& name)
    : values(std::move(vals)), node_name_(name.empty() ? "TypedSource" : name) {
  out = TypedOutputs<Outs...>(output_keys);
}

template <typename... Outs>
std::shared_future<std::any> TypedSource<Outs...>::get_output_future(const std::string& key) const {
  return out.get(key);
}

template <typename... Outs>
std::vector<std::string> TypedSource<Outs...>::get_output_keys() const {
  return out.keys();
}

template <typename... Outs>
std::function<void()> TypedSource<Outs...>::functor(const char* node_name) const {
  auto promises = out.promises;
  auto any_promises = out.any_promises_;
  auto vals = values;
  return [promises, any_promises, vals, node_name]() mutable {
    detail::tuple_for_each(const_cast<std::tuple<Outs...>&>(vals), [&](auto& v, auto I){
      // Set typed promise
      std::get<I.value>(promises)->set_value(v);
      // Set any promise for key-based access
      if (auto it = any_promises.find(I.value); it != any_promises.end()) {
        it->second->set_value(std::any{v});
      }
    });
    std::cout << (node_name ? node_name : "TypedSource") << " emitted\n";
  };
}

// ============================================================================
// TypedNode implementation
// ============================================================================

// Helper to extract value type from future
template <typename T>
struct FutureValueType;

template <typename T>
struct FutureValueType<std::shared_future<T>> {
  using type = T;
};

// Helper to extract value types from tuple of futures
template <typename Tuple>
struct ExtractValueTypesHelper;

template <typename... Futures>
struct ExtractValueTypesHelper<std::tuple<Futures...>> {
  // For std::shared_future<T>, extract T
  using type = std::tuple<typename FutureValueType<Futures>::type...>;
};

template <typename InputsTuple, typename... Outs>
template <typename OpType>
TypedNode<InputsTuple, Outs...>::TypedNode(InputsTuple fin, OpType&& fn, const std::string& name)
    : inputs(std::move(fin)), node_name_(name.empty() ? "TypedNode" : name) {
  // Initialize outputs with default keys
  out = TypedOutputs<Outs...>();
  // Convert fn to std::function with proper signature
  using ValuesTuple = typename ExtractValueTypesHelper<InputsTuple>::type;
  using OpFuncType = std::function<std::tuple<Outs...>(ValuesTuple)>;
  op_ = OpFuncType(std::forward<OpType>(fn));
}

template <typename InputsTuple, typename... Outs>
template <typename OpType>
TypedNode<InputsTuple, Outs...>::TypedNode(InputsTuple fin, OpType&& fn,
                                            const std::vector<std::string>& output_keys,
                                            const std::string& name)
    : inputs(std::move(fin)), node_name_(name.empty() ? "TypedNode" : name) {
  // Initialize outputs with explicit keys
  out = TypedOutputs<Outs...>(output_keys);
  // Convert fn to std::function with proper signature
  using ValuesTuple = typename ExtractValueTypesHelper<InputsTuple>::type;
  using OpFuncType = std::function<std::tuple<Outs...>(ValuesTuple)>;
  op_ = OpFuncType(std::forward<OpType>(fn));
}

template <typename InputsTuple, typename... Outs>
std::function<void()> TypedNode<InputsTuple, Outs...>::functor(const char* node_name) const {
  auto fin = inputs;
  auto promises = out.promises;
  auto any_promises = out.any_promises_;
  // Extract op from std::any
  using ValuesTuple = typename ExtractValueTypesHelper<InputsTuple>::type;
  using OpFuncType = std::function<std::tuple<Outs...>(ValuesTuple)>;
  OpFuncType fn = std::any_cast<OpFuncType>(op_);
  
  return [fin, promises, any_promises, fn, node_name]() mutable {
    // Extract values from futures
    auto in_vals = detail::tuple_transform(fin, [](auto& f){ return f.get(); });
    // Call op with unwrapped values
    auto out_vals = fn(std::move(in_vals));
    detail::tuple_for_each(out_vals, [&](auto& v, auto I){
      // Set typed promise
      std::get<I.value>(promises)->set_value(v);
      // Set any promise for key-based access
      if (auto it = any_promises.find(I.value); it != any_promises.end()) {
        it->second->set_value(std::any{v});
      }
    });
    std::cout << (node_name ? node_name : "TypedNode") << " done\n";
  };
}

template <typename InputsTuple, typename... Outs>
std::shared_future<std::any> TypedNode<InputsTuple, Outs...>::get_output_future(const std::string& key) const {
  return out.get(key);
}

template <typename InputsTuple, typename... Outs>
std::vector<std::string> TypedNode<InputsTuple, Outs...>::get_output_keys() const {
  return out.keys();
}

// ============================================================================
// TypedSink implementation
// ============================================================================

template <typename... Ins>
std::shared_future<std::any> TypedSink<Ins...>::get_output_future(const std::string& key) const {
  throw std::runtime_error("TypedSink has no outputs");
}

template <typename... Ins>
std::vector<std::string> TypedSink<Ins...>::get_output_keys() const {
  return {};  // Sink has no outputs
}

template <typename... Ins>
TypedSink<Ins...>::TypedSink(std::tuple<std::shared_future<Ins>...> fin, const std::string& name)
    : inputs(std::move(fin)), node_name_(name.empty() ? "TypedSink" : name) {}

template <typename... Ins>
std::function<void()> TypedSink<Ins...>::functor(const char* node_name) const {
  auto fin = inputs;
  return [fin, node_name]() mutable {
    auto vals = detail::tuple_transform(fin, [](auto& f){ return f.get(); });
    std::cout << (node_name ? node_name : "TypedSink") << ": ";
    detail::tuple_for_each(vals, [](auto& v, auto I){
      if (I.value > 0) std::cout << ' ';
      std::cout << v;
    });
    std::cout << '\n';
  };
}

// ============================================================================
// GraphBuilder template methods
// ============================================================================

template <typename... Outs>
tf::Task GraphBuilder::add_typed_source(std::shared_ptr<TypedSource<Outs...>> node) {
  return add_node(std::static_pointer_cast<INode>(node));
}

template <typename InputsTuple, typename... Outs>
tf::Task GraphBuilder::add_typed_node(std::shared_ptr<TypedNode<InputsTuple, Outs...>> node) {
  return add_node(std::static_pointer_cast<INode>(node));
}

template <typename... Ins>
tf::Task GraphBuilder::add_typed_sink(std::shared_ptr<TypedSink<Ins...>> node) {
  return add_node(std::static_pointer_cast<INode>(node));
}

// Deprecated methods (inline implementations)
template <typename Container>
void GraphBuilder::precede(tf::Task from, const Container& to) {
  from.precede(to.begin(), to.end());
}

template <typename Container>
void GraphBuilder::succeed(tf::Task to, const Container& from) {
  to.succeed(from.begin(), from.end());
}

// ============================================================================
// GraphBuilder: get_input implementation
// ============================================================================

template <typename T>
std::shared_future<T> GraphBuilder::get_input(const std::string& node_name, const std::string& key) const {
  auto node = get_node(node_name);
  if (!node) {
    throw std::runtime_error("Node not found: " + node_name);
  }

  // Try to get typed future from TypedSource/TypedNode
  // We need to access the out member, but it's not accessible via INode
  
  // Solution: Use type erasure with a helper that tries to extract typed future
  // For TypedSource/TypedNode, we can access out.get_typed_by_key<T>
  // But we need to know it's a Typed node...
  
  // Practical approach: Store a helper in TypedOutputs that can extract by type
  // But we need access to TypedOutputs, which is only available via template
  
  // Best solution: Create an adapter promise that extracts T from any
  // This adapter will be set when the source node executes
  auto any_fut = node->get_output_future(key);
  
  // Create adapter: when any_fut is ready, extract T and set typed promise
  auto p_typed = std::make_shared<std::promise<T>>();
  auto f_typed = p_typed->get_future().share();
  
  // We need to create a task that will execute when any_fut is ready
  // But we can't easily do this without storing state...
  
  // Actually, we can use a lambda that captures the promise and future
  // and schedule it as a task that runs after the source
  // But this requires storing adapter tasks...
  
  // Simplest working solution for now:
  // For TypedSource/TypedNode, we need direct access to out
  // Since we can't do this via INode, we'll require users to use
  // node->out.get_typed_by_key<T>(key) directly
  
  // But this defeats the purpose of get_input...
  
  // Better: Add a virtual method to INode that can extract typed future
  // But that requires storing type information...
  
  // Practical compromise: Use runtime type checking
  // Store a function pointer or use std::function to extract typed future
  
  // For now, let's use a simple approach: create an adapter promise
  // that will be fulfilled by an adapter task (created when needed)
  
  // Actually, we can create the adapter task now and schedule it
  // But we need to know when to run it...
  
  // Let me use a different approach: 
  // Check if we can cast to a known TypedSource/TypedNode type
  // and extract directly. But this is type-dependent...
  
  // Best practical solution: Add a virtual method template
  // But C++ doesn't support virtual templates...
  
  // Alternative: Use a type registry or visitor pattern
  // But that's complex...
  
  // For now, let's use a simpler helper that works with runtime extraction
  // We'll create an adapter that extracts T at runtime from any
  // Store this as a deferred task or use a shared state
  
  // Simplest working implementation:
  // Create a shared state that extracts T when any_fut is ready
  // This will be set by an adapter lambda
  
  // Since we need to schedule this, let's create an adapter task
  // But tasks can only be scheduled after nodes are added...
  
  // Actually, we can store the adapter info and create the task later
  // Or, we can use std::shared_future with a custom getter
  
  // Let me use the simplest approach: require the output index
  // or use a helper that tries to match types
  
  // For now, let's add a method to TypedOutputs that does this
  // But we need access to TypedOutputs...
  
  // Practical solution: Create a wrapper future that extracts T
  // This wrapper will block until any_fut is ready, then extract T
  
  // We can't easily do this with std::shared_future...
  // We'd need a custom future wrapper
  
  // Best solution: Add get_typed_by_key<T> to TypedOutputs (done above)
  // Then, in get_input, try to use that if node is Typed
  
  // But we can't easily check if node is Typed without type info...
  
  // For now, throw error with helpful message
  // Users can use node->out.get_typed_by_key<T>(key) for Typed nodes
  
  throw std::runtime_error(
    "get_input<T> for typed extraction is not yet fully implemented. "
    "For TypedSource/TypedNode, use: source_node->out.get_typed_by_key<T>(key). "
    "Or use create_typed_node with input_specs for automatic handling."
  );
}

// ============================================================================
// GraphBuilder: Declarative API implementation
// ============================================================================

template <typename... Outs>
std::pair<std::shared_ptr<TypedSource<Outs...>>, tf::Task>
GraphBuilder::create_typed_source(const std::string& name,
                                   std::tuple<Outs...> values,
                                   const std::vector<std::string>& output_keys) {
  auto node = std::make_shared<TypedSource<Outs...>>(std::move(values), output_keys, name);
  auto task = add_typed_source(node);
  return {node, task};
}

// Helper to extract return type from functor
template <typename OpType>
struct FunctorReturnType;

template <typename OpType>
struct FunctorReturnType {
  using type = typename std::invoke_result<OpType, std::tuple<>>::type;
};

// Specialization for functions that take tuple
template <typename... Ins, typename... Outs>
struct FunctorReturnType<std::function<std::tuple<Outs...>(std::tuple<Ins...>)>> {
  using type = std::tuple<Outs...>;
};

template <typename... Ins, typename OpType>
auto GraphBuilder::create_typed_node(const std::string& name,
                                     const std::vector<std::pair<std::string, std::string>>& input_specs,
                                     OpType&& functor,
                                     const std::vector<std::string>& output_keys) {
  if (input_specs.size() != sizeof...(Ins)) {
    throw std::runtime_error("Number of input specifications (" + 
                             std::to_string(input_specs.size()) + 
                             ") must match number of input types (" + 
                             std::to_string(sizeof...(Ins)) + ")");
  }
  
  // Test the functor to get output types
  // Create a test tuple and invoke functor to get return type
  using TestInput = std::tuple<Ins...>;
  using ReturnType = typename std::invoke_result<OpType, TestInput>::type;
  
  // Extract output types from return type (should be tuple<Outs...>)
  static_assert(std::is_same_v<ReturnType, std::tuple<typename std::tuple_element<0, ReturnType>::type>> ||
                (std::tuple_size_v<ReturnType> > 0), 
                "Functor must return std::tuple");
  
  // Extract output types
  using OutsTuple = ReturnType;
  
  // Extract typed futures from source nodes
  std::tuple<std::shared_future<Ins>...> input_futures = 
    [this, &input_specs]<std::size_t... Is>(std::index_sequence<Is...>) {
      return std::make_tuple(
        get_typed_input_impl<std::tuple_element_t<Is, std::tuple<Ins...>>>(
          input_specs[Is].first, 
          input_specs[Is].second
        )...
      );
    }(std::index_sequence_for<Ins...>{});
  
  // Create the node using the extracted output types
  auto result = create_typed_node_impl<Ins...>(name, std::move(input_futures), 
                                               std::forward<OpType>(functor), 
                                               output_keys, 
                                               std::make_index_sequence<std::tuple_size_v<OutsTuple>>{});
  
  // Auto-register dependencies: prefer adapter→target; otherwise source→target
  auto task = result.second;
  for (const auto& [source_node, source_key] : input_specs) {
    const std::string adapter_key = source_node + "::" + source_key;
    auto adapter_it = adapter_tasks_.find(adapter_key);
    if (adapter_it != adapter_tasks_.end()) {
      adapter_it->second.precede(task);
    } else {
      auto source_task_it = tasks_.find(source_node);
      if (source_task_it != tasks_.end()) {
        source_task_it->second.precede(task);
      }
    }
  }
  
  return result;
}

// Implementation helper that knows output types
template <typename... Ins, typename OpType, std::size_t... OutIndices>
auto GraphBuilder::create_typed_node_impl(const std::string& name,
                                          std::tuple<std::shared_future<Ins>...> input_futures,
                                          OpType&& functor,
                                          const std::vector<std::string>& output_keys,
                                          std::index_sequence<OutIndices...>) {
  using ReturnType = typename std::invoke_result<OpType, std::tuple<Ins...>>::type;
  using OutsTuple = ReturnType;
  using NodeType = TypedNode<std::tuple<std::shared_future<Ins>...>, 
                             std::tuple_element_t<OutIndices, OutsTuple>...>;
  
  auto node = std::make_shared<NodeType>(
    std::move(input_futures),
    std::forward<OpType>(functor),
    output_keys,
    name
  );
  
  auto task = add_typed_node(node);
  
  // Note: Dependencies are auto-registered in the caller (create_typed_node)
  // based on input_specs
  
  return std::make_pair(node, task);
}

// Helper to extract typed future from source node
template <typename T>
std::shared_future<T> GraphBuilder::get_typed_input_impl(const std::string& node_name, 
                                                         const std::string& key) const {
  auto node = get_node(node_name);
  if (!node) {
    throw std::runtime_error("Source node not found: " + node_name);
  }
  
  // Get any future from source node
  auto any_fut = node->get_output_future(key);
  
  // Create adapter promise that will extract T from any
  auto p_typed = std::make_shared<std::promise<T>>();
  auto f_typed = p_typed->get_future().share();
  
  // Create an adapter task that extracts T from any
  // This adapter will run after the source node and before any node using this input
  auto source_task_it = tasks_.find(node_name);
  if (source_task_it == tasks_.end()) {
    throw std::runtime_error("Source node task not found: " + node_name);
  }
  auto source_task = source_task_it->second;
  
  // Create adapter task (non-const access to taskflow_ needed)
  auto& tf = const_cast<GraphBuilder*>(this)->taskflow_mutable();
  auto adapter_task = tf.emplace([any_fut, p_typed]() {
    try {
      std::any value = any_fut.get();
      T typed_value = std::any_cast<T>(value);
      p_typed->set_value(std::move(typed_value));
    } catch (const std::bad_any_cast& e) {
      p_typed->set_exception(std::make_exception_ptr(e));
    }
  }).name(node_name + "_to_" + key + "_adapter");
  
  // Schedule adapter after source
  // The node using this future will depend on source, and adapter will complete
  // before the node runs (since adapter depends on source and completes quickly)
  source_task.precede(adapter_task);
  // Register adapter task so that consumers can depend on it explicitly
  adapter_tasks_[node_name + "::" + key] = adapter_task;
  
  return f_typed;
}

// ============================================================================
// PipelineNode template implementation
// ============================================================================

template <typename... Ps>
PipelineNode::PipelineNode(tf::Pipeline<Ps...>& pipeline, const std::string& name)
    : node_name_(name) {
  // This constructor is mainly for compatibility
  // In practice, create_pipeline_node creates the shared_ptr directly
  pipeline_wrapper_ = std::make_shared<tf::Pipeline<Ps...>>(std::move(pipeline));
}

// ============================================================================
// GraphBuilder: create_pipeline_node template implementation
// ============================================================================

template <typename... Ps>
std::pair<std::shared_ptr<PipelineNode>, tf::Task>
GraphBuilder::create_pipeline_node(const std::string& name,
                                   size_t num_lines,
                                   Ps&&... pipes) {
  // Create pipeline with given lines and pipes
  auto pipeline_ptr = std::make_shared<tf::Pipeline<Ps...>>(num_lines, std::forward<Ps>(pipes)...);
  
  // Create task using composed_of first (before moving pipeline)
  auto task = taskflow_.composed_of(*pipeline_ptr).name(name);
  
  // Wrap in PipelineNode and store pipeline reference
  // Note: We store the pipeline_ptr in a way that maintains lifetime
  // PipelineNode will hold a reference to keep it alive
  auto node = std::make_shared<PipelineNode>();
  node->node_name_ = name;
  node->pipeline_wrapper_ = pipeline_ptr;  // Store shared_ptr in any
  
  nodes_[name] = node;
  tasks_[name] = task;
  
  return {node, task};
}

}  // namespace workflow

#endif  // WORKFLOW_NODEFLOW_IMPL_HPP

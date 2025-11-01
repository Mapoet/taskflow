// Implementation file for nodeflow.hpp

#include <workflow/nodeflow.hpp>
#include <stdexcept>
#include <optional>

namespace workflow {

// ============================================================================
// AnyOutputs implementation
// ============================================================================

AnyOutputs::AnyOutputs(const std::vector<std::string>& keys) {
  add(keys);
}

void AnyOutputs::add(const std::string& key) {
  auto p = std::make_shared<std::promise<std::any>>();
  promises[key] = p;
  futures[key] = p->get_future().share();
}

void AnyOutputs::add(const std::vector<std::string>& keys) {
  for (const auto& key : keys) {
    add(key);
  }
}

// ============================================================================
// AnySource implementation
// ============================================================================

AnySource::AnySource(std::unordered_map<std::string, std::any> vals, const std::string& name)
    : values(std::move(vals)), out(extract_keys(values)), node_name_(name.empty() ? "AnySource" : name) {}

std::vector<std::string> AnySource::extract_keys(const std::unordered_map<std::string, std::any>& m) {
  std::vector<std::string> keys;
  keys.reserve(m.size());
  for (const auto& [key, _] : m) {
    keys.push_back(key);
  }
  return keys;
}

std::function<void()> AnySource::functor(const char* node_name) const {
  auto vals = values;
  auto promises = out.promises;
  return [vals, promises, node_name]() mutable {
    for (const auto& [key, val] : vals) {
      auto it = promises.find(key);
      if (it == promises.end()) {
        throw std::runtime_error("Unknown source key: " + key);
      }
      it->second->set_value(val);
    }
    // std::cout << (node_name ? node_name : "AnySource") << " emitted\n";
  };
}

std::shared_future<std::any> AnySource::get_output_future(const std::string& key) const {
  auto it = out.futures.find(key);
  if (it == out.futures.end()) {
    throw std::runtime_error("Unknown output key: " + key);
  }
  return it->second;
}

std::vector<std::string> AnySource::get_output_keys() const {
  std::vector<std::string> keys;
  for (const auto& [key, _] : out.futures) {
    keys.push_back(key);
  }
  return keys;
}

// ============================================================================
// AnyNode implementation
// ============================================================================

AnyNode::AnyNode(std::unordered_map<std::string, std::shared_future<std::any>> fin,
                 const std::vector<std::string>& out_keys,
                 std::function<std::unordered_map<std::string, std::any>(
                     const std::unordered_map<std::string, std::any>&)> fn,
                 const std::string& name)
    : inputs(std::move(fin)), out(out_keys), op(std::move(fn)), node_name_(name.empty() ? "AnyNode" : name) {}

std::function<void()> AnyNode::functor(const char* node_name) const {
  auto fin = inputs;
  auto promises = out.promises;
  auto fn = op;
  return [fin, promises, fn, node_name]() mutable {
    // Collect input values
    std::unordered_map<std::string, std::any> in_vals;
    for (const auto& [key, fut] : fin) {
      in_vals[key] = fut.get();
    }
    // Apply operation
    auto out_vals = fn(in_vals);
    // Set promises
    for (const auto& [key, val] : out_vals) {
      auto it = promises.find(key);
      if (it == promises.end()) {
        throw std::runtime_error("Unknown output key: " + key);
      }
      it->second->set_value(val);
    }
    // std::cout << (node_name ? node_name : "AnyNode") << " done\n";
  };
}

std::shared_future<std::any> AnyNode::get_output_future(const std::string& key) const {
  auto it = out.futures.find(key);
  if (it == out.futures.end()) {
    throw std::runtime_error("Unknown output key: " + key);
  }
  return it->second;
}

std::vector<std::string> AnyNode::get_output_keys() const {
  std::vector<std::string> keys;
  for (const auto& [key, _] : out.futures) {
    keys.push_back(key);
  }
  return keys;
}

// ============================================================================
// AnySink implementation
// ============================================================================

AnySink::AnySink(std::unordered_map<std::string, std::shared_future<std::any>> fin, const std::string& name)
    : inputs(std::move(fin)), node_name_(name.empty() ? "AnySink" : name), callback_(nullptr) {}

AnySink::AnySink(std::unordered_map<std::string, std::shared_future<std::any>> fin,
                 std::function<void(const std::unordered_map<std::string, std::any>&)> callback,
                 const std::string& name)
    : inputs(std::move(fin)), node_name_(name.empty() ? "AnySink" : name), callback_(std::move(callback)) {}

std::function<void()> AnySink::functor(const char* node_name) const {
  auto fin = inputs;
  auto callback = callback_;
  return [fin, callback, node_name]() mutable {
    // Collect values from futures
    std::unordered_map<std::string, std::any> values;
    for (const auto& [key, fut] : fin) {
      values[key] = fut.get();
    }
    
    // Call callback if provided, otherwise use default output
    if (callback) {
      callback(values);
    } else {
      std::cout << (node_name ? node_name : "AnySink") << ": ";
      bool first = true;
      for (const auto& [key, val] : values) {
        if (!first) std::cout << ' ';
        first = false;
        std::cout << key << '=';
        if (val.type() == typeid(double)) {
          std::cout << std::any_cast<double>(val);
        } else if (val.type() == typeid(int)) {
          std::cout << std::any_cast<int>(val);
        } else if (val.type() == typeid(std::string)) {
          std::cout << std::any_cast<std::string>(val);
        } else {
          std::cout << "<" << val.type().name() << ">";
        }
      }
      std::cout << '\n';
    }
  };
}

std::shared_future<std::any> AnySink::get_output_future(const std::string& key) const {
  throw std::runtime_error("AnySink has no outputs");
}

std::vector<std::string> AnySink::get_output_keys() const {
  return {};  // Sink has no outputs
}

// ============================================================================
// Condition Node Implementation
// ============================================================================

ConditionNode::ConditionNode(const std::unordered_map<std::string, std::shared_future<std::any>>& inputs,
                             ConditionFunc func,
                             const std::vector<std::string>& output_keys,
                             const std::string& name)
    : inputs(inputs), out(output_keys), func_(std::move(func)), node_name_(name.empty() ? "ConditionNode" : name) {}

std::string ConditionNode::name() const {
  return node_name_;
}

std::function<void()> ConditionNode::functor(const char* node_name) const {
  auto fin = inputs;
  auto promises = out.promises;
  auto fn = func_;
  return [fin, promises, fn, node_name]() mutable {
    // Collect input values
    std::unordered_map<std::string, std::any> in_vals;
    for (const auto& [key, fut] : fin) {
      in_vals[key] = fut.get();
    }
    // Execute condition function with inputs
    int result = fn(in_vals);
    // Store result as output if output_keys contains "result"
    if (auto it = promises.find("result"); it != promises.end()) {
      it->second->set_value(std::any{result});
    }
  };
}

std::shared_future<std::any> ConditionNode::get_output_future(const std::string& key) const {
  auto it = out.futures.find(key);
  if (it == out.futures.end()) {
    throw std::runtime_error("Unknown output key: " + key);
  }
  return it->second;
}

std::vector<std::string> ConditionNode::get_output_keys() const {
  std::vector<std::string> keys;
  for (const auto& [key, _] : out.futures) {
    keys.push_back(key);
  }
  return keys;
}

// ============================================================================
// Multi-Condition Node Implementation
// ============================================================================

MultiConditionNode::MultiConditionNode(const std::unordered_map<std::string, std::shared_future<std::any>>& inputs,
                                       MultiConditionFunc func,
                                       const std::vector<std::string>& output_keys,
                                       const std::string& name)
    : inputs(inputs), out(output_keys), func_(std::move(func)), node_name_(name.empty() ? "MultiConditionNode" : name) {}

std::string MultiConditionNode::name() const {
  return node_name_;
}

std::function<void()> MultiConditionNode::functor(const char* node_name) const {
  auto fin = inputs;
  auto promises = out.promises;
  auto fn = func_;
  return [fin, promises, fn, node_name]() mutable {
    // Collect input values
    std::unordered_map<std::string, std::any> in_vals;
    for (const auto& [key, fut] : fin) {
      in_vals[key] = fut.get();
    }
    // Execute multi-condition function with inputs
    auto result = fn(in_vals);
    // Store result as output if output_keys contains "result"
    if (auto it = promises.find("result"); it != promises.end()) {
      // Store SmallVector<int> as any
      std::vector<int> result_vec(result.begin(), result.end());
      it->second->set_value(std::any{result_vec});
    }
  };
}

std::shared_future<std::any> MultiConditionNode::get_output_future(const std::string& key) const {
  auto it = out.futures.find(key);
  if (it == out.futures.end()) {
    throw std::runtime_error("Unknown output key: " + key);
  }
  return it->second;
}

std::vector<std::string> MultiConditionNode::get_output_keys() const {
  std::vector<std::string> keys;
  for (const auto& [key, _] : out.futures) {
    keys.push_back(key);
  }
  return keys;
}

// ============================================================================
// Pipeline Node Implementation
// ============================================================================

std::string PipelineNode::name() const {
  return node_name_;
}

std::function<void()> PipelineNode::functor(const char* node_name) const {
  // Pipeline is executed via composed_of, not directly
  return []() {
    // Pipeline execution is handled by Taskflow's composed_of
  };
}

std::shared_future<std::any> PipelineNode::get_output_future(const std::string& key) const {
  // Pipeline nodes don't have outputs in the traditional sense
  throw std::runtime_error("PipelineNode::get_output_future: Pipeline nodes do not have key-based outputs");
}

std::vector<std::string> PipelineNode::get_output_keys() const {
  return {};  // Pipeline nodes don't have key-based outputs
}

// ============================================================================
// Loop Node Implementation
// ============================================================================

LoopNode::LoopNode(const std::unordered_map<std::string, std::shared_future<std::any>>& inputs,
                   std::function<void(const std::unordered_map<std::string, std::any>&)> body_func,
                   LoopConditionFunc condition_func,
                   const std::vector<std::string>& output_keys,
                   const std::string& name)
    : inputs(inputs), out(output_keys), 
      body_func_(std::move(body_func)),
      condition_func_(std::move(condition_func)),
      node_name_(name.empty() ? "LoopNode" : name) {}

std::string LoopNode::name() const {
  return node_name_;
}

std::function<void()> LoopNode::functor(const char* node_name) const {
  // Loop execution is handled via condition task graph, not as a single functor
  return []() {
    // Loop execution is handled via condition task graph
  };
}

std::shared_future<std::any> LoopNode::get_output_future(const std::string& key) const {
  auto it = out.futures.find(key);
  if (it == out.futures.end()) {
    throw std::runtime_error("Unknown output key: " + key);
  }
  return it->second;
}

std::vector<std::string> LoopNode::get_output_keys() const {
  std::vector<std::string> keys;
  for (const auto& [key, _] : out.futures) {
    keys.push_back(key);
  }
  return keys;
}

// ============================================================================
// GraphBuilder implementation
// ============================================================================

GraphBuilder::GraphBuilder(const std::string& name)
    : taskflow_(name),  executor_(nullptr) {}

tf::Task GraphBuilder::add_node(std::shared_ptr<INode> node) {
  if (!node) {
    throw std::runtime_error("Cannot add null node");
  }
  
  std::string node_name = node->name();
  if (node_name.empty()) {
    node_name = "node_" + std::to_string(nodes_.size());
  }
  
  // Check for duplicate names only if we own the Taskflow (not operating on external Subflow)
  // When operating on Subflow, the graph is cleared after each iteration, so duplicates are expected
  // and allowed. The Subflow's graph handles the actual task lifecycle.
  if (nodes_.find(node_name) != nodes_.end()) {
    throw std::runtime_error("Duplicate node name: " + node_name);
  }
  
  nodes_[node_name] = node;
  std::string task_name = node_name;  // Store name as std::string for lambda capture
  auto task = taskflow_.emplace([node, task_name]() {
    // Call the node's functor with the stored name
    node->functor(task_name.c_str())();
  }).name(node_name);
  tasks_[node_name] = task;
  
  return task;
}

tf::Task GraphBuilder::add_any_source(std::shared_ptr<AnySource> node) {
  return add_node(std::static_pointer_cast<INode>(node));
}

tf::Task GraphBuilder::add_any_node(std::shared_ptr<AnyNode> node) {
  return add_node(std::static_pointer_cast<INode>(node));
}

tf::Task GraphBuilder::add_any_sink(std::shared_ptr<AnySink> node) {
  return add_node(std::static_pointer_cast<INode>(node));
}

void GraphBuilder::precede(tf::Task from, tf::Task to) {
  from.precede(to);
}

void GraphBuilder::succeed(tf::Task to, tf::Task from) {
  to.succeed(from);
}

tf::Future<void> GraphBuilder::run_async(tf::Executor& executor) {
  executor_ = &executor;
  return executor.run(taskflow_);
}

void GraphBuilder::run(tf::Executor& executor) {
  run_async(executor).wait();
}

void GraphBuilder::dump(std::ostream& os) const {
  taskflow_.dump(os);
}

std::shared_ptr<INode> GraphBuilder::get_node(const std::string& name) const {
  auto it = nodes_.find(name);
  if (it == nodes_.end()) {
    return nullptr;
  }
  return it->second;
}

std::shared_future<std::any> GraphBuilder::get_output(const std::string& node_name, const std::string& key) const {
  auto node = get_node(node_name);
  if (!node) {
    throw std::runtime_error("Node not found: " + node_name);
  }
  return node->get_output_future(key);
}

// ============================================================================
// Declarative API implementation (non-template parts)
// ============================================================================

std::pair<std::shared_ptr<AnySource>, tf::Task>
GraphBuilder::create_any_source(const std::string& name,
                                std::unordered_map<std::string, std::any> values) {
  auto node = std::make_shared<AnySource>(std::move(values), name);
  auto task = add_any_source(node);
  return {node, task};
}

std::pair<std::shared_ptr<AnyNode>, tf::Task>
GraphBuilder::create_any_node(const std::string& name,
                              const std::vector<std::pair<std::string, std::string>>& input_specs,
                              std::function<std::unordered_map<std::string, std::any>(
                                  const std::unordered_map<std::string, std::any>&)> functor,
                              const std::vector<std::string>& output_keys) {
  // Get any futures from source nodes
  std::unordered_map<std::string, std::shared_future<std::any>> input_futures;
  for (const auto& [source_node, source_key] : input_specs) {
    input_futures[source_key] = get_output(source_node, source_key);
  }
  
  auto node = std::make_shared<AnyNode>(std::move(input_futures), output_keys, std::move(functor), name);
  auto task = add_any_node(node);
  
  // Auto-register dependencies
  for (const auto& [source_node, _] : input_specs) {
    auto source_task_it = tasks_.find(source_node);
    if (source_task_it != tasks_.end()) {
      source_task_it->second.precede(task);
    }
  }
  
  return {node, task};
}

std::pair<std::shared_ptr<AnySink>, tf::Task>
GraphBuilder::create_any_sink(const std::string& name,
                              const std::vector<std::pair<std::string, std::string>>& input_specs) {
  return create_any_sink(name, input_specs, nullptr);
}

std::pair<std::shared_ptr<AnySink>, tf::Task>
GraphBuilder::create_any_sink(const std::string& name,
                              const std::vector<std::pair<std::string, std::string>>& input_specs,
                              std::function<void(const std::unordered_map<std::string, std::any>&)> callback) {
  // Get any futures from source nodes
  std::unordered_map<std::string, std::shared_future<std::any>> input_futures;
  for (const auto& [source_node, source_key] : input_specs) {
    input_futures[source_key] = get_output(source_node, source_key);
  }
  
  auto node = callback 
    ? std::make_shared<AnySink>(std::move(input_futures), std::move(callback), name)
    : std::make_shared<AnySink>(std::move(input_futures), name);
  auto task = add_any_sink(node);
  
  // Auto-register dependencies
  for (const auto& [source_node, _] : input_specs) {
    auto source_task_it = tasks_.find(source_node);
    if (source_task_it != tasks_.end()) {
      source_task_it->second.precede(task);
    }
  }
  
  return {node, task};
}

// ============================================================================
// GraphBuilder: Advanced Control Flow Node Creation (using Declarative API)
// ============================================================================

tf::Task GraphBuilder::create_subgraph(const std::string& name,
                                       const std::function<void(GraphBuilder&)>& builder_fn) {
  // Build a nested graph and keep it alive under this builder
  auto nested = std::make_unique<GraphBuilder>(name);
  if (builder_fn) {
    builder_fn(*nested);
  }
  auto task = taskflow_.composed_of(nested->taskflow()).name(name);
  subgraph_builders_.push_back(std::move(nested));  // keep lifetime
  return task;
}

std::pair<std::shared_ptr<AnyNode>, tf::Task>
GraphBuilder::create_subgraph(const std::string& name,
                             const std::vector<std::pair<std::string, std::string>>& input_specs,
                             std::function<void(GraphBuilder&, const std::unordered_map<std::string, std::any>&)> builder_fn,
                             const std::vector<std::string>& output_keys) {
  // Get any futures from source nodes
  std::unordered_map<std::string, std::shared_future<std::any>> input_futures;
  for (const auto& [source_node, source_key] : input_specs) {
    input_futures[source_key] = get_output(source_node, source_key);
  }
  
  // Build nested graph with inputs
  auto nested = std::make_unique<GraphBuilder>(name);
  if (builder_fn) {
    // We need to get the values from futures synchronously, but this is tricky
    // For now, we'll pass empty map and handle it in the builder function
    // Better approach: store futures and extract in the composed_of task
    std::unordered_map<std::string, std::any> input_vals;
    for (const auto& [key, fut] : input_futures) {
      // This is problematic - we can't get() here as futures may not be ready
      // We'll need to defer this until execution time
      input_vals[key] = std::any{};  // Placeholder
    }
    builder_fn(*nested, input_vals);
  }
  
  // Create an AnyNode wrapper that will collect outputs from the subgraph
  // The subgraph should expose its outputs via nodes
  auto node = std::make_shared<AnyNode>(
    input_futures,
    output_keys,
    [nested_ptr = nested.get(), output_keys](const std::unordered_map<std::string, std::any>& inputs) {
      // Execute the nested graph and collect outputs
      // This is complex - we need to track which nodes produce the outputs
      // For now, return empty map - this needs more sophisticated implementation
      std::unordered_map<std::string, std::any> outputs;
      for (const auto& key : output_keys) {
        outputs[key] = std::any{};
      }
      return outputs;
    },
    name
  );
  
  auto task = taskflow_.composed_of(nested->taskflow()).name(name);
  subgraph_builders_.push_back(std::move(nested));  // keep lifetime
  
  // Auto-register dependencies
  for (const auto& [source_node, _] : input_specs) {
    auto source_task_it = tasks_.find(source_node);
    if (source_task_it != tasks_.end()) {
      source_task_it->second.precede(task);
    }
  }
  
  nodes_[name] = node;
  tasks_[name] = task;
  
  return {node, task};
}

tf::Task GraphBuilder::create_subtask(const std::string& name,
                                      const std::function<void(GraphBuilder&)>& builder_fn) {
  auto task = taskflow_.emplace([this, builder_fn, name]() mutable {
    if (executor_ == nullptr) {
      throw std::runtime_error("create_subtask requires GraphBuilder::run or run_async to set executor");
    }
    GraphBuilder nested{name};
    if (builder_fn) {
      builder_fn(nested);
    }
    // Run the nested subgraph synchronously on the same executor
    executor_->corun(nested.taskflow());
  }).name(name);
  return task;
}

std::pair<std::shared_ptr<AnyNode>, tf::Task>
GraphBuilder::create_subtask(const std::string& name,
                            const std::vector<std::pair<std::string, std::string>>& input_specs,
                            std::function<void(GraphBuilder&, const std::unordered_map<std::string, std::any>&)> builder_fn,
                            const std::vector<std::string>& output_keys) {
  // Get any futures from source nodes
  std::unordered_map<std::string, std::shared_future<std::any>> input_futures;
  for (const auto& [source_node, source_key] : input_specs) {
    input_futures[source_key] = get_output(source_node, source_key);
  }
  
  // Create an AnyNode wrapper
  auto node = std::make_shared<AnyNode>(
    input_futures,
    output_keys,
    [this, builder_fn, name, output_keys](const std::unordered_map<std::string, std::any>& inputs) {
      // Build and run subgraph at execution time
      if (executor_ == nullptr) {
        throw std::runtime_error("create_subtask requires GraphBuilder::run or run_async to set executor");
      }
      GraphBuilder nested{name};
      if (builder_fn) {
        builder_fn(nested, inputs);
      }
      // Run the nested subgraph synchronously
      executor_->corun(nested.taskflow());
      
      // Collect outputs from the nested graph
      // This is complex - we need to track which nodes produce the outputs
      // For now, return empty map
      std::unordered_map<std::string, std::any> outputs;
      for (const auto& key : output_keys) {
        outputs[key] = std::any{};
      }
      return outputs;
    },
    name
  );
  
  auto task = add_any_node(node);
  
  // Auto-register dependencies
  for (const auto& [source_node, _] : input_specs) {
    auto source_task_it = tasks_.find(source_node);
    if (source_task_it != tasks_.end()) {
      source_task_it->second.precede(task);
    }
  }
  
  return {node, task};
}

std::pair<std::shared_ptr<ConditionNode>, tf::Task>
GraphBuilder::create_condition_decl(const std::string& name,
                                    const std::vector<std::pair<std::string, std::string>>& input_specs,
                                    std::function<int(const std::unordered_map<std::string, std::any>&)> condition_func,
                                    const std::vector<tf::Task>& successors,
                                    const std::vector<std::string>& output_keys) {
  // Get any futures from source nodes
  std::unordered_map<std::string, std::shared_future<std::any>> input_futures;
  for (const auto& [source_node, source_key] : input_specs) {
    input_futures[source_key] = get_output(source_node, source_key);
  }
  
  auto node = std::make_shared<ConditionNode>(input_futures, std::move(condition_func), output_keys, name);
  
  // Create condition task
  auto cond_task = taskflow_.emplace([fin = input_futures, fn = node->func_, promises = node->out.promises]() mutable {
    std::unordered_map<std::string, std::any> in_vals;
    for (const auto& [key, fut] : fin) {
      in_vals[key] = fut.get();
    }
    int result = fn(in_vals);
    if (auto it = promises.find("result"); it != promises.end()) {
      it->second->set_value(std::any{result});
    }
    return result;
  }).name(name);
  
  nodes_[name] = node;
  tasks_[name] = cond_task;
  
  // Auto-register dependencies
  for (const auto& [source_node, _] : input_specs) {
    auto source_task_it = tasks_.find(source_node);
    if (source_task_it != tasks_.end()) {
      source_task_it->second.precede(cond_task);
    }
  }
  
  // Wire successors explicitly
  if (!successors.empty()) {
    for (const auto& s : successors) {
      cond_task.precede(s);
    }
  }
  
  return {node, cond_task};
}

std::pair<std::shared_ptr<MultiConditionNode>, tf::Task>
GraphBuilder::create_multi_condition_decl(const std::string& name,
                                          const std::vector<std::pair<std::string, std::string>>& input_specs,
                                          std::function<tf::SmallVector<int>(const std::unordered_map<std::string, std::any>&)> func,
                                          const std::vector<tf::Task>& successors,
                                          const std::vector<std::string>& output_keys) {
  // Get any futures from source nodes
  std::unordered_map<std::string, std::shared_future<std::any>> input_futures;
  for (const auto& [source_node, source_key] : input_specs) {
    input_futures[source_key] = get_output(source_node, source_key);
  }
  
  auto node = std::make_shared<MultiConditionNode>(input_futures, std::move(func), output_keys, name);
  
  // Create multi-condition task
  auto cond_task = taskflow_.emplace([fin = input_futures, fn = node->func_, promises = node->out.promises]() mutable {
    std::unordered_map<std::string, std::any> in_vals;
    for (const auto& [key, fut] : fin) {
      in_vals[key] = fut.get();
    }
    auto result = fn(in_vals);
    if (auto it = promises.find("result"); it != promises.end()) {
      std::vector<int> result_vec(result.begin(), result.end());
      it->second->set_value(std::any{result_vec});
    }
    return result;
  }).name(name);
  
  nodes_[name] = node;
  tasks_[name] = cond_task;
  
  // Auto-register dependencies
  for (const auto& [source_node, _] : input_specs) {
    auto source_task_it = tasks_.find(source_node);
    if (source_task_it != tasks_.end()) {
      source_task_it->second.precede(cond_task);
    }
  }
  
  // Wire successors explicitly
  if (!successors.empty()) {
    for (const auto& s : successors) {
      cond_task.precede(s);
    }
  }
  
  return {node, cond_task};
}

std::pair<std::shared_ptr<LoopNode>, tf::Task>
GraphBuilder::create_loop_decl(const std::string& name,
                               const std::vector<std::pair<std::string, std::string>>& input_specs,
                               std::function<void(GraphBuilder&, const std::unordered_map<std::string, std::any>&)> body_builder_fn,
                               std::function<int(const std::unordered_map<std::string, std::any>&)> condition_func,
                               std::function<void(GraphBuilder&, const std::unordered_map<std::string, std::any>&)> exit_builder_fn,
                               const std::vector<std::string>& output_keys) {
  // Get any futures from source nodes (used for both body and condition)
  std::unordered_map<std::string, std::shared_future<std::any>> input_futures;
  for (const auto& [source_node, source_key] : input_specs) {
    input_futures[source_key] = get_output(source_node, source_key);
  }
  
  // Create the loop node (stores condition function and output promises)
  auto node = std::make_shared<LoopNode>(input_futures, 
                                         std::function<void(const std::unordered_map<std::string, std::any>&)>{}, 
                                         std::move(condition_func), 
                                         output_keys, 
                                         name);
  
  // Create body task following master's create_subtask pattern:
  // - Use taskflow_.emplace() to create regular task (not Subflow)
  // - Inside task, create fresh GraphBuilder, build subgraph, and run synchronously
  // This allows the task to be re-scheduled multiple times in the loop
  // CRITICAL: Store body_task reference early (like master) so it can be found when setting dependencies
  tf::Task body_task = taskflow_.emplace([this, body_builder_fn, name, fin = input_futures]() mutable {
    static int call_count = 0;
    call_count++;
    std::cout << "[DEBUG] Body task call #" << call_count << "\n";
    
    if (executor_ == nullptr) {
      throw std::runtime_error("Loop body task requires executor to be set. Call run() or run_async() before creating loops.");
    }
    
    // Extract input values from futures at execution time
    std::unordered_map<std::string, std::any> in_vals;
    for (const auto& [key, fut] : fin) {
      in_vals[key] = fut.get();
    }
    
    if (body_builder_fn) {
      // Create a fresh Taskflow for this iteration
      // Use stack allocation like master's create_subtask, not unique_ptr
      // The taskflow inside will be used by executor, which keeps it alive during execution
      auto nested = std::make_unique<GraphBuilder>(name + "_body");
      body_builder_fn(*nested, in_vals);
      
      // Run the nested subgraph synchronously (master's approach)
      //executor_->run(nested.taskflow()).wait();
      executor_->corun(nested->taskflow());
      //taskflow_.composed_of(nested->taskflow());
      subgraph_builders_.push_back(std::move(nested));
    }
  }).name(name + "_body");
  
  // CRITICAL DIFFERENCE from master: Store body_task in tasks_ immediately after creation
  // This ensures it can be found when setting Input -> body_task dependency
  // In master version, body_task is created via create_subtask which already stores it
  tasks_[name + "_body"] = body_task;
  
  // Create exit task if provided (same pattern as body_task)
  std::optional<tf::Task> exit_task;
  if (exit_builder_fn) {
    exit_task = taskflow_.emplace([this, exit_builder_fn, name, fin = input_futures]() mutable {
      if (executor_ == nullptr) {
        throw std::runtime_error("Loop exit task requires executor to be set.");
      }
      // Extract input values from futures
      std::unordered_map<std::string, std::any> inputs;
      for (const auto& [key, fut] : fin) {
        inputs[key] = fut.get();
      }
      // Build exit subgraph in a fresh Taskflow
      // Use stack allocation like master's create_subtask
      auto nested = std::make_unique<GraphBuilder>(name + "_exit");
      exit_builder_fn(*nested, inputs);
      // Run synchronously like body task
      //executor_->run(nested.taskflow()).wait();
      executor_->corun(nested->taskflow());
      subgraph_builders_.push_back(std::move(nested));
    }).name(name + "_exit");
  }
  
  // Create condition task (returns int: 0 = continue loop, 1 = exit)
  // CRITICAL: Condition task should be simple like master - just execute the function
  // The function already has access to all needed state via closures (like master's counter)
  // Extracting from futures here may cause issues if futures are shared across iterations
  tf::Task cond_task = taskflow_.emplace([fin = input_futures, fn = node->condition_func_, promises = node->out.promises]() mutable -> int {
    static int cond_call_count = 0;
    cond_call_count++;
    // Extract input values from futures
    std::unordered_map<std::string, std::any> in_vals;
    for (const auto& [key, fut] : fin) {
      in_vals[key] = fut.get();
    }
    int result = fn(in_vals);
    std::cout << "[DEBUG] Condition task call #" << cond_call_count << ", result=" << result << "\n";
    // Store result as output if output_keys contains "result"
    if (auto it = promises.find("result"); it != promises.end()) {
      it->second->set_value(std::any{result});
    }
    return result;
  }).name(name + "_condition");
  
  // Wire loop structure following master pattern:
  // body_task -> cond_task -> (body_task if 0, exit_task if 1)
  // When condition returns 0, Taskflow resets body_task's join_counter and schedules it again
  // 
  // CRITICAL: Wire the loop structure FIRST, then add initial dependencies (like master)
  // This ensures the loop structure is complete before adding external dependencies
  body_task.precede(cond_task);
  
  // Master pattern: cond_task.precede(body_task, exit_task)
  // Index 0 = body_task (continue loop), Index 1 = exit_task (exit loop)
  // If exit_task is empty tf::Task{}, condition returning 1 schedules nothing (ends loop)
  tf::Task final_exit = exit_task.has_value() ? *exit_task : tf::Task{};
  cond_task.precede(body_task, final_exit);
  
  // Add initial dependencies from input_specs AFTER wiring the loop (master pattern)
  // Master checks: it->second != body_task to avoid circular dependency
  // 
  // CRITICAL DISCOVERY: In loops, having Input -> body_task creates a dependency where
  // body_task has 2 predecessors: Input and cond_task. When body_task completes, its
  // join_counter is reset to num_predecessors() = 2. When cond_task returns 0, it sets
  // body_task's join_counter to 0, but this might not work correctly if Input is still
  // a predecessor.
  //
  // SOLUTION: Don't add Input -> body_task dependency. Instead, let the first iteration
  // be triggered by manually calling body_task, or use a different mechanism.
  // Actually, looking at master version, it DOES add Input -> body_task, so this isn't the issue.
  //
  // Let me check if the issue is that Input dependency needs to be "dynamic" - only
  // applicable for the first iteration. But Taskflow doesn't support dynamic dependencies.
  //
  // Alternative: Maybe we should NOT add Input -> body_task at all, and instead make
  // the loop start differently. But master version does add it, so that's not it either.
  //
  // Wait - maybe the issue is that Input -> body_task is added BEFORE cond_task.precede,
  // so the order matters? We changed it to AFTER, but still have the issue.
  //
  // Let me try: In the first body_task execution, check if we need to "consume" the Input
  // dependency, then it won't be required for subsequent iterations.
  // But Taskflow doesn't have a mechanism to remove dependencies at runtime.
  //
  // KEY INSIGHT from debugging: body_task executes 2 times, then stops even though
  // cond_task returns 0. This suggests that after 2 iterations, something prevents
  // body_task from being rescheduled. 
  //
  // Possible causes:
  // 1. join_counter reset issue with multiple predecessors
  // 2. Topology completion - maybe executor thinks the graph is done?
  // 3. Input dependency preventing rescheduling even after cond_task sets join_counter to 0
  //
  // Let's keep the master pattern for now - add Input dependency but after loop wiring
  for (const auto& [source_node, _] : input_specs) {
    auto source_task_it = tasks_.find(source_node);
    if (source_task_it != tasks_.end() && source_task_it->second != body_task) {
      // Only body depends on predecessors; condition is triggered by body
      source_task_it->second.precede(body_task);
    }
  }
  
  // Store the body task as the main task (entry point of loop)
  nodes_[name] = node;
  tasks_[name] = body_task;
  
  return {node, body_task};
}

// Master-style create_loop_decl: accepts pre-created body_task
tf::Task GraphBuilder::create_loop_decl(const std::string& name,
                                        tf::Task& body_task,
                                        std::function<int()> condition_func,
                                        tf::Task exit_task) {
  // Create the condition controller
  auto cond_task = taskflow_.emplace(std::move(condition_func)).name(name);
  // Wire loop: body -> cond
  // cond returns 0 for loop-back (body), non-zero for exit
  // Use separate precede calls (like examples/condition.cpp)
  // Return 0 executes first precede (body), return 1 executes second precede (exit)
  body_task.precede(cond_task);
  cond_task.precede(body_task, exit_task);  // Index 0: continue loop
  tasks_[name] = cond_task;
  return cond_task;
}

// Master-style create_loop_decl with depend_on_nodes
tf::Task GraphBuilder::create_loop_decl(const std::string& name,
                                        const std::vector<std::string>& depend_on_nodes,
                                        tf::Task& body_task,
                                        std::function<int()> condition_func,
                                        tf::Task exit_task) {
  auto cond_task = create_loop_decl(name, body_task, std::move(condition_func), exit_task);
  for (const auto& n : depend_on_nodes) {
    auto it = tasks_.find(n);
    if(it != tasks_.end() && it->second != body_task) {
      // Only body depends on predecessors; condition is triggered by body
      it->second.precede(body_task);
    }
  }
  return cond_task;
}

// Deprecated precede/succeed methods are implemented inline in nodeflow_impl.hpp

}  // namespace workflow


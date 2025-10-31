// Implementation file for nodeflow.hpp

#include <workflow/nodeflow.hpp>
#include <stdexcept>

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

ConditionNode::ConditionNode(ConditionFunc func, const std::string& name)
    : func_(std::move(func)), node_name_(name) {}

std::string ConditionNode::name() const {
  return node_name_;
}

std::function<void()> ConditionNode::functor(const char* node_name) const {
  return [this, node_name]() {
    if (func_) {
      func_();  // Condition task will be created via emplace
    }
  };
}

std::shared_future<std::any> ConditionNode::get_output_future(const std::string& key) const {
  // Condition nodes don't have outputs
  throw std::runtime_error("ConditionNode::get_output_future: Condition nodes do not have outputs");
}

std::vector<std::string> ConditionNode::get_output_keys() const {
  return {};  // Condition nodes don't have outputs
}

// ============================================================================
// Multi-Condition Node Implementation
// ============================================================================

MultiConditionNode::MultiConditionNode(MultiConditionFunc func, const std::string& name)
    : func_(std::move(func)), node_name_(name) {}

std::string MultiConditionNode::name() const {
  return node_name_;
}

std::function<void()> MultiConditionNode::functor(const char* node_name) const {
  return [this, node_name]() {
    if (func_) {
      func_();  // Multi-condition task will be created via emplace
    }
  };
}

std::shared_future<std::any> MultiConditionNode::get_output_future(const std::string& key) const {
  // Multi-condition nodes don't have outputs
  throw std::runtime_error("MultiConditionNode::get_output_future: Multi-condition nodes do not have outputs");
}

std::vector<std::string> MultiConditionNode::get_output_keys() const {
  return {};  // Multi-condition nodes don't have outputs
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

LoopNode::LoopNode(std::function<void()> body_func,
                   LoopConditionFunc condition_func,
                   const std::string& name)
    : body_func_(std::move(body_func)),
      condition_func_(std::move(condition_func)),
      node_name_(name) {}

std::string LoopNode::name() const {
  return node_name_;
}

std::function<void()> LoopNode::functor(const char* node_name) const {
  // Loop is constructed using condition tasks, not as a single functor
  return []() {
    // Loop execution is handled via condition task graph
  };
}

std::shared_future<std::any> LoopNode::get_output_future(const std::string& key) const {
  // Loop nodes don't have outputs
  throw std::runtime_error("LoopNode::get_output_future: Loop nodes do not have outputs");
}

std::vector<std::string> LoopNode::get_output_keys() const {
  return {};  // Loop nodes don't have outputs
}

// ============================================================================
// GraphBuilder implementation
// ============================================================================

GraphBuilder::GraphBuilder(const std::string& name)
    : taskflow_(name), executor_(nullptr) {}

tf::Task GraphBuilder::add_node(std::shared_ptr<INode> node) {
  if (!node) {
    throw std::runtime_error("Cannot add null node");
  }
  
  std::string node_name = node->name();
  if (node_name.empty()) {
    node_name = "node_" + std::to_string(nodes_.size());
  }
  
  // Check for duplicate names
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
// GraphBuilder: Advanced Control Flow Node Creation
// ============================================================================

std::pair<std::shared_ptr<ConditionNode>, tf::Task>
GraphBuilder::create_condition_node(const std::string& name,
                                    std::function<int()> condition_func) {
  auto node = std::make_shared<ConditionNode>(std::move(condition_func), name);
  
  // Create condition task directly using Taskflow's emplace
  auto task = taskflow_.emplace([func = node->func_]() {
    return func();
  }).name(name);
  
  nodes_[name] = node;
  tasks_[name] = task;
  
  return {node, task};
}

std::pair<std::shared_ptr<MultiConditionNode>, tf::Task>
GraphBuilder::create_multi_condition_node(const std::string& name,
                                          std::function<tf::SmallVector<int>()> multi_condition_func) {
  auto node = std::make_shared<MultiConditionNode>(std::move(multi_condition_func), name);
  
  // Create multi-condition task directly using Taskflow's emplace
  auto task = taskflow_.emplace([func = node->func_]() {
    return func();
  }).name(name);
  
  nodes_[name] = node;
  tasks_[name] = task;
  
  return {node, task};
}

std::pair<std::shared_ptr<LoopNode>, tf::Task>
GraphBuilder::create_loop_node(const std::string& name,
                               std::function<void()> body_func,
                               std::function<int()> condition_func) {
  auto node = std::make_shared<LoopNode>(std::move(body_func), std::move(condition_func), name);
  
  // Create loop structure: body task -> condition task -> (loop back to body if 0, or exit)
  auto body_task = taskflow_.emplace(node->body_func_).name(name + "_body");
  auto cond_task = taskflow_.emplace([func = node->condition_func_]() {
    return func();
  }).name(name + "_condition");
  
  // Loop structure: 
  // - body -> condition
  // - condition -> body (if returns 0, continue loop)
  // - condition -> (exit) (if returns non-zero)
  body_task.precede(cond_task);
  // Loop back on index 0
  cond_task.precede(body_task);
  
  // Store the body task as the main task (entry point of loop)
  nodes_[name] = node;
  tasks_[name] = body_task;
  
  // Store both tasks in node for reference (for loop closure)
  // Note: This requires exposing internal tasks, or the user manages the loop manually
  
  return {node, body_task};
}

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
    executor_->run(nested.taskflow()).wait();
  }).name(name);
  return task;
}

tf::Task GraphBuilder::create_condition_decl(const std::string& name,
                                             std::function<int()> condition_func,
                                             const std::vector<tf::Task>& successors) {
  auto task = taskflow_.emplace(std::move(condition_func)).name(name);
  // Wire successors explicitly for clear DOT edges
  if (!successors.empty()) {
    // Expand precede connections
    for (const auto& s : successors) {
      task.precede(s);
    }
  }
  tasks_[name] = task;
  return task;
}

tf::Task GraphBuilder::create_condition_decl(const std::string& name,
                                             const std::vector<std::string>& depend_on_nodes,
                                             std::function<int()> condition_func,
                                             const std::vector<tf::Task>& successors) {
  auto task = create_condition_decl(name, std::move(condition_func), successors);
  for (const auto& n : depend_on_nodes) {
    auto it = tasks_.find(n);
    if (it != tasks_.end()) {
      it->second.precede(task);
    }
  }
  return task;
}

tf::Task GraphBuilder::create_multi_condition_decl(const std::string& name,
                                                   std::function<tf::SmallVector<int>()> func,
                                                   const std::vector<tf::Task>& successors) {
  auto task = taskflow_.emplace(std::move(func)).name(name);
  if (!successors.empty()) {
    for (const auto& s : successors) {
      task.precede(s);
    }
  }
  tasks_[name] = task;
  return task;
}

tf::Task GraphBuilder::create_multi_condition_decl(const std::string& name,
                                                   const std::vector<std::string>& depend_on_nodes,
                                                   std::function<tf::SmallVector<int>()> func,
                                                   const std::vector<tf::Task>& successors) {
  auto task = create_multi_condition_decl(name, std::move(func), successors);
  for (const auto& n : depend_on_nodes) {
    auto it = tasks_.find(n);
    if (it != tasks_.end()) {
      it->second.precede(task);
    }
  }
  return task;
}

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
  cond_task.precede(body_task,exit_task);  // Index 0: continue loop
  tasks_[name] = cond_task;
  return cond_task;
}

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


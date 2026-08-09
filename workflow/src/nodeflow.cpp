// Implementation file for nodeflow.hpp

#include <workflow/nodeflow.hpp>
#include <algorithm>
#include <stdexcept>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace workflow {

namespace {

std::atomic_uint64_t next_runtime_id {1};

std::string make_runtime_id(const char* prefix) {
  return std::string(prefix) + "-" + std::to_string(next_runtime_id.fetch_add(1));
}

const char* loop_status_name(LoopStatus status) {
  switch (status) {
    case LoopStatus::Running: return "running";
    case LoopStatus::Completed: return "completed";
    case LoopStatus::MaxIterations: return "max_iterations";
    case LoopStatus::Cancelled: return "cancelled";
    case LoopStatus::DeadlineExceeded: return "deadline_exceeded";
    case LoopStatus::BodyError: return "body_error";
    case LoopStatus::ConditionError: return "condition_error";
    case LoopStatus::ExitError: return "exit_error";
  }
  return "unknown";
}

}  // namespace

bool RunContext::cancelled() const {
  return cancel_requested && cancel_requested->load();
}

bool RunContext::deadline_exceeded() const {
  return deadline && std::chrono::steady_clock::now() >= *deadline;
}

RunContext RunContext::child(const std::string& child_id, std::size_t child_attempt) const {
  if (depth >= max_depth) {
    throw std::runtime_error("Maximum subflow depth exceeded at " + child_id);
  }
  RunContext result = *this;
  result.parent_run_id = run_id;
  result.run_id = make_runtime_id("run");
  result.subtask_id = child_id;
  result.attempt = child_attempt;
  result.iteration = 0;
  result.depth = depth + 1;
  return result;
}

SubflowModule::SubflowModule(SubflowBuilder builder, SubflowOptions options)
    : builder_(std::move(builder)), options_(std::move(options)) {
  if (!builder_) {
    throw std::invalid_argument("SubflowModule requires a builder");
  }
}

const SubflowBuilder& SubflowModule::builder() const { return builder_; }
const SubflowOptions& SubflowModule::options() const { return options_; }

// ============================================================================
// AnyOutputs implementation
// ============================================================================

void AnyValueSlot::publish(std::any value) {
  std::lock_guard<std::mutex> lock(mutex_);
  value_ = std::move(value);
  ++generation_;
  ready_ = true;
}

std::any AnyValueSlot::read() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_) {
    throw std::logic_error("AnyValueSlot read before publication");
  }
  return value_;
}

bool AnyValueSlot::ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ready_;
}

std::uint64_t AnyValueSlot::generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

AnyOutputs::AnyOutputs(const std::vector<std::string>& keys) {
  add(keys);
}

void AnyOutputs::add(const std::string& key) {
  if (slots.find(key) != slots.end()) {
    throw std::logic_error("Duplicate output key: " + key);
  }
  auto p = std::make_shared<std::promise<std::any>>();
  promises[key] = p;
  futures[key] = p->get_future().share();
  slots[key] = std::make_shared<AnyValueSlot>();
  first_publication[key] = std::make_shared<std::atomic_bool>(false);
}

void AnyOutputs::publish(const std::string& key, std::any value) const {
  auto slot_it = slots.find(key);
  if (slot_it == slots.end()) {
    throw std::runtime_error("Unknown output key: " + key);
  }
  slot_it->second->publish(value);

  auto first_it = first_publication.find(key);
  bool expected = false;
  if (first_it != first_publication.end() &&
      first_it->second->compare_exchange_strong(expected, true)) {
    promises.at(key)->set_value(std::move(value));
  }
}

std::shared_ptr<AnyValueSlot> AnyOutputs::slot(const std::string& key) const {
  auto it = slots.find(key);
  if (it == slots.end()) {
    throw std::runtime_error("Unknown output key: " + key);
  }
  return it->second;
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
  std::unordered_map<std::string, std::any> vals;
  {
    std::lock_guard<std::mutex> lock(values_mutex_);
    vals = values;
  }
  auto outputs = out;
  return [vals, outputs, node_name]() mutable {
    for (const auto& [key, val] : vals) {
      outputs.publish(key, val);
    }
    // std::cout << (node_name ? node_name : "AnySource") << " emitted\n";
  };
}

void AnySource::set_values(std::unordered_map<std::string, std::any> vals) {
  std::lock_guard<std::mutex> lock(values_mutex_);
  if(vals.size() != values.size()) {
    throw std::invalid_argument("AnySource values must preserve its output keys");
  }
  for(const auto& [key, _] : values) {
    if(!vals.contains(key)) {
      throw std::invalid_argument("AnySource values must preserve its output keys");
    }
  }
  values = std::move(vals);
}

std::shared_ptr<AnyValueSlot> AnySource::get_output_slot(const std::string& key) const {
  return out.slot(key);
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

AnyNode::AnyNode(std::unordered_map<std::string, std::shared_ptr<AnyValueSlot>> fin,
                 const std::vector<std::string>& out_keys,
                 std::function<std::unordered_map<std::string, std::any>(
                     const std::unordered_map<std::string, std::any>&)> fn,
                 const std::string& name)
    : slot_inputs(std::move(fin)), out(out_keys), op(std::move(fn)),
      node_name_(name.empty() ? "AnyNode" : name) {}

std::function<void()> AnyNode::functor(const char* node_name) const {
  auto fin = inputs;
  auto slots = slot_inputs;
  auto outputs = out;
  auto fn = op;
  return [fin, slots, outputs, fn, node_name]() mutable {
    // Collect input values
    std::unordered_map<std::string, std::any> in_vals;
    if (!slots.empty()) {
      for (const auto& [key, slot] : slots) {
        in_vals[key] = slot->read();
      }
    } else {
      for (const auto& [key, fut] : fin) {
        in_vals[key] = fut.get();
      }
    }
    // Apply operation
    auto out_vals = fn(in_vals);
    // Set promises
    for (const auto& [key, val] : out_vals) {
      outputs.publish(key, val);
    }
    // std::cout << (node_name ? node_name : "AnyNode") << " done\n";
  };
}

std::shared_ptr<AnyValueSlot> AnyNode::get_output_slot(const std::string& key) const {
  return out.slot(key);
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

AnySink::AnySink(std::unordered_map<std::string, std::shared_ptr<AnyValueSlot>> fin,
                 std::function<void(const std::unordered_map<std::string, std::any>&)> callback,
                 const std::string& name)
    : slot_inputs(std::move(fin)), node_name_(name.empty() ? "AnySink" : name),
      callback_(std::move(callback)) {}

std::function<void()> AnySink::functor(const char* node_name) const {
  auto fin = inputs;
  auto slots = slot_inputs;
  auto callback = callback_;
  return [fin, slots, callback, node_name]() mutable {
    // Collect values from futures
    std::unordered_map<std::string, std::any> values;
    if (!slots.empty()) {
      for (const auto& [key, slot] : slots) {
        values[key] = slot->read();
      }
    } else {
      for (const auto& [key, fut] : fin) {
        values[key] = fut.get();
      }
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

std::shared_ptr<AnyValueSlot> LoopNode::get_output_slot(const std::string& key) const {
  return out.slot(key);
}

LoopResult LoopNode::last_result() const {
  std::lock_guard<std::mutex> lock(result_mutex_);
  return last_result_;
}

// ============================================================================
// GraphBuilder implementation
// ============================================================================

GraphBuilder::GraphBuilder(const std::string& name)
    : taskflow_(name), executor_(nullptr), running_(std::make_shared<std::atomic_bool>(false)),
      context_mutex_(std::make_shared<std::mutex>()) {}

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
  bool expected = false;
  if (!running_->compare_exchange_strong(expected, true)) {
    throw std::logic_error("GraphBuilder does not support overlapping runs of the same graph");
  }
  executor_ = &executor;
  {
    std::lock_guard<std::mutex> lock(*context_mutex_);
    run_context_.run_id = make_runtime_id("run");
    run_context_.parent_run_id.clear();
    run_context_.subtask_id.clear();
    run_context_.attempt = 0;
    run_context_.iteration = 0;
    run_context_.depth = 0;
  }
  try {
    return executor.run(taskflow_, [running = running_]() {
      running->store(false);
    });
  } catch (...) {
    running_->store(false);
    throw;
  }
}

void GraphBuilder::run(tf::Executor& executor) {
  run_async(executor).get();
}

void GraphBuilder::configure_run_context(
    std::shared_ptr<std::atomic_bool> cancel_requested,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    std::size_t max_depth) {
  if (running_->load()) {
    throw std::logic_error("Cannot change run context while the graph is running");
  }
  std::lock_guard<std::mutex> lock(*context_mutex_);
  run_context_.cancel_requested = cancel_requested
    ? std::move(cancel_requested) : std::make_shared<std::atomic_bool>(false);
  run_context_.deadline = deadline;
  run_context_.max_depth = max_depth;
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

std::any GraphBuilder::get_latest_output(const std::string& node_name, const std::string& key) const {
  auto node = get_node(node_name);
  if (!node) {
    throw std::runtime_error("Node not found: " + node_name);
  }
  auto slot = node->get_output_slot(key);
  if (!slot) {
    throw std::logic_error("Node does not expose a reusable output slot: " + node_name + "." + key);
  }
  return slot->read();
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
  std::unordered_map<std::string, std::shared_ptr<AnyValueSlot>> input_slots;
  for (const auto& [source_node, source_key] : input_specs) {
    auto source = get_node(source_node);
    if (!source) {
      throw std::runtime_error("Node not found: " + source_node);
    }
    auto slot = source->get_output_slot(source_key);
    if (!slot) {
      throw std::logic_error("Source node does not expose a reusable output slot: " + source_node);
    }
    input_slots[source_key] = std::move(slot);
  }
  
  auto node = std::make_shared<AnyNode>(std::move(input_slots), output_keys, std::move(functor), name);
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
  std::unordered_map<std::string, std::shared_ptr<AnyValueSlot>> input_slots;
  for (const auto& [source_node, source_key] : input_specs) {
    auto source = get_node(source_node);
    if (!source) {
      throw std::runtime_error("Node not found: " + source_node);
    }
    auto slot = source->get_output_slot(source_key);
    if (!slot) {
      throw std::logic_error("Source node does not expose a reusable output slot: " + source_node);
    }
    input_slots[source_key] = std::move(slot);
  }
  
  auto node = callback 
    ? std::make_shared<AnySink>(std::move(input_slots), std::move(callback), name)
    : std::make_shared<AnySink>(std::move(input_slots),
        std::function<void(const std::unordered_map<std::string, std::any>&)>{}, name);
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
  (void)input_specs;
  (void)builder_fn;
  (void)output_keys;
  throw std::logic_error(
    "Keyed create_subgraph is legacy and cannot bind outputs safely: " + name +
    ". Use create_subgraph_module with explicit OutputBindings.");
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
  (void)input_specs;
  (void)builder_fn;
  (void)output_keys;
  throw std::logic_error(
    "Keyed create_subtask is legacy and cannot bind outputs safely: " + name +
    ". Use create_subtask_module with explicit OutputBindings.");
}

ValueMap GraphBuilder::execute_subflow(const std::string& name,
                                       const ValueMap& inputs,
                                       const SubflowBuilder& builder_fn,
                                       const std::vector<std::string>& output_keys,
                                       const SubflowOptions& options) {
  if (executor_ == nullptr) {
    throw std::logic_error("Subflow execution requires GraphBuilder::run or run_async");
  }
  if (!builder_fn) {
    throw std::invalid_argument("Subflow builder is empty: " + name);
  }

  RunContext parent;
  {
    std::lock_guard<std::mutex> lock(*context_mutex_);
    parent = run_context_;
  }
  parent.max_depth = std::min(parent.max_depth, options.max_depth);
  RunContext child = parent.child(name, options.attempt);

  GraphBuilder nested(name + "[" + child.run_id + "]");
  nested.executor_ = executor_;
  nested.run_context_ = child;
  OutputBindings bindings = builder_fn(nested, inputs, child);

  std::unordered_set<std::string> declared(output_keys.begin(), output_keys.end());
  if (declared.size() != output_keys.size()) {
    throw std::invalid_argument("Duplicate declared output key in subflow: " + name);
  }
  for (const auto& [key, _] : bindings) {
    if (declared.find(key) == declared.end()) {
      throw std::invalid_argument("Undeclared subflow output binding: " + key);
    }
  }
  for (const auto& key : output_keys) {
    if (bindings.find(key) == bindings.end()) {
      throw std::invalid_argument("Missing subflow output binding: " + key);
    }
  }

  executor_->corun(nested.taskflow());
  ValueMap outputs;
  for (const auto& key : output_keys) {
    const auto& port = bindings.at(key);
    outputs.emplace(key, nested.get_latest_output(port.node, port.key));
  }
  return outputs;
}

std::pair<std::shared_ptr<AnyNode>, tf::Task>
GraphBuilder::create_subgraph_module(
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    std::shared_ptr<const SubflowModule> module,
    const std::vector<std::string>& output_keys) {
  if (!module) {
    throw std::invalid_argument("create_subgraph_module requires a module");
  }
  return create_subtask_module(
    name, input_specs, module->builder(), output_keys, module->options());
}

std::pair<std::shared_ptr<AnyNode>, tf::Task>
GraphBuilder::create_subtask_module(
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    SubflowBuilder builder_fn,
    const std::vector<std::string>& output_keys,
    SubflowOptions options) {
  return create_any_node(
    name,
    input_specs,
    [this, name, builder_fn = std::move(builder_fn), output_keys, options](const ValueMap& inputs) {
      return execute_subflow(name, inputs, builder_fn, output_keys, options);
    },
    output_keys);
}

std::pair<std::shared_ptr<LoopNode>, tf::Task>
GraphBuilder::create_loop(
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    LoopBody body,
    LoopCondition condition,
    LoopExit exit,
    const std::vector<std::string>& output_keys,
    LoopOptions options) {
  if (!body || !condition) {
    throw std::invalid_argument("create_loop requires body and condition callbacks");
  }

  std::unordered_map<std::string, std::shared_ptr<AnyValueSlot>> inputs;
  for (const auto& [source_node, source_key] : input_specs) {
    auto source = get_node(source_node);
    if (!source) {
      throw std::runtime_error("Node not found: " + source_node);
    }
    auto slot = source->get_output_slot(source_key);
    if (!slot) {
      throw std::logic_error("Loop source does not expose reusable output: " + source_node);
    }
    inputs[source_key] = std::move(slot);
  }

  auto node = std::make_shared<LoopNode>(
    std::unordered_map<std::string, std::shared_future<std::any>>{},
    std::function<void(const ValueMap&)>{},
    std::function<int(const ValueMap&)>{},
    output_keys,
    name);

  auto task = taskflow_.emplace([
    this, node, name, inputs, body = std::move(body), condition = std::move(condition),
    exit = std::move(exit), output_keys, options = std::move(options)]() mutable {
    if (executor_ == nullptr) {
      throw std::logic_error("Loop execution requires GraphBuilder::run or run_async");
    }

    struct State {
      ValueMap next_inputs;
      ValueMap body_outputs;
      LoopResult result;
      IterationContext context;
      bool first_condition {true};
    } state;

    for (const auto& [key, slot] : inputs) {
      state.next_inputs[key] = slot->read();
    }
    {
      std::lock_guard<std::mutex> lock(*context_mutex_);
      static_cast<RunContext&>(state.context) = run_context_;
    }
    state.context.subtask_id = name;
    if (options.cancel_requested) {
      state.context.cancel_requested = options.cancel_requested;
    }
    if (options.deadline) {
      state.context.deadline = options.deadline;
    }

    tf::Taskflow loopflow(name + "_runtime");
    auto entry_task = loopflow.emplace([]() {}).name(name + "_entry");
    auto condition_task = loopflow.emplace([&]() -> int {
      if (state.context.cancelled()) {
        state.result.status = LoopStatus::Cancelled;
        return 1;
      }
      if (state.context.deadline_exceeded()) {
        state.result.status = LoopStatus::DeadlineExceeded;
        return 1;
      }
      if (state.first_condition) {
        state.first_condition = false;
        if (options.max_iterations == 0) {
          state.result.status = LoopStatus::MaxIterations;
          return 1;
        }
        return 0;
      }
      if (state.result.status != LoopStatus::Running) {
        return 1;
      }
      try {
        if (condition(state.body_outputs, state.context) == LoopDecision::Exit) {
          state.result.status = LoopStatus::Completed;
          return 1;
        }
      } catch (const std::exception& e) {
        state.result.status = LoopStatus::ConditionError;
        state.result.error = e.what();
        return 1;
      } catch (...) {
        state.result.status = LoopStatus::ConditionError;
        state.result.error = "unknown condition exception";
        return 1;
      }
      if (state.result.iterations >= options.max_iterations) {
        state.result.status = LoopStatus::MaxIterations;
        return 1;
      }
      return 0;
    }).name(name + "_condition");

    auto body_task = loopflow.emplace([&]() {
      try {
        state.context.iteration = state.result.iterations;
        state.body_outputs = body(state.next_inputs, state.context);
        ++state.result.iterations;
        for (const auto& [output_key, input_key] : options.feedback) {
          auto it = state.body_outputs.find(output_key);
          if (it == state.body_outputs.end()) {
            throw std::runtime_error("Missing loop feedback output: " + output_key);
          }
          state.next_inputs[input_key] = it->second;
        }
      } catch (const std::exception& e) {
        state.result.status = LoopStatus::BodyError;
        state.result.error = e.what();
      } catch (...) {
        state.result.status = LoopStatus::BodyError;
        state.result.error = "unknown body exception";
      }
    }).name(name + "_body");

    auto exit_task = loopflow.emplace([&]() {
      const ValueMap& terminal_inputs = state.body_outputs.empty()
        ? state.next_inputs : state.body_outputs;
      state.result.outputs = terminal_inputs;
      if (exit) {
        try {
          state.result.outputs = exit(terminal_inputs, state.context);
        } catch (const std::exception& e) {
          state.result.status = LoopStatus::ExitError;
          state.result.error = e.what();
        } catch (...) {
          state.result.status = LoopStatus::ExitError;
          state.result.error = "unknown exit exception";
        }
      }
      if (state.result.status == LoopStatus::Running) {
        state.result.status = LoopStatus::Completed;
      }
    }).name(name + "_exit");

    auto back_task = loopflow.emplace([]() -> int { return 0; }).name(name + "_back");

    condition_task.precede(body_task, exit_task);
    body_task.precede(back_task);
    back_task.precede(condition_task);
    entry_task.precede(condition_task);
    executor_->corun(loopflow);

    state.result.outputs["__loop_status"] = std::string(loop_status_name(state.result.status));
    state.result.outputs["__loop_iterations"] = state.result.iterations;
    state.result.outputs["__loop_error"] = state.result.error;
    for (const auto& key : output_keys) {
      auto it = state.result.outputs.find(key);
      if (it == state.result.outputs.end()) {
        throw std::runtime_error(
          "Loop " + name + " did not produce declared output " + key +
          " (status=" + loop_status_name(state.result.status) +
          ", iterations=" + std::to_string(state.result.iterations) + ")");
      }
      node->out.publish(key, it->second);
    }
    {
      std::lock_guard<std::mutex> lock(node->result_mutex_);
      node->last_result_ = state.result;
    }
  }).name(name);

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
  (void)input_specs;
  (void)body_builder_fn;
  (void)condition_func;
  (void)exit_builder_fn;
  (void)output_keys;
  throw std::logic_error(
    "GraphBuilder callback create_loop_decl is legacy and has no explicit body output ports: " +
    name + ". Use create_loop.");
}

// Native function version of create_loop_decl (no GraphBuilder, with input/output)
std::pair<std::shared_ptr<LoopNode>, tf::Task>
GraphBuilder::create_loop_decl(const std::string& name,
                               const std::vector<std::pair<std::string, std::string>>& input_specs,
                               std::function<std::unordered_map<std::string, std::any>(
                                   const std::unordered_map<std::string, std::any>&)> body_func,
                               std::function<int(const std::unordered_map<std::string, std::any>&)> condition_func,
                               std::function<std::unordered_map<std::string, std::any>(
                                   const std::unordered_map<std::string, std::any>&)> exit_func,
                               const std::vector<std::string>& output_keys) {
  LoopOptions options;
  std::unordered_set<std::string> input_keys;
  for (const auto& [_, key] : input_specs) {
    input_keys.insert(key);
  }
  for (const auto& key : output_keys) {
    if (input_keys.find(key) != input_keys.end()) {
      options.feedback[key] = key;
    }
  }
  return create_loop(
    name,
    input_specs,
    [body_func = std::move(body_func)](const ValueMap& inputs, const IterationContext&) {
      return body_func(inputs);
    },
    [condition_func = std::move(condition_func)](const ValueMap& outputs,
                                                 const IterationContext&) {
      return condition_func(outputs) == 0 ? LoopDecision::Continue : LoopDecision::Exit;
    },
    exit_func
      ? LoopExit{[exit_func = std::move(exit_func)](const ValueMap& outputs,
                                                   const IterationContext&) {
          return exit_func(outputs);
        }}
      : LoopExit{},
    output_keys,
    std::move(options));
}

// Declarative loop with pre-created body_task (master mode)
// input_specs are used for condition_func inputs but dependencies are NOT automatically set
tf::Task
GraphBuilder::create_loop_decl(const std::string& name,
                               const std::vector<std::pair<std::string, std::string>>& input_specs,
                                        tf::Task& body_task,
                               std::function<int(const std::unordered_map<std::string, std::any>&)> condition_func,
                                        tf::Task exit_task) {
  // Get futures from source nodes for condition function inputs
  std::unordered_map<std::string, std::shared_future<std::any>> input_futures;
  for (const auto& [source_node, source_key] : input_specs) {
    input_futures[source_key] = get_output(source_node, source_key);
  }
  
  // Create condition task that receives inputs from input_specs and calls condition function
  // Note: We do NOT automatically set dependencies here - user must set them manually
  tf::Task cond_task = taskflow_.emplace([fin = std::move(input_futures), fn = std::move(condition_func)]() mutable -> int {
    // Extract input values from futures
    std::unordered_map<std::string, std::any> in_vals;
    for (const auto& [key, fut] : fin) {
      in_vals[key] = fut.get();
    }
    // Call condition function with inputs from input_specs
    return fn(in_vals);
  }).name(name + "_condition");
  
  // Wire loop: body -> cond
  // cond returns 0 for loop-back (body), non-zero for exit
  body_task.precede(cond_task);
  // Check if exit_task is valid using empty() method
  if (!exit_task.empty()) {
    cond_task.precede(body_task, exit_task);  // Index 0: continue loop, Index 1: exit
  } else {
    cond_task.precede(body_task);  // Only body as successor
  }
  
  // Auto-set initial dependencies based on input_specs
  // If input_specs exist, connect source nodes to body_task (initial trigger)
  // If no input_specs, create an empty start task to trigger the loop
  if (!input_specs.empty()) {
    // Connect all unique source nodes from input_specs to body_task
    std::unordered_set<std::string> processed_nodes;
    for (const auto& [source_node, source_key] : input_specs) {
      // Only process each source node once
      if (processed_nodes.find(source_node) != processed_nodes.end()) {
        continue;
      }
      processed_nodes.insert(source_node);
      
      // Find the source task in tasks_ map
      auto source_task_it = tasks_.find(source_node);
      if (source_task_it != tasks_.end()) {
        // Connect source -> body_task (initial trigger for first iteration)
        source_task_it->second.precede(body_task);
      }
    }
  } else {
    // No input_specs: create an empty start task to trigger the loop
    tf::Task start_task = taskflow_.emplace([]() {
      // Empty task - just triggers the loop body
    }).name(name + "_start");
    
    // Connect start -> body_task
    start_task.precede(body_task);
    
    // Store start task for potential future reference
    tasks_[name + "_start"] = start_task;
  }
  
  // Store condition task in tasks_ map for potential future reference
  tasks_[name] = cond_task;
  
  return cond_task;
}

// Deprecated precede/succeed methods are implemented inline in nodeflow_impl.hpp

}  // namespace workflow

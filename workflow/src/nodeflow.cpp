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
      std::cout << (node_name ? node_name : "AnySource") << " emitted\n";
  };
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
      std::cout << (node_name ? node_name : "AnyNode") << " done\n";
  };
}

// ============================================================================
// AnySink implementation
// ============================================================================

AnySink::AnySink(std::unordered_map<std::string, std::shared_future<std::any>> fin, const std::string& name)
    : inputs(std::move(fin)), node_name_(name.empty() ? "AnySink" : name) {}

std::function<void()> AnySink::functor(const char* node_name) const {
  auto fin = inputs;
  return [fin, node_name]() mutable {
    std::cout << (node_name ? node_name : "AnySink") << ": ";
    bool first = true;
    for (const auto& [key, fut] : fin) {
      if (!first) std::cout << ' ';
      first = false;
      const std::any& a = fut.get();
      std::cout << key << '=';
      if (a.type() == typeid(double)) {
        std::cout << std::any_cast<double>(a);
      } else if (a.type() == typeid(int)) {
        std::cout << std::any_cast<int>(a);
      } else if (a.type() == typeid(std::string)) {
        std::cout << std::any_cast<std::string>(a);
      } else {
        std::cout << "<" << a.type().name() << ">";
      }
    }
    std::cout << '\n';
  };
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

}  // namespace workflow


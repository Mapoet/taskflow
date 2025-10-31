// Nodeflow library: string-keyed heterogeneous dataflow nodes based on Taskflow
// Inputs/outputs are accessed via unordered_map<string, any> for flexible key-based access

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

namespace workflow {

// Outputs: key -> promise<any> mapping
struct AnyOutputs {
  std::unordered_map<std::string, std::shared_ptr<std::promise<std::any>>> promises;
  std::unordered_map<std::string, std::shared_future<std::any>> futures;

  void add(const std::string& key) {
    auto p = std::make_shared<std::promise<std::any>>();
    promises[key] = p;
    futures[key] = p->get_future().share();
  }

  void add(const std::vector<std::string>& keys) {
    for (const auto& key : keys) {
      add(key);
    }
  }

  // Convenience: initialize from key list
  AnyOutputs() = default;
  explicit AnyOutputs(const std::vector<std::string>& keys) {
    add(keys);
  }
};

// Node with string-keyed inputs and outputs
struct AnyNode {
  std::unordered_map<std::string, std::shared_future<std::any>> inputs;
  AnyOutputs out;
  std::function<std::unordered_map<std::string, std::any>(
      const std::unordered_map<std::string, std::any>&)> op;

  AnyNode() = default;

  AnyNode(std::unordered_map<std::string, std::shared_future<std::any>> fin,
          const std::vector<std::string>& out_keys,
          std::function<std::unordered_map<std::string, std::any>(
              const std::unordered_map<std::string, std::any>&)> fn)
      : inputs(std::move(fin)), out(out_keys), op(std::move(fn)) {}

  auto functor(const char* name) const {
    auto fin = inputs;
    auto promises = out.promises;
    auto fn = op;
    return [fin, promises, fn, name]() mutable {
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
      std::cout << name << " done" << '\n';
    };
  }
};

// Source node producing initial values
struct AnySource {
  std::unordered_map<std::string, std::any> values;
  AnyOutputs out;

  AnySource() = default;
  explicit AnySource(std::unordered_map<std::string, std::any> vals)
      : values(std::move(vals)), out(extract_keys(values)) {}

 private:
  static std::vector<std::string> extract_keys(
      const std::unordered_map<std::string, std::any>& m) {
    std::vector<std::string> keys;
    keys.reserve(m.size());
    for (const auto& [key, _] : m) {
      keys.push_back(key);
    }
    return keys;
  }

 public:
  auto functor(const char* name) const {
    auto vals = values;
    auto promises = out.promises;
    return [vals, promises, name]() mutable {
      for (const auto& [key, val] : vals) {
        auto it = promises.find(key);
        if (it == promises.end()) {
          throw std::runtime_error("Unknown source key: " + key);
        }
        it->second->set_value(val);
      }
      std::cout << name << " emitted" << '\n';
    };
  }
};

// Sink node consuming final values
struct AnySink {
  std::unordered_map<std::string, std::shared_future<std::any>> inputs;

  AnySink() = default;
  explicit AnySink(std::unordered_map<std::string, std::shared_future<std::any>> fin)
      : inputs(std::move(fin)) {}

  auto functor(const char* name) const {
    auto fin = inputs;
    return [fin, name]() mutable {
      std::cout << name << ": ";
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
};

}  // namespace workflow

#endif  // WORKFLOW_NODEFLOW_HPP


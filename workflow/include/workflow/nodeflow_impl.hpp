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
std::shared_future<std::any> TypedSource<Outs...>::get_output_future(const std::string& key) const {
  return out.get(key);
}

template <typename... Outs>
std::vector<std::string> TypedSource<Outs...>::get_output_keys() const {
  return out.keys();
}

template <typename... Outs>
TypedSource<Outs...>::TypedSource(std::tuple<Outs...> vals, 
                                   const std::vector<std::string>& output_keys,
                                   const std::string& name)
    : values(std::move(vals)), node_name_(name.empty() ? "TypedSource" : name) {
  out = TypedOutputs<Outs...>(output_keys);
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
  return add_node(node);
}

template <typename InputsTuple, typename... Outs>
tf::Task GraphBuilder::add_typed_node(std::shared_ptr<TypedNode<InputsTuple, Outs...>> node) {
  return add_node(std::static_pointer_cast<INode>(node));
}

template <typename... Ins>
tf::Task GraphBuilder::add_typed_sink(std::shared_ptr<TypedSink<Ins...>> node) {
  return add_node(std::static_pointer_cast<INode>(node));
}

template <typename Container>
void GraphBuilder::precede(tf::Task from, const Container& to) {
  for (const auto& t : to) {
    from.precede(t);
  }
}

template <typename Container>
void GraphBuilder::succeed(tf::Task to, const Container& from) {
  for (const auto& t : from) {
    to.succeed(t);
  }
}

}  // namespace workflow

#endif  // WORKFLOW_NODEFLOW_IMPL_HPP


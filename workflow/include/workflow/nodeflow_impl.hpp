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
}

// ============================================================================
// TypedSource implementation
// ============================================================================

template <typename... Outs>
TypedSource<Outs...>::TypedSource(std::tuple<Outs...> vals, const std::string& name)
    : values(std::move(vals)), node_name_(name.empty() ? "TypedSource" : name) {}

template <typename... Outs>
std::function<void()> TypedSource<Outs...>::functor(const char* node_name) const {
  auto promises = out.promises;
  auto vals = values;
  return [promises, vals, node_name]() mutable {
    detail::tuple_for_each(const_cast<std::tuple<Outs...>&>(vals), [&](auto& v, auto I){
      std::get<I.value>(promises)->set_value(v);
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
  // Convert fn to std::function with proper signature
  // Extract value types from InputsTuple
  using ValuesTuple = typename ExtractValueTypesHelper<InputsTuple>::type;
  using OpFuncType = std::function<std::tuple<Outs...>(ValuesTuple)>;
  op_ = OpFuncType(std::forward<OpType>(fn));
}

template <typename InputsTuple, typename... Outs>
std::function<void()> TypedNode<InputsTuple, Outs...>::functor(const char* node_name) const {
  auto fin = inputs;
  auto promises = out.promises;
  // Extract op from std::any
  using ValuesTuple = typename ExtractValueTypesHelper<InputsTuple>::type;
  using OpFuncType = std::function<std::tuple<Outs...>(ValuesTuple)>;
  OpFuncType fn = std::any_cast<OpFuncType>(op_);
  
  return [fin, promises, fn, node_name]() mutable {
    // Extract values from futures
    auto in_vals = detail::tuple_transform(fin, [](auto& f){ return f.get(); });
    // Call op with unwrapped values
    auto out_vals = fn(std::move(in_vals));
    detail::tuple_for_each(out_vals, [&](auto& v, auto I){
      std::get<I.value>(promises)->set_value(std::move(v));
    });
    std::cout << (node_name ? node_name : "TypedNode") << " done\n";
  };
}

// ============================================================================
// TypedSink implementation
// ============================================================================

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


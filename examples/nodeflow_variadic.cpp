// Variadic, heterogeneous nodeflow example.
// Generic nodes accept arbitrary number/types of inputs and outputs using
// parameter packs and std::tuple of shared_future/promises.

#include <taskflow/taskflow.hpp>
#include <future>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <iostream>

// tuple utilities -------------------------------------------------------------

template <typename Tuple, typename F, std::size_t... I>
auto tuple_transform_impl(const Tuple& t, F&& f, std::index_sequence<I...>) {
  return std::make_tuple(f(std::get<I>(t))...);
}

template <typename... Ts, typename F>
auto tuple_transform(const std::tuple<Ts...>& t, F&& f) {
  return tuple_transform_impl(t, std::forward<F>(f), std::index_sequence_for<Ts...>{});
}

template <typename Tuple, typename F, std::size_t... I>
void tuple_for_each_impl(Tuple&& t, F&& f, std::index_sequence<I...>) {
  (f(std::get<I>(t), std::integral_constant<std::size_t, I>{}), ...);
}

template <typename... Ts, typename F>
void tuple_for_each(std::tuple<Ts...>& t, F&& f) {
  tuple_for_each_impl(t, std::forward<F>(f), std::index_sequence_for<Ts...>{});
}

// Outputs: tuple of promises + futures for types Outs...
template <typename... Outs>
struct Outputs {
  std::tuple<std::shared_ptr<std::promise<Outs>>...> promises;
  std::tuple<std::shared_future<Outs>...> futures;

  Outputs() {
    // init promises
    promises = std::make_tuple(std::make_shared<std::promise<Outs>>()...);
    // init futures from promises
    futures = tuple_transform(promises, [](auto& p){ return p->get_future().share(); });
  }
};

// Node: inputs as a single tuple type; outputs as pack Outs...
template <typename InputsTuple, typename... Outs>
struct Node {
  InputsTuple inputs;
  Outputs<Outs...> out;

  Node() = default;
  explicit Node(InputsTuple fin) : inputs(std::move(fin)) {}

  template <typename F>
  auto functor(const char* name, F op) const {
    auto fin = inputs;                                 // copy of input futures
    auto promises = out.promises;                      // copy of shared_ptr<promise<...>>
    return [fin, promises, op, name]() mutable {
      auto in_vals = tuple_transform(fin, [](auto& f){ return f.get(); });
      if constexpr (sizeof...(Outs) == 1) {
        using OutT = std::tuple_element_t<0, std::tuple<Outs...>>;
        OutT y = std::apply(op, in_vals);
        std::get<0>(promises)->set_value(std::move(y));
        std::cout << name << " done" << '\n';
      } else {
        auto ys = std::apply(op, in_vals); // tuple<Outs...>
        static_assert(std::is_same_v<decltype(ys), std::tuple<Outs...>>,
                      "op must return tuple<Outs...>");
        tuple_for_each(ys, [&](auto& v, auto I){
          std::get<I.value>(promises)->set_value(std::move(v));
        });
        std::cout << name << " done" << '\n';
      }
    };
  }
};

// Factory to infer input types; usage: make_node<Outs...>(std::make_tuple(futs...))
template <typename... Outs, typename... Ins>
auto make_node(std::tuple<std::shared_future<Ins>...> fin) {
  return Node<decltype(fin), Outs...>(std::move(fin));
}

// Source producing Outs... with no Ins
template <typename... Outs>
struct SourceNode {
  std::tuple<Outs...> values;
  Outputs<Outs...> out;
  explicit SourceNode(std::tuple<Outs...> vals) : values(std::move(vals)) {}
  auto functor(const char* name) const {
    auto promises = out.promises;
    auto vals = values;
    return [promises, vals, name]() mutable {
      tuple_for_each(const_cast<std::tuple<Outs...>&>(vals), [&](auto& v, auto I){
        std::get<I.value>(promises)->set_value(v);
      });
      std::cout << name << " emitted" << '\n';
    };
  }
};

// Sink consuming Ins... and printing results
template <typename... Ins>
struct SinkNode {
  std::tuple<std::shared_future<Ins>...> inputs;
  explicit SinkNode(std::tuple<std::shared_future<Ins>...> fin) : inputs(std::move(fin)) {}
  auto functor(const char* name) const {
    auto fin = inputs;
    return [fin, name]() mutable {
      auto vals = tuple_transform(fin, [](auto& f){ return f.get(); });
      std::cout << name << ": ";
      std::apply([](const auto&... xs){ ((std::cout << xs << ' '), ...); }, vals);
      std::cout << '\n';
    };
  }
};

int main() {
  tf::Executor executor;
  tf::Taskflow tf("nodeflow_variadic");

  // A emits two heterogeneous values: double x, int k
  SourceNode<double, int> A(std::make_tuple(3.5, 7));

  // Split A's outputs
  auto [x_fut, k_fut] = A.out.futures;

  // B: unary double -> double (x + 1.0)
  auto B = make_node<double>(std::make_tuple(x_fut));

  // C: unary double -> double (2x)
  auto C = make_node<double>(std::make_tuple(x_fut));

  // E: unary int -> int (k - 2)
  auto E = make_node<int>(std::make_tuple(k_fut));

  // D: binary (double,double) -> double  (prod = (x+1)*(2x))
  auto D = make_node<double>(std::make_tuple(std::get<0>(B.out.futures), std::get<0>(C.out.futures)));

  // G: ternary (double, double, int) -> std::tuple<double, int>
  // sum = (2x) + (x+1), parity = (k-2) % 2
  auto G = make_node<double, int>(std::make_tuple(
    std::get<0>(C.out.futures), std::get<0>(B.out.futures), std::get<0>(E.out.futures))
  );

  // H: sink of (double prod, double sum, int parity)
  auto prod_fut = std::get<0>(D.out.futures);
  auto sum_fut  = std::get<0>(G.out.futures);
  auto par_fut  = std::get<1>(G.out.futures);
  SinkNode<double, double, int> H(std::make_tuple(prod_fut, sum_fut, par_fut));

  // Create tasks (bind ops)
  auto tA = tf.emplace(A.functor("A"));
  tA.name("A");

  auto tB = tf.emplace(B.functor("B", [](double x){ return x + 1.0; }));
  tB.name("B");

  auto tC = tf.emplace(C.functor("C", [](double x){ return 2.0 * x; }));
  tC.name("C");

  auto tE = tf.emplace(E.functor("E", [](int k){ return k - 2; }));
  tE.name("E");

  auto tD = tf.emplace(D.functor("D", [](double b, double c){ return b * c; }));
  tD.name("D");

  auto tG = tf.emplace(G.functor("G", [](double c, double b, int ek){
    double sum = c + b;
    int parity = (ek % 2 + 2) % 2;
    return std::make_tuple(sum, parity);
  }));
  tG.name("G");

  auto tH = tf.emplace(H.functor("H"));
  tH.name("H");

  // Dependencies
  tA.precede(tB, tC, tE);
  tD.succeed(tB, tC);
  tG.succeed(tC, tB, tE);
  tH.succeed(tD, tG);

  executor.run(tf).wait();
  tf.dump(std::cout);
  return 0;
}



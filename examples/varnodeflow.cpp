// Any-based nodeflow: portable lambdas with heterogeneous inputs/outputs via std::any
// Graph mirrors previous examples:
//   A: emits [double x=3.5, int k=7]
//   B: [x] -> [x+1]
//   C: [x] -> [2x]
//   E: [k] -> [k-2]
//   D: [b, c] -> [prod]
//   G: [c, b, ek] -> [sum, parity]
//   H: sink prints [prod, sum, parity]

#include <taskflow/taskflow.hpp>
#include <future>
#include <memory>
#include <any>
#include <vector>
#include <iostream>
#include <functional>

struct AnyOutputs {
  std::vector<std::shared_ptr<std::promise<std::any>>> promises;
  std::vector<std::shared_future<std::any>> futures;
  explicit AnyOutputs(std::size_t n = 0) {
    resize(n);
  }
  void resize(std::size_t n) {
    promises.clear();
    futures.clear();
    promises.reserve(n);
    futures.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      auto p = std::make_shared<std::promise<std::any>>();
      promises.emplace_back(p);
      futures.emplace_back(p->get_future().share());
    }
  }
};

struct AnyNode {
  std::vector<std::shared_future<std::any>> inputs;
  AnyOutputs out;
  std::function<std::vector<std::any>(const std::vector<std::any>&)> op;

  AnyNode() = default;
  AnyNode(std::vector<std::shared_future<std::any>> fin,
          std::size_t out_count,
          std::function<std::vector<std::any>(const std::vector<std::any>&)> fn)
  : inputs(std::move(fin)), out(out_count), op(std::move(fn)) {}

  auto functor(const char* name) const {
    auto fin = inputs;
    auto promises = out.promises;
    auto fn = op;
    return [fin, promises, fn, name]() mutable {
      std::vector<std::any> values;
      values.reserve(fin.size());
      for (auto& f : fin) values.emplace_back(f.get());
      auto outs = fn(values);
      if (outs.size() != promises.size()) {
        throw std::runtime_error("output size mismatch");
      }
      for (std::size_t i = 0; i < outs.size(); ++i) {
        promises[i]->set_value(std::move(outs[i]));
      }
      std::cout << name << " done" << '\n';
    };
  }
};

struct AnySource {
  std::vector<std::any> values;
  AnyOutputs out;
  explicit AnySource(std::vector<std::any> vals)
  : values(std::move(vals)), out(values.size()) {}
  auto functor(const char* name) const {
    auto vals = values;
    auto promises = out.promises;
    return [vals, promises, name]() mutable {
      if (vals.size() != promises.size()) {
        throw std::runtime_error("source size mismatch");
      }
      for (std::size_t i = 0; i < vals.size(); ++i) {
        promises[i]->set_value(vals[i]);
      }
      std::cout << name << " emitted" << '\n';
    };
  }
};

struct AnySink {
  std::vector<std::shared_future<std::any>> inputs;
  explicit AnySink(std::vector<std::shared_future<std::any>> fin)
  : inputs(std::move(fin)) {}
  auto functor(const char* name) const {
    auto fin = inputs;
    return [fin, name]() mutable {
      std::cout << name << ": ";
      for (std::size_t i = 0; i < fin.size(); ++i) {
        const std::any& a = fin[i].get();
        if (a.type() == typeid(double)) {
          std::cout << std::any_cast<double>(a);
        } else if (a.type() == typeid(int)) {
          std::cout << std::any_cast<int>(a);
        } else {
          std::cout << "<unknown>";
        }
        if (i + 1 < fin.size()) std::cout << ' ';
      }
      std::cout << '\n';
    };
  }
};

int main() {
  tf::Executor executor;
  tf::Taskflow tf("varnodeflow");

  // A: emit [x=3.5, k=7]
  AnySource A({std::any{3.5}, std::any{7}});

  // Split outputs
  auto x_fut = A.out.futures[0];
  auto k_fut = A.out.futures[1];

  // B: [x] -> [x+1]
  AnyNode B({x_fut}, 1, [](const std::vector<std::any>& in){
    double x = std::any_cast<double>(in[0]);
    return std::vector<std::any>{x + 1.0};
  });

  // C: [x] -> [2x]
  AnyNode C({x_fut}, 1, [](const std::vector<std::any>& in){
    double x = std::any_cast<double>(in[0]);
    return std::vector<std::any>{2.0 * x};
  });

  // E: [k] -> [k-2]
  AnyNode E({k_fut}, 1, [](const std::vector<std::any>& in){
    int k = std::any_cast<int>(in[0]);
    return std::vector<std::any>{k - 2};
  });

  // D: [b, c] -> [prod]
  AnyNode D({B.out.futures[0], C.out.futures[0]}, 1,
    [](const std::vector<std::any>& in){
      double b = std::any_cast<double>(in[0]);
      double c = std::any_cast<double>(in[1]);
      return std::vector<std::any>{b * c};
    }
  );

  // G: [c, b, ek] -> [sum, parity]
  AnyNode G({C.out.futures[0], B.out.futures[0], E.out.futures[0]}, 2,
    [](const std::vector<std::any>& in){
      double c = std::any_cast<double>(in[0]);
      double b = std::any_cast<double>(in[1]);
      int ek = std::any_cast<int>(in[2]);
      double sum = c + b;
      int parity = (ek % 2 + 2) % 2;
      return std::vector<std::any>{sum, parity};
    }
  );

  // H: sink [prod, sum, parity]
  AnySink H({D.out.futures[0], G.out.futures[0], G.out.futures[1]});

  // Create tasks
  auto tA = tf.emplace(A.functor("A")).name("A");
  auto tB = tf.emplace(B.functor("B")).name("B");
  auto tC = tf.emplace(C.functor("C")).name("C");
  auto tE = tf.emplace(E.functor("E")).name("E");
  auto tD = tf.emplace(D.functor("D")).name("D");
  auto tG = tf.emplace(G.functor("G")).name("G");
  auto tH = tf.emplace(H.functor("H")).name("H");

  // Dependencies
  tA.precede(tB, tC, tE);
  tD.succeed(tB, tC);
  tG.succeed(tC, tB, tE);
  tH.succeed(tD, tG);

  executor.run(tf).wait();
  tf.dump(std::cout);
  return 0;
}



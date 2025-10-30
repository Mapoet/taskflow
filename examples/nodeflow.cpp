// Nodeflow example: encapsulate per-node inputs/outputs and execution using
// shared_future/shared_ptr<promise>, built on top of Taskflow.
//
// Graph (same math as dataflow_arith.cpp):
//   A: x
//   B: x+1
//   C: 2x
//   E: x-3
//   D: (x+1)*(2x)
//   G: (2x)+(x-3)
//   H: sink prints prod and sum

#include <taskflow/taskflow.hpp>
#include <future>
#include <memory>
#include <iostream>
#include <functional>

struct SingleOutput {
  std::shared_ptr<std::promise<double>> promise;
  std::shared_future<double> future;
  SingleOutput() : promise(std::make_shared<std::promise<double>>()),
                   future(promise->get_future().share()) {}
};

struct SourceNode {
  double value;
  SingleOutput out;
  explicit SourceNode(double v) : value(v) {}
  auto functor() const {
    auto p = out.promise;
    double x = value;
    return [p, x]() {
      std::cout << "A: x = " << x << '\n';
      p->set_value(x);
    };
  }
};

struct MapUnaryNode {
  std::shared_future<double> in;
  std::function<double(double)> op; // e.g., add1, mul2, sub3
  SingleOutput out;
  MapUnaryNode(std::shared_future<double> fin,
               std::function<double(double)> f)
  : in(std::move(fin)), op(std::move(f)) {}
  auto functor(const char* name) const {
    auto f = in;
    auto p = out.promise;
    auto opf = op;
    return [f, p, opf, name]() {
      const double x = f.get();
      const double y = opf(x);
      std::cout << name << ": " << y << '\n';
      p->set_value(y);
    };
  }
};

struct MapBinaryNode {
  std::shared_future<double> in1;
  std::shared_future<double> in2;
  std::function<double(double,double)> op; // e.g., mul, add
  SingleOutput out;
  MapBinaryNode(std::shared_future<double> f1,
                std::shared_future<double> f2,
                std::function<double(double,double)> f)
  : in1(std::move(f1)), in2(std::move(f2)), op(std::move(f)) {}
  auto functor(const char* name) const {
    auto f1 = in1;
    auto f2 = in2;
    auto p  = out.promise;
    auto opf = op;
    return [f1, f2, p, opf, name]() {
      const double a = f1.get();
      const double b = f2.get();
      const double y = opf(a, b);
      std::cout << name << ": " << y << '\n';
      p->set_value(y);
    };
  }
};

struct SinkBinaryNode {
  std::shared_future<double> in1;
  std::shared_future<double> in2;
  SinkBinaryNode(std::shared_future<double> f1,
                 std::shared_future<double> f2)
  : in1(std::move(f1)), in2(std::move(f2)) {}
  auto functor(const char* name) const {
    auto f1 = in1;
    auto f2 = in2;
    return [f1, f2, name]() {
      const double a = f1.get();
      const double b = f2.get();
      std::cout << name << ": prod = " << a << ", sum = " << b << '\n';
    };
  }
};

int main() {
  tf::Executor executor;
  tf::Taskflow tf("nodeflow");

  // Define nodes
  SourceNode A(3.5);
  MapUnaryNode  B(A.out.future, [](double x){ return x + 1.0; });      // x+1
  MapUnaryNode  C(A.out.future, [](double x){ return 2.0 * x; });      // 2x
  MapUnaryNode  E(A.out.future, [](double x){ return x - 3.0; });      // x-3
  MapBinaryNode D(B.out.future, C.out.future,
                  [](double b, double c){ return b * c; });            // prod
  MapBinaryNode G(C.out.future, E.out.future,
                  [](double c, double e){ return c + e; });            // sum
  SinkBinaryNode H(D.out.future, G.out.future);

  // Create tasks
  auto tA = tf.emplace(A.functor()).name("A");
  auto tB = tf.emplace(B.functor("B: x+1")).name("B");
  auto tC = tf.emplace(C.functor("C: 2*x")).name("C");
  auto tE = tf.emplace(E.functor("E: x-3")).name("E");
  auto tD = tf.emplace(D.functor("D: (x+1)*(2*x)")).name("D");
  auto tG = tf.emplace(G.functor("G: (2*x)+(x-3)")).name("G");
  auto tH = tf.emplace(H.functor("H")).name("H");

  // Dependencies (execution ordering)
  tA.precede(tB, tC, tE);
  tD.succeed(tB, tC);
  tG.succeed(tC, tE);
  tH.succeed(tD, tG);

  executor.run(tf).wait();
  tf.dump(std::cout);
  return 0;
}



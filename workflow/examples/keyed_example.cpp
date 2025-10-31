// Example using string-keyed nodeflow library
// Graph:
//   A: emits {"x": 3.5, "k": 7}
//   B: {"x"} -> {"b": x+1}
//   C: {"x"} -> {"c": 2*x}
//   E: {"k"} -> {"ek": k-2}
//   D: {"b", "c"} -> {"prod": b*c}
//   G: {"c", "b", "ek"} -> {"sum": c+b, "parity": ek%2}
//   H: sink prints {"prod", "sum", "parity"}

#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>
#include <stdexcept>

int main() {
  tf::Executor executor;
  tf::Taskflow tf("keyed_nodeflow");

  namespace wf = workflow;

  // A: emit {"x": 3.5, "k": 7}
  wf::AnySource A({{"x", std::any{3.5}}, {"k", std::any{7}}});

  // B: {"x"} -> {"b": x+1}
  wf::AnyNode B(
      {{"x", A.out.futures.at("x")}},
      {"b"},
      [](const std::unordered_map<std::string, std::any>& in) {
        double x = std::any_cast<double>(in.at("x"));
        return std::unordered_map<std::string, std::any>{{"b", x + 1.0}};
      });

  // C: {"x"} -> {"c": 2*x}
  wf::AnyNode C(
      {{"x", A.out.futures.at("x")}},
      {"c"},
      [](const std::unordered_map<std::string, std::any>& in) {
        double x = std::any_cast<double>(in.at("x"));
        return std::unordered_map<std::string, std::any>{{"c", 2.0 * x}};
      });

  // E: {"k"} -> {"ek": k-2}
  wf::AnyNode E(
      {{"k", A.out.futures.at("k")}},
      {"ek"},
      [](const std::unordered_map<std::string, std::any>& in) {
        int k = std::any_cast<int>(in.at("k"));
        return std::unordered_map<std::string, std::any>{{"ek", k - 2}};
      });

  // D: {"b", "c"} -> {"prod": b*c}
  wf::AnyNode D(
      {{"b", B.out.futures.at("b")}, {"c", C.out.futures.at("c")}},
      {"prod"},
      [](const std::unordered_map<std::string, std::any>& in) {
        double b = std::any_cast<double>(in.at("b"));
        double c = std::any_cast<double>(in.at("c"));
        return std::unordered_map<std::string, std::any>{{"prod", b * c}};
      });

  // G: {"c", "b", "ek"} -> {"sum": c+b, "parity": ek%2}
  wf::AnyNode G(
      {{"c", C.out.futures.at("c")},
       {"b", B.out.futures.at("b")},
       {"ek", E.out.futures.at("ek")}},
      {"sum", "parity"},
      [](const std::unordered_map<std::string, std::any>& in) {
        double c = std::any_cast<double>(in.at("c"));
        double b = std::any_cast<double>(in.at("b"));
        int ek = std::any_cast<int>(in.at("ek"));
        double sum = c + b;
        int parity = (ek % 2 + 2) % 2;
        return std::unordered_map<std::string, std::any>{
            {"sum", sum}, {"parity", parity}};
      });

  // H: sink prints {"prod", "sum", "parity"}
  wf::AnySink H({{"prod", D.out.futures.at("prod")},
                 {"sum", G.out.futures.at("sum")},
                 {"parity", G.out.futures.at("parity")}});

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


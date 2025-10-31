// Unified example demonstrating:
// - Pure virtual base class INode usage
// - Typed nodes (compile-time type-safe)
// - Any-based nodes (runtime type-erased)
// - GraphBuilder for graph construction and execution
//
// Graph:
//   A (typed source): emits {x: 3.5, k: 7}
//   B (typed): x -> x+1
//   C (typed): x -> 2*x
//   E (typed): k -> k-2
//   D (typed): {b, c} -> {prod}
//   G (typed): {c, b, ek} -> {sum, parity}
//   H (any-based sink): prints {prod, sum, parity}

#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>
#include <memory>
#include <vector>

int main() {
  namespace wf = workflow;

  tf::Executor executor;
  wf::GraphBuilder builder("unified_workflow");

  // ==========================================================================
  // Typed Source Node (compile-time type-safe)
  // ==========================================================================
  auto A = std::make_shared<wf::TypedSource<double, int>>(
    std::make_tuple(3.5, 7), "A"
  );
  auto tA = builder.add_typed_source(A);

  // Access typed outputs
  auto x_fut = std::get<0>(A->out.futures);
  auto k_fut = std::get<1>(A->out.futures);

  // ==========================================================================
  // Typed Nodes (compile-time type-safe)
  // ==========================================================================
  
  // B: double -> double (x+1)
  using B_Inputs = std::tuple<std::shared_future<double>>;
  auto B = std::make_shared<wf::TypedNode<B_Inputs, double>>(
    std::make_tuple(x_fut),
    [](const std::tuple<double>& in) {
      double x = std::get<0>(in);
      return std::make_tuple(x + 1.0);
    },
    "B"
  );
  auto tB = builder.add_typed_node(B);

  // C: double -> double (2*x)
  using C_Inputs = std::tuple<std::shared_future<double>>;
  auto C = std::make_shared<wf::TypedNode<C_Inputs, double>>(
    std::make_tuple(x_fut),
    [](const std::tuple<double>& in) {
      double x = std::get<0>(in);
      return std::make_tuple(2.0 * x);
    },
    "C"
  );
  auto tC = builder.add_typed_node(C);

  // E: int -> int (k-2)
  using E_Inputs = std::tuple<std::shared_future<int>>;
  auto E = std::make_shared<wf::TypedNode<E_Inputs, int>>(
    std::make_tuple(k_fut),
    [](const std::tuple<int>& in) {
      int k = std::get<0>(in);
      return std::make_tuple(k - 2);
    },
    "E"
  );
  auto tE = builder.add_typed_node(E);

  // ==========================================================================
  // Typed Node with Multiple Inputs
  // ==========================================================================
  
  // D: (double, double) -> double (prod = b*c)
  using D_Inputs = std::tuple<std::shared_future<double>, std::shared_future<double>>;
  auto D = std::make_shared<wf::TypedNode<D_Inputs, double>>(
    std::make_tuple(
      std::get<0>(B->out.futures),
      std::get<0>(C->out.futures)
    ),
    [](const std::tuple<double, double>& in) {
      double b = std::get<0>(in);
      double c = std::get<1>(in);
      return std::make_tuple(b * c);
    },
    "D"
  );
  auto tD = builder.add_typed_node(D);

  // ==========================================================================
  // Typed Node with Multiple Outputs
  // ==========================================================================
  
  // G: (double, double, int) -> (double, int)
  using G_Inputs = std::tuple<std::shared_future<double>, std::shared_future<double>, std::shared_future<int>>;
  auto G = std::make_shared<wf::TypedNode<G_Inputs, double, int>>(
    std::make_tuple(
      std::get<0>(C->out.futures),
      std::get<0>(B->out.futures),
      std::get<0>(E->out.futures)
    ),
    [](const std::tuple<double, double, int>& in) {
      double c = std::get<0>(in);
      double b = std::get<1>(in);
      int ek = std::get<2>(in);
      double sum = c + b;
      int parity = (ek % 2 + 2) % 2;
      return std::make_tuple(sum, parity);
    },
    "G"
  );
  auto tG = builder.add_typed_node(G);

  // ==========================================================================
  // Bridge: Convert typed outputs to any for AnySink
  // Create adapter tasks that bridge typed -> any
  // ==========================================================================
  
  auto p_prod = std::make_shared<std::promise<std::any>>();
  auto p_sum = std::make_shared<std::promise<std::any>>();
  auto p_parity = std::make_shared<std::promise<std::any>>();
  
  auto f_prod = p_prod->get_future().share();
  auto f_sum = p_sum->get_future().share();
  auto f_parity = p_parity->get_future().share();
  
  // Manual adapter tasks
  auto adapter_prod_task = builder.taskflow().emplace([p_prod, prod_fut=std::get<0>(D->out.futures)]() {
    p_prod->set_value(std::any{prod_fut.get()});
  }).name("Adapter_prod");
  
  auto adapter_sum_task = builder.taskflow().emplace([p_sum, sum_fut=std::get<0>(G->out.futures)]() {
    p_sum->set_value(std::any{sum_fut.get()});
  }).name("Adapter_sum");
  
  auto adapter_parity_task = builder.taskflow().emplace([p_parity, par_fut=std::get<1>(G->out.futures)]() {
    p_parity->set_value(std::any{par_fut.get()});
  }).name("Adapter_parity");
  
  // ==========================================================================
  // Any-based Sink (for demonstration of mixed typed/any usage)
  // ==========================================================================
  
  auto H = std::make_shared<wf::AnySink>(
    std::unordered_map<std::string, std::shared_future<std::any>>{
      {"prod", f_prod},
      {"sum", f_sum},
      {"parity", f_parity}
    },
    "H"
  );
  auto tH = builder.add_any_sink(H);

  // ==========================================================================
  // Use INode base class interface (polymorphism demonstration)
  // ==========================================================================
  
  std::vector<std::shared_ptr<wf::INode>> all_nodes = {
    std::static_pointer_cast<wf::INode>(A),
    std::static_pointer_cast<wf::INode>(B),
    std::static_pointer_cast<wf::INode>(C),
    std::static_pointer_cast<wf::INode>(E),
    std::static_pointer_cast<wf::INode>(D),
    std::static_pointer_cast<wf::INode>(G),
    std::static_pointer_cast<wf::INode>(H)
  };
  
  std::cout << "\n=== Node Information (via INode interface) ===" << '\n';
  for (auto& node : all_nodes) {
    std::cout << "Node: " << node->name() 
              << ", Type: " << node->type() << '\n';
  }
  std::cout << '\n';

  // ==========================================================================
  // Configure dependencies using GraphBuilder
  // ==========================================================================
  
  // Configure dependencies
  builder.precede(tA, std::vector<tf::Task>{tB, tC, tE});
  builder.succeed(tD, std::vector<tf::Task>{tB, tC});
  builder.succeed(tG, std::vector<tf::Task>{tC, tB, tE});
  
  // Adapter tasks depend on their sources
  adapter_prod_task.succeed(tD);
  adapter_sum_task.succeed(tG);
  adapter_parity_task.succeed(tG);
  
  // Sink depends on all adapters
  builder.succeed(tH, std::vector<tf::Task>{adapter_prod_task, adapter_sum_task, adapter_parity_task});

  // ==========================================================================
  // Run the workflow
  // ==========================================================================
  
  builder.run(executor);
  builder.dump(std::cout);

  return 0;
}

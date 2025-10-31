// Unified example demonstrating improved key-based API:
// - Nodes specify output keys at construction
// - Connections via keys (no manual future access)
// - Type-free functor operations

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
  // Create nodes with explicit output keys
  // ==========================================================================
  
  // A: Typed source with keys "x" and "k"
  auto A = std::make_shared<wf::TypedSource<double, int>>(
    std::make_tuple(3.5, 7), 
    std::vector<std::string>{"x", "k"},
    "A"
  );
  auto tA = builder.add_typed_source(A);

  // ==========================================================================
  // Demonstrate key-based output access
  // ==========================================================================
  
  std::cout << "=== Node Output Keys ===" << '\n';
  auto a_keys = A->get_output_keys();
  std::cout << "A outputs: ";
  for (const auto& k : a_keys) {
    std::cout << k << " ";
  }
  std::cout << '\n';
  
  // B: double -> double (x+1)
  using B_Inputs = std::tuple<std::shared_future<double>>;
  auto B = std::make_shared<wf::TypedNode<B_Inputs, double>>(
    std::make_tuple(A->out.get_typed<0>("x")),
    [](const std::tuple<double>& in) {
      return std::make_tuple(std::get<0>(in) + 1.0);
    },
    std::vector<std::string>{"b"},
    "B"
  );
  auto tB = builder.add_typed_node(B);

  // C: double -> double (2*x)
  using C_Inputs = std::tuple<std::shared_future<double>>;
  auto C = std::make_shared<wf::TypedNode<C_Inputs, double>>(
    std::make_tuple(A->out.get_typed<0>("x")),
    [](const std::tuple<double>& in) {
      return std::make_tuple(2.0 * std::get<0>(in));
    },
    std::vector<std::string>{"c"},
    "C"
  );
  auto tC = builder.add_typed_node(C);

  // E: int -> int (k-2)
  using E_Inputs = std::tuple<std::shared_future<int>>;
  auto E = std::make_shared<wf::TypedNode<E_Inputs, int>>(
    std::make_tuple(A->out.get_typed<1>("k")),
    [](const std::tuple<int>& in) {
      return std::make_tuple(std::get<0>(in) - 2);
    },
    std::vector<std::string>{"ek"},
    "E"
  );
  auto tE = builder.add_typed_node(E);

  // D: (double, double) -> double
  using D_Inputs = std::tuple<std::shared_future<double>, std::shared_future<double>>;
  auto D = std::make_shared<wf::TypedNode<D_Inputs, double>>(
    std::make_tuple(B->out.get_typed<0>("b"), C->out.get_typed<0>("c")),
    [](const std::tuple<double, double>& in) {
      return std::make_tuple(std::get<0>(in) * std::get<1>(in));
    },
    std::vector<std::string>{"prod"},
    "D"
  );
  auto tD = builder.add_typed_node(D);

  // G: (double, double, int) -> (double, int)
  using G_Inputs = std::tuple<std::shared_future<double>, std::shared_future<double>, std::shared_future<int>>;
  auto G = std::make_shared<wf::TypedNode<G_Inputs, double, int>>(
    std::make_tuple(C->out.get_typed<0>("c"), B->out.get_typed<0>("b"), E->out.get_typed<0>("ek")),
    [](const std::tuple<double, double, int>& in) {
      double sum = std::get<0>(in) + std::get<1>(in);
      int parity = (std::get<2>(in) % 2 + 2) % 2;
      return std::make_tuple(sum, parity);
    },
    std::vector<std::string>{"sum", "parity"},
    "G"
  );
  auto tG = builder.add_typed_node(G);

  // ==========================================================================
  // Use key-based access for AnySink (demonstrates unified interface)
  // ==========================================================================
  
  auto H = std::make_shared<wf::AnySink>(
    std::unordered_map<std::string, std::shared_future<std::any>>{
      {"prod", D->get_output_future("prod")},
      {"sum", G->get_output_future("sum")},
      {"parity", G->get_output_future("parity")}
    },
    "H"
  );
  auto tH = builder.add_any_sink(H);

  // ==========================================================================
  // Demonstrate key-based API
  // ==========================================================================
  
  std::cout << "\n=== Node Output Keys (via key-based API) ===" << '\n';
  for (const auto& [name, node] : builder.nodes()) {
    auto keys = node->get_output_keys();
    std::cout << node->name() << " outputs: [";
    for (size_t i = 0; i < keys.size(); ++i) {
      if (i > 0) std::cout << ", ";
      std::cout << keys[i];
    }
    std::cout << "]" << '\n';
  }

  // ==========================================================================
  // Configure dependencies using GraphBuilder (simplified)
  // ==========================================================================
  
  builder.precede(tA, std::vector<tf::Task>{tB, tC, tE});
  builder.succeed(tD, std::vector<tf::Task>{tB, tC});
  builder.succeed(tG, std::vector<tf::Task>{tC, tB, tE});
  builder.succeed(tH, std::vector<tf::Task>{tD, tG});

  // ==========================================================================
  // Run the workflow
  // ==========================================================================
  
  builder.run(executor);
  builder.dump(std::cout);

  return 0;
}

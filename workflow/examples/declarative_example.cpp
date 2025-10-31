// Declarative API example: 
// - Key-based inputs via input specifications
// - Automatic dependency inference
// - No manual precede/succeed calls

#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>
#include <memory>
#include <vector>

int main() {
  namespace wf = workflow;

  tf::Executor executor;
  wf::GraphBuilder builder("declarative_workflow");

  // ==========================================================================
  // Create source node using declarative API
  // ==========================================================================
  auto [A, tA] = builder.create_typed_source("A",
    std::make_tuple(3.5, 7),
    std::vector<std::string>{"x", "k"}
  );

  // ==========================================================================
  // Create typed nodes using declarative API with automatic dependency inference
  // ==========================================================================
  
  // B: A::x -> b (x+1)
  // Note: First template parameter(s) are input types, output types are inferred
  auto [B, tB] = builder.create_typed_node<double>(
    "B",
    {{"A", "x"}},  // Input: from A's "x" output
    [](const std::tuple<double>& in) {
      return std::make_tuple(std::get<0>(in) + 1.0);
    },
    {"b"}  // Output key
  );

  // C: A::x -> c (2*x)
  auto [C, tC] = builder.create_typed_node<double>(
    "C",
    {{"A", "x"}},
    [](const std::tuple<double>& in) {
      return std::make_tuple(2.0 * std::get<0>(in));
    },
    {"c"}
  );

  // E: A::k -> ek (k-2)
  auto [E, tE] = builder.create_typed_node<int>(
    "E",
    {{"A", "k"}},
    [](const std::tuple<int>& in) {
      return std::make_tuple(std::get<0>(in) - 2);
    },
    {"ek"}
  );

  // D: B::b, C::c -> prod (b*c)
  auto [D, tD] = builder.create_typed_node<double, double>(
    "D",
    {{"B", "b"}, {"C", "c"}},  // Multiple inputs
    [](const std::tuple<double, double>& in) {
      return std::make_tuple(std::get<0>(in) * std::get<1>(in));
    },
    {"prod"}
  );

  // G: C::c, B::b, E::ek -> sum, parity
  auto [G, tG] = builder.create_typed_node<double, double, int>(
    "G",
    {{"C", "c"}, {"B", "b"}, {"E", "ek"}},
    [](const std::tuple<double, double, int>& in) {
      double sum = std::get<0>(in) + std::get<1>(in);
      int parity = (std::get<2>(in) % 2 + 2) % 2;
      return std::make_tuple(sum, parity);
    },
    {"sum", "parity"}
  );

  // ==========================================================================
  // Create sink using declarative API
  // ==========================================================================
  
  auto [H, tH] = builder.create_any_sink("H",
    {{"D", "prod"}, {"G", "sum"}, {"G", "parity"}}
  );

  // ==========================================================================
  // No manual dependency configuration needed!
  // Dependencies are automatically inferred from input specifications:
  // - B, C, E depend on A (from input specs)
  // - D depends on B, C (from input specs)
  // - G depends on C, B, E (from input specs)
  // - H depends on D, G (from input specs)
  // ==========================================================================

  // ==========================================================================
  // Run the workflow
  // ==========================================================================
  
  std::cout << "=== Running declarative workflow ===" << '\n';
  builder.run(executor);
  builder.dump(std::cout);

  return 0;
}


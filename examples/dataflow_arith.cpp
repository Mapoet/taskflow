// A Taskflow example using std::promise/std::future for clean data passing.
//
//                +---+ (x+1) ----+               +---+
//          +---> | B |-----------+-------------->|   |
//          |     +---+                          |   |
//        +---+            +---+ (2x) ----+      | D | (prod = (x+1)*(2x))
//  x --->| A |-----------> | C |---------+----->|   |----+
//        +---+            +---+                +---+    |
//          |                 | (x-3) ---> +---+          v
//          |                 +----------> | E |------> +---+
//          |                                +---+      | H | (print prod & sum)
//          | (broadcast futures)                           +---+
//          |                              +---+
//          +----------------------------> | G | (sum = 2x + (x-3))
//                                         +---+
//
// - A produces a double x and fulfills dedicated promises for B and C.
// - B computes x+1 and fulfills a promise for D.
// - C computes 2x and fulfills promises for D and G.
// - E computes x-3 (fed by C's input x) and fulfills a promise for G.
// - G computes sum = (2x) + (x-3) and fulfills a promise for H.
// - D computes prod = (x+1)*(2x) and fulfills a promise for H.
// - H waits on both prod and sum to print results.

#include <taskflow/taskflow.hpp>
#include <iostream>
#include <future>
#include <memory>

int main() {

  tf::Executor executor;
  tf::Taskflow taskflow("dataflow_arith");

  // Promises/futures wiring (copyable lambdas via shared_ptr/shared_future)
  auto pAtoB = std::make_shared<std::promise<double>>();
  auto pAtoC = std::make_shared<std::promise<double>>();
  auto pAtoE = std::make_shared<std::promise<double>>();
  std::shared_future<double> fAtoB = pAtoB->get_future().share();
  std::shared_future<double> fAtoC = pAtoC->get_future().share();
  std::shared_future<double> fAtoE = pAtoE->get_future().share();

  auto pBtoD = std::make_shared<std::promise<double>>();
  auto pCtoD = std::make_shared<std::promise<double>>();
  std::shared_future<double> fBtoD = pBtoD->get_future().share();
  std::shared_future<double> fCtoD = pCtoD->get_future().share();

  auto pCtoG = std::make_shared<std::promise<double>>();
  auto pEtoG = std::make_shared<std::promise<double>>();
  std::shared_future<double> fCtoG = pCtoG->get_future().share();
  std::shared_future<double> fEtoG = pEtoG->get_future().share();

  auto pDtoH = std::make_shared<std::promise<double>>();  // prod
  auto pGtoH = std::make_shared<std::promise<double>>();  // sum
  std::shared_future<double> fDtoH = pDtoH->get_future().share();
  std::shared_future<double> fGtoH = pGtoH->get_future().share();

  // A: produce x and broadcast via two promises
  double x_input = 3.5;  // could be read from config/args
  auto A = taskflow.emplace(
    [x = x_input,
     pB = pAtoB,
     pC = pAtoC,
     pE = pAtoE]() mutable {
      std::cout << "A: x = " << x << '\n';
      pB->set_value(x);
      pC->set_value(x);
      pE->set_value(x);
    }
  ).name("A");

  // B: consumes x, outputs x+1 -> D
  auto B = taskflow.emplace(
    [f = fAtoB,
     p = pBtoD]() mutable {
      const double x = f.get();
      const double b = x + 1.0;
      std::cout << "B: x+1 = " << b << '\n';
      p->set_value(b);
    }
  ).name("B");

  // C: consumes x, outputs 2x -> D and G
  auto C = taskflow.emplace(
    [f = fAtoC,
     pD = pCtoD,
     pG = pCtoG]() mutable {
      const double x = f.get();
      const double c = 2.0 * x;
      std::cout << "C: 2*x = " << c << '\n';
      pD->set_value(c);
      pG->set_value(c);
    }
  ).name("C");

  // E: consumes x to compute x-3 -> G
  auto E = taskflow.emplace(
    [f = fAtoE,
     p = pEtoG]() mutable {
      const double x = f.get();
      const double e = x - 3.0;
      std::cout << "E: x-3 = " << e << '\n';
      p->set_value(e);
    }
  ).name("E");

  // D: consumes (x+1) and (2x) -> prod -> H
  auto D = taskflow.emplace(
    [f1 = fBtoD,
     f2 = fCtoD,
     p  = pDtoH]() mutable {
      const double b = f1.get();
      const double c = f2.get();
      const double prod = b * c;
      std::cout << "D: (x+1)*(2*x) = " << prod << '\n';
      p->set_value(prod);
    }
  ).name("D");

  // G: consumes (2x) and (x-3) -> sum -> H
  auto G = taskflow.emplace(
    [f1 = fCtoG,
     f2 = fEtoG,
     p  = pGtoH]() mutable {
      const double c = f1.get();
      const double e = f2.get();
      const double sum = c + e;
      std::cout << "G: (2*x) + (x-3) = " << sum << '\n';
      p->set_value(sum);
    }
  ).name("G");

  // H: sink, prints both prod and sum
  auto H = taskflow.emplace(
    [fp = fDtoH,
     fs = fGtoH]() mutable {
      const double prod = fp.get();
      const double sum  = fs.get();
      std::cout << "H: prod = " << prod << ", sum = " << sum << '\n';
    }
  ).name("H");

  // Edges (execution dependencies). Futures enforce data ordering; we add minimal structure:
  // A before B, C, E; B,C before D; C,E before G; D,G before H.
  A.precede(B, C, E);
  D.succeed(B, C);
  G.succeed(C, E);
  H.succeed(D, G);

  executor.run(taskflow).wait();
  taskflow.dump(std::cout);
  return 0;
}



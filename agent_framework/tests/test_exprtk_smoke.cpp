/**
 * @file test_exprtk_smoke.cpp
 * @brief Smoke test: ExprTk submodule compiles and evaluates a simple expression.
 */
#include "exprtk.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

int main() {
    using T = double;
    exprtk::symbol_table<T> sym;
    exprtk::expression<T> expr;
    exprtk::parser<T> parser;

    T x = 2.0;
    T y = 3.0;
    sym.add_variable("x", x);
    sym.add_variable("y", y);
    expr.register_symbol_table(sym);

    const std::string program = "x * y + 1";
    if (!parser.compile(program, expr)) {
        return 1;
    }

    const T v = expr.value();
    if (std::abs(v - 7.0) > 1e-9) {
        return 2;
    }
    return 0;
}

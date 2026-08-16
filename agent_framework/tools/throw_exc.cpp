#include <boost/throw_exception.hpp>
#include <boost/exception/exception.hpp>
#include <iostream>
int main() {
    try {
        throw std::runtime_error("test");
    } catch (const std::exception& e) {
        std::cout << "caught: " << e.what() << "\n";
    }
    return 0;
}

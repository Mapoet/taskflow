cmake_minimum_required(VERSION 3.18)
project(workflow_package_consumer LANGUAGES CXX)

find_package(workflow CONFIG REQUIRED)

add_executable(workflow_package_consumer main.cpp)
target_link_libraries(workflow_package_consumer PRIVATE workflow::workflow)

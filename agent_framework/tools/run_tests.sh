#!/bin/bash
# Agent Framework 测试脚本

set -e

BUILD_DIR="build"

if [ ! -d "${BUILD_DIR}" ]; then
    echo "Build directory not found. Running build first..."
    ./tools/build.sh Debug
fi

cd ${BUILD_DIR}

echo "Running tests..."
ctest --output-on-failure

echo "Tests completed!"


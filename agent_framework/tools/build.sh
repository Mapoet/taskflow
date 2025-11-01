#!/bin/bash
# Agent Framework 构建脚本

set -e

BUILD_TYPE="${1:-Release}"
BUILD_DIR="build"

echo "Building Agent Framework (${BUILD_TYPE})..."

mkdir -p ${BUILD_DIR}
cd ${BUILD_DIR}

cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} ..
make -j$(nproc)

echo "Build completed successfully!"
echo "Run examples:"
echo "  ./build/examples/simple_agent"
echo "  ./build/examples/multimodal_agent"


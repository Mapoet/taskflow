#!/bin/bash
# Agent Framework 性能分析脚本

set -e

BUILD_DIR="build"
PROGRAM="${1:-examples/simple_agent}"

if [ ! -f "${BUILD_DIR}/${PROGRAM}" ]; then
    echo "Program ${PROGRAM} not found. Building first..."
    ./tools/build.sh Release
fi

echo "Profiling ${PROGRAM}..."
echo "Setting TF_ENABLE_PROFILER=1"

cd ${BUILD_DIR}
TF_ENABLE_PROFILER=1 TF_PROFILER_OUTPUT=tfprof.json ./${PROGRAM}

if [ -f "tfprof.json" ]; then
    echo "Profiling data saved to tfprof.json"
    echo "You can analyze it using Taskflow profiling tools"
else
    echo "Warning: Profiling data not generated"
fi


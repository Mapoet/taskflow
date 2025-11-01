#!/bin/bash
# 初始化所有 git submodules
# 注意：子模块位于项目根目录的 3rd-party/ 目录中

set -e

echo "Initializing git submodules for Agent Framework..."
echo "Submodules are located in the root directory: 3rd-party/"

# 切换到项目根目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "Root directory: $ROOT_DIR"
cd "$ROOT_DIR"

# 初始化并更新所有 submodules
git submodule update --init --recursive

echo ""
echo "Submodules initialized successfully!"
echo ""
echo "Available submodules in 3rd-party/:"
git submodule status 3rd-party/


#!/bin/bash
# build.sh - 构建脚本

set -e  # 遇到错误退出

echo "=== LuckFox Pico视频项目构建脚本 ==="

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 配置
PROJECT_DIR=$(pwd)
BUILD_DIR="${PROJECT_DIR}/build"
TOOLCHAIN_FILE="${PROJECT_DIR}/toolchain.cmake"

# 清理旧构建
if [ -d "${BUILD_DIR}" ]; then
    echo -e "${YELLOW}清理旧构建...${NC}"
    rm -rf "${BUILD_DIR}"
fi

# 创建构建目录
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# 配置
echo -e "${GREEN}配置CMake...${NC}"
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release

# 检查配置结果
if [ $? -ne 0 ]; then
    echo -e "${RED}CMake配置失败${NC}"
    exit 1
fi

# 编译
echo -e "${GREEN}开始编译...${NC}"
make -j$(nproc)

# 检查编译结果
if [ $? -ne 0 ]; then
    echo -e "${RED}编译失败${NC}"
    exit 1
fi

echo -e "${GREEN}编译成功！${NC}"
echo "输出文件在: ${BUILD_DIR}/bin/"

# 列出生成的文件
echo -e "\n${YELLOW}生成的可执行文件:${NC}"
ls -lh bin/

# 检查文件类型
echo -e "\n${YELLOW}文件类型:${NC}"
for file in bin/*; do
    if [ -f "$file" ]; then
        echo -n "$(basename $file): "
        file "$file" | cut -d: -f2-
    fi
done

# 显示文件大小
echo -e "\n${YELLOW}文件大小统计:${NC}"
du -h bin/* | sort -h
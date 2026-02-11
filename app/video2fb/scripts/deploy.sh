#!/bin/bash
# scripts/deploy.sh - 部署到开发板

set -e

# 配置
LOCAL_BUILD_DIR="$(pwd)/build/bin"
REMOTE_USER="root"
REMOTE_HOST="172.32.0.93"  # 修改为你的开发板IP
REMOTE_DIR="/root"

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo "=== 部署到LuckFox Pico ==="

# 检查本地文件
if [ ! -d "${LOCAL_BUILD_DIR}" ]; then
    echo -e "${RED}错误: 构建目录不存在，请先运行 build.sh${NC}"
    exit 1
fi

# 询问开发板IP
read -p "请输入开发板IP地址 [默认: ${REMOTE_HOST}]: " input_host
REMOTE_HOST=${input_host:-$REMOTE_HOST}

# 检查连接
echo "检查与开发板的连接..."
if ! ping -c 1 -W 2 "${REMOTE_HOST}" &> /dev/null; then
    echo -e "${RED}无法连接到开发板 ${REMOTE_HOST}${NC}"
    exit 1
fi

echo -e "${GREEN}连接到开发板成功${NC}"

# 上传文件
echo "上传文件到开发板..."
for file in ${LOCAL_BUILD_DIR}/*; do
    if [ -f "$file" ] && [ -x "$file" ]; then
        filename=$(basename "$file")
        echo "上传: $filename"
        
        # 使用scp上传
        scp "$file" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_DIR}/"
        
        if [ $? -eq 0 ]; then
            echo -e "${GREEN}✓ $filename 上传成功${NC}"
        else
            echo -e "${RED}✗ $filename 上传失败${NC}"
        fi
    fi
done

# 设置执行权限
# echo "设置执行权限..."
# ssh "${REMOTE_USER}@${REMOTE_HOST}" "chmod +x ${REMOTE_DIR}/*"

# echo -e "\n${GREEN}部署完成！${NC}"
# echo "在开发板上运行:"
# for file in ${LOCAL_BUILD_DIR}/*; do
#     if [ -f "$file" ] && [ -x "$file" ]; then
#         filename=$(basename "$file")
#         echo "  ssh ${REMOTE_USER}@${REMOTE_HOST} \"cd /root && ./${filename}\""
#     fi
# done
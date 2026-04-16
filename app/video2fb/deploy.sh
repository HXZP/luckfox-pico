#!/bin/bash
# scripts/deploy.sh - 部署到开发板

set -e

# 配置
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}" && pwd)"
LOCAL_BUILD_DIR="${PROJECT_DIR}/build/bin"
REMOTE_USER="root"
REMOTE_HOST="192.168.2.16"  # 修改为你的开发板IP
REMOTE_DIR="/duck"
REMOTE_PASSWORD="luckfox"

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo "=== 部署到LuckFox Pico ==="

if ! command -v sshpass >/dev/null 2>&1; then
    echo -e "${RED}错误: 未找到 sshpass，请先安装（例如: sudo apt-get install sshpass）${NC}"
    exit 1
fi

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

# 收集可上传文件
files=()
for file in "${LOCAL_BUILD_DIR}"/*; do
    if [ -f "$file" ]; then
        files+=("$file")
    fi
done

if [ ${#files[@]} -eq 0 ]; then
    echo -e "${RED}错误: ${LOCAL_BUILD_DIR} 下没有可上传文件${NC}"
    exit 1
fi

echo "可上传文件:"
for file in "${files[@]}"; do
    echo "  - $(basename "$file")"
done
echo
read -p "请输入要上传的文件名（多个用空格分隔，输入 all 上传全部）: " selected_input

selected_files=()
if [ -z "$selected_input" ] || [ "$selected_input" = "all" ]; then
    selected_files=("${files[@]}")
else
    for name in $selected_input; do
        candidate="${LOCAL_BUILD_DIR}/${name}"
        if [ -f "$candidate" ]; then
            selected_files+=("$candidate")
        else
            echo -e "${RED}错误: 文件不存在 -> ${name}${NC}"
            exit 1
        fi
    done
fi

# 上传文件
echo "上传文件到开发板..."
for file in "${selected_files[@]}"; do
    filename=$(basename "$file")
    echo "上传: $filename"

    # 使用sshpass + scp自动输入密码
    sshpass -p "${REMOTE_PASSWORD}" scp -o StrictHostKeyChecking=no "$file" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_DIR}/"

    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ $filename 上传成功${NC}"
    else
        echo -e "${RED}✗ $filename 上传失败${NC}"
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
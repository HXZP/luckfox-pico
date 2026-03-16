#!/bin/bash
###############################################################################
# Luckfox Pico Zero - 一键部署与首次编译脚本
# 适用系统: Ubuntu 22.04 x86_64
# 板型: RV1106_Luckfox_Pico_Zero (EMMC + Buildroot)
###############################################################################
set -e

# ===== 颜色定义 =====
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

SDK_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN_REL="tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"
TOOLCHAIN_DIR="${SDK_DIR}/${TOOLCHAIN_REL}"
TOOLCHAIN_BIN="${TOOLCHAIN_DIR}/bin"

# Lunch 菜单选项（Zero = 11, EMMC = 0, Buildroot = 0）
BOARD_INDEX=11
MEDIUM_INDEX=0
SYSTEM_INDEX=0

log_info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()  { echo -e "\n${CYAN}========== $* ==========${NC}\n"; }

# ===== 检查运行环境 =====
check_env() {
    log_step "步骤 0: 检查运行环境"

    if [[ "$(uname -m)" != "x86_64" ]]; then
        log_error "仅支持 x86_64 架构，当前: $(uname -m)"
        exit 1
    fi

    if ! grep -qi "ubuntu" /etc/os-release 2>/dev/null; then
        log_warn "当前系统可能不是 Ubuntu，编译可能出现兼容性问题"
    fi

    if [[ ! -f "${SDK_DIR}/build.sh" ]]; then
        log_error "未在 ${SDK_DIR} 找到 build.sh，请确保脚本放置在 SDK 根目录"
        exit 1
    fi

    if [[ ! -d "${TOOLCHAIN_BIN}" ]]; then
        log_error "未找到交叉编译工具链: ${TOOLCHAIN_BIN}"
        exit 1
    fi

    log_info "运行环境检查通过"
}

# ===== 安装编译依赖 =====
install_deps() {
    log_step "步骤 1: 安装编译依赖"

    sudo apt-get update || log_warn "apt update 部分源可能失败，继续安装..."

    sudo apt-get install -y \
        git ssh make gcc gcc-multilib g++-multilib module-assistant \
        expect g++ gawk texinfo libssl-dev bison flex fakeroot cmake \
        unzip gperf autoconf device-tree-compiler libncurses5-dev \
        pkg-config bc python-is-python3 passwd openssl \
        openssh-server openssh-client vim file cpio rsync curl

    log_info "编译依赖安装完成"
}

# ===== 配置交叉编译工具链环境变量 =====
setup_toolchain() {
    log_step "步骤 2: 配置交叉编译工具链"

    local marker="# Luckfox Pico Zero toolchain"

    if grep -qF "${marker}" ~/.bashrc 2>/dev/null; then
        log_info "工具链路径已存在于 ~/.bashrc，跳过"
    else
        {
            echo ""
            echo "${marker}"
            echo "export PATH=${TOOLCHAIN_BIN}:\$PATH"
        } >> ~/.bashrc
        log_info "已将工具链路径添加到 ~/.bashrc"
    fi

    # 在当前 shell 中也生效
    export PATH="${TOOLCHAIN_BIN}:${PATH}"

    if command -v arm-rockchip830-linux-uclibcgnueabihf-gcc &>/dev/null; then
        local ver
        ver=$(arm-rockchip830-linux-uclibcgnueabihf-gcc --version | head -1)
        log_info "交叉编译器可用: ${ver}"
    else
        log_error "交叉编译器不可用，请检查工具链路径"
        exit 1
    fi
}

# ===== 选择板型并编译 =====
select_board_and_build() {
    log_step "步骤 3: 选择板型 (RV1106_Luckfox_Pico_Zero / EMMC / Buildroot)"

    cd "${SDK_DIR}"
    echo -e "${BOARD_INDEX}\n${MEDIUM_INDEX}\n${SYSTEM_INDEX}" | ./build.sh lunch

    log_info "板型选择完成"

    log_step "步骤 4: 开始全量编译 (此过程耗时较长)"
    log_info "编译日志同步输出到: /tmp/luckfox_zero_build.log"

    ./build.sh 2>&1 | tee /tmp/luckfox_zero_build.log

    # 检查编译结果
    if grep -q "Running build_allsave succeeded" /tmp/luckfox_zero_build.log; then
        log_info "编译成功!"
    else
        log_error "编译可能失败，请检查日志: /tmp/luckfox_zero_build.log"
        exit 1
    fi
}

# ===== 打印编译结果 =====
show_result() {
    log_step "编译结果"

    local img_dir="${SDK_DIR}/output/image"
    if [[ -d "${img_dir}" ]]; then
        echo -e "${GREEN}镜像文件列表:${NC}"
        ls -lh "${img_dir}"/ | grep -v "^total\|^总计" | awk '{printf "  %-25s %s\n", $NF, $5}'
        echo ""
        log_info "镜像目录: ${img_dir}"
        log_info "完整升级包: ${img_dir}/update.img"
    else
        log_warn "未找到镜像输出目录"
    fi

    echo ""
    log_info "交叉编译器使用方法:"
    echo "  arm-rockchip830-linux-uclibcgnueabihf-gcc hello.c -o hello"
    echo "  scp hello root@<板子IP>:/root/"
    echo ""
    log_info "常用编译命令:"
    echo "  ./build.sh lunch          # 重新选择板型"
    echo "  ./build.sh                # 全量编译"
    echo "  ./build.sh kernel         # 单独编译内核"
    echo "  ./build.sh uboot          # 单独编译 U-Boot"
    echo "  ./build.sh rootfs         # 单独编译根文件系统"
    echo "  ./build.sh firmware       # 固件打包(自定义文件后)"
    echo "  ./build.sh clean          # 清除全部编译产物"
}

# ===== 主流程 =====
main() {
    echo ""
    echo "============================================================"
    echo "  Luckfox Pico Zero - 一键部署与首次编译"
    echo "  板型: RV1106_Luckfox_Pico_Zero | EMMC | Buildroot"
    echo "  SDK:  ${SDK_DIR}"
    echo "============================================================"
    echo ""

    # 支持分步执行
    case "${1:-all}" in
        deps)
            check_env
            install_deps
            ;;
        toolchain)
            check_env
            setup_toolchain
            ;;
        build)
            check_env
            setup_toolchain
            select_board_and_build
            show_result
            ;;
        all)
            check_env
            install_deps
            setup_toolchain
            select_board_and_build
            show_result
            ;;
        *)
            echo "用法: $0 [all|deps|toolchain|build]"
            echo ""
            echo "  all        完整流程: 安装依赖 + 配置工具链 + 编译 (默认)"
            echo "  deps       仅安装编译依赖"
            echo "  toolchain  仅配置交叉编译工具链"
            echo "  build      仅选择板型并编译 (需已安装依赖)"
            exit 0
            ;;
    esac
}

main "$@"

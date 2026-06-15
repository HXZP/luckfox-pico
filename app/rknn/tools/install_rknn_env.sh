#!/bin/bash
#
# install_rknn_env.sh - 一键安装 RKNN-Toolkit2 1.6.x PC 转换环境
#
# 方案说明：
# 1. 本脚本用于上位机 Ubuntu/WSL 环境，不用于开发板端运行。
# 2. RKNN-Toolkit2 对 Python 版本有要求，当前使用独立 Conda 环境，避免污染系统 Python。
# 3. 环境安装在 app/rknn/.rknn/miniconda3 下，Conda 环境名为 RKNN-Toolkit2-1.6。
# 4. 默认使用 Python 3.8、rknn-toolkit2 1.6.0 和 CPU 版 PyTorch。
# 5. 当前工程最终模型使用 1.6.x 工具链转换，用于匹配板端较旧的 RKNN runtime。
#
# 使用说明：
#   bash app/rknn/tools/install_rknn_env.sh
#
# 安装完成后进入环境：
#   source app/rknn/.rknn/miniconda3/bin/activate RKNN-Toolkit2-1.6
#
# 退出环境：
#   conda deactivate

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
RKNN_DIR="${PROJECT_DIR}/.rknn"
MINICONDA_DIR="${RKNN_DIR}/miniconda3"
CONDA_BIN="${MINICONDA_DIR}/bin/conda"
ENV_NAME="RKNN-Toolkit2-1.6"
PYTHON_VERSION="3.8"
RKNN_TOOLKIT_VERSION="1.6.0"
TORCH_VERSION="2.4.0+cpu"
OPENCV_VERSION="4.11.0.86"
NUMPY_VERSION="1.23.5"
PROTOBUF_VERSION="4.25.4"
ONNX_VERSION="1.14.1"
PYPI_MIRROR="${PYPI_MIRROR:-https://pypi.tuna.tsinghua.edu.cn/simple}"
MINICONDA_URL="${MINICONDA_URL:-https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh}"
MINICONDA_INSTALLER="${RKNN_DIR}/Miniconda3-latest-Linux-x86_64.sh"
RKNN_TOOLKIT_WHL="${RKNN_TOOLKIT_WHL:-}"

# 打印普通信息。
log_info()
{
    echo "[INFO] $*"
}

# 打印错误信息。
log_error()
{
    echo "[ERROR] $*" >&2
}

# 检查当前主机架构是否满足 RKNN-Toolkit2 x86_64 wheel 要求。
check_host()
{
    local arch_name

    arch_name="$(uname -m)"
    if [ "${arch_name}" != "x86_64" ]
    then
        log_error "当前架构为 ${arch_name}，RKNN-Toolkit2 PC 工具链需要 x86_64。"
        return 1
    fi

    if ! command -v curl >/dev/null 2>&1
    then
        log_error "未找到 curl，请先安装 curl。"
        return 1
    fi

    mkdir -p "${RKNN_DIR}"
}

# 安装 Miniconda；如果已经存在则跳过。
install_miniconda()
{
    if [ -x "${CONDA_BIN}" ]
    then
        log_info "检测到 Miniconda 已安装：${MINICONDA_DIR}"
        "${CONDA_BIN}" --version
        return 0
    fi

    log_info "下载 Miniconda：${MINICONDA_URL}"
    curl -L -o "${MINICONDA_INSTALLER}" "${MINICONDA_URL}"

    log_info "安装 Miniconda 到：${MINICONDA_DIR}"
    bash "${MINICONDA_INSTALLER}" -b -p "${MINICONDA_DIR}"
    "${CONDA_BIN}" --version
}

# 接受 Conda 默认源服务条款；旧版本 Conda 不支持该命令时忽略。
accept_conda_tos()
{
    log_info "检查 Conda 默认源服务条款"
    "${CONDA_BIN}" tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main >/dev/null 2>&1 || true
    "${CONDA_BIN}" tos accept --override-channels --channel https://repo.anaconda.com/pkgs/r >/dev/null 2>&1 || true
}

# 创建 RKNN-Toolkit2 Conda 环境；如果已经存在则跳过。
create_conda_env()
{
    if "${CONDA_BIN}" env list | awk '{print $1}' | grep -x "${ENV_NAME}" >/dev/null 2>&1
    then
        log_info "检测到 Conda 环境已存在：${ENV_NAME}"
        return 0
    fi

    log_info "创建 Conda 环境：${ENV_NAME}，Python ${PYTHON_VERSION}"
    "${CONDA_BIN}" create -y -n "${ENV_NAME}" "python=${PYTHON_VERSION}"
}

# 在 RKNN 环境中执行 Python/pip 命令。
run_in_env()
{
    # shellcheck disable=SC1091
    source "${MINICONDA_DIR}/bin/activate" "${ENV_NAME}"
    "$@"
}

# 安装 RKNN-Toolkit2 及依赖。
install_rknn_packages()
{
    log_info "升级 pip"
    run_in_env python -m pip install --upgrade pip

    log_info "安装 CPU 版 PyTorch：${TORCH_VERSION}"
    run_in_env python -m pip install \
        --index-url https://download.pytorch.org/whl/cpu \
        "torch==${TORCH_VERSION}"

    log_info "安装 RKNN-Toolkit2 依赖包"
    run_in_env python -m pip install \
        -i "${PYPI_MIRROR}" \
        --timeout 120 \
        "numpy==${NUMPY_VERSION}" \
        "protobuf==${PROTOBUF_VERSION}" \
        psutil \
        ruamel.yaml \
        scipy \
        tqdm \
        "opencv-python==${OPENCV_VERSION}" \
        fast-histogram \
        "onnx==${ONNX_VERSION}" \
        "onnxruntime>=1.10.0"

    if [ -n "${RKNN_TOOLKIT_WHL}" ]
    then
        log_info "从本地 wheel 安装 rknn-toolkit2：${RKNN_TOOLKIT_WHL}"
        run_in_env python -m pip install --no-deps "${RKNN_TOOLKIT_WHL}"
    else
        log_info "从 PyPI 安装 rknn-toolkit2：${RKNN_TOOLKIT_VERSION}"
        run_in_env python -m pip install \
            -i "${PYPI_MIRROR}" \
            --timeout 120 \
            --no-deps \
            "rknn-toolkit2==${RKNN_TOOLKIT_VERSION}"
    fi
}

# 验证 RKNN 环境是否可用。
verify_rknn_env()
{
    log_info "验证 RKNN-Toolkit2 环境"
    run_in_env python - <<'PY'
import sys
from rknn.api import RKNN
import torch
import cv2
import onnx
import onnxruntime as ort

print("python", sys.version.split()[0])
print("rknn-toolkit2 import ok")
print("torch", torch.__version__)
print("cv2", cv2.__version__)
print("onnx", onnx.__version__)
print("onnxruntime", ort.__version__)
if not hasattr(onnx, "mapping"):
    raise RuntimeError("当前 onnx 版本缺少 onnx.mapping，无法兼容 rknn-toolkit2。")
PY

    run_in_env python -m pip check
}

# 打印后续使用说明。
print_usage()
{
    echo
    echo "================ RKNN 环境安装完成 ================"
    echo "进入环境："
    echo "  source ${MINICONDA_DIR}/bin/activate ${ENV_NAME}"
    echo
    echo "验证导入："
    echo "  python -c \"from rknn.api import RKNN; print('RKNN ok')\""
    echo
    echo "退出环境："
    echo "  conda deactivate"
    echo "=================================================="
}

main()
{
    check_host
    install_miniconda
    accept_conda_tos
    create_conda_env
    install_rknn_packages
    verify_rknn_env
    print_usage
}

main "$@"

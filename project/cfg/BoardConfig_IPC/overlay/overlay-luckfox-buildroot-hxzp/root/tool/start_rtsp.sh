#!/bin/sh
#
# start_rtsp.sh - LuckFox Pico 一键启动 RTSP 摄像头预览
#
# 方案说明：
# 1. 本脚本使用 Rockchip 厂商提供的 rkipc 程序启动摄像头预览。
# 2. rkipc 会加载 rk_aiq，并读取 /oem/usr/share/iqfiles 下的 IQ 调校文件。
# 3. rkipc 会从摄像头采集图像，经硬件编码后，通过 RTSP 输出到上位机。
# 4. start/status 不会主动停止 v2fb、simple_camera_fb 或旧的 rkipc，只会打印提醒。
# 5. stop 只用于关闭 rkipc，不会处理其他摄像头测试程序。
#
# 使用说明：
#   sh /duck/test/start_rtsp.sh start
#   sh /duck/test/start_rtsp.sh stop
#   sh /duck/test/start_rtsp.sh force-stop
#   sh /duck/test/start_rtsp.sh status
#
# 上位机查看方式：
#   ffplay -rtsp_transport tcp rtsp://<开发板IP>/live/1
#   VLC 打开网络串流：rtsp://<开发板IP>/live/0 或 rtsp://<开发板IP>/live/1

RKIPC_BIN="/oem/usr/bin/rkipc"
RKIPC_CONFIG="/userdata/rkipc.ini"
RKIPC_DEFAULT_CONFIG="/oem/usr/share/rkipc-300w.ini"
IQ_DIR="/oem/usr/share/iqfiles"
LOG_FILE="/tmp/rkipc.log"

export PATH="/oem/usr/bin:/usr/bin:/bin:/sbin:/usr/sbin:${PATH}"
export LD_LIBRARY_PATH="/oem/usr/lib:/usr/lib:${LD_LIBRARY_PATH}"

# 获取开发板当前 IP 地址，用于打印上位机访问地址。
get_board_ip()
{
    local ip_addr

    ip_addr="$(ifconfig wlan0 2>/dev/null | awk '/inet addr:/{sub("addr:", "", $2); print $2; exit} /inet /{print $2; exit}')"

    if [ -z "${ip_addr}" ]
    then
        ip_addr="$(ifconfig eth0 2>/dev/null | awk '/inet addr:/{sub("addr:", "", $2); print $2; exit} /inet /{print $2; exit}')"
    fi

    if [ -z "${ip_addr}" ]
    then
        ip_addr="$(ifconfig usb0 2>/dev/null | awk '/inet addr:/{sub("addr:", "", $2); print $2; exit} /inet /{print $2; exit}')"
    fi

    if [ -z "${ip_addr}" ]
    then
        ip_addr="$(ifconfig 2>/dev/null | awk '/inet addr:/{sub("addr:", "", $2); if ($2 != "127.0.0.1") {print $2; exit}} /inet /{if ($2 != "127.0.0.1") {print $2; exit}}')"
    fi

    if [ -z "${ip_addr}" ]
    then
        ip_addr="${BOARD_IP:-10.8.49.116}"
    fi

    echo "${ip_addr}"
}

# 判断指定进程名是否正在运行。
is_running()
{
    local process_name

    process_name="$1"
    ps | grep "${process_name}" | grep -v grep >/dev/null 2>&1
}

# 打印脚本命令说明。
print_script_usage()
{
    echo "使用方法："
    echo "  sh /duck/test/start_rtsp.sh start       # 启动 RTSP"
    echo "  sh /duck/test/start_rtsp.sh stop        # 优雅关闭 rkipc"
    echo "  sh /duck/test/start_rtsp.sh force-stop  # 强制关闭 rkipc"
    echo "  sh /duck/test/start_rtsp.sh status      # 查看运行状态"
}

# 打印上位机查看 RTSP 图像的操作方式。
print_view_usage()
{
    local board_ip

    board_ip="$(get_board_ip)"

    echo
    echo "================ RTSP 预览地址 ================"
    echo "主码流：rtsp://${board_ip}/live/0"
    echo "子码流：rtsp://${board_ip}/live/1"
    echo
    echo "上位机推荐命令："
    echo "  ffplay -rtsp_transport tcp rtsp://${board_ip}/live/1"
    echo
    echo "也可以使用 VLC："
    echo "  媒体 -> 打开网络串流 -> rtsp://${board_ip}/live/0"
    echo "  媒体 -> 打开网络串流 -> rtsp://${board_ip}/live/1"
    echo "==============================================="
    echo
}

# 打印可能占用摄像头的进程提醒。
print_camera_owner_warning()
{
    if is_running "v2fb"
    then
        echo "提醒：检测到 v2fb 正在运行，可能会占用摄像头。"
    fi

    if is_running "simple_camera_fb"
    then
        echo "提醒：检测到 simple_camera_fb 正在运行，可能会占用摄像头。"
    fi

    if is_running "simplecamearafb"
    then
        echo "提醒：检测到 simplecamearafb 正在运行，可能会占用摄像头。"
    fi
}

# 准备 rkipc 运行需要的配置文件。
prepare_rkipc_config()
{
    if [ ! -x "${RKIPC_BIN}" ]
    then
        echo "错误：未找到可执行文件 ${RKIPC_BIN}"
        return 1
    fi

    if [ ! -d "${IQ_DIR}" ]
    then
        echo "错误：未找到 IQ 文件目录 ${IQ_DIR}"
        return 1
    fi

    if [ ! -f "${RKIPC_CONFIG}" ]
    then
        echo "未找到 ${RKIPC_CONFIG}，准备复制默认配置。"

        if [ ! -f "${RKIPC_DEFAULT_CONFIG}" ]
        then
            echo "错误：未找到默认配置 ${RKIPC_DEFAULT_CONFIG}"
            return 1
        fi

        cp -f "${RKIPC_DEFAULT_CONFIG}" "${RKIPC_CONFIG}"
    fi

    return 0
}

# 启动 rkipc，并保持其脱离当前 shell 运行。
start_rtsp()
{
    echo "=== LuckFox Pico RTSP 启动 ==="
    print_camera_owner_warning

    if is_running "rkipc"
    then
        echo "提醒：检测到 rkipc 已经在运行，本脚本不会重复启动。"
        print_view_usage
        return 0
    fi

    if ! prepare_rkipc_config
    then
        return 1
    fi

    echo "启动 rkipc..."
    nohup "${RKIPC_BIN}" -a "${IQ_DIR}" >"${LOG_FILE}" 2>&1 </dev/null &

    sleep 3

    if is_running "rkipc"
    then
        echo "rkipc 启动成功，日志文件：${LOG_FILE}"
        print_view_usage
        return 0
    fi

    echo "错误：rkipc 启动失败，请查看日志：${LOG_FILE}"
    sed -n '1,120p' "${LOG_FILE}" 2>/dev/null
    return 1
}

# 关闭 rkipc。
stop_rtsp()
{
    echo "=== LuckFox Pico RTSP 关闭 ==="

    if ! is_running "rkipc"
    then
        echo "rkipc 当前未运行。"
        return 0
    fi

    local rkipc_pid
    local wait_count

    rkipc_pid="$(pidof rkipc 2>/dev/null)"
    echo "正在关闭 rkipc，PID：${rkipc_pid}"
    kill ${rkipc_pid} 2>/dev/null

    wait_count=0
    while is_running "rkipc" && [ "${wait_count}" -lt 3 ]
    do
        sleep 1
        wait_count=$((wait_count + 1))
    done

    if is_running "rkipc"
    then
        echo "提醒：rkipc 仍在运行。"
        echo "如需强制关闭，请执行：sh /duck/test/start_rtsp.sh force-stop"
        return 1
    fi

    echo "rkipc 已关闭。"
    return 0
}

# 强制关闭 rkipc。
force_stop_rtsp()
{
    echo "=== LuckFox Pico RTSP 强制关闭 ==="

    if ! is_running "rkipc"
    then
        echo "rkipc 当前未运行。"
        return 0
    fi

    local rkipc_pid

    rkipc_pid="$(pidof rkipc 2>/dev/null)"
    echo "正在强制关闭 rkipc，PID：${rkipc_pid}"
    kill -9 ${rkipc_pid} 2>/dev/null
    sleep 1

    if is_running "rkipc"
    then
        echo "错误：rkipc 强制关闭失败。"
        return 1
    fi

    echo "rkipc 已强制关闭。"
    return 0
}

# 查看 rkipc 状态。
status_rtsp()
{
    echo "=== LuckFox Pico RTSP 状态 ==="
    print_camera_owner_warning

    if is_running "rkipc"
    then
        echo "rkipc 正在运行。"
        ps | grep "rkipc" | grep -v grep
        print_view_usage
        return 0
    fi

    echo "rkipc 当前未运行。"
    print_script_usage
    return 1
}

case "${1:-start}" in
    start)
        start_rtsp
        ;;
    stop)
        stop_rtsp
        ;;
    force-stop)
        force_stop_rtsp
        ;;
    status)
        status_rtsp
        ;;
    -h|--help|help)
        print_script_usage
        ;;
    *)
        echo "错误：未知命令 $1"
        print_script_usage
        exit 1
        ;;
esac

exit $?

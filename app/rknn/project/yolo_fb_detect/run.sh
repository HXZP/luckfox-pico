#!/bin/sh

# 切换到脚本所在目录，避免从其他路径执行时找不到模型和可执行文件。
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

APP="./yolo_fb_detect"
MODEL="./model/yolov5.rknn"
LOG_FILE="./stream.log"
PID_FILE="./stream.pid"
DEFAULT_MODE="parallel"
DEFAULT_INFERENCE_INTERVAL_MS="1000"
DEFAULT_DISPLAY_INTERVAL_MS="30"

export PATH=/oem/usr/bin:/usr/bin:/bin:$PATH
export LD_LIBRARY_PATH=/oem/usr/lib:$SCRIPT_DIR/lib:$LD_LIBRARY_PATH
export YOLO_CAMERA_ROTATION="${YOLO_CAMERA_ROTATION:-none}"

print_usage()
{
    echo "usage:"
    echo "  $0 start [serial <inference_interval_ms>]"
    echo "  $0 start [parallel <inference_interval_ms> <display_interval_ms>]"
    echo "  $0 restart [serial <inference_interval_ms>]"
    echo "  $0 restart [parallel <inference_interval_ms> <display_interval_ms>]"
    echo "  $0 stop"
    echo "  $0 status"
    echo "examples:"
    echo "  $0 start"
    echo "  $0 start serial 1000"
    echo "  $0 start parallel 1000 30"
}

build_app_args()
{
    mode="$1"

    if [ "$#" -eq 0 ]
    then
        APP_ARGS="$MODEL $DEFAULT_MODE $DEFAULT_INFERENCE_INTERVAL_MS $DEFAULT_DISPLAY_INTERVAL_MS"
        return 0
    fi

    case "$mode" in
        serial|sync)
            if [ "$#" -ne 2 ]
            then
                print_usage
                return 1
            fi
            APP_ARGS="$MODEL $mode $2"
            ;;
        parallel|async)
            if [ "$#" -ne 3 ]
            then
                print_usage
                return 1
            fi
            APP_ARGS="$MODEL $mode $2 $3"
            ;;
        *)
            print_usage
            return 1
            ;;
    esac

    return 0
}

# 检查记录的进程是否仍在运行。
is_running()
{
    if [ ! -f "$PID_FILE" ]
    then
        return 1
    fi

    pid="$(cat "$PID_FILE" 2>/dev/null)"
    if [ -z "$pid" ]
    then
        return 1
    fi

    if kill -0 "$pid" 2>/dev/null
    then
        return 0
    fi

    return 1
}

# 启动摄像头 YOLO 检测程序，并记录日志和进程号。
start_app()
{
    build_app_args "$@" || return 1

    if is_running
    then
        echo "already running, pid=$(cat "$PID_FILE")"
        echo "camera rotation: ${YOLO_CAMERA_ROTATION}"
        echo "stream url: http://10.8.49.116:8080/stream.mjpg"
        return 0
    fi

    if [ ! -x "$APP" ]
    then
        echo "app not executable: $APP"
        return 1
    fi

    if [ ! -f "$MODEL" ]
    then
        echo "model not found: $MODEL"
        return 1
    fi

    nohup "$APP" $APP_ARGS > "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
    sleep 1

    if is_running
    then
        echo "started, pid=$(cat "$PID_FILE")"
        echo "log: $SCRIPT_DIR/$LOG_FILE"
        echo "camera rotation: ${YOLO_CAMERA_ROTATION}"
        echo "stream url: http://10.8.49.116:8080/stream.mjpg"
        return 0
    fi

    echo "start failed, recent log:"
    tail -n 80 "$LOG_FILE"
    return 1
}

# 停止正在运行的检测程序，超时后强制结束。
stop_app()
{
    if ! is_running
    then
        echo "not running"
        rm -f "$PID_FILE"
        return 0
    fi

    pid="$(cat "$PID_FILE")"
    kill "$pid" 2>/dev/null

    i=0
    while [ "$i" -lt 10 ]
    do
        if ! kill -0 "$pid" 2>/dev/null
        then
            rm -f "$PID_FILE"
            echo "stopped"
            return 0
        fi

        i=$((i + 1))
        sleep 1
    done

    echo "stop timeout, force killing pid=$pid"
    kill -9 "$pid" 2>/dev/null
    rm -f "$PID_FILE"
    echo "stopped"
    return 0
}

# 查询检测程序当前运行状态。
status_app()
{
    if is_running
    then
        echo "running, pid=$(cat "$PID_FILE")"
        echo "camera rotation: ${YOLO_CAMERA_ROTATION}"
        echo "stream url: http://10.8.49.116:8080/stream.mjpg"
        return 0
    fi

    echo "not running"
    return 1
}

case "$1" in
    start)
        shift
        start_app "$@"
        ;;
    stop)
        stop_app
        ;;
    restart)
        shift
        stop_app
        start_app "$@"
        ;;
    status)
        status_app
        ;;
    "")
        start_app
        ;;
    *)
        print_usage
        exit 1
        ;;
esac

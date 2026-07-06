#!/bin/sh

WIFI_IF="wlan0"
WIFI_PERSIST_DIR="/userdata"
WIFI_PERSIST_CONF="${WIFI_PERSIST_DIR}/wpa_supplicant.conf"
WIFI_DEFAULT_CONF="/etc/wpa_supplicant.conf"
WIFI_LEGACY_CONF="/data/wpa_supplicant.conf"
WIFI_CONF="${WIFI_DEFAULT_CONF}"
WIFI_CONF_BAK="${WIFI_DEFAULT_CONF}.bak"
WPA_SUPPLICANT_BIN="wpa_supplicant"
WPA_CLI_BIN="wpa_cli"
UDHCPC_BIN="udhcpc"
SCAN_RESULT_FILE="/tmp/wifi_scan_result.$$"
SCAN_MAP_FILE="/tmp/wifi_scan_map.$$"
SAVED_RESULT_FILE="/tmp/wifi_saved_result.$$"
SAVED_MAP_FILE="/tmp/wifi_saved_map.$$"

export PATH="/oem/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin:${PATH}"

# 清理脚本运行过程中生成的临时文件。
cleanup_temp_files()
{
    rm -f "${SCAN_RESULT_FILE}" "${SCAN_MAP_FILE}" \
        "${SAVED_RESULT_FILE}" "${SAVED_MAP_FILE}"
}

# 打印脚本支持的命令说明。
print_usage()
{
    echo "使用方法："
    echo "  sh /root/tool/wifi_switch.sh"
    echo "  sh /root/tool/wifi_switch.sh status"
    echo "  sh /root/tool/wifi_switch.sh scan"
    echo "  sh /root/tool/wifi_switch.sh saved-list"
    echo "  sh /root/tool/wifi_switch.sh saved-id <网络ID>"
    echo "  sh /root/tool/wifi_switch.sh connect <SSID> [密码]"
    echo "说明：优先保存到 /userdata/wpa_supplicant.conf，便于重烧 rootfs/oem 后保留 WiFi 记忆。"
}

# 打印交互菜单。
print_main_menu()
{
    echo
    echo "================ WiFi 切换工具 ================"
    echo "1. 连接已经保存的 WiFi"
    echo "2. 扫描附近 WiFi 并连接"
    echo "3. 手动输入新的 WiFi 账号和密码"
    echo "4. 查看当前 WiFi 状态"
    echo "0. 退出"
    echo "==============================================="
}

# 等待 /userdata 挂载且可写。
wait_userdata_writable()
{
    local wait_count
    local test_file

    wait_count=0
    test_file="${WIFI_PERSIST_DIR}/.wifi_write_test"

    while [ "${wait_count}" -lt 5 ]
    do
        if grep -q " ${WIFI_PERSIST_DIR} " /proc/mounts 2>/dev/null &&
            touch "${test_file}" 2>/dev/null
        then
            rm -f "${test_file}"
            return 0
        fi

        sleep 1
        wait_count=$((wait_count + 1))
    done

    return 1
}

has_wifi_network_config()
{
    [ -f "$1" ] && grep -q "^[[:space:]]*network={" "$1"
}

get_current_wifi_ssid()
{
    "${WPA_CLI_BIN}" -i "${WIFI_IF}" status 2>/dev/null |
        awk -F '=' '/^ssid=/{print $2; exit}'
}

config_has_ssid()
{
    [ -f "$2" ] && awk -v target_ssid="$1" '
        /^[[:space:]]*ssid="/ {
            line = $0
            sub(/^[[:space:]]*ssid="/, "", line)
            sub(/"[[:space:]]*$/, "", line)
            if (line == target_ssid) {
                found = 1
                exit
            }
        }
        END { exit found ? 0 : 1 }
    ' "$2"
}

append_network_for_ssid()
{
    local target_ssid
    local source_conf
    local tmp_file

    target_ssid="$1"
    source_conf="$2"
    tmp_file="/tmp/wifi_network_block.$$"

    awk -v target_ssid="${target_ssid}" '
        /^[[:space:]]*network=\{/ {
            inside = 1
            block = $0 ORS
            matched = 0
            next
        }
        inside {
            block = block $0 ORS
            line = $0
            if (line ~ /^[[:space:]]*ssid="/) {
                sub(/^[[:space:]]*ssid="/, "", line)
                sub(/"[[:space:]]*$/, "", line)
                if (line == target_ssid) {
                    matched = 1
                }
            }
            if ($0 ~ /^[[:space:]]*\}/) {
                if (matched) {
                    printf "%s", block
                    found = 1
                    exit
                }
                inside = 0
                block = ""
                matched = 0
            }
        }
        END { exit found ? 0 : 1 }
    ' "${source_conf}" > "${tmp_file}" || {
        rm -f "${tmp_file}"
        return 1
    }

    if [ -s "${tmp_file}" ]
    then
        printf "\n" >> "${WIFI_CONF}"
        cat "${tmp_file}" >> "${WIFI_CONF}"
        rm -f "${tmp_file}"
        return 0
    fi

    rm -f "${tmp_file}"
    return 1
}

# 第一次使用 userdata 时，从旧位置迁移已经保存过的 WiFi 网络。
seed_persistent_wifi_config()
{
    local current_ssid
    local source_conf

    current_ssid="$(get_current_wifi_ssid)"
    if [ -n "${current_ssid}" ] &&
        ! config_has_ssid "${current_ssid}" "${WIFI_CONF}"
    then
        for source_conf in "${WIFI_LEGACY_CONF}" "${WIFI_DEFAULT_CONF}"
        do
            if [ "${source_conf}" != "${WIFI_CONF}" ] &&
                config_has_ssid "${current_ssid}" "${source_conf}"
            then
                if [ ! -f "${WIFI_CONF}" ]
                then
                    cp -f "${source_conf}" "${WIFI_CONF}"
                else
                    append_network_for_ssid "${current_ssid}" "${source_conf}"
                fi
                chmod 600 "${WIFI_CONF}" 2>/dev/null
                return 0
            fi
        done
    fi

    if has_wifi_network_config "${WIFI_CONF}"
    then
        return 0
    fi

    for source_conf in "${WIFI_LEGACY_CONF}" "${WIFI_DEFAULT_CONF}"
    do
        if [ "${source_conf}" != "${WIFI_CONF}" ] &&
            has_wifi_network_config "${source_conf}"
        then
            cp -f "${source_conf}" "${WIFI_CONF}"
            chmod 600 "${WIFI_CONF}" 2>/dev/null
            return 0
        fi
    done

    if [ ! -f "${WIFI_CONF}" ]
    then
        for source_conf in "${WIFI_LEGACY_CONF}" "${WIFI_DEFAULT_CONF}"
        do
            if [ "${source_conf}" != "${WIFI_CONF}" ] &&
                [ -f "${source_conf}" ]
            then
                cp -f "${source_conf}" "${WIFI_CONF}"
                chmod 600 "${WIFI_CONF}" 2>/dev/null
                return 0
            fi
        done
    fi

    return 1
}

# 选择 WiFi 配置文件。userdata 可写时优先使用持久配置。
select_wifi_config()
{
    if wait_userdata_writable
    then
        WIFI_CONF="${WIFI_PERSIST_CONF}"
        WIFI_CONF_BAK="${WIFI_PERSIST_CONF}.bak"
        seed_persistent_wifi_config >/dev/null 2>&1
    else
        WIFI_CONF="${WIFI_DEFAULT_CONF}"
        WIFI_CONF_BAK="${WIFI_DEFAULT_CONF}.bak"
    fi
}

# 确保 wpa_supplicant 基础配置存在。
ensure_wifi_base_config()
{
    if [ ! -f "${WIFI_CONF}" ]
    then
        cat > "${WIFI_CONF}" <<'EOF'
ctrl_interface=/var/run/wpa_supplicant
ap_scan=1
update_config=1
EOF
        chmod 600 "${WIFI_CONF}" 2>/dev/null
    fi

    if ! grep -q "^ctrl_interface=/var/run/wpa_supplicant$" "${WIFI_CONF}"
    then
        printf "ctrl_interface=/var/run/wpa_supplicant\n" >> "${WIFI_CONF}"
    fi

    if ! grep -q "^ap_scan=1$" "${WIFI_CONF}"
    then
        printf "ap_scan=1\n" >> "${WIFI_CONF}"
    fi

    if ! grep -q "^update_config=1$" "${WIFI_CONF}"
    then
        printf "update_config=1\n" >> "${WIFI_CONF}"
    fi

    chmod 600 "${WIFI_CONF}" 2>/dev/null
}

# 备份当前 WiFi 配置，便于问题回退。
backup_wifi_config()
{
    select_wifi_config

    if [ -f "${WIFI_CONF}" ]
    then
        cp -f "${WIFI_CONF}" "${WIFI_CONF_BAK}"
    fi
}

# 拉起无线网卡，避免后续命令执行失败。
bring_wifi_interface_up()
{
    ifconfig "${WIFI_IF}" up >/dev/null 2>&1
}

# 停掉旧的 WiFi 相关进程，保证重新连接过程干净。
stop_wifi_processes()
{
    killall -9 "${UDHCPC_BIN}" >/dev/null 2>&1
    killall -9 rkwifi_server >/dev/null 2>&1
    killall -9 "${WPA_SUPPLICANT_BIN}" >/dev/null 2>&1
    killall -9 wpa_supplicant_nl80211 >/dev/null 2>&1
}

# 等待 wpa_cli 可以正常控制 wpa_supplicant。
wait_wpa_ready()
{
    local wait_count

    wait_count=0
    while [ "${wait_count}" -lt 6 ]
    do
        if "${WPA_CLI_BIN}" -i "${WIFI_IF}" ping 2>/dev/null | grep -q "PONG"
        then
            return 0
        fi

        sleep 1
        wait_count=$((wait_count + 1))
    done

    echo "错误：wpa_supplicant 启动后未就绪。"
    return 1
}

# 等待保存网络完成关联，避免 DHCP 过早执行。
wait_wifi_connected()
{
    local wait_count
    local current_state

    wait_count=0
    while [ "${wait_count}" -lt 30 ]
    do
        current_state="$("${WPA_CLI_BIN}" -i "${WIFI_IF}" status 2>/dev/null | awk -F '=' '/^wpa_state=/{print $2; exit}')"
        if [ "${current_state}" = "COMPLETED" ]
        then
            return 0
        fi

        sleep 1
        wait_count=$((wait_count + 1))
    done

    echo "错误：等待 WiFi 连接完成超时。"
    return 1
}

# 重启 WiFi 连接栈，确保配置变更能立即生效。
restart_wifi_stack()
{
    select_wifi_config
    ensure_wifi_base_config
    bring_wifi_interface_up
    stop_wifi_processes
    mkdir -p /var/run/wpa_supplicant
    rm -f "/var/run/wpa_supplicant/${WIFI_IF}"

    if ! "${WPA_SUPPLICANT_BIN}" -B -i "${WIFI_IF}" -c "${WIFI_CONF}" >/dev/null 2>&1
    then
        echo "错误：启动 ${WPA_SUPPLICANT_BIN} 失败。"
        return 1
    fi

    wait_wpa_ready
}

# 执行返回 OK 的 wpa_cli 命令。
run_wpa_cli_ok()
{
    local command_output

    command_output="$("${WPA_CLI_BIN}" -i "${WIFI_IF}" "$@" 2>/dev/null)"

    if [ "${command_output}" != "OK" ]
    then
        echo "错误：执行 wpa_cli $* 失败，返回：${command_output}"
        return 1
    fi

    return 0
}

# 获取当前连接状态，方便确认连接结果。
print_wifi_status()
{
    local current_ssid
    local current_ip
    local current_state

    select_wifi_config
    current_ssid="$("${WPA_CLI_BIN}" -i "${WIFI_IF}" status 2>/dev/null | awk -F '=' '/^ssid=/{print $2; exit}')"
    current_ip="$(ifconfig "${WIFI_IF}" 2>/dev/null | awk '/inet addr:/{sub("addr:", "", $2); print $2; exit} /^ *inet /{print $2; exit}')"
    current_state="$("${WPA_CLI_BIN}" -i "${WIFI_IF}" status 2>/dev/null | awk -F '=' '/^wpa_state=/{print $2; exit}')"

    echo "当前接口：${WIFI_IF}"
    echo "配置文件：${WIFI_CONF}"
    echo "当前状态：${current_state:-UNKNOWN}"
    echo "当前 SSID：${current_ssid:-未连接}"
    echo "当前 IP：${current_ip:-未获取到}"
}

# 重新获取 DHCP 地址。
renew_wifi_ip()
{
    if ! wait_wifi_connected
    then
        return 1
    fi

    killall -9 "${UDHCPC_BIN}" >/dev/null 2>&1
    "${UDHCPC_BIN}" -i "${WIFI_IF}"
}

# 列出已经保存的 WiFi 配置。
list_saved_networks()
{
    local index
    local network_id
    local network_ssid
    local network_flags

    if ! restart_wifi_stack
    then
        return 1
    fi

    "${WPA_CLI_BIN}" -i "${WIFI_IF}" list_networks 2>/dev/null | \
        awk -F '\t' 'NF >= 2 && $1 ~ /^[0-9]+$/ {print $1 "|" $2 "|" $4}' > "${SAVED_RESULT_FILE}"

    if [ ! -s "${SAVED_RESULT_FILE}" ]
    then
        echo "未找到已保存的 WiFi 配置。"
        return 1
    fi

    : > "${SAVED_MAP_FILE}"
    index=1

    while IFS='|' read -r network_id network_ssid network_flags
    do
        printf "%s|%s|%s|%s\n" "${index}" "${network_id}" "${network_ssid}" "${network_flags}" >> "${SAVED_MAP_FILE}"
        echo "${index}. SSID=${network_ssid}  网络ID=${network_id}  标记=${network_flags:-无}"
        index=$((index + 1))
    done < "${SAVED_RESULT_FILE}"

    return 0
}

# 根据菜单序号获取保存网络对应的 network id。
get_saved_network_id_by_index()
{
    awk -F '|' -v target_index="$1" '$1 == target_index {print $2; exit}' "${SAVED_MAP_FILE}"
}

# 按 network id 连接已经保存的 WiFi。
connect_saved_network_by_id()
{
    local network_id

    network_id="$1"

    if [ -z "${network_id}" ]
    then
        echo "错误：未提供要连接的网络ID。"
        return 1
    fi

    backup_wifi_config

    if ! restart_wifi_stack
    then
        return 1
    fi

    if ! run_wpa_cli_ok select_network "${network_id}"
    then
        return 1
    fi

    if ! run_wpa_cli_ok enable_network all
    then
        return 1
    fi

    if ! run_wpa_cli_ok save_config
    then
        return 1
    fi

    renew_wifi_ip
    print_wifi_status
    return 0
}

# 交互式选择已经保存的 WiFi 并连接。
choose_saved_network()
{
    local menu_index
    local network_id

    if ! list_saved_networks
    then
        return 1
    fi

    printf "请输入要连接的序号："
    read menu_index

    network_id="$(get_saved_network_id_by_index "${menu_index}")"

    if [ -z "${network_id}" ]
    then
        echo "错误：无效的 WiFi 序号。"
        return 1
    fi

    connect_saved_network_by_id "${network_id}"
}

# 扫描附近的 WiFi，并整理成可选择的列表。
scan_wifi_networks()
{
    local index
    local wifi_ssid
    local wifi_signal
    local wifi_flags

    if ! restart_wifi_stack
    then
        return 1
    fi

    echo "开始扫描附近 WiFi，请稍候..."
    if ! run_wpa_cli_ok scan
    then
        return 1
    fi

    sleep 3

    "${WPA_CLI_BIN}" -i "${WIFI_IF}" scan_results 2>/dev/null | \
        awk '
            $1 ~ /^[0-9a-fA-F:]+$/ {
                ssid = ""
                for (i = 5; i <= NF; i++) {
                    ssid = ssid (i == 5 ? "" : " ") $i
                }
                if (ssid != "") {
                    print ssid "|" $3 "|" $4
                }
            }
        ' > "${SCAN_RESULT_FILE}"

    if [ ! -s "${SCAN_RESULT_FILE}" ]
    then
        echo "未扫描到可见的 WiFi。"
        return 1
    fi

    : > "${SCAN_MAP_FILE}"
    index=1

    while IFS='|' read -r wifi_ssid wifi_signal wifi_flags
    do
        printf "%s|%s|%s|%s\n" "${index}" "${wifi_ssid}" "${wifi_signal}" "${wifi_flags}" >> "${SCAN_MAP_FILE}"
        echo "${index}. SSID=${wifi_ssid}  信号=${wifi_signal} dBm  加密=${wifi_flags}"
        index=$((index + 1))
    done < "${SCAN_RESULT_FILE}"

    return 0
}

# 根据扫描结果序号获取 SSID。
get_scan_ssid_by_index()
{
    awk -F '|' -v target_index="$1" '$1 == target_index {print $2; exit}' "${SCAN_MAP_FILE}"
}

# 根据 SSID 查找已经保存的网络ID，避免重复创建。
find_network_id_by_ssid()
{
    "${WPA_CLI_BIN}" -i "${WIFI_IF}" list_networks 2>/dev/null | \
        awk -F '\t' -v target_ssid="$1" '$1 ~ /^[0-9]+$/ && $2 == target_ssid {print $1; exit}'
}

# 按照输入的 SSID 和密码创建或更新 WiFi 配置并连接。
connect_network_by_ssid()
{
    local wifi_ssid
    local wifi_psk
    local network_id

    wifi_ssid="$1"
    wifi_psk="$2"

    if [ -z "${wifi_ssid}" ]
    then
        echo "错误：SSID 不能为空。"
        return 1
    fi

    backup_wifi_config

    if ! restart_wifi_stack
    then
        return 1
    fi

    network_id="$(find_network_id_by_ssid "${wifi_ssid}")"

    if [ -z "${network_id}" ]
    then
        network_id="$("${WPA_CLI_BIN}" -i "${WIFI_IF}" add_network 2>/dev/null | awk 'NF > 0 {print $1; exit}')"
    fi

    case "${network_id}" in
        ""|FAIL*)
            echo "错误：创建新的 WiFi 网络配置失败。"
            return 1
            ;;
    esac

    if ! run_wpa_cli_ok set_network "${network_id}" ssid "\"${wifi_ssid}\""
    then
        return 1
    fi

    if [ -n "${wifi_psk}" ]
    then
        if ! run_wpa_cli_ok set_network "${network_id}" psk "\"${wifi_psk}\""
        then
            return 1
        fi

        if ! run_wpa_cli_ok set_network "${network_id}" key_mgmt WPA-PSK
        then
            return 1
        fi
    else
        if ! run_wpa_cli_ok set_network "${network_id}" key_mgmt NONE
        then
            return 1
        fi
    fi

    if ! run_wpa_cli_ok set_network "${network_id}" scan_ssid 1
    then
        return 1
    fi

    if ! run_wpa_cli_ok select_network "${network_id}"
    then
        return 1
    fi

    if ! run_wpa_cli_ok enable_network all
    then
        return 1
    fi

    if ! run_wpa_cli_ok reassociate
    then
        return 1
    fi

    if ! run_wpa_cli_ok save_config
    then
        return 1
    fi

    renew_wifi_ip
    print_wifi_status
    return 0
}

# 扫描后让用户选择 WiFi，并输入密码完成连接。
choose_scanned_network()
{
    local menu_index
    local wifi_ssid
    local wifi_psk

    if ! scan_wifi_networks
    then
        return 1
    fi

    printf "请输入要连接的序号，直接回车改为手动输入 SSID："
    read menu_index

    if [ -n "${menu_index}" ]
    then
        wifi_ssid="$(get_scan_ssid_by_index "${menu_index}")"
        if [ -z "${wifi_ssid}" ]
        then
            echo "错误：无效的扫描序号。"
            return 1
        fi
    else
        printf "请输入 WiFi 名称 SSID："
        read wifi_ssid
    fi

    printf "请输入 WiFi 密码，开放网络可直接回车："
    read wifi_psk

    connect_network_by_ssid "${wifi_ssid}" "${wifi_psk}"
}

# 让用户手工输入 SSID 和密码进行连接。
manual_connect_network()
{
    local wifi_ssid
    local wifi_psk

    printf "请输入 WiFi 名称 SSID："
    read wifi_ssid

    printf "请输入 WiFi 密码，开放网络可直接回车："
    read wifi_psk

    connect_network_by_ssid "${wifi_ssid}" "${wifi_psk}"
}

# 以交互菜单方式运行整个工具。
run_interactive_menu()
{
    local menu_choice

    while true
    do
        print_main_menu
        printf "请输入功能序号："
        read menu_choice

        case "${menu_choice}" in
            1)
                choose_saved_network
                ;;
            2)
                choose_scanned_network
                ;;
            3)
                manual_connect_network
                ;;
            4)
                print_wifi_status
                ;;
            0)
                echo "退出 WiFi 切换工具。"
                return 0
                ;;
            *)
                echo "错误：无效的菜单序号。"
                ;;
        esac
    done
}

trap cleanup_temp_files EXIT INT TERM

case "$1" in
    "")
        run_interactive_menu
        ;;
    status)
        restart_wifi_stack && print_wifi_status
        ;;
    scan)
        scan_wifi_networks
        ;;
    saved-list)
        list_saved_networks
        ;;
    saved-id)
        connect_saved_network_by_id "$2"
        ;;
    connect)
        connect_network_by_ssid "$2" "$3"
        ;;
    -h|--help|help)
        print_usage
        ;;
    *)
        echo "错误：未知命令 $1"
        print_usage
        exit 1
        ;;
esac

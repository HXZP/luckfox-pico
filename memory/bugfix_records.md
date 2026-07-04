# Bug 修复记录

本文件用于记录本仓库 bug 修复过程中的根因、解决方案和可复用规避规则。

## 记录模板

```md
## YYYY-MM-DD - [S1/high][fixed] 问题标题

- 模块：
- 现象：
- 根因：
- 解决方案：
- 验证方式：
- 相关文件：
  - `path/to/file`
- 规避规则：
- 标签：#tag
```

## 历史记录

## 2026-07-04 - [S2/medium][fixed] hxzp overlay Wi-Fi 记忆迁移到 userdata 持久化

- 模块：Buildroot `overlay-luckfox-buildroot-hxzp` Wi-Fi 启动 / `/etc/init.d/S99wlan0` / `/root/tool/wifi_switch.sh`
- 现象：需要将此前 Wi-Fi 多地点自连和分区放置方案落到 `overlay-luckfox-buildroot-hxzp`，避免保存的 Wi-Fi 只写在 rootfs 中，重烧 rootfs/oem 后丢失记忆。
- 根因：`hxzp` overlay 中 `S99wlan0` 和 `wifi_switch.sh` 默认使用 `/etc/wpa_supplicant.conf`；该文件位于 rootfs，重新烧录包含 rootfs 的镜像时会被覆盖，不适合作为长期保存 Wi-Fi 账号的位置。
- 解决方案：启动脚本和切换工具统一优先使用 `/userdata/wpa_supplicant.conf`，`/userdata` 未挂载或不可写时退回 `/etc/wpa_supplicant.conf`；如果系统已有 `wpa_supplicant` 在运行，先执行 `wpa_cli save_config` 将当前已连接/已配置 Wi-Fi 刷回原配置文件，再按当前 SSID 从 `/data/wpa_supplicant.conf` 或 `/etc/wpa_supplicant.conf` 迁移对应 `network` 块到 `/userdata`，避免覆盖已有多地点网络；开机连接成功后再次执行 `save_config`，确保当前网络写入持久分区；在 `overlay-luckfox-buildroot-hxzp/userdata/wpa_supplicant.conf` 中预置 `HXZP` 与 `acemate`，并扩展 `luckfox-userdata-pre.sh` 将各 overlay 下的 `userdata/` 目录拷入 `userdata.img`，让新烧录设备首次启动即可从预置 Wi-Fi 中自动选择可用网络。
- 验证方式：本地执行 `sh -n project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-hxzp/etc/init.d/S99wlan0`、`sh -n project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-hxzp/root/tool/wifi_switch.sh` 和 `bash -n project/cfg/BoardConfig_IPC/luckfox-userdata-pre.sh` 均通过；使用临时 `RK_PROJECT_PACKAGE_USERDATA_DIR` 模拟运行 `luckfox-userdata-pre.sh`，确认能从 `overlay-luckfox-buildroot-hxzp/userdata/` 拷贝 `wpa_supplicant.conf`，且文件包含 `HXZP` 与 `acemate` 两个 enabled network 块、没有 `disabled=1`；尚未重新打包固件并上板重启验证实际持久化自连。
- 相关文件：
  - `project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-hxzp/etc/init.d/S99wlan0`
  - `project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-hxzp/root/tool/wifi_switch.sh`
  - `project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-hxzp/userdata/wpa_supplicant.conf`
  - `project/cfg/BoardConfig_IPC/luckfox-userdata-pre.sh`
  - `project/cfg/BoardConfig_IPC/BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_Zero-IPC.mk`
- 规避规则：保存型 Wi-Fi 配置不要长期只放 rootfs；有 userdata 分区时优先放 `/userdata/wpa_supplicant.conf`，并在启动脚本和手动切换工具中使用同一份配置，避免开机读取与手动保存分裂。
- 标签：#wifi #userdata #buildroot #overlay #persistence

## 2026-07-04 - [S2/medium][fixed] Wi-Fi 多地点保存网络开机自连逻辑加固

- 模块：Buildroot overlay Wi-Fi 启动 / `/etc/init.d/S99wlan0` / `/root/tool/wifi_switch.sh`
- 现象：用户确认 `S99wlan0` 是否需要 `.sh` 后缀，并指出现有启动脚本看不出会在两个地点切换后自动连接已保存 Wi-Fi。
- 根因：`S99wlan0` 不需要 `.sh` 后缀，Buildroot `rcS` 会执行 `/etc/init.d/S??* start`；但旧脚本只在 `wpa_supplicant` 不存在时启动并立刻执行 `udhcpc`，没有等待保存网络连接完成，且 `wifi_switch.sh` 使用 `select_network` 后可能导致其它已保存网络被禁用，影响多地点自动漫游。
- 解决方案：重写 overlay 中 `S99wlan0` 启动逻辑，确保基础 wpa 配置存在、拉起 `wlan0`、启动或复用 `wpa_supplicant`、`enable_network all`、`reassociate`、等待 `wpa_state=COMPLETED` 后再执行 DHCP；同步调整 `wifi_switch.sh`，连接保存/新增网络时优先选中当前网络，但保存前重新启用所有网络，并在 DHCP 前等待连接完成。
- 验证方式：本地执行 `sh -n project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-duck/etc/init.d/S99wlan0` 和 `sh -n project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-duck/root/tool/wifi_switch.sh` 均通过；尚未重新打包固件并上板重启验证实际多地点自连。
- 相关文件：
  - `project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-duck/etc/init.d/S99wlan0`
  - `project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-duck/root/tool/wifi_switch.sh`
- 规避规则：Buildroot init 脚本不依赖 `.sh` 后缀，关键是 `S??*` 命名和可执行权限；多地点 Wi-Fi 自连必须保留多个 network block 为 enabled，避免连接某个 SSID 后永久禁用其它保存网络，DHCP 应在 `wpa_state=COMPLETED` 后执行。
- 标签：#wifi #wpa-supplicant #buildroot #boot #roaming

## 2026-07-04 - [S2/medium][accepted] BMI088 1kHz pose 用户态链路验证

- 模块：`app/imu_pose` / BMI088 姿态融合
- 现象：需要参考 `acemate-mcu` 中 IMU pose 融合方案，在 RK 侧写 1kHz 更新应用，并确认频率是否稳定。
- 根因：姿态融合计算本身耗时很低，1kHz 余量充足；限制点在 Linux 用户态调度和 BMI088 I2C 读取。当前板端设备树已将 BMI088 绑定到内核 `bmi088_i2c`，普通 `/dev/i2c-2` 访问返回 `Device or resource busy`；IIO scan/buffer 路径启用时返回 `Invalid argument`，当前系统未直接形成可用的连续缓冲触发链路。使用实验性 `I2C_SLAVE_FORCE` 直连时，400kHz I2C 读取六轴平均约 496us，约占 1ms 周期一半，存在毫秒级偶发抖动，因此只能作为软实时验证，不能作为硬实时保证。
- 解决方案：新增 `imu_pose` 应用，移植 acemate 的 Mahony 6轴融合、加速度置信度门控、静止陀螺自动校准和 1kHz 绝对时间循环；增加 `--simulate` 测算法/调度开销，增加显式 `--i2c-force` 用于受控实验验证，默认遇到内核驱动占用时提示原因；补充 README 说明用法、限制和长期建议。
- 验证方式：本地 `make -C app/imu_pose host` 与 `make -C app/imu_pose` 均通过；板端 `192.168.2.16` 执行 `/userdata/imu_pose --simulate --duration 5 --report-ms 1000 --verbose`，前四个窗口稳定 1000.0Hz、周期约 950-1055us、融合平均约 0.8us，最后窗口出现一次 3008us 抖动；普通 `/userdata/imu_pose --duration 1 --verbose` 返回 `Device or resource busy` 并提示内核驱动占用；`/userdata/imu_pose --i2c-force --no-auto-offset --duration 8 --report-ms 1000 --verbose` 基本维持 1000Hz，读数平均约 496us、融合平均约 3us，8秒内一次窗口出现 `max=2703.8us` 且 `late=1`。
- 相关文件：
  - `app/imu_pose/imu_pose.c`
  - `app/imu_pose/Makefile`
  - `app/imu_pose/README.md`
- 规避规则：验证 BMI088 1kHz 时先检查 `/sys/bus/i2c/devices/2-0018` 是否绑定 `bmi088_i2c`；默认不要在生产路径使用 `I2C_SLAVE_FORCE` 抢内核驱动。若要长期稳定 1kHz，优先补齐 BMI088 data-ready 中断/IIO buffered trigger、使用内核推样路径、改 SPI/更高速 I2C，或把 1kHz 采样融合放到 MCU。
- 标签：#imu #bmi088 #i2c #iio #pose #runtime-verification #soft-realtime

## 2026-07-04 - [S3/low][fixed] protocol_tool 串口 termios 构建兼容性修复

- 模块：`app/protocol_tool`
- 现象：新增协议工具后执行 `make -C app/protocol_tool host` 失败，编译器提示 `cfmakeraw` 隐式声明、`CRTSCTS` 未定义，并对 `usleep` 给出隐式声明警告。
- 根因：源文件定义 `_POSIX_C_SOURCE=200809L` 后，glibc/uClibc 下部分非 POSIX 扩展接口或宏不会稳定暴露；`cfmakeraw` 和 `CRTSCTS` 属于实现相关扩展，直接依赖会影响 host 和目标 libc 的可移植构建。
- 解决方案：改为手动设置 raw termios 位，`CRTSCTS` 只在宏存在时清除；将重试等待从 `usleep()` 改为 POSIX `nanosleep()` 包装；同时修正 UART 帧长度比较的 signedness 警告。
- 验证方式：`make -C app/protocol_tool host` 通过；`app/protocol_tool/build/protocol_tool-host selftest` 通过；`make -C app/protocol_tool` 使用 Luckfox uClibc 交叉工具链编译通过；上传 `/userdata/protocol_tool` 后板端 `selftest` 通过，并通过 `/dev/ttyS0` 成功执行 `uart version` 和 `uart ping`。
- 相关文件：
  - `app/protocol_tool/protocol_tool.c`
  - `app/protocol_tool/Makefile`
- 规避规则：面向 BusyBox/uClibc 小系统的串口工具尽量使用 POSIX termios 基础位手动配置 raw 模式；非标准流控宏如 `CRTSCTS` 必须用 `#ifdef` 保护，短等待优先用 `nanosleep()`。
- 标签：#protocol-tool #uart #termios #uclibc #build

## 2026-07-04 - [S3/low][accepted] 大灯程序退出自动回到关灯 duty 验证通过

- 模块：`app/headlight_control`
- 现象：用户询问大灯控制程序退出时是否会自动把亮度设为 `0`，需要确认自然结束和手动中断两种退出路径是否都会回到关灯状态。
- 根因：未发现新的异常。当前程序退出清理逻辑会对 active-low 大灯写入关灯亮度对应的 duty，并保持 PWM 使能，避免释放 PWM 后引脚回到点亮电平。
- 解决方案：无需修改代码。维持当前 active-low 关灯策略：退出时写入 `duty_cycle=period`，并保持 `enable=1`。
- 验证方式：板端 `192.168.2.16` 执行 `/root/headlight_control --verbose blink --brightness 35 --repeat 2 --on-ms 120 --off-ms 120` 自然结束后，`/root/headlight_control status` 显示 `period_ns: 1000000`、`duty_cycle_ns: 1000000`、`enable: 1`；执行 `/root/headlight_control --verbose breathe --brightness 35 --repeat -1 --step-ms 8 --hold-ms 80` 后用 `Ctrl+C` 中断，再次读取状态仍为 `period_ns: 1000000`、`duty_cycle_ns: 1000000`、`enable: 1`。
- 相关文件：
  - `app/headlight_control/headlight_control.c`
  - `app/headlight_control/README.md`
  - `app/document/rk_hardware_topology.md`
- 规避规则：active-low PWM 负载的退出清理不能只看 `enable` 是否为 `0`；实际关灯状态应以硬件安全电平为准。本项目大灯关灯状态为 `enable=1` 且 `duty_cycle=period`。
- 标签：#headlight #pwm #active-low #cleanup #runtime-verification

## 2026-07-04 - [S2/medium][fixed] 大灯 set 0 时 disable PWM 导致反而最亮

- 模块：`app/headlight_control`
- 现象：用户测试发现 `./headlight_control set 0` 时大灯亮度仍为最大；此前程序虽然把 `0` 换算成关灯 duty，但随后关闭 PWM 输出，导致实际硬件回到点亮状态。
- 根因：大灯为 active-low 驱动，`duty_cycle=period` 且 PWM 保持使能时才是关灯高电平；disable PWM 后引脚进入默认/空闲状态，该状态在当前硬件上会点亮大灯。
- 解决方案：修改关灯和清理策略：`brightness=0`、`off`、默认退出清理都保持 PWM `enable=1`，并写入 `duty_cycle=period`；active-low 默认下不再自动 unexport PWM，避免引脚释放后回到点亮电平；同步更新 README 和硬件拓扑说明。
- 验证方式：本地 `make -C app/headlight_control host` 与 `make -C app/headlight_control` 均通过；上传新版到 `/userdata/headlight_control` 并复制到 `/root/headlight_control`；板端执行 `set 0` 后 `status` 显示 `period_ns: 1000000`、`duty_cycle_ns: 1000000`、`enable: 1`；执行 `set 35` 后 `duty_cycle_ns: 650000`、`enable: 1`；执行 `off` 后仍为 `duty_cycle_ns: 1000000`、`enable: 1`，保持关灯电平。
- 相关文件：
  - `app/headlight_control/headlight_control.c`
  - `app/headlight_control/README.md`
  - `app/document/rk_hardware_topology.md`
- 规避规则：active-low PWM 控制负载时，不能假设 disable PWM 等价于关闭负载；必须确认 disable 后的引脚默认电平。若默认电平会打开负载，关断路径应保持 PWM 使能并输出安全 duty。
- 标签：#headlight #pwm #active-low #gpio-default #runtime-verification

## 2026-07-04 - [S2/medium][fixed] 大灯亮度参数与 PWM 极性相反

- 模块：`app/headlight_control`
- 现象：用户测试发现大灯硬件表现为 `0` 最亮、`100` 最暗，和命令行亮度参数的自然语义相反；示例中的 `set 35`、`--brightness 80` 容易被误解。
- 根因：大灯 PWM 驱动链路是低 duty 更亮的 active-low 行为；原程序直接使用 `brightness%` 作为 `duty_cycle/period`，导致用户参数越大，硬件越暗。
- 解决方案：程序默认按 active-low PWM 处理，命令行参数保持 `0=关/最暗，100=最亮`，内部换算为 `duty_percent = 100 - brightness`；初始化和退出关灯逻辑也改为写入关灯亮度对应的 duty，再 disable，避免反相后退出瞬间变亮；同步更新 README 和硬件拓扑说明。
- 验证方式：本地 `make -C app/headlight_control host` 与 `make -C app/headlight_control` 均通过；上传新版到 `/userdata/headlight_control` 并复制到 `/root/headlight_control`；板端执行 `set 35` 后 `status` 显示 `period_ns: 1000000`、`duty_cycle_ns: 650000`、`enable: 1`；执行 `set 100` 后 `duty_cycle_ns: 0`、`enable: 1`；执行 `off` 后 `duty_cycle_ns: 1000000`、`enable: 0`；`blink --brightness 35 --repeat 3` 正常执行，最终确认关灯。
- 相关文件：
  - `app/headlight_control/headlight_control.c`
  - `app/headlight_control/README.md`
  - `app/document/rk_hardware_topology.md`
- 规避规则：面对外部硬件亮度控制时，CLI/API 参数应表达用户感知亮度，而不是裸 PWM duty；如果硬件是 active-low，必须在驱动适配层反向换算，并在关灯路径写入安全 duty 后再 disable。
- 标签：#headlight #pwm #active-low #runtime-verification

## 2026-07-04 - [S2/medium][fixed] 大灯 PWM 控制程序初始化误报 EIO

- 模块：`app/headlight_control`
- 现象：大灯控制程序能自动识别 `pwmchip10 -> ff490020.pwm`，`status` 可读取 `/sys/class/pwm/pwmchip10/pwm0`，但执行 `off`、`blink`、`breathe`、`sos` 均报 `headlight: failed to init PWM /sys/class/pwm/pwmchip10 channel 0: Input/output error`。
- 根因：程序初始化时先写 `enable=0`，板端 PWM sysfs 在 `period=0` 的初始状态下对 `enable=0` 返回 `EINVAL`；但 `write_file()` 在 `write()` 失败后没有保留真实 `errno`，统一覆盖成 `EIO`，导致 `pwm_init()` 中用于忽略 `EINVAL` 的兼容逻辑失效。
- 解决方案：修改 `write_file()`，当 `write()` 返回负值时先保存真实 `errno`，关闭 fd 后恢复该错误码；保留短写时才设置 `EIO`。这样 `pwm_init()` 能正确识别并忽略初始 `enable=0` 的 `EINVAL`，随后配置 `period`、`duty_cycle`、`enable`。
- 验证方式：本地 `make -C app/headlight_control host` 与 `make -C app/headlight_control` 均通过；上传新版 `/userdata/headlight_control` 到板端后，手动确认 `/sys/class/pwm/pwmchip10 -> ../../devices/platform/ff490020.pwm/pwm/pwmchip10`；执行 `off`、`blink --brightness 20 --repeat 3`、`breathe --brightness 25 --repeat 1`、`sos --brightness 20 --repeat 1`、`strobe --brightness 15 --repeat 5` 均成功；最终 `status` 显示 `period_ns: 1000000`、`duty_cycle_ns: 0`、`enable: 0`。
- 相关文件：
  - `app/headlight_control/headlight_control.c`
  - `app/headlight_control/Makefile`
  - `app/headlight_control/README.md`
- 规避规则：写 sysfs 控制文件时必须保留 `write()` 失败的真实 `errno`，不要把所有失败统一覆盖为 `EIO`；PWM 初始化时先配置 `period`/`duty_cycle` 再开启，初始 `enable=0` 在部分驱动状态下可能返回 `EINVAL`。
- 标签：#headlight #pwm #sysfs #runtime-verification

## 2026-07-04 - [S0/blocking][fixed] 板子重启后 YOLO 启动验证通过

- 模块：板端连接 / YOLO 启动验证
- 现象：上一轮 `192.168.2.16` 网络不可达导致无法启动 YOLO；用户重启板子后，需要重新验证网络和 YOLO 运行状态。
- 根因：重启后板子重新接入 `192.168.2.0/24` 网络，`192.168.2.16` 恢复可达。上一轮不可达的直接原因是板端链路未在线，未进一步定位到具体供电、Wi-Fi 或 DHCP 子原因。
- 解决方案：无需修改代码。通过 SSH 登录 `192.168.2.16`，在 `/userdata/yolo_fb_detect_demo` 执行 `./run.sh start` 启动 YOLO。
- 验证方式：`ping -c 4 -W 2 192.168.2.16` 4/4 收包、0% 丢包；`./run.sh start` 输出 `started, pid=1596`、`camera rotation: none`、`stream url: http://10.8.49.116:8080/stream.mjpg`；`./run.sh status` 输出 `running, pid=1596`；`stream.log` 显示 `frame=... mode=parallel ... ret=0 detect_count=1` 和 `person @ ...` 检测结果。
- 相关文件：
  - `/userdata/yolo_fb_detect_demo/run.sh`
  - `/userdata/yolo_fb_detect_demo/stream.log`
- 规避规则：板子重启或网络恢复后，先用 ping 确认链路，再启动服务并同时检查脚本状态与业务日志；YOLO 启动成功以进程 `running` 和日志内 `ret=0`/检测输出共同确认。
- 标签：#yolo #ssh #network #runtime-verification

## 2026-07-04 - [S0/blocking][investigating] 板子网络不可达导致 YOLO 启动验证受阻

- 模块：板端连接 / YOLO 启动验证
- 现象：尝试通过 `192.168.2.16` 登录板子执行 `/userdata/yolo_fb_detect_demo/run.sh start` 时，SSH 返回 `No route to host`；随后 ping `192.168.2.16` 和备用 `172.32.0.93` 均无回包，无法在板端实际启动或验证 YOLO。
- 根因：investigating。本机 `eth0` 处于 `192.168.2.10/24`，到 `192.168.2.0/24` 路由存在，网关 `192.168.2.1` 可达；但 `ip neigh show` 中 `192.168.2.16 dev eth0 FAILED`，说明当前局域网内未解析到板子，可能是板子未上电、Wi-Fi 未连接、IP 变化或链路断开，尚未上板确认。
- 解决方案：暂无代码修改。需要先恢复板子网络可达性或确认新 IP，再重新执行 YOLO 启动命令并查看 `stream.log`。
- 验证方式：`ssh root@192.168.2.16` 返回 `No route to host`；`ping -c 4 -W 2 192.168.2.16` 返回 `Destination Host Unreachable`；`ping -c 2 -W 2 172.32.0.93` 100% 丢包；`ip -br addr` 显示本机 `eth0` 为 `192.168.2.10/24`；`ip route` 显示 `192.168.2.0/24 dev eth0`。
- 相关文件：
  - `/userdata/yolo_fb_detect_demo/run.sh`
  - `/userdata/yolo_fb_detect_demo/stream.log`
- 规避规则：远程启动板端服务前先确认板子 IP 可 ping/SSH；若 `ip neigh` 为 `FAILED`，优先排查板子供电、Wi-Fi 连接、DHCP 地址变化和 USB/RNDIS 链路。
- 标签：#yolo #ssh #network #runtime-verification

## 2026-07-03 - [S3/low][fixed] YOLO 相机默认方向改回横向不旋转

- 模块：`app/rknn/project/yolo_fb_detect`
- 现象：根据桌面机器人摄像头安装方案调整，决定默认采用横向摆放摄像头，不再默认补偿逆时针 90 度安装。
- 根因：前一版默认 `YOLO_CAMERA_ROTATION=cw90` 适合逆时针 90 度安装；改回横向摆放后，默认旋转会导致画面方向反而不符合实际安装。
- 解决方案：保留 `YOLO_CAMERA_ROTATION` 手动配置能力，但将源码默认值和启动脚本默认值从 `cw90` 改为 `none`；非法配置 fallback 也改为 `none`。
- 验证方式：本地重新交叉编译 `yolo_fb_detect` 成功；上传新版二进制和 `run.sh` 到 `/userdata/yolo_fb_detect_demo` 后启动成功，脚本输出 `camera rotation: none`，日志显示 `YOLO fb mode=parallel, camera_rotation=none` 和 `mjpeg stream url: http://0.0.0.0:8080/stream.mjpg`，进程保持 `running`。
- 相关文件：
  - `app/rknn/project/yolo_fb_detect/src/main.cc`
  - `app/rknn/project/yolo_fb_detect/run.sh`
  - `app/rknn/install/uclibc/yolo_fb_detect/run.sh`
  - `app/rknn/install/uclibc/yolo_fb_detect_demo/run.sh`
- 规避规则：相机安装方向变更时优先调整默认启动参数，不删除运行时方向开关；部署前后都要检查脚本输出和程序日志中的 `camera_rotation` 是否一致。
- 标签：#yolo #camera #rotation #deployment

## 2026-07-03 - [S2/medium][fixed] YOLO 相机逆时针 90 度安装导致画面方向不正

- 模块：`app/rknn/project/yolo_fb_detect`
- 现象：当前摄像头物理逆时针 90 度安装，原始采集画面方向与实际观看方向不一致，YOLO 显示、MJPEG 和检测框需要在同一方向坐标系下工作。
- 根因：原程序直接使用 OpenCV 采集到的原始帧进行推理、绘制、保存和 framebuffer 输出，没有针对摄像头安装角度做旋转补偿；90 度旋转后画面宽高互换，如果直接写入 `240x135` framebuffer 会被裁切。
- 解决方案：新增 `YOLO_CAMERA_ROTATION` 配置，默认 `cw90` 补偿逆时针 90 度安装；采集后先旋转为正向帧，再进入推理、检测框映射、绘制、保存和 MJPEG；framebuffer 输出改为等比缩放居中并清黑边，避免竖向画面被横屏裁切；同步更新板端启动脚本默认使用 `parallel 1000 30` 和 `YOLO_CAMERA_ROTATION=cw90`。
- 验证方式：本地使用 `build_yolo_fb_rotation_sdk` 交叉编译 `yolo_fb_detect` 成功；上传新版二进制和 `run.sh` 到 `/userdata/yolo_fb_detect_demo` 后启动成功，日志显示 `YOLO fb mode=parallel, camera_rotation=cw90`、`Framebuffer: 240x135`、`mjpeg stream url: http://0.0.0.0:8080/stream.mjpg`，推理线程日志显示 `parallel inference ... ret=0`，进程保持 `running`。
- 相关文件：
  - `app/rknn/project/yolo_fb_detect/src/main.cc`
  - `app/rknn/project/yolo_fb_detect/run.sh`
  - `app/rknn/install/uclibc/yolo_fb_detect/run.sh`
  - `app/rknn/install/uclibc/yolo_fb_detect_demo/run.sh`
- 规避规则：处理相机旋转安装时，应在采集后统一生成“已校正帧”，后续推理、显示、保存和串流全部使用该帧，避免检测框坐标和显示坐标分叉；90 度旋转后写横向 framebuffer 必须等比缩放居中或重新配置输出分辨率。
- 标签：#yolo #camera #rotation #framebuffer #mjpeg

## 2026-07-03 - [S3/low][accepted] Wi-Fi 保存配置开机自连验证通过

- 模块：板端 Wi-Fi 启动 / `/root/tool/wifi_switch.sh`
- 现象：用户要求登录 `192.168.2.16` 检查 `./tool/wifi_switch.sh`，确认开机后是否会自动连接保存的 Wi-Fi。
- 根因：未发现自连异常。板端保存配置存在，`/etc/wpa_supplicant.conf`、`/userdata/wpa_supplicant.conf`、`/data/wpa_supplicant.conf` 内容一致；开机后 `wpa_supplicant` 使用 `/data/wpa_supplicant.conf` 连接保存的 `HXZP` 网络。
- 解决方案：无需修改。现有启动链路会在开机后拉起 Wi-Fi 并连接保存配置；`/etc/init.d/S99wlan0` 也具备兜底启动 `wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf` 和 `udhcpc -i wlan0` 的逻辑。
- 验证方式：通过 SSH 登录板端执行重启；重启后重新 ping/SSH 成功，`wpa_cli -i wlan0 status` 显示 `ssid=HXZP`、`wpa_state=COMPLETED`、`ip_address=192.168.2.16`，`wpa_cli -i wlan0 list_networks` 显示 `HXZP` 为 `[CURRENT]`。
- 相关文件：
  - `/root/tool/wifi_switch.sh`
  - `/etc/init.d/S99wlan0`
  - `/oem/usr/ko/insmod_wifi.sh`
  - `/etc/wpa_supplicant.conf`
  - `/userdata/wpa_supplicant.conf`
- 规避规则：验证开机 Wi-Fi 自连时，不只检查 `S99wlan0`，还要确认驱动脚本和 `rkwifi_server` 是否提前启动了 `wpa_supplicant`；同时比对 `/etc`、`/userdata`、`/data` 下的 wpa 配置是否一致。
- 标签：#wifi #wpa-supplicant #boot #runtime-verification

## 2026-06-16 - [S1/high][mitigated] NPU 固定 700MHz 可能导致摄像头并发推理提交失败

- 模块：kernel dts / rknpu
- 现象：摄像头持续采样并发 NPU 推理时，`rknn_run()` 偶发或首帧触发 `E RKNN: failed to submit!`，失败耗时约 6 秒，并可能导致板端 SSH/ping 无响应。
- 根因：厂家反馈当前板型 DTS 中固定 NPU 频率 `assigned-clock-rates = <700000000>;` 可能引发该并发稳定性问题；该根因仍需上板运行验证。
- 解决方案：按厂家建议注释当前板型 `&npu` 节点中的固定 700MHz 频率配置，让 NPU 使用默认频率策略。
- 验证方式：已执行 `./build.sh kernel`，DTC 生成 `rv1106g-luckfox-pico-zero.dtb` 成功，`boot.img` 构建成功；尚未烧录/部署到板端验证运行稳定性。
- 相关文件：
  - `sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-zero.dts`
- 规避规则：遇到 RKNPU submit timeout/failed to submit 时，除模型和 runtime 版本外，还要检查板级 DTS 是否固定了 NPU 频率、电源和时钟策略。
- 标签：#npu #rknn #device-tree #clock #vendor-suggestion

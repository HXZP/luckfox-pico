# 协议工具

`protocol_tool` 根据 `duck-mid-f103/docs/uart_protocol.md` 和
`duck-mid-f103/docs/can_protocol.md` 实现：

- UART 线协议构帧、解帧、CRC16-CCITT 校验。
- RK 通过 UART0 访问 STM32 的 `PING` 和 `GET_VERSION` 通路测试。
- STM32 WS2812 双灯 `LED_ACTION_ENQUEUE` 入队和事件解码。
- RK 大灯 PWM + STM32 尾部双 RGB 的开机灯效编排。
- CAN 标准帧 ID 分类和 8 字节数据语义解析。
- 常用 CAN 命令数据构造。

当前 UART 业务协议版本为 `duck-mid-f103 0.6.0`。新增 LED 动作队列命令：

| 命令 | 说明 |
| --- | --- |
| `0x0110` | `LED_ACTION_ENQUEUE`，提交 WS2812 常亮/闪烁动作 |
| `0x0111` | `LED_ACTION_EVENT`，下位机主动上报动作完成/取消/失败 |

## 编译

主机自测版本：

```sh
make -C app/protocol_tool host
app/protocol_tool/build/protocol_tool-host selftest
```

RK 板端版本：

```sh
make -C app/protocol_tool
scp app/protocol_tool/build/protocol_tool root@192.168.2.16:/userdata/
ssh root@192.168.2.16 'chmod +x /userdata/protocol_tool'
```

## UART 通路测试

当前硬件拓扑中 RK UART0 连接 STM32，板端表现为 `/dev/ttyS0`，波特率 `1000000`。

```sh
/userdata/protocol_tool uart version --port /dev/ttyS0 --baud 1000000 --seq 7 --verbose
/userdata/protocol_tool uart ping --port /dev/ttyS0 --baud 1000000 --seq 8 --verbose --payload 11 22 33 44
```

也可以用通用请求命令测试任意已注册命令：

```sh
/userdata/protocol_tool uart request 0x0002 --port /dev/ttyS0 --baud 1000000 --seq 9 --verbose
```

## 开机灯效

默认灯效：

1. 大灯 `1000 ms` 渐变到 `5%` 亮度。
2. 大灯熄灭 `300 ms`。
3. 大灯和尾部两个 RGB 灯一起快速闪烁 `5` 次，每次亮 `50 ms`、灭 `50 ms`。
4. 全部熄灭。

尾部 RGB 默认颜色为偏黄暖白 `RGB(255, 220, 140)`；大灯使用 RK 侧 `PWM10-M2`
控制，默认按设备名 `ff490020` 自动匹配 PWM chip。

```sh
/userdata/protocol_tool led boot --port /dev/ttyS0 --baud 1000000 --verbose
```

调试参数：

```sh
/userdata/protocol_tool led boot --tail-rgb 255 240 180 --brightness 4
/userdata/protocol_tool led boot --no-headlight
/userdata/protocol_tool led boot --no-tail
```

hxzp overlay 中已加入开机脚本：

```text
project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-buildroot-hxzp/etc/init.d/S30bootlight
```

它会在启动时后台等待 `/userdata/protocol_tool` 可执行，然后运行默认开机灯效，
日志写入 `/tmp/bootlight.log`。

## UART 离线构帧/解帧

```sh
app/protocol_tool/build/protocol_tool-host uart build 0x0002 7
app/protocol_tool/build/protocol_tool-host uart decode A5 5A 01 00 02 00 07 00 00 00 80 AC
```

## CAN 帧解析/构造

解析未配置电机发现上报：

```sh
app/protocol_tool/build/protocol_tool-host can decode 0x27e 60 00 7e 78 56 34 12 00
```

解析已配置电机主动上报：

```sh
app/protocol_tool/build/protocol_tool-host can decode 0x202 10 27 00 00 20 4E 00 00
```

构造 yaw 节点 `0x02` 的速度目标命令，标准帧 ID 为 `0x100 + 0x02 = 0x102`：

```sh
app/protocol_tool/build/protocol_tool-host can build 0x102 0x03 12000
```

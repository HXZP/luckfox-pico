# 协议工具

`protocol_tool` 根据 `app/document/uart_protocol.md` 和 `app/document/can_protocol.md` 实现：

- UART 线协议构帧、解帧、CRC16-CCITT 校验。
- RK 通过 UART0 访问 STM32 的 `PING` 和 `GET_VERSION` 通路测试。
- CAN 标准帧 ID 分类和 8 字节数据语义解析。
- 常用 CAN 命令数据构造。

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

本次实测结果：

- `GET_VERSION` 返回 `duck-mid-f103 0.2.0`，状态码 `0`。
- `PING` 返回状态码 `0`，并原样回显 `11 22 33 44`。

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

构造 yaw 节点 `0x02` 的速度目标命令，标准帧 ID 为 `0x100 + 0x02 = 0x102`：

```sh
app/protocol_tool/build/protocol_tool-host can build 0x102 0x03 12000
```

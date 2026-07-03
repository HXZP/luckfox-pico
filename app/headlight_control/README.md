# 大灯控制工具

`headlight_control` 通过 Linux PWM sysfs 控制大灯。硬件拓扑中大灯接在 `PWM10-M2`，板级 DTS 已启用 `&pwm10`，控制器地址为 `ff490020`。

## 编译

在仓库根目录执行：

```sh
make -C app/headlight_control
```

输出：

```text
app/headlight_control/build/headlight_control
```

本机语法检查可用：

```sh
make -C app/headlight_control host
```

## 使用

```sh
./headlight_control on
./headlight_control off
./headlight_control set 35
./headlight_control blink --repeat 10 --brightness 80
./headlight_control breathe --repeat -1 --brightness 70
./headlight_control sos --repeat 3
./headlight_control strobe --repeat 20
./headlight_control heartbeat --repeat -1 --brightness 60
./headlight_control status
```

亮度参数统一按“人能理解的亮度”解释：`0` 表示关闭/最暗，`100` 表示最亮。当前大灯硬件为低 duty 更亮，程序内部已经做了反向换算。关灯时会保持 PWM 使能并输出关灯 duty，避免 disable 后引脚回到点亮电平。

常用参数：

| 参数 | 说明 | 默认 |
| --- | --- | --- |
| `--chip <chip>` | PWM chip，可填 `pwmchipN`、数字、sysfs 路径或设备名 | 自动匹配 `ff490020` |
| `--channel <n>` | PWM chip 下的通道 | `0` |
| `--freq <hz>` | PWM 频率 | `1000` |
| `--brightness <0-100>` | 灯效亮度百分比，`0` 为关/最暗，`100` 为最亮 | `100` |
| `--repeat <n>` | 灯效循环次数，`-1` 表示一直循环 | `1` |
| `--on-ms <ms>` | 闪烁亮灯时间 | `500` |
| `--off-ms <ms>` | 闪烁灭灯时间 | `500` |
| `--step-ms <ms>` | 呼吸灯每级亮度间隔 | `12` |
| `--hold-ms <ms>` | 呼吸灯最高/最低亮度停留时间 | `120` |
| `--keep` | 灯效结束后保留最后输出 | 默认退出时输出关灯 duty |

## 部署到板端

示例：

```sh
scp app/headlight_control/build/headlight_control root@192.168.2.16:/userdata/
ssh root@192.168.2.16
chmod +x /userdata/headlight_control
/userdata/headlight_control breathe --repeat -1 --brightness 70
```

如果自动匹配失败，先在板端查看：

```sh
ls -l /sys/class/pwm
```

然后手动指定：

```sh
/userdata/headlight_control --chip pwmchip0 --channel 0 blink --repeat 10
```

## 设备树对应

- 板级文件：`sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-zero.dts`
- 节点：`&pwm10`
- 当前配置：`status = "okay"`，`pinctrl-0 = <&pwm10m2_pins>`
- 硬件连接：大灯端子，`PWM10-M2`，RK 侧口号 57
- 亮度极性：硬件为低 duty 更亮，程序默认按 active-low PWM 处理；命令行仍保持 `0=关/最暗，100=最亮`。关灯状态下 `enable=1`、`duty_cycle=period` 是正常状态。

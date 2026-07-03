# RK 硬件连接拓扑

本文档记录 RK 主控侧外设连接规划，用于后续硬件检查、设备树配置、驱动适配和线束核对。

## 总览

| 模块 | 连接方式 | RK 侧接口/引脚 | 位置/连接形态 | 说明 |
| --- | --- | --- | --- | --- |
| 调试口 | UART2 | 42、43 | 4pin SH1.0 端子 | 端子接 5V，用于调试串口 |
| STM32F103C8T6 | UART0 | 0、1 | 贴板子 | STM32 转发 CAN 消息，RK 通过 UART0 与 STM32 通信 |
| BMI088 | I2C2 | 32、33 | 贴板子 | IMU，通过 I2C2 与 RK 通信 |
| ST7789 | SPI0 | CS0-M0、CLK 49、MOSI 50、A0 51 | 屏幕 | 屏幕驱动使用 SPI0 |
| ST7789 背光 | GPIO/PWM 待定 | 56 | 屏幕 | 背光控制，当前选择空余引脚 56 |
| ST7789 复位 | GPIO 待定 | 122 | 屏幕 | 复位控制，当前选择空余引脚 122 |
| TOF 传感器 | I2C3 | 70、71 | 靠近左眼/摄像头位置 | 距离传感器，通过 I2C3 通信 |
| 大灯 | PWM10-M2 | 57 | 端子连接 | PWM 控制大灯亮度 |

## 设备树对应关系

当前板级设备树入口：

- `sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-luckfox-pico-zero.dts`
- `sysdrv/source/kernel/arch/arm/boot/dts/rv1106-luckfox-pico-zero-ipc.dtsi`
- `sysdrv/source/kernel/arch/arm/boot/dts/rv1106.dtsi`
- `sysdrv/source/kernel/arch/arm/boot/dts/rv1106-pinctrl.dtsi`

| 硬件模块 | DTS 节点 | 当前状态 | pinctrl/引脚定义 | 备注 |
| --- | --- | --- | --- | --- |
| 调试口 | `fiq_debugger` / `uart2` | `fiq_debugger` 已启用，`uart2` 基础节点默认 disabled | `rockchip,serial-id = <2>`；`uart2m1_xfer` 默认在 `rv1106.dtsi`；可选 `uart2m0_xfer` 为 `<3 RK_PA3>`/`<3 RK_PA2>` | 当前 bootargs 使用 `console=ttyFIQ0`，早期串口地址是 `0xff4c0000`，对应 UART2 |
| STM32F103C8T6 | `&uart0` | 已启用 | `uart0m0_xfer`，`<0 RK_PA0>` RX、`<0 RK_PA1>` TX | 板级 DTS 已配置 `status = "okay"` |
| BMI088 | `&i2c2` / `bmi088: imu@18` | 已启用 | `i2c2m0_xfer`，`<1 RK_PA0>` SCL、`<1 RK_PA1>` SDA | 已有 `compatible = "bosch,bmi088"`，地址 `0x18` |
| ST7789 | `&spi0` / `fbtft@0` | 已启用 | SPI：`spi0m0_clk` `<1 RK_PC1>`、`spi0m0_mosi` `<1 RK_PC2>`、`spi0m0_cs0` `<1 RK_PC0>`；DC：`<&gpio1 RK_PC3>`；BL：`<&gpio1 RK_PD0>`；RST：`<&gpio3 RK_PD2>` | 当前 `spidev@0` disabled，`fbtft@0` 绑定 `sitronix,st7789v` |
| TOF 传感器 | `&i2c3` | I2C 总线已启用，未添加具体传感器子节点 | `i2c3m0_xfer`，`<2 RK_PA6>` SCL、`<2 RK_PA7>` SDA | 后续需要按实际型号补子节点，例如 `vl53l0x`/`vl53l1x` 等 |
| 大灯 | `&pwm10` | 已启用 | `pwm10m2_pins` | 板级 DTS 注释为“大灯” |

> 说明：上表里的 `RK_PAx/RK_PCx` 是设备树 pinctrl 视角；总览表里的 0、1、32、33、49、50、51、56、57、70、71、122 是板子丝印/排针口编号视角。两者需要在原理图或 Luckfox 引脚复用表中保持一致。

## 拓扑关系

```text
RK 主控
├── UART2(42,43) ── 调试口，4pin SH1.0，接 5V，DTS 走 fiq_debugger/ttyFIQ0
├── UART0(0,1) ──── STM32F103C8T6，DTS: &uart0 + uart0m0_xfer
│                    └── CAN 消息转发
├── I2C2(32,33) ─── BMI088，DTS: &i2c2 + bmi088@18
├── SPI0 ────────── ST7789 屏幕，DTS: &spi0 + fbtft@0
│   ├── CS: SPI0-CS0-M0
│   ├── CLK: 49
│   ├── MOSI: 50
│   ├── A0/DC: 51
│   ├── BL: 56
│   └── RST: 122
├── I2C3(70,71) ─── TOF 传感器，靠近左眼/摄像头位置，DTS: &i2c3 已启用
└── PWM10-M2(57) ── 大灯，端子连接，DTS: &pwm10 + pwm10m2_pins
```

## 接口细节

### 调试串口

- 接口：UART2
- 引脚：42、43
- 连接器：4pin SH1.0 端子
- 电源：接 5V
- 用途：系统调试串口
- 设备树现状：`fiq_debugger` 已启用，`rv1106.dtsi` 中 `rockchip,serial-id = <2>`；`rv1106-luckfox-pico-zero-ipc.dtsi` 的 bootargs 使用 `console=ttyFIQ0`。
- 注意：调试控制台走 `ttyFIQ0`，不一定暴露为普通 `/dev/ttyS2` 使用方式。

### STM32F103C8T6

- 安装方式：贴板子
- RK 通信接口：UART0
- RK 引脚：0、1
- 功能：STM32 负责转发 CAN 消息，RK 通过 UART0 与 STM32 通信
- 设备树现状：`rv1106g-luckfox-pico-zero.dts` 中 `&uart0` 已启用，`pinctrl-0 = <&uart0m0_xfer>`。
- pinctrl：`uart0m0_xfer` 在 `rv1106-pinctrl.dtsi` 中对应 `<0 RK_PA0>` RX、`<0 RK_PA1>` TX。

### BMI088

- 安装方式：贴板子
- 通信接口：I2C2
- RK 引脚：32、33
- 功能：IMU 姿态/运动数据采集
- 设备树现状：`&i2c2` 已启用，`clock-frequency = <400000>`，`pinctrl-0 = <&i2c2m0_xfer>`。
- 子节点：已有 `bmi088: imu@18`，`compatible = "bosch,bmi088"`，`reg = <0x18>`，`status = "okay"`。

### ST7789 屏幕

- 通信接口：SPI0
- CS：SPI0-CS0-M0
- CLK：49
- MOSI：50
- A0/DC：51
- 背光：56
- 复位：122
- 设备树现状：`&spi0` 已启用，`fbtft@0` 使用 `compatible = "sitronix,st7789v"`；`spidev@0` 当前 disabled。
- pinctrl：当前只启用 `spi0m0_clk`、`spi0m0_mosi`、`spi0m0_cs0`，没有启用 MISO。
- GPIO：`led-gpios = <&gpio1 RK_PD0 GPIO_ACTIVE_HIGH>`，`dc-gpios = <&gpio1 RK_PC3 GPIO_ACTIVE_HIGH>`，`reset-gpios = <&gpio3 RK_PD2 GPIO_ACTIVE_LOW>`。
- 显示参数：当前 DTS 配置 `width = <135>`、`height = <240>`、`x-offset = <40>`、`y-offset = <53>`、`rotate = <270>`。
- 注意：`RK_PC3` 在通用 pinctrl 中也是 `spi0_miso_m0` 的可选脚，当前屏幕把它作为 DC/A0 使用，因此 SPI0 不应同时打开 MISO。

### TOF 传感器

- 通信接口：I2C3
- RK 引脚：70、71
- 接口位置：靠近左眼，即摄像头位置附近
- 功能：距离检测
- 设备树现状：`&i2c3` 已启用，`clock-frequency = <400000>`，`pinctrl-0 = <&i2c3m0_xfer>`。
- 待补：当前还没有 TOF 传感器子节点；确定具体型号和 I2C 地址后再补 `compatible`、`reg`、可选中断/复位 GPIO。

### 大灯

- 控制接口：PWM10-M2
- RK 引脚：57
- 连接方式：端子连接
- 功能：通过 PWM 控制大灯亮度
- 设备树现状：`&pwm10` 已启用，`pinctrl-0 = <&pwm10m2_pins>`，板级 DTS 注释为“大灯”。
- 亮度极性：当前硬件表现为低 duty 更亮，应用层应按 active-low PWM 处理；用户参数建议保持 `0=关/最暗，100=最亮`，由程序内部反向换算到 PWM duty。关灯时需要保持 PWM 使能并输出高 duty，避免 disable 后引脚回到点亮电平。

## 后续配置提醒

- UART0、I2C2、I2C3、SPI0、PWM10-M2 当前已在板级 DTS 中启用；UART2 调试口当前主要由 `fiq_debugger` 接管。
- TOF 目前只启用了 I2C3 总线，还需要按传感器型号补具体设备节点。
- ST7789 的 A0/DC、背光和复位已经有 GPIO 绑定；如果硬件实际改脚，需要同步修改 `dc-gpios`、`led-gpios`、`reset-gpios` 和对应 pinctrl。
- 大灯使用 PWM10-M2 时，需要确认引脚 57 没有被其他外设占用，并在应用层使用对应 PWM chip/channel。
- 调试口端子接 5V，接线时注意串口电平与供电定义分开核对。

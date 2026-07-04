# imu_pose

`imu_pose` is a BMI088 pose test app for the RK board. It ports the acemate MCU
pose fusion scheme to Linux user space:

- Mahony-style 6-axis accel/gyro fusion.
- Quaternion integration with roll/pitch/yaw output.
- Accel norm confidence gate.
- Static gyro auto-offset calibration.
- Absolute-time 1 kHz loop with per-window timing statistics.
- Combined `I2C_RDWR` register reads by default, with fallback to the simple
  write/read path.
- Optional CPU affinity, FIFO priority and short busy-wait wake margin for
  user-space timing experiments.

## Build

```sh
make -C app/imu_pose
make -C app/imu_pose host
```

The cross build writes `app/imu_pose/build/imu_pose`.

## Run

Simulated IMU input, useful for measuring scheduler and fusion overhead:

```sh
/userdata/imu_pose --simulate --duration 5 --report-ms 1000 --verbose
```

Direct BMI088 I2C input:

```sh
/userdata/imu_pose --duration 10 --report-ms 1000 --verbose
```

On the current board, the BMI088 is already registered by the kernel as
`bmi088_i2c` on `/sys/bus/i2c/devices/2-0018`, with gyro dummy client
`2-0068`. In that state, normal `/dev/i2c-2` access returns `Device or resource
busy`. For lab timing validation only, force direct access explicitly:

```sh
/userdata/imu_pose --i2c-force --no-auto-offset --duration 8 --report-ms 1000 --verbose
```

`--i2c-force` uses `I2C_SLAVE_FORCE` and may race the kernel driver. Do not use
it as the long-term production path while the kernel BMI088 driver is bound.
When there is no BMI088 data-ready interrupt wired yet, a practical lab command
is:

```sh
/userdata/imu_pose --i2c-force --no-auto-offset --duration 30 --report-ms 1000 --wake-margin-us 80 --cpu 0 --verbose
```

If the combined I2C transaction is not accepted by the board/driver stack, the
program automatically falls back to the simple write/read register path. You can
force the fallback for A/B testing with `--no-i2c-rdwr`.

## Main Options

- `--rate <hz>`: update frequency, default `1000`.
- `--duration <sec>`: run time, `0` means forever.
- `--report-ms <ms>`: timing report interval, default `1000`.
- `--kp <value>` / `--ki <value>`: Mahony gains, default `0.1` / `0.0`.
- `--no-auto-offset`: skip startup static gyro calibration.
- `--no-realtime`: skip `mlockall` and `SCHED_FIFO`.
- `--wake-margin-us <us>`: sleep until just before the next tick, then busy-wait;
  default `50`.
- `--cpu <n>`: pin the process to one CPU for timing tests, default disabled.
- `--fifo-priority <n>`: realtime priority, default `60`.
- `--fixed-dt`: integrate with nominal period instead of measured loop period.
- `--no-i2c-rdwr`: disable combined I2C register reads for comparison.
- `--bus <n>` / `--accel-addr <addr>` / `--gyro-addr <addr>`: I2C selection.

## 1 kHz Stability Notes

Board test on `192.168.2.16`, idle system:

- `--simulate`: measured near 1000 Hz; fusion update was below 1 us on average.
- `--i2c-force --no-auto-offset`: measured near 1000 Hz for 8 s; BMI088 read
  averaged about 496 us and fusion averaged about 3 us. One report window had a
  2.7 ms max period/read spike.

Conclusion: the fusion math has enough margin for 1 kHz. The limiting factors
are Linux user-space scheduling and I2C transfer latency. At 400 kHz I2C, reading
accel and gyro separately consumes about half of each 1 ms period, so this is
soft real-time only. The user-space mitigations above help reduce jitter and
make testing more repeatable, but they are still not a hard real-time substitute
for data-ready driven sampling. For a production-stable 1 kHz path, prefer one
of:

- BMI088 data-ready interrupt plus IIO buffered reads.
- A kernel driver path that pushes samples without sysfs polling.
- Moving the 1 kHz acquisition/fusion loop to the MCU and sending pose to RK.
- SPI or faster I2C, if the hardware design allows it.

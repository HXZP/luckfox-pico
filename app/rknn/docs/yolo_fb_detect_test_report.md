# yolo_fb_detect 测试记录

本文记录 `app/rknn/project/yolo_fb_detect` 在 LuckFox Pico / RV1106 板端的显示与 RKNN 推理测试结果。

## 测试环境

- 板端 IP：`10.8.49.116`
- 板端部署目录：`/userdata/yolo_fb_detect_demo`
- 运行程序：`yolo_fb_detect`
- 模型路径：`./model/yolov5.rknn`
- 显示设备：`/dev/fb0`
- 摄像头设备：`/dev/video11`
- MJPEG 地址：`http://10.8.49.116:8080/stream.mjpg`
- 模型输入：`640x640 NHWC INT8`
- framebuffer：`240x135, RGB565, 16bpp`

## 画面链路

纯显示链路如下：

```text
Sensor
-> MIPI CSI
-> ISP / rk_aiq
-> /dev/video11
-> OpenCV VideoCapture
-> BGR 图像
-> RGB565 转换
-> /dev/fb0
-> 屏幕显示
```

实测结论：关闭 RKNN 推理后，画面流畅，说明摄像头采集、ISP、OpenCV 取帧、framebuffer 显示链路基本正常。

## 纯显示测试

运行方式：

```sh
YOLO_DISABLE_INFERENCE=1 ./run.sh start
```

关键日志：

```text
YOLO inference disabled, camera and framebuffer display only
mjpeg stream url: http://0.0.0.0:8080/stream.mjpg
```

上位机访问 MJPEG：

```text
status 200
content-type multipart/x-mixed-replace; boundary=frame
```

结果：

- 画面流畅。
- 没有新的 `RKNPU` 超时日志。
- 说明卡顿不是 framebuffer 显示本身造成的。

## 离线 30 帧逐帧推理测试

测试目的：获取正常 RKNN 推理耗时，排除实时摄像头持续运行对 NPU 的影响。

测试流程：

```text
采集 30 帧摄像头画面
-> 释放摄像头 / ISP
-> 初始化 RKNN
-> 对 30 帧逐帧推理
-> 记录每帧 resize / inference / total 耗时
```

运行方式：

```sh
YOLO_BENCH_FRAMES=30 ./yolo_fb_detect ./model/yolov5.rknn
```

结果汇总：

```text
bench summary frames=30
resize_avg_ms=14.533
inference_avg_ms=65.600
inference_min_ms=65.000
inference_max_ms=70.000
total_avg_ms=80.133
```

逐帧结果范围：

```text
resize_ms:    13 ~ 24 ms
inference_ms: 65 ~ 70 ms
total_ms:     79 ~ 90 ms
ret:          0
```

结果：

- 离线推理稳定。
- 没有新的 `RKNPU timeout`。
- 正常单次 RKNN 推理约 `65 ms`。
- 包含 resize 后单帧约 `80 ms`。
- 理论处理能力约 `12 FPS`。

## 实时 12 FPS 推理测试

测试目的：将实时推理频率限制到接近离线测试上限，观察是否仍然触发 NPU 超时。

运行方式：

```sh
YOLO_INFERENCE_FPS=12 ./run.sh start
```

关键日志：

```text
YOLO inference target fps=12, interval=83ms
E RKNN: failed to submit!, op id: 21, op name: Conv:/model.6/cv2/conv/Conv
rknn_run fail! ret=-1
```

内核日志：

```text
RKNPU: wait time: 6150513us
RKNPU: job timeout
RKNPU: soft reset
```

结果：

- 12 FPS 限频已生效。
- 实时摄像头运行状态下仍出现约 6 秒 NPU 超时。
- 说明问题不是单纯的推理提交频率过高。

## 实时 1 FPS 推理测试

测试目的：进一步降低推理频率，判断是否只要摄像头持续运行就可能触发 NPU 异常。

运行方式：

```sh
YOLO_INFERENCE_FPS=1 ./run.sh start
```

关键日志：

```text
YOLO inference target fps=1, interval=1000ms
person @ (199 58 220 82) 0.308
chair @ (9 24 33 47) 0.296
save new object image: ./detect_result/000001_person_20260615_073111.jpg
save new object image: ./detect_result/000002_chair_20260615_073111.jpg
bottle @ (136 32 158 85) 0.280
save new object image: ./detect_result/000003_bottle_20260615_073112.jpg
```

随后仍出现：

```text
E RKNN: failed to submit!, op id: 10, op name: Conv:/model.3/conv/Conv
rknn_run fail! ret=-1
E RKNN: failed to submit!, op id: 63, op name: Conv:/model.17/cv3/conv/Conv
rknn_run fail! ret=-1
```

内核日志：

```text
RKNPU: wait time: 6493603us
RKNPU: job timeout
RKNPU: soft reset
```

结果：

- 1 FPS 下不是立即失败。
- 初期可以成功检测并保存图片。
- 运行一段时间后仍会出现 `failed to submit` 和 `RKNPU timeout`。
- 说明降低频率可以缓解压力，但不能根除实时并发异常。

## 关键对比

```text
纯摄像头 + fb 显示：
  流畅，无 RKNPU timeout

离线视频帧推理：
  稳定，单次 RKNN 推理约 65 ms

实时摄像头 + NPU 12 FPS：
  很快出现 failed to submit / RKNPU timeout

实时摄像头 + NPU 1 FPS：
  初期能成功推理，但后续仍出现 failed to submit / RKNPU timeout
```

## 分时处理测试

新增分时工程：

```text
app/rknn/project/yolo_fb_timeshare
```

分时流程：

```text
打开摄像头
-> 采集 1 帧
-> 释放摄像头 / 停止 ISP 出流
-> resize 到 640x640
-> RKNN 推理
-> 绘制检测框
-> 写入 fb0
```

单轮测试结果：

```text
loop=1 capture_ms=265 resize_ms=16 inference_ms=66 total_ms=347 ret=0 detect_count=3
```

5 轮测试结果：

```text
loop=1 capture_ms=265 resize_ms=15 inference_ms=66 total_ms=346 ret=0 detect_count=2
loop=2 capture_ms=130 resize_ms=21 inference_ms=66 total_ms=217 ret=0 detect_count=3
loop=3 capture_ms=129 resize_ms=15 inference_ms=66 total_ms=210 ret=0 detect_count=2
loop=4 capture_ms=129 resize_ms=14 inference_ms=66 total_ms=209 ret=0 detect_count=2
loop=5 capture_ms=126 resize_ms=15 inference_ms=65 total_ms=206 ret=0 detect_count=3
```

10 轮成功测试结果：

```text
loop=1  capture_ms=271 resize_ms=24 inference_ms=66 total_ms=361 ret=0 detect_count=3
loop=2  capture_ms=127 resize_ms=15 inference_ms=66 total_ms=208 ret=0 detect_count=3
loop=3  capture_ms=128 resize_ms=15 inference_ms=66 total_ms=209 ret=0 detect_count=3
loop=4  capture_ms=127 resize_ms=15 inference_ms=66 total_ms=208 ret=0 detect_count=4
loop=5  capture_ms=126 resize_ms=15 inference_ms=67 total_ms=208 ret=0 detect_count=3
loop=6  capture_ms=132 resize_ms=15 inference_ms=66 total_ms=213 ret=0 detect_count=3
loop=7  capture_ms=127 resize_ms=15 inference_ms=65 total_ms=207 ret=0 detect_count=2
loop=8  capture_ms=126 resize_ms=15 inference_ms=66 total_ms=207 ret=0 detect_count=1
loop=9  capture_ms=126 resize_ms=15 inference_ms=66 total_ms=207 ret=0 detect_count=1
loop=10 capture_ms=126 resize_ms=15 inference_ms=67 total_ms=208 ret=0 detect_count=2
```

分时处理频率估算：

```text
首轮总耗时：约 347 ~ 361 ms
  折算频率：约 2.8 FPS

后续稳定单轮总耗时：约 206 ~ 217 ms
  折算频率：约 4.6 ~ 4.9 FPS

典型分时组成：
  摄像头打开/采集/释放：约 126 ~ 132 ms
  resize：约 14 ~ 21 ms
  RKNN 推理：约 65 ~ 67 ms
  总周期：约 207 ~ 213 ms
```

结论：

- 分时处理可以避开“ISP 持续出流 + NPU 同时推理”的并发冲突。
- 分时方案的稳定频率约 `4.6 ~ 4.9 FPS`。
- 首轮因为 rk_aiq / ISP 初始化更慢，频率约 `2.8 FPS`。
- 多次 10 轮测试曾出现板端失联，后续用串口和 `/tmp` 日志重新测试时 10 轮成功，说明该方案比实时并发稳定，但仍需继续长期测试。

## 批量分时测试

批量分时流程：

```text
打开摄像头
-> 连续采集 1 秒内容到内存
-> 释放摄像头 / 停止 ISP 出流
-> 对这一批帧逐帧 RKNN 推理
-> 每帧绘制检测框并写入 fb0
```

测试命令：

```sh
YOLO_TIMESHARE_LOOPS=1 YOLO_CAPTURE_SECONDS=1 /userdata/yolo_fb_timeshare/yolo_fb_timeshare /userdata/yolo_fb_timeshare/model/yolov5.rknn
```

1 轮测试结果：

```text
loop=1 frame=1 resize_ms=30 inference_ms=66 total_ms=96 ret=0 detect_count=2
loop=1 frame=2 resize_ms=14 inference_ms=66 total_ms=80 ret=0 detect_count=1
loop=1 frame=3 resize_ms=14 inference_ms=66 total_ms=80 ret=0 detect_count=4
loop=1 frame=4 resize_ms=14 inference_ms=66 total_ms=80 ret=0 detect_count=3
loop=1 frame=5 resize_ms=14 inference_ms=67 total_ms=81 ret=0 detect_count=3
loop=1 frame=6 resize_ms=14 inference_ms=66 total_ms=80 ret=0 detect_count=3
loop=1 frame=7 resize_ms=14 inference_ms=66 total_ms=80 ret=0 detect_count=2
loop=1 frame=8 resize_ms=14 inference_ms=66 total_ms=80 ret=0 detect_count=3
loop=1 frame=9 resize_ms=14 inference_ms=66 total_ms=80 ret=0 detect_count=3
loop=1 frame=10 resize_ms=14 inference_ms=66 total_ms=80 ret=0 detect_count=4
loop=1 frame=11 resize_ms=13 inference_ms=66 total_ms=79 ret=0 detect_count=2
loop=1 frame=12 resize_ms=14 inference_ms=67 total_ms=81 ret=0 detect_count=1
loop=1 frame=13 resize_ms=19 inference_ms=66 total_ms=85 ret=0 detect_count=2
```

1 轮汇总：

```text
captured_frames=13
capture_ms=1090
infer_frames=13
inference_success=13
resize_avg_ms=15.538
inference_avg_ms=66.154
total_ms=2255
detect_count=33
```

结果：

- 1 秒采集到 13 帧。
- 13 帧全部推理成功，`ret=0`。
- 没有新的 `RKNPU timeout`。
- 完整一轮约 `2.255s`，其中采集约 `1.09s`，推理阶段约 `1.16s`。

10 轮测试：

```text
YOLO_TIMESHARE_LOOPS=10 YOLO_CAPTURE_SECONDS=1
```

现象：

```text
Timeout, server 10.8.49.116 not responding
ping: 100% packet loss
ssh: connect to host 10.8.49.116 port 22: Connection timed out
```

后续在串口日志持续采集、日志写 `/tmp` 的条件下，重新运行 10 轮批量分时测试曾完整结束，串口侧没有捕获到新的异常内核输出。这说明 10 轮批量分时不是每次必现失联，但该组合测试存在明显波动，仍不能认为长期稳定。

日志写 eMMC 的复测：

```sh
YOLO_TIMESHARE_LOOPS=10 YOLO_CAPTURE_SECONDS=1 ./yolo_fb_timeshare ./model/yolov5.rknn > batch_timeshare_10_emmc.log 2>&1
```

结果：

```text
Timeout, server 10.8.49.116 not responding.
ping: 100% packet loss
```

重启后检查：

```text
/userdata/yolo_fb_timeshare/batch_timeshare_10_emmc.log 不存在
/userdata 仍为 rw 挂载
/sys/fs/pstore 为空
dmesg 只看到重启后的 rkisp 初始化日志，没有保存到 panic/oops 记录
```

说明：

- 本次失联发生得比较彻底，日志文件没有成功落盘到 eMMC。
- 没有 pstore 记录，无法从重启后的系统中还原失联瞬间的内核栈。
- 失联仍与“批量摄像头采集 + 多帧 NPU 推理 + eMMC 日志”组合相关，但当前证据不足以区分是内核卡死、总线阻塞、供电瞬态还是存储链路异常。

整秒节拍复测：

程序修改为每轮完成“采样 -> 释放摄像头 -> 批量推理 -> 显示”后，按本轮耗时向上补齐到整数秒，再进入下一轮。例如本轮耗时 `2255ms` 时补齐到 `3000ms`。

新增日志字段：

```text
total_ms=本轮实际耗时
slot_ms=补齐后的整数秒槽位
sleep_ms=本轮结束后实际睡眠时间
```

测试命令：

```sh
YOLO_TIMESHARE_LOOPS=10 YOLO_CAPTURE_SECONDS=1 ./yolo_fb_timeshare ./model/yolov5.rknn > batch_timeshare_10_paced_emmc.log 2>&1
```

结果：

```text
Timeout, server 10.8.49.116 not responding.
ping: 100% packet loss
```

重启后检查：

```text
/userdata/yolo_fb_timeshare/batch_timeshare_10_paced_emmc.log 不存在
/userdata 仍为 rw 挂载
/sys/fs/pstore 为空
dmesg 只看到重启后的 rkisp 初始化日志，没有保存到 panic/oops 记录
```

串口短时抓取只捕获到异常字符，没有可读 kernel panic / oops 日志。该结果说明单纯把轮间节拍补齐到整数秒，仍不能消除 10 轮批量分时失联问题；同时失联时日志文件仍没有成功落盘。

单轮进程间隔 5 秒复测：

测试方式：不让 `yolo_fb_timeshare` 在同一个进程内连续跑 10 轮，而是在 shell 中每次只运行 1 轮，程序完全退出后等待 5 秒，再启动下一次，总共 10 次。

测试命令逻辑：

```sh
for i in 1 2 3 4 5 6 7 8 9 10
do
    YOLO_TIMESHARE_LOOPS=1 YOLO_CAPTURE_SECONDS=1 ./yolo_fb_timeshare ./model/yolov5.rknn
    sleep 5
done
```

测试结果：

```text
cycle=1  exit=0 captured_frames=13 inference_success=13 total_ms=2263 detect_count=27
cycle=2  exit=0 captured_frames=13 inference_success=13 total_ms=2247 detect_count=27
cycle=3  exit=0 captured_frames=13 inference_success=13 total_ms=2242 detect_count=34
cycle=4  exit=0 captured_frames=13 inference_success=13 total_ms=2241 detect_count=31
cycle=5  exit=0 captured_frames=13 inference_success=13 total_ms=2278 detect_count=31
cycle=6  exit=0 captured_frames=13 inference_success=13 total_ms=2221 detect_count=29
cycle=7  exit=0 captured_frames=13 inference_success=13 total_ms=2213 detect_count=24
cycle=8  exit=0 captured_frames=13 inference_success=13 total_ms=2200 detect_count=24
cycle=9  exit=0 captured_frames=13 inference_success=13 total_ms=2190 detect_count=16
cycle=10 exit=0 captured_frames=13 inference_success=13 total_ms=2235 detect_count=15
```

补充检查：

```text
每轮刚退出后 runtime_status 多次显示 active
最终延迟 5 秒后 runtime_status=suspended
无 yolo_fb_timeshare 用户进程残留
只有 rknpu_power_off 内核线程
无新的 RKNPU timeout / failed to submit / soft reset
日志成功落盘：/userdata/yolo_fb_timeshare/one_loop_sleep5_10.log
```

结论：

- “单进程内连续 10 轮批量分时”会复现失联。
- “每轮独立进程，退出后等待 5 秒再启动下一轮”完成 10 次且稳定。
- 这说明问题很可能与同一进程内连续反复使用 camera / rk_aiq / RKNN runtime 后资源没有完全释放，或内核异步 power off / runtime suspend 尚未完成就进入下一轮有关。
- 5 秒间隔给了 NPU 和相关驱动完成 power off / suspend 的时间，因此稳定性明显改善。

40 次扩展复测：

测试方式仍为每次只运行 1 轮，程序退出后等待 5 秒再启动下一次，目标总次数扩展为 40 次。

测试结果：

```text
one loop sleep 5s test start, cycles=40
=== cycle=1 start ===
Timeout, server 10.8.49.116 not responding.
ping: 100% packet loss
```

串口短时抓取没有可读输出。该结果与前一次 10 次成功结果不一致，说明当前板端状态存在波动。由于失联发生在第 1 轮期间，需重启后继续检查 `/userdata/yolo_fb_timeshare/one_loop_sleep5_40.log` 是否落盘，才能确认程序日志卡在初始化、采样还是推理阶段。

重启后检查：

```text
/userdata/yolo_fb_timeshare/one_loop_sleep5_40.log 不存在
/userdata 仍为 rw 挂载
/sys/fs/pstore 为空
dmesg 只看到重启后的 rkisp 初始化日志，没有保存到 panic/oops 记录
```

说明：这次失联发生在第 1 轮早期，连 shell 已经通过 `tee` 打印到终端的日志也没有形成文件。结合前面多次“失联后 eMMC 日志不存在”的现象，当前板端卡死时很可能已经影响到 eMMC 写入或系统调度，导致用户态日志不能作为最后现场依据。

40 次重新复测：

在重启后重新执行相同方案，并在每次写入 `cycle start` 后增加 `sync`，本次 40 次完整成功。

日志文件：

```text
/userdata/yolo_fb_timeshare/one_loop_sleep5_40_rerun.log
```

统计结果：

```text
exit_ok=40
loops=40
frames_min=12
frames_max=13
inference_avg_min=65.462ms
inference_avg_max=66.692ms
total_min=2106ms
total_max=2317ms
final runtime_status=suspended
```

结果：

- 40 次全部 `exit=0`。
- 每轮推理成功帧数为 `12 ~ 13`。
- 没有新的 `RKNPU timeout / failed to submit / soft reset`。
- 最终 NPU runtime 状态为 `suspended`。
- 日志成功落盘，大小约 `224KB`。
- 日志中仍有 `XCORE:E:invalid main scene len!`，该信息在成功测试中也存在，当前看不像导致失联的直接原因。

3 秒间隔复测：

测试方式与 40 次重新复测相同，只把每轮之间的等待时间从 `5s` 改为 `3s`。

测试结果：

```text
one loop sleep 3s test start, cycles=40
=== cycle=1 start ===
=== cycle=1 exit=0 elapsed_s=3 ===
/sys/devices/platform/ff660000.npu/power/runtime_status=active
sleep 3s before next cycle
=== cycle=2 start ===
Timeout, server 10.8.49.116 not responding.
ping: 100% packet loss
```

串口短时抓取没有可读输出。该结果说明 `3s` 间隔不足以稳定支撑多轮独立进程测试；第 1 轮刚退出时 NPU runtime 仍为 `active`，等待 3 秒后进入第 2 轮仍可能触发板端失联。

采样 3 秒单轮复测：

测试方式：恢复使用 `5s` 作为测试间隔判断口径，但单次程序设置为连续采样 `3s` 后再进入推理阶段。

测试命令：

```sh
YOLO_TIMESHARE_LOOPS=1 YOLO_CAPTURE_SECONDS=3 ./yolo_fb_timeshare ./model/yolov5.rknn
```

结果：

```text
one loop capture 3s test start
Timeout, server 10.8.49.116 not responding.
ping: 100% packet loss
```

串口现场：

```text
blk_update_request: I/O error, dev mmcblk0, sector 1197240 op 0x0:(READ)
blk_update_request: I/O error, dev mmcblk0, sector 1197241 op 0x0:(READ)
blk_update_request: I/O error, dev mmcblk0, sector 1197246 op 0x0:(READ)
blk_update_request: I/O error, dev mmcblk0, sector 1197247 op 0x0:(READ)
blk_update_request: I/O error, dev mmcblk0, sector 1197248 op 0x0:(READ)
```

说明：

- 本次失联时串口明确捕获到持续的 `mmcblk0` 读 I/O 错误。
- 这与前面多次 eMMC 日志无法落盘、文件丢失、板端网络失联现象一致。
- 当前已经不能只按 NPU 或 ISP 资源冲突来解释，eMMC / SDIO / 供电 / 板级稳定性问题也在直接参与故障。
- 在存储链路不稳定时，继续增加采样时长会放大内存、日志、模型库读取和系统调度压力，导致问题更容易复现。

判断：

- 批量分时 1 轮稳定。
- 批量分时 10 轮存在波动：有成功完成记录，也有板端失联记录。
- 失联不是单纯由日志写 `/userdata` 直接导致，因为日志写 `/tmp` 时也曾复现；但日志写 eMMC 时更容易丢失最后现场。
- 更可能是多轮摄像头 / ISP 启停与批量推理组合后触发系统级不稳定。

## 纯摄像头 1Hz 启停测试

测试目的：不初始化 RKNN，不执行 NPU 推理，只验证 `rk_aiq/ISP` 反复启停是否独立导致系统异常。

测试命令：

```sh
YOLO_CAMERA_CYCLE_TEST=1 YOLO_CAMERA_CYCLE_HZ=1 YOLO_CAMERA_CYCLE_LOOPS=10 /userdata/yolo_fb_timeshare/yolo_fb_timeshare /userdata/yolo_fb_timeshare/model/yolov5.rknn
```

测试结果：

```text
camera_cycle=1 open_ms=187 capture_ms=40 release_ms=34 total_ms=261 ret=0 frame_empty=0
camera_cycle=2 open_ms=52 capture_ms=40 release_ms=35 total_ms=127 ret=0 frame_empty=0
camera_cycle=3 open_ms=48 capture_ms=40 release_ms=34 total_ms=122 ret=0 frame_empty=0
camera_cycle=4 open_ms=52 capture_ms=39 release_ms=36 total_ms=127 ret=0 frame_empty=0
camera_cycle=5 open_ms=52 capture_ms=39 release_ms=35 total_ms=126 ret=0 frame_empty=0
camera_cycle=6 open_ms=51 capture_ms=40 release_ms=35 total_ms=126 ret=0 frame_empty=0
camera_cycle=7 open_ms=52 capture_ms=39 release_ms=36 total_ms=127 ret=0 frame_empty=0
camera_cycle=8 open_ms=51 capture_ms=39 release_ms=36 total_ms=126 ret=0 frame_empty=0
camera_cycle=9 open_ms=52 capture_ms=39 release_ms=35 total_ms=126 ret=0 frame_empty=0
camera_cycle=10 open_ms=51 capture_ms=40 release_ms=35 total_ms=126 ret=0 frame_empty=0
camera cycle test finish, success=10/10
```

结果：

- 10 次 1Hz 摄像头启停全部成功。
- 没有初始化 RKNN，也没有执行 NPU 推理。
- 没有新的 `RKNPU/mmc/EXT4` 错误。
- 说明“单纯 1Hz 摄像头启停 10 次”暂时不能复现板端失联。
- 更可疑的是摄像头启停后再进行多帧 NPU 推理，或更长轮数下的组合压力。

## 当前判断

目前问题不是 CPU 和 NPU 不能并行，也不是模型本身无法推理。更符合测试结果的判断是：

```text
摄像头 / ISP 持续出流
+
NPU 实时提交 RKNN job
```

这个组合会触发板端资源或驱动层异常。

可能冲突点：

- ISP 与 NPU 同时访问 DDR / AXI 总线。
- 摄像头 DMA buffer、OpenCV/RGA/CPU resize、RKNN tensor buffer 之间的缓存一致性或 DMA 同步问题。
- ISP / RGA / NPU 并发访问内存导致带宽或 QoS 配置不足。
- RKNPU driver / RKNN runtime 在实时摄像头并发场景下存在稳定性问题。
- 板级 DTS 中 NPU、ISP、内存、时钟、电源域、QoS 配置不完善。

## DDR 资源估算

当前板端内存为 `256MB DDR3L`。该容量和带宽对 `3MP ISP + YOLOv5s 640` 实时并发比较紧张。

摄像头按 SC3336 / 3MP 级别估算：

```text
2304 x 1296 = 2.99MP
```

如果 ISP 输出 NV12：

```text
2304 x 1296 x 1.5 = 4.48MB/帧
30 FPS = 134MB/s 写 DDR
```

如果 ISP 输出 UYVY / YUYV：

```text
2304 x 1296 x 2 = 5.97MB/帧
30 FPS = 179MB/s 写 DDR
```

以上只是最终图像帧，ISP 内部统计、3A、缩放、多缓冲、cache miss 等会继续增加 DDR 压力。实际 DDR 压力可能达到上述估算的 2 倍左右。

RKNN / YOLOv5 侧的静态占用：

```text
yolov5.rknn 模型：约 7.6MB
输入 tensor：640 x 640 x 3 = 1.2MB
输出 tensor：约 2.14MB
```

真正的大头是中间 feature map、权重和激活在 NPU 推理过程中的反复读写。离线推理实测：

```text
单次 RKNN 推理：约 65ms
resize + 推理：约 80ms
理论处理能力：约 12 FPS
```

因此实时并发时可能形成如下 DDR/AXI 压力：

```text
ISP 持续写帧：约 100 ~ 300MB/s+
NPU YOLO 推理：数百 MB/s 甚至更高
CPU/OpenCV resize：读原图 + 写 640x640 输入，几十 MB/s
framebuffer / MJPEG / 日志：相对较小，但仍占用带宽
```

判断：

- 更像是 DDR 带宽 / AXI 总线 / QoS / DMA 仲裁压力，而不是单纯 256MB 容量不够。
- 容量也偏紧，因为 Linux、rk_aiq、OpenCV、RKNN runtime、模型、tensor、camera buffer 都在 256MB 内竞争。
- 当前测试现象符合该估算：离线推理稳定，纯显示稳定，实时 ISP + NPU 并发不稳定，分时后明显改善。

## 后续建议

优先继续验证以下方向：

1. 测试更小模型或更低输入尺寸，例如 `320x320`。
2. 测试摄像头低分辨率输入，降低 ISP 和 DDR 压力。
3. 测试只采集帧但不写 framebuffer，再进行实时 NPU 推理。
4. 测试不使用 OpenCV/RGA resize，改用更简单的 CPU resize 或预处理方式。
5. 检查板端 `librknnmrt.so`、RKNPU 驱动、RKNN-Toolkit2 转换版本是否完全匹配。
6. 如条件允许，升级板端 runtime / driver / firmware。

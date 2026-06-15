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

## 后续建议

优先继续验证以下方向：

1. 测试更小模型或更低输入尺寸，例如 `320x320`。
2. 测试摄像头低分辨率输入，降低 ISP 和 DDR 压力。
3. 测试只采集帧但不写 framebuffer，再进行实时 NPU 推理。
4. 测试不使用 OpenCV/RGA resize，改用更简单的 CPU resize 或预处理方式。
5. 检查板端 `librknnmrt.so`、RKNPU 驱动、RKNN-Toolkit2 转换版本是否完全匹配。
6. 如条件允许，升级板端 runtime / driver / firmware。

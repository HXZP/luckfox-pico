# RKNN 模型部署到 LuckFox Pico Zero 指导

本文档记录将一个 AI 模型部署到 LuckFox Pico Zero（RV1106/RV1103 RKNPU）的完整流程。当前工程已经验证过 YOLOv5 目标检测例程，并新增了 `luckfox_pico_yolov5_save` 示例，可在识别到新的物品类别时保存带框和文字标注的图片。

## 1. 整体流程

模型部署不是只把一个文件复制到板子上，完整链路通常是：

1. 在 PC 上准备训练好的模型。
2. 将原始模型导出为 ONNX。
3. 使用 RKNN-Toolkit2 将 ONNX 转换为 `.rknn`。
4. 在 C/C++ 例程中适配模型输入预处理和输出后处理。
5. 使用 LuckFox SDK 交叉编译板端程序。
6. 将可执行程序、`.rknn` 模型、标签文件、动态库一起部署到 RK 板端。
7. 在板端运行并检查摄像头、NPU、识别结果和性能。

## 2. PC 环境准备

当前工程提供了一键安装 RKNN-Toolkit2 环境的脚本：

```bash
bash tools/install_rknn_env.sh
```

安装完成后进入环境：

```bash
source .rknn/miniconda3/bin/activate RKNN-Toolkit2
```

注意事项：

- 当前脚本固定使用 `rknn-toolkit2==2.3.2`。
- 当前脚本固定使用 `onnx==1.16.1`，避免新版 ONNX 缺少 `onnx.mapping` 导致 RKNN 转换失败。
- `.rknn/` 是本地环境目录，已经加入 `.gitignore`，不要提交。

## 3. 准备 ONNX 模型

RKNN-Toolkit2 通常不直接读取 PyTorch `.pt/.pth` 或 TensorFlow 原始模型，而是先读取 ONNX。

以 YOLOv5 为例，官方/示例导出方式类似：

```bash
cd app/yolov5
source /home/hxzp/luckfox-pico/.rknn/miniconda3/bin/activate yolov5
python export.py --rknpu --weight yolov5s.pt
```

导出后会得到：

```bash
yolov5s.onnx
```

如果是自己的模型，需要确认以下内容：

- 输入尺寸，例如 `1x3x640x640` 或 `1x640x640x3`。
- 输入通道顺序，是 RGB 还是 BGR。
- 输入归一化方式，例如 `/255`、`x - mean`、`(x - 127.5) / 128`。
- 输出 tensor 的含义，例如框、类别、置信度、关键点、分割 mask 等。
- 是否存在 RKNPU 不支持的算子。

## 4. 转换为 RKNN

LuckFox 示例工程中的转换脚本在：

```bash
app/luckfox_pico_rknn_example/scripts/luckfox_onnx_to_rknn/convert/convert.py
```

进入转换目录：

```bash
cd app/luckfox_pico_rknn_example/scripts/luckfox_onnx_to_rknn/convert
source /home/hxzp/luckfox-pico/.rknn/miniconda3/bin/activate RKNN-Toolkit2
```

通用命令格式：

```bash
python convert.py <onnx模型路径> <量化数据集txt> <输出rknn路径> <模型类型>
```

YOLOv5 示例：

```bash
python convert.py ../model/yolov5.onnx ../dataset/yolov5_dataset.txt ../model/yolov5.rknn Yolov5
```

RetinaFace 示例：

```bash
python convert.py ../model/retinaface.onnx ../dataset/retinaface_dataset.txt ../model/retinaface.rknn Retinaface
```

量化数据集 `txt` 中写的是少量参考图片路径，例如：

```text
./pic/yolov5/bus.jpg
```

## 5. 自定义模型时必须检查 rknn.config

`rknn.config()` 决定转换时的输入预处理方式。它必须和模型训练/推理源码保持一致。

以 RetinaFace 为例，原模型源码中有：

```python
image -= np.array((104, 117, 123), np.float32)
```

所以转换时要配置：

```python
rknn.config(
    mean_values=[[104, 117, 123]],
    std_values=[[1, 1, 1]],
    target_platform="rv1103",
    quantized_algorithm="normal",
    quant_img_RGB2BGR=True,
)
```

如果换成自己的模型，需要按训练代码修改：

- `mean_values`
- `std_values`
- `quant_img_RGB2BGR`
- `target_platform`
- `quantized_algorithm`
- 是否需要多输入、多输出参数

如果这些配置不匹配，常见现象是：

- 能转换成功，但板端识别结果明显错误。
- 置信度异常低。
- 类别识别混乱。
- ONNX 在 PC 上正常，RKNN 在板端异常。

## 6. C/C++ 例程适配点

板端程序通常分为三部分：

1. 摄像头取图。
2. RKNN 推理。
3. 输出后处理和显示/保存。

当前 YOLOv5 示例目录：

```bash
app/luckfox_pico_rknn_example/example/luckfox_pico_yolov5
```

当前新增保存图片示例目录：

```bash
app/luckfox_pico_rknn_example/example/luckfox_pico_yolov5_save
```

关键文件：

```bash
src/main.cc
src/yolov5.cc
src/postprocess.cc
include/yolov5.h
include/postprocess.h
model/anchors_yolov5.txt
model/coco_80_labels_list.txt
```

适配一个新模型时，通常要改：

- `main.cc`：摄像头输入、resize、保存/显示逻辑。
- `yolov5.cc` 或对应模型文件：RKNN 初始化、输入输出 tensor 设置、推理调用。
- `postprocess.cc`：输出解析、NMS、类别名称、框坐标映射。
- `model/`：放置标签文件、anchor 文件、其他模型配置文件。

如果是检测模型，重点改后处理。如果是分类模型，通常只需要解析分类概率。如果是关键点、分割、人脸特征模型，输出解析逻辑要按模型结构重写。

## 7. 新增一个自己的 example

推荐不要直接改官方示例，而是复制一个新目录：

```bash
cd app/luckfox_pico_rknn_example/example
cp -a luckfox_pico_yolov5 my_model_demo
```

然后修改：

```bash
my_model_demo/src/main.cc
my_model_demo/src/postprocess.cc
my_model_demo/src/yolov5.cc
my_model_demo/include/*.h
my_model_demo/model/*
```

再修改：

```bash
app/luckfox_pico_rknn_example/build.sh
```

把新 demo 加入菜单：

```bash
options=("luckfox_pico_retinaface_facenet"
    "luckfox_pico_retinaface_facenet_spidev"
    "luckfox_pico_yolov5"
    "luckfox_pico_yolov5_save"
    "my_model_demo")
```

## 8. 编译板端程序

进入示例工程：

```bash
cd app/luckfox_pico_rknn_example
```

设置 SDK 路径：

```bash
export LUCKFOX_SDK_PATH=/home/hxzp/luckfox-pico
```

执行构建：

```bash
./build.sh
```

选择：

```text
1) uclibc
```

然后选择对应 demo，例如：

```text
4) luckfox_pico_yolov5_save
```

编译成功后会生成：

```bash
app/luckfox_pico_rknn_example/install/uclibc/<demo_name>_demo
```

例如当前保存图片示例：

```bash
app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_save_demo
```

## 9. 部署到 RK 板端

当前 RK 板端地址：

```bash
10.8.49.116
```

推荐部署目录：

```bash
/duck/<demo_name>_demo
```

以 `luckfox_pico_yolov5_save_demo` 为例：

```bash
cd /home/hxzp/luckfox-pico

sshpass -p 'hxzp' ssh -o StrictHostKeyChecking=no root@10.8.49.116 \
    'rm -rf /duck/luckfox_pico_yolov5_save_demo && mkdir -p /duck/luckfox_pico_yolov5_save_demo'

sshpass -p 'hxzp' scp -o StrictHostKeyChecking=no -r \
    app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_save_demo/* \
    root@10.8.49.116:/duck/luckfox_pico_yolov5_save_demo/
```

板端设置可执行权限：

```bash
sshpass -p 'hxzp' ssh -o StrictHostKeyChecking=no root@10.8.49.116 \
    'chmod +x /duck/luckfox_pico_yolov5_save_demo/luckfox_pico_yolov5_save'
```

## 10. 板端运行

进入板端目录：

```bash
ssh root@10.8.49.116
cd /duck/luckfox_pico_yolov5_save_demo
```

运行前建议设置环境变量：

```bash
export PATH=/oem/usr/bin:/usr/bin:/bin:$PATH
export LD_LIBRARY_PATH=/oem/usr/lib:$PWD/lib:$LD_LIBRARY_PATH
```

当前部署时已经生成了 `run.sh`，所以可以直接执行：

```bash
./run.sh
```

`run.sh` 内容类似：

```sh
#!/bin/sh
export PATH=/oem/usr/bin:/usr/bin:/bin:$PATH
export LD_LIBRARY_PATH=/oem/usr/lib:$PWD/lib:$LD_LIBRARY_PATH
exec ./luckfox_pico_yolov5_save ./model/yolov5.rknn
```

如果没有 `run.sh`，手动执行：

```bash
./luckfox_pico_yolov5_save ./model/yolov5.rknn
```

## 11. 保存和导出识别结果

`luckfox_pico_yolov5_save` 会在板端生成：

```bash
/duck/luckfox_pico_yolov5_save_demo/detect_result
```

文件名示例：

```text
000001_person_20260612_034446.jpg
000002_cat_20260612_034658.jpg
```

导出到上位机：

```bash
cd /home/hxzp/luckfox-pico
mkdir -p app/luckfox_pico_rknn_example/output
rm -rf app/luckfox_pico_rknn_example/output/yolov5_save_detect_result

sshpass -p 'hxzp' scp -o StrictHostKeyChecking=no -r \
    root@10.8.49.116:/duck/luckfox_pico_yolov5_save_demo/detect_result \
    app/luckfox_pico_rknn_example/output/yolov5_save_detect_result
```

## 12. 性能查看

查看程序进程：

```bash
ps | grep luckfox_pico
```

查看 CPU/内存：

```bash
top
```

查看 NPU 负载：

```bash
cat /proc/rknpu/load
cat /proc/rknpu/freq
cat /proc/rknpu/version
```

当前 YOLOv5 保存示例实测情况：

- CPU 稳定后大约 `6.5% ~ 9%`。
- RSS 内存大约 `8.2 MB`。
- NPU 频率 `700 MHz`。
- NPU load 大多接近 `99% ~ 100%`。

注意：当前程序原始 FPS 计算使用 `clock()`，不适合精确统计端到端识别帧率。建议后续改成 `clock_gettime(CLOCK_MONOTONIC, ...)`，分别统计取帧、预处理、推理、后处理和保存耗时。

## 13. 常见问题

### 13.1 can't load library 'librknnmrt.so'

原因：运行时找不到 RKNN runtime 动态库。

处理：

```bash
export LD_LIBRARY_PATH=/oem/usr/lib:$PWD/lib:$LD_LIBRARY_PATH
```

也可以写入 `run.sh`，避免每次手动设置。

### 13.2 RkLunch-stop.sh: not found

原因：`RkLunch-stop.sh` 在 `/oem/usr/bin`，但当前 `PATH` 没包含它。

处理：

```bash
export PATH=/oem/usr/bin:/usr/bin:/bin:$PATH
```

如果用 `run.sh`，不需要每次手动设置。

### 13.3 摄像头打不开或被占用

先停止默认摄像头业务：

```bash
RkLunch-stop.sh
killall rkipc
```

检查视频节点：

```bash
ls -l /dev/video*
```

当前 OpenCV-Mobile 默认打开的是 `/dev/video11`。

### 13.4 E RKNN: failed to submit!

含义：RKNN runtime 向 NPU 提交任务时失败或出现异常状态。

常见原因：

- PC 端 RKNN-Toolkit2 版本和板端 `librknnmrt.so` 版本不完全匹配。
- 模型中存在 RKNPU 支持不完整的算子组合。
- 量化结果或输出 tensor 形状不符合板端 runtime 预期。
- NPU runtime 版本偏旧。

如果程序仍然能识别并保存图片，说明不是完全不可运行。但如果出现卡死、频繁报错或结果异常，需要统一 toolkit/runtime 版本并重新转换模型。

### 13.5 ONNX 转换报错 module 'onnx' has no attribute 'mapping'

原因：ONNX 版本过新，RKNN-Toolkit2 内部还依赖 `onnx.mapping`。

处理：

```bash
pip install onnx==1.16.1
```

当前 `tools/install_rknn_env.sh` 已经固定该版本。

## 14. 需要提交和不需要提交的文件

建议提交：

- C/C++ 示例源码。
- `build.sh` 里的 demo 菜单修改。
- 标签文件、anchor 文件等小型配置文件。
- 部署指导文档。
- 环境安装脚本。

不建议提交：

- `.rknn/` Conda 环境。
- `.rknn`、`.onnx`、`.pt`、`.pth` 模型产物。
- `install/` 部署目录。
- `output/` 检测结果图片。
- `build/` 或 `build_yolov5_save_check/` 编译目录。
- 临时下载的训练仓库，例如 `app/retinaface-pytorch/`、`app/yolov5/`。

这些内容已经在 `.gitignore` 中加入忽略规则。

## 15. 推荐的自定义模型落地顺序

1. 先在 PC 上确认原模型推理正确。
2. 导出 ONNX，并用 Netron 查看输入输出结构。
3. 准备少量量化图片数据集。
4. 修改 `convert.py` 中对应模型类型的 `rknn.config()`。
5. 转换得到 `.rknn`。
6. 复制一个官方 example，改成自己的 demo。
7. 先只跑一张图片或摄像头单帧，确认输入输出正确。
8. 再接入实时摄像头循环。
9. 最后再加显示、保存图片、性能统计等功能。


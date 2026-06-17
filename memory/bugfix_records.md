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

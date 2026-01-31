#!/bin/bash
# luckfox-kernel-config - 快速配置Luckfox内核

# 保存当前目录
ORIG_DIR=$(pwd)

# 进入内核目录
KERNEL_DIR="$ORIG_DIR/sysdrv/source/kernel"
cd "$KERNEL_DIR" || {
    echo "错误：无法进入内核目录 $KERNEL_DIR"
    exit 1
}

# 如果不存在.config，则复制默认配置
if [ ! -f .config ]; then
    echo "复制默认配置..."
    cp ./arch/arm/configs/luckfox_rv1106_linux_defconfig .config
fi

# 运行menuconfig
echo "启动内核配置..."
make ARCH=arm menuconfig

# 询问是否保存为默认配置
read -p "是否将当前配置保存为默认配置? (y/N): " save_default
if [[ $save_default =~ ^[Yy]$ ]]; then
    echo "保存默认配置..."
    make ARCH=arm savedefconfig
    cp defconfig ./arch/arm/configs/luckfox_rv1106_linux_defconfig
fi

# 返回原目录
cd "$ORIG_DIR"
echo "已返回原目录: $(pwd)"

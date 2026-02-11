# toolchain.cmake - 交叉编译工具链配置
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 设置工具链路径
set(TOOLCHAIN_PATH "/home/hxzp/luckfox-zero/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf")
set(TOOLCHAIN_PREFIX "arm-rockchip830-linux-uclibcgnueabihf")
set(CMAKE_C_COMPILER "${TOOLCHAIN_PATH}/bin/${TOOLCHAIN_PREFIX}-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/bin/${TOOLCHAIN_PREFIX}-g++")

# 设置其他工具
set(CMAKE_AR "${TOOLCHAIN_PATH}/bin/${TOOLCHAIN_PREFIX}-ar" CACHE FILEPATH "ar")
set(CMAKE_STRIP "${TOOLCHAIN_PATH}/bin/${TOOLCHAIN_PREFIX}-strip" CACHE FILEPATH "strip")
set(CMAKE_RANLIB "${TOOLCHAIN_PATH}/bin/${TOOLCHAIN_PREFIX}-ranlib" CACHE FILEPATH "ranlib")

# 设置编译标志
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")

# 设置查找路径
set(CMAKE_FIND_ROOT_PATH 
    "${TOOLCHAIN_PATH}/${TOOLCHAIN_PREFIX}"
    "${CMAKE_SOURCE_DIR}/arm-linux-gnueabihf"
)

# 设置查找模式
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 设置系统信息
set(CMAKE_SYSTEM_INCLUDE_PATH "${TOOLCHAIN_PATH}/${TOOLCHAIN_PREFIX}/include")
set(CMAKE_SYSTEM_LIBRARY_PATH "${TOOLCHAIN_PATH}/${TOOLCHAIN_PREFIX}/lib")

# 设置pkg-config
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_FIND_ROOT_PATH}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_FIND_ROOT_PATH}")

# 设置编译器特性检测
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
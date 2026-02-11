# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(video2fb VERSION 1.0.0 LANGUAGES C CXX)

# 设置CMake策略
cmake_policy(SET CMP0077 NEW)  # 使用选项优先级

# 项目描述
message(STATUS "========================================")
message(STATUS "视频到Framebuffer显示项目")
message(STATUS "目标平台: LuckFox Pico (RK1103)")
message(STATUS "编译器: ${CMAKE_C_COMPILER}")
message(STATUS "========================================")

# 设置C/C++标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置输出目录
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 设置编译选项
set(COMMON_FLAGS "-O2 -Wall -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS} -std=c++11")

# 链接器选项
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

# 设置OpenCV路径
set(OPENCV_PATH "${CMAKE_SOURCE_DIR}/opencv-mobile-4.10.0-luckfox-pico")
set(OPENCV_INCLUDE_DIR "${OPENCV_PATH}/include/opencv4")
set(OPENCV_LIB_DIR "${OPENCV_PATH}/lib")
set(OpenCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/opencv-mobile-4.10.0-luckfox-pico/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)

# 设置OpenCV变量
set(OpenCV_INCLUDE_DIRS ${OPENCV_INCLUDE_DIR})

# 添加OpenCV库的依赖库
set(OPENCV_EXTRA_LIBS 
    pthread 
    m 
    dl
)

# 添加子目录
add_subdirectory(src)

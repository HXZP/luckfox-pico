cmake_minimum_required(VERSION 3.5)
project(opencv-mobile-test)
set(CMAKE_CXX_STANDARD 11)

SET(CMAKE_C_COMPILER "/home/hxzp/luckfox-zero/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc")
SET(CMAKE_CXX_COMPILER "/home/hxzp/luckfox-zero/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-g++")

set(OpenCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/cmake/opencv4")
# set(OpenCV_DIR "/home/xt/Luckfox/opencv-mobile-4.10.0-luckfox-pico/lib/cmake/opencv4")

find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})
message(STATUS "OpenCV_LIBS=${OpenCV_LIBS}")

add_executable(opencv-mobile-test main.cpp)

target_link_libraries(opencv-mobile-test ${OpenCV_LIBS})

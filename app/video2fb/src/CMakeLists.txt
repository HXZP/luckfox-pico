# src/CMakeLists.txt
# 设置源文件
set(FBTEST_SOURCES fbtest.c)
set(V2FB_SOURCES v2fb.c)
set(YUV2FB_SOURCES yuv2fb.c)
set(SIMPLE_CAMERA_FB_SOURCES simple_camera_fb.cpp)

# 添加可执行文件

# 1. fbtest - 纯C版本
add_executable(fbtest ${FBTEST_SOURCES})
target_link_libraries(fbtest m)

# 2. v2fb - 摄像头到framebuffer
add_executable(v2fb ${V2FB_SOURCES})
target_link_libraries(v2fb m)

# 3. yuv2fb - YUV文件播放
add_executable(yuv2fb ${YUV2FB_SOURCES})
target_link_libraries(yuv2fb m)


add_executable(simple_camera_fb ${SIMPLE_CAMERA_FB_SOURCES})
target_include_directories(simple_camera_fb PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(simple_camera_fb ${OpenCV_LIBS} ${OPENCV_EXTRA_LIBS})

# 设置OpenCV宏定义
target_compile_definitions(simple_camera_fb PRIVATE 
    HAVE_OPENCV=1
    OPENCV_VERSION=4
)


# 为所有目标设置属性
set_target_properties(simple_camera_fb fbtest v2fb yuv2fb
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    OUTPUT_NAME_DEBUG "${CMAKE_PROJECT_NAME}_debug"
)
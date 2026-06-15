# Install script for directory: /home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/home/hxzp/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/luckfox_pico_yolov5_image" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/luckfox_pico_yolov5_image")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/luckfox_pico_yolov5_image"
         RPATH "$ORIGIN/lib")
  endif()
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/luckfox_pico_yolov5_image")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo" TYPE EXECUTABLE FILES "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/luckfox_pico_yolov5_image")
  if(EXISTS "$ENV{DESTDIR}/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/luckfox_pico_yolov5_image" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/luckfox_pico_yolov5_image")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/luckfox_pico_yolov5_image"
         OLD_RPATH "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/lib/uclibc:"
         NEW_RPATH "$ORIGIN/lib")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/home/hxzp/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-strip" "$ENV{DESTDIR}/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/luckfox_pico_yolov5_image")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/CMakeFiles/luckfox_pico_yolov5_image.dir/install-cxx-module-bmi-noconfig.cmake" OPTIONAL)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/model/anchors_yolov5.txt;/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/model/coco_80_labels_list.txt;/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/model/yolov5.rknn")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_yolov5_image_demo/model" TYPE FILE FILES
    "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/example/luckfox_pico_yolov5_image/model/anchors_yolov5.txt"
    "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/example/luckfox_pico_yolov5_image/model/coco_80_labels_list.txt"
    "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/example/luckfox_pico_yolov5_image/model/yolov5.rknn"
    )
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/3rdparty.out/cmake_install.cmake")
  include("/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/utils.out/cmake_install.cmake")
  include("/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/lib/Config/cmake_install.cmake")
  include("/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/lib/GUI/cmake_install.cmake")
  include("/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/lib/LCD/cmake_install.cmake")
  include("/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/lib/SPI/cmake_install.cmake")
  include("/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/lib/GPIO/cmake_install.cmake")

endif()

if(CMAKE_INSTALL_COMPONENT)
  set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
file(WRITE "/home/hxzp/luckfox-pico/app/luckfox_pico_rknn_example/build_yolov5_image_check/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")

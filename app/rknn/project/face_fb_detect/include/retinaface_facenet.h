// Copyright (c) 2023 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef _RKNN_DEMO_RETINAFACE_RV1106_H_
#define _RKNN_DEMO_RETINAFACE_RV1106_H_

#include "rknn_api.h"

#include <stdint.h>
#include <vector>
#include "rknn_api.h"

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"


// 保存 RKNN DMA 缓冲区的虚拟地址、文件描述符和大小。
typedef struct {
    char *dma_buf_virt_addr;
    int dma_buf_fd;
    int size;
}rknn_dma_buf;


// 保存检测框的左上角和右下角坐标。
typedef struct {
    int left;
    int top;
    int right;
    int bottom;
}image_rect_t; 

// 保存二维关键点坐标。
typedef struct {
    int x;
    int y;
}point_t;

// 保存单个人脸检测结果，包括检测框、置信度和五个关键点。
typedef struct {
    image_rect_t box;
    float prop;
    point_t point[5];
} object_detect_result;

// 保存一帧图像中的全部人脸检测结果。
typedef struct {
    //int id;
    int count;
    object_detect_result results[128];
} object_detect_result_list;

// 保存 RKNN 模型上下文、输入输出属性和零拷贝内存。
typedef struct {
    rknn_context rknn_ctx;
    rknn_tensor_mem* max_mem;
    rknn_tensor_mem* net_mem;
    rknn_input_output_num io_num;
    rknn_tensor_attr* input_attrs;
    rknn_tensor_attr* output_attrs;
#if defined(RV1106_1103) 
    rknn_tensor_mem* input_mems[1];
    rknn_tensor_mem* output_mems[9];
    rknn_dma_buf img_dma_buf;
#endif
    int model_channel;
    int model_width;
    int model_height;
    
    bool is_quant;
} rknn_app_context_t;


int init_retinaface_facenet_model(const char *model_path, const char *model_path2,rknn_app_context_t *app_retinaface_ctx,rknn_app_context_t *app_facenet_ctx);

//retinaface
int init_retinaface_model(const char* model_path, rknn_app_context_t* app_ctx);
int release_retinaface_model(rknn_app_context_t* app_ctx);
int inference_retinaface_model(rknn_app_context_t* app_ctx,object_detect_result_list* od_results);
void mapCoordinates(cv::Mat input, cv::Mat output, int *x, int *y);	

//facenet
int init_facenet_model(const char* model_path, rknn_app_context_t* app_ctx);
int release_facenet_model(rknn_app_context_t* app_ctx);
void letterbox(cv::Mat input, cv::Mat output);

float get_duclidean_distance(float *output1,float *output2);
void output_normalization(rknn_app_context_t* app_ctx,uint8_t *output, float *out_fp32);

#endif 
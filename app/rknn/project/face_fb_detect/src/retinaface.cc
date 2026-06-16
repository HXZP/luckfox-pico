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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "retinaface_facenet.h"

#include "rknn_box_priors.h"

#define USE_RKNN_MEM_SHARE 0


// 获取单调时钟毫秒值，用于统计 RKNN 推理耗时。
static long long get_retinaface_monotonic_ms()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


int clamp(float x, int min, int max) {
    if (x > max) return max;
    if (x < min) return min;
    return x;
}

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
           attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

int init_retinaface_model(const char *model_path, rknn_app_context_t *app_ctx)
{
    int ret;
    int model_len = 0;
    char *model;
    rknn_context ctx = 0;

    ret = rknn_init(&ctx, (char *)model_path, 0, 0, NULL);
    if (ret < 0)
    {
        printf("rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    // Get Model Input Output Number
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC)
    {
        printf("rknn_query fail! ret=%d\n", ret);
        return -1;
    }
    printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    // Get Model Input Info
    printf("input tensors:\n");
    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }
    
    // Get Model Output Info
    printf("output tensors:\n");
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        //When using the zero-copy API interface, query the native output tensor attribute
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(output_attrs[i]));
    }

    // default input type is int8 (normalize and quantize need compute in outside)
    // if set uint8, will fuse normalize and quantize to npu
    input_attrs[0].type = RKNN_TENSOR_UINT8;
    // default fmt is NHWC,1106 npu only support NHWC in zero copy mode
    input_attrs[0].fmt = RKNN_TENSOR_NHWC;
    printf("input_attrs[0].size_with_stride=%d\n", input_attrs[0].size_with_stride);
    app_ctx->input_mems[0] = rknn_create_mem(ctx, input_attrs[0].size_with_stride);

    // Set input tensor memory
    ret = rknn_set_io_mem(ctx, app_ctx->input_mems[0], &input_attrs[0]);
    if (ret < 0) {
        printf("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
        return -1;
    }

    // Set output tensor memory
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        app_ctx->output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size_with_stride);
        printf("output mem [%d] = %d \n",i ,output_attrs[i].size_with_stride);
        ret = rknn_set_io_mem(ctx, app_ctx->output_mems[i], &output_attrs[i]);
        if (ret < 0) {
            printf("output_mems rknn_set_io_mem fail! ret=%d\n", ret);
            return -1;
        }
    }

    // Set to context
    app_ctx->rknn_ctx = ctx;

    // TODO
    if (output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC)
    {
        app_ctx->is_quant = true;
    }
    else
    {
        app_ctx->is_quant = false;
    }

    app_ctx->io_num = io_num;
    app_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) 
    {
        printf("model is NCHW input fmt\n");
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height  = input_attrs[0].dims[2];
        app_ctx->model_width   = input_attrs[0].dims[3];
    } else 
    {
        printf("model is NHWC input fmt\n");
        app_ctx->model_height  = input_attrs[0].dims[1];
        app_ctx->model_width   = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    } 

    printf("model input height=%d, width=%d, channel=%d\n",
           app_ctx->model_height, app_ctx->model_width, app_ctx->model_channel);

    return 0;
}



int init_retinaface_facenet_model(const char *model_path, const char *model_path2,rknn_app_context_t *app_retinaface_ctx,rknn_app_context_t *app_facenet_ctx)
{
    int ret;
    int model_len = 0;
    char *model;
    rknn_context ctx_retinaface = 0;
    rknn_context ctx_facenet = 0;

#if USE_RKNN_MEM_SHARE
    //retinaface
    printf("init retinaface\n");
    rknn_init(&ctx_retinaface, (char *)model_path, 0, RKNN_FLAG_MEM_ALLOC_OUTSIDE, NULL);
    rknn_mem_size mem_size_retinaface;
    rknn_query(ctx_retinaface, RKNN_QUERY_MEM_SIZE, &mem_size_retinaface, sizeof(mem_size_retinaface)); //查询分配给网络的内存大小

    //facenet
    printf("init facenet\n");
    rknn_init(&ctx_facenet, (char *)model_path2, 0, RKNN_FLAG_MEM_ALLOC_OUTSIDE, NULL);
    rknn_mem_size mem_size_facenet;
    rknn_query(ctx_facenet, RKNN_QUERY_MEM_SIZE, &mem_size_facenet, sizeof(mem_size_facenet)); //查询分配给网络的内存大小

    uint32_t max_interal_size = (mem_size_retinaface.total_internal_size > mem_size_facenet.total_internal_size)?
                                mem_size_retinaface.total_internal_size : mem_size_facenet.total_internal_size;
                                 
    //uint32_t max_interal_size = mem_size_retinaface.total_internal_size + mem_size_facenet.total_internal_size;
    

    printf("max internal mem size = %d\n",max_interal_size);

    //alloc memory
    printf("Alloc memory\n");
    app_retinaface_ctx->max_mem = rknn_create_mem(ctx_retinaface,max_interal_size);
    app_retinaface_ctx->net_mem = rknn_create_mem_from_fd(ctx_retinaface,app_retinaface_ctx->max_mem->fd,       //内存文件描述符
                                                            app_retinaface_ctx->max_mem->virt_addr,             //内存的虚拟地址
                                                            mem_size_retinaface.total_internal_size,            //内存大小
                                                            0);
    rknn_set_internal_mem(ctx_retinaface,app_retinaface_ctx->net_mem);
    printf("Alloc retinaface memory success\n");

    app_facenet_ctx->net_mem    = rknn_create_mem_from_fd(ctx_facenet,app_retinaface_ctx->max_mem->fd,    
                                                            app_retinaface_ctx->max_mem->virt_addr + mem_size_retinaface.total_internal_size,
                                                            mem_size_facenet.total_internal_size,
                                                            0);
    rknn_set_internal_mem(ctx_facenet,app_facenet_ctx->net_mem); 
    printf("Alloc facenet memory success\n");
#else

    //retinaface
    printf("init retinaface\n");
    rknn_init(&ctx_retinaface, (char *)model_path, 0, 0, NULL);

    //facenet
    printf("init facenet\n");
    rknn_init(&ctx_facenet, (char *)model_path2, 0, 0, NULL);

#endif


    //-----------------------------------
    // Retinaface in/output Setting
    printf("Retinaface Info\n");
    // Get Model Input Output Number
    rknn_input_output_num io_num;
    ret = rknn_query(ctx_retinaface, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC)
    {
        printf("rknn_query fail! ret=%d\n", ret);
        return -1;
    }
    printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    // Get Model Input Info
    printf("input tensors:\n");
    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(ctx_retinaface,
                        RKNN_QUERY_NATIVE_INPUT_ATTR,
                        &(input_attrs[i]),
                        sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }

    // Get Model Output Info
    printf("output tensors:\n");
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        //When using the zero-copy API interface, query the native output tensor attribute
        ret = rknn_query(ctx_retinaface,
                        RKNN_QUERY_NATIVE_OUTPUT_ATTR,
                        &(output_attrs[i]),
                        sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(output_attrs[i]));
    }

    // default input type is int8 (normalize and quantize need compute in outside)
    // if set uint8, will fuse normalize and quantize to npu
    input_attrs[0].type = RKNN_TENSOR_UINT8;
    // default fmt is NHWC,1106 npu only support NHWC in zero copy mode
    input_attrs[0].fmt = RKNN_TENSOR_NHWC;
    printf("input_attrs[0].size_with_stride=%d\n", input_attrs[0].size_with_stride);
    app_retinaface_ctx->input_mems[0] = rknn_create_mem(ctx_retinaface, input_attrs[0].size_with_stride);
    // Set input tensor memory
    ret = rknn_set_io_mem(ctx_retinaface, app_retinaface_ctx->input_mems[0], &input_attrs[0]);
    if (ret < 0) {
        printf("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
        return -1;
    }

    // Set output tensor memory
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        app_retinaface_ctx->output_mems[i] = rknn_create_mem(ctx_retinaface, output_attrs[i].size_with_stride);
        printf("output mem [%d] = %d \n",i ,output_attrs[i].size_with_stride);
        ret = rknn_set_io_mem(ctx_retinaface, app_retinaface_ctx->output_mems[i], &output_attrs[i]);
        if (ret < 0) {
            printf("output_mems rknn_set_io_mem fail! ret=%d\n", ret);
            return -1;
        }
    }

    // Set to context
    app_retinaface_ctx->rknn_ctx = ctx_retinaface;

    // TODO
    if (output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC)
    {
        app_retinaface_ctx->is_quant = true;
    }
    else
    {
        app_retinaface_ctx->is_quant = false;
    }

    app_retinaface_ctx->io_num = io_num;
    
    app_retinaface_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_retinaface_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    
    app_retinaface_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_retinaface_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    printf("model is NHWC input fmt\n");
    app_retinaface_ctx->model_height  = input_attrs[0].dims[1];
    app_retinaface_ctx->model_width   = input_attrs[0].dims[2];
    app_retinaface_ctx->model_channel = input_attrs[0].dims[3];

    //-----------------------------------------
    // Facenet in/output Setting 
    printf("Facenet Info\n");
    // Get Model Input Output Number
    rknn_input_output_num io_num_facenet;
    rknn_query(ctx_facenet, RKNN_QUERY_IN_OUT_NUM, &io_num_facenet, sizeof(io_num_facenet));
    printf("model input num: %d, output num: %d\n", io_num_facenet.n_input, io_num_facenet.n_output);

    // Get Model Input Info
    printf("input tensors:\n");
    rknn_tensor_attr input_attrs_facenet[io_num_facenet.n_input];
    memset(input_attrs_facenet, 0, sizeof(input_attrs_facenet));
    for (int i = 0; i < io_num_facenet.n_input; i++)
    {
        input_attrs_facenet[i].index = i;
        ret = rknn_query(ctx_facenet, RKNN_QUERY_NATIVE_INPUT_ATTR, &(input_attrs_facenet[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_attrs_facenet[i]));
    }

    // Get Model Output Info
    printf("output tensors:\n");
    rknn_tensor_attr output_attrs_facenet[io_num_facenet.n_output];
    memset(output_attrs_facenet, 0, sizeof(output_attrs_facenet));
    for (int i = 0; i < io_num_facenet.n_output; i++)
    {
        output_attrs_facenet[i].index = i;
        //When using the zero-copy API interface, query the native output tensor attribute
        ret = rknn_query(ctx_facenet, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &(output_attrs_facenet[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(output_attrs_facenet[i]));
    }

    // default input type is int8 (normalize and quantize need compute in outside)
    // if set uint8, will fuse normalize and quantize to npu
    input_attrs[0].type = RKNN_TENSOR_UINT8;
    // default fmt is NHWC,1106 npu only support NHWC in zero copy mode
    input_attrs[0].fmt = RKNN_TENSOR_NHWC;
    printf("input_attrs[0].size_with_stride=%d\n", input_attrs_facenet[0].size_with_stride);
    app_facenet_ctx->input_mems[0] = rknn_create_mem(ctx_facenet, input_attrs_facenet[0].size_with_stride);

    // Set input tensor memory
    ret = rknn_set_io_mem(ctx_facenet, app_facenet_ctx->input_mems[0], &input_attrs_facenet[0]);
    if (ret < 0) {
        printf("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
        return -1;
    }

    // Set output tensor memory
    for (uint32_t i = 0; i < io_num_facenet.n_output; ++i) {
        app_facenet_ctx->output_mems[i] = rknn_create_mem(ctx_facenet, output_attrs_facenet[i].size_with_stride);
        printf("output mem [%d] = %d \n",i ,output_attrs_facenet[i].size_with_stride);
        ret = rknn_set_io_mem(ctx_facenet, app_facenet_ctx->output_mems[i], &output_attrs_facenet[i]);
        if (ret < 0) {
            printf("output_mems rknn_set_io_mem fail! ret=%d\n", ret);
            return -1;
        }
    }
    // Set to context
    app_facenet_ctx->rknn_ctx = ctx_facenet;

    // TODO
    if (output_attrs_facenet[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC)
    {
        app_facenet_ctx->is_quant = true;
    }
    else
    {
        app_facenet_ctx->is_quant = false;
    }

    app_facenet_ctx->io_num = io_num_facenet;
    app_facenet_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num_facenet.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_facenet_ctx->input_attrs, input_attrs_facenet, io_num_facenet.n_input * sizeof(rknn_tensor_attr));
    app_facenet_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num_facenet.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_facenet_ctx->output_attrs, output_attrs_facenet, io_num_facenet.n_output * sizeof(rknn_tensor_attr));


    printf("Init success \n");

    return 0;
}


int release_retinaface_model(rknn_app_context_t *app_ctx)
{
    
    if (app_ctx->rknn_ctx != 0)
    {
        rknn_destroy(app_ctx->rknn_ctx);
        app_ctx->rknn_ctx = 0;
    }
    
    if (app_ctx->net_mem != NULL)
    {
        printf("destory mem\n");
        rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->net_mem);
        free(app_ctx->net_mem);
    }
    
    if (app_ctx->max_mem != NULL)
    {
        printf("destory mem\n");
        rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->max_mem);
        free(app_ctx->max_mem);
    }
    
    
    if (app_ctx->input_attrs != NULL)
    {
        free(app_ctx->input_attrs);
        app_ctx->input_attrs = NULL;
    }
    if (app_ctx->output_attrs != NULL)
    {
        free(app_ctx->output_attrs);
        app_ctx->output_attrs = NULL;
    }
    for (int i = 0; i < app_ctx->io_num.n_input; i++) {
        if (app_ctx->input_mems[i] != NULL) {
            rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->input_mems[i]);
            free(app_ctx->input_mems[i]);
        }
    }
    for (int i = 0; i < app_ctx->io_num.n_output; i++) {
        if (app_ctx->output_mems[i] != NULL) {
            rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->output_mems[i]);
            free(app_ctx->output_mems[i]);
        }
    }
    return 0;
}



static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1, float ymax1) {
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1) * (ymax0 - ymin0 + 1) + (xmax1 - xmin1 + 1) * (ymax1 - ymin1 + 1) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static int quick_sort_indice_inverse(float *input, int left, int right, int *indices) {
    float key;
    int key_index;
    int low = left;
    int high = right;
    if (left < right) {
        key_index = indices[left];
        key = input[left];
        while (low < high) {
            while (low < high && input[high] <= key) {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key) {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

static int nms(int validCount, float *outputLocations, int order[], float threshold, int width, int height) {
    for (int i = 0; i < validCount; ++i) {
        if (order[i] == -1) {
            continue;
        }
        int n = order[i];
        for (int j = i + 1; j < validCount; ++j) {
            int m = order[j];
            if (m == -1) {
                continue;
            }
            float xmin0 = outputLocations[n * 4 + 0] * width;
            float ymin0 = outputLocations[n * 4 + 1] * height;
            float xmax0 = outputLocations[n * 4 + 2] * width;
            float ymax0 = outputLocations[n * 4 + 3] * height;

            float xmin1 = outputLocations[m * 4 + 0] * width;
            float ymin1 = outputLocations[m * 4 + 1] * height;
            float xmax1 = outputLocations[m * 4 + 2] * width;
            float ymax1 = outputLocations[m * 4 + 3] * height;

            float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

            if (iou > threshold) {
                order[j] = -1;
            }
        }
    }
    return 0;
}

inline static int32_t __clip(float val, float min, float max)
{
    float f = val <= min ? min : (val >= max ? max : val);
    return f;
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
    float dst_val = (f32 / scale) + zp;
    int8_t res = (int8_t)__clip(dst_val, -128, 127);
    return res;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

// 按 NC1HWC2 布局读取量化输出，并反量化为 float。
static float read_nc1hwc2_output(const int8_t *data,
                                 int channel,
                                 int y,
                                 int x,
                                 int height,
                                 int width,
                                 int32_t zp,
                                 float scale)
{
    const int channel_pack = 16;
    int channel_group;
    int channel_offset;
    int index;

    channel_group = channel / channel_pack;
    channel_offset = channel % channel_pack;
    index = ((channel_group * height * width + y * width + x) * channel_pack) + channel_offset;
    return deqnt_affine_to_f32(data[index], zp, scale);
}

int inference_retinaface_model(rknn_app_context_t *app_ctx, object_detect_result_list *od_results)
{
    int ret;
    long long run_start_ms;
    long long run_end_ms;

    run_start_ms = get_retinaface_monotonic_ms();
    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    run_end_ms = get_retinaface_monotonic_ms();
    printf("retinaface_rknn_run_ms=%lld ret=%d\n", run_end_ms - run_start_ms, ret);
    if (ret < 0)
    {
        printf("rknn_run fail! ret=%d\n", ret);
        return -1;
    }
    const float (*prior_ptr)[4];
    int num_priors = 16800;
    prior_ptr = BOX_PRIORS_640;
    
    int *filter_indices = (int *)malloc(sizeof(int) * num_priors);
    float *props = (float *)malloc(sizeof(float) * num_priors);
    float *loc_fp32 = (float *)malloc(sizeof(float) * num_priors * 4);
    float *landms_fp32 = (float *)malloc(sizeof(float) * num_priors * 10);
    if (filter_indices == NULL || props == NULL || loc_fp32 == NULL || landms_fp32 == NULL)
    {
        printf("malloc retinaface postprocess buffer failed\n");
        free(filter_indices);
        free(props);
        free(loc_fp32);
        free(landms_fp32);
        return -1;
    }

    memset(loc_fp32, 0, sizeof(float) * num_priors * 4);
    memset(landms_fp32, 0, sizeof(float) * num_priors * 10);
    memset(filter_indices, 0, sizeof(int) * num_priors);
    memset(props, 0, sizeof(float) * num_priors);

    int validCount = 0;
    const float VARIANCES[2] = {0.1, 0.2};

    const int feature_widths[3] = {80, 40, 20};
    const int feature_heights[3] = {80, 40, 20};
    int prior_index = 0;

    // NCHW 输出顺序：bbox 三层、score 三层、landmark 三层。
    for (int level = 0; level < 3; level++)
    {
        int width = feature_widths[level];
        int height = feature_heights[level];
        int bbox_output_index = level;
        int score_output_index = level + 3;
        int landmark_output_index = level + 6;
        int loc_zp = app_ctx->output_attrs[bbox_output_index].zp;
        float loc_scale = app_ctx->output_attrs[bbox_output_index].scale;
        int scores_zp = app_ctx->output_attrs[score_output_index].zp;
        float scores_scale = app_ctx->output_attrs[score_output_index].scale;
        int landms_zp = app_ctx->output_attrs[landmark_output_index].zp;
        float landms_scale = app_ctx->output_attrs[landmark_output_index].scale;
        const int8_t *location = (const int8_t *)(app_ctx->output_mems[bbox_output_index]->virt_addr);
        const int8_t *scores = (const int8_t *)(app_ctx->output_mems[score_output_index]->virt_addr);
        const int8_t *landms = (const int8_t *)(app_ctx->output_mems[landmark_output_index]->virt_addr);

        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                for (int anchor_id = 0; anchor_id < 2; anchor_id++)
                {
                    float face_score;
                    int offset;

                    face_score = read_nc1hwc2_output(scores,
                                                     anchor_id * 2 + 1,
                                                     y,
                                                     x,
                                                     height,
                                                     width,
                                                     scores_zp,
                                                     scores_scale);
                    if (face_score > 0.5)
                    {
                        float box_x;
                        float box_y;
                        float box_w;
                        float box_h;
                        float xmin;
                        float ymin;
                        float xmax;
                        float ymax;

                        filter_indices[validCount] = prior_index;
                        props[validCount] = face_score;
                        offset = prior_index * 4;

                        box_x = read_nc1hwc2_output(location, anchor_id * 4 + 0, y, x, height, width, loc_zp, loc_scale)
                                * VARIANCES[0] * prior_ptr[prior_index][2] + prior_ptr[prior_index][0];
                        box_y = read_nc1hwc2_output(location, anchor_id * 4 + 1, y, x, height, width, loc_zp, loc_scale)
                                * VARIANCES[0] * prior_ptr[prior_index][3] + prior_ptr[prior_index][1];
                        box_w = expf(read_nc1hwc2_output(location, anchor_id * 4 + 2, y, x, height, width, loc_zp, loc_scale)
                                     * VARIANCES[1]) * prior_ptr[prior_index][2];
                        box_h = expf(read_nc1hwc2_output(location, anchor_id * 4 + 3, y, x, height, width, loc_zp, loc_scale)
                                     * VARIANCES[1]) * prior_ptr[prior_index][3];

                        xmin = box_x - box_w * 0.5f;
                        ymin = box_y - box_h * 0.5f;
                        xmax = xmin + box_w;
                        ymax = ymin + box_h;

                        loc_fp32[offset + 0] = xmin;
                        loc_fp32[offset + 1] = ymin;
                        loc_fp32[offset + 2] = xmax;
                        loc_fp32[offset + 3] = ymax;
                        for (int j = 0; j < 5; j++)
                        {
                            landms_fp32[prior_index * 10 + 2 * j] =
                                read_nc1hwc2_output(landms, anchor_id * 10 + 2 * j, y, x, height, width, landms_zp, landms_scale)
                                * VARIANCES[0] * prior_ptr[prior_index][2] + prior_ptr[prior_index][0];
                            landms_fp32[prior_index * 10 + 2 * j + 1] =
                                read_nc1hwc2_output(landms, anchor_id * 10 + 2 * j + 1, y, x, height, width, landms_zp, landms_scale)
                                * VARIANCES[0] * prior_ptr[prior_index][3] + prior_ptr[prior_index][1];
                        }

                        ++validCount;
                    }

                    prior_index++;
                }
            }
        }
    }

    if (validCount <= 0)
    {
        od_results->count = 0;
        free(filter_indices);
        free(props);
        free(loc_fp32);
        free(landms_fp32);
        return ret;
    }

    //快速排列
    quick_sort_indice_inverse(props, 0, validCount - 1, filter_indices);

    //nms
    nms(validCount, loc_fp32, filter_indices, 0.2, 640, 640);

    uint8_t num_face_count = 0; 
    for (int i = 0; i < validCount; ++i) {
        if (num_face_count >= 4) {
            printf("Warning: face result limit reached, only keep 4 faces\n");
            break;
        }
        if (filter_indices[i] == -1 || props[i] < 0.5) {
            continue;
        }

        int n = filter_indices[i];
        
        
        float x1 = loc_fp32[n * 4 + 0] * app_ctx->model_width ;
        float y1 = loc_fp32[n * 4 + 1] * app_ctx->model_height;
        float x2 = loc_fp32[n * 4 + 2] * app_ctx->model_width ;
        float y2 = loc_fp32[n * 4 + 3] * app_ctx->model_height ;


        od_results->results[num_face_count].box.left   = (int)(clamp(x1, 0, 640) );
        od_results->results[num_face_count].box.top    = (int)(clamp(y1, 0, 640) );
        od_results->results[num_face_count].box.right  = (int)(clamp(x2, 0, 640) );
        od_results->results[num_face_count].box.bottom = (int)(clamp(y2, 0, 640) );
        od_results->results[num_face_count].prop = props[i];//置信度
        for(int j = 0;j < 5;j++)
        {
            float ponit_x = landms_fp32[n * 10 + 2 * j]*app_ctx->model_width;
            float ponit_y = landms_fp32[n * 10 + 2 * j+1]*app_ctx->model_height;

            od_results->results[num_face_count].point[j].x = (int)(clamp(ponit_x, 0, 640));
            od_results->results[num_face_count].point[j].y = (int)(clamp(ponit_y, 0, 640));
            //printf("x = %d,y=%d\n",(int)(clamp(ponit_x, 0, 640)),(int)(clamp(ponit_y, 0, 640)));
        }
        
        num_face_count++;
    }

    od_results->count = num_face_count;

    //printf("num_face_count = %d\n",num_face_count);

    free(filter_indices);
    free(props);
    free(loc_fp32);
    free(landms_fp32);
    return ret;
}

void mapCoordinates(cv::Mat input, cv::Mat output, int *x, int *y) {	
	float scaleX = (float)output.cols / (float)input.cols; 
	float scaleY = (float)output.rows / (float)input.rows;
    
    *x = (int)((float)*x / scaleX);
    *y = (int)((float)*y / scaleY);
}
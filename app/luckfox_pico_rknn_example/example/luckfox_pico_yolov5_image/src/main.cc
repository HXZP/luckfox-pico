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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yolov5.h"

//opencv
#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

// 将模型输入坐标映射回原始图片坐标。
static void mapCoordinates(const cv::Mat &input, const cv::Mat &output, int *x, int *y)
{
    float scaleX = (float)output.cols / (float)input.cols;
    float scaleY = (float)output.rows / (float)input.rows;
    
    *x = (int)((float)*x / scaleX);
    *y = (int)((float)*y / scaleY);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4)
    {
        printf("%s <yolov5 model_path> <input_image> [output_image]\n", argv[0]);
        return -1;
    }

    const char *model_path = argv[1];
    const char *input_image_path = argv[2];
    const char *output_image_path = (argc == 4) ? argv[3] : "/tmp/yolov5_image_result.jpg";
    char text[128];

    int model_width = 640;
    int model_height = 640;

    int ret;
    rknn_app_context_t rknn_app_ctx;
    object_detect_result_list od_results;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));

    init_yolov5_model(model_path, &rknn_app_ctx);
    init_post_process();

    cv::Mat bgr = cv::imread(input_image_path);
    if (bgr.empty())
    {
        printf("read image failed: %s\n", input_image_path);
        deinit_post_process();
        release_yolov5_model(&rknn_app_ctx);
        return -1;
    }

    printf("input image: %s, width=%d, height=%d\n", input_image_path, bgr.cols, bgr.rows);

    cv::Mat bgr_model_input(model_height, model_width, CV_8UC3, rknn_app_ctx.input_mems[0]->virt_addr);
    cv::resize(bgr, bgr_model_input, cv::Size(model_width, model_height), 0, 0, cv::INTER_LINEAR);
    inference_yolov5_model(&rknn_app_ctx, &od_results);

    printf("detect count: %d\n", od_results.count);
    for (int i = 0; i < od_results.count; i++)
    {
        object_detect_result *det_result = &(od_results.results[i]);

        mapCoordinates(bgr, bgr_model_input, &det_result->box.left, &det_result->box.top);
        mapCoordinates(bgr, bgr_model_input, &det_result->box.right, &det_result->box.bottom);

        printf("%s @ (%d %d %d %d) %.3f\n", coco_cls_to_name(det_result->cls_id),
               det_result->box.left, det_result->box.top,
               det_result->box.right, det_result->box.bottom,
               det_result->prop);

        cv::rectangle(bgr, cv::Point(det_result->box.left, det_result->box.top),
                      cv::Point(det_result->box.right, det_result->box.bottom), cv::Scalar(0, 255, 0), 3);

        snprintf(text, sizeof(text), "%s %.1f%%", coco_cls_to_name(det_result->cls_id), det_result->prop * 100);
        cv::putText(bgr, text, cv::Point(det_result->box.left, det_result->box.top - 8),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 255, 0), 2);
    }

    if (!cv::imwrite(output_image_path, bgr))
    {
        printf("write result image failed: %s\n", output_image_path);
        deinit_post_process();
        release_yolov5_model(&rknn_app_ctx);
        return -1;
    }

    printf("result image saved: %s\n", output_image_path);

    deinit_post_process();

    ret = release_yolov5_model(&rknn_app_ctx);
    if (ret != 0)
    {
        printf("release_yolov5_model fail! ret=%d\n", ret);
    }

    return 0;
}

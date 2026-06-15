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

/*-------------------------------------------
                Includes
-------------------------------------------*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yolov5.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"

#include <errno.h>
#include <unistd.h>   
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <time.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

//opencv
#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include "dma_alloc.cpp"

#define USE_DMA 0
#define SAVE_RESULT_DIR "./detect_result"
#define SAVE_PATH_MAX_LEN 256
#define MJPEG_STREAM_PORT 8080
#define MJPEG_JPEG_QUALITY 80
#define YOLO_STATUS_LOG_INTERVAL 30
#define FB_DEVICE "/dev/fb0"
#define DETECT_RESULT_TTL_MS 1000
#define MJPEG_UPDATE_INTERVAL 3
#define DISPLAY_IDLE_US 1000
#define DEFAULT_BENCH_CAMERA_WIDTH 240
#define DEFAULT_BENCH_CAMERA_HEIGHT 135
#define DEFAULT_INFERENCE_FPS 12

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static pthread_mutex_t g_mjpeg_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::vector<uchar> g_mjpeg_frame;
static pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_result_mutex = PTHREAD_MUTEX_INITIALIZER;
static cv::Mat g_latest_frame;
static unsigned int g_latest_frame_id = 0;
static object_detect_result_list g_latest_results;
static unsigned int g_latest_result_frame_id = 0;
static long long g_latest_result_time_ms = 0;
static volatile sig_atomic_t g_running = 1;

static long long get_monotonic_ms();

// 保存 framebuffer 设备信息和映射后的显存地址。
struct FramebufferInfo
{
    int fd;
    unsigned char *data;
    long int screensize;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    int bytes_per_pixel;
    int line_length;
    int width;
    int height;
};

// 推理线程所需的模型上下文和输入尺寸。
struct InferenceThreadArgs
{
    rknn_app_context_t *app_ctx;
    int model_width;
    int model_height;
    int inference_interval_ms;
};

// 处理退出信号，让主循环和推理线程可以一起退出。
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

// 读取环境变量，决定是否跳过 RKNN 推理，仅保留摄像头和 framebuffer 显示。
static int is_inference_enabled()
{
    const char *disable_inference;

    disable_inference = getenv("YOLO_DISABLE_INFERENCE");
    if (disable_inference != NULL && strcmp(disable_inference, "1") == 0)
    {
        return 0;
    }

    return 1;
}

// 读取离线推理测试帧数，返回 0 表示不进入 benchmark 模式。
static int get_benchmark_frame_count()
{
    const char *bench_frames;
    int frame_count;

    bench_frames = getenv("YOLO_BENCH_FRAMES");
    if (bench_frames == NULL || bench_frames[0] == '\0')
    {
        return 0;
    }

    frame_count = atoi(bench_frames);
    if (frame_count <= 0)
    {
        return 0;
    }

    return frame_count;
}

// 读取实时推理目标帧率，并转换为两次推理之间的最小间隔。
static int get_inference_interval_ms()
{
    const char *fps_text;
    int inference_fps;
    int interval_ms;

    fps_text = getenv("YOLO_INFERENCE_FPS");
    if (fps_text == NULL || fps_text[0] == '\0')
    {
        inference_fps = DEFAULT_INFERENCE_FPS;
    }
    else
    {
        inference_fps = atoi(fps_text);
        if (inference_fps <= 0)
        {
            inference_fps = DEFAULT_INFERENCE_FPS;
        }
    }

    interval_ms = 1000 / inference_fps;
    if (interval_ms <= 0)
    {
        interval_ms = 1;
    }

    printf("YOLO inference target fps=%d, interval=%dms\n", inference_fps, interval_ms);
    return interval_ms;
}

// 读取分时测试循环次数，返回 0 表示一直循环。
static int get_timeshare_loop_limit()
{
    const char *loop_text;
    int loop_limit;

    loop_text = getenv("YOLO_TIMESHARE_LOOPS");
    if (loop_text == NULL || loop_text[0] == '\0')
    {
        return 0;
    }

    loop_limit = atoi(loop_text);
    if (loop_limit < 0)
    {
        loop_limit = 0;
    }

    return loop_limit;
}

// 离线逐帧推理测试：先采集视频帧，再释放摄像头，最后逐帧记录 RKNN 推理耗时。
static int run_video_inference_benchmark(const char *model_path, int frame_count)
{
    std::vector<cv::Mat> frames;
    std::vector<double> resize_times;
    std::vector<double> inference_times;
    std::vector<double> total_times;
    cv::VideoCapture cap;
    rknn_app_context_t rknn_app_ctx;
    cv::Mat bgr_model_input;
    int model_width;
    int model_height;
    int ret;

    printf("benchmark start, frame_count=%d\n", frame_count);
    frames.reserve(frame_count);
    resize_times.reserve(frame_count);
    inference_times.reserve(frame_count);
    total_times.reserve(frame_count);

    cap.set(cv::CAP_PROP_FRAME_WIDTH, DEFAULT_BENCH_CAMERA_WIDTH);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, DEFAULT_BENCH_CAMERA_HEIGHT);
    cap.open(0);
    if (!cap.isOpened())
    {
        printf("benchmark open camera failed\n");
        return -1;
    }

    while ((int)frames.size() < frame_count)
    {
        cv::Mat frame;

        cap >> frame;
        if (frame.empty())
        {
            printf("benchmark capture empty frame, captured=%zu\n", frames.size());
            usleep(10 * 1000);
            continue;
        }

        frames.push_back(frame.clone());
    }

    cap.release();
    printf("benchmark captured %zu frames, camera released\n", frames.size());

    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));
    ret = init_yolov5_model(model_path, &rknn_app_ctx);
    if (ret != 0)
    {
        printf("benchmark init_yolov5_model fail, ret=%d\n", ret);
        return -1;
    }

    init_post_process();
    model_width = 640;
    model_height = 640;
    bgr_model_input = cv::Mat(model_height,
                              model_width,
                              CV_8UC3,
                              rknn_app_ctx.input_mems[0]->virt_addr);

    for (size_t i = 0; i < frames.size(); i++)
    {
        object_detect_result_list od_results;
        long long start_ms;
        long long after_resize_ms;
        long long after_inference_ms;

        memset(&od_results, 0, sizeof(object_detect_result_list));
        start_ms = get_monotonic_ms();
        cv::resize(frames[i],
                   bgr_model_input,
                   cv::Size(model_width, model_height),
                   0,
                   0,
                   cv::INTER_LINEAR);
        after_resize_ms = get_monotonic_ms();
        ret = inference_yolov5_model(&rknn_app_ctx, &od_results);
        after_inference_ms = get_monotonic_ms();

        resize_times.push_back((double)(after_resize_ms - start_ms));
        inference_times.push_back((double)(after_inference_ms - after_resize_ms));
        total_times.push_back((double)(after_inference_ms - start_ms));

        printf("bench frame=%zu resize_ms=%.3f inference_ms=%.3f total_ms=%.3f ret=%d detect_count=%d\n",
               i + 1,
               resize_times.back(),
               inference_times.back(),
               total_times.back(),
               ret,
               od_results.count);
    }

    if (!inference_times.empty())
    {
        double resize_sum;
        double inference_sum;
        double total_sum;
        double inference_min;
        double inference_max;

        resize_sum = 0.0;
        inference_sum = 0.0;
        total_sum = 0.0;
        inference_min = inference_times[0];
        inference_max = inference_times[0];

        for (size_t i = 0; i < inference_times.size(); i++)
        {
            resize_sum += resize_times[i];
            inference_sum += inference_times[i];
            total_sum += total_times[i];
            inference_min = std::min(inference_min, inference_times[i]);
            inference_max = std::max(inference_max, inference_times[i]);
        }

        printf("bench summary frames=%zu resize_avg_ms=%.3f inference_avg_ms=%.3f inference_min_ms=%.3f inference_max_ms=%.3f total_avg_ms=%.3f\n",
               inference_times.size(),
               resize_sum / inference_times.size(),
               inference_sum / inference_times.size(),
               inference_min,
               inference_max,
               total_sum / total_times.size());
    }

    deinit_post_process();
    ret = release_yolov5_model(&rknn_app_ctx);
    if (ret != 0)
    {
        printf("benchmark release_yolov5_model fail, ret=%d\n", ret);
        return -1;
    }

    printf("benchmark finish\n");
    return 0;
}

// 获取单调递增时间，避免系统时间变化影响检测结果保留时间。
static long long get_monotonic_ms()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((long long)ts.tv_sec * 1000) + ((long long)ts.tv_nsec / 1000000);
}

// 将检测框限制在当前图像范围内，避免绘制越界。
static void clamp_detect_box(object_detect_result *det_result, int image_width, int image_height)
{
    det_result->box.left = std::max(0, std::min(det_result->box.left, image_width - 1));
    det_result->box.right = std::max(0, std::min(det_result->box.right, image_width - 1));
    det_result->box.top = std::max(0, std::min(det_result->box.top, image_height - 1));
    det_result->box.bottom = std::max(0, std::min(det_result->box.bottom, image_height - 1));
}

// 更新最新摄像头帧，推理线程只读取最新一帧，避免排队堆积。
static void update_latest_frame(const cv::Mat &frame, unsigned int frame_id)
{
    pthread_mutex_lock(&g_frame_mutex);
    frame.copyTo(g_latest_frame);
    g_latest_frame_id = frame_id;
    pthread_mutex_unlock(&g_frame_mutex);
}

// 复制最新摄像头帧给推理线程，返回 0 表示拿到新帧。
static int copy_latest_frame(cv::Mat *frame, unsigned int *frame_id, unsigned int last_frame_id)
{
    int ret;

    ret = -1;
    pthread_mutex_lock(&g_frame_mutex);
    if (!g_latest_frame.empty() && g_latest_frame_id != last_frame_id)
    {
        g_latest_frame.copyTo(*frame);
        *frame_id = g_latest_frame_id;
        ret = 0;
    }
    pthread_mutex_unlock(&g_frame_mutex);

    return ret;
}

// 更新最近一次有效检测结果，显示线程会在短时间内复用该结果。
static void update_latest_results(const object_detect_result_list *results, unsigned int frame_id)
{
    pthread_mutex_lock(&g_result_mutex);
    memcpy(&g_latest_results, results, sizeof(object_detect_result_list));
    g_latest_result_frame_id = frame_id;
    g_latest_result_time_ms = get_monotonic_ms();
    pthread_mutex_unlock(&g_result_mutex);
}

// 获取仍在有效期内的检测结果，超时后返回空结果避免旧框长时间残留。
static void get_display_results(object_detect_result_list *results)
{
    long long now_ms;

    memset(results, 0, sizeof(object_detect_result_list));
    now_ms = get_monotonic_ms();

    pthread_mutex_lock(&g_result_mutex);
    if (g_latest_result_time_ms > 0 &&
        (now_ms - g_latest_result_time_ms) <= DETECT_RESULT_TTL_MS)
    {
        memcpy(results, &g_latest_results, sizeof(object_detect_result_list));
        results->id = g_latest_result_frame_id;
    }
    pthread_mutex_unlock(&g_result_mutex);
}

// 初始化 framebuffer，并映射显存用于直接写屏。
static int init_framebuffer(FramebufferInfo *fb)
{
    memset(fb, 0, sizeof(FramebufferInfo));
    fb->fd = -1;

    fb->fd = open(FB_DEVICE, O_RDWR);
    if (fb->fd == -1)
    {
        perror("open framebuffer failed");
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->vinfo) != 0)
    {
        perror("get framebuffer variable info failed");
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->finfo) != 0)
    {
        perror("get framebuffer fixed info failed");
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    fb->width = fb->vinfo.xres;
    fb->height = fb->vinfo.yres;
    fb->bytes_per_pixel = fb->vinfo.bits_per_pixel / 8;
    fb->line_length = fb->finfo.line_length;
    fb->screensize = fb->line_length * fb->height;
    fb->data = (unsigned char *)mmap(NULL, fb->screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->data == MAP_FAILED)
    {
        perror("mmap framebuffer failed");
        fb->data = NULL;
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    printf("Framebuffer: %dx%d, %dbpp, line_length=%d\n",
           fb->width,
           fb->height,
           fb->vinfo.bits_per_pixel,
           fb->line_length);

    if (fb->bytes_per_pixel != 2)
    {
        printf("unsupported framebuffer format: %dbpp, only RGB565 is supported\n", fb->vinfo.bits_per_pixel);
        munmap(fb->data, fb->screensize);
        fb->data = NULL;
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    return 0;
}

// 将 framebuffer 清为黑色，避免退出后残留最后一帧。
static void clear_framebuffer(const FramebufferInfo *fb)
{
    if (fb->data != NULL)
    {
        memset(fb->data, 0, fb->screensize);
    }
}

// 释放 framebuffer 显存映射和设备句柄。
static void close_framebuffer(FramebufferInfo *fb)
{
    if (fb->data != NULL)
    {
        munmap(fb->data, fb->screensize);
        fb->data = NULL;
    }

    if (fb->fd != -1)
    {
        close(fb->fd);
        fb->fd = -1;
    }
}

// 参考 simple_camera_fb 的方案，将 OpenCV BGR 图像转换为 RGB565 后写入 fb0。
static int display_mat_to_framebuffer(const FramebufferInfo *fb, const cv::Mat &image)
{
    cv::Mat rgb_image;
    int width;
    int height;

    if (image.empty() || fb->data == NULL || fb->bytes_per_pixel != 2)
    {
        return -1;
    }

    cv::cvtColor(image, rgb_image, cv::COLOR_BGR2RGB);
    width = std::min(image.cols, fb->width);
    height = std::min(image.rows, fb->height);

    for (int y = 0; y < height; y++)
    {
        const uint8_t *src;
        uint16_t *dst;
        int x;

        src = rgb_image.ptr<uint8_t>(y);
        dst = reinterpret_cast<uint16_t *>(fb->data + y * fb->line_length);
        x = 0;

        for (; x + 3 < width; x += 4)
        {
            uint16_t r0 = src[x * 3 + 0] >> 3;
            uint16_t g0 = src[x * 3 + 1] >> 2;
            uint16_t b0 = src[x * 3 + 2] >> 3;
            uint16_t r1 = src[x * 3 + 3] >> 3;
            uint16_t g1 = src[x * 3 + 4] >> 2;
            uint16_t b1 = src[x * 3 + 5] >> 3;
            uint16_t r2 = src[x * 3 + 6] >> 3;
            uint16_t g2 = src[x * 3 + 7] >> 2;
            uint16_t b2 = src[x * 3 + 8] >> 3;
            uint16_t r3 = src[x * 3 + 9] >> 3;
            uint16_t g3 = src[x * 3 + 10] >> 2;
            uint16_t b3 = src[x * 3 + 11] >> 3;

            dst[x + 0] = (r0 << 11) | (g0 << 5) | b0;
            dst[x + 1] = (r1 << 11) | (g1 << 5) | b1;
            dst[x + 2] = (r2 << 11) | (g2 << 5) | b2;
            dst[x + 3] = (r3 << 11) | (g3 << 5) | b3;
        }

        for (; x < width; x++)
        {
            uint16_t r = src[x * 3 + 0] >> 3;
            uint16_t g = src[x * 3 + 1] >> 2;
            uint16_t b = src[x * 3 + 2] >> 3;
            dst[x] = (r << 11) | (g << 5) | b;
        }
    }

    return 0;
}

// 将模型输入坐标映射回原始显示图像坐标。
void mapCoordinates(cv::Mat input, cv::Mat output, int *x, int *y)
{
    float scaleX = (float)output.cols / (float)input.cols; 
    float scaleY = (float)output.rows / (float)input.rows;
    
    *x = (int)((float)*x / scaleX);
    *y = (int)((float)*y / scaleY);
}

// 后台推理线程，只处理最新帧，避免 NPU 慢或超时时阻塞 framebuffer 刷新。
static void *inference_thread(void *arg)
{
    InferenceThreadArgs *thread_args;
    cv::Mat infer_frame;
    cv::Mat bgr_model_input;
    unsigned int last_frame_id;
    unsigned int current_frame_id;
    unsigned int inference_count;
    int inference_interval_ms;

    thread_args = (InferenceThreadArgs *)arg;
    bgr_model_input = cv::Mat(thread_args->model_height,
                              thread_args->model_width,
                              CV_8UC3,
                              thread_args->app_ctx->input_mems[0]->virt_addr);
    last_frame_id = 0;
    current_frame_id = 0;
    inference_count = 0;
    inference_interval_ms = thread_args->inference_interval_ms;

    while (g_running)
    {
        object_detect_result_list od_results;
        long long inference_start_ms;
        long long inference_end_ms;
        long long elapsed_ms;
        int ret;

        if (copy_latest_frame(&infer_frame, &current_frame_id, last_frame_id) != 0)
        {
            usleep(10 * 1000);
            continue;
        }

        last_frame_id = current_frame_id;
        inference_count++;
        memset(&od_results, 0, sizeof(object_detect_result_list));
        inference_start_ms = get_monotonic_ms();

        cv::resize(infer_frame,
                   bgr_model_input,
                   cv::Size(thread_args->model_width, thread_args->model_height),
                   0,
                   0,
                   cv::INTER_LINEAR);
        ret = inference_yolov5_model(thread_args->app_ctx, &od_results);
        inference_end_ms = get_monotonic_ms();
        elapsed_ms = inference_end_ms - inference_start_ms;

        if (elapsed_ms < inference_interval_ms)
        {
            usleep((inference_interval_ms - elapsed_ms) * 1000);
        }

        if (ret != 0)
        {
            if ((inference_count % YOLO_STATUS_LOG_INTERVAL) == 0)
            {
                printf("inference thread failed, frame=%u, ret=%d, cost_ms=%lld\n",
                       current_frame_id,
                       ret,
                       elapsed_ms);
            }
            continue;
        }

        for (int i = 0; i < od_results.count; i++)
        {
            object_detect_result *det_result;

            det_result = &(od_results.results[i]);
            mapCoordinates(infer_frame, bgr_model_input, &det_result->box.left, &det_result->box.top);
            mapCoordinates(infer_frame, bgr_model_input, &det_result->box.right, &det_result->box.bottom);
            clamp_detect_box(det_result, infer_frame.cols, infer_frame.rows);
        }

        if (od_results.count == 0)
        {
            if ((inference_count % YOLO_STATUS_LOG_INTERVAL) == 0)
            {
                printf("inference thread detect count=0, frame=%u, cost_ms=%lld\n",
                       current_frame_id,
                       elapsed_ms);
            }
            continue;
        }

        if ((inference_count % YOLO_STATUS_LOG_INTERVAL) == 0)
        {
            printf("inference thread ok, frame=%u, cost_ms=%lld, detect_count=%d\n",
                   current_frame_id,
                   elapsed_ms,
                   od_results.count);
        }

        update_latest_results(&od_results, current_frame_id);
    }

    return NULL;
}

// 确保检测结果保存目录存在。
static int ensure_save_dir(const char *dir_path)
{
    if (mkdir(dir_path, 0755) == 0)
    {
        return 0;
    }

    if (errno == EEXIST)
    {
        return 0;
    }

    printf("create save dir %s failed, errno=%d\n", dir_path, errno);
    return -1;
}

// 将类别名称转换为适合放入文件名的字符串。
static std::string make_safe_label_name(const char *label_name)
{
    std::string safe_name;

    if (label_name == NULL)
    {
        return "unknown";
    }

    for (const char *p = label_name; *p != '\0'; ++p)
    {
        if ((*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9'))
        {
            safe_name.push_back(*p);
        }
        else
        {
            safe_name.push_back('_');
        }
    }

    if (safe_name.empty())
    {
        return "unknown";
    }

    return safe_name;
}

// 根据类别、序号和当前时间生成图片保存路径。
static std::string build_save_path(const char *save_dir, int save_index, const char *label_name)
{
    char time_text[32];
    char save_path[SAVE_PATH_MAX_LEN];
    time_t now;
    struct tm tm_now;

    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(time_text, sizeof(time_text), "%Y%m%d_%H%M%S", &tm_now);

    snprintf(save_path,
             sizeof(save_path),
             "%s/%06d_%s_%s.jpg",
             save_dir,
             save_index,
             make_safe_label_name(label_name).c_str(),
             time_text);

    return save_path;
}

// 保存已经绘制检测框和类别文字的图像。
static int save_detect_image(const char *save_dir, int save_index, int cls_id, const cv::Mat &image)
{
    const char *label_name;
    std::string save_path;

    label_name = coco_cls_to_name(cls_id);
    save_path = build_save_path(save_dir, save_index, label_name);

    if (!cv::imwrite(save_path, image))
    {
        printf("save detect image failed: %s\n", save_path.c_str());
        return -1;
    }

    printf("save new object image: %s\n", save_path.c_str());
    return 0;
}

// 将完整缓冲区发送到客户端，避免短写导致 MJPEG 分片损坏。
static int send_all(int fd, const void *data, size_t data_len)
{
    const char *send_ptr;
    size_t remain_len;

    send_ptr = (const char *)data;
    remain_len = data_len;

    while (remain_len > 0)
    {
        ssize_t send_len;

        send_len = send(fd, send_ptr, remain_len, MSG_NOSIGNAL);
        if (send_len <= 0)
        {
            return -1;
        }

        send_ptr += send_len;
        remain_len -= send_len;
    }

    return 0;
}

// 更新 HTTP MJPEG 服务使用的最新一帧 JPEG 数据。
static int update_mjpeg_frame(const cv::Mat &image)
{
    std::vector<int> encode_params;
    std::vector<uchar> jpeg_buffer;

    encode_params.push_back(cv::IMWRITE_JPEG_QUALITY);
    encode_params.push_back(MJPEG_JPEG_QUALITY);

    if (!cv::imencode(".jpg", image, jpeg_buffer, encode_params))
    {
        printf("encode mjpeg frame failed\n");
        return -1;
    }

    pthread_mutex_lock(&g_mjpeg_mutex);
    g_mjpeg_frame.swap(jpeg_buffer);
    pthread_mutex_unlock(&g_mjpeg_mutex);

    return 0;
}

// 向单个 HTTP 客户端持续输出 multipart MJPEG 图像流。
static void stream_mjpeg_client(int client_fd)
{
    char request_buffer[512];
    const char *http_header =
        "HTTP/1.0 200 OK\r\n"
        "Server: yolo-fb-detect\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "\r\n";

    recv(client_fd, request_buffer, sizeof(request_buffer), 0);

    if (send_all(client_fd, http_header, strlen(http_header)) != 0)
    {
        return;
    }

    while (1)
    {
        char part_header[128];
        std::vector<uchar> frame_copy;

        pthread_mutex_lock(&g_mjpeg_mutex);
        frame_copy = g_mjpeg_frame;
        pthread_mutex_unlock(&g_mjpeg_mutex);

        if (frame_copy.empty())
        {
            usleep(100 * 1000);
            continue;
        }

        snprintf(part_header,
                 sizeof(part_header),
                 "--frame\r\n"
                 "Content-Type: image/jpeg\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n",
                 frame_copy.size());

        if (send_all(client_fd, part_header, strlen(part_header)) != 0)
        {
            break;
        }

        if (send_all(client_fd, frame_copy.data(), frame_copy.size()) != 0)
        {
            break;
        }

        if (send_all(client_fd, "\r\n", 2) != 0)
        {
            break;
        }

        usleep(100 * 1000);
    }
}

// 后台 HTTP 服务线程，浏览器访问 /stream.mjpg 即可查看带识别框画面。
static void *mjpeg_server_thread(void *arg)
{
    int server_fd;
    int enable;
    struct sockaddr_in server_addr;

    (void)arg;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        printf("create mjpeg socket failed, errno=%d\n", errno);
        return NULL;
    }

    enable = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(MJPEG_STREAM_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0)
    {
        printf("bind mjpeg port %d failed, errno=%d\n", MJPEG_STREAM_PORT, errno);
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 2) != 0)
    {
        printf("listen mjpeg port %d failed, errno=%d\n", MJPEG_STREAM_PORT, errno);
        close(server_fd);
        return NULL;
    }

    printf("mjpeg stream url: http://0.0.0.0:%d/stream.mjpg\n", MJPEG_STREAM_PORT);

    while (1)
    {
        int client_fd;

        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0)
        {
            continue;
        }

        stream_mjpeg_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return NULL;
}

// 启动 MJPEG HTTP 串流服务。
static int start_mjpeg_server()
{
    pthread_t thread_id;
    int ret;

    ret = pthread_create(&thread_id, NULL, mjpeg_server_thread, NULL);
    if (ret != 0)
    {
        printf("start mjpeg server failed, ret=%d\n", ret);
        return -1;
    }

    pthread_detach(thread_id);
    return 0;
}

/*-------------------------------------------
                  Main Function
-------------------------------------------*/
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc != 2)
    {
        printf("%s <yolov5 model_path>\n", argv[0]);
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    system("RkLunch-stop.sh");
    const char *model_path = argv[1];
    char text[128];
    int save_index = 0;
    int save_enable = 0;
    int loop_index = 0;
    int loop_limit = 0;
    std::set<int> saved_class_ids;

    int model_width = 640;
    int model_height = 640;

    int ret;
    rknn_app_context_t rknn_app_ctx;
    FramebufferInfo fb_info;
    cv::Mat bgr_model_input;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));

    ret = init_yolov5_model(model_path, &rknn_app_ctx);
    if (ret != 0)
    {
        printf("init_yolov5_model fail! ret=%d model_path=%s\n", ret, model_path);
        return -1;
    }

    init_post_process();
    if (ensure_save_dir(SAVE_RESULT_DIR) == 0)
    {
        save_enable = 1;
    }

    // 初始化 fb0 显示，分时推理完成后把最新标注帧写到 RGB565 framebuffer。
    if (init_framebuffer(&fb_info) != 0)
    {
        deinit_post_process();
        release_yolov5_model(&rknn_app_ctx);
        return -1;
    }
    clear_framebuffer(&fb_info);

    bgr_model_input = cv::Mat(model_height,
                              model_width,
                              CV_8UC3,
                              rknn_app_ctx.input_mems[0]->virt_addr);
    loop_limit = get_timeshare_loop_limit();
    printf("yolo_fb_timeshare start, camera and NPU run in separate time slots, loop_limit=%d\n",
           loop_limit);

    while (g_running)
    {
        std::vector<int> new_class_ids;
        object_detect_result_list od_results;
        cv::VideoCapture cap;
        cv::Mat bgr;
        long long loop_start_ms;
        long long capture_start_ms;
        long long capture_end_ms;
        long long resize_end_ms;
        long long inference_end_ms;
        int capture_try;

        loop_index++;
        memset(&od_results, 0, sizeof(object_detect_result_list));
        memset(text, 0, sizeof(text));

        loop_start_ms = get_monotonic_ms();
        capture_start_ms = loop_start_ms;

        cap.set(cv::CAP_PROP_FRAME_WIDTH, fb_info.width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, fb_info.height);
        cap.open(0);
        if (!cap.isOpened())
        {
            printf("loop=%d open camera failed\n", loop_index);
            usleep(200 * 1000);
            continue;
        }

        // 只取一帧用于本轮推理，随后立即释放摄像头，避免 ISP 和 NPU 同时运行。
        for (capture_try = 0; capture_try < 10; capture_try++)
        {
            cap >> bgr;
            if (!bgr.empty())
            {
                break;
            }

            usleep(10 * 1000);
        }

        cap.release();
        capture_end_ms = get_monotonic_ms();
        if (bgr.empty())
        {
            printf("loop=%d camera frame empty, capture_ms=%lld\n",
                   loop_index,
                   capture_end_ms - capture_start_ms);
            continue;
        }

        cv::resize(bgr,
                   bgr_model_input,
                   cv::Size(model_width, model_height),
                   0,
                   0,
                   cv::INTER_LINEAR);
        resize_end_ms = get_monotonic_ms();
        ret = inference_yolov5_model(&rknn_app_ctx, &od_results);
        inference_end_ms = get_monotonic_ms();

        if (ret == 0)
        {
            for (int i = 0; i < od_results.count; i++)
            {
                object_detect_result *det_result;

                det_result = &(od_results.results[i]);
                mapCoordinates(bgr, bgr_model_input, &det_result->box.left, &det_result->box.top);
                mapCoordinates(bgr, bgr_model_input, &det_result->box.right, &det_result->box.bottom);
                clamp_detect_box(det_result, bgr.cols, bgr.rows);

                printf("%s @ (%d %d %d %d) %.3f\n",
                       coco_cls_to_name(det_result->cls_id),
                       det_result->box.left,
                       det_result->box.top,
                       det_result->box.right,
                       det_result->box.bottom,
                       det_result->prop);

                cv::rectangle(bgr,
                              cv::Point(det_result->box.left, det_result->box.top),
                              cv::Point(det_result->box.right, det_result->box.bottom),
                              cv::Scalar(0, 255, 0),
                              3);

                sprintf(text, "%s %.1f%%", coco_cls_to_name(det_result->cls_id), det_result->prop * 100);
                cv::putText(bgr,
                            text,
                            cv::Point(det_result->box.left, det_result->box.top - 8),
                            cv::FONT_HERSHEY_SIMPLEX,
                            0.5,
                            cv::Scalar(0, 255, 0),
                            2);

                if (saved_class_ids.find(det_result->cls_id) == saved_class_ids.end())
                {
                    saved_class_ids.insert(det_result->cls_id);
                    new_class_ids.push_back(det_result->cls_id);
                }
            }

            if (save_enable)
            {
                for (size_t i = 0; i < new_class_ids.size(); i++)
                {
                    save_index++;
                    save_detect_image(SAVE_RESULT_DIR, save_index, new_class_ids[i], bgr);
                }
            }
        }

        printf("loop=%d capture_ms=%lld resize_ms=%lld inference_ms=%lld total_ms=%lld ret=%d detect_count=%d\n",
               loop_index,
               capture_end_ms - capture_start_ms,
               resize_end_ms - capture_end_ms,
               inference_end_ms - resize_end_ms,
               inference_end_ms - loop_start_ms,
               ret,
               od_results.count);

        display_mat_to_framebuffer(&fb_info, bgr);

        if (loop_limit > 0 && loop_index >= loop_limit)
        {
            printf("timeshare loop limit reached: %d\n", loop_limit);
            break;
        }
    }

    g_running = 0;
    deinit_post_process();

    clear_framebuffer(&fb_info);
    close_framebuffer(&fb_info);

    ret = release_yolov5_model(&rknn_app_ctx);
    if (ret != 0)
    {
        printf("release_yolov5_model fail! ret=%d\n", ret);
    }

    return 0;
}

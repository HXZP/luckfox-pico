// face_fb_detect - RetinaFace 实时检测并显示到 framebuffer。

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "retinaface_facenet.h"

#define FB_DEVICE "/dev/fb0"
#define RETINAFACE_MODEL_WIDTH 640
#define RETINAFACE_MODEL_HEIGHT 640

static volatile sig_atomic_t g_running = 1;

// 保存 framebuffer 设备参数和映射后的显存地址。
struct FramebufferInfo
{
    int fd;
    unsigned char *data;
    long int screensize;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    int bytes_per_pixel;
    int width;
    int height;
};

// 接收退出信号，使实时检测循环可以正常释放资源。
static void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
    {
        g_running = 0;
    }
}

// 获取单调时钟毫秒数，用于记录每轮耗时。
static long long get_monotonic_ms()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 初始化 framebuffer，并映射显存供后续直接写屏。
static int init_framebuffer(FramebufferInfo *fb_info)
{
    memset(fb_info, 0, sizeof(FramebufferInfo));
    fb_info->fd = open(FB_DEVICE, O_RDWR);
    if (fb_info->fd < 0)
    {
        printf("open %s failed, errno=%d\n", FB_DEVICE, errno);
        return -1;
    }

    if (ioctl(fb_info->fd, FBIOGET_VSCREENINFO, &fb_info->vinfo) != 0)
    {
        printf("FBIOGET_VSCREENINFO failed, errno=%d\n", errno);
        close(fb_info->fd);
        return -1;
    }

    if (ioctl(fb_info->fd, FBIOGET_FSCREENINFO, &fb_info->finfo) != 0)
    {
        printf("FBIOGET_FSCREENINFO failed, errno=%d\n", errno);
        close(fb_info->fd);
        return -1;
    }

    fb_info->width = fb_info->vinfo.xres;
    fb_info->height = fb_info->vinfo.yres;
    fb_info->bytes_per_pixel = fb_info->vinfo.bits_per_pixel / 8;
    fb_info->screensize = fb_info->finfo.line_length * fb_info->height;
    fb_info->data = (unsigned char *)mmap(NULL,
                                          fb_info->screensize,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED,
                                          fb_info->fd,
                                          0);
    if (fb_info->data == MAP_FAILED)
    {
        printf("mmap framebuffer failed, errno=%d\n", errno);
        close(fb_info->fd);
        return -1;
    }

    printf("Framebuffer: %dx%d, %dbpp, line_length=%d\n",
           fb_info->width,
           fb_info->height,
           fb_info->vinfo.bits_per_pixel,
           fb_info->finfo.line_length);
    return 0;
}

// 释放 framebuffer 映射和文件描述符。
static void close_framebuffer(FramebufferInfo *fb_info)
{
    if (fb_info->data != NULL && fb_info->data != MAP_FAILED)
    {
        munmap(fb_info->data, fb_info->screensize);
        fb_info->data = NULL;
    }

    if (fb_info->fd >= 0)
    {
        close(fb_info->fd);
        fb_info->fd = -1;
    }
}

// 将 BGR 图像转换为 framebuffer 格式并显示。
static int display_mat_to_framebuffer(FramebufferInfo *fb_info, const cv::Mat &bgr)
{
    cv::Mat resized;
    cv::Mat converted;

    if (bgr.empty())
    {
        return -1;
    }

    cv::resize(bgr, resized, cv::Size(fb_info->width, fb_info->height), 0, 0, cv::INTER_LINEAR);
    if (fb_info->bytes_per_pixel == 2)
    {
        cv::cvtColor(resized, converted, cv::COLOR_BGR2BGR565);
    }
    else if (fb_info->bytes_per_pixel == 4)
    {
        cv::cvtColor(resized, converted, cv::COLOR_BGR2BGRA);
    }
    else
    {
        printf("unsupported framebuffer bpp=%d\n", fb_info->vinfo.bits_per_pixel);
        return -1;
    }

    for (int y = 0; y < fb_info->height; y++)
    {
        memcpy(fb_info->data + y * fb_info->finfo.line_length,
               converted.data + y * converted.step,
               fb_info->width * fb_info->bytes_per_pixel);
    }

    return 0;
}

// 将检测框限制在图像范围内，避免绘制越界。
static void clamp_face_box(object_detect_result *result, int image_width, int image_height)
{
    if (result->box.left < 0)
    {
        result->box.left = 0;
    }
    if (result->box.top < 0)
    {
        result->box.top = 0;
    }
    if (result->box.right >= image_width)
    {
        result->box.right = image_width - 1;
    }
    if (result->box.bottom >= image_height)
    {
        result->box.bottom = image_height - 1;
    }
}

// 绘制人脸框和置信度，图像会直接用于本轮显示。
static void draw_face_results(cv::Mat &bgr, cv::Mat &model_input, object_detect_result_list *results)
{
    char text[64];

    for (int i = 0; i < results->count; i++)
    {
        object_detect_result *result;

        result = &(results->results[i]);
        mapCoordinates(bgr, model_input, &result->box.left, &result->box.top);
        mapCoordinates(bgr, model_input, &result->box.right, &result->box.bottom);
        clamp_face_box(result, bgr.cols, bgr.rows);

        cv::rectangle(bgr,
                      cv::Point(result->box.left, result->box.top),
                      cv::Point(result->box.right, result->box.bottom),
                      cv::Scalar(0, 255, 0),
                      2);
        snprintf(text, sizeof(text), "face %.1f%%", result->prop * 100.0f);
        cv::putText(bgr,
                    text,
                    cv::Point(result->box.left, result->box.top > 12 ? result->box.top - 6 : result->box.top + 14),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.45,
                    cv::Scalar(0, 255, 0),
                    1);
    }
}

// 程序入口：循环执行一帧采集、一次 RetinaFace 推理、一次 framebuffer 显示。
int main(int argc, char **argv)
{
    const char *model_path;
    rknn_app_context_t app_ctx;
    FramebufferInfo fb_info;
    cv::VideoCapture cap;
    cv::Mat bgr;
    cv::Mat model_input;
    int ret;
    unsigned int frame_id;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc != 2)
    {
        printf("%s <retinaface model_path>\n", argv[0]);
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    system("RkLunch-stop.sh");

    model_path = argv[1];
    memset(&app_ctx, 0, sizeof(rknn_app_context_t));
    memset(&fb_info, 0, sizeof(FramebufferInfo));
    fb_info.fd = -1;
    frame_id = 0;

    ret = init_retinaface_model(model_path, &app_ctx);
    if (ret != 0)
    {
        printf("init_retinaface_model failed, ret=%d, model=%s\n", ret, model_path);
        return -1;
    }

    if (init_framebuffer(&fb_info) != 0)
    {
        release_retinaface_model(&app_ctx);
        return -1;
    }

    model_input = cv::Mat(RETINAFACE_MODEL_HEIGHT,
                          RETINAFACE_MODEL_WIDTH,
                          CV_8UC3,
                          app_ctx.input_mems[0]->virt_addr);
    cap.set(cv::CAP_PROP_FRAME_WIDTH, fb_info.width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, fb_info.height);
    cap.open(0);
    if (!cap.isOpened())
    {
        printf("open camera failed\n");
        close_framebuffer(&fb_info);
        release_retinaface_model(&app_ctx);
        return -1;
    }

    printf("face_fb_detect start: one frame, one inference, one display\n");
    while (g_running)
    {
        object_detect_result_list results;
        long long loop_start_ms;
        long long capture_end_ms;
        long long resize_end_ms;
        long long inference_end_ms;
        long long display_end_ms;

        memset(&results, 0, sizeof(object_detect_result_list));
        loop_start_ms = get_monotonic_ms();
        cap >> bgr;
        capture_end_ms = get_monotonic_ms();
        if (bgr.empty())
        {
            printf("frame=%u camera frame empty\n", frame_id);
            usleep(10 * 1000);
            continue;
        }

        cv::resize(bgr,
                   model_input,
                   cv::Size(RETINAFACE_MODEL_WIDTH, RETINAFACE_MODEL_HEIGHT),
                   0,
                   0,
                   cv::INTER_LINEAR);
        resize_end_ms = get_monotonic_ms();
        ret = inference_retinaface_model(&app_ctx, &results);
        inference_end_ms = get_monotonic_ms();
        if (ret == 0)
        {
            draw_face_results(bgr, model_input, &results);
        }
        else
        {
            printf("frame=%u inference failed, ret=%d\n", frame_id, ret);
        }

        display_mat_to_framebuffer(&fb_info, bgr);
        display_end_ms = get_monotonic_ms();
        printf("frame=%u capture_ms=%lld resize_ms=%lld inference_ms=%lld display_ms=%lld total_ms=%lld ret=%d face_count=%d\n",
               frame_id,
               capture_end_ms - loop_start_ms,
               resize_end_ms - capture_end_ms,
               inference_end_ms - resize_end_ms,
               display_end_ms - inference_end_ms,
               display_end_ms - loop_start_ms,
               ret,
               results.count);
        frame_id++;
    }

    cap.release();
    close_framebuffer(&fb_info);
    release_retinaface_model(&app_ctx);
    return 0;
}

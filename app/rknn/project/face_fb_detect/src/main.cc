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
#include <pthread.h>
#include <unistd.h>

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "retinaface_facenet.h"
#include <vector>

#define FB_DEVICE "/dev/fb0"
#define RETINAFACE_MODEL_WIDTH 640
#define RETINAFACE_MODEL_HEIGHT 640
#define DEFAULT_FACE_REALTIME_INTERVAL_MS 30
#define DEFAULT_FACE_DISPLAY_INTERVAL_MS 30
#define FACE_RESULT_TTL_MS 1000
#define FACE_STATUS_LOG_INTERVAL 30

static pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_result_mutex = PTHREAD_MUTEX_INITIALIZER;
static cv::Mat g_latest_frame;
static unsigned int g_latest_frame_id = 0;
static object_detect_result_list g_latest_results;
static long long g_latest_result_time_ms = 0;
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

// 并行模式下，后台推理线程拿最新帧，主线程持续刷新 framebuffer。
struct InferenceThreadArgs
{
    rknn_app_context_t *app_ctx;
    int model_width;
    int model_height;
    int inference_interval_ms;
};

struct RuntimeOptions
{
    int parallel_output;
    int inference_interval_ms;
    int display_interval_ms;
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

static void print_usage(const char *program_name)
{
    printf("usage:\n");
    printf("  %s <retinaface model_path> serial <inference_interval_ms>\n", program_name);
    printf("  %s <retinaface model_path> parallel <inference_interval_ms> <display_interval_ms>\n", program_name);
    printf("examples:\n");
    printf("  %s ./model/retinaface.rknn serial 30\n", program_name);
    printf("  %s ./model/retinaface.rknn parallel 30 30\n", program_name);
}

static int parse_positive_int(const char *text, const char *name, int *value)
{
    char *end_ptr;
    long parsed;

    if (text == NULL || text[0] == '\0')
    {
        printf("%s is required\n", name);
        return -1;
    }

    errno = 0;
    end_ptr = NULL;
    parsed = strtol(text, &end_ptr, 10);
    if (errno != 0 || end_ptr == text || *end_ptr != '\0' || parsed <= 0 || parsed > 600000)
    {
        printf("%s must be a positive integer in milliseconds: %s\n", name, text);
        return -1;
    }

    *value = (int)parsed;
    return 0;
}

// 解析命令行输出模式：serial 为采一帧推一帧显示一帧，parallel 为后台推理、前台持续显示。
static int parse_runtime_options(int argc, char **argv, RuntimeOptions *options)
{
    const char *mode_text;

    if (argc < 4)
    {
        print_usage(argv[0]);
        return -1;
    }

    memset(options, 0, sizeof(RuntimeOptions));
    mode_text = argv[2];
    if (strcmp(mode_text, "serial") == 0 || strcmp(mode_text, "sync") == 0)
    {
        if (argc != 4)
        {
            print_usage(argv[0]);
            return -1;
        }
        options->parallel_output = 0;
        if (parse_positive_int(argv[3], "inference_interval_ms", &options->inference_interval_ms) != 0)
        {
            print_usage(argv[0]);
            return -1;
        }
        options->display_interval_ms = options->inference_interval_ms;
        return 0;
    }

    if (strcmp(mode_text, "parallel") == 0 || strcmp(mode_text, "async") == 0)
    {
        if (argc != 5)
        {
            print_usage(argv[0]);
            return -1;
        }
        options->parallel_output = 1;
        if (parse_positive_int(argv[3], "inference_interval_ms", &options->inference_interval_ms) != 0 ||
            parse_positive_int(argv[4], "display_interval_ms", &options->display_interval_ms) != 0)
        {
            print_usage(argv[0]);
            return -1;
        }
        return 0;
    }

    printf("unknown mode: %s\n", mode_text);
    print_usage(argv[0]);
    return -1;
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

// 更新最新摄像头帧，推理线程只处理最新帧，避免排队导致延迟越来越高。
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

// 保存最近一次有效结果。结果保持模型坐标，显示前再映射到当前帧坐标。
static void update_latest_results(const object_detect_result_list *results)
{
    pthread_mutex_lock(&g_result_mutex);
    memcpy(&g_latest_results, results, sizeof(object_detect_result_list));
    g_latest_result_time_ms = get_monotonic_ms();
    pthread_mutex_unlock(&g_result_mutex);
}

// 获取仍在有效期内的检测结果，超时后返回空结果。
static void get_display_results(object_detect_result_list *results)
{
    long long now_ms;

    memset(results, 0, sizeof(object_detect_result_list));
    now_ms = get_monotonic_ms();

    pthread_mutex_lock(&g_result_mutex);
    if (g_latest_result_time_ms > 0 &&
        (now_ms - g_latest_result_time_ms) <= FACE_RESULT_TTL_MS)
    {
        memcpy(results, &g_latest_results, sizeof(object_detect_result_list));
    }
    pthread_mutex_unlock(&g_result_mutex);
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

// 绘制 framebuffer 上的实时帧率。
static void draw_fps(cv::Mat &bgr, float fps)
{
    char text[32];

    snprintf(text, sizeof(text), "fps=%.1f", fps);
    cv::putText(bgr,
                text,
                cv::Point(0, 20),
                cv::FONT_HERSHEY_SIMPLEX,
                0.5,
                cv::Scalar(0, 255, 0),
                1);
}

// 判断是否进入单帧离线推理计时模式。
static int is_single_inference_benchmark_enabled()
{
    const char *bench_text;

    bench_text = getenv("FACE_BENCH_ONCE");
    if (bench_text != NULL && strcmp(bench_text, "1") == 0)
    {
        return 1;
    }

    return 0;
}

// 判断是否进入连续采样后逐帧推理的离线计时模式。
static int is_sequence_inference_benchmark_enabled()
{
    const char *bench_text;

    bench_text = getenv("FACE_BENCH_SEQUENCE");
    if (bench_text != NULL && strcmp(bench_text, "1") == 0)
    {
        return 1;
    }

    return 0;
}

// 获取连续采样秒数，默认采样 3 秒。
static int get_sequence_benchmark_seconds()
{
    const char *seconds_text;
    int seconds;

    seconds_text = getenv("FACE_BENCH_SECONDS");
    if (seconds_text == NULL)
    {
        return 3;
    }

    seconds = atoi(seconds_text);
    if (seconds <= 0)
    {
        return 3;
    }

    return seconds;
}

// 并行模式后台推理线程：拿最新帧推理，主线程不等待 NPU。
static void *parallel_inference_thread(void *arg)
{
    InferenceThreadArgs *thread_args;
    cv::Mat infer_frame;
    cv::Mat model_input;
    unsigned int last_frame_id;
    unsigned int current_frame_id;
    unsigned int inference_count;

    thread_args = (InferenceThreadArgs *)arg;
    model_input = cv::Mat(thread_args->model_height,
                          thread_args->model_width,
                          CV_8UC3,
                          thread_args->app_ctx->input_mems[0]->virt_addr);
    last_frame_id = 0;
    current_frame_id = 0;
    inference_count = 0;

    while (g_running)
    {
        object_detect_result_list results;
        long long inference_start_ms;
        long long resize_end_ms;
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
        memset(&results, 0, sizeof(object_detect_result_list));
        inference_start_ms = get_monotonic_ms();
        cv::resize(infer_frame,
                   model_input,
                   cv::Size(thread_args->model_width, thread_args->model_height),
                   0,
                   0,
                   cv::INTER_LINEAR);
        resize_end_ms = get_monotonic_ms();
        ret = inference_retinaface_model(thread_args->app_ctx, &results);
        inference_end_ms = get_monotonic_ms();
        elapsed_ms = inference_end_ms - inference_start_ms;
        if (ret == 0)
        {
            update_latest_results(&results);
        }

        if (ret != 0 || (inference_count % FACE_STATUS_LOG_INTERVAL) == 0)
        {
            printf("parallel inference frame=%u resize_ms=%lld inference_ms=%lld total_ms=%lld sleep_target_ms=%d ret=%d face_count=%d\n",
                   current_frame_id,
                   resize_end_ms - inference_start_ms,
                   inference_end_ms - resize_end_ms,
                   elapsed_ms,
                   thread_args->inference_interval_ms,
                   ret,
                   results.count);
        }

        if (elapsed_ms < thread_args->inference_interval_ms)
        {
            usleep((thread_args->inference_interval_ms - elapsed_ms) * 1000);
        }
    }

    return NULL;
}

// 采集一帧后释放摄像头，再执行一次 RetinaFace 推理，用于测量非并发推理耗时。
static int run_single_inference_benchmark(rknn_app_context_t *app_ctx,
                                          FramebufferInfo *fb_info,
                                          cv::Mat &model_input)
{
    cv::VideoCapture cap;
    cv::Mat bgr;
    object_detect_result_list results;
    long long start_ms;
    long long capture_end_ms;
    long long resize_end_ms;
    long long inference_end_ms;
    long long display_end_ms;
    int ret;

    memset(&results, 0, sizeof(object_detect_result_list));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, fb_info->width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, fb_info->height);

    start_ms = get_monotonic_ms();
    cap.open(0);
    if (!cap.isOpened())
    {
        printf("bench open camera failed\n");
        return -1;
    }

    for (int i = 0; i < 10; i++)
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
        printf("bench camera frame empty\n");
        return -1;
    }

    cv::resize(bgr,
               model_input,
               cv::Size(RETINAFACE_MODEL_WIDTH, RETINAFACE_MODEL_HEIGHT),
               0,
               0,
               cv::INTER_LINEAR);
    resize_end_ms = get_monotonic_ms();
    ret = inference_retinaface_model(app_ctx, &results);
    inference_end_ms = get_monotonic_ms();
    if (ret == 0)
    {
        draw_face_results(bgr, model_input, &results);
    }
    display_mat_to_framebuffer(fb_info, bgr);
    display_end_ms = get_monotonic_ms();

    printf("face_bench_once capture_ms=%lld resize_ms=%lld inference_ms=%lld display_ms=%lld total_ms=%lld ret=%d face_count=%d\n",
           capture_end_ms - start_ms,
           resize_end_ms - capture_end_ms,
           inference_end_ms - resize_end_ms,
           display_end_ms - inference_end_ms,
           display_end_ms - start_ms,
           ret,
           results.count);
    return ret;
}

// 连续采样一段时间后释放摄像头，再逐帧推理并统计识别耗时。
static int run_sequence_inference_benchmark(rknn_app_context_t *app_ctx,
                                            FramebufferInfo *fb_info,
                                            cv::Mat &model_input)
{
    std::vector<cv::Mat> frames;
    cv::VideoCapture cap;
    long long capture_start_ms;
    long long capture_end_ms;
    long long target_capture_ms;
    long long total_inference_ms;
    long long total_frame_ms;
    long long min_inference_ms;
    long long max_inference_ms;
    int sample_seconds;
    int failed_count;

    sample_seconds = get_sequence_benchmark_seconds();
    target_capture_ms = (long long)sample_seconds * 1000;
    total_inference_ms = 0;
    total_frame_ms = 0;
    min_inference_ms = 0;
    max_inference_ms = 0;
    failed_count = 0;

    cap.set(cv::CAP_PROP_FRAME_WIDTH, fb_info->width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, fb_info->height);
    capture_start_ms = get_monotonic_ms();
    cap.open(0);
    if (!cap.isOpened())
    {
        printf("sequence open camera failed\n");
        return -1;
    }

    while (g_running)
    {
        cv::Mat frame;
        long long now_ms;

        now_ms = get_monotonic_ms();
        if (now_ms - capture_start_ms >= target_capture_ms)
        {
            break;
        }

        cap >> frame;
        if (!frame.empty())
        {
            frames.push_back(frame.clone());
        }
        else
        {
            usleep(10 * 1000);
        }
    }

    cap.release();
    capture_end_ms = get_monotonic_ms();
    printf("face_bench_sequence_sample sample_seconds=%d capture_ms=%lld frame_count=%zu\n",
           sample_seconds,
           capture_end_ms - capture_start_ms,
           frames.size());

    if (frames.empty())
    {
        printf("sequence camera frame empty\n");
        return -1;
    }

    for (size_t i = 0; i < frames.size() && g_running; i++)
    {
        object_detect_result_list results;
        long long frame_start_ms;
        long long resize_end_ms;
        long long inference_end_ms;
        long long display_end_ms;
        long long inference_ms;
        long long frame_ms;
        int ret;

        memset(&results, 0, sizeof(object_detect_result_list));
        frame_start_ms = get_monotonic_ms();
        cv::resize(frames[i],
                   model_input,
                   cv::Size(RETINAFACE_MODEL_WIDTH, RETINAFACE_MODEL_HEIGHT),
                   0,
                   0,
                   cv::INTER_LINEAR);
        resize_end_ms = get_monotonic_ms();
        ret = inference_retinaface_model(app_ctx, &results);
        inference_end_ms = get_monotonic_ms();
        if (ret == 0)
        {
            draw_face_results(frames[i], model_input, &results);
        }
        else
        {
            failed_count++;
        }

        display_mat_to_framebuffer(fb_info, frames[i]);
        display_end_ms = get_monotonic_ms();
        inference_ms = inference_end_ms - resize_end_ms;
        frame_ms = display_end_ms - frame_start_ms;
        total_inference_ms += inference_ms;
        total_frame_ms += frame_ms;
        if (i == 0 || inference_ms < min_inference_ms)
        {
            min_inference_ms = inference_ms;
        }
        if (i == 0 || inference_ms > max_inference_ms)
        {
            max_inference_ms = inference_ms;
        }

        printf("face_bench_sequence_frame index=%zu resize_ms=%lld inference_ms=%lld display_ms=%lld total_ms=%lld ret=%d face_count=%d\n",
               i,
               resize_end_ms - frame_start_ms,
               inference_ms,
               display_end_ms - inference_end_ms,
               frame_ms,
               ret,
               results.count);
    }

    printf("face_bench_sequence_summary sample_seconds=%d frame_count=%zu failed_count=%d avg_inference_ms=%lld min_inference_ms=%lld max_inference_ms=%lld total_inference_ms=%lld avg_total_frame_ms=%lld\n",
           sample_seconds,
           frames.size(),
           failed_count,
           total_inference_ms / (long long)frames.size(),
           min_inference_ms,
           max_inference_ms,
           total_inference_ms,
           total_frame_ms / (long long)frames.size());
    return failed_count == 0 ? 0 : -1;
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
    int realtime_interval_ms;
    int display_interval_ms;
    int parallel_output;
    int inference_thread_started;
    float fps;
    pthread_t inference_thread_id;
    InferenceThreadArgs inference_args;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    RuntimeOptions runtime_options;
    if (parse_runtime_options(argc, argv, &runtime_options) != 0)
    {
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
    realtime_interval_ms = runtime_options.inference_interval_ms;
    display_interval_ms = runtime_options.display_interval_ms;
    parallel_output = runtime_options.parallel_output;
    inference_thread_started = 0;
    fps = 0.0f;

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

    if (is_single_inference_benchmark_enabled())
    {
        ret = run_single_inference_benchmark(&app_ctx, &fb_info, model_input);
        close_framebuffer(&fb_info);
        release_retinaface_model(&app_ctx);
        return ret;
    }

    if (is_sequence_inference_benchmark_enabled())
    {
        ret = run_sequence_inference_benchmark(&app_ctx, &fb_info, model_input);
        close_framebuffer(&fb_info);
        release_retinaface_model(&app_ctx);
        return ret;
    }

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
    printf("face_fb_detect camera opened\n");

    if (parallel_output)
    {
        inference_args.app_ctx = &app_ctx;
        inference_args.model_width = RETINAFACE_MODEL_WIDTH;
        inference_args.model_height = RETINAFACE_MODEL_HEIGHT;
        inference_args.inference_interval_ms = realtime_interval_ms;
        ret = pthread_create(&inference_thread_id, NULL, parallel_inference_thread, &inference_args);
        if (ret != 0)
        {
            printf("start parallel inference thread failed, ret=%d\n", ret);
            cap.release();
            close_framebuffer(&fb_info);
            release_retinaface_model(&app_ctx);
            return -1;
        }
        inference_thread_started = 1;
    }

    printf("face_fb_detect start: mode=%s, %s, inference_interval_ms=%d, display_interval_ms=%d\n",
           parallel_output ? "parallel" : "serial",
           parallel_output ? "display every captured frame, infer latest frame in background" : "one frame, one inference, one display",
           realtime_interval_ms,
           parallel_output ? display_interval_ms : realtime_interval_ms);
    while (g_running)
    {
        object_detect_result_list results;
        long long loop_start_ms;
        long long capture_end_ms;
        long long resize_end_ms;
        long long inference_end_ms;
        long long display_end_ms;
        long long loop_process_ms;
        long long sleep_ms;
        long long overrun_ms;
        int current_interval_ms;

        memset(&results, 0, sizeof(object_detect_result_list));
        ret = 0;
        loop_start_ms = get_monotonic_ms();
        if (frame_id == 0)
        {
            printf("frame=0 capture begin\n");
        }
        cap >> bgr;
        capture_end_ms = get_monotonic_ms();
        if (frame_id == 0)
        {
            printf("frame=0 capture end, empty=%d\n", bgr.empty() ? 1 : 0);
        }
        if (bgr.empty())
        {
            printf("frame=%u camera frame empty\n", frame_id);
            usleep(10 * 1000);
            continue;
        }

        if (parallel_output)
        {
            update_latest_frame(bgr, frame_id);
            get_display_results(&results);
            resize_end_ms = capture_end_ms;
            inference_end_ms = resize_end_ms;
            draw_face_results(bgr, model_input, &results);
        }
        else
        {
            cv::resize(bgr,
                       model_input,
                       cv::Size(RETINAFACE_MODEL_WIDTH, RETINAFACE_MODEL_HEIGHT),
                       0,
                       0,
                       cv::INTER_LINEAR);
            resize_end_ms = get_monotonic_ms();
            if (frame_id == 0)
            {
                printf("frame=0 inference begin\n");
            }
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
        }

        draw_fps(bgr, fps);
        display_mat_to_framebuffer(&fb_info, bgr);
        display_end_ms = get_monotonic_ms();
        loop_process_ms = display_end_ms - loop_start_ms;
        if (loop_process_ms > 0)
        {
            fps = 1000.0f / (float)loop_process_ms;
        }
        sleep_ms = 0;
        overrun_ms = 0;
        current_interval_ms = parallel_output ? display_interval_ms : realtime_interval_ms;
        if (loop_process_ms < current_interval_ms)
        {
            sleep_ms = current_interval_ms - loop_process_ms;
            usleep(sleep_ms * 1000);
        }
        else
        {
            overrun_ms = loop_process_ms - current_interval_ms;
        }

        printf("frame=%u mode=%s capture_ms=%lld resize_ms=%lld inference_ms=%lld display_ms=%lld process_ms=%lld sleep_ms=%lld overrun_ms=%lld fps=%.1f ret=%d face_count=%d\n",
               frame_id,
               parallel_output ? "parallel" : "serial",
               capture_end_ms - loop_start_ms,
               resize_end_ms - capture_end_ms,
               inference_end_ms - resize_end_ms,
               display_end_ms - inference_end_ms,
               loop_process_ms,
               sleep_ms,
               overrun_ms,
               fps,
               ret,
               results.count);
        frame_id++;
    }

    g_running = 0;
    if (inference_thread_started)
    {
        pthread_join(inference_thread_id, NULL);
    }
    cap.release();
    close_framebuffer(&fb_info);
    release_retinaface_model(&app_ctx);
    return 0;
}

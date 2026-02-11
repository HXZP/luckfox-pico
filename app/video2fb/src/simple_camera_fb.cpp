#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <signal.h>

// 配置
#define FB_DEVICE "/dev/fb0"


// Framebuffer 显示结构体
struct FramebufferInfo {
    int fd;
    unsigned char *data; // 映射的内存指针
    long int screensize;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    int bytes_per_pixel;
    int line_length;
    int width;
    int height;
};

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 135
#define CAMERA_WIDTH 240
#define CAMERA_HEIGHT 135

int init_framebuffer(FramebufferInfo &fb) {
    fb.fd = open(FB_DEVICE, O_RDWR);
    if (fb.fd == -1) {
        perror("错误：无法打开 framebuffer 设备");
        return -1;
    }

    // 获取屏幕可变信息
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.vinfo)) {
        perror("错误：读取屏幕可变信息失败");
        close(fb.fd);
        return -1;
    }

    // 获取屏幕固定信息
    if (ioctl(fb.fd, FBIOGET_FSCREENINFO, &fb.finfo)) {
        perror("错误：读取屏幕固定信息失败");
        close(fb.fd);
        return -1;
    }

    fb.width = fb.vinfo.xres;
    fb.height = fb.vinfo.yres;
    fb.bytes_per_pixel = fb.vinfo.bits_per_pixel / 8;
    fb.line_length = fb.finfo.line_length;

    printf("Framebuffer: %dx%d, %dbpp\n", 
           fb.vinfo.xres, fb.vinfo.yres, fb.vinfo.bits_per_pixel);

    // 计算屏幕大小并映射内存
    fb.screensize = fb.vinfo.xres * fb.vinfo.yres * fb.vinfo.bits_per_pixel / 8;
    fb.data = (unsigned char*)mmap(0, fb.screensize, 
                                    PROT_READ | PROT_WRITE, 
                                    MAP_SHARED, fb.fd, 0);
    if ((long)fb.data == -1) {
        perror("错误：映射 framebuffer 内存失败");
        close(fb.fd);
        return -1;
    }

    if (fb.width != CAMERA_WIDTH || fb.height != CAMERA_HEIGHT) {
        printf("警告: Framebuffer 分辨率 (%dx%d) 与摄像头分辨率 (%dx%d) 不匹配\n",
               fb.width, fb.height, CAMERA_WIDTH, CAMERA_HEIGHT);
    }

    return 0;
}



void display_mat_to_framebuffer(const FramebufferInfo &fb, const cv::Mat &image) {
    if (image.empty() || fb.data == NULL || fb.bytes_per_pixel != 2) return;
    
    // 先转换 BGR 到 RGB
    cv::Mat rgb_image;
    cv::cvtColor(image, rgb_image, cv::COLOR_BGR2RGB);
    
    int width = std::min(image.cols, fb.width);
    int height = std::min(image.rows, fb.height);
    
    // 使用指针和循环展开优化
    for (int y = 0; y < height; y++) {
        const uint8_t* src = rgb_image.ptr<uint8_t>(y);
        uint16_t* dst = reinterpret_cast<uint16_t*>(fb.data + y * fb.line_length);
        
        int x = 0;
        
        // 循环展开：每次处理 4 个像素
        for (; x + 3 < width; x += 4) {
            // 像素 0
            uint16_t r0 = src[x*3 + 0] >> 3;
            uint16_t g0 = src[x*3 + 1] >> 2;
            uint16_t b0 = src[x*3 + 2] >> 3;
            dst[x + 0] = (r0 << 11) | (g0 << 5) | b0;
            
            // 像素 1
            uint16_t r1 = src[x*3 + 3] >> 3;
            uint16_t g1 = src[x*3 + 4] >> 2;
            uint16_t b1 = src[x*3 + 5] >> 3;
            dst[x + 1] = (r1 << 11) | (g1 << 5) | b1;
            
            // 像素 2
            uint16_t r2 = src[x*3 + 6] >> 3;
            uint16_t g2 = src[x*3 + 7] >> 2;
            uint16_t b2 = src[x*3 + 8] >> 3;
            dst[x + 2] = (r2 << 11) | (g2 << 5) | b2;
            
            // 像素 3
            uint16_t r3 = src[x*3 + 9] >> 3;
            uint16_t g3 = src[x*3 + 10] >> 2;
            uint16_t b3 = src[x*3 + 11] >> 3;
            dst[x + 3] = (r3 << 11) | (g3 << 5) | b3;
        }
        
        // 处理剩余的像素
        for (; x < width; x++) {
            uint16_t r = src[x*3 + 0] >> 3;
            uint16_t g = src[x*3 + 1] >> 2;
            uint16_t b = src[x*3 + 2] >> 3;
            dst[x] = (r << 11) | (g << 5) | b;
        }
    }
}

// #include </home/hxzp/luckfox-zero/media/rga/release_rga_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rga/im2d.h>
// // 终极优化：用 RGA 一步完成 BGR888 → RGB565 + 通道交换
// int display_mat_to_framebuffer_rga(const FramebufferInfo &fb, const cv::Mat &image) {
//     if (image.empty() || fb.data == NULL || fb.bytes_per_pixel != 2) return -1;
    
//     int width = std::min(image.cols, fb.width);
//     int height = std::min(image.rows, fb.height);
    
//     // 1. 包装输入：BGR888（OpenCV默认格式）
//     rga_buffer_t src = wrapbuffer_virtualaddr((void*)image.data, width, height, 
//                                               RK_FORMAT_BGR_888);
    
//     // 2. 包装输出：直接映射到 framebuffer 内存！
//     //    关键优化：不需要中间 buffer，直接写入 fb.data
//     rga_buffer_t dst = wrapbuffer_virtualaddr(fb.data, fb.width, fb.height, 
//                                               RK_FORMAT_RGB_565);
//     // 注意：这里用 RGB_565 而不是 BGR_565，因为 fb 期望 RGB 顺序
    
//     // 3. 执行硬件转换
//     //    使用 imcvtcolor 同时完成：BGR→RGB 通道交换 + RGB888→RGB565 位压缩
//     int ret = imcvtcolor(src, dst, RK_FORMAT_BGR_888, RK_FORMAT_RGB_565);
    
//     if (ret != IM_STATUS_SUCCESS) {
//         printf("RGA convert failed: %d, fallback to CPU\n", ret);
//         // 降级到原来的 CPU 方案
//         display_mat_to_framebuffer(fb, image);
//         return -1;
//     }
    
//     return 0;
// }

// 清屏（设为黑色）
void clear_framebuffer(const FramebufferInfo &fb) {
    memset(fb.data, 255, fb.screensize);
}

// 释放 Framebuffer 资源
void close_framebuffer(FramebufferInfo &fb) {
    if (fb.data) munmap(fb.data, fb.screensize);
    if (fb.fd != -1) close(fb.fd);
}

// 性能统计
struct PerfStats {
    long total_frames;
    double total_time_ms;
    double max_time_ms;
    double min_time_ms;
    clock_t last_time;
};

void init_perf_stats(PerfStats &stats) {
    stats.total_frames = 0;
    stats.total_time_ms = 0;
    stats.max_time_ms = 0;
    stats.min_time_ms = 99999;
    stats.last_time = clock();
}

// 更新性能统计
void update_perf_stats(PerfStats &stats) {
    clock_t current_time = clock();
    double elapsed_ms = (double)(current_time - stats.last_time) * 1000.0 / CLOCKS_PER_SEC;
    
    stats.total_frames++;
    stats.total_time_ms += elapsed_ms;
    
    if (elapsed_ms > stats.max_time_ms) stats.max_time_ms = elapsed_ms;
    if (elapsed_ms < stats.min_time_ms) stats.min_time_ms = elapsed_ms;
    
    stats.last_time = current_time;
    
    // 每秒打印一次性能统计
    if (stats.total_frames % 30 == 0) {
        double avg_ms = stats.total_time_ms / stats.total_frames;
        double fps = 1000.0 / avg_ms;
        printf("性能: %.1f FPS, 平均: %.2fms, 最小: %.2fms, 最大: %.2fms\n",
               fps, avg_ms, stats.min_time_ms, stats.max_time_ms);
    }
}




static volatile int running = 1;
// 信号处理，用于Ctrl+C退出
void signal_handler(int sig) {
    running = 0;
}

int main()
{
    signal(SIGINT, signal_handler);
    
    FramebufferInfo fb_info;
    if (init_framebuffer(fb_info) != 0) {
        return -1;
    }
    clear_framebuffer(fb_info);

    cv::VideoCapture cap;
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 240);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 135);

    // 尝试打开摄像头，0 通常是默认摄像头
    if (!cap.open(0)) {
        printf("错误：无法打开摄像头！\n");
        close_framebuffer(fb_info);
        return -1;
    }

    PerfStats perf_stats;
    init_perf_stats(perf_stats);

    cv::Mat bgr;
    while (running)
    {
        cap >> bgr;
        // cv::imshow("fb", bgr);

        // 更新性能统计
        // update_perf_stats(perf_stats);

        display_mat_to_framebuffer(fb_info, bgr);

        usleep(33000);
    }

    cap.release();
    clear_framebuffer(fb_info); // 退出前清屏
    close_framebuffer(fb_info);

    return 0;
}
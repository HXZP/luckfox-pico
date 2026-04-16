// fb_display.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/fb.h>
#include <stdint.h>
#include <time.h>

#define CAMERA_DEVICE "/dev/video11"
#define FB_DEVICE "/dev/fb0"
#define TARGET_WIDTH 240
#define TARGET_HEIGHT 135
#define SOURCE_WIDTH 640
#define SOURCE_HEIGHT 480

// YUV转RGB查找表（优化性能）
static int16_t rgb_yuv_table[256][4];
static int table_initialized = 0;

// 初始化YUV转RGB查找表
static void init_yuv_table(void) {
    if (table_initialized) return;
    
    for (int i = 0; i < 256; i++) {
        int y = i - 16;
        int u = i - 128;
        int v = i - 128;
        
        // 预计算Y分量
        rgb_yuv_table[i][0] = (298 * y + 128) >> 8;  // R分量系数
        rgb_yuv_table[i][1] = (298 * y) >> 8;        // G分量系数
        rgb_yuv_table[i][2] = (298 * y + 516) >> 8;  // B分量系数
        rgb_yuv_table[i][3] = 0;
    }
    table_initialized = 1;
}

// 快速NV12转RGB24
static void nv12_to_rgb24_fast(uint8_t *yuv, uint8_t *rgb, int width, int height) {
    uint8_t *y = yuv;
    uint8_t *uv = y + width * height;
    
    int y_index, uv_index;
    int r, g, b;
    
    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            y_index = j * width + i;
            uv_index = (j / 2) * width + (i & ~1);
            
            int y_val = y[y_index];
            int u_val = uv[uv_index];
            int v_val = uv[uv_index + 1];
            
            // 使用查找表加速计算
            int c = y_val - 16;
            int d = u_val - 128;
            int e = v_val - 128;
            
            r = (298 * c + 409 * e + 128) >> 8;
            g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            b = (298 * c + 516 * d + 128) >> 8;
            
            // 限制范围
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            
            int rgb_index = (j * width + i) * 3;
            rgb[rgb_index] = b;     // Blue
            rgb[rgb_index + 1] = g; // Green
            rgb[rgb_index + 2] = r; // Red
        }
    }
}

// 裁剪图像
static void crop_and_scale(uint8_t *src, uint8_t *dst, 
                           int src_w, int src_h, 
                           int dst_w, int dst_h) {
    // 计算裁剪区域（居中裁剪）
    int crop_x = (src_w - dst_w) / 2;
    int crop_y = (src_h - dst_h) / 2;
    
    // 简单裁剪，不做缩放
    for (int y = 0; y < dst_h; y++) {
        int src_y = crop_y + y;
        memcpy(dst + y * dst_w * 3,
               src + (src_y * src_w + crop_x) * 3,
               dst_w * 3);
    }
}

int main() {
    int cam_fd, fb_fd;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    enum v4l2_buf_type buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    uint8_t *framebuffer;
    uint8_t *yuv_buffer;
    uint8_t *rgb_buffer;
    uint8_t *cropped_buffer;
    size_t yuv_mmap_len;
    
    // 打开摄像头
    cam_fd = open(CAMERA_DEVICE, O_RDWR);
    if (cam_fd < 0) {
        perror("无法打开摄像头");
        return -1;
    }
    
    // 设置摄像头格式
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = buf_type;
    fmt.fmt.pix_mp.width = SOURCE_WIDTH;
    fmt.fmt.pix_mp.height = SOURCE_HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    
    if (ioctl(cam_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("设置摄像头格式失败");
        close(cam_fd);
        return -1;
    }
    
    // 申请缓冲区
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = buf_type;
    req.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(cam_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("申请缓冲区失败");
        close(cam_fd);
        return -1;
    }
    
    // 映射缓冲区
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.length = 1;
    buf.m.planes = planes;
    
    if (ioctl(cam_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        perror("查询缓冲区失败");
        close(cam_fd);
        return -1;
    }
    
    yuv_mmap_len = buf.m.planes[0].length;
    yuv_buffer = mmap(NULL, yuv_mmap_len, PROT_READ | PROT_WRITE,
                      MAP_SHARED, cam_fd, buf.m.planes[0].m.mem_offset);
    if (yuv_buffer == MAP_FAILED) {
        perror("映射缓冲区失败");
        close(cam_fd);
        return -1;
    }
    
    // 打开framebuffer
    fb_fd = open(FB_DEVICE, O_RDWR);
    if (fb_fd < 0) {
        perror("无法打开framebuffer");
        close(cam_fd);
        return -1;
    }
    
    // 获取framebuffer信息
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("获取屏幕信息失败");
        close(cam_fd);
        close(fb_fd);
        return -1;
    }
    
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("获取固定信息失败");
        close(cam_fd);
        close(fb_fd);
        return -1;
    }
    
    printf("Framebuffer: %dx%d, %dbpp\n", 
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
    
    // 映射framebuffer
    framebuffer = mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, fb_fd, 0);
    if (framebuffer == MAP_FAILED) {
        perror("映射framebuffer失败");
        close(cam_fd);
        close(fb_fd);
        return -1;
    }
    
    // 分配RGB缓冲区
    rgb_buffer = malloc(SOURCE_WIDTH * SOURCE_HEIGHT * 3);
    cropped_buffer = malloc(TARGET_WIDTH * TARGET_HEIGHT * 3);
    
    if (!rgb_buffer || !cropped_buffer) {
        perror("分配内存失败");
        close(cam_fd);
        close(fb_fd);
        return -1;
    }
    
    // 初始化查找表
    init_yuv_table();
    
    // 开始捕获
    buf.type = buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.length = 1;
    buf.m.planes = planes;

    if (ioctl(cam_fd, VIDIOC_QBUF, &buf) < 0) {
        perror("入队初始缓冲区失败");
        close(cam_fd);
        close(fb_fd);
        return -1;
    }
    
    if (ioctl(cam_fd, VIDIOC_STREAMON, &buf_type) < 0) {
        perror("开始捕获失败");
        close(cam_fd);
        close(fb_fd);
        return -1;
    }
    
    printf("开始显示摄像头画面...\n");
    printf("按Ctrl+C退出\n");
    
    struct timespec start, end;
    long frame_count = 0;
    
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        // 捕获一帧
        memset(planes, 0, sizeof(planes));
        buf.type = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = 0;
        buf.length = 1;
        buf.m.planes = planes;

        if (ioctl(cam_fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("出队缓冲区失败");
            break;
        }

        if (ioctl(cam_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("入队缓冲区失败");
            break;
        }

        // 转换NV12到RGB
        nv12_to_rgb24_fast(yuv_buffer, rgb_buffer,
                            SOURCE_WIDTH, SOURCE_HEIGHT);
        
        // 裁剪到目标尺寸
        crop_and_scale(rgb_buffer, cropped_buffer,
                      SOURCE_WIDTH, SOURCE_HEIGHT,
                      TARGET_WIDTH, TARGET_HEIGHT);
        
        // 写入framebuffer（居中显示）
        int fb_offset_x = (vinfo.xres - TARGET_WIDTH) / 2;
        int fb_offset_y = (vinfo.yres - TARGET_HEIGHT) / 2;
        
        for (int y = 0; y < TARGET_HEIGHT; y++) {
            int fb_y = fb_offset_y + y;
            if (fb_y >= vinfo.yres) break;
            
            for (int x = 0; x < TARGET_WIDTH; x++) {
                int fb_x = fb_offset_x + x;
                if (fb_x >= vinfo.xres) break;
                
                int fb_pos = (fb_y * vinfo.xres + fb_x) * (vinfo.bits_per_pixel / 8);
                int src_pos = (y * TARGET_WIDTH + x) * 3;
                
                // 根据bpp处理不同的像素格式
                if (vinfo.bits_per_pixel == 32) {
                    framebuffer[fb_pos] = cropped_buffer[src_pos];     // B
                    framebuffer[fb_pos + 1] = cropped_buffer[src_pos + 1]; // G
                    framebuffer[fb_pos + 2] = cropped_buffer[src_pos + 2]; // R
                    framebuffer[fb_pos + 3] = 0; // Alpha
                } else if (vinfo.bits_per_pixel == 24) {
                    framebuffer[fb_pos] = cropped_buffer[src_pos];     // B
                    framebuffer[fb_pos + 1] = cropped_buffer[src_pos + 1]; // G
                    framebuffer[fb_pos + 2] = cropped_buffer[src_pos + 2]; // R
                } else if (vinfo.bits_per_pixel == 16) {
                    // RGB565转换
                    uint16_t r = cropped_buffer[src_pos + 2] >> 3;
                    uint16_t g = cropped_buffer[src_pos + 1] >> 2;
                    uint16_t b = cropped_buffer[src_pos] >> 3;
                    uint16_t rgb565 = (r << 11) | (g << 5) | b;
                    *(uint16_t*)(framebuffer + fb_pos) = rgb565;
                }
            }
        }
        
        frame_count++;
        clock_gettime(CLOCK_MONOTONIC, &end);
        
        // 每30帧显示一次FPS
        if (frame_count % 30 == 0) {
            long elapsed = (end.tv_sec - start.tv_sec) * 1000000 + 
                          (end.tv_nsec - start.tv_nsec) / 1000;
            float fps = 1000000.0 / (elapsed / 30.0);
            printf("FPS: %.2f\n", fps);
        }
    }
    
    // 清理
    ioctl(cam_fd, VIDIOC_STREAMOFF, &buf_type);
    munmap(yuv_buffer, yuv_mmap_len);
    munmap(framebuffer, finfo.smem_len);
    free(rgb_buffer);
    free(cropped_buffer);
    close(cam_fd);
    close(fb_fd);
    
    return 0;
}
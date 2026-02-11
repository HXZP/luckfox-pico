// yuv2fb.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>

#define FB_DEVICE "/dev/fb0"
#define YUV_WIDTH 640
#define YUV_HEIGHT 480
#define TARGET_WIDTH 240
#define TARGET_HEIGHT 135

// 全局变量，用于信号处理
static int running = 1;

// 信号处理函数
void handle_signal(int sig) {
    running = 0;
    printf("\n收到信号 %d，准备退出...\n", sig);
}

// YUV420转RGB24
void yuv420_to_rgb24(uint8_t *yuv, uint8_t *rgb, int width, int height) {
    uint8_t *y = yuv;
    uint8_t *u = y + width * height;
    uint8_t *v = u + (width * height) / 4;
    
    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            int y_idx = j * width + i;
            int uv_idx = (j/2) * (width/2) + (i/2);
            
            int y_val = y[y_idx];
            int u_val = u[uv_idx];
            int v_val = v[uv_idx];
            
            // YUV to RGB转换公式
            int c = y_val - 16;
            int d = u_val - 128;
            int e = v_val - 128;
            
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;
            
            // 限幅
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            
            int rgb_idx = (j * width + i) * 3;
            rgb[rgb_idx] = b;     // Blue
            rgb[rgb_idx + 1] = g; // Green
            rgb[rgb_idx + 2] = r; // Red
        }
    }
}

// 裁剪图像
void crop_image(uint8_t *src, uint8_t *dst, 
                int src_w, int src_h, 
                int dst_w, int dst_h) {
    // 居中裁剪
    int crop_x = (src_w - dst_w) / 2;
    int crop_y = (src_h - dst_h) / 2;
    
    // 边界检查
    if (crop_x < 0) crop_x = 0;
    if (crop_y < 0) crop_y = 0;
    if (crop_x + dst_w > src_w) dst_w = src_w - crop_x;
    if (crop_y + dst_h > src_h) dst_h = src_h - crop_y;
    
    for (int y = 0; y < dst_h; y++) {
        int src_y = crop_y + y;
        memcpy(dst + y * dst_w * 3,
               src + (src_y * src_w + crop_x) * 3,
               dst_w * 3);
    }
}

// 显示到framebuffer
void display_on_fb(uint8_t *image, int img_w, int img_h,
                  struct fb_var_screeninfo *vinfo,
                  struct fb_fix_screeninfo *finfo,
                  uint8_t *fb_ptr) {
    // 计算居中显示位置
    int offset_x = (vinfo->xres - img_w) / 2;
    int offset_y = (vinfo->yres - img_h) / 2;
    
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;
    
    int bytes_per_pixel = vinfo->bits_per_pixel / 8;
    
    for (int y = 0; y < img_h && (y + offset_y) < vinfo->yres; y++) {
        for (int x = 0; x < img_w && (x + offset_x) < vinfo->xres; x++) {
            int img_idx = (y * img_w + x) * 3;
            int fb_idx = ((y + offset_y) * finfo->line_length) + 
                         ((x + offset_x) * bytes_per_pixel);
            
            if (bytes_per_pixel == 4) {
                // 32bpp: BGRA (或ARGB，取决于framebuffer)
                fb_ptr[fb_idx] = image[img_idx];        // B
                fb_ptr[fb_idx + 1] = image[img_idx + 1]; // G
                fb_ptr[fb_idx + 2] = image[img_idx + 2]; // R
                fb_ptr[fb_idx + 3] = 0;                 // Alpha
            } else if (bytes_per_pixel == 3) {
                // 24bpp: BGR
                fb_ptr[fb_idx] = image[img_idx];        // B
                fb_ptr[fb_idx + 1] = image[img_idx + 1]; // G
                fb_ptr[fb_idx + 2] = image[img_idx + 2]; // R
            } else if (bytes_per_pixel == 2) {
                // 16bpp: RGB565
                uint8_t r = image[img_idx + 2] >> 3;
                uint8_t g = image[img_idx + 1] >> 2;
                uint8_t b = image[img_idx] >> 3;
                uint16_t rgb565 = (r << 11) | (g << 5) | b;
                *((uint16_t*)(fb_ptr + fb_idx)) = rgb565;
            }
        }
    }
}

// 获取YUV文件大小（根据分辨率）
off_t get_yuv_file_size(const char *filename, int width, int height) {
    // YUV420格式：Y分量 = width * height，UV分量各占1/4
    return width * height * 3 / 2;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("使用方法: %s <yuv文件>\n", argv[0]);
        printf("示例: %s /root/video50.yuv\n", argv[0]);
        return 1;
    }
    
    const char *yuv_filename = argv[1];
    int yuv_fd, fb_fd;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    uint8_t *framebuffer = NULL;
    uint8_t *yuv_buffer = NULL;
    uint8_t *rgb_buffer = NULL;
    uint8_t *cropped_buffer = NULL;
    
    // 注册信号处理
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // 1. 打开YUV文件
    yuv_fd = open(yuv_filename, O_RDONLY);
    if (yuv_fd < 0) {
        perror("无法打开YUV文件");
        return -1;
    }
    printf("成功打开YUV文件: %s\n", yuv_filename);
    
    // 2. 打开framebuffer
    fb_fd = open(FB_DEVICE, O_RDWR);
    if (fb_fd < 0) {
        perror("无法打开framebuffer");
        close(yuv_fd);
        return -1;
    }
    
    // 3. 获取framebuffer信息
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("获取屏幕信息失败");
        close(yuv_fd);
        close(fb_fd);
        return -1;
    }
    
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("获取固定信息失败");
        close(yuv_fd);
        close(fb_fd);
        return -1;
    }
    
    printf("Framebuffer: %dx%d, %dbpp, 行字节数: %d\n", 
           vinfo.xres, vinfo.yres, 
           vinfo.bits_per_pixel,
           finfo.line_length);
    
    // 4. 映射framebuffer
    framebuffer = mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, fb_fd, 0);
    if (framebuffer == MAP_FAILED) {
        perror("映射framebuffer失败");
        close(yuv_fd);
        close(fb_fd);
        return -1;
    }
    
    // 5. 计算YUV文件大小并分配缓冲区
    off_t yuv_size = get_yuv_file_size(yuv_filename, YUV_WIDTH, YUV_HEIGHT);
    yuv_buffer = malloc(yuv_size);
    rgb_buffer = malloc(YUV_WIDTH * YUV_HEIGHT * 3);
    cropped_buffer = malloc(TARGET_WIDTH * TARGET_HEIGHT * 3);
    
    if (!yuv_buffer || !rgb_buffer || !cropped_buffer) {
        perror("分配内存失败");
        close(yuv_fd);
        close(fb_fd);
        return -1;
    }
    
    printf("YUV文件大小: %ld 字节\n", yuv_size);
    printf("按Ctrl+C退出播放\n");
    
    // 6. 读取并播放YUV文件
    ssize_t bytes_read = read(yuv_fd, yuv_buffer, yuv_size);
    if (bytes_read != yuv_size) {
        printf("警告: 读取字节数 (%ld) 不等于预期大小 (%ld)\n", 
               bytes_read, yuv_size);
    }
    
    // 7. 处理YUV数据
    printf("正在处理YUV数据...\n");
    
    // 转换为RGB
    yuv420_to_rgb24(yuv_buffer, rgb_buffer, YUV_WIDTH, YUV_HEIGHT);
    
    // 裁剪到目标尺寸
    crop_image(rgb_buffer, cropped_buffer, 
               YUV_WIDTH, YUV_HEIGHT,
               TARGET_WIDTH, TARGET_HEIGHT);
    
    // 显示到framebuffer
    display_on_fb(cropped_buffer, TARGET_WIDTH, TARGET_HEIGHT,
                  &vinfo, &finfo, framebuffer);
    
    printf("画面已显示到framebuffer\n");
    printf("按任意键退出...\n");
    
    // 8. 等待用户输入或信号
    while (running) {
        // 检查键盘输入（非阻塞）
        fd_set readfds;
        struct timeval tv;
        
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        
        if (ret > 0) {
            // 有输入，退出
            char c;
            read(STDIN_FILENO, &c, 1);
            break;
        }
        
        // 检查是否需要退出
        if (!running) break;
    }
    
    // 9. 清理
    printf("正在清理...\n");
    munmap(framebuffer, finfo.smem_len);
    free(yuv_buffer);
    free(rgb_buffer);
    free(cropped_buffer);
    close(yuv_fd);
    close(fb_fd);
    
    printf("程序退出\n");
    return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <string.h>

#define FB_DEVICE "/dev/fb0"
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 135

int main() {
    int fb_fd;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    unsigned char *fb_buf;
    
    printf("=== 纯Framebuffer测试程序 ===\n");
    
    // 1. 打开framebuffer
    fb_fd = open(FB_DEVICE, O_RDWR);
    if (fb_fd < 0) {
        perror("打开framebuffer失败");
        return 1;
    }
    
    // 2. 获取当前framebuffer信息
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("获取framebuffer信息失败");
        close(fb_fd);
        return 1;
    }
    
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);
    
    printf("当前Framebuffer: %dx%d, %dbpp, 行长度: %d\n",
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length);
    
    // 3. 尝试设置新的分辨率
    int orig_xres = vinfo.xres;
    int orig_yres = vinfo.yres;
    
    vinfo.xres = DISPLAY_WIDTH;
    vinfo.yres = DISPLAY_HEIGHT;
    vinfo.xres_virtual = DISPLAY_WIDTH;
    vinfo.yres_virtual = DISPLAY_HEIGHT;
    vinfo.bits_per_pixel = 32;
    
    if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vinfo) < 0) {
        printf("无法设置framebuffer分辨率，使用当前分辨率\n");
        // 恢复原始设置
        ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    }
    
    // 4. 映射framebuffer内存
    long screensize = vinfo.yres_virtual * finfo.line_length;
    fb_buf = (unsigned char*)mmap(0, screensize,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fb_fd, 0);
    
    if (fb_buf == MAP_FAILED) {
        perror("映射framebuffer失败");
        close(fb_fd);
        return 1;
    }
    
    printf("显示分辨率: %dx%d, 缓冲区大小: %ld字节\n",
           vinfo.xres, vinfo.yres, screensize);
    
    // 5. 显示测试图案
    printf("显示测试图案...按Ctrl+C停止\n");
    
    int frame = 0;
    while (1) {
        // 清屏
        memset(fb_buf, 0, screensize);
        
        // 画一个移动的彩色方块
        int box_size = 30;
        int x = (frame * 3) % (vinfo.xres - box_size);
        int y = (frame * 2) % (vinfo.yres - box_size);
        
        for (int j = y; j < y + box_size && j < vinfo.yres; j++) {
            for (int i = x; i < x + box_size && i < vinfo.xres; i++) {
                int pixel_index;
                if (vinfo.bits_per_pixel == 32) {
                    pixel_index = (j * finfo.line_length) + (i * 4);
                    
                    // 彩虹色
                    int r = (i * 255) / vinfo.xres;
                    int g = (j * 255) / vinfo.yres;
                    int b = (frame * 10) % 255;
                    
                    fb_buf[pixel_index] = b;       // B
                    fb_buf[pixel_index + 1] = g;   // G
                    fb_buf[pixel_index + 2] = r;   // R
                    fb_buf[pixel_index + 3] = 0;   // Alpha
                } else if (vinfo.bits_per_pixel == 24) {
                    pixel_index = (j * finfo.line_length) + (i * 3);
                    fb_buf[pixel_index] = 255;     // B
                    fb_buf[pixel_index + 1] = 0;   // G
                    fb_buf[pixel_index + 2] = 0;   // R
                } else if (vinfo.bits_per_pixel == 16) {
                    pixel_index = (j * finfo.line_length) + (i * 2);
                    // RGB565 - 红色
                    fb_buf[pixel_index] = 0x00;
                    fb_buf[pixel_index + 1] = 0xF8;
                }
            }
        }
        
        // 显示帧计数器
        if (vinfo.bits_per_pixel == 32) {
            for (int i = 0; i < 10 && i < vinfo.xres / 10; i++) {
                int x_pos = i * 8;
                int y_pos = 5;
                int pixel_index = (y_pos * finfo.line_length) + (x_pos * 4);
                
                if (pixel_index + 3 < screensize) {
                    fb_buf[pixel_index] = 255;     // B
                    fb_buf[pixel_index + 1] = 255; // G
                    fb_buf[pixel_index + 2] = 255; // R
                    fb_buf[pixel_index + 3] = 0;   // Alpha
                }
            }
        }
        
        frame++;
        usleep(100000); // 10 FPS
        
        if (frame % 10 == 0) {
            printf("已显示 %d 帧\n", frame);
        }
    }
    
    // 6. 清理
    munmap(fb_buf, screensize);
    
    // 恢复原始分辨率
    vinfo.xres = orig_xres;
    vinfo.yres = orig_yres;
    vinfo.xres_virtual = orig_xres;
    vinfo.yres_virtual = orig_yres;
    ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vinfo);
    
    close(fb_fd);
    return 0;
}
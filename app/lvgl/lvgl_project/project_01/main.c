#if 1
#include "lvgl/lvgl.h"
// #include "lv_demos/lv_demo.h"
#include "lv_drivers/display/fbdev.h"
// #include "lv_drivers/indev/evdev.h"
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

#define DISP_BUF_SIZE (240 * 135)

void fbdev_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p);

int main(void)
{
    /*LittlevGL init*/
    lv_init();

    /*Linux frame buffer device init*/
    fbdev_init();

    /*A small buffer for LittlevGL to draw the screen's content*/
    static lv_color_t buf[DISP_BUF_SIZE];

    /*Initialize a descriptor for the buffer*/
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, DISP_BUF_SIZE);

    /*Initialize and register a display driver*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf   = &disp_buf;
    disp_drv.flush_cb   = fbdev_flush;
    disp_drv.hor_res    = 240;
    disp_drv.ver_res    = 135;
    lv_disp_drv_register(&disp_drv);

    lv_obj_t *scr = lv_disp_get_scr_act(NULL); // 获取当前活跃的屏幕对象
    // 创建一个基础对象（用作黑色正方形）
    lv_obj_t *square = lv_obj_create(scr);
    // 设置正方形大小（宽度和高度相同）
    lv_obj_set_size(square, 100, 100); // 100x100 像素
    // 移除默认的边框和阴影（可选）
    lv_obj_set_style_border_width(square, 0, LV_PART_MAIN); // 边框宽度设为 0
    lv_obj_set_style_shadow_width(square, 0, LV_PART_MAIN); // 阴影宽度设为 0
    // 设置背景颜色为纯黑
    lv_obj_set_style_bg_color(square, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(square, LV_OPA_50, LV_PART_MAIN); // 不透明度 100%
    // 居中显示
    lv_obj_center(square);    

//    lv_demo_widgets();

    /*Handle LitlevGL tasks (tickless mode)*/
    while(1) {
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}

/*Set in lv_conf.h as `LV_TICK_CUSTOM_SYS_TIME_EXPR`*/
uint32_t custom_tick_get(void)
{
    static uint64_t start_ms = 0;
    if(start_ms == 0) {
        struct timeval tv_start;
        gettimeofday(&tv_start, NULL);
        start_ms = (tv_start.tv_sec * 1000000 + tv_start.tv_usec) / 1000;
    }

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t now_ms;
    now_ms = (tv_now.tv_sec * 1000000 + tv_now.tv_usec) / 1000;

    uint32_t time_ms = now_ms - start_ms;
    return time_ms;
}
#endif

// #include <stdio.h>

// int main(void)
// {
//     printf("hello");
//     return 0;
// }
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

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static pthread_mutex_t g_mjpeg_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::vector<uchar> g_mjpeg_frame;

// 将模型输入坐标映射回原始显示图像坐标。
void mapCoordinates(cv::Mat input, cv::Mat output, int *x, int *y)
{
    float scaleX = (float)output.cols / (float)input.cols; 
    float scaleY = (float)output.rows / (float)input.rows;
    
    *x = (int)((float)*x / scaleX);
    *y = (int)((float)*y / scaleY);
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
        "Server: luckfox-yolov5-save\r\n"
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
    if (argc != 2)
    {
        printf("%s <yolov5 model_path>\n", argv[0]);
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
    system("RkLunch-stop.sh");
    const char *model_path = argv[1];

    clock_t start_time;
    clock_t end_time;
    char text[128];
    float fps = 0;
    int save_index = 0;
    int save_enable = 0;
    std::set<int> saved_class_ids;

    //Model Input (Yolov5)
    int model_width    = 640;
    int model_height   = 640;
    int channels = 3;

    int ret;
    rknn_app_context_t rknn_app_ctx;
    object_detect_result_list od_results;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));
    memset(&od_results, 0, sizeof(object_detect_result_list));

    init_yolov5_model(model_path, &rknn_app_ctx);
    init_post_process();
    if (ensure_save_dir(SAVE_RESULT_DIR) == 0)
    {
        save_enable = 1;
    }
    start_mjpeg_server();

    //Init fb
    int disp_flag = 0;
    int pixel_size = 0;
    size_t screensize = 0;
    int disp_width = 0;
    int disp_height = 0;
    void* framebuffer = NULL; 
    struct fb_fix_screeninfo fb_fix;
    struct fb_var_screeninfo fb_var;

    int framebuffer_fd = 0; //for DMA
    cv::Mat disp;

    int fb = open("/dev/fb0", O_RDWR); 
    if(fb == -1)
        printf("Screen OFF!\n");
    else 
        disp_flag = 1;

    if(disp_flag){
        ioctl(fb, FBIOGET_VSCREENINFO, &fb_var);
        ioctl(fb, FBIOGET_FSCREENINFO, &fb_fix);

        disp_width = fb_var.xres;
        disp_height = fb_var.yres;
        pixel_size = fb_var.bits_per_pixel / 8;
        printf("Screen width = %d, Screen height = %d, Pixel_size = %d\n",disp_width, disp_height, pixel_size);

        screensize = disp_width * disp_height * pixel_size;
        framebuffer = (uint8_t*)mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);

        if( pixel_size == 4 )//ARGB8888
            disp = cv::Mat(disp_height, disp_width, CV_8UC3);
        else if ( pixel_size == 2 ) //RGB565
            disp = cv::Mat(disp_height, disp_width, CV_16UC1); 

#if USE_DMA
        dma_buf_alloc(RV1106_CMA_HEAP_PATH,
                      disp_width * disp_height * pixel_size,  
                      &framebuffer_fd, 
                      (void **) & (disp.data)); 
#endif
    }
    else{
        disp_height = 240;
        disp_width = 240;
    }


    //Init Opencv-mobile 
    cv::VideoCapture cap;
    cv::Mat bgr(disp_height, disp_width, CV_8UC3); 
    cv::Mat bgr_model_input(model_height, model_width, CV_8UC3, rknn_app_ctx.input_mems[0]->virt_addr);
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  disp_width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, disp_height);
    cap.open(0); 

    while(1)
    {
        std::vector<int> new_class_ids;

        start_time = clock();
        cap >> bgr;

        // 对比版恢复为每帧提交一次 NPU，便于和最初成功状态做现象对照。
        cv::resize(bgr, bgr_model_input, cv::Size(model_width,model_height), 0, 0, cv::INTER_LINEAR);
        inference_yolov5_model(&rknn_app_ctx, &od_results);

        // Add rectangle and probability
        for (int i = 0; i < od_results.count; i++)
        {
            object_detect_result *det_result = &(od_results.results[i]); 
            mapCoordinates(bgr, bgr_model_input, &det_result->box.left,  &det_result->box.top);
            mapCoordinates(bgr, bgr_model_input, &det_result->box.right, &det_result->box.bottom);	

            printf("%s @ (%d %d %d %d) %.3f\n", coco_cls_to_name(det_result->cls_id),
                   det_result->box.left, det_result->box.top,
                   det_result->box.right, det_result->box.bottom,
                   det_result->prop);

            cv::rectangle(bgr,cv::Point(det_result->box.left ,det_result->box.top),
                              cv::Point(det_result->box.right,det_result->box.bottom),cv::Scalar(0,255,0),3);

            sprintf(text, "%s %.1f%%", coco_cls_to_name(det_result->cls_id), det_result->prop * 100);
            cv::putText(bgr,text,cv::Point(det_result->box.left, det_result->box.top - 8),
                                         cv::FONT_HERSHEY_SIMPLEX,0.5,
                                         cv::Scalar(0,255,0),2); 

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

        sprintf(text,"fps=%.1f",fps); 
        cv::putText(bgr,text,cv::Point(0, 20),
                    cv::FONT_HERSHEY_SIMPLEX,0.5,
                    cv::Scalar(0,255,0),1);
        update_mjpeg_frame(bgr);

        if(disp_flag){
            //LCD Show
            if( pixel_size == 4 )
                cv::cvtColor(bgr, disp, cv::COLOR_BGR2BGRA);
            else if( pixel_size == 2 )
                cv::cvtColor(bgr, disp, cv::COLOR_BGR2BGR565);
            memcpy(framebuffer, disp.data, disp_width * disp_height * pixel_size);
#if USE_DMA
            dma_sync_cpu_to_device(framebuffer_fd);
#endif
        }
        //Update Fps
        end_time = clock();
        clock_t elapsed_time = end_time - start_time;
        if (elapsed_time > 0)
        {
            fps = (float)CLOCKS_PER_SEC / (float)elapsed_time;
        }
        //printf("%s\n",text);
        memset(text,0,sizeof(text)); 
    }
    deinit_post_process();

    if(disp_flag){
        close(fb);
        munmap(framebuffer, screensize);
#if USE_DMA
        dma_buf_free(disp_width*disp_height*pixel_size,
                     &framebuffer_fd, 
                     bgr.data);
#endif
    }

    ret = release_yolov5_model(&rknn_app_ctx);
    if (ret != 0)
    {
        printf("release_yolov5_model fail! ret=%d\n", ret);
    }

    return 0;
}

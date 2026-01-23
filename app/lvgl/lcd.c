#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>

#define SPI_DEVICE_PATH "/dev/spidev0.0"

#define LCD_A0  "51"
#define LCD_RES "52"
#define LCD_LED "53"
#define LCD_CS  "48"

#define LCD_ADRR_WSTART 0x28
#define LCD_ADRR_WEND   0x117
#define LCD_ADRR_HSTART 0x35
#define LCD_ADRR_HEND   0xBB

#define LCD_Width      240
#define LCD_HEIGHT     135
#define LCD_AREA       LCD_Width*LCD_HEIGHT //32400

#define LCD_DATA_TX_TIMES     4                            //传输次数
#define LCD_DATA_LEN          LCD_AREA*2                     //数据长度 64800  0.0144  69.44hz
#define LCD_DATA_BUFF_LEN     LCD_DATA_LEN/LCD_DATA_TX_TIMES //一次传输数量 648  

uint8_t tx_buffer[64800] = {0};
uint8_t rx_buffer[8] = {0};

void gpio_init(const char* gpio) {
    FILE *fp;
    char path[64];

    // 导出 GPIO
    fp = fopen("/sys/class/gpio/export", "w");
    if (!fp) {
        perror("Export failed");
        return;
    }
    fprintf(fp, "%s", gpio);
    fclose(fp);

    // 等待 sysfs 生成文件（避免竞态条件）
    usleep(100000);  // 延迟 100ms[6](@ref)

    // 设置方向为输出
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/direction", gpio);
    fp = fopen(path, "w");
    if (!fp) {
        perror("Direction set failed");
        return;
    }
    fprintf(fp, "out");
    fclose(fp);

    // 设置初始电平为高
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/value", gpio);
    fp = fopen(path, "w");
    if (!fp) {
        perror("Value set failed");
        return;
    }
    fprintf(fp, "1");
    fclose(fp);
}

void gpio_write(const char* gpio, int value) {
    FILE *fp;
    char path[64];

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/value", gpio);
    fp = fopen(path, "w");
    if (!fp) {
        perror("Value set failed");
        return;
    }
    fprintf(fp, "%d", value);
    fclose(fp);
}

int hxzp_st7789_Write(uint8_t *data, uint16_t len)
{
    int spi_file;
    // Open the SPI device
    if ((spi_file = open(SPI_DEVICE_PATH, O_RDWR)) < 0) {
        perror("Failed to open SPI device");
        return -1;
    }

    // Configure SPI mode and bits per word
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    if (ioctl(spi_file, SPI_IOC_WR_MODE, &mode) < 0) {
        perror("Failed to set SPI mode");
        close(spi_file);
        return -1;
    }
    if (ioctl(spi_file, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        perror("Failed to set SPI bits per word");
        close(spi_file);
        return -1;
    }
        
    struct spi_ioc_transfer transfer = {
        .tx_buf = (unsigned long)data,
        .rx_buf = (unsigned long)rx_buffer,
        .len = len,
        .delay_usecs = 0,
        .speed_hz = 20000000,  // SPI speed in Hz
        .bits_per_word = 8,
    };

    ioctl(spi_file, SPI_IOC_MESSAGE(1), &transfer);

    close(spi_file);

    return 0;
}
static void hxzp_st7789_Commad(uint8_t data)
{
    // LCD.setCS(ST7798_GPIOL);
    // LCD.setA0(ST7798_GPIOL);
    gpio_write(LCD_CS,0);
    gpio_write(LCD_A0,0);
    hxzp_st7789_Write(&data,1);
    // LCD.setCS(ST7798_GPIOH);
    gpio_write(LCD_CS,1);
}

static void hxzp_st7789_Data(uint8_t data)
{
    gpio_write(LCD_CS,0);
    gpio_write(LCD_A0,1);
    // LCD.setCS(ST7798_GPIOL);
    // LCD.setA0(ST7798_GPIOH);
    hxzp_st7789_Write(&data,1);
    // LCD.setCS(ST7798_GPIOH);
    gpio_write(LCD_CS,1);
}
void hxzp_st7789_SetWindow(uint16_t xstart, uint16_t ystart, uint16_t xend, uint16_t yend)
{
    uint16_t temp,res[2];

    /*设置横坐标*/
    hxzp_st7789_Commad(0x2a);

    temp = LCD_ADRR_WSTART + xstart;
    res[0] = temp >> 8;
    res[1] = temp & 0xFF;
    hxzp_st7789_Data(res[0]);
    hxzp_st7789_Data(res[1]);
    temp = LCD_ADRR_WSTART + xend;
    res[0] = temp >> 8;
    res[1] = temp & 0xFF;    	
    hxzp_st7789_Data(res[0]);
    hxzp_st7789_Data(res[1]);
    
    /*设置纵坐标*/
    hxzp_st7789_Commad(0x2b);

    temp = LCD_ADRR_HSTART + ystart;
    res[0] = temp >> 8;
    res[1] = temp & 0xFF;
    hxzp_st7789_Data(res[0]);
    hxzp_st7789_Data(res[1]);
    temp = LCD_ADRR_HSTART + yend;
    res[0] = temp >> 8;
    res[1] = temp & 0xFF;    	
    hxzp_st7789_Data(res[0]);
    hxzp_st7789_Data(res[1]);

    /*准备写入数据*/
    hxzp_st7789_Commad(0x2c);
}
void hxzp_st7789_SetBackground(uint16_t color)
{
    hxzp_st7789_SetWindow(0,0,LCD_Width-1,LCD_HEIGHT-1);

    // LCD.setCS(ST7798_GPIOL);
    // LCD.setA0(ST7798_GPIOH);
    gpio_write(LCD_CS,0);
    gpio_write(LCD_A0,1);

    for (uint16_t i = 0; i < LCD_DATA_BUFF_LEN/2; i++)
    {
        tx_buffer[i * 2]      = (uint8_t)(color>>8);
        tx_buffer[i * 2 + 1]  = (uint8_t)(color&0xFF); 
    }

    for (uint16_t i = 0; i < LCD_DATA_TX_TIMES; i++)
    {
        hxzp_st7789_Write(tx_buffer, LCD_DATA_BUFF_LEN);
    }    

//    LCD.setCS(ST7798_GPIOH);
    gpio_write(LCD_CS,1);
}

int main() {

    int color = 0;
    // gpio_init("34");
    // gpio_write("34",1);

    gpio_init(LCD_A0);
    gpio_init(LCD_CS);
    gpio_init(LCD_LED);
    // gpio_init("LCD_RES");
    // gpio_init("LCD_LED");
    gpio_write(LCD_LED,1);

/*
在Linux中使用SPI子系统时，通过ioctl调用和spi_ioc_transfer结构体操作SPI设备文件（如/dev/spidevX.Y）时，不会向该文件写入任何内容。
SPI设备文件（如/dev/spidevX.Y）是内核驱动的接口，而非普通文件。它不存储数据，而是通过系统调用（如ioctl）与硬件交互的桥梁。
ioctl(SPI_IOC_MESSAGE(1))的作用：

    该操作通过SPI总线触发一次全双工传输。发送数据到从设备（通过tx_buffer），同时接收从设备返回的数据到rx_buffer。
    数据直接传递到硬件，不会写入文件系统。

*/

    // Perform SPI transfer
    // struct spi_ioc_transfer transfer = {
    //     .tx_buf = (unsigned long)tx_buffer,
    //     .rx_buf = (unsigned long)rx_buffer,
    //     .len = sizeof(tx_buffer),
    //     .delay_usecs = 0,
    //     .speed_hz = 20000000,  // SPI speed in Hz
    //     .bits_per_word = 8,
    // };

    // ioctl(spi_file, SPI_IOC_MESSAGE(1), &transfer);


    while (1)
    {
        // if (ioctl(spi_file, SPI_IOC_MESSAGE(1), &transfer) < 0) {
        //     perror("Failed to perform SPI transfer");
        //     close(spi_file);
        //     return -1;
        // }


        /* Print tx_buffer and rx_buffer*/
        // printf("\rtx_buffer: \n %s\n ", tx_buffer);
        // printf("\rrx_buffer: \n %s\n ", rx_buffer);        
        color++;
        hxzp_st7789_SetBackground(color);
        /* code */
        sleep(1000);
    }
    


    // Close the SPI device

    return 0;
}
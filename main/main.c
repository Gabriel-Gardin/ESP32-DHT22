#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "driver/rmt.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"

void main_task(void *pvParameters);
static void nec_rx_init(void);
void pulse_line(gpio_num_t gpio);
void set_read(gpio_num_t gpio);
int *get_bits(void);

typedef struct
{
    float temperature;
    float humity;
} dht_data;

dht_data get_temp_humity();

int check_sum(int *bits);

RingbufHandle_t rb = NULL;

#define RMT_RX_CHANNEL    0     /*!< RMT channel for receiver */
#define RMT_RX_GPIO_NUM  19     /*!< GPIO number for receiver */
#define RMT_CLK_DIV      80    /*!< RMT counter clock divider */
#define RMT_TICK_10_US    (80000000/RMT_CLK_DIV/100000)   /*!< RMT counter value for 10 us.(Source clock is APB clock) */
#define rmt_item32_tIMEOUT_US  200   /*!< RMT receiver timeout value(us) */

float temperature;
float humity;

void app_main(void)
{
    xTaskCreatePinnedToCore(main_task, "main_task", 2048, NULL, 0, NULL, 1);
}

void main_task(void * pvParameters)
{
    for(;;)
    {
        dht_data data;
        data = get_temp_humity();
        printf("Temperature is %f\n", data.temperature);
        printf("Humity is %f\n", data.humity);
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

dht_data get_temp_humity()
{
    dht_data data;

    int *bits;

    int count = 0;

    unsigned char first_byte_temp = 0;
    unsigned char second_byte_temp = 0;
    unsigned char first_byte_hum = 0;
    unsigned char second_byte_hum = 0;

    int high_temp = 0;
    int high_hum = 0;

    int checksum = 0;

    bits = get_bits();
    int check = check_sum(bits);
    //printf("checksum = %i\n", check);
    for(int i = 0; i <= 8; i++)
    {
        first_byte_hum |= bits[i + 2] <<(7 - count);
        count ++;
    }
    count = 0;

    for(int i = 8; i <= 15; i++)
    {
        second_byte_hum |= bits[i + 2] << (7 - count);
        count ++;
    }
    count = 0;

    for(int i = 16; i <= 23; i++)
    {
        first_byte_temp |= bits[i+2] << (7 - count);  //Os dois primeiros bits são referentes aos dois primeiros pulsos de resposta. 
        //printf("%i\n", bits[i]);
        count ++;
    }
    count = 0;

    for(int i = 24; i <= 31; i++)
    {
        second_byte_temp |= bits[i+2] << (7 - count);  //Os dois primeiros bits são referentes aos dois primeiros pulsos de resposta. 
        //printf("%i\n", bits[i]);
        count ++;
    }
    count = 0;

    checksum = (int)((first_byte_hum + second_byte_hum + first_byte_temp + second_byte_temp) & 0XFF);

    high_hum = first_byte_hum << 8;
    high_temp = first_byte_temp << 8;

    if(checksum != check)
    {
        printf("checksum error!!! %i, %i\n", check, checksum);
        data.temperature = 0;
        data.humity = 0;
        return data;
    }
    
    else
    {
       // printf("first byte %i\n", high_temp);
       // printf("second byte %i\n", (int)second_byte_temp);
        data.temperature = (((float)(high_temp + second_byte_temp)) / 10);
        data.humity = (((float)high_hum + second_byte_hum) / 10);
        return data;
    }
}


int check_sum(int *bits)
{
    int check_value = 0;
    int count = 0;
    for(int i = 32; i <= 39; i++)
    {
        check_value |= bits[i + 2] << (7 - count); //The index is always implement by a factor of two in order to ignore the first two pulses, that are the response signal from the DHT. Real data come after...
        count++;
    }
    return check_value;
}


int *get_bits(void)
{
    rmt_item32_t *items = NULL;
    uint32_t length = 4;
    bool rmt_installed = 0;
    static int bits[42] = {0};
    //memset(bits, 0, sizeof(bits));

    if (!rmt_installed)
    {
        nec_rx_init();
        rmt_get_ringbuf_handle(RMT_RX_CHANNEL, &rb);
        rmt_rx_start(RMT_RX_CHANNEL, true);
        pulse_line(RMT_RX_GPIO_NUM);
        rmt_installed = 1;
    }
    else
    {
        pulse_line(RMT_RX_GPIO_NUM);
    }
    if(rb)
    {
        items = (rmt_item32_t *) xRingbufferReceive(rb, &length, 1000);
        if(items)
        {
            int i = 0;
            for(int i = 0; i < 42; i++){
                length /= 4; // one RMT = 4 Bytes
                if(items->level0 == 1)
                {
                    printf("Level_0 is %i and duration is %i\n", items->level0,items->duration0);
                    //printf("%i; %i uS\n", items->level0,items->duration0);
                    if(items->duration0 < 30 && items->duration0 > 20)
                    {
                        bits[i] = 0;
                    //    printf("%i\n", bits[i]);
                    }
                    else if(items->duration0 > 70 && items->duration0 < 80)
                    {
                        bits[i] = 1;
                    //    printf("%i\n", bits[i]);
                    }
                }
                else if(items->level1 == 1)
                {
                    printf("Level_1 is %i and duration is %i\n", items->level1,items->duration1);
                    if(items->duration1 < 30 && items->duration1 > 20)
                    {
                        bits[i] = 0;
                        //printf("%i\n", bits[i]);
                    }
                    else if(items->duration1 > 70 && items->duration1 < 80)
                    {
                        bits[i] = 1;
                        //printf("%i\n", bits[i]);
                    }
                }
            //    ets_delay_us(50);
                items++;
            }
            //vRingbufferReturnItem(rb, (void*) items);
        }
        rmt_rx_stop(RMT_RX_CHANNEL);
        rmt_driver_uninstall(RMT_RX_CHANNEL);
        rmt_installed = 0;
    }
    return &bits[0];
}


void pulse_line(gpio_num_t gpio)
{
    //PIN_FUNC_SELECT(RMT_RX_GPIO_NUM, PIN_FUNC_GPIO);
    gpio_matrix_out(gpio, SIG_GPIO_OUT_IDX, 0, 0);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    //GPIO.enable_w1tc = (0x1 << gpio);
    ets_delay_us(900);
    rmt_set_pin(RMT_RX_CHANNEL, RMT_MODE_RX, gpio);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[gpio]); 
    //GPIO.pin[gpio].pad_driver = 1;   
}

/*
 * @brief RMT receiver initialization
 */
static void nec_rx_init(void)
{
    rmt_config_t rmt_rx;
    rmt_rx.channel = RMT_RX_CHANNEL;
    rmt_rx.gpio_num = RMT_RX_GPIO_NUM;
    rmt_rx.clk_div = RMT_CLK_DIV;
    rmt_rx.mem_block_num = 1;
    rmt_rx.rmt_mode = RMT_MODE_RX;
    rmt_rx.rx_config.filter_en = 1;
    rmt_rx.rx_config.filter_ticks_thresh = 200;
    rmt_rx.rx_config.idle_threshold = 1000;
    rmt_config(&rmt_rx);
    rmt_driver_install(rmt_rx.channel, 1000, 0);
}
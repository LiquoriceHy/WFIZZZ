#include "driver/gpio.h"
#include "led_strip.h"

#include "app_config.h"
#include "led_ctrl.h"

static led_strip_handle_t s_strip;
static volatile int s_on;

void led_ctrl_init(void)
{
    /* 外接 LED（GPIO3，上拉点亮）：led 命令的实际受控对象 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << EXT_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(EXT_LED_GPIO, 0);

    /* 板载 WS2812：同状态镜像亮绿，作为板内可视反馈 */
    led_strip_config_t strip = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &s_strip));
    led_strip_clear(s_strip);
}

void led_ctrl_set(int on)
{
    gpio_set_level(EXT_LED_GPIO, on ? 1 : 0);

    if (on)
    {
        led_strip_set_pixel(s_strip, 0, 0, 96, 0);   /* 绿色，亮度别拉满 */
        led_strip_refresh(s_strip);
    }
    else
    {
        led_strip_clear(s_strip);
    }
    s_on = on ? 1 : 0;
}

int led_ctrl_is_on(void)
{
    return s_on;
}

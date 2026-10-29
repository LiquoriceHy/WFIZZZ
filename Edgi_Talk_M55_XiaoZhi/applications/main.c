#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <board.h>

#define LED_PIN_G GET_PIN(16, 6)
#define LCD_BL_GPIO_NUM GET_PIN(15, 7)
#define BL_PWM_DISP_CTRL GET_PIN(20, 6)

#ifndef BSP_LCD_STARTUP_STABILIZE_MS
#define BSP_LCD_STARTUP_STABILIZE_MS 1500U
#endif

#ifndef BSP_LCD_FIRST_FRAME_DELAY_MS
#define BSP_LCD_FIRST_FRAME_DELAY_MS 300U
#endif

#ifndef BSP_LCD_ROTATION_DEGREES
#define BSP_LCD_ROTATION_DEGREES 0
#endif

extern int lvgl_thread_init(void);

static void m55_lvgl_cpu_cache_enable(void)
{
#if defined(BSP_LVGL_ENABLE_CPU_CACHE) && defined(RT_USING_CACHE)
#if defined(__ICACHE_PRESENT) && (__ICACHE_PRESENT == 1U)
    if (!rt_hw_cpu_icache_status())
    {
        rt_hw_cpu_icache_enable();
    }
#endif

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if (!rt_hw_cpu_dcache_status())
    {
        rt_hw_cpu_dcache_enable();
    }
#endif

    rt_kprintf("M55 cache enabled: I=%d D=%d\n",
               rt_hw_cpu_icache_status(),
               rt_hw_cpu_dcache_status());
#endif
}

static void m55_lcd_backlight_enable(void)
{
    rt_pin_mode(LCD_BL_GPIO_NUM, PIN_MODE_OUTPUT);
    rt_pin_mode(BL_PWM_DISP_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(LCD_BL_GPIO_NUM, PIN_HIGH);
    rt_pin_write(BL_PWM_DISP_CTRL, PIN_HIGH);
}

int main(void)
{
    uint32_t last_led_ms = rt_tick_get_millisecond();
    rt_bool_t led_on = RT_FALSE;

    rt_kprintf("Hello RT-Thread\n");
    rt_kprintf("It's cortex-m55\n");
    rt_kprintf("LVGL button test, lcd rotation=%d\n", BSP_LCD_ROTATION_DEGREES);

    rt_pin_mode(LED_PIN_G, PIN_MODE_OUTPUT);
    m55_lvgl_cpu_cache_enable();
    rt_thread_mdelay(BSP_LCD_STARTUP_STABILIZE_MS);
    lvgl_thread_init();
    rt_thread_mdelay(BSP_LCD_FIRST_FRAME_DELAY_MS);
    m55_lcd_backlight_enable();

    while (1)
    {
        uint32_t now = rt_tick_get_millisecond();

        if ((now - last_led_ms) >= 500U)
        {
            led_on = !led_on;
            rt_pin_write(LED_PIN_G, led_on ? PIN_HIGH : PIN_LOW);
            last_led_ms = now;
        }

        rt_thread_mdelay(50);
    }

    return 0;
}

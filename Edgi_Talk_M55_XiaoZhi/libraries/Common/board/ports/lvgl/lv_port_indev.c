/*******************************************************************************
* File Name        : lv_port_indev.c
*
* Description      : This file provides implementation of low level input device
*                    driver for LVGL.
*
* Related Document : See README.md
*
******************************************************************************/

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "lv_port_indev.h"
#include "lv_port_disp.h"
#include "cy_utils.h"
#include "drv_touch.h"
#include "cybsp.h"

/*******************************************************************************
* Global Variables
*******************************************************************************/
lv_indev_t *indev_touchpad;

#define TOUCH_PHYS_HOR_RES ((int32_t)BSP_LCD_PHYSICAL_HOR_RES)
#define TOUCH_PHYS_VER_RES ((int32_t)BSP_LCD_PHYSICAL_VER_RES)

/* ============================ 去抖参数 ============================
 *
 * 2026-10-26 加。原因：原实现"一帧读到触摸就报按下"，而 LVGL 的
 * indev 采样周期 LV_DEF_REFR_PERIOD = 18ms。一次轻擦（约 30-60ms）
 * 只要在某一帧恰好被采样到，就会走完 PRESSED → RELEASED → CLICKED
 * 全序列，于是任何挂在 LV_EVENT_CLICKED 上的入口（底部抽屉热区、
 * WiFi 行、闹钟整屏遮罩…）都会被误触发。实测 drv_touch.c:277-282
 * 更糟：坐标与上一帧不同就无条件上报 touch_down，连 IRQ 都不看，
 * 所以抬起瞬间的坐标漂移也会变成一次"按下"。
 *
 * 为什么用"时间窗 + 累计帧数"而不是"连续 N 帧"：
 *   驱动在手指**按住不动**时会走 drv_touch.c:265 的分支——坐标没变
 *   就去看 IRQ，而 READ_STATUS 已被清零、IRQ 已释放，于是报
 *   touch_up，即按住期间会间歇性丢帧。若去抖严格要求"连续 N 帧
 *   都有触摸"，一次正常点击可能永远凑不满 N 帧，反而点不动。
 *   所以这里改成：
 *     - 按下确认：自首次检测到触摸起，累计有效帧数达标即确认，
 *       中间的丢帧不清零计数，只受时间窗约束；
 *     - 松开确认：连续多帧无触摸才算松开（这一侧必须严格，
 *       否则按住期间一次丢帧就会误判松手 → 触发 CLICKED）。
 *
 * 参数取 2 帧 × 18ms ≈ 36ms 的确认延迟，明显短于人的最小有意点击
 * （约 60-100ms），手感不受影响，但足以滤掉单帧毛刺。
 */
#ifndef TOUCH_DOWN_CONFIRM
#define TOUCH_DOWN_CONFIRM  2   /* 按下确认需要累计几次"有触摸" */
#endif
#ifndef TOUCH_UP_CONFIRM
#define TOUCH_UP_CONFIRM    2   /* 松开确认需要连续几次"无触摸" */
#endif
#ifndef TOUCH_DOWN_WINDOW_MS
#define TOUCH_DOWN_WINDOW_MS 120   /* 按下确认的时间窗，超时重新开始计 */
#endif

/* 采样状态机：把"单帧瞬时值"变成"去抖后的稳定状态" */
typedef struct
{
    uint8_t   down_cnt;     /* 时间窗内累计的有效帧数 */
    uint8_t   up_cnt;       /* 连续无效帧数 */
    uint8_t   pressed;      /* 去抖后的稳定状态（1=按住） */
    uint8_t   saw_valid;    /* 是否已经收到过有效坐标 */
    rt_tick_t down_tick;    /* 本次按下确认窗口的起点 */
    lv_point_t point;       /* 最后一次有效坐标 */
} touch_filter_t;

static touch_filter_t s_filter;

static int32_t touch_clamp(int32_t value, int32_t min, int32_t max)
{
    if (value < min)
    {
        return min;
    }
    if (value > max)
    {
        return max;
    }
    return value;
}

/* 原始坐标是否落在合理范围内。
 *
 * 这个检查只能用于**明显离谱**的值（负数、远大于物理分辨率），不能用
 * 它做精确的越界判定 —— 因为 ST7102 上报的原始坐标范围是芯片固件自己
 * 定的（drv_touch.c:82-105 读到的 range_x/range_y），而本工程**没有任何
 * 消费者使用那个范围做缩放**。也就是说我们并不知道芯片确切的坐标上限，
 * 若按 480/800 硬判，一旦芯片实际上报 0..1023 之类的值，就会把所有触摸
 * 都判为无效，屏幕直接失灵。
 *
 * 所以这里留 2 倍余量：只挡掉"明显不可能"的采样（负数、超过物理尺寸
 * 两倍的荒谬值），既能滤掉 I2C 残留字节造成的巨值噪声，又不会因为
 * 对芯片量程的误判而废掉触摸。真正的误触发治理靠下面的时间窗去抖。
 */
#define TOUCH_RAW_SLACK   2

static int touch_raw_in_range(rt_int16_t raw_x, rt_int16_t raw_y)
{
    if (raw_x < 0 || raw_y < 0)
        return 0;
    if ((int32_t)raw_x >= TOUCH_PHYS_HOR_RES * TOUCH_RAW_SLACK ||
        (int32_t)raw_y >= TOUCH_PHYS_VER_RES * TOUCH_RAW_SLACK)
        return 0;
    return 1;
}

static void touchpad_transform_point(rt_int16_t raw_x, rt_int16_t raw_y, lv_point_t *point)
{
    int32_t x;
    int32_t y;

#if defined(BSP_LCD_ROTATION_DEGREES) && (BSP_LCD_ROTATION_DEGREES == 90)
    x = raw_y;
    y = (TOUCH_PHYS_HOR_RES - 1) - raw_x;
#elif defined(BSP_LCD_ROTATION_DEGREES) && (BSP_LCD_ROTATION_DEGREES == 180)
    x = (TOUCH_PHYS_HOR_RES - 1) - raw_x;
    y = (TOUCH_PHYS_VER_RES - 1) - raw_y;
#elif defined(BSP_LCD_ROTATION_DEGREES) && (BSP_LCD_ROTATION_DEGREES == 270)
    x = (TOUCH_PHYS_VER_RES - 1) - raw_y;
    y = raw_x;
#else
    x = raw_x;
    y = raw_y;
#endif

    /* 调用前已用 touch_raw_in_range() 挡掉越界，这里的钳位只防 ±1 像素
     * 的变换溢出（比如 raw_x=0 时 y 会算成 PHYS_HOR-1，仍合法）。 */
    point->x = touch_clamp(x, 0, MY_DISP_HOR_RES - 1);
    point->y = touch_clamp(y, 0, MY_DISP_VER_RES - 1);
}

/*******************************************************************************
* Function Name: touchpad_init
********************************************************************************
* Summary:
*  Initialization function for touchpad supported by LittelvGL.
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
static void touchpad_init(void)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    if (rt_hw_ST7102_port() != result)
    {
        CY_ASSERT(0);
    }
}


/*******************************************************************************
* Function Name: touchpad_read
********************************************************************************
* Summary:
*  Touchpad read function called by the LVGL library.
*  Here you will find example implementation of input devices supported by
*  LittelvGL:
*   - Touchpad
*   - Mouse (with cursor support)
*   - Keypad (supports GUI usage only with key)
*   - Encoder (supports GUI usage only with: left, right, push)
*   - Button (external buttons to press points on the screen)
*
*   The `..._read()` function are only examples.
*   You should shape them according to your hardware.
*
*
* Parameters:
*  *indev_drv: Pointer to the input driver structure to be registered by HAL.
*  *data: Pointer to the data buffer holding touch coordinates.
*
* Return:
*  void
*
*******************************************************************************/
static void touchpad_read(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
    rt_int16_t touch_x = 0;
    rt_int16_t touch_y = 0;
    cy_rslt_t result;
    int valid = 0;

    (void)indev_drv;

    /* ---- 1. 采样一次，判断这一帧是否拿到"可信的触摸" ---- */
    result = ST7102_get_single_touch(&touch_x, &touch_y);
    if (CY_RSLT_SUCCESS == result && touch_raw_in_range(touch_x, touch_y))
    {
        valid = 1;
    }

    /* ---- 2. 状态机：按下用时间窗累计确认，松开用连续帧确认 ---- */
    if (valid)
    {
        /* 更新坐标只在采样有效时做，这样松开确认期间 point 保持
         * 在最后位置，不会跳回原点。 */
        touchpad_transform_point(touch_x, touch_y, &s_filter.point);
        s_filter.saw_valid = 1;

        /* 有触摸就打断"松开计时" */
        s_filter.up_cnt = 0;

        if (!s_filter.pressed)
        {
            rt_tick_t now = rt_tick_get();

            /* 首次检测到触摸、或距上次累计已超时 → 重开一个确认窗口。
             * 为什么不因为中途丢帧就清零：按住不动时驱动会间歇性报
             * up（见文件顶部注释），清零会让正常点击永远凑不满。 */
            if (s_filter.down_cnt == 0 ||
                (now - s_filter.down_tick) > rt_tick_from_millisecond(TOUCH_DOWN_WINDOW_MS))
            {
                s_filter.down_tick = now;
                s_filter.down_cnt = 1;
            }
            else if (s_filter.down_cnt < 0xFF)
            {
                s_filter.down_cnt++;
            }

            if (s_filter.down_cnt >= TOUCH_DOWN_CONFIRM)
            {
                s_filter.pressed = 1;
                s_filter.down_cnt = 0;   /* 归零，为下一次按下准备 */
            }
        }
    }
    else
    {
        /* 松开必须严格连续：按住期间单帧丢帧若被当成松手，LVGL 会
         * 立刻发 RELEASED + CLICKED，等于"按一下变两次点击"。 */
        if (s_filter.up_cnt < 0xFF) s_filter.up_cnt++;

        if (s_filter.pressed && s_filter.up_cnt >= TOUCH_UP_CONFIRM)
        {
            s_filter.pressed = 0;
            s_filter.down_cnt = 0;   /* 完全松开，清掉按下计数 */
        }
        else if (!s_filter.pressed && s_filter.up_cnt >= TOUCH_UP_CONFIRM)
        {
            /* 未按下状态下持续无触摸：把未完成的按下计数作废 */
            s_filter.down_cnt = 0;
        }
    }

    /* ---- 3. 输出 ---- */
    /* 首帧保护：还没收到过任何有效坐标前，不希望把 (0,0) 当作按点
     * 上报 —— 那正好命中屏幕左上角。此时一律报松开。 */
    if (!s_filter.saw_valid)
    {
        data->state = LV_INDEV_STATE_REL;
        data->point.x = 0;
        data->point.y = 0;
        return;
    }

    data->point = s_filter.point;
    data->state = s_filter.pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}


/*******************************************************************************
* Function Name: lv_port_indev_init
********************************************************************************
* Summary:
*  Initialization function for input devices supported by LittelvGL.
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void lv_port_indev_init(void)
{
    /* Initialize your touchpad if you have. */
    touchpad_init();

    /* Register a touchpad input device */
    indev_touchpad = lv_indev_create();
    lv_indev_set_type(indev_touchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_touchpad, touchpad_read);
}


/* [] END OF FILE */

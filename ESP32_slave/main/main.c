/*
 * ESP32-C6 LAN 设备端 —— 对接 WFIZZZ M55「LAN Devices」应用
 *
 * 模块拆分（同目录单文件模块，全部可调配置集中在 app_config.h）：
 *   wifi_sta    WiFi STA 接入与断线重连
 *   mqtt_link   MQTT 链路 + LWT + 30s 心跳
 *   dev_cmd     命令分发（ping/led/reboot）
 *   led_ctrl    外接 LED（GPIO3）+ 板载 WS2812 镜像
 *   status_ui   ST7789 状态屏（LVGL）
 *
 * 协议（与 M55 侧 applications/lanctl_cfg.h 的头注释一致）：
 *   订阅 wfizzz/dev/esp32c6/cmd    (QoS1)   M55 按钮下发的命令 JSON
 *   发布 wfizzz/dev/esp32c6/reply  (QoS1)   执行结果，原文显示在 M55 界面
 *   发布 wfizzz/dev/esp32c6/status (retain) 心跳/在线；LWT 遗嘱报 offline
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "status_ui.h"
#include "wifi_sta.h"
#include "led_ctrl.h"
#include "mqtt_link.h"

static const char *TAG = "lan_dev";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 屏先点亮：后面的联网过程直接看得见。
     * lvgl 任务优先级/栈照 Learn 工程的验证配置（prio 2 / 8K）：
     * 高于 main 但低于 WiFi(23)/mqtt(5)，即便它异常也不至于饿死系统任务 */
    status_ui_start();

    /* 顺序不可换：wifi_sta_start 完成后屏刷新才允许调 WiFi API（见
     * wifi_sta.h）；LED 初始化须赶在 MQTT 之前，避免 cmd 早到时未就绪 */
    wifi_sta_start();
    led_ctrl_init();
    mqtt_link_start();      /* WiFi 未连上时客户端内部排队等待 */

    ESP_LOGI(TAG, "lan device %s started", DEV_ID);
}

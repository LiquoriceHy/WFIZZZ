/* LAN Devices 命令集（与 M55 侧 lanctl_cfg.h 的按钮一一对应）：
 *   {"cmd":"ping"}            → 回 pong + uptime
 *   {"cmd":"led","state":1|0} → GPIO3 外接 LED 亮/灭，WS2812 镜像
 *   {"cmd":"reboot"}          → 回 ok 后重启
 *   其他                      → {"ok":false,"msg":"unknown cmd"} */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#include "app_config.h"
#include "mqtt_link.h"
#include "led_ctrl.h"
#include "status_ui.h"
#include "dev_cmd.h"

static const char *TAG = "dev_cmd";

void dev_cmd_handle(const char *data, int len)
{
    char buf[128];
    char reply[200];
    cJSON *root, *item;
    const char *cmd = "";
    int state = 0;
    bool ok = false;
    char msg[64] = "";

    /* MQTT_EVENT_DATA 的 data 不带结束符，先拷出来 */
    if (len <= 0 || len >= (int)sizeof(buf))
    {
        ESP_LOGW(TAG, "cmd too long (%d bytes), ignored", len);
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';
    ESP_LOGI(TAG, "cmd <- %s", buf);
    status_ui_set_cmd(buf);

    root = cJSON_Parse(buf);
    if (root == NULL)
    {
        snprintf(reply, sizeof(reply),
                 "{\"ok\":false,\"cmd\":\"?\",\"msg\":\"bad json\"}");
        mqtt_link_publish_reply(reply);
        status_ui_set_reply(reply);
        return;
    }

    item = cJSON_GetObjectItem(root, "cmd");
    if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL)
        cmd = item->valuestring;

    if (strcmp(cmd, "ping") == 0)
    {
        ok = true;
        snprintf(msg, sizeof(msg), "pong");
    }
    else if (strcmp(cmd, "led") == 0)
    {
        item = cJSON_GetObjectItem(root, "state");
        state = (item != NULL && cJSON_IsNumber(item)) ? item->valueint : 0;
        led_ctrl_set(state);
        ok = true;
        snprintf(msg, sizeof(msg), "led %s", state ? "on" : "off");
    }
    else if (strcmp(cmd, "reboot") == 0)
    {
        ok = true;
        snprintf(msg, sizeof(msg), "rebooting");
    }
    else
    {
        snprintf(msg, sizeof(msg), "unknown cmd");
    }

    snprintf(reply, sizeof(reply),
             "{\"ok\":%s,\"cmd\":\"%s\",\"msg\":\"%s\""
             ",\"state\":%d,\"uptime\":%llu}",
             ok ? "true" : "false", cmd, msg, state,
             (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    mqtt_link_publish_reply(reply);
    status_ui_set_reply(reply);

    /* 重启放最后：先让 QoS1 reply 发出去 */
    if (ok && strcmp(cmd, "reboot") == 0)
    {
        ESP_LOGW(TAG, "rebooting in 500ms");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

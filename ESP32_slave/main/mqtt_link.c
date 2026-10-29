/* esp-mqtt 客户端：LWT 遗嘱报 offline（retain），连上先发一条
 * retained status 让 M55 立刻看到在线，之后由心跳任务周期发布。 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include "app_config.h"
#include "wifi_sta.h"
#include "dev_cmd.h"
#include "mqtt_link.h"

static const char *TAG = "mqtt_link";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_up;

void mqtt_link_publish_status(void)
{
    char json[160];
    char ip_str[16];

    wifi_sta_get_ip(ip_str, sizeof(ip_str));
    snprintf(json, sizeof(json),
             "{\"online\":true,\"ip\":\"%s\",\"rssi\":%d,"
             "\"uptime\":%llu}",
             ip_str, wifi_sta_get_rssi(),
             (unsigned long long)(esp_timer_get_time() / 1000000ULL));

    if (s_up)
        esp_mqtt_client_publish(s_client, TOPIC_STATUS, json, 0, 1, 1);
    ESP_LOGI(TAG, "status: %s", json);
}

void mqtt_link_publish_reply(const char *json)
{
    if (s_up)
        esp_mqtt_client_publish(s_client, TOPIC_REPLY, json, 0, 1, 0);
    else
        ESP_LOGW(TAG, "mqtt down, reply dropped: %s", json);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    esp_mqtt_event_handle_t ev = event_data;

    switch ((esp_mqtt_event_id_t)id)
    {
    case MQTT_EVENT_CONNECTED:
        s_up = true;
        esp_mqtt_client_subscribe(s_client, TOPIC_CMD, 1);
        mqtt_link_publish_status();     /* retained，M55 立刻能看到在线 */
        ESP_LOGI(TAG, "mqtt connected, subscribed %s", TOPIC_CMD);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_up = false;                   /* esp-mqtt 内部自动重连 */
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len == strlen(TOPIC_CMD) &&
            strncmp(ev->topic, TOPIC_CMD, ev->topic_len) == 0 &&
            ev->data_len == ev->total_data_len)      /* 本协议命令都很短 */
        {
            dev_cmd_handle(ev->data, ev->data_len);
        }
        break;
    default:
        break;
    }
}

/* 心跳：协议要求 30-60s 一次在线状态 */
static void heartbeat_task(void *arg)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
        if (s_up)
            mqtt_link_publish_status();
    }
}

void mqtt_link_start(void)
{
    if (MQTT_URI[0] == '\0')
    {
        ESP_LOGW(TAG, "MQTT_URI not configured, mqtt link disabled");
        return;
    }

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
        .credentials.client_id = "dev_" DEV_ID,
        .session.keepalive = 30,
        .session.last_will = {
            .topic = TOPIC_STATUS,
            .msg = "{\"online\":false}",
            .qos = 1,
            .retain = true,             /* 掉线时 broker 代发离线状态 */
        },
    };
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);    /* WiFi 没连上它内部会排队等待 */

    xTaskCreate(heartbeat_task, "hb", 4096, NULL, 5, NULL);
}

bool mqtt_link_is_up(void)
{
    return s_up;
}

/* WiFi STA 接入（SSID/密码见 app_config.h）
 * 断线在事件回调里直接 esp_wifi_connect() 重连，无重试上限。 */
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "app_config.h"
#include "wifi_sta.h"

static const char *TAG = "wifi_sta";

static volatile bool s_up;          /* 已拿到 IP */
static volatile bool s_api_ready;   /* esp_wifi_start 完成前禁调 WiFi API */
static volatile int  s_reason;      /* 最近一次断连原因码 */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *disc = event_data;

            s_up = false;
            s_reason = disc->reason;   /* 屏上显示，便于定位 */
            ESP_LOGW(TAG, "wifi disconnected, reason=%d", disc->reason);
            esp_wifi_connect();          /* 掉线自动重连 */
            break;
        }
        default:
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        s_up = true;
    }
}

void wifi_sta_start(void)
{
    if (WIFI_SSID[0] == '\0')
    {
        ESP_LOGW(TAG, "WIFI_SSID not configured, wifi disabled");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi = { 0 };
    strlcpy((char *)wifi.sta.ssid, WIFI_SSID, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, WIFI_PASS, sizeof(wifi.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));

    /* STA 限 11b/g/n：与 M55 同路由的已验证路径，MQTT 设备带宽足够。
     * 此前怀疑的 11ax 兼容性实为密码错导致的 reason=15，已排除；
     * 如需 ax 可删掉本调用恢复默认 */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                          WIFI_PROTOCOL_11N));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_api_ready = true;        /* 从此屏刷新任务可以安全调 WiFi API */
}

bool wifi_sta_is_up(void)
{
    return s_up;
}

int wifi_sta_last_reason(void)
{
    return s_reason;
}

bool wifi_sta_api_ready(void)
{
    return s_api_ready;
}

void wifi_sta_get_ip(char *buf, int len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;

    buf[0] = '-';
    buf[1] = '\0';
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK)
        esp_ip4addr_ntoa(&ip.ip, buf, len);
}

int wifi_sta_get_rssi(void)
{
    wifi_ap_record_t ap;

    if (!s_api_ready || esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
        return 0;
    return ap.rssi;
}

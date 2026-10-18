#include "time_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "time_manager";
static bool s_time_synced = false;

static void sntp_sync_cb(struct timeval *tv) {
    (void)tv;
    s_time_synced = true;
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    ESP_LOGI(TAG, "SNTP 同步成功: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_time_synced) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "已获取 IP，启动 SNTP 对时");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "cn.pool.ntp.org");
        sntp_set_time_sync_notification_cb(sntp_sync_cb);
        esp_sntp_init();
    }
}

static void time_sync_task(void *arg) {
    (void)arg;
    // 延迟 500ms，等待系统初始化平稳
    vTaskDelay(pdMS_TO_TICKS(500));

    // 先初始化 NVS
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);

    // 设置 Wi-Fi 凭证 (Jarvis)
    wifi_config_t wifi_cfg = {0};
    strcpy((char *)wifi_cfg.sta.ssid, "Jarvis");
    strcpy((char *)wifi_cfg.sta.password, "meiyoumima~");

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    // 维持一段时间，一旦同步成功等待几秒即可
    for (int i = 0; i < 30; i++) {
        if (s_time_synced) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

void time_manager_init(void) {
    // 设置时区为东八区北京时间
    setenv("TZ", "CST-8", 1);
    tzset();

    xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 3, NULL);
}

bool time_manager_is_synced(void) {
    return s_time_synced;
}

void time_manager_get_time_str(char *buf, size_t max_len) {
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    if (ti.tm_year >= (2020 - 1900)) {
        snprintf(buf, max_len, "%02d:%02d", ti.tm_hour, ti.tm_min);
    } else {
        // 未完成网络对时时，显示开机时间分秒
        snprintf(buf, max_len, "%02d:%02d", ti.tm_min, ti.tm_sec);
    }
}

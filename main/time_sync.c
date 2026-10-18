#include "time_sync.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "time_sync";
static bool s_synced = false;

static void sntp_cb(struct timeval *tv) {
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "SNTP 同步成功！");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    (void)arg; (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi 断开，5秒后重试");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi 已连上并获得 IP，启动 SNTP 对时");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "cn.pool.ntp.org");
        sntp_set_time_sync_notification_cb(sntp_cb);
        esp_sntp_init();
    }
}

void time_sync_init(void) {
    // 设置东八区中国标准时间
    setenv("TZ", "CST-8", 1);
    tzset();

    // 从 NVS 读取用户配置的 STA SSID / 密码
    nvs_handle_t handle;
    char ssid[33] = {0};
    char password[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(password);

    // 兼容可能存放的位置
    bool found = false;
    if (nvs_open("nvs.net80211", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_str(handle, "sta.ssid", ssid, &ssid_len) == ESP_OK) {
            nvs_get_str(handle, "sta.pswd", password, &pass_len);
            found = true;
        }
        nvs_close(handle);
    }
    if (!found) {
        // 默认备选配置（刚才从串口日志探测到的 Jarvis）
        strncpy(ssid, "Jarvis", sizeof(ssid) - 1);
        strncpy(password, "meiyoumima~", sizeof(password) - 1);
        found = true;
    }

    if (found && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "准备连接 Wi-Fi: %s 进行后台时钟同步", ssid);
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_start();
    }
}

bool time_sync_is_synced(void) {
    return s_synced;
}

void time_sync_get_time_str(char *buf, size_t max_len) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(buf, max_len, "%H:%M", &timeinfo);
}

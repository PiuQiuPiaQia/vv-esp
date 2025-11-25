/**
 * WiFi 管理模块实现
 */

#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";

// WiFi 事件组
static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// WiFi 配置
static wifi_manager_config_t s_wifi_config = {0};
static esp_netif_t *s_netif = NULL;
static int s_retry_num = 0;
static bool s_is_connected = false;

/**
 * WiFi 事件处理器
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "WiFi 开始连接...");
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_is_connected = false;

    if (s_wifi_config.callback) {
      s_wifi_config.callback(false);
    }

    if (s_wifi_config.max_retry == 0 ||
        s_retry_num < s_wifi_config.max_retry) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "重试连接 WiFi... (%d/%d)", s_retry_num,
               s_wifi_config.max_retry);
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      ESP_LOGE(TAG, "WiFi 连接失败");
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "获得 IP 地址: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    s_is_connected = true;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    
    if (s_wifi_config.callback) {
      s_wifi_config.callback(true);
    }
  }
}

/**
 * 初始化 WiFi
 */
esp_err_t wifi_manager_init(const wifi_manager_config_t *config) {
  if (config == NULL || config->ssid == NULL || config->password == NULL) {
    ESP_LOGE(TAG, "无效的 WiFi 配置");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "初始化 WiFi...");

  // 保存配置
  s_wifi_config = *config;

  // 初始化 NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 创建事件组
  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == NULL) {
    ESP_LOGE(TAG, "创建事件组失败");
    return ESP_FAIL;
  }

  // 初始化网络接口
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_netif = esp_netif_create_default_wifi_sta();

  // 初始化 WiFi
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // 注册事件处理器
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

  // 配置 WiFi
  wifi_config_t wifi_config = {
      .sta =
          {
              .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
              .pmf_cfg = {.capable = true, .required = false},
          },
  };

  strncpy((char *)wifi_config.sta.ssid, config->ssid,
          sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, config->password,
          sizeof(wifi_config.sta.password) - 1);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "WiFi 初始化完成,正在连接 SSID: %s", config->ssid);

  // 等待连接结果
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE, pdFALSE, portMAX_DELAY);

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "✓ WiFi 连接成功");
    return ESP_OK;
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "✗ WiFi 连接失败");
    return ESP_FAIL;
  } else {
    ESP_LOGE(TAG, "✗ WiFi 连接超时");
    return ESP_ERR_TIMEOUT;
  }
}

/**
 * 断开 WiFi
 */
esp_err_t wifi_manager_disconnect(void) {
  ESP_LOGI(TAG, "断开 WiFi 连接");
  s_is_connected = false;
  return esp_wifi_disconnect();
}

/**
 * 检查连接状态
 */
bool wifi_manager_is_connected(void) { return s_is_connected; }

/**
 * 获取 IP 地址字符串
 */
esp_err_t wifi_manager_get_ip_str(char *ip_str, size_t len) {
  if (ip_str == NULL || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!s_is_connected || s_netif == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_netif_ip_info_t ip_info;
  esp_err_t ret = esp_netif_get_ip_info(s_netif, &ip_info);
  if (ret != ESP_OK) {
    return ret;
  }

  snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
  return ESP_OK;
}
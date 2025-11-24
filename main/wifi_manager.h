/**
 * WiFi 管理模块
 * 简化的 WiFi 连接管理
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WiFi 连接状态回调
 * @param connected true 表示已连接，false 表示断开
 */
typedef void (*wifi_status_callback_t)(bool connected);

/**
 * WiFi 配置结构
 */
typedef struct {
    const char *ssid;              // WiFi SSID
    const char *password;          // WiFi 密码
    wifi_status_callback_t callback; // 状态回调 (可选)
    uint32_t max_retry;            // 最大重试次数 (0 表示无限重试)
} wifi_manager_config_t;

/**
 * 初始化并连接 WiFi
 * @param config WiFi 配置
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t wifi_manager_init(const wifi_manager_config_t *config);

/**
 * 断开 WiFi 连接
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * 检查 WiFi 是否已连接
 * @return true 已连接，false 未连接
 */
bool wifi_manager_is_connected(void);

/**
 * 获取 IP 地址字符串
 * @param ip_str 输出缓冲区
 * @param len 缓冲区长度
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t wifi_manager_get_ip_str(char *ip_str, size_t len);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
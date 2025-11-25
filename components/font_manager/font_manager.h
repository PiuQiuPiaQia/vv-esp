/*
 * Font Manager for LVGL
 * Provides dynamic font selection for Chinese and English text
 */

#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include "lvgl.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize font manager
 * @return ESP_OK on success
 */
esp_err_t font_manager_init(void);

/**
 * @brief Get appropriate font for text content
 * @param text Text content to analyze
 * @param size Font size (14, 16, 20, 24, etc.)
 * @return Pointer to appropriate font
 */
const lv_font_t* font_manager_get_font(const char* text, int size);

/**
 * @brief Check if text contains Chinese characters
 * @param text Text to check
 * @return true if contains Chinese, false otherwise
 */
bool font_manager_has_chinese(const char* text);

/**
 * @brief Get default Chinese font for given size
 * @param size Font size
 * @return Pointer to Chinese font
 */
const lv_font_t* font_manager_get_chinese_font(int size);

/**
 * @brief Get default English font for given size
 * @param size Font size
 * @return Pointer to English font
 */
const lv_font_t* font_manager_get_english_font(int size);

#ifdef __cplusplus
}
#endif

#endif // FONT_MANAGER_H

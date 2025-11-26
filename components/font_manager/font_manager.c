/*
 * Font Manager Implementation
 * Dynamically selects appropriate fonts based on text content
 */

#include "font_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "font_manager";

// Declare xiaozhi-fonts puhui fonts (阿里巴巴普惠体) - 完整字符集
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

esp_err_t font_manager_init(void) {
    ESP_LOGI(TAG, "Font manager initialized");
    return ESP_OK;
}

bool font_manager_has_chinese(const char* text) {
    if (text == NULL) {
        return false;
    }

    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        // Check for UTF-8 Chinese character range
        // Chinese characters typically start with 0xE4-0xE9 in UTF-8
        if (*p >= 0xE4 && *p <= 0xE9) {
            // Verify it's a valid 3-byte UTF-8 sequence
            if (p[1] >= 0x80 && p[1] <= 0xBF &&
                p[2] >= 0x80 && p[2] <= 0xBF) {
                return true;
            }
        }
        // Skip to next character
        if (*p < 0x80) {
            p++; // ASCII
        } else if ((*p & 0xE0) == 0xC0) {
            p += 2; // 2-byte UTF-8
        } else if ((*p & 0xF0) == 0xE0) {
            p += 3; // 3-byte UTF-8 (Chinese)
        } else if ((*p & 0xF8) == 0xF0) {
            p += 4; // 4-byte UTF-8
        } else {
            p++; // Invalid, skip
        }
    }
    return false;
}

const lv_font_t* font_manager_get_chinese_font(int size) {
    // Return appropriate Chinese font based on size
    // Use Alibaba PuHui font (阿里巴巴普惠体) for Chinese text - 完整字符集

    switch (size) {
        case 14:
            return &font_puhui_14_1;
        case 16:
            return &font_puhui_16_4;
        case 18:
        case 20:
            return &font_puhui_20_4;
        case 24:
        case 30:
            return &font_puhui_30_4;
        default:
            // Fall back to appropriate font for other sizes
            if (size <= 14) {
                return &font_puhui_14_1;
            } else if (size <= 20) {
                return &font_puhui_16_4;
            } else {
                return &font_puhui_30_4;
            }
    }
}

const lv_font_t* font_manager_get_english_font(int size) {
    // Return appropriate English font based on size
    switch (size) {
        case 10:
#if defined(CONFIG_LV_FONT_MONTSERRAT_10)
            return &lv_font_montserrat_10;
#else
            return LV_FONT_DEFAULT;
#endif
        case 12:
#if defined(CONFIG_LV_FONT_MONTSERRAT_12)
            return &lv_font_montserrat_12;
#else
            return LV_FONT_DEFAULT;
#endif
        case 14:
#if defined(CONFIG_LV_FONT_MONTSERRAT_14)
            return &lv_font_montserrat_14;
#else
            return LV_FONT_DEFAULT;
#endif
        case 16:
#if defined(CONFIG_LV_FONT_MONTSERRAT_16)
            return &lv_font_montserrat_16;
#else
            return LV_FONT_DEFAULT;
#endif
        case 18:
#if defined(CONFIG_LV_FONT_MONTSERRAT_18)
            return &lv_font_montserrat_18;
#else
            return LV_FONT_DEFAULT;
#endif
        case 20:
#if defined(CONFIG_LV_FONT_MONTSERRAT_20)
            return &lv_font_montserrat_20;
#else
            return LV_FONT_DEFAULT;
#endif
        case 24:
#if defined(CONFIG_LV_FONT_MONTSERRAT_24)
            return &lv_font_montserrat_24;
#else
            return LV_FONT_DEFAULT;
#endif
        default:
            return LV_FONT_DEFAULT;
    }
}

const lv_font_t* font_manager_get_font(const char* text, int size) {
    if (text == NULL) {
        return font_manager_get_english_font(size);
    }

    // Check if text contains Chinese characters
    if (font_manager_has_chinese(text)) {
        return font_manager_get_chinese_font(size);
    } else {
        return font_manager_get_english_font(size);
    }
}

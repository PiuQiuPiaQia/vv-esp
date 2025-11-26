/*
 * LVGL Hello World 示例
 * 在 ST7789 显示屏上显示 Hello World
 * 适配立创实战派 ESP32-S3 开发板
 * 基于 xiaozhi-esp32 项目的正确实现
 */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "baidu_agent_client.h"
#include "wifi_manager.h"
#include "font_manager.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "MARIO_AI";

// I2C 配置 - 用于 PCA9557 IO 扩展芯片
#define I2C_MASTER_NUM I2C_NUM_1
#define I2C_MASTER_SDA_IO 1
#define I2C_MASTER_SCL_IO 2
#define PCA9557_ADDR 0x19

// ST7789 显示屏配置 - 立创实战派 ESP32-S3
#define LCD_HOST SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)

#define PIN_NUM_MOSI 40     // SPI MOSI
#define PIN_NUM_CLK 41      // SPI CLK
#define PIN_NUM_CS -1       // SPI CS (未使用)
#define PIN_NUM_DC 39       // DC (Data/Command)
#define PIN_NUM_RST -1      // Reset (未使用)
#define PIN_NUM_BK_LIGHT 42 // 背光

// ST7789 屏幕参数
#define LCD_H_RES 320
#define LCD_V_RES 240

// 显示参数
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY true
#define DISPLAY_INVERT_COLOR true

static lv_display_t *lvgl_disp = NULL;
static esp_lcd_panel_io_handle_t panel_io = NULL;
static esp_lcd_panel_handle_t panel = NULL;
static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t pca9557_dev = NULL;

// 百度智能体客户端
static baidu_agent_handle_t agent_handle = NULL;
static lv_obj_t *title_label = NULL;        // 顶部标题
static lv_obj_t *user_input_label = NULL;   // 用户输入（右对齐）
static lv_obj_t *response_label = NULL;     // AI 响应（左对齐）
static lv_obj_t *status_label = NULL;       // 底部状态（右下角）

// 响应文本累积缓冲区
#define RESPONSE_BUFFER_SIZE 2048
static char response_buffer[RESPONSE_BUFFER_SIZE] = {0};
static size_t response_buffer_len = 0;

// 当前用户输入
static char current_user_input[256] = {0};

// PCA9557 寄存器地址
#define PCA9557_REG_INPUT 0x00
#define PCA9557_REG_OUTPUT 0x01
#define PCA9557_REG_POLARITY 0x02
#define PCA9557_REG_CONFIG 0x03

// PCA9557 写寄存器
static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t data) {
  uint8_t write_buf[2] = {reg, data};
  return i2c_master_transmit(pca9557_dev, write_buf, 2, -1);
}

// PCA9557 读寄存器
static esp_err_t pca9557_read_reg(uint8_t reg, uint8_t *data) {
  return i2c_master_transmit_receive(pca9557_dev, &reg, 1, data, 1, -1);
}

// PCA9557 设置输出引脚状态
static esp_err_t pca9557_set_output(uint8_t bit, uint8_t level) {
  uint8_t data;
  esp_err_t ret = pca9557_read_reg(PCA9557_REG_OUTPUT, &data);
  if (ret != ESP_OK) {
    return ret;
  }
  data = (data & ~(1 << bit)) | (level << bit);
  return pca9557_write_reg(PCA9557_REG_OUTPUT, data);
}

// 初始化 I2C 和 PCA9557
static void init_i2c_and_pca9557(void) {
  ESP_LOGI(TAG, "初始化 I2C 总线...");
  i2c_master_bus_config_t i2c_bus_cfg = {
      .i2c_port = I2C_MASTER_NUM,
      .sda_io_num = I2C_MASTER_SDA_IO,
      .scl_io_num = I2C_MASTER_SCL_IO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));
  ESP_LOGI(TAG, "✓ I2C 总线初始化完成");

  ESP_LOGI(TAG, "初始化 PCA9557 IO 扩展芯片...");
  i2c_device_config_t pca9557_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = PCA9557_ADDR,
      .scl_speed_hz = 100000,
  };
  ESP_ERROR_CHECK(
      i2c_master_bus_add_device(i2c_bus, &pca9557_cfg, &pca9557_dev));

  // 配置 PCA9557: 设置输出方向和初始状态
  ESP_ERROR_CHECK(pca9557_write_reg(PCA9557_REG_OUTPUT, 0x03));
  ESP_ERROR_CHECK(pca9557_write_reg(PCA9557_REG_CONFIG, 0xf8));
  ESP_LOGI(TAG, "✓ PCA9557 初始化完成");
}

// 初始化背光
static void init_backlight(void) {
  ESP_LOGI(TAG, "初始化背光...");
  gpio_config_t bk_gpio_config = {.mode = GPIO_MODE_OUTPUT,
                                  .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT};
  ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
  // 立创实战派背光控制反向，低电平点亮
  gpio_set_level(PIN_NUM_BK_LIGHT, 0);
  ESP_LOGI(TAG, "✓ 背光初始化完成");
}

// 初始化 SPI 总线
static void init_spi_bus(void) {
  ESP_LOGI(TAG, "初始化 SPI 总线...");
  spi_bus_config_t buscfg = {
      .mosi_io_num = PIN_NUM_MOSI,
      .miso_io_num = GPIO_NUM_NC,
      .sclk_io_num = PIN_NUM_CLK,
      .quadwp_io_num = GPIO_NUM_NC,
      .quadhd_io_num = GPIO_NUM_NC,
      .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
  };
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
  ESP_LOGI(TAG, "✓ SPI 总线初始化完成");
}

// 初始化 LCD 显示屏
static void init_lcd_panel(void) {
  ESP_LOGI(TAG, "初始化 LCD 面板...");

  // 配置面板 IO (SPI)
  ESP_LOGI(TAG, "配置面板 IO...");
  esp_lcd_panel_io_spi_config_t io_config = {
      .cs_gpio_num = GPIO_NUM_NC, // 立创实战派未使用 CS 引脚
      .dc_gpio_num = PIN_NUM_DC,
      .spi_mode = 2, // 立创实战派使用 SPI 模式 2
      .pclk_hz = LCD_PIXEL_CLOCK_HZ,
      .trans_queue_depth = 10,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));
  ESP_LOGI(TAG, "✓ 面板 IO 配置完成");

  // 初始化液晶屏驱动芯片
  ESP_LOGI(TAG, "安装 ST7789 驱动...");
  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = GPIO_NUM_NC, // 立创实战派未使用 RST 引脚
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
  ESP_LOGI(TAG, "✓ ST7789 驱动安装完成");

  // 重置和初始化面板
  ESP_LOGI(TAG, "重置和初始化面板...");
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));

  // 通过 PCA9557 控制显示屏复位 (bit 0)
  ESP_LOGI(TAG, "通过 PCA9557 控制显示屏使能...");
  ESP_ERROR_CHECK(pca9557_set_output(0, 0));
  vTaskDelay(pdMS_TO_TICKS(10));

  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
  ESP_ERROR_CHECK(
      esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
  
  // 设置间隙（如果需要）
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y));
  
  // 开启显示！（关键步骤）
  ESP_LOGI(TAG, "开启显示屏...");
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
  vTaskDelay(pdMS_TO_TICKS(100));
  
  ESP_LOGI(TAG, "✓ LCD 面板初始化完成");
}

// 初始化 LVGL
static void init_lvgl(void) {
  ESP_LOGI(TAG, "初始化 LVGL 库...");
  lv_init();
  ESP_LOGI(TAG, "✓ LVGL 库初始化完成");

  ESP_LOGI(TAG, "初始化 LVGL 端口...");
  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  port_cfg.task_priority = 4;  // 提高优先级到4，避免看门狗超时
  port_cfg.task_max_sleep_ms = 10;  // 设置最大休眠时间，让空闲任务有机会运行
#if CONFIG_SOC_CPU_CORES_NUM > 1
  port_cfg.task_affinity = 1;
#endif
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
  ESP_LOGI(TAG, "✓ LVGL 端口初始化完成");

  ESP_LOGI(TAG, "添加 LCD 显示器...");
  const lvgl_port_display_cfg_t display_cfg = {
      .io_handle = panel_io,
      .panel_handle = panel,
      .control_handle = NULL,
      .buffer_size = LCD_H_RES * 10, // 减小缓冲区到10行，避免看门狗超时
      .double_buffer = true,  // 启用双缓冲提高性能
      .trans_size = 0,
      .hres = LCD_H_RES,
      .vres = LCD_V_RES,
      .monochrome = false,
      .rotation =
          {
              .swap_xy = DISPLAY_SWAP_XY,
              .mirror_x = DISPLAY_MIRROR_X,
              .mirror_y = DISPLAY_MIRROR_Y,
          },
      .color_format = LV_COLOR_FORMAT_RGB565,
      .flags =
          {
              .buff_dma = 1,
              .buff_spiram = 0,
              .sw_rotate = 0,
              .swap_bytes = 1,
              .full_refresh = 0,
              .direct_mode = 0,
          },
  };

  lvgl_disp = lvgl_port_add_disp(&display_cfg);
  if (lvgl_disp == NULL) {
    ESP_LOGE(TAG, "✗ 添加显示器失败");
    return;
  }

  if (DISPLAY_OFFSET_X != 0 || DISPLAY_OFFSET_Y != 0) {
    lv_display_set_offset(lvgl_disp, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
  }

  ESP_LOGI(TAG, "✓ LCD 显示器添加完成");
}

// 百度智能体事件回调
static void agent_event_callback(
    baidu_agent_event_type_t event_type,
    const char *data,
    size_t data_len,
    void *user_data) {
  
  switch (event_type) {
    case BAIDU_AGENT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "百度智能体已连接");
      if (lvgl_port_lock(100)) {
        if (status_label != NULL) {
          const char *status_text = "回答中...";
          lv_label_set_text(status_label, status_text);
          lv_obj_set_style_text_font(status_label, font_manager_get_font(status_text, 10), 0);
        }
        lvgl_port_unlock();
      }
      break;
      
    case BAIDU_AGENT_EVENT_MESSAGE:
      ESP_LOGI(TAG, "收到回复片段: %.*s", (int)data_len, data);
      
      // 追加到缓冲区
      if (response_buffer_len + data_len < RESPONSE_BUFFER_SIZE - 1) {
        memcpy(response_buffer + response_buffer_len, data, data_len);
        response_buffer_len += data_len;
        response_buffer[response_buffer_len] = '\0';
      } else {
        ESP_LOGW(TAG, "响应缓冲区已满，无法追加更多内容");
      }
      
      // 更新屏幕显示
      if (lvgl_port_lock(100)) {
        if (response_label != NULL) {
          lv_label_set_text(response_label, response_buffer);
          // 动态更新字体以支持中文
          lv_obj_set_style_text_font(response_label, font_manager_get_font(response_buffer, 14), 0);
          ESP_LOGI(TAG, "✓ 已更新屏幕显示 (累积长度: %d)", response_buffer_len);
        } else {
          ESP_LOGW(TAG, "response_label 为 NULL");
        }
        lvgl_port_unlock();
      } else {
        ESP_LOGE(TAG, "✗ 无法获取 LVGL 锁");
      }
      break;
      
    case BAIDU_AGENT_EVENT_ERROR:
      ESP_LOGE(TAG, "错误: %s", data);
      if (lvgl_port_lock(100)) {
        if (status_label != NULL) {
          char error_text[64];
          snprintf(error_text, sizeof(error_text), "错误: %s", data);
          lv_label_set_text(status_label, error_text);
          lv_obj_set_style_text_font(status_label, font_manager_get_font(error_text, 10), 0);
          lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);  // 红色
        }
        lvgl_port_unlock();
      }
      break;
      
    case BAIDU_AGENT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "百度智能体已断开");
      if (lvgl_port_lock(100)) {
        if (status_label != NULL) {
          const char *done_text = "回答结束";
          lv_label_set_text(status_label, done_text);
          lv_obj_set_style_text_font(status_label, font_manager_get_font(done_text, 10), 0);
          lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFD700), 0);  // 恢复金色
        }
        lvgl_port_unlock();
      }
      break;
      
    default:
      break;
  }
}

// 创建对话 UI
static void create_mario_ui(void) {
  ESP_LOGI(TAG, "创建对话 UI 界面...");

  // 锁定 LVGL
  if (lvgl_port_lock(0)) {
    // 获取活动屏幕
    lv_obj_t *scr = lv_screen_active();

    // 设置背景色为纯黑色
    ESP_LOGI(TAG, "  - 设置背景色");
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);

    // 顶部标题 "百度智能体"
    ESP_LOGI(TAG, "  - 创建顶部标题");
    title_label = lv_label_create(scr);
    const char *title_text = "百度智能体";
    lv_label_set_text(title_label, title_text);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_label, font_manager_get_font(title_text, 16), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 5);

    // 用户输入标签（第二行，右对齐）
    ESP_LOGI(TAG, "  - 创建用户输入标签");
    user_input_label = lv_label_create(scr);
    lv_label_set_text(user_input_label, "");
    lv_obj_set_style_text_color(user_input_label, lv_color_hex(0x4CAF50), 0);  // 绿色
    lv_obj_set_style_text_font(user_input_label, font_manager_get_font("", 12), 0);
    lv_obj_set_width(user_input_label, LCD_H_RES - 20);
    lv_label_set_long_mode(user_input_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(user_input_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(user_input_label, LV_ALIGN_TOP_RIGHT, -10, 30);

    // AI 响应标签（左对齐，占据大部分空间）
    ESP_LOGI(TAG, "  - 创建响应标签");
    response_label = lv_label_create(scr);
    const char *wait_text = "等待消息...";
    lv_label_set_text(response_label, wait_text);
    lv_obj_set_style_text_color(response_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(response_label, font_manager_get_font(wait_text, 12), 0);
    lv_obj_set_width(response_label, LCD_H_RES - 20);
    lv_obj_set_height(response_label, LCD_V_RES - 80);  // 留出顶部和底部空间
    lv_label_set_long_mode(response_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(response_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(response_label, LV_ALIGN_TOP_LEFT, 10, 55);

    // 底部状态标签（右下角）
    ESP_LOGI(TAG, "  - 创建状态标签");
    status_label = lv_label_create(scr);
    const char *ready_text = "准备就绪";
    lv_label_set_text(status_label, ready_text);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFD700), 0);  // 金色
    lv_obj_set_style_text_font(status_label, font_manager_get_font(ready_text, 10), 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_RIGHT, -5, -5);

    // 强制刷新屏幕
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "✓ 对话 UI 创建完成");
  } else {
    ESP_LOGE(TAG, "✗ 无法锁定 LVGL");
  }
}

// WiFi 状态回调
static void wifi_status_callback(bool connected) {
  if (connected) {
    ESP_LOGI(TAG, "WiFi 已连接");
    if (lvgl_port_lock(0)) {
      if (status_label != NULL) {
        lv_label_set_text(status_label, "WiFi 已连接");
      }
      lvgl_port_unlock();
    }
  } else {
    ESP_LOGI(TAG, "WiFi 断开连接");
    if (lvgl_port_lock(0)) {
      if (status_label != NULL) {
        lv_label_set_text(status_label, "WiFi 断开");
      }
      lvgl_port_unlock();
    }
  }
}

// 初始化 WiFi
static void init_wifi(void) {
  ESP_LOGI(TAG, "初始化 WiFi...");
  
  wifi_manager_config_t wifi_cfg = {
    .ssid = "88888888",
    .password = "dami1010",
    .callback = wifi_status_callback,
    .max_retry = 5,
  };
  
  esp_err_t ret = wifi_manager_init(&wifi_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "✗ WiFi 初始化失败: %s", esp_err_to_name(ret));
    return;
  }
  
  // 获取 IP 地址
  char ip_str[16];
  if (wifi_manager_get_ip_str(ip_str, sizeof(ip_str)) == ESP_OK) {
    ESP_LOGI(TAG, "✓ WiFi 连接成功, IP: %s", ip_str);
  }
}

// 发送消息到百度智能体（清空之前的响应缓冲区）
static esp_err_t send_message_to_agent(const char *message) {
  // 保存用户输入
  strncpy(current_user_input, message, sizeof(current_user_input) - 1);
  current_user_input[sizeof(current_user_input) - 1] = '\0';
  
  // 清空响应缓冲区，准备接收新的回复
  response_buffer_len = 0;
  response_buffer[0] = '\0';
  
  ESP_LOGI(TAG, "发送消息: %s", message);
  ESP_LOGI(TAG, "已清空响应缓冲区");
  
  // 更新 UI 显示用户输入
  if (lvgl_port_lock(100)) {
    if (user_input_label != NULL) {
      lv_label_set_text(user_input_label, current_user_input);
      lv_obj_set_style_text_font(user_input_label, font_manager_get_font(current_user_input, 12), 0);
    }
    if (status_label != NULL) {
      lv_label_set_text(status_label, "发送中...");
      lv_obj_set_style_text_font(status_label, font_manager_get_font("发送中...", 10), 0);
    }
    if (response_label != NULL) {
      lv_label_set_text(response_label, "");
    }
    lvgl_port_unlock();
  }
  
  return baidu_agent_send_message(agent_handle, message, 0);
}

// 初始化百度智能体
static void init_baidu_agent(void) {
  ESP_LOGI(TAG, "初始化百度智能体客户端...");
  
  baidu_agent_config_t config = {
    .app_id = "PcQ6T6ShKPSGSeaITclWx8WS0HQ70opz",
    .secret_key = "YLMyCANTXF4TNhRdww9LrLXSGVtTKdje",
    .open_id = "esp32_user_001",  // 唯一用户ID
    .thread_id = NULL,  // 首次对话为NULL
    .callback = agent_event_callback,
    .user_data = NULL,
    .auto_reconnect = true,
    .reconnect_interval = 5000,
  };
  
  agent_handle = baidu_agent_init(&config);
  if (agent_handle == NULL) {
    ESP_LOGE(TAG, "✗ 百度智能体初始化失败");
    return;
  }
  
  ESP_LOGI(TAG, "✓ 百度智能体初始化完成");
}

void app_main(void) {
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
  ESP_LOGI(TAG, "║   Mario AI Assistant 启动中...        ║");
  ESP_LOGI(TAG, "║   立创实战派 ESP32-S3                 ║");
  ESP_LOGI(TAG, "║   百度智能体集成版                    ║");
  ESP_LOGI(TAG, "╚════════════════════════════════════════╝");
  ESP_LOGI(TAG, "");

  // 步骤 1: 初始化 I2C 和 PCA9557 IO 扩展芯片
  init_i2c_and_pca9557();

  // 步骤 2: 初始化背光
  init_backlight();

  // 步骤 3: 初始化 SPI 总线
  init_spi_bus();

  // 步骤 4: 初始化 LCD 面板
  init_lcd_panel();

  // 步骤 5: 初始化 LVGL
  init_lvgl();

  // 步骤 5.5: 初始化字体管理器
  font_manager_init();

  // 步骤 6: 创建 Mario UI
  create_mario_ui();

  // 步骤 7: 初始化 WiFi
  init_wifi();

  // 步骤 8: 初始化百度智能体
  init_baidu_agent();

  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
  ESP_LOGI(TAG, "║   Mario AI 初始化完成！              ║");
  ESP_LOGI(TAG, "║   It's-a me, Mario!                  ║");
  ESP_LOGI(TAG, "╚════════════════════════════════════════╝");
  ESP_LOGI(TAG, "");

  // 等待 WiFi 连接稳定
  vTaskDelay(pdMS_TO_TICKS(2000));

  // 发送测试消息
  ESP_LOGI(TAG, "发送测试消息到百度智能体...");
  esp_err_t ret = send_message_to_agent("你好，我是Mario！");
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "✓ 消息发送成功");
  } else {
    ESP_LOGE(TAG, "✗ 消息发送失败: %s", esp_err_to_name(ret));
  }

  // 主循环
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

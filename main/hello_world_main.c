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
#include <stdio.h>

static const char *TAG = "LVGL_DEMO";

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
  port_cfg.task_priority = 1;
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
      .buffer_size = LCD_H_RES * 20, // 使用较小的缓冲区
      .double_buffer = false,
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

// 创建 UI
static void create_demo_ui(void) {
  ESP_LOGI(TAG, "创建 UI 界面...");

  // 锁定 LVGL
  if (lvgl_port_lock(0)) {
    // 获取活动屏幕
    lv_obj_t *scr = lv_screen_active();
    ESP_LOGI(TAG, "  - 设置背景色");
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x003a57), LV_PART_MAIN);

    // 创建标题标签
    ESP_LOGI(TAG, "  - 创建标题标签");
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Frontend Chao Fen King");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -20);

    // 创建日期标签
    ESP_LOGI(TAG, "  - 创建日期标签");
    lv_obj_t *date_label = lv_label_create(scr);
    lv_label_set_text(date_label, "2025-10-16");
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 20);

    // 强制刷新屏幕
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "✓ UI 创建完成");
  } else {
    ESP_LOGE(TAG, "✗ 无法锁定 LVGL");
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
  ESP_LOGI(TAG, "║   LVGL Hello World Demo 启动中...     ║");
  ESP_LOGI(TAG, "║   立创实战派 ESP32-S3                 ║");
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

  // 步骤 6: 创建 UI
  create_demo_ui();

  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
  ESP_LOGI(TAG, "║   所有初始化完成！                    ║");
  ESP_LOGI(TAG, "║   屏幕应该显示内容了                  ║");
  ESP_LOGI(TAG, "╚════════════════════════════════════════╝");
  ESP_LOGI(TAG, "");

  // 主循环
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

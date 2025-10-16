# LVGL Hello World 使用说明

## 项目简介
这是一个在 ESP32-S3 上运行的 LVGL Hello World 示例，将在 ST7789 显示屏上显示 "Hello World" 文本和一个可触摸的按钮。

## 硬件要求
- ESP32-S3-WROOM-1-N16R8 开发板
- ST7789 2.0寸 IPS 显示屏 (320x240分辨率，SPI接口)
- FT6336 电容触摸屏 (I2C接口)

## 引脚连接
请参考 `CONFIG.md` 文件中的引脚配置说明。

**重要提示**：如果你的硬件连接与默认配置不同，请修改 `main/hello_world_main.c` 文件中的引脚定义：
```c
// ST7789 显示屏引脚
#define PIN_NUM_MOSI        11
#define PIN_NUM_CLK         12
#define PIN_NUM_CS          10
#define PIN_NUM_DC          13
#define PIN_NUM_RST         14
#define PIN_NUM_BK_LIGHT    9

// FT6336 触摸屏引脚
#define I2C_MASTER_SCL_IO   17
#define I2C_MASTER_SDA_IO   18
```

## 构建和烧录

### 1. 设置 ESP-IDF 环境
确保你已经安装了 ESP-IDF 开发环境。如果还没有安装，请参考 [ESP-IDF 官方文档](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/index.html)。

```bash
# 激活 ESP-IDF 环境
. $HOME/esp/esp-idf/export.sh
```

### 2. 配置项目（可选）
```bash
cd /Users/zhangao/Documents/vv-esp
idf.py menuconfig
```

在 menuconfig 中，你可以配置：
- LVGL 相关设置（Component config -> LVGL configuration）
- 启用更多字体（LVGL configuration -> Font usage）
- 其他 ESP32 系统设置

### 3. 编译项目
```bash
idf.py build
```

### 4. 烧录到开发板
```bash
# 替换 /dev/ttyUSB0 为你的串口设备
idf.py -p /dev/ttyUSB0 flash
```

### 5. 查看日志输出
```bash
idf.py -p /dev/ttyUSB0 monitor
```

或者一步完成编译、烧录和监控：
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## 预期效果
1. 屏幕应该显示深蓝色背景
2. 中间显示白色的 "Hello World!" 文本
3. 下方有一个 "Touch Me" 按钮
4. 触摸按钮会有视觉反馈（如果触摸屏已正确连接和配置）

## 故障排除

### 1. 屏幕无显示
- 检查所有引脚连接是否正确
- 确认供电是否稳定（建议使用外部电源）
- 检查背光引脚是否正确连接和配置
- 尝试调整 `esp_lcd_panel_swap_xy` 和 `esp_lcd_panel_mirror` 的参数

### 2. 显示颜色异常
- 尝试修改 `esp_lcd_panel_invert_color` 的参数
- 检查 RGB 顺序设置

### 3. 显示方向不对
- 修改 `lvgl_init` 函数中的 rotation 参数
- 调整 `esp_lcd_panel_swap_xy` 和 `esp_lcd_panel_mirror` 设置

### 4. 编译错误
- 确保所有依赖组件已正确安装
- 运行 `idf.py fullclean` 然后重新编译
- 检查 ESP-IDF 版本是否兼容

### 5. 触摸不工作
- 检查 I2C 引脚连接
- 确认触摸芯片地址是否正确（FT6336 默认地址通常是 0x38）
- 当前代码只初始化了 I2C，如需完整触摸功能，需要添加 FT6336 驱动代码

## 下一步
- 添加触摸事件处理
- 创建更复杂的 UI 界面
- 集成 WiFi、蓝牙等功能
- 添加传感器数据显示

## 参考资料
- [ESP-IDF 文档](https://docs.espressif.com/projects/esp-idf/)
- [LVGL 文档](https://docs.lvgl.io/)
- [ESP LVGL Port](https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port)


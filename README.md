# Mario AI Assistant - ESP32-S3

基于 ESP32-S3 的智能语音助手项目，集成百度智能体 API 和 LVGL 图形界面。

## 项目简介

本项目在立创实战派 ESP32-S3 开发板上实现了一个智能语音助手，主要功能包括：

- ✅ LVGL 图形用户界面
- ✅ ST7789 显示屏驱动（320x240）
- ✅ WiFi 网络连接
- ✅ 百度智能体 API 集成
- ✅ 中英文字体支持（自动切换）
- ✅ 云端消息接收和显示

## 硬件要求

### 开发板
- 立创实战派 ESP32-S3 (ESP32-S3-WROOM-1-N16R8)

### 显示屏
- ST7789 2.0寸 IPS 全视角屏幕
- 分辨率：320x240
- 接口：SPI

### 引脚连接

| 功能 | ESP32-S3 引脚 | 说明 |
|------|---------------|------|
| LCD MOSI | GPIO 40 | SPI 数据 |
| LCD CLK  | GPIO 41 | SPI 时钟 |
| LCD DC   | GPIO 39 | 数据/命令 |
| LCD BL   | GPIO 42 | 背光（低电平点亮） |
| I2C SDA  | GPIO 1  | I2C 数据（PCA9557） |
| I2C SCL  | GPIO 2  | I2C 时钟（PCA9557） |

## 项目结构

```
vv-esp/
├── main/
│   ├── main.c              # 主程序
│   ├── font_manager.c      # 字体管理器
│   ├── font_manager.h      # 字体管理器头文件
│   ├── CMakeLists.txt      # 主组件构建配置
│   └── idf_component.yml   # 组件依赖
├── components/
│   ├── wifi_manager/       # WiFi 管理组件
│   └── baidu_agent/        # 百度智能体客户端
├── managed_components/     # ESP 组件管理器下载的组件
├── xiaozhi-esp32/          # 官方参考项目（xiaozhi）
├── CMakeLists.txt          # 顶层构建配置
├── sdkconfig               # 项目配置
├── README.md               # 本文件
├── README_BAIDU_AGENT.md   # 百度智能体集成说明
└── README_CHINESE_FONT.md  # 中文字体支持说明
```

## 快速开始

### 1. 环境准备

安装 ESP-IDF v5.5 或更高版本：

```bash
# 激活 ESP-IDF 环境
. $HOME/esp/esp-idf/export.sh
```

### 2. 配置 WiFi 和 API 密钥

编辑 `main/main.c`，修改以下配置：

```c
// WiFi 配置
wifi_manager_config_t wifi_cfg = {
    .ssid = "YOUR_WIFI_SSID",
    .password = "YOUR_WIFI_PASSWORD",
    // ...
};

// 百度智能体配置
baidu_agent_config_t config = {
    .app_id = "YOUR_APP_ID",
    .secret_key = "YOUR_SECRET_KEY",
    // ...
};
```

### 3. 编译和烧录

```bash
# 编译项目
idf.py build

# 烧录到设备
idf.py -p /dev/ttyUSB0 flash

# 查看日志
idf.py -p /dev/ttyUSB0 monitor

# 或一次完成所有步骤
idf.py -p /dev/ttyUSB0 flash monitor
```

## 功能特性

### 中文字体支持

项目使用字体管理器自动根据文本内容选择合适的字体：

- **中文文本**：使用思源黑体（Source Han Sans SC）
- **英文文本**：使用 Montserrat 字体
- **自动检测**：无需手动指定字体类型

详细说明请参考 [README_CHINESE_FONT.md](README_CHINESE_FONT.md)

### 百度智能体集成

通过 HTTP 客户端与百度智能体 API 通信：

- 支持发送文本消息
- 接收 AI 回复并在屏幕上显示
- 自动重连机制
- 事件回调处理

详细说明请参考 [README_BAIDU_AGENT.md](README_BAIDU_AGENT.md)

### WiFi 管理

独立的 WiFi 管理组件：

- 自动连接和重连
- 连接状态回调
- IP 地址获取

## 配置选项

使用 `idf.py menuconfig` 可以配置：

- LVGL 设置（Component config → LVGL configuration）
- 字体选项（LVGL configuration → Font usage）
- WiFi 设置
- 其他 ESP32 系统配置

## 故障排除

### 屏幕无显示
- 检查引脚连接
- 确认背光引脚（GPIO 42）为低电平
- 检查 SPI 配置

### WiFi 连接失败
- 确认 SSID 和密码正确
- 检查路由器 2.4GHz 频段已启用
- 查看串口日志排查问题

### 中文显示为方框
- 确认已启用思源黑体字体（sdkconfig 中）
- 重新编译项目
- 检查字体管理器初始化

### 编译错误
```bash
# 清理并重新编译
idf.py fullclean
idf.py build
```

## 开发指南

### 添加新的 UI 元素

```c
// 在 create_mario_ui() 函数中添加
lv_obj_t *my_label = lv_label_create(scr);
const char *text = "新文本";
lv_label_set_text(my_label, text);
lv_obj_set_style_text_font(my_label,
    font_manager_get_font(text, 16), 0);
lv_obj_align(my_label, LV_ALIGN_CENTER, 0, 0);
```

### 处理百度智能体消息

在 `agent_event_callback()` 函数中处理不同事件：

```c
case BAIDU_AGENT_EVENT_MESSAGE:
    // 处理接收到的消息
    break;
```

## 依赖组件

- `espressif/esp_lvgl_port` - LVGL 端口
- `lvgl/lvgl` - LVGL 图形库
- ESP-IDF 内置组件（esp_lcd, esp_wifi, esp_http_client 等）

## 许可证

本项目基于 ESP-IDF 示例项目开发，遵循相应的开源许可证。

## 参考资源

- [ESP-IDF 文档](https://docs.espressif.com/projects/esp-idf/)
- [LVGL 文档](https://docs.lvgl.io/)
- [百度智能体文档](https://cloud.baidu.com/doc/WENXINWORKSHOP/index.html)
- [立创实战派文档](https://www.lichuang.com/)

## 贡献

欢迎提交 Issue 和 Pull Request！

## 联系方式

如有问题，请通过 GitHub Issues 联系。

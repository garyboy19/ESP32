# ESP32 开放开发环境

本目录是一套可独立使用的 **ESP-IDF v6.1** 开发环境，目标芯片默认为 **ESP32**。

## 目录

| 路径 | 说明 |
| --- | --- |
| `sdk/v6.1/esp-idf` | ESP-IDF 源码 |
| `export.sh/` | 环境激活脚本 |
| `firmware/` | ESP32 开发板自检与遥测固件 |
| `firmware/components/ili9341/` | ILI9341 TFT 驱动 + 绘图层组件 |
| `firmware/TFT_WIRING.md` | TFT 接线说明与排错指南 |
| `gui/board_monitor.py` | 桌面实时监控界面 |
| `activate.sh` | 一键激活当前终端 |

交叉编译器、Python 虚拟环境等工具安装在 `~/.espressif/tools`（乐鑫官方安装器默认位置）。

## 使用

每次打开新终端后：

```bash
cd /Users/freyrandy/Projects/ESP32
source ./activate.sh
cd firmware
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

退出监视器：`Ctrl+]`。

> **注意**：若构建报
> `'.../venv/bin/python3' is currently active in the environment while the project was configured with '.../venv/bin/python'`
> 说明 CMake 缓存里记录的 Python 解释器与当前 venv 不一致，执行 `idf.py fullclean` 后重新构建即可。

## 实时图形监控

先将 `firmware` 中的固件编译并烧录到 ESP32（固件通过 USB 串口发送遥测数据），然后在项目根目录运行：

```bash
python3 -m pip install -r gui/requirements.txt
python3 gui/board_monitor.py
```

在界面中选择 ESP32 串口并点击"连接开发板"。界面会显示运行时间、可用/最低堆内存、Flash 容量、CPU 核心、芯片版本、Wi‑Fi 扫描结果、BOOT 键和 GPIO2 LED 状态，并绘制最近 60 个堆内存采样点。

### 监控界面能力

- **双曲线堆内存图**：青色为可用堆，橙色为历史最低堆，Y 轴按实测数据动态定标（自适应不同板卡/固件负载）
- **堆健康度告警**：窗口内最低堆相对峰值下滑超过 10% 即提示"疑似泄漏"
- **串口自动重连**：拔线或设备异常断开后按 1→5s 退避自动重连，最多重试 10 次；手动断开则不再自动重连
- **链路状态提示**：灰=未连接、橙=已连接但遥测中断 >3s、青=实时接收中

## TFT 仪表盘（ILI9341 显示屏）

固件支持两种模式，通过 `firmware/main/Kconfig` 的 choice 切换：

| 模式 | 配置项 | 说明 |
| --- | --- | --- |
| 板卡自检 | `CONFIG_FIRMWARE_BOARD_TEST=y` | 只通过串口输出遥测 |
| TFT 仪表盘 | `CONFIG_FIRMWARE_TFT_DASHBOARD=y` | 驱动 ILI9341 屏显示遥测，串口同时输出 |

TFT 模式启动后屏幕显示开机自检画面（含红绿蓝彩条，用于验证颜色通道），随后刷新仪表盘：
标题栏、四张数据卡（运行时间 / 可用堆 / 最低堆 / Wi-Fi AP 数）、堆内存趋势图、底部 RSSI 与修订号。

### 组件构成

| 文件 | 说明 |
| --- | --- |
| `components/ili9341/esp_lcd_panel_ili9341.c` | ILI9341 控制器驱动，实现 `esp_lcd_panel` 标准接口 |
| `components/ili9341/tft_disp.c` | RGB565 绘图层：矩形 / 线条 / 文字 / 图表 / 分块刷屏 |
| `components/ili9341/include/font6x8.h` | 6×8 ASCII 点阵字体 |
| `components/ili9341/include/font11x16.h` | 11×16 ASCII 点阵字体 |
| `main/tft_dashboard.c` | 仪表盘主程序，复用板卡遥测数据 |
| `tools/gen_font.py` | 用 Pillow 重新生成点阵字体 |

开机画面含**左三角/右方块**不对称标记与从左到右的彩条，用于确认屏幕方向是否镜像。

**方向不对（文字左右颠倒）**：ILI9341 竖屏模式下 MADCTL 的 MX 位是必选的
（数据手册参考值 `0x48`），固件默认已带 MX 位；按板载 **BOOT 键**可循环切换
4 种镜像组合并立即重绘，无需重新烧录。

接线方式、常见花屏/黑屏排查见 **[firmware/TFT_WIRING.md](firmware/TFT_WIRING.md)**。

## 版本控制

项目已纳入 git 管理（`.gitignore` 排除了 `sdk/`、`export.sh/`、`firmware/build/`、`firmware/sdkconfig` 等本地产物）。

```bash
git log --oneline
git status
```

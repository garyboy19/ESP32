# ESP32 开放开发环境

本目录是一套可独立使用的 **ESP-IDF v6.1** 开发环境，目标芯片默认为 **ESP32**。

## 目录

| 路径 | 说明 |
| --- | --- |
| `sdk/v6.1/esp-idf` | ESP-IDF 源码 |
| `export.sh/` | 环境激活脚本 |
| `firmware/` | ESP32 开发板自检与遥测固件 |
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

## 实时图形监控

先将 `firmware` 中的固件编译并烧录到 ESP32（固件通过 USB 串口发送遥测数据），然后在项目根目录运行：

```bash
python3 -m pip install -r gui/requirements.txt
python3 gui/board_monitor.py
```

在界面中选择 ESP32 串口并点击“连接开发板”。界面会显示运行时间、可用/最低堆内存、Flash 容量、CPU 核心、芯片版本、Wi‑Fi 扫描结果、BOOT 键和 GPIO2 LED 状态，并绘制最近 60 个堆内存采样点。

# ESP32 TFT 仪表盘固件 — 发布清单

生成时间：2026-11-01
目标板：ESP32-D0WDQ6 (rev 1.1) / 4 MB Flash / 无 PSRAM
固件 ID：`tft-dashboard-cjk-2026-11-01`

## 产物

| 文件 | 大小 | SHA-256 |
|---|---|---|
| `bootloader.bin` | 26,176 B | `a8f1150a4a3379b363752db6a32260983c74ac32e9fa31797529b9ee68017d71` |
| `partition-table.bin` | 3,072 B | `3915d47e612cc044a76c58a1e6fcab2157525099e95df2448edec865b733bd84` |
| `firmware.bin` | 1,096,800 B | `e4ebfb34c60c01d597e2db859fd06bccf7e498a18b8550675118120dd0a93f12` |

## 烧录

```bash
cd firmware/release
esptool.py --chip esp32 --port /dev/cu.usbserial-110 --baud 460800 \
  --flash-mode dio --flash-freq 40m --flash-size 4MB \
  write_flash 0x1000 bootloader.bin \
               0x8000 partition-table.bin \
               0x10000 firmware.bin
```

或用 ESP-IDF：

```bash
cd firmware
python3 "$IDF_PATH/tools/idf.py" -p /dev/cu.usbserial-110 flash
```

### ⚠️ 分区表已变更，必须全量擦除

分区表从 1 MB app 分区改为 **2 MB app + 1 MB storage**（为容纳扩大的中文字库）。
分区表位于 flash `0x8000`，改变后引导程序找不到 app，**只刷 app 会导致无法启动**。

首次刷写本固件必须：

```bash
python3 "$IDF_PATH/tools/idf.py" -p /dev/cu.usbserial-110 erase-flash
python3 "$IDF_PATH/tools/idf.py" -p /dev/cu.usbserial-110 flash
```

**副作用**：`erase-flash` 会清除 NVS 中的 Wi-Fi 保存配置，需要重新设置。

## 分区表

```
# Name, Type, SubType, Offset, Size, Flags
nvs,      data, nvs,     0x9000,   24K,
phy_init, data, phy,     0xf000,    4K,
factory,  app,  factory, 0x10000,  2M,
storage,  data, fat,      ,         1M,
```

## 本版本内容

- ILI9341 240×320 TFT 仪表盘（主页面 + Wi-Fi 网络列表页）
- **UTF-8 中文显示**：GB2312 一级 + 二级共 6763 汉字，16×16 点阵
- 中文字库存于 Flash（`EMBED_FILES`），运行时按码位查表读取，**不占用 RAM**
- 中英混排：中文 16 px 全宽 / ASCII 9 px 半宽，共用基线
- 缺字降级为 `?`
- 渲染互斥锁，防止页面刷新与方向切换并发写 framebuffer 导致花屏
- BOOT 键：短按切页 / 长按 1.2 s 切方向（运行时可切，无需重刷）
- 串口命令：`@CMD SCAN | LIST | CONNECT "ssid" "pass" | DISCONNECT | PAGE`
- 串口回包：`@WIFI {...}` 单行 JSON（不回显密码）；`@PAGE {...}` 用于中文渲染取证

## 运行时资源

实测（串口 `@DATA` 帧）：

```
free_heap ≈ 48,800 B
min_heap  ≈ 44,468 B
```

## 重新构建

字库由脚本生成，**不要手工编辑**：

```bash
cd firmware/tools
python3 gen_cjk_font.py
```

生成器依赖 macOS 系统字体（`/System/Library/Fonts/STHeiti Light.ttc`）和 Pillow。
`CHAR_SET` 变量控制字库范围：

- `"l1"`   — GB2312 一级 3755 字（124 KB），1 MB 分区即可
- `"l1l2"` — 一级 + 二级 6763 字（~218 KB），**需 2 MB 分区**

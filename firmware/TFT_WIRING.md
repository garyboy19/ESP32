# ESP32 ILI9341 TFT 接线说明

## 硬件前提

- **ESP32 rev 1.1，4 MB Flash，无 PSRAM**（本项目实测的板子）
- **2.8 寸 ILI9341 SPI 屏**，240×320，4 线 SPI（不是 8 位并口版本）

> ⚠️ 屏幕背面若写着 `ILI9341 SPI` / 有 `SCLK / MOSI / CS / DC / RST / BL` 丝印，才是本项目支持的型号。
> 标着 `ILI9341 8-bit parallel` 的排针版本**不适用**。

## 接线表

| TFT 模块引脚 | ESP32 引脚 | 说明 |
|---|---|---|
| `VCC` | `3V3` | 多数 ILI9341 模块支持 3.3V。**不要接 5V**（除非模块明确标注带电平转换） |
| `GND` | `GND` | |
| `SCL` / `CLK` / `SCK` | **GPIO 18** | SPI2 SCLK |
| `SDA` / `DIN` / `MOSI` | **GPIO 23** | SPI2 MOSI |
| `CS` | **GPIO 5** | 片选 |
| `DC` / `RS` | **GPIO 21** | 数据/命令选择 |
| `RST` / `RES` | **GPIO 22** | 复位 |
| `BL` / `LED` | **GPIO 4** | 背光（高电平点亮） |
| `MISO` / `SDO` | **不接** | 本项目只写不读屏幕；不接 MISO 可以省一根线 |
| `SD_CS` / `TF_CS` | **不接** | 模块上 TF 卡座专用，与屏幕无关 |

### 接线图

```
   ILI9341 模块                  ESP32
  ┌────────────┐
  │ VCC        │──────────────► 3V3
  │ GND        │──────────────► GND
  │ SCL / CLK  │──────────────► GPIO18   (SCLK)
  │ SDA / MOSI │──────────────► GPIO23   (MOSI)
  │ CS         │──────────────► GPIO5    (CS)
  │ DC  / RS   │──────────────► GPIO21   (DC)
  │ RST / RES  │──────────────► GPIO22   (RST)
  │ BL  / LED  │──────────────► GPIO4    (背光)
  │ MISO       │   ✗ 不接
  └────────────┘
```

## 与板上已有资源的关系

| 资源 | GPIO | 冲突情况 |
|---|---|---|
| 板载 LED | GPIO 2 | 不冲突，TFT 未使用 |
| BOOT 键 | GPIO 0 | 不冲突，TFT 未使用 |
| 串口 TX/RX | GPIO 1 / 3 | 不冲突（TFT 走 SPI） |

## 配置方式

所有参数在 `firmware/main/Kconfig` 里，改完重新编译即可：

| Kconfig 项 | 默认值 | 说明 |
|---|---|---|
| `TFT_WIDTH` / `TFT_HEIGHT` | 240 / 320 | 屏幕分辨率 |
| `TFT_SPI_HOST` | 1 | 1=SPI2(HSPI)，2=SPI3(VSPI) |
| `TFT_PIN_SCLK` | 18 | |
| `TFT_PIN_MOSI` | 23 | |
| `TFT_PIN_CS` | 5 | |
| `TFT_PIN_DC` | 21 | |
| `TFT_PIN_RST` | 22 | |
| `TFT_PIN_BACKLIGHT` | 4 | 填 -1 表示不控制背光 |
| `TFT_PCLK_HZ` | 40000000 | SPI 时钟，线过长可降到 20000000 |
| `TFT_ROTATION` | 0 | 0 / 90 / 180 / 270 |
| `TFT_BGR` | n | 颜色红蓝互换时打开 |
| `TFT_INVERT_COLOR` | n | 颜色整体反相时打开 |

## 常见问题排查

### 屏幕全黑 / 全白

1. **先看背光**：GPIO4 是否接对？屏幕亮了吗？亮着但全黑说明是数据问题，不亮说明是供电或 BL 线问题。
2. **看串口日志**，确认有这两行：
   ```
   I (811) tft_disp: disp created: 240x320, 153600 bytes in 10 chunks, dma chunk=3840 bytes
   I (811) tft_dash: tft ready: 240x320, pclk=40000000 Hz
   ```
   没有 = 初始化就失败了。
3. **接 MISO**：`display id: 00 00 00 00` 表示读不到控制器 ID，说明没接 MISO 或读失败。接上 MISO 后应显示 `00 FF 93 xx`（`0x93` = ILI9341）。这一步能直接证明 SPI 通了。

### 花屏 / 颜色不对

- 红蓝互换 → 打开 `TFT_BGR`
- 整体颜色反相（像负片）→ 打开 `TFT_INVERT_COLOR`
- 画面旋转 90° → 把 `TFT_ROTATION` 改成 90 或 270
- 花屏 → 把 `TFT_PCLK_HZ` 降到 20000000，缩短杜邦线

### 串口日志刷屏但屏幕不动

检查 `CS` 和 `DC` 是否接反 —— 这两个最容易接错，且接错时程序不会报错，只是不显示。

## 内存说明（重要）

本板是 ESP32 rev 1.1，**没有 PSRAM**。240×320×2 = 153600 字节的帧缓冲无法作为一整块连续内存分配（最大 DRAM 堆区约 154 KiB，Wi-Fi 驱动还要占一大块）。

因此 `tft_disp.c` 把帧缓冲**拆成 10 块 16 KiB** 分别分配，绘制 API 对上层透明，不受影响。

日志里的这行是正常的：
```
W (811) tft_disp: PSRAM unavailable, frame buffer in internal RAM
```

固件启动后剩余可用堆约 60 KB，属于正常范围。

## 内存与分区占用

| 项 | 值 |
|---|---|
| firmware.bin | 0xC4AB0 ≈ 805 KB |
| 最小分区 | 0x100000 (1 MB) |
| 剩余 | 约 23% |

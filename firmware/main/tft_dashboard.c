/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP32 + ILI9341 2.8" TFT dashboard.
 *
 * Shows live board telemetry (heap, uptime, Wi-Fi scan) on the 240x320
 * display and keeps printing the same data as JSON on the serial port, so
 * the existing PC-side GUI (gui/board_monitor.py) still works.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ili9341.h"
#include "tft_disp.h"
#include "tft_cjk.h"
#include "wifi_page.h"

static const char *TAG = "tft_dash";

/* ---------------- board telemetry (mirrors board_test.c) ---------------- */
typedef struct {
    uint32_t uptime_s;
    size_t free_heap;
    size_t min_heap;
    int ap_count;
    int best_rssi;
    int best_channel;
    char revision[16];
    char mac[24];
} board_info_t;

static board_info_t g_info;
static SemaphoreHandle_t g_info_mutex;

/* ---------------- TFT config ---------------- */
#define TFT_SPI_HOST       SPI2_HOST
#define TFT_PIN_SCLK       CONFIG_TFT_PIN_SCLK
#define TFT_PIN_MOSI       CONFIG_TFT_PIN_MOSI
#define TFT_PIN_CS         CONFIG_TFT_PIN_CS
#define TFT_PIN_DC         CONFIG_TFT_PIN_DC
#define TFT_PIN_RST        CONFIG_TFT_PIN_RST
#define TFT_PIN_BACKLIGHT  CONFIG_TFT_PIN_BACKLIGHT
#define TFT_PCLK_HZ        CONFIG_TFT_PCLK_HZ

#define FB_WIDTH           CONFIG_TFT_WIDTH
#define FB_HEIGHT          CONFIG_TFT_HEIGHT

/* ---------------- Wi-Fi ---------------- */
static int g_wifi_ap_count;
static int g_wifi_best_rssi;
static int g_wifi_best_channel;
static esp_netif_t *g_sta_netif;

/* Security label for the Wi-Fi list, indexed by wifi_auth_mode_t. */
static const char *wifi_auth_label(uint8_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:                  return "OPEN";
    case WIFI_AUTH_WEP:                   return "WEP";
    case WIFI_AUTH_WPA_PSK:               return "WPA";
    case WIFI_AUTH_WPA2_PSK:              return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:          return "WPA2";
    case WIFI_AUTH_WPA3_PSK:              return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:         return "WPA3";
    case WIFI_AUTH_OWE:                   return "OWE";
    case WIFI_AUTH_WAPI_PSK:              return "WAPI";
    case WIFI_AUTH_ENTERPRISE:            return "EAP";
    default:                              return "SEC";
    }
}

/* Security label in Chinese, for the Wi-Fi list column. */
static const char *wifi_auth_label_cjk(uint8_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:                  return "开放";
    case WIFI_AUTH_WEP:                   return "WEP";
    case WIFI_AUTH_WPA_PSK:               return "WPA";
    case WIFI_AUTH_WPA2_PSK:              return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:          return "WPA2";
    case WIFI_AUTH_WPA3_PSK:              return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:         return "WPA3";
    case WIFI_AUTH_OWE:                   return "OWE";
    case WIFI_AUTH_WAPI_PSK:              return "WAPI";
    case WIFI_AUTH_ENTERPRISE:            return "EAP";
    default:                              return "加密";
    }
}

/* Signal strength as a 0-4 bar count, for the list rows. */
static int wifi_rssi_bars(int8_t rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    if (rssi >= -90) return 1;
    return 0;
}

static void wifi_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != WIFI_EVENT_SCAN_DONE) {
        return;
    }
    wifi_page_set_scanning(false);
    uint16_t number = 0;
    if (esp_wifi_scan_get_ap_num(&number) != ESP_OK) {
        return;
    }
    int best = -127;
    int chan = 0;
    uint16_t show = number > 16 ? 16 : number;
    if (show > 0) {
        wifi_ap_record_t *records = calloc(show, sizeof(wifi_ap_record_t));
        if (records) {
            if (esp_wifi_scan_get_ap_records(&show, records) == ESP_OK) {
                for (uint16_t i = 0; i < show; i++) {
                    if (records[i].rssi > best) {
                        best = records[i].rssi;
                        chan = records[i].primary;
                    }
                }
                /* keep a compact copy for the on-screen list */
                wifi_page_store_scan(records, (int)show, (int)number);
            }
            free(records);
        }
    } else {
        wifi_page_store_scan(NULL, 0, (int)number);
    }
    g_wifi_ap_count = number;
    g_wifi_best_rssi = best;
    g_wifi_best_channel = chan;
    ESP_LOGI(TAG, "wifi scan: %d aps, best %d dBm ch%d", number, best, chan);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_page_set_status("disconnected");
        wifi_page_clear_connected();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        char ipstr[16];
        snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&event->ip_info.ip));
        wifi_page_set_connected(NULL, ipstr);
        wifi_page_set_status("connected");
        ESP_LOGI(TAG, "got ip: %s", ipstr);
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    g_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_scan_done, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}


/* ---------------- TFT bring-up ---------------- */
static esp_lcd_panel_handle_t g_panel;
static tft_disp_t *g_disp;

static void draw_splash(void);
static void render_dashboard(void);
static void render_wifi_page(void);
static void dump_page_text(void);
static void hist_push(int v_free, int v_min);
static void wifi_reply(const char *fmt, ...);
static void wifi_reply_list(void);
static void wifi_do_connect(const char *ssid, const char *password);
static void wifi_do_disconnect(void);

/*
 * Rendering mutex.
 *
 * The dashboard task refreshes once per second while the BOOT key task may
 * redraw at any moment (page switch / orientation change). Both write the
 * same frame buffer and push it over SPI, so without this lock the two tasks
 * interleave their drawing and flush cycles - that shows up as garbage on
 * the panel.
 */
static SemaphoreHandle_t g_render_mutex;

/*
 * Orientation presets.
 *
 * MADCTL bit layout: MY=0x80 MX=0x40 MV=0x20 ML=0x10 BGR=0x08 MH=0x04
 *
 * The ILI9341 datasheet reference init for 240x320 portrait writes 0x48
 * (MX|BGR). The MX bit is REQUIRED - leaving it clear mirrors the whole
 * image horizontally. The splash colour bars are ordered RED->GREEN->BLUE,
 * so a mirrored image is visible (red ends up on the right), but the text
 * makes it obvious.
 */
#define ORIENT_COUNT 4
static const struct {
    bool mx;
    bool my;
    const char *label;
} g_orientations[ORIENT_COUNT] = {
    { true,  false, "MX"    },  /* datasheet portrait default (0x48) */
    { false, false, "NONE"  },  /* mirrored - use if your panel needs it */
    { false, true,  "MY"    },  /* vertical flip */
    { true,  true,  "MXY"   },  /* 180 degrees */
};
static int g_orient = 0;

/*
 * Apply an orientation preset to the panel.
 *
 * MX is the horizontal mirror bit and it is REQUIRED for portrait mode:
 * the datasheet reference init writes MADCTL = 0x48 (MX|BGR). Clearing it
 * mirrors the whole image horizontally - the splash colour bars are ordered
 * RED->GREEN->BLUE so red ends up on the right, and text reads backwards.
 *
 * The MV bit (swap_xy) selects landscape; the frame buffer dimensions are
 * swapped accordingly (see FB_WIDTH/FB_HEIGHT).
 */
static void apply_orientation(int idx)
{
    idx = ((idx % ORIENT_COUNT) + ORIENT_COUNT) % ORIENT_COUNT;
    g_orient = idx;
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(g_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(g_panel,
                                         g_orientations[idx].mx,
                                         g_orientations[idx].my));
    ESP_LOGI(TAG, "orientation %d: MADCTL mx=%d my=%d (%s)", idx,
             g_orientations[idx].mx, g_orientations[idx].my,
             g_orientations[idx].label);
}

/*
 * Parse and run one command line. Returns true if the line was a command.
 */
static bool handle_command(const char *line)
{
    if (strncmp(line, "@CMD ", 5) != 0) {
        return false;
    }
    const char *args = line + 5;

    if (strncmp(args, "SCAN", 4) == 0) {
        esp_err_t err = esp_wifi_scan_start(NULL, true);
        wifi_page_set_scanning(err == ESP_OK);
        wifi_reply("{\"type\":\"scan\",\"err\":%d}", (int)err);
        return true;
    }
    if (strncmp(args, "LIST", 4) == 0) {
        wifi_reply_list();
        return true;
    }
    if (strncmp(args, "PAGE", 4) == 0) {
        /* Debug: dump the exact text the Wi-Fi page renders, plus the
         * code points that are missing from the CJK font (drawn as '?'). */
        dump_page_text();
        return true;
    }
    if (strncmp(args, "DISCONNECT", 10) == 0) {
        wifi_do_disconnect();
        return true;
    }
    if (strncmp(args, "CONNECT ", 8) == 0) {
        args += 8;
        char ssid[33];
        char password[65];
        password[0] = '\0';

        /* Both the SSID and the password may be quoted so that names with
         * spaces survive the round trip. Strip the quotes and unescape
         * backslash-escaped characters. */
        const char *p = args;
        char *out = ssid;
        size_t out_len = 0;
        bool in_quotes = false;

        /* Read the SSID. A quoted token may contain spaces; an unquoted one
         * ends at the first space. */
        while (*p && out_len < sizeof(ssid) - 1) {
            if (*p == '"') {
                in_quotes = !in_quotes;
                p++;
                continue;
            }
            if (!in_quotes && *p == ' ') {
                break;
            }
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            *out++ = *p++;
            out_len++;
        }
        *out = '\0';

        /* The password is the rest of the line (may also be quoted). */
        while (*p == ' ') {
            p++;
        }
        out = password;
        out_len = 0;
        in_quotes = false;
        while (*p && out_len < sizeof(password) - 1) {
            if (*p == '"') {
                in_quotes = !in_quotes;
                p++;
                continue;
            }
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            *out++ = *p++;
            out_len++;
        }
        *out = '\0';

        wifi_do_connect(ssid, password[0] ? password : NULL);
        return true;
    }

    wifi_reply("{\"type\":\"error\",\"msg\":\"unknown command\"}");
    return true;
}

/*
 * Serial command task.
 *
 * The project does not link esp_console, so no UART driver is installed yet.
 * Installing one with tx_buffer_size = 0 keeps stdout working (writes go
 * straight to the FIFO) while giving us an interrupt driven RX buffer.
 */
static void serial_cmd_task(void *arg)
{
    static char line[160];
    int len = 0;

    const uart_port_t port = UART_NUM_0;
    if (!uart_is_driver_installed(port)) {
        /* rx_buffer_size must be > the 128 byte HW FIFO, tx stays at 0 so
         * that printf()/stdout keeps using the direct FIFO path. */
        esp_err_t err = uart_driver_install(port, 1024, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart driver install failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "uart rx driver installed");
    }

    while (1) {
        uint8_t c;
        int n = uart_read_bytes(port, &c, 1, pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                handle_command(line);
                len = 0;
            }
            continue;
        }
        if (c == 8 || c == 127) { /* backspace */
            if (len > 0) {
                len--;
            }
            continue;
        }
        if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

/* ---------------- Wi-Fi list page ---------------- */
#define PAGE_DASHBOARD 0
#define PAGE_WIFI     1

static int g_page = PAGE_DASHBOARD;

/*
 * Column layout for the Wi-Fi list, in pixels.
 *
 * The list uses the CJK font: Chinese glyphs are 16 px wide, ASCII 9 px.
 * The screen is 240 px wide, so the columns are laid out like this:
 *
 *   marker x=2    one ASCII glyph ( 9 px)  -> ends at  11
 *   SSID   x=11   mixed, up to 100 px     -> ends at 111
 *   SEC    x=116  "WPA2" (4 x 9 = 36 px)  -> ends at 152
 *   SIG    x=158  4 bars        ( 16 px)  -> ends at 174
 *   CH     x=200  3 glyphs      ( 27 px)  -> ends at 227
 */
#define WIFI_COL_SSID    2
#define WIFI_COL_SEC     116
#define WIFI_COL_SIG     158
#define WIFI_COL_CH      200
#define WIFI_SSID_X      11
#define WIFI_SSID_PX     100

#define WIFI_ROW_H       18
#define WIFI_LIST_TOP    92

/* Draw a small signal-strength bar (4 segments) at (x, y), 13 px tall. */
static void draw_signal_bars(int x, int y, int bars)
{
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        uint16_t color = (i < bars) ? TFT_COLOR_GREEN : TFT_COLOR_DARKGRAY;
        tft_fill_rect(g_disp, x + i * 4, y + (13 - h), 3, h, color);
    }
}

/*
 * The Wi-Fi page lists the nearby networks that can be joined:
 *   - SSID (clipped to the column)
 *   - security type (开放 / WPA2 / ...)
 *   - signal strength as bars
 *   - channel
 */
static void render_wifi_page(void)
{
    wifi_page_state_t st;
    wifi_page_get(&st);

    char buf[64];
    char ssid[40];

    tft_clear(g_disp, TFT_COLOR_BLACK);

    /* header */
    tft_fill_rect(g_disp, 0, 0, FB_WIDTH, 32, TFT_COLOR_DARKGRAY);
    tft_draw_text_cjk(g_disp, 4, 8, "Wi-Fi 网络列表", TFT_COLOR_CYAN, 1);
    tft_draw_hline(g_disp, 0, 32, FB_WIDTH, TFT_COLOR_GRAY, 1);

    /* status line: connection state */
    if (st.connected) {
        snprintf(buf, sizeof(buf), "已连接：%s", st.connected_ssid);
        tft_draw_text_cjk(g_disp, 2, 36, buf, TFT_COLOR_GREEN, 1);
        if (st.ip[0]) {
            snprintf(buf, sizeof(buf), "IP %s", st.ip);
            tft_draw_text_cjk(g_disp, 2, 54, buf, TFT_COLOR_YELLOW, 1);
        }
    } else {
        snprintf(buf, sizeof(buf), "状态：%s", st.status);
        tft_draw_text_cjk(g_disp, 2, 36, buf, TFT_COLOR_ORANGE, 1);
    }

    /* column headers */
    tft_draw_text_cjk(g_disp, WIFI_COL_SSID, 74, "名称", TFT_COLOR_GRAY, 1);
    tft_draw_text_cjk(g_disp, WIFI_COL_SEC, 74, "加密", TFT_COLOR_GRAY, 1);
    tft_draw_text_cjk(g_disp, WIFI_COL_SIG, 74, "信号", TFT_COLOR_GRAY, 1);
    tft_draw_text_cjk(g_disp, WIFI_COL_CH, 74, "信道", TFT_COLOR_GRAY, 1);
    tft_draw_hline(g_disp, 0, 90, FB_WIDTH, TFT_COLOR_DARKGRAY, 1);

    /* list rows */
    int row_y = WIFI_LIST_TOP;
    const int max_rows = (FB_HEIGHT - 44 - WIFI_LIST_TOP) / WIFI_ROW_H;
    for (int i = 0; i < st.count && i < max_rows; i++) {
        const wifi_page_ap_t *ap = &st.aps[i];

        /* the connected network gets a marker in the margin */
        bool is_current = st.connected && strcmp(ap->ssid, st.connected_ssid) == 0;

        tft_clip_text_cjk(ssid, sizeof(ssid), ap->ssid, WIFI_SSID_PX);
        if (is_current) {
            tft_draw_text_cjk(g_disp, WIFI_COL_SSID, row_y, "*", TFT_COLOR_GREEN, 1);
        }
        tft_draw_text_cjk(g_disp, WIFI_SSID_X, row_y, ssid,
                          is_current ? TFT_COLOR_GREEN : TFT_COLOR_WHITE, 1);

        /* security */
        tft_draw_text_cjk(g_disp, WIFI_COL_SEC, row_y, wifi_auth_label_cjk(ap->auth),
                          ap->auth == WIFI_AUTH_OPEN ? TFT_COLOR_ORANGE : TFT_COLOR_LIGHTGRAY, 1);

        /* signal bars */
        draw_signal_bars(WIFI_COL_SIG, row_y, wifi_rssi_bars(ap->rssi));

        /* channel */
        snprintf(buf, sizeof(buf), "%d", ap->channel);
        tft_draw_text_cjk(g_disp, WIFI_COL_CH, row_y, buf, TFT_COLOR_LIGHTGRAY, 1);

        row_y += WIFI_ROW_H;
    }

    if (st.count == 0) {
        tft_draw_text_cjk(g_disp, 4, WIFI_LIST_TOP + 4,
                          st.scanning ? "正在扫描…" : "未发现网络",
                          TFT_COLOR_GRAY, 1);
    }

    /* footer */
    tft_draw_hline(g_disp, 0, FB_HEIGHT - 36, FB_WIDTH, TFT_COLOR_GRAY, 1);
    snprintf(buf, sizeof(buf), "共发现 %d 个网络", st.total);
    tft_draw_text_cjk(g_disp, 2, FB_HEIGHT - 32, buf, TFT_COLOR_CYAN, 1);
    tft_draw_text_cjk(g_disp, 2, FB_HEIGHT - 16, "BOOT键：切换页面/方向",
                      TFT_COLOR_LIGHTGRAY, 1);

    ESP_ERROR_CHECK(tft_flush(g_disp));
}

/* ---------------- telemetry task ---------------- */
static void telemetry_task(void *arg)
{
    TickType_t last_wifi_scan = 0;
    while (1) {
        board_info_t info = {
            .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL),
            .free_heap = esp_get_free_heap_size(),
            .min_heap = esp_get_minimum_free_heap_size(),
            .ap_count = g_wifi_ap_count,
            .best_rssi = g_wifi_best_rssi,
            .best_channel = g_wifi_best_channel,
        };
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        snprintf(info.revision, sizeof(info.revision), "%d.%d",
                 chip.revision / 100, chip.revision % 100);
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(info.mac, sizeof(info.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        xSemaphoreTake(g_info_mutex, portMAX_DELAY);
        g_info = info;
        xSemaphoreGive(g_info_mutex);

        hist_push((int)info.free_heap, (int)info.min_heap);

        /* serial telemetry: keep the format the PC GUI expects */
        printf("@DATA {\"uptime_s\":%" PRIu32 ",\"free_heap\":%zu,\"min_heap\":%zu,"
               "\"ap_count\":%d,\"best_rssi\":%d,\"best_channel\":%d,\"revision\":\"%s\"}\n",
               info.uptime_s, info.free_heap, info.min_heap, info.ap_count,
               info.best_rssi, info.best_channel, info.revision);
        fflush(stdout);

        /* periodic wifi rescan */
        if (xTaskGetTickCount() - last_wifi_scan > pdMS_TO_TICKS(30000)) {
            last_wifi_scan = xTaskGetTickCount();
            if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
                wifi_page_set_scanning(true);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---------------- BOOT key: short press = page, long press = orientation ------ */
#define BOOT_GPIO GPIO_NUM_0
#define BOOT_LONG_PRESS_MS 1200

static void render_current_page(void)
{
    /* Serialise all drawing: the dashboard task and the BOOT key task both
     * render, and concurrent frame-buffer writes produce garbage on the
     * panel. */
    if (g_render_mutex) {
        xSemaphoreTake(g_render_mutex, portMAX_DELAY);
    }
    if (g_page == PAGE_WIFI) {
        render_wifi_page();
    } else {
        render_dashboard();
    }
    if (g_render_mutex) {
        xSemaphoreGive(g_render_mutex);
    }
}

/*
 * Debug helper for @CMD PAGE.
 *
 * Prints the exact text the Wi-Fi page draws, one line per row, so the
 * host can compare it against what is on the panel. Characters that are
 * not in the CJK font are drawn as '?' by the renderer; they are marked
 * here with a trailing note so a missing glyph is visible in the log
 * instead of only on the screen.
 */
static void page_report_line(const char *label, const char *text)
{
    char missing[64];
    size_t miss_len = 0;
    missing[0] = '\0';
    const char *p = text;
    while (*p && miss_len + 8 < sizeof(missing)) {
        uint32_t cp = 0;
        int len = 0;
        if (!tft_utf8_decode(p, &cp, &len)) {
            p++;
            continue;
        }
        if (cp >= 0x80) {
            const uint8_t *g = NULL;
            if (!tft_cjk_glyph(cp, &g)) {
                /* record the code point as UTF-8 */
                if (cp < 0x800) {
                    missing[miss_len++] = (char)(0xC0 | (cp >> 6));
                    missing[miss_len++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    missing[miss_len++] = (char)(0xE0 | (cp >> 12));
                    missing[miss_len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    missing[miss_len++] = (char)(0x80 | (cp & 0x3F));
                }
                missing[miss_len] = '\0';
            }
        }
        p += len;
    }
    /* Escape the missing-glyph code points so the reply is valid JSON
     * even when they are multi-byte UTF-8. */
    char miss_hex[256];
    size_t hx = 0;
    for (size_t i = 0; i < miss_len && hx + 8 < sizeof(miss_hex); i++) {
        hx += (size_t)snprintf(miss_hex + hx, sizeof(miss_hex) - hx, "%02X",
                               (unsigned char)missing[i]);
    }
    miss_hex[hx] = '\0';

    printf("@PAGE {\"row\":\"%s\",\"text\":\"%s\",\"missing\":\"%s\"}\n",
           label, text, miss_hex);
    fflush(stdout);
}

static void dump_page_text(void)
{
    wifi_page_state_t st;
    wifi_page_get(&st);

    char buf[64];
    char ssid[40];

    page_report_line("title", "Wi-Fi 网络列表");
    if (st.connected) {
        snprintf(buf, sizeof(buf), "已连接：%s", st.connected_ssid);
        page_report_line("status", buf);
        if (st.ip[0]) {
            snprintf(buf, sizeof(buf), "IP %s", st.ip);
            page_report_line("ip", buf);
        }
    } else {
        snprintf(buf, sizeof(buf), "状态：%s", st.status);
        page_report_line("status", buf);
    }
    page_report_line("hdr_ssid", "名称");
    page_report_line("hdr_sec", "加密");
    page_report_line("hdr_sig", "信号");
    page_report_line("hdr_ch", "信道");

    int row = 0;
    for (int i = 0; i < st.count; i++) {
        const wifi_page_ap_t *ap = &st.aps[i];
        tft_clip_text_cjk(ssid, sizeof(ssid), ap->ssid, WIFI_SSID_PX);
        char line[128];
        snprintf(line, sizeof(line), "%s | %s | ch%d", ssid,
                 wifi_auth_label_cjk(ap->auth), ap->channel);
        char label[16];
        snprintf(label, sizeof(label), "row%d", row++);
        page_report_line(label, line);
    }

    snprintf(buf, sizeof(buf), "共发现 %d 个网络", st.total);
    page_report_line("footer", buf);
    page_report_line("hint", "BOOT键：切换页面/方向");
}


static void backlight_on(void)
{
#if TFT_PIN_BACKLIGHT >= 0
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << TFT_PIN_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(TFT_PIN_BACKLIGHT, 1));
#endif
}

static void tft_bringup(void)
{
    /* SPI bus */
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = TFT_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = TFT_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TFT_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* panel IO */
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = TFT_PIN_CS,
        .dc_gpio_num = TFT_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = TFT_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_high_on_cmd = 0,
            .dc_low_on_data = 0,
            .dc_low_on_param = 0,
        },
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(TFT_SPI_HOST, &io_cfg, &io));

    /* panel */
    ili9341_vendor_config_t vc = {
        .x_gap = 0,
        .y_gap = 0,
        .use_rgb666 = false,
        .madctl_mirror_x = false,
        .madctl_mirror_y = false,
        .madctl_swap_xy = false,
#ifdef CONFIG_TFT_BGR
        .madctl_bgr = true,
#else
        .madctl_bgr = false,
#endif
#ifdef CONFIG_TFT_INVERT_COLOR
        .invert_color = true,
#else
        .invert_color = false,
#endif
        .use_hw_reset = true,
        .gamma = 0,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TFT_PIN_RST,
        #ifdef CONFIG_TFT_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
        .vendor_config = &vc,
        .flags = {
            .reset_active_high = false,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &g_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));

    /* orientation: the MX bit is mandatory for portrait mode */
    apply_orientation(0);

    ESP_ERROR_CHECK(esp_lcd_panel_set_brightness(g_panel, 255));
    backlight_on();

    /* drawing surface */
    ESP_ERROR_CHECK(tft_disp_create(g_panel, FB_WIDTH, FB_HEIGHT, &g_disp));

    /* Read the controller ID so we can tell a real ILI9341 from a clone. */
    uint8_t id[4] = {0};
    if (ili9341_read_display_id(g_panel, id, sizeof(id)) == ESP_OK) {
        ESP_LOGI(TAG, "display id: %02X %02X %02X %02X", id[0], id[1], id[2], id[3]);
    } else {
        ESP_LOGW(TAG, "could not read display id (no MISO wired?)");
    }

    ESP_LOGI(TAG, "tft ready: %dx%d, pclk=%d Hz", FB_WIDTH, FB_HEIGHT, TFT_PCLK_HZ);

    /* show a splash so the panel is not blank while the first samples arrive */
    draw_splash();
}

/* ---------------- boot splash ---------------- */
static void draw_splash(void)
{
    tft_clear(g_disp, TFT_COLOR_BLACK);

    /* title bar */
    tft_fill_rect(g_disp, 0, 0, FB_WIDTH, 40, TFT_COLOR_DARKGRAY);
    tft_draw_text(g_disp, 6, 8, "ESP32 TFT DASHBOARD", TFT_COLOR_CYAN, TFT_FONT_SMALL, 1);
    tft_draw_hline(g_disp, 0, 40, FB_WIDTH, TFT_COLOR_GRAY, 1);

    /* colour bars - also a sanity check of the R/B channel order */
    const uint16_t bars[] = {
        TFT_COLOR_RED, TFT_COLOR_GREEN, TFT_COLOR_BLUE,
        TFT_COLOR_YELLOW, TFT_COLOR_MAGENTA, TFT_COLOR_CYAN,
        TFT_COLOR_WHITE, TFT_COLOR_BLACK,
    };
    const int n = (int)(sizeof(bars) / sizeof(bars[0]));
    int bw = FB_WIDTH / n;
    for (int i = 0; i < n; i++) {
        tft_fill_rect(g_disp, i * bw, 46, bw, 40, bars[i]);
    }

    tft_draw_text(g_disp, 8, 100, "ILI9341 240x320", TFT_COLOR_WHITE, TFT_FONT_SMALL, 1);
    tft_draw_text(g_disp, 8, 116, "SPI2 40MHz", TFT_COLOR_LIGHTGRAY, TFT_FONT_SMALL, 1);

    /* Asymmetric marker: a triangle on the left and a square on the right.
     * If these appear swapped, the screen is mirrored horizontally. */
    for (int r = 0; r < 14; r++) {
        int half = 7 - r;
        tft_fill_rect(g_disp, 12 + half, 132 + r, half * 2, 1, TFT_COLOR_WHITE);
    }
    tft_fill_rect(g_disp, FB_WIDTH - 26, 132, 14, 14, TFT_COLOR_WHITE);

    tft_draw_text_center(g_disp, 0, 150, FB_WIDTH, 40, "STARTING...", TFT_COLOR_YELLOW,
                         TFT_FONT_MEDIUM, 1);

    tft_draw_hline(g_disp, 0, FB_HEIGHT - 46, FB_WIDTH, TFT_COLOR_GRAY, 1);
    tft_draw_text(g_disp, 6, FB_HEIGHT - 36, "ESP-IDF v6.1", TFT_COLOR_LIGHTGRAY,
                  TFT_FONT_SMALL, 1);

    tft_flush(g_disp);
}


/* ---------------- dashboard rendering ---------------- */
#define HIST_LEN 60
static int s_hist_free[HIST_LEN];
static int s_hist_min[HIST_LEN];
static int s_hist_idx;
static int s_hist_count;

static void hist_push(int v_free, int v_min)
{
    s_hist_free[s_hist_idx] = v_free;
    s_hist_min[s_hist_idx] = v_min;
    s_hist_idx = (s_hist_idx + 1) % HIST_LEN;
    if (s_hist_count < HIST_LEN) {
        s_hist_count++;
    }
}

static void draw_header(void)
{
    tft_fill_rect(g_disp, 0, 0, FB_WIDTH, 34, TFT_COLOR_DARKGRAY);
    tft_draw_text_cjk(g_disp, 6, 9, "ESP32 开发板监控", TFT_COLOR_CYAN, 1);
    tft_draw_hline(g_disp, 0, 34, FB_WIDTH, TFT_COLOR_GRAY, 1);
}

static void draw_stat_box(int x, int y, int w, int h, const char *label, const char *value,
                          uint16_t value_color)
{
    tft_draw_rect(g_disp, x, y, w, h, TFT_COLOR_GRAY, 1);
    tft_draw_text_cjk(g_disp, x + 5, y + 3, label, TFT_COLOR_LIGHTGRAY, 1);
    tft_draw_text(g_disp, x + 5, y + 19, value, value_color, TFT_FONT_MEDIUM, 1);
}

static void render_dashboard(void)
{
    board_info_t info;
    xSemaphoreTake(g_info_mutex, portMAX_DELAY);
    info = g_info;
    xSemaphoreGive(g_info_mutex);

    char buf[32];

    tft_clear(g_disp, TFT_COLOR_BLACK);
    draw_header();

    /* stat cards */
    snprintf(buf, sizeof(buf), "%" PRIu32 "s", info.uptime_s);
    draw_stat_box(4, 40, 108, 38, "运行时间", buf, TFT_COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%zu KB", info.free_heap / 1024);
    draw_stat_box(128, 40, 108, 38, "可用堆", buf, TFT_COLOR_GREEN);

    snprintf(buf, sizeof(buf), "%zu KB", info.min_heap / 1024);
    draw_stat_box(4, 82, 108, 38, "最低堆", buf, TFT_COLOR_ORANGE);

    snprintf(buf, sizeof(buf), "%d AP", info.ap_count);
    draw_stat_box(128, 82, 108, 38, "Wi-Fi 数量", buf, TFT_COLOR_CYAN);

    /* heap chart */
    const int chart_x = 4, chart_y = 128, chart_w = FB_WIDTH - 8, chart_h = 100;
    tft_draw_rect(g_disp, chart_x, chart_y, chart_w, chart_h, TFT_COLOR_GRAY, 1);
    tft_draw_text_cjk(g_disp, chart_x + 4, chart_y - 12, "堆内存趋势", TFT_COLOR_LIGHTGRAY, 1);

    if (s_hist_count >= 2) {
        /* scale: dynamic, based on the current window */
        int vmax = 0, vmin = 1 << 30;
        for (int i = 0; i < s_hist_count; i++) {
            int idx = (s_hist_idx - s_hist_count + i + HIST_LEN) % HIST_LEN;
            if (s_hist_free[idx] > vmax) vmax = s_hist_free[idx];
            if (s_hist_free[idx] < vmin) vmin = s_hist_free[idx];
            if (s_hist_min[idx] > vmax) vmax = s_hist_min[idx];
            if (s_hist_min[idx] < vmin) vmin = s_hist_min[idx];
        }
        if (vmax - vmin < 1024) {
            vmax = vmin + 1024;
        }
        int range = vmax - vmin;

        int free_pts[HIST_LEN], min_pts[HIST_LEN];
        for (int i = 0; i < s_hist_count; i++) {
            int idx = (s_hist_idx - s_hist_count + i + HIST_LEN) % HIST_LEN;
            free_pts[i] = (s_hist_free[idx] - vmin) * (chart_h - 2) / range;
            min_pts[i]  = (s_hist_min[idx]  - vmin) * (chart_h - 2) / range;
        }
        /* chart values are measured from the bottom, flip for the screen */
        for (int i = 0; i < s_hist_count; i++) {
            free_pts[i] = chart_h - 2 - free_pts[i];
            min_pts[i]  = chart_h - 2 - min_pts[i];
        }
        tft_draw_chart(g_disp, min_pts, s_hist_count, chart_x + 1, chart_y + 1,
                       chart_w - 2, chart_h - 2, TFT_COLOR_ORANGE, false);
        tft_draw_chart(g_disp, free_pts, s_hist_count, chart_x + 1, chart_y + 1,
                       chart_w - 2, chart_h - 2, TFT_COLOR_GREEN, false);
    } else {
        tft_draw_text_cjk(g_disp, chart_x + 10, chart_y + 40, "正在采集…", TFT_COLOR_GRAY, 1);
    }

    /* legend */
    tft_fill_rect(g_disp, chart_x + 2, chart_y + chart_h - 12, 10, 3, TFT_COLOR_GREEN);
    tft_draw_text_cjk(g_disp, chart_x + 16, chart_y + chart_h - 17, "可用", TFT_COLOR_GREEN, 1);
    tft_fill_rect(g_disp, chart_x + 60, chart_y + chart_h - 12, 10, 3, TFT_COLOR_ORANGE);
    tft_draw_text_cjk(g_disp, chart_x + 74, chart_y + chart_h - 17, "最低", TFT_COLOR_ORANGE, 1);

    /* bottom info */
    tft_draw_hline(g_disp, 0, FB_HEIGHT - 42, FB_WIDTH, TFT_COLOR_GRAY, 1);
    snprintf(buf, sizeof(buf), "RSSI %d dBm  CH %d", info.best_rssi, info.best_channel);
    tft_draw_text(g_disp, 6, FB_HEIGHT - 38, buf, TFT_COLOR_LIGHTGRAY, TFT_FONT_SMALL, 1);

    snprintf(buf, sizeof(buf), "REV %s", info.revision);
    tft_draw_text(g_disp, 6, FB_HEIGHT - 24, buf, TFT_COLOR_LIGHTGRAY, TFT_FONT_SMALL, 1);

    snprintf(buf, sizeof(buf), "HEAP %zu/%zu KB", info.min_heap / 1024, info.free_heap / 1024);
    tft_draw_text(g_disp, 130, FB_HEIGHT - 38, buf, TFT_COLOR_YELLOW, TFT_FONT_SMALL, 1);

    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             (int)(info.uptime_s / 3600), (int)((info.uptime_s % 3600) / 60),
             (int)(info.uptime_s % 60));
    tft_draw_text(g_disp, 130, FB_HEIGHT - 24, buf, TFT_COLOR_CYAN, TFT_FONT_SMALL, 1);

    ESP_ERROR_CHECK(tft_flush(g_disp));
}

/* ---------------- serial command interface ----------------
 *
 * Commands are line based and start with "@CMD ":
 *   @CMD SCAN                       start a Wi-Fi scan
 *   @CMD CONNECT <ssid> [password]  join a network (open network if no password)
 *   @CMD DISCONNECT                 leave the current network
 *
 * Replies use "@WIFI ..." so the PC side GUI can pick them up.
 * The password is never echoed back.
 */

static void wifi_reply(const char *fmt, ...)
{
    /* Static buffer: the heap is tight (no PSRAM) and this must not fail. */
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("@WIFI %s\n", buf);
    fflush(stdout);
}

/*
 * Reply with the cached list so the GUI can show what the screen shows.
 *
 * The whole JSON is built in one buffer and printed as a single line - the
 * GUI parses it with json.loads(), so it must not be split across lines.
 */
static void wifi_reply_list(void)
{
    wifi_page_state_t st;
    wifi_page_get(&st);

    /* Worst case: 12 entries x ~96 bytes + a small header. */
    static char buf[1600];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"list\",\"total\":%d,\"count\":%d,"
                     "\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"aps\":[",
                     st.total, st.count, st.connected ? "true" : "false",
                     st.connected_ssid, st.ip);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return;
    }
    size_t used = (size_t)n;

    for (int i = 0; i < st.count; i++) {
        int m = snprintf(buf + used, sizeof(buf) - used,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,"
                         "\"sec\":\"%s\",\"open\":%s}",
                         i ? "," : "", st.aps[i].ssid, st.aps[i].rssi,
                         st.aps[i].channel, wifi_auth_label(st.aps[i].auth),
                         st.aps[i].auth == WIFI_AUTH_OPEN ? "true" : "false");
        if (m < 0 || (size_t)m >= sizeof(buf) - used) {
            break;
        }
        used += (size_t)m;
    }

    if (used + 3 < sizeof(buf)) {
        buf[used++] = ']';
        buf[used++] = '}';
        buf[used] = '\0';
        printf("@WIFI %s\n", buf);
        fflush(stdout);
    }
}

static void wifi_do_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        wifi_reply("{\"type\":\"error\",\"msg\":\"missing ssid\"}");
        return;
    }

    wifi_config_t cfg = { 0 };
    wifi_sta_config_t *sta = &cfg.sta;

    snprintf((char *)sta->ssid, sizeof(sta->ssid), "%s", ssid);
    sta->ssid[sizeof(sta->ssid) - 1] = '\0';

    if (password && password[0]) {
        snprintf((char *)sta->password, sizeof(sta->password), "%s", password);
        sta->password[sizeof(sta->password) - 1] = '\0';
        sta->threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta->password[0] = '\0';
        sta->threshold.authmode = WIFI_AUTH_OPEN;
    }

    /* keep scanning and the dashboard alive while the join is in progress */
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        wifi_page_set_status("config failed");
        wifi_reply("{\"type\":\"error\",\"msg\":\"set_config %d\",\"ssid\":\"%s\"}",
                   (int)err, ssid);
        return;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        wifi_page_set_status("connect failed");
        wifi_reply("{\"type\":\"error\",\"msg\":\"connect %d\",\"ssid\":\"%s\"}",
                   (int)err, ssid);
        return;
    }

    wifi_page_set_connected(ssid, "");
    wifi_page_set_status("connecting");
    ESP_LOGI(TAG, "connecting to '%s' (password %s)", ssid, password && password[0] ? "given" : "none");
    wifi_reply("{\"type\":\"connecting\",\"ssid\":\"%s\"}", ssid);
}

static void wifi_do_disconnect(void)
{
    esp_err_t err = esp_wifi_disconnect();
    wifi_page_clear_connected();
    wifi_page_set_status("disconnected");
    wifi_reply("{\"type\":\"disconnected\",\"err\":%d}", (int)err);
}



static void boot_key_task(void *arg)
{
    gpio_config_t boot = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&boot));

    int last = gpio_get_level(BOOT_GPIO);
    while (1) {
        int lvl = gpio_get_level(BOOT_GPIO);
        if (last == 1 && lvl == 0) {
            /* falling edge: button just pressed */
            TickType_t press_start = xTaskGetTickCount();
            bool long_press = false;

            while (gpio_get_level(BOOT_GPIO) == 0) {
                if ((xTaskGetTickCount() - press_start) > pdMS_TO_TICKS(BOOT_LONG_PRESS_MS)) {
                    long_press = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            if (long_press) {
                /* long press: rotate the screen.
                 * The MADCTL change and the redraw must not interleave with
                 * the dashboard task, otherwise the panel gets the new
                 * orientation with a half-drawn old frame. */
                if (g_render_mutex) {
                    xSemaphoreTake(g_render_mutex, portMAX_DELAY);
                }
                apply_orientation(g_orient + 1);
                if (g_page == PAGE_WIFI) {
                    render_wifi_page();
                } else {
                    render_dashboard();
                }
                if (g_render_mutex) {
                    xSemaphoreGive(g_render_mutex);
                }
                ESP_LOGI(TAG, "long press: orientation -> %d", g_orient);
            } else {
                /* short press: switch page */
                g_page = (g_page == PAGE_DASHBOARD) ? PAGE_WIFI : PAGE_DASHBOARD;
                render_current_page();
                ESP_LOGI(TAG, "short press: page -> %d", g_page);
            }
        }
        last = lvl;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---------------- dashboard task ---------------- */
static void dashboard_task(void *arg)
{
    /* let the panel settle after init */
    vTaskDelay(pdMS_TO_TICKS(300));
    while (1) {
        render_current_page();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_info_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(g_info_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    g_render_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(g_render_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    /* Allocate the TFT frame buffer BEFORE the Wi-Fi driver starts.
     * This ESP32 (rev 1.1) has no PSRAM, so the 240x320x2 = 153600 byte
     * frame buffer must come from internal DRAM. Grabbing it while the heap
     * is still contiguous is the difference between success and ESP_ERR_NO_MEM.
     */
    tft_bringup();

    wifi_page_init();
    wifi_init();

    /* Kick off the first scan right away so the Wi-Fi page is not empty
     * when the user switches to it. */
    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        wifi_page_set_scanning(true);
    }

    xTaskCreatePinnedToCore(telemetry_task, "telemetry", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(dashboard_task, "dashboard", 6144, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(boot_key_task, "bootkey", 2560, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(serial_cmd_task, "serialcmd", 4096, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "dashboard started");
}

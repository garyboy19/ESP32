#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "board_test";

/* ESP32-DevKit 常见接法：板上 LED 走 GPIO2，BOOT 键走 GPIO0。 */
#define LED_GPIO GPIO_NUM_2
#define BOOT_GPIO GPIO_NUM_0

static uint16_t wifi_ap_count;
static int wifi_best_rssi = -127;
static uint8_t wifi_best_channel;

static const char *pass_fail(bool ok)
{
    return ok ? "PASS" : "FAIL";
}

static void print_mac(const char *label, uint8_t mac[6])
{
    ESP_LOGI(TAG, "  %-12s %02x:%02x:%02x:%02x:%02x:%02x",
             label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool test_identity(void)
{
    esp_chip_info_t chip;
    uint32_t flash_bytes = 0;
    uint8_t sta_mac[6] = {0};
    uint8_t bt_mac[6] = {0};
    bool ok = true;

    esp_chip_info(&chip);
    if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK) {
        ESP_LOGE(TAG, "读取 Flash 容量失败");
        ok = false;
        flash_bytes = 0;
    }
    if (esp_read_mac(sta_mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "读取 STA MAC 失败");
        ok = false;
    }
    if (esp_read_mac(bt_mac, ESP_MAC_BT) != ESP_OK) {
        ESP_LOGW(TAG, "读取 BT MAC 失败（若无蓝牙可忽略）");
    }

    ESP_LOGI(TAG, "---------- 芯片 ----------");
    ESP_LOGI(TAG, "  目标         %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "  内核         %d", chip.cores);
    ESP_LOGI(TAG, "  修订         v%d.%d", chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "  Wi-Fi        %s", (chip.features & CHIP_FEATURE_WIFI_BGN) ? "yes" : "no");
    ESP_LOGI(TAG, "  BT/BLE       %s%s",
             (chip.features & CHIP_FEATURE_BT) ? "BT " : "",
             (chip.features & CHIP_FEATURE_BLE) ? "BLE" : "");
    ESP_LOGI(TAG, "  Flash        %" PRIu32 " MB %s",
             flash_bytes / (1024 * 1024),
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    ESP_LOGI(TAG, "  空闲堆       %" PRIu32 " bytes", (uint32_t)esp_get_free_heap_size());
    print_mac("STA MAC", sta_mac);
    print_mac("BT MAC", bt_mac);

    if (chip.cores < 1 || flash_bytes < 1 * 1024 * 1024) {
        ok = false;
    }
    ESP_LOGI(TAG, "[%s] 芯片 / Flash / MAC", pass_fail(ok));
    return ok;
}

static bool test_gpio(void)
{
    bool ok = true;

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t boot = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (gpio_config(&led) != ESP_OK || gpio_config(&boot) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO 配置失败");
        return false;
    }

    ESP_LOGI(TAG, "---------- GPIO ----------");
    ESP_LOGI(TAG, "  闪烁 GPIO%d（多数开发板板载 LED）", (int)LED_GPIO);
    for (int i = 0; i < 6; ++i) {
        gpio_set_level(LED_GPIO, i % 2);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    gpio_set_level(LED_GPIO, 0);

    int boot_level = gpio_get_level(BOOT_GPIO);
    ESP_LOGI(TAG, "  BOOT(GPIO%d) 当前电平 = %d（未按时通常为 1）", (int)BOOT_GPIO, boot_level);
    if (boot_level == 0) {
        ESP_LOGW(TAG, "  BOOT 键处于按下或被拉低，下载模式相关电路可能卡住");
        ok = false;
    }
    ESP_LOGI(TAG, "[%s] GPIO 输出/输入", pass_fail(ok));
    return ok;
}

static bool test_wifi_scan(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    uint16_t ap_num = 0;
    bool ok = true;

    ESP_LOGI(TAG, "---------- Wi-Fi ----------");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 扫描启动失败: %s", esp_err_to_name(err));
        return false;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_num));
    wifi_ap_count = ap_num;
    ESP_LOGI(TAG, "  扫描到 %u 个 AP", ap_num);

    uint16_t show = ap_num > 8 ? 8 : ap_num;
    wifi_ap_record_t records[8] = {0};
    if (show > 0) {
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&show, records));
        wifi_best_rssi = records[0].rssi;
        wifi_best_channel = records[0].primary;
        for (uint16_t i = 0; i < show; ++i) {
            ESP_LOGI(TAG, "  [%u] rssi=%4d ch=%2d  %s",
                     i + 1, records[i].rssi, records[i].primary, records[i].ssid);
        }
    } else {
        ESP_LOGW(TAG, "  未扫到热点：射频可能异常，或周围确实没有 Wi-Fi");
        ok = false;
    }

    ESP_LOGI(TAG, "[%s] Wi-Fi 扫描", pass_fail(ok));
    return ok;
}

void app_main(void)
{
    int passed = 0;
    int total = 3;
    esp_chip_info_t chip;
    uint32_t flash_bytes = 0;
    esp_chip_info(&chip);
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_bytes));

    ESP_LOGI(TAG, "ESP32 开发板自检开始");

    if (test_identity()) {
        passed++;
    }
    if (test_gpio()) {
        passed++;
    }
    if (test_wifi_scan()) {
        passed++;
    }

    ESP_LOGI(TAG, "========== 结果 %d/%d %s ==========",
             passed, total, passed == total ? "全部通过" : "存在失败项");
    ESP_LOGI(TAG, "进入循环：LED 心跳；按住 BOOT 键会在日志里看到按下");

    bool last_boot = gpio_get_level(BOOT_GPIO);
    int64_t start_us = esp_timer_get_time();
    int heartbeat = 0;
    while (1) {
        bool led_on = (heartbeat++ % 2) == 0;
        gpio_set_level(LED_GPIO, led_on);

        int boot_level = gpio_get_level(BOOT_GPIO);
        if (boot_level != last_boot) {
            ESP_LOGI(TAG, "BOOT 键 %s", boot_level == 0 ? "按下" : "松开");
            last_boot = boot_level;
        }

        int64_t uptime_s = (esp_timer_get_time() - start_us) / 1000000;
        printf("@DATA {\"uptime_s\":%lld,\"free_heap\":%" PRIu32
               ",\"min_heap\":%" PRIu32 ",\"boot\":%d,\"led\":%d"
               ",\"ap_count\":%u,\"best_rssi\":%d,\"best_channel\":%u"
               ",\"cores\":%d,\"revision\":\"%d.%d\",\"flash_mb\":%" PRIu32
               ",\"wifi\":%s,\"bt\":%s}\n",
               (long long)uptime_s, (uint32_t)esp_get_free_heap_size(),
               (uint32_t)esp_get_minimum_free_heap_size(), boot_level, led_on,
               (unsigned)wifi_ap_count, wifi_best_rssi, (unsigned)wifi_best_channel, chip.cores,
               chip.revision / 100, chip.revision % 100,
               flash_bytes / (1024 * 1024),
               (chip.features & CHIP_FEATURE_WIFI_BGN) ? "true" : "false",
               (chip.features & CHIP_FEATURE_BT) ? "true" : "false");
        fflush(stdout);
        if (heartbeat % 10 == 0) {
            ESP_LOGI(TAG, "心跳 %lld s  空闲堆 %" PRIu32,
                     (long long)uptime_s, (uint32_t)esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

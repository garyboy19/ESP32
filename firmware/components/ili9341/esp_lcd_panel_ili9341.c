/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ILI9341 panel implementation for the esp_lcd panel interface.
 * Transport (SPI) is provided by the generic esp_lcd_panel_io layer.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_ili9341.h"

static const char *TAG = "lcd_panel.ili9341";

/* ---------------- ILI9341 command set ---------------- */
#define ILI9341_CMD_SWRESET      0x01
#define ILI9341_CMD_RDID         0x04
#define ILI9341_CMD_SLPOUT       0x11
#define ILI9341_CMD_INVOFF       0x20
#define ILI9341_CMD_INVON        0x21
#define ILI9341_CMD_DISPON       0x29
#define ILI9341_CMD_CASET        0x2A
#define ILI9341_CMD_RASET        0x2B
#define ILI9341_CMD_RAMWR        0x2C
#define ILI9341_CMD_MADCTL       0x36
#define ILI9341_CMD_COLMOD       0x3A
#define ILI9341_CMD_DISPOFF      0x28
#define ILI9341_CMD_SLPIN        0x10
#define ILI9341_CMD_GAMCT        0x26
#define ILI9341_CMD_WRGBCTL      0xB0
#define ILI9341_CMD_FRMCTR1      0xB1
#define ILI9341_CMD_FRMCTR2      0xB2
#define ILI9341_CMD_FRMCTR3      0xB3
#define ILI9341_CMD_PWR1         0xC0
#define ILI9341_CMD_PWR2         0xC1
#define ILI9341_CMD_PWR3         0xC2
#define ILI9341_CMD_PWR4         0xC3
#define ILI9341_CMD_PWR7         0xC7
#define ILI9341_CMD_VMCTR1       0xC5
#define ILI9341_CMD_VMCTR2       0xC6
#define ILI9341_CMD_VMCTR3       0xC7
#define ILI9341_CMD_PCLKEN       0xC9
#define ILI9341_CMD_VCOMS        0xDB
#define ILI9341_CMD_DFUNCTR      0xE0
#define ILI9341_CMD_IDMOFF       0x38
#define ILI9341_CMD_IDMON        0x39

/* MADCTL bits (same layout as the MIPI-DBI standard) */
#define ILI9341_MADCTL_MY  0x80
#define ILI9341_MADCTL_MX  0x40
#define ILI9341_MADCTL_MV  0x20
#define ILI9341_MADCTL_ML  0x10
#define ILI9341_MADCTL_BGR 0x08
#define ILI9341_MADCTL_MH  0x04

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    gpio_num_t reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;   /* 16 or 24 (18bpp stored in 3 bytes) */
    uint8_t madctl_val;
    uint8_t colmod_val;
    bool inverted;
} ili9341_panel_t;

static esp_err_t panel_ili9341_del(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9341_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9341_init(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9341_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data);
static esp_err_t panel_ili9341_invert_color(esp_lcd_panel_t *panel, bool invert);
static esp_err_t panel_ili9341_mirror(esp_lcd_panel_t *panel, bool mx, bool my);
static esp_err_t panel_ili9341_swap_xy(esp_lcd_panel_t *panel, bool swap);
static esp_err_t panel_ili9341_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_ili9341_set_brightness(esp_lcd_panel_t *panel, int brightness);
static esp_err_t panel_ili9341_disp_on_off(esp_lcd_panel_t *panel, bool off);
static esp_err_t panel_ili9341_disp_sleep(esp_lcd_panel_t *panel, bool sleep);

#define ILI9341_CHECK(io, cmd) \
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, (cmd), NULL, 0), TAG, "tx cmd 0x%02X failed", (cmd))

static esp_err_t ili9341_write_cmd_param(esp_lcd_panel_io_handle_t io, int cmd,
                                         const void *param, size_t param_size)
{
    return esp_lcd_panel_io_tx_param(io, cmd, param, param_size);
}

esp_err_t esp_lcd_new_panel_ili9341(const esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    ili9341_panel_t *panel = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");

    panel = calloc(1, sizeof(ili9341_panel_t));
    ESP_GOTO_ON_FALSE(panel, ESP_ERR_NO_MEM, err, TAG, "no mem for ili9341 panel");

    /* --- vendor specific options --- */
    const ili9341_vendor_config_t *vc =
        (const ili9341_vendor_config_t *)panel_dev_config->vendor_config;
    ili9341_vendor_config_t vend = {
        .x_gap = 0, .y_gap = 0,
        .use_rgb666 = false,
        .madctl_mirror_x = false, .madctl_mirror_y = false, .madctl_swap_xy = false,
        .madctl_bgr = false, .invert_color = false, .use_hw_reset = true, .gamma = 0,
    };
    if (vc) {
        vend = *vc;
    }
    if (vend.gamma > 3) {
        vend.gamma = 0;
    }

    /* --- color format --- */
    uint8_t fb_bpp = 0;
    if (panel_dev_config->bits_per_pixel == 16) {
        panel->colmod_val = 0x55;                 /* 16bpp, RGB565 */
        fb_bpp = 16;
    } else if (panel_dev_config->bits_per_pixel == 18) {
        panel->colmod_val = 0x66;                 /* 18bpp, RGB666 */
        fb_bpp = 24;                              /* stored as 3 bytes per pixel */
    } else if (vend.use_rgb666) {
        panel->colmod_val = 0x66;
        fb_bpp = 24;
    } else {
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width %u",
                          (unsigned)panel_dev_config->bits_per_pixel);
    }

    /* --- MADCTL --- */
    uint8_t madctl = 0;
    if (vend.madctl_mirror_x) madctl |= ILI9341_MADCTL_MX;
    if (vend.madctl_mirror_y) madctl |= ILI9341_MADCTL_MY;
    if (vend.madctl_swap_xy)  madctl |= ILI9341_MADCTL_MV;
    if (vend.madctl_bgr || panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR) {
        madctl |= ILI9341_MADCTL_BGR;
    }
    panel->madctl_val = madctl;

    /* --- reset pin --- */
    panel->reset_gpio_num = panel_dev_config->reset_gpio_num;
    panel->reset_level = panel_dev_config->flags.reset_active_high;
    if (panel->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << panel->reset_gpio_num,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    } else {
        vend.use_hw_reset = false;
    }

    panel->io = io;
    panel->fb_bits_per_pixel = fb_bpp;
    panel->x_gap = vend.x_gap;
    panel->y_gap = vend.y_gap;
    panel->inverted = vend.invert_color;

    panel->base.del = panel_ili9341_del;
    panel->base.reset = panel_ili9341_reset;
    panel->base.init = panel_ili9341_init;
    panel->base.draw_bitmap = panel_ili9341_draw_bitmap;
    panel->base.invert_color = panel_ili9341_invert_color;
    panel->base.mirror = panel_ili9341_mirror;
    panel->base.swap_xy = panel_ili9341_swap_xy;
    panel->base.set_gap = panel_ili9341_set_gap;
    panel->base.set_brightness = panel_ili9341_set_brightness;
    panel->base.disp_on_off = panel_ili9341_disp_on_off;
    panel->base.disp_sleep = panel_ili9341_disp_sleep;

    *ret_panel = &(panel->base);
    ESP_LOGD(TAG, "new ili9341 panel @%p, bpp=%u", panel, (unsigned)fb_bpp);
    return ESP_OK;

err:
    if (panel) {
        if (panel->reset_gpio_num >= 0) {
            gpio_reset_pin(panel->reset_gpio_num);
        }
        free(panel);
    }
    return ret;
}

static esp_err_t panel_ili9341_del(esp_lcd_panel_t *panel)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    free(p);
    return ESP_OK;
}

static esp_err_t panel_ili9341_reset(esp_lcd_panel_t *panel)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    if (p->reset_gpio_num >= 0) {
        /* hardware reset: assert low (or high if active high), wait, deassert */
        gpio_set_level(p->reset_gpio_num, p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(p->reset_gpio_num, !p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ILI9341_CHECK(io, ILI9341_CMD_SWRESET);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_ili9341_init(esp_lcd_panel_t *panel)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    /* display off while we configure */
    ILI9341_CHECK(io, ILI9341_CMD_DISPOFF);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* power control */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_PWR1,
                     (uint8_t[]){0x00, 0x00}, 2), TAG, "PWR1 failed");
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_PWR2,
                     (uint8_t[]){0x00, 0x00}, 2), TAG, "PWR2 failed");
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_PWR3,
                     (uint8_t[]){0x00, 0x00}, 2), TAG, "PWR3 failed");
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_PWR4,
                     (uint8_t[]){0x00, 0x00}, 2), TAG, "PWR4 failed");
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_PWR7,
                     (uint8_t[]){0x00}, 1), TAG, "PWR7 failed");

    /* frame rate */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_FRMCTR1,
                     (uint8_t[]){0x00, 0x18}, 2), TAG, "FRMCTR1 failed");
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_FRMCTR2,
                     (uint8_t[]){0x00, 0x27}, 2), TAG, "FRMCTR2 failed");
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_FRMCTR3,
                     (uint8_t[]){0x00, 0x22}, 2), TAG, "FRMCTR3 failed");

    /* RGB interface control */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_WRGBCTL,
                     (uint8_t[]){0x40}, 1), TAG, "WRGBCTL failed");

    /* VCOM control */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_VMCTR1,
                     (uint8_t[]){0x0E}, 1), TAG, "VMCTR1 failed");
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_VMCTR2,
                     (uint8_t[]){0x27}, 1), TAG, "VMCTR2 failed");

    /* VCOMS */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_VCOMS,
                     (uint8_t[]){0x0B}, 1), TAG, "VCOMS failed");

    /* pixel clock */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_PCLKEN,
                     (uint8_t[]){0x20}, 1), TAG, "PCLKEN failed");

    /* access control */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_MADCTL,
                     (uint8_t[]){p->madctl_val}, 1), TAG, "MADCTL failed");

    /* pixel format */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_COLMOD,
                     (uint8_t[]){p->colmod_val}, 1), TAG, "COLMOD failed");

    /* gamma */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_GAMCT,
                     (uint8_t[]){0x01}, 1), TAG, "GAMCT failed");

    /* exit sleep mode */
    ILI9341_CHECK(io, ILI9341_CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* optional inversion */
    if (p->inverted) {
        ILI9341_CHECK(io, ILI9341_CMD_INVON);
    } else {
        ILI9341_CHECK(io, ILI9341_CMD_INVOFF);
    }

    /* display on */
    ILI9341_CHECK(io, ILI9341_CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(20));

    return ESP_OK;
}

static esp_err_t panel_ili9341_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    x_start += p->x_gap;
    x_end += p->x_gap;
    y_start += p->y_gap;
    y_end += p->y_gap;

    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_CASET, (uint8_t[]){
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "CASET failed");
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_RASET, (uint8_t[]){
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "RASET failed");

    size_t len = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) * p->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, ILI9341_CMD_RAMWR, color_data, len),
                        TAG, "tx color failed");
    return ESP_OK;
}

static esp_err_t panel_ili9341_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    ILI9341_CHECK(io, invert ? ILI9341_CMD_INVON : ILI9341_CMD_INVOFF);
    p->inverted = invert;
    return ESP_OK;
}

static esp_err_t panel_ili9341_mirror(esp_lcd_panel_t *panel, bool mx, bool my)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    if (mx) {
        p->madctl_val |= ILI9341_MADCTL_MX;
    } else {
        p->madctl_val &= ~ILI9341_MADCTL_MX;
    }
    if (my) {
        p->madctl_val |= ILI9341_MADCTL_MY;
    } else {
        p->madctl_val &= ~ILI9341_MADCTL_MY;
    }
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_MADCTL,
                     (uint8_t[]){p->madctl_val}, 1), TAG, "MADCTL failed");
    return ESP_OK;
}

static esp_err_t panel_ili9341_swap_xy(esp_lcd_panel_t *panel, bool swap)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    if (swap) {
        p->madctl_val |= ILI9341_MADCTL_MV;
    } else {
        p->madctl_val &= ~ILI9341_MADCTL_MV;
    }
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_MADCTL,
                     (uint8_t[]){p->madctl_val}, 1), TAG, "MADCTL failed");
    return ESP_OK;
}

static esp_err_t panel_ili9341_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_ili9341_set_brightness(esp_lcd_panel_t *panel, int brightness)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    if (brightness < 0) {
        brightness = 0;
    }
    if (brightness > 255) {
        brightness = 255;
    }
    /* WRGBCTL: bit7 = brightness control enable */
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, ILI9341_CMD_WRGBCTL,
                     (uint8_t[]){0x40 | 0x80}, 1), TAG, "WRGBCTL failed");
    ESP_RETURN_ON_ERROR(ili9341_write_cmd_param(io, 0x51 /* WDISBV */,
                     (uint8_t[]){(uint8_t)brightness}, 1), TAG, "WDISBV failed");
    return ESP_OK;
}

static esp_err_t panel_ili9341_disp_on_off(esp_lcd_panel_t *panel, bool off)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    ILI9341_CHECK(io, off ? ILI9341_CMD_DISPOFF : ILI9341_CMD_DISPON);
    return ESP_OK;
}

static esp_err_t panel_ili9341_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    ILI9341_CHECK(io, sleep ? ILI9341_CMD_SLPIN : ILI9341_CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

esp_err_t ili9341_read_display_id(esp_lcd_panel_handle_t panel, uint8_t *id, size_t id_size)
{
    ESP_RETURN_ON_FALSE(panel && id && id_size >= 3, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ili9341_panel_t *p = __containerof(panel, ili9341_panel_t, base);
    memset(id, 0, id_size);
    /* RDID returns 4 bytes: 1 dummy byte + 3 ID bytes (0x93 for plain ILI9341) */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(p->io, ILI9341_CMD_RDID, id, 4),
                        TAG, "read display id failed");
    return ESP_OK;
}

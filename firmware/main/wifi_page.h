/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi page state for the ILI9341 dashboard.
 *
 * The scan callback runs in the Wi-Fi driver's task and the renderer runs in
 * the dashboard task, so the state is guarded by a mutex.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PAGE_MAX_APS 12

/* Compact per-AP entry: 33 bytes of SSID + a few small fields. */
typedef struct {
    char     ssid[33];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  auth;        /* wifi_auth_mode_t, kept as a small integer */
} wifi_page_ap_t;

typedef struct {
    wifi_page_ap_t aps[WIFI_PAGE_MAX_APS];
    int            count;      /* number of valid entries in aps[] */
    int            total;      /* total APs seen by the driver */
    bool           scanning;
    bool           connected;
    char           connected_ssid[33];
    char           ip[16];
    char           status[40];
} wifi_page_state_t;

/** @brief Create the mutex and reset the state. Call once at startup. */
void wifi_page_init(void);

/** @brief Copy a snapshot of the current state into @p out */
void wifi_page_get(wifi_page_state_t *out);

/**
 * @brief Store a scan result
 *
 * @param[in] records  Records from esp_wifi_scan_get_ap_records()
 * @param[in] n        Number of records
 * @param[in] total    Total AP count reported by the driver
 */
void wifi_page_store_scan(const wifi_ap_record_t *records, int n, int total);

/** @brief Update the status line shown on the page */
void wifi_page_set_status(const char *status);

/** @brief Mark the scanning flag */
void wifi_page_set_scanning(bool scanning);

/** @brief Record the connected SSID / IP (pass NULL to leave unchanged) */
void wifi_page_set_connected(const char *ssid, const char *ip);

/** @brief Clear the connected SSID / IP (on disconnect) */
void wifi_page_clear_connected(void);

#ifdef __cplusplus
}
#endif

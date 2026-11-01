/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi page state for the ILI9341 dashboard.
 *
 * The scan callback runs in the Wi-Fi driver's task and the renderer runs in
 * the dashboard task, so the state is guarded by a mutex.
 */
#include "wifi_page.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static wifi_page_state_t s_state;
static SemaphoreHandle_t s_mutex;

void wifi_page_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            memset(&s_state, 0, sizeof(s_state));
            snprintf(s_state.status, sizeof(s_state.status), "idle");
            xSemaphoreGive(s_mutex);
        }
    }
}

void wifi_page_get(wifi_page_state_t *out)
{
    if (out == NULL || s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mutex);
}

void wifi_page_store_scan(const wifi_ap_record_t *records, int n, int total)
{
    if (s_mutex == NULL) {
        return;
    }
    if (records == NULL && n > 0) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_state.total = total;
    s_state.scanning = false;
    s_state.count = 0;

    for (int i = 0; i < n && s_state.count < WIFI_PAGE_MAX_APS; i++) {
        const wifi_ap_record_t *r = &records[i];
        /* hidden SSID: skip, it cannot be connected to by name */
        if (r->ssid[0] == '\0') {
            continue;
        }
        wifi_page_ap_t *ap = &s_state.aps[s_state.count];
        memcpy(ap->ssid, r->ssid, sizeof(ap->ssid));
        ap->ssid[sizeof(ap->ssid) - 1] = '\0';
        ap->rssi = r->rssi;
        ap->channel = r->primary;
        ap->auth = (uint8_t)r->authmode;
        s_state.count++;
    }

    xSemaphoreGive(s_mutex);
}

void wifi_page_set_status(const char *status)
{
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_state.status, sizeof(s_state.status), "%s", status ? status : "");
    xSemaphoreGive(s_mutex);
}

void wifi_page_set_scanning(bool scanning)
{
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.scanning = scanning;
    xSemaphoreGive(s_mutex);
}

void wifi_page_set_connected(const char *ssid, const char *ip)
{
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ssid) {
        snprintf(s_state.connected_ssid, sizeof(s_state.connected_ssid), "%s", ssid);
    }
    if (ip) {
        snprintf(s_state.ip, sizeof(s_state.ip), "%s", ip);
    }
    s_state.connected = (ssid && ssid[0] != '\0');
    xSemaphoreGive(s_mutex);
}

void wifi_page_clear_connected(void)
{
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.connected = false;
    s_state.connected_ssid[0] = '\0';
    s_state.ip[0] = '\0';
    xSemaphoreGive(s_mutex);
}

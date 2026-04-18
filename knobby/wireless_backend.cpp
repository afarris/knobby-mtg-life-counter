#include <WiFi.h>
#include <string.h>

extern "C" {
#include "src/wireless_manager.h"
}

/* Track whether a connection attempt is in flight so we can distinguish
 * "connecting" from "never-tried/idle" — Arduino's WiFi.status() returns
 * WL_IDLE_STATUS / WL_DISCONNECTED in both cases otherwise.
 */
static bool attempting = false;

extern "C" void wireless_hw_wifi_on(void)
{
    /* Don't let Arduino persist creds across reboots — we manage our own MRU in NVS. */
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    /* NOTE: WiFi.setSleep(false) is deferred to wireless_hw_wifi_status()
     * once we observe WL_CONNECTED, because calling it while STA is mid-
     * associate trips `wifi:sta is connecting, cannot set config`. */
    attempting = false;
}

extern "C" void wireless_hw_wifi_off(void)
{
    WiFi.disconnect(true, true);   /* wifioff=true, eraseap=true */
    WiFi.mode(WIFI_OFF);
    attempting = false;
}

extern "C" void wireless_hw_wifi_begin(const char *ssid, const char *password)
{
    attempting = true;
    if (password && password[0] != '\0')
        WiFi.begin(ssid, password);
    else
        WiFi.begin(ssid);
}

extern "C" void wireless_hw_wifi_disconnect(void)
{
    WiFi.disconnect(false, false);
    attempting = false;
}

extern "C" int wireless_hw_wifi_status(void)
{
    wl_status_t s = WiFi.status();
    switch (s) {
        case WL_CONNECTED: {
            /* Latch WiFi.setSleep(false) once after association. Disables
             * WIFI_PS_MIN_MODEM so the radio stays fully on — avoids
             * beacon-gated latency/packet-loss that was killing short HTTP
             * round-trips. Idempotent; cheap to call every poll, but we
             * gate it to avoid the "cannot set config" warning during
             * brief reassociations. */
            static bool sleep_configured = false;
            if (!sleep_configured) {
                WiFi.setSleep(false);
                sleep_configured = true;
            }
            attempting = false;
            return WIRELESS_HW_WIFI_CONNECTED;
        }
        case WL_CONNECT_FAILED:
        case WL_NO_SSID_AVAIL:
        case WL_CONNECTION_LOST:
            attempting = false;
            return WIRELESS_HW_WIFI_FAILED;
        default:
            return attempting ? WIRELESS_HW_WIFI_CONNECTING : WIRELESS_HW_WIFI_DISCONNECTED;
    }
}

extern "C" int8_t wireless_hw_wifi_rssi(void)
{
    if (WiFi.status() != WL_CONNECTED) return 0;
    long r = WiFi.RSSI();
    if (r < INT8_MIN) r = INT8_MIN;
    if (r > INT8_MAX) r = INT8_MAX;
    return (int8_t)r;
}

extern "C" uint32_t wireless_hw_wifi_ip(void)
{
    if (WiFi.status() != WL_CONNECTED) return 0;
    return (uint32_t)WiFi.localIP();
}

extern "C" void wireless_hw_wifi_current_ssid(char *out, size_t n)
{
    if (out == NULL || n == 0) return;
    out[0] = '\0';
    if (WiFi.status() != WL_CONNECTED) return;
    String s = WiFi.SSID();
    strncpy(out, s.c_str(), n - 1);
    out[n - 1] = '\0';
}

extern "C" void wireless_hw_wifi_scan_start(void)
{
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false);  /* async, hide hidden APs */
}

extern "C" int wireless_hw_wifi_scan_check(void)
{
    int16_t r = WiFi.scanComplete();
    if (r == WIFI_SCAN_RUNNING) return -1;
    if (r < 0) return 0;             /* error -> treat as empty result */
    return (int)r;
}

extern "C" void wireless_hw_wifi_scan_result(int idx, char *ssid_out, size_t n,
                                              int8_t *rssi_out, bool *secured_out)
{
    if (ssid_out && n > 0) ssid_out[0] = '\0';
    if (rssi_out) *rssi_out = 0;
    if (secured_out) *secured_out = false;

    int16_t count = WiFi.scanComplete();
    if (count < 0 || idx < 0 || idx >= count) return;

    if (ssid_out && n > 0) {
        String s = WiFi.SSID(idx);
        strncpy(ssid_out, s.c_str(), n - 1);
        ssid_out[n - 1] = '\0';
    }
    if (rssi_out) {
        long r = WiFi.RSSI(idx);
        if (r < INT8_MIN) r = INT8_MIN;
        if (r > INT8_MAX) r = INT8_MAX;
        *rssi_out = (int8_t)r;
    }
    if (secured_out) {
        *secured_out = (WiFi.encryptionType(idx) != WIFI_AUTH_OPEN);
    }
}

extern "C" void wireless_hw_wifi_scan_clear(void)
{
    WiFi.scanDelete();
}

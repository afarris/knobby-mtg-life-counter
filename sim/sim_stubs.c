#include "sim_stubs.h"
#include "board_detect.h"
#include "bidi_switch_knob.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/ledc.h"
#include "esp_random.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- sim_millis ---- */

static uint32_t sim_tick_ms = 0;

uint32_t sim_millis(void)
{
    return sim_tick_ms;
}

void sim_tick_advance(uint32_t ms)
{
    sim_tick_ms += ms;
}

/* ---- Board detection ---- */

/* Pin tables come from knobby/board_pins.c (shared with firmware). */

const board_pins_t *board = NULL;

void board_detect(void)
{
    board = &board_k518;
}

/* ---- In-memory NVS store ---- */

#define NVS_MAX_ENTRIES 32
#define NVS_KEY_LEN     16
#define NVS_BLOB_MAX    256

typedef struct {
    char     key[NVS_KEY_LEN];
    int64_t  int_value;
    uint8_t  blob[NVS_BLOB_MAX];
    size_t   blob_len;
    int      has_int;
    int      has_blob;
} nvs_entry_t;

static nvs_entry_t nvs_store[NVS_MAX_ENTRIES];
static int nvs_count = 0;

static nvs_entry_t *nvs_find(const char *key)
{
    int i;
    for (i = 0; i < nvs_count; i++) {
        if (strcmp(nvs_store[i].key, key) == 0)
            return &nvs_store[i];
    }
    return NULL;
}

static nvs_entry_t *nvs_find_or_create(const char *key)
{
    nvs_entry_t *e = nvs_find(key);
    if (e) return e;
    if (nvs_count >= NVS_MAX_ENTRIES) return NULL;
    e = &nvs_store[nvs_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->key, NVS_KEY_LEN, "%s", key);
    return e;
}

void sim_nvs_preset_i8(const char *key, int8_t value)
{
    nvs_entry_t *e = nvs_find_or_create(key);
    if (e) { e->int_value = value; e->has_int = 1; }
}

void sim_nvs_preset_i16(const char *key, int16_t value)
{
    nvs_entry_t *e = nvs_find_or_create(key);
    if (e) { e->int_value = value; e->has_int = 1; }
}

void sim_nvs_preset_u32(const char *key, uint32_t value)
{
    nvs_entry_t *e = nvs_find_or_create(key);
    if (e) { e->int_value = value; e->has_int = 1; }
}

/* NVS API stubs */

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }

esp_err_t nvs_open(const char *namespace_name, int open_mode, nvs_handle_t *out_handle)
{
    (void)namespace_name;
    (void)open_mode;
    if (out_handle) *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_get_i8(nvs_handle_t handle, const char *key, int8_t *out_value)
{
    nvs_entry_t *e;
    (void)handle;
    e = nvs_find(key);
    if (e && e->has_int) { *out_value = (int8_t)e->int_value; return ESP_OK; }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_get_i16(nvs_handle_t handle, const char *key, int16_t *out_value)
{
    nvs_entry_t *e;
    (void)handle;
    e = nvs_find(key);
    if (e && e->has_int) { *out_value = (int16_t)e->int_value; return ESP_OK; }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    nvs_entry_t *e;
    (void)handle;
    e = nvs_find(key);
    if (e && e->has_blob) {
        size_t copy = (e->blob_len < *length) ? e->blob_len : *length;
        memcpy(out_value, e->blob, copy);
        *length = copy;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_set_i8(nvs_handle_t handle, const char *key, int8_t value)
{
    nvs_entry_t *e;
    (void)handle;
    e = nvs_find_or_create(key);
    if (e) { e->int_value = value; e->has_int = 1; return ESP_OK; }
    return ESP_FAIL;
}

esp_err_t nvs_set_i16(nvs_handle_t handle, const char *key, int16_t value)
{
    nvs_entry_t *e;
    (void)handle;
    e = nvs_find_or_create(key);
    if (e) { e->int_value = value; e->has_int = 1; return ESP_OK; }
    return ESP_FAIL;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    nvs_entry_t *e;
    (void)handle;
    e = nvs_find_or_create(key);
    if (e && length <= NVS_BLOB_MAX) {
        memcpy(e->blob, value, length);
        e->blob_len = length;
        e->has_blob = 1;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }

/* ---- LEDC (brightness PWM) ---- */

esp_err_t ledc_timer_config(const ledc_timer_config_t *c) { (void)c; return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *c) { (void)c; return ESP_OK; }
esp_err_t ledc_set_duty(ledc_mode_t m, ledc_channel_t ch, uint32_t d) { (void)m; (void)ch; (void)d; return ESP_OK; }
esp_err_t ledc_update_duty(ledc_mode_t m, ledc_channel_t ch) { (void)m; (void)ch; return ESP_OK; }

/* ---- Rotary encoder ---- */

knob_handle_t iot_knob_create(const knob_config_t *config)
{
    (void)config;
    return (knob_handle_t)1; /* non-NULL dummy */
}

esp_err_t iot_knob_register_cb(knob_handle_t h, knob_event_t e, knob_cb_t cb, void *d)
{
    (void)h; (void)e; (void)cb; (void)d;
    return ESP_OK;
}
esp_err_t iot_knob_unregister_cb(knob_handle_t h, knob_event_t e) { (void)h; (void)e; return ESP_OK; }
knob_event_t iot_knob_get_event(knob_handle_t h) { (void)h; return KNOB_NONE; }
int iot_knob_get_count_value(knob_handle_t h) { (void)h; return 0; }
esp_err_t iot_knob_clear_count_value(knob_handle_t h) { (void)h; return ESP_OK; }
esp_err_t iot_knob_resume(void) { return ESP_OK; }
esp_err_t iot_knob_stop(void) { return ESP_OK; }
esp_err_t knob_gpio_init(uint32_t gpio_num) { (void)gpio_num; return ESP_OK; }
esp_err_t knob_gpio_deinit(uint32_t gpio_num) { (void)gpio_num; return ESP_OK; }
uint8_t knob_gpio_get_key_level(void *gpio_num) { (void)gpio_num; return 0; }

/* ---- Battery ---- */

float sim_battery_voltage = 4.0f;

float knob_read_battery_voltage(void) { return sim_battery_voltage; }

/* ---- Display ---- */

void scr_display_on(void) { /* no-op in simulator */ }

/* Physical display rotation: firmware sets the panel's MADCTL flags; the
   sim records the value and the flush callbacks remap pixels to match. */
int sim_display_rotation = 0;

void display_apply_rotation(int rot) { sim_display_rotation = rot & 3; }

/* ---- Random ---- */

uint32_t esp_random(void) { return (uint32_t)rand(); }

/* ---- Table Sync ---- */

/* Radio bridges implemented by knobby/knobby_net.cpp on firmware; the
   simulator has no radio. Firmware sessions are RAM-only, so the fake
   session lives in the sim's NVS store purely as a screenshot fixture:
   --table-sync / --table-session preset it, and Start/Join/Leave behave
   sensibly in the interactive sim (Join succeeds immediately — there is
   no table to join). */
#include "net_sync.h"

static int64_t sim_nvs_value(const char *key)
{
    nvs_entry_t *e = nvs_find(key);
    return (e && e->has_int) ? e->int_value : 0;
}

void net_sync_send_state(void) {}
void net_sync_send_names(void) {}
void net_sync_send_reply(void) {}

int net_sync_start_game(void)
{
    sim_nvs_preset_i8("net_sync", 1);
    sim_nvs_preset_u32("net_sessn", (esp_random() % 9999u) + 1u);
    return 1;
}

int net_sync_join_game(void)
{
    return net_sync_start_game();
}

void net_sync_leave_game(void)
{
    sim_nvs_preset_i8("net_sync", 0);
    sim_nvs_preset_u32("net_sessn", 0);
}

int net_sync_status(void)
{
    if (sim_nvs_value("net_sync") && sim_nvs_value("net_sessn") != 0)
        return NET_SYNC_IN_GAME;
    return NET_SYNC_OFF;
}

int net_sync_code(void)
{
    uint32_t id = (uint32_t)sim_nvs_value("net_sessn");
    return (id != 0) ? (int)(id % 10000u) : -1;
}

/* ---- Shared test fixtures ---- */

/* Populate the event log with random entries (shared by the headless
   and SDL mains so --random-log behaves the same in both). */
#include "damage_log.h"
#include "game.h"

void sim_populate_random_log(void)
{
    int i;
    const uint8_t event_types[] = {LOG_EVT_LIFE, LOG_EVT_CMD_DAMAGE, LOG_EVT_COUNTER};
    /* > LOG_PAGE_SIZE (32) so screenshots exercise the paginated render */
    for (i = 0; i < 40; i++) {
        int player = rand() % 4;
        int delta = (rand() % 20) - 10;
        uint8_t evt = event_types[rand() % 3];
        int source = -1;
        if (delta == 0) delta = 1;
        if (evt == LOG_EVT_CMD_DAMAGE) source = (player + 1) % 4;
        else if (evt == LOG_EVT_COUNTER) { source = rand() % COUNTER_TYPE_COUNT; if (delta < 0) delta = -delta; }
        sim_tick_advance(5000 + (uint32_t)(rand() % 30000));
        damage_log_add(player, delta, evt, source);
    }
}

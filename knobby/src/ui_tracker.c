#include "ui_tracker.h"
#include "tracker_sync.h"
#include "settings.h"
#include "types.h"
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

/* ---------- screens ---------- */
lv_obj_t *screen_tracker_url_entry  = NULL;
lv_obj_t *screen_tracker_roster     = NULL;

/* ---------- URL entry ---------- */
static lv_obj_t *textarea_url      = NULL;
static lv_obj_t *keyboard_url      = NULL;
static lv_obj_t *btn_url_connect   = NULL;

/* ---------- roster screen ---------- */
static lv_obj_t   *roster_list_container = NULL;
static lv_obj_t   *label_roster_hint     = NULL;
static lv_timer_t *roster_poll_timer     = NULL;
static int         roster_last_count     = -1;
static tracker_state_t roster_last_state = TRACKER_DISCONNECTED;

/* ---------- forward decls ---------- */
static void refresh_tracker_roster_ui(void);

/* ---------- list-row helper (same look as WiFi/rename lists) ---------- */

static lv_obj_t *make_row(lv_obj_t *parent,
                          const char *primary, const char *secondary,
                          lv_color_t color,
                          lv_event_cb_t click_cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 300, 42);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (click_cb) lv_obj_add_event_cb(row, click_cb, LV_EVENT_SHORT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, primary);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, 220);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    if (secondary && secondary[0]) {
        lv_obj_t *lbl2 = lv_label_create(row);
        lv_label_set_text(lbl2, secondary);
        lv_obj_set_style_text_color(lbl2, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl2, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return row;
}


/* ============================================================
 *  URL ENTRY screen
 * ============================================================ */

static void event_url_connect(lv_event_t *e)
{
    (void)e;
    if (textarea_url == NULL) return;
    const char *url = lv_textarea_get_text(textarea_url);
    if (url == NULL || url[0] == '\0') return;
    open_tracker_roster_screen(url);
}

static void build_tracker_url_entry_screen(void)
{
    screen_tracker_url_entry = lv_obj_create(NULL);
    lv_obj_set_size(screen_tracker_url_entry, 360, 360);
    lv_obj_set_style_bg_color(screen_tracker_url_entry, lv_color_black(), 0);
    lv_obj_set_style_border_width(screen_tracker_url_entry, 0, 0);
    lv_obj_set_scrollbar_mode(screen_tracker_url_entry, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(screen_tracker_url_entry, 0, 0);
    lv_obj_clear_flag(screen_tracker_url_entry, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen_tracker_url_entry);
    lv_label_set_text(title, "Server URL");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    textarea_url = lv_textarea_create(screen_tracker_url_entry);
    lv_obj_set_size(textarea_url, 300, 44);
    lv_obj_align(textarea_url, LV_ALIGN_TOP_MID, 0, 64);
    lv_textarea_set_max_length(textarea_url, TRACKER_URL_LEN - 1);
    lv_textarea_set_one_line(textarea_url, true);
    lv_textarea_set_placeholder_text(textarea_url, "http://host:port");

    btn_url_connect = make_button(screen_tracker_url_entry, "connect", 110, 40, event_url_connect);
    lv_obj_align(btn_url_connect, LV_ALIGN_TOP_MID, 0, 118);

    keyboard_url = lv_keyboard_create(screen_tracker_url_entry);
    lv_obj_set_size(keyboard_url, 360, 180);
    lv_obj_align(keyboard_url, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard_url, textarea_url);
}

void open_tracker_url_entry_screen(void)
{
    /* Pre-fill with http:// so the user starts after the scheme. If they
     * already have a previous URL cached in tracker_sync, offer that. */
    const char *prior = tracker_server_url();
    lv_textarea_set_text(textarea_url, (prior && prior[0]) ? prior : "http://");
    lv_keyboard_set_textarea(keyboard_url, textarea_url);
    load_screen_if_needed(screen_tracker_url_entry);
}

/* ============================================================
 *  ROSTER screen — shown after tracker_connect.
 *  Waits for the first WS frame to populate the roster.
 * ============================================================ */

extern lv_obj_t *screen_wireless_menu;   /* declared in ui_wireless.h; avoid the include cycle */

static void event_roster_row_click(lv_event_t *e)
{
    int player_id = (int)(intptr_t)lv_event_get_user_data(e);
    tracker_claim_player(player_id);
    load_screen_if_needed(screen_wireless_menu);
}

static void roster_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != screen_tracker_roster) {
        if (roster_poll_timer) lv_timer_pause(roster_poll_timer);
        return;
    }
    tracker_state_t s = tracker_state();
    int c = tracker_roster_count();
    if (s != roster_last_state || c != roster_last_count) {
        refresh_tracker_roster_ui();
        roster_last_state = s;
        roster_last_count = c;
    }
}

static void refresh_tracker_roster_ui(void)
{
    if (roster_list_container == NULL) return;
    lv_obj_clean(roster_list_container);

    tracker_state_t s = tracker_state();
    int count = tracker_roster_count();

    if (s == TRACKER_CONNECTING) {
        lv_label_set_text(label_roster_hint, "Connecting...");
        lv_obj_clear_flag(label_roster_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s == TRACKER_ERROR) {
        lv_label_set_text(label_roster_hint, "Connection failed.");
        lv_obj_clear_flag(label_roster_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (count == 0) {
        lv_label_set_text(label_roster_hint, "No game in progress.\nStart one from the tracker app.");
        lv_obj_clear_flag(label_roster_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(label_roster_hint, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < count; i++) {
        tracker_roster_entry_t e;
        tracker_roster_entry(i, &e);
        char life_str[16];
        snprintf(life_str, sizeof(life_str), "%d", e.life);
        make_row(roster_list_container, e.name, life_str,
                 lv_color_white(), event_roster_row_click,
                 (void *)(intptr_t)e.player_id);
    }
}

static void build_tracker_roster_screen(void)
{
    screen_tracker_roster = lv_obj_create(NULL);
    lv_obj_set_size(screen_tracker_roster, 360, 360);
    lv_obj_set_style_bg_color(screen_tracker_roster, lv_color_black(), 0);
    lv_obj_set_style_border_width(screen_tracker_roster, 0, 0);
    lv_obj_set_scrollbar_mode(screen_tracker_roster, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(screen_tracker_roster, 0, 0);
    lv_obj_clear_flag(screen_tracker_roster, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen_tracker_roster);
    lv_label_set_text(title, "Pick Player");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    roster_list_container = lv_obj_create(screen_tracker_roster);
    lv_obj_remove_style_all(roster_list_container);
    lv_obj_set_size(roster_list_container, 310, 260);
    lv_obj_align(roster_list_container, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_flex_flow(roster_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(roster_list_container, 4, 0);
    lv_obj_set_scrollbar_mode(roster_list_container, LV_SCROLLBAR_MODE_OFF);

    label_roster_hint = lv_label_create(screen_tracker_roster);
    lv_label_set_text(label_roster_hint, "Connecting...");
    lv_obj_set_style_text_color(label_roster_hint, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_roster_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(label_roster_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label_roster_hint, LV_ALIGN_CENTER, 0, 0);
}

void open_tracker_roster_screen(const char *base_url)
{
    if (base_url && base_url[0]) {
        tracker_connect(base_url);
    }
    roster_last_state = TRACKER_DISCONNECTED;  /* force first refresh */
    roster_last_count = -1;
    refresh_tracker_roster_ui();
    if (roster_poll_timer == NULL)
        roster_poll_timer = lv_timer_create(roster_poll_cb, 500, NULL);
    else
        lv_timer_reset(roster_poll_timer);
    load_screen_if_needed(screen_tracker_roster);
}

/* ============================================================
 *  Build-all entry point
 * ============================================================ */

void build_tracker_screens(void)
{
    build_tracker_url_entry_screen();
    build_tracker_roster_screen();
}

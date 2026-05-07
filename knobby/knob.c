#include "src/types.h"
#include "src/hw.h"
#include "src/storage.h"
#include "src/game.h"
#include "src/timer.h"
#include "src/dice.h"
#include "src/intro.h"
#include "src/ui_1p.h"
#include "src/ui_mp.h"
#include "src/ui_player_menu.h"
#include "src/settings.h"
#include "src/game_mode.h"
#include "src/damage_log.h"
#include "src/rename.h"
#include "src/mana.h"

// ---------- swipe state ----------
static lv_obj_t *previous_screen = NULL;
static lv_obj_t *swipe_hint = NULL;
static lv_obj_t *swipe_hint_icon = NULL;
static volatile bool swipe_up_pending = false;
static volatile bool swipe_down_pending = false;
static volatile bool swipe_left_pending = false;
static volatile bool swipe_right_pending = false;

#define SWIPE_HINT_SIZE 52
#define SWIPE_HINT_EDGE_INSET 12
#define SWIPE_HINT_TRAVEL 18
#define SWIPE_HINT_REVEAL_START 12
#define SWIPE_HINT_TOP_EDGE_ZONE 80
#define SWIPE_HINT_BOTTOM_EDGE_ZONE 80
#define SWIPE_HINT_OPACITY_MAX 255
#define SWIPE_HINT_OPACITY_MIN 48
#define SWIPE_HINT_BG_OPACITY_MAX 120

typedef enum {
    SWIPE_HINT_NONE = 0,
    SWIPE_HINT_LEFT,
    SWIPE_HINT_RIGHT,
    SWIPE_HINT_UP,
    SWIPE_HINT_DOWN
} swipe_hint_direction_t;

// ---------- knob event queue ----------
static int last_knob_cont = 0;
static bool knob_initialized = false;
static knob_input_event_t knob_event_queue[KNOB_EVENT_QUEUE_SIZE];
static volatile uint8_t knob_event_head = 0;
static volatile uint8_t knob_event_tail = 0;

// ---------- swipe notifications ----------
void knob_notify_swipe_up(void)
{
    swipe_up_pending = true;
}

void knob_notify_swipe_down(void)
{
    swipe_down_pending = true;
}

void knob_notify_swipe_left(void)
{
    swipe_left_pending = true;
}

void knob_notify_swipe_right(void)
{
    swipe_right_pending = true;
}

static bool is_player_screen(lv_obj_t *screen)
{
    return screen == screen_1p ||
           screen == screen_multiplayer;
}

static int clamp_value(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static int scale_value(int value, int input_max, int output_max)
{
    if (value <= 0 || input_max <= 0 || output_max <= 0) return 0;
    return (int)(((int64_t)value * output_max) + (input_max / 2)) / input_max;
}

static int get_display_width(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    return (disp != NULL) ? (int)lv_disp_get_hor_res(disp) : 360;
}

static int get_display_height(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    return (disp != NULL) ? (int)lv_disp_get_ver_res(disp) : 360;
}

static swipe_hint_direction_t get_swipe_hint_direction(lv_obj_t *screen,
                                                       int start_x, int start_y,
                                                       int dx, int dy)
{
    int width = get_display_width();
    int height = get_display_height();
    int abs_dx = dx >= 0 ? dx : -dx;
    int abs_dy = dy >= 0 ? dy : -dy;

    if (is_player_screen(screen)) {
        if (start_x <= KNOB_SWIPE_LEFT_EDGE_ZONE &&
            dx > SWIPE_HINT_REVEAL_START &&
            abs_dy < KNOB_SWIPE_MAX_LATERAL &&
            dx >= abs_dy) {
            return SWIPE_HINT_RIGHT;
        }
        if (start_x >= (width - KNOB_SWIPE_RIGHT_EDGE_ZONE) &&
            -dx > SWIPE_HINT_REVEAL_START &&
            abs_dy < KNOB_SWIPE_MAX_LATERAL &&
            -dx >= abs_dy) {
            return SWIPE_HINT_LEFT;
        }
        if (start_y <= SWIPE_HINT_TOP_EDGE_ZONE &&
            dy > SWIPE_HINT_REVEAL_START &&
            abs_dx < KNOB_SWIPE_MAX_LATERAL &&
            dy >= abs_dx) {
            return SWIPE_HINT_DOWN;
        }
        if (start_y >= (height - SWIPE_HINT_BOTTOM_EDGE_ZONE) &&
            -dy > SWIPE_HINT_REVEAL_START &&
            abs_dx < KNOB_SWIPE_MAX_LATERAL &&
            -dy >= abs_dx) {
            return SWIPE_HINT_UP;
        }
    } else {
        if (start_x >= (width - KNOB_SWIPE_RIGHT_EDGE_ZONE) &&
            -dx > SWIPE_HINT_REVEAL_START &&
            abs_dy < KNOB_SWIPE_MAX_LATERAL &&
            -dx >= abs_dy) {
            return SWIPE_HINT_LEFT;
        }
        if (dy > SWIPE_HINT_REVEAL_START &&
            abs_dx < KNOB_SWIPE_MAX_LATERAL &&
            dy >= abs_dx) {
            return SWIPE_HINT_DOWN;
        }
    }

    return SWIPE_HINT_NONE;
}

static int get_swipe_hint_distance(swipe_hint_direction_t direction, int dx, int dy)
{
    switch (direction) {
    case SWIPE_HINT_LEFT:
        return -dx;
    case SWIPE_HINT_RIGHT:
        return dx;
    case SWIPE_HINT_UP:
        return -dy;
    case SWIPE_HINT_DOWN:
        return dy;
    default:
        return 0;
    }
}

static void ensure_swipe_hint(void)
{
    if (swipe_hint != NULL) return;

    swipe_hint = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(swipe_hint);
    lv_obj_set_size(swipe_hint, SWIPE_HINT_SIZE, SWIPE_HINT_SIZE);
    lv_obj_set_style_radius(swipe_hint, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(swipe_hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(swipe_hint, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(swipe_hint, 0, 0);
    lv_obj_set_style_outline_width(swipe_hint, 0, 0);
    lv_obj_set_style_pad_all(swipe_hint, 0, 0);
    lv_obj_clear_flag(swipe_hint, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(swipe_hint, LV_OBJ_FLAG_HIDDEN);

    swipe_hint_icon = lv_label_create(swipe_hint);
    lv_label_set_text(swipe_hint_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(swipe_hint_icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(swipe_hint_icon, lv_color_white(), 0);
    lv_obj_set_style_text_opa(swipe_hint_icon, LV_OPA_TRANSP, 0);
    lv_obj_center(swipe_hint_icon);
}

void knob_swipe_hint_clear(void)
{
    if (swipe_hint == NULL) return;
    lv_obj_add_flag(swipe_hint, LV_OBJ_FLAG_HIDDEN);
}

void knob_swipe_hint_update(int start_x, int start_y, int cur_x, int cur_y)
{
    lv_obj_t *screen = lv_scr_act();
    swipe_hint_direction_t direction;
    int dx = cur_x - start_x;
    int dy = cur_y - start_y;
    int distance;
    int width;
    int height;
    int progress;
    int inset;
    int pos_x;
    int pos_y;
    const char *symbol;

    ensure_swipe_hint();
    direction = get_swipe_hint_direction(screen, start_x, start_y, dx, dy);
    if (direction == SWIPE_HINT_NONE) {
        knob_swipe_hint_clear();
        return;
    }

    distance = get_swipe_hint_distance(direction, dx, dy);
    if (distance <= SWIPE_HINT_REVEAL_START) {
        knob_swipe_hint_clear();
        return;
    }

    width = get_display_width();
    height = get_display_height();
    progress = scale_value(distance - SWIPE_HINT_REVEAL_START,
                           KNOB_SWIPE_THRESHOLD - SWIPE_HINT_REVEAL_START,
                           SWIPE_HINT_OPACITY_MAX);
    if (progress < SWIPE_HINT_OPACITY_MIN) progress = SWIPE_HINT_OPACITY_MIN;
    if (progress > SWIPE_HINT_OPACITY_MAX) progress = SWIPE_HINT_OPACITY_MAX;
    inset = SWIPE_HINT_EDGE_INSET +
            scale_value(distance - SWIPE_HINT_REVEAL_START,
                        KNOB_SWIPE_THRESHOLD,
                        SWIPE_HINT_TRAVEL);
    if (inset > (SWIPE_HINT_EDGE_INSET + SWIPE_HINT_TRAVEL)) {
        inset = SWIPE_HINT_EDGE_INSET + SWIPE_HINT_TRAVEL;
    }

    pos_x = clamp_value(start_x - (SWIPE_HINT_SIZE / 2), SWIPE_HINT_EDGE_INSET, width - SWIPE_HINT_SIZE - SWIPE_HINT_EDGE_INSET);
    pos_y = clamp_value(start_y - (SWIPE_HINT_SIZE / 2), SWIPE_HINT_EDGE_INSET, height - SWIPE_HINT_SIZE - SWIPE_HINT_EDGE_INSET);
    symbol = LV_SYMBOL_LEFT;

    switch (direction) {
    case SWIPE_HINT_RIGHT:
        symbol = LV_SYMBOL_RIGHT;
        pos_x = inset;
        break;
    case SWIPE_HINT_LEFT:
        symbol = LV_SYMBOL_LEFT;
        pos_x = width - SWIPE_HINT_SIZE - inset;
        break;
    case SWIPE_HINT_DOWN:
        symbol = LV_SYMBOL_DOWN;
        pos_y = inset;
        break;
    case SWIPE_HINT_UP:
        symbol = LV_SYMBOL_UP;
        pos_y = height - SWIPE_HINT_SIZE - inset;
        break;
    default:
        break;
    }

    lv_label_set_text(swipe_hint_icon, symbol);
    lv_obj_set_pos(swipe_hint, pos_x, pos_y);
    lv_obj_set_style_bg_opa(swipe_hint,
                            (lv_opa_t)scale_value(progress, SWIPE_HINT_OPACITY_MAX, SWIPE_HINT_BG_OPACITY_MAX),
                            0);
    lv_obj_set_style_text_opa(swipe_hint_icon, (lv_opa_t)progress, 0);
    lv_obj_clear_flag(swipe_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(swipe_hint);
}

static void open_menu_for_screen(lv_obj_t *screen)
{
    if (is_player_screen(screen)) {
        previous_screen = screen;
        open_quad_menu();
    }
}

static void handle_back_navigation(lv_obj_t *screen)
{
    if (screen == screen_quad_menu && previous_screen != NULL) {
        refresh_multiplayer_ui();
        lv_scr_load(previous_screen);
    } else if (screen == screen_tools_menu) {
        lv_scr_load(screen_quad_menu);
    } else if (screen == screen_screen_settings_menu) {
        settings_save();
        lv_scr_load(screen_quad_menu);
    } else if (screen == screen_settings_page2) {
        lv_scr_load(screen_screen_settings_menu);
    } else if (screen == screen_settings) {
        settings_save();
        lv_scr_load(screen_screen_settings_menu);
    } else if (screen == screen_battery) {
        lv_scr_load(screen_screen_settings_menu);
    } else if (screen == screen_dice) {
        lv_scr_load(screen_tools_menu);
    } else if (screen == screen_damage_log) {
        lv_scr_load(screen_tools_menu);
    } else if (screen == screen_select) {
        back_to_main();
    } else if (screen == screen_damage) {
        damage_cancel();
        open_select_screen();
    } else if (screen == screen_game_mode_menu) {
        lv_scr_load(screen_quad_menu);
    } else if (screen == screen_custom_life) {
        refresh_game_mode_menu_ui();
        lv_scr_load(screen_game_mode_menu);
    } else if (screen == screen_player_menu) {
        back_to_main();
    } else if (screen == screen_player_name) {
        if (!name_screen_handle_back())
            open_player_menu(menu_player);
    } else if (screen == screen_counter_menu) {
        open_player_menu(menu_player);
    } else if (screen == screen_counter_edit) {
        open_counter_menu();
    } else if (screen == screen_player_all_damage) {
        open_player_menu(menu_player);
    } else if (screen == screen_player_color_menu) {
        open_player_menu(menu_player);
    } else if (screen == screen_player_color_picker) {
        load_screen_if_needed(screen_player_color_menu);
    } else if (screen == screen_mana) {
        mana_discard_preview();
        lv_scr_load(screen_tools_menu);
    }
}

// ---------- reset ----------
void reset_all_values(void)
{
    knob_life_reset();

    brightness_percent = nvs_get_brightness();
    brightness_apply();

    turn_timer_reset();

    refresh_main_ui();
    refresh_select_ui();
    refresh_damage_ui();
    refresh_settings_ui();
    refresh_multiplayer_ui();

    refresh_rename_ui();
    refresh_all_damage_ui();
    refresh_counter_edit_ui();
    mana_clear_all();

    start_player_selection_animation();
}

void knob_cb(lv_event_t *e)
{
    (void)e;
}

// ---------- init ----------
void knob_gui(void)
{
    knob_hw_init();
    ensure_swipe_hint();

    build_intro_screen();
    lv_scr_load(screen_intro);
    lv_refr_now(NULL);
    scr_display_on();
    brightness_apply();
    build_dice_screen();
    build_main_screen();
    build_multiplayer_screen();
    build_player_menu_screen();
    build_eliminated_player_menu_screen();
    build_rename_screen();
    build_all_damage_screen();
    build_counter_menu_screen();
    build_counter_edit_screen();
    build_player_color_menu_screen();
    build_player_color_picker_screen();
    build_select_screen();
    build_damage_screen();
    build_mana_screen();
    build_settings_screen();
    build_battery_screen();
    build_damage_log_screen();
    build_quad_menus();
    build_game_mode_menu_screen();
    build_custom_life_screen();

    refresh_main_ui();
    refresh_multiplayer_ui();

    refresh_rename_ui();
    refresh_select_ui();
    refresh_damage_ui();
    refresh_all_damage_ui();
    refresh_counter_edit_ui();
    refresh_settings_ui();

    knob_timer_init();
    knob_life_init();
    knob_intro_init();
}

// ---------- knob event handler ----------
static void handle_knob_event(knob_event_t k)
{
    activity_kick();
    if (in_undim_grace()) return;

    if (lv_scr_act() == screen_intro)
    {
        return;
    }
    else if (lv_scr_act() == screen_1p)
    {
        selected_player = 0;
        if (k == KNOB_LEFT)      change_player_life(-1);
        else if (k == KNOB_RIGHT) change_player_life(+1);
    }
    else if (lv_scr_act() == screen_damage)
    {
        if (k == KNOB_LEFT)      add_damage_to_selected_enemy(-1);
        else if (k == KNOB_RIGHT) add_damage_to_selected_enemy(+1);
    }
    else if (lv_scr_act() == screen_settings)
    {
        if (k == KNOB_LEFT)      change_brightness(-1);
        else if (k == KNOB_RIGHT) change_brightness(+1);
        refresh_settings_ui();
    }
    else if (lv_scr_act() == screen_multiplayer)
    {
        if (k == KNOB_LEFT)      change_player_life(-1);
        else if (k == KNOB_RIGHT) change_player_life(+1);
    }
    else if (lv_scr_act() == screen_player_all_damage)
    {
        if (k == KNOB_LEFT)      change_all_damage(-1);
        else if (k == KNOB_RIGHT) change_all_damage(+1);
    }
    else if (lv_scr_act() == screen_custom_life)
    {
        if (k == KNOB_LEFT)      change_custom_life(-1);
        else if (k == KNOB_RIGHT) change_custom_life(+1);
    }
    else if (lv_scr_act() == screen_counter_edit)
    {
        if (k == KNOB_LEFT)      change_counter_edit(-1);
        else if (k == KNOB_RIGHT) change_counter_edit(+1);
        refresh_counter_edit_ui();
    }
    else if (lv_scr_act() == screen_damage_log)
    {
        if (k == KNOB_LEFT)      damage_log_select_prev();
        else if (k == KNOB_RIGHT) damage_log_select_next();
    }
    else if (lv_scr_act() == screen_player_name)
    {
        if (k == KNOB_LEFT)      mru_select_prev();
        else if (k == KNOB_RIGHT) mru_select_next();
    }
    else if (lv_scr_act() == screen_player_color_picker)
    {
        if (k == KNOB_LEFT)      change_player_color(-1);
        else if (k == KNOB_RIGHT) change_player_color(+1);
    }
    else if (lv_scr_act() == screen_mana)
    {
        if (k == KNOB_LEFT)      change_mana_value(-1);
        else if (k == KNOB_RIGHT) change_mana_value(+1);
    }
}

void knob_change(knob_event_t k, int cont)
{
    uint8_t next_head;

    if (!knob_initialized)
    {
        last_knob_cont = cont;
        knob_initialized = true;
    }

    last_knob_cont = cont;

    next_head = (uint8_t)((knob_event_head + 1U) % KNOB_EVENT_QUEUE_SIZE);
    if (next_head == knob_event_tail) {
        knob_event_tail = (uint8_t)((knob_event_tail + 1U) % KNOB_EVENT_QUEUE_SIZE);
    }

    knob_event_queue[knob_event_head].event = k;
    knob_event_queue[knob_event_head].cont = cont;
    knob_event_head = next_head;
}

void knob_process_pending(void)
{
    uint8_t processed = 0;

    if (swipe_up_pending) {
        swipe_up_pending = false;
        open_menu_for_screen(lv_scr_act());
    }

    if (swipe_down_pending) {
        lv_obj_t *cur;

        swipe_down_pending = false;
        cur = lv_scr_act();

        if (is_player_screen(cur))
            open_menu_for_screen(cur);
        else
            handle_back_navigation(cur);
    }

    if (swipe_left_pending) {
        lv_obj_t *cur;

        swipe_left_pending = false;
        cur = lv_scr_act();

        if (is_player_screen(cur))
            open_menu_for_screen(cur);
        else
            handle_back_navigation(cur);
    }

    if (swipe_right_pending) {
        swipe_right_pending = false;
        open_menu_for_screen(lv_scr_act());
    }

    while (knob_event_tail != knob_event_head && processed < 8U) {
        knob_event_t event = knob_event_queue[knob_event_tail].event;
        knob_event_tail = (uint8_t)((knob_event_tail + 1U) % KNOB_EVENT_QUEUE_SIZE);
        handle_knob_event(event);
        processed++;
    }
}

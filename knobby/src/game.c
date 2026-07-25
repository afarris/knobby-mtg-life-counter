#include "game.h"
#include "storage.h"
#include "damage_log.h"
#include "esp_random.h"
#include "net_sync.h"
// Forward declarations for UI refresh (defined in screen modules)
extern void refresh_player_ui(void);
extern void refresh_select_ui(void);
extern void refresh_damage_ui(void);
extern void refresh_all_damage_ui(void);
extern void refresh_rename_ui(void);
extern void select_kick_timer(void);

// ---------- state ----------
int active_enemy_count = 3;

enemy_state_t enemies[MAX_ENEMY_COUNT] = {
    {"P1", 0}, {"P2", 0}, {"P3", 0}, {"P4", 0},
    {"P5", 0}, {"P6", 0}, {"P7", 0}
};

int selected_enemy = -1;
int dice_result = 0;

int player_life[MAX_DISPLAY_PLAYERS] = {40, 40, 40, 40};
bool player_selected[MAX_DISPLAY_PLAYERS] = {false};
char player_names[MAX_GAME_PLAYERS][16] = {
    "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"
};
int menu_player = 0;
int cmd_damage_totals[MAX_GAME_PLAYERS][MAX_DISPLAY_PLAYERS] = {{0}};
int all_damage_value = 0;
int cmd_damage_target = -1;
static int damage_start_value = 0;
int pending_life_delta = 0;
bool life_preview_active = false;
int player_counters[MAX_DISPLAY_PLAYERS][COUNTER_TYPE_COUNT] = {{0}};
counter_type_t counter_edit_type = COUNTER_TYPE_COMMANDER_TAX;
int counter_edit_value = 0;
bool player_eliminated[MAX_DISPLAY_PLAYERS] = {false};
/* Conceded players (manual elimination) tracked apart from auto-elimination,
   so an undo that recomputes auto conditions can't revive someone who
   manually conceded while still above 0 life. */
static bool player_manually_eliminated[MAX_DISPLAY_PLAYERS] = {false};

typedef struct {
    bool valid;
    uint8_t event_type;
    int source;
    int delta;
} elimination_action_t;

static elimination_action_t elimination_action[MAX_DISPLAY_PLAYERS] = {{0}};



static lv_timer_t *life_preview_timer = NULL;

/* Per-player Lamport versions for Table Sync, scoped by a game epoch.
   Every local commit bumps the touched player's version and broadcasts
   a full state snapshot; adopting a remote block adopts its version.
   The epoch dominates the comparison (see net_sync_apply_state), so
   versions from different games are never compared against each other.
   uint16 wrap is handled with serial arithmetic. */
static uint16_t game_epoch = 0;
static uint16_t player_version[MAX_DISPLAY_PLAYERS] = {0};
static uint16_t names_version = 0;

static void clear_player_elimination_action(int player);

static void net_sync_commit_player(int player)
{
    player_version[player]++;
    net_sync_send_state();
}

void net_sync_commit_names(void)
{
    names_version++;
    net_sync_send_names();
}

void net_sync_begin_game(void)
{
    int i;
    game_epoch++;
    for (i = 0; i < MAX_DISPLAY_PLAYERS; i++) player_version[i] = 1;
    /* Names outlive game resets, so a mid-session reset leaves the
       roster version alone. A fresh host must still seed it above a
       joiner's zero, or the joiner's leftover roster could win the
       first tie. */
    if (names_version == 0) names_version = 1;
}

void net_sync_reset_versions(void)
{
    int i;
    game_epoch = 0;
    memset(player_version, 0, sizeof(player_version));
    names_version = 0;
    /* Joining a table: elimination-undo bookkeeping and the event log
       refer to a game this device is leaving behind; replaying either
       against adopted state would corrupt life and mirror the
       corruption table-wide. */
    for (i = 0; i < MAX_DISPLAY_PLAYERS; i++)
        clear_player_elimination_action(i);
    damage_log_reset();
}

#define MANA_ICON_COMMANDER "\xEE\xA7\x86"
#define MANA_ICON_PARTY     "\xEE\xA6\x87"
#define MANA_ICON_SKULL     "\xEE\x98\x98"
#define MANA_ICON_LEVEL     "\xEE\xA4\x80"

static const counter_definition_t counter_definitions[COUNTER_TYPE_COUNT] = {
    {"Commander\nTax", "Commander Tax", "C", MANA_ICON_COMMANDER, 0xA84300, true},
    {"Partner\nTax", "Partner Tax", "P", MANA_ICON_PARTY, 0x1565C0, true},
    {"Poison", "Poison", "!", MANA_ICON_SKULL, 0x2E7D32, true},
    {"Experience", "Experience", "E", MANA_ICON_LEVEL, 0x6A1B9A, true},
};

static void clear_player_elimination_action(int player)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;
    elimination_action[player].valid = false;
}

static void set_player_elimination_action(int player, uint8_t event_type, int source, int delta)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;
    elimination_action[player].valid = true;
    elimination_action[player].event_type = event_type;
    elimination_action[player].source = source;
    elimination_action[player].delta = delta;
}

bool elimination_action_available(int player)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return false;
    return elimination_action[player].valid;
}

void undo_elimination_action(int player)
{
    if (!elimination_action_available(player)) return;

    elimination_action_t action = elimination_action[player];
    clear_player_elimination_action(player);

    if (action.event_type == LOG_EVT_LIFE) {
        undo_life_change(player, action.delta);
    } else if (action.event_type == LOG_EVT_CMD_DAMAGE) {
        undo_life_change(player, action.delta);
        undo_cmd_damage(action.source, player, action.delta);
    } else if (action.event_type == LOG_EVT_COUNTER) {
        undo_counter_change(player, action.source, action.delta);
    }

    /* Drop the log entry that caused the elimination so the same event can't
       be undone a second time from the Event Log. The eliminating event is
       the newest one for this player (eliminated players accrue no more). */
    damage_log_remove_last_for(player, action.event_type);
}

void check_player_elimination(int player)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;
    bool was_eliminated = player_eliminated[player];
    bool now_eliminated = false;

    /* Elimination is a multiplayer concept: with a single tracked player
       there is no eliminated-menu route in the 1p UI, so eliminating
       player 0 would brick the counter until reset. */
    if (nvs_get_auto_eliminate() && nvs_get_players_to_track() > 1) {
        if (player_life[player] <= 0) {
            now_eliminated = true;
        } else {
            for (int i = 0; i < MAX_GAME_PLAYERS; i++) {
                if (i != player && cmd_damage_totals[i][player] >= 21) {
                    now_eliminated = true;
                    break;
                }
            }
            if (!now_eliminated && player_counters[player][COUNTER_TYPE_POISON] >= 10) {
                now_eliminated = true;
            }
        }
    }

    if (player_manually_eliminated[player]) {
        now_eliminated = true;
    }

    player_eliminated[player] = now_eliminated;
    if (!now_eliminated) {
        clear_player_elimination_action(player);
    } else if (player_selected[player]) {
        /* An eliminated player is no longer a life-change target: drop it
           from the selection so the knob doesn't preview onto a dead panel
           that can't be tapped to deselect. */
        player_selected[player] = false;
        select_kick_timer();
    }

    if (was_eliminated != now_eliminated) {
        refresh_player_ui();
    }
}

void manual_eliminate_player(int player)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;
    if (player_eliminated[player]) return;
    /* Same solo-mode exemption as check_player_elimination. */
    if (nvs_get_players_to_track() <= 1) return;
    player_eliminated[player] = true;
    player_manually_eliminated[player] = true;
    clear_player_elimination_action(player);
    if (player_selected[player]) {
        player_selected[player] = false;
        select_kick_timer();
    }
    net_sync_commit_player(player);
    refresh_player_ui();
}

void manual_uneliminate_player(int player)
{
    int i;
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;
    if (!player_eliminated[player]) return;
    player_eliminated[player] = false;
    player_manually_eliminated[player] = false;
    clear_player_elimination_action(player);
    /* A remotely-caused elimination arrives with no local
       elimination_action to undo, so revival must also clear whatever
       condition would instantly re-kill the player — otherwise they
       come back at e.g. -2 life and re-die on the next touch. Pull
       each lethal condition just below its threshold, but only when
       auto-elimination would actually re-fire (same gate as
       check_player_elimination): with it off, life <= 0 or poison >=
       10 are legitimate alive states that must not be rewritten. */
    if (nvs_get_auto_eliminate() && nvs_get_players_to_track() > 1) {
        if (player_life[player] < 1) player_life[player] = 1;
        if (player_counters[player][COUNTER_TYPE_POISON] > 9)
            player_counters[player][COUNTER_TYPE_POISON] = 9;
        for (i = 0; i < MAX_GAME_PLAYERS; i++) {
            if (cmd_damage_totals[i][player] > 20)
                cmd_damage_totals[i][player] = 20;
        }
    }
    net_sync_commit_player(player);
    refresh_player_ui();
}

// ---------- player colors ----------
static const uint32_t player_color_table[MAX_GAME_PLAYERS][LIFE_VIB_COUNT] = {
    /*  dim        mid        vivid  */
    {0x024D3A, 0x06D6A0, 0x66FFD9},  /* P1 green  (bottom-left) */
    {0x2A0A4D, 0x7B1FE0, 0x9C4DFF},  /* P2 purple (top-left)    */
    {0x0A3A4D, 0x29B6F6, 0x4FC3F7},  /* P3 blue   (top-right)   */
    {0x4D4400, 0xFFD600, 0xFFEA61},  /* P4 yellow (bottom-right) */
    {0x4D1C1C, 0xF44336, 0xFF5252},  /* P5 red    */
    {0x4D3300, 0xFF9800, 0xFFB74D},  /* P6 orange */
    {0x004D4D, 0x00BCD4, 0x4DD0E1},  /* P7 cyan   */
    {0x3D0A4D, 0xE040FB, 0xEA80FC},  /* P8 pink   */
};

// ---------- custom color palette (18 colors) ----------
static const uint32_t custom_color_table[CUSTOM_COLOR_COUNT][LIFE_VIB_COUNT] = {
    /*  dim        mid        vivid  */
    {0x024D3A, 0x06D6A0, 0x66FFD9},  /*  0 Green (logo) */
    {0x2A0A4D, 0x7B1FE0, 0x9C4DFF},  /*  1 Purple */
    {0x0A3A4D, 0x29B6F6, 0x4FC3F7},  /*  2 Blue   */
    {0x4D4400, 0xFFD600, 0xFFEA61},  /*  3 Yellow */
    {0x4D1C1C, 0xF44336, 0xFF5252},  /*  4 Red    */
    {0x4D3300, 0xFF9800, 0xFFB74D},  /*  5 Orange */
    {0x004D4D, 0x00BCD4, 0x4DD0E1},  /*  6 Cyan   */
    {0x3D0A4D, 0xE040FB, 0xEA80FC},  /*  7 Pink   */
    {0x2D4D00, 0x8BC34A, 0xAED581},  /*  8 Lime   */
    {0x0A0A4D, 0x3F51B5, 0x7986CB},  /*  9 Indigo */
    {0x4D0A2A, 0xE91E63, 0xF06292},  /* 10 Rose   */
    {0x333333, 0xAAAAAA, 0xFFFFFF},  /* 11 White  */
    {0x00332E, 0x009688, 0x4DB6AC},  /* 12 Teal   */
    {0x4D3800, 0xFFC107, 0xFFD54F},  /* 13 Amber  */
    {0x2E1F16, 0x795548, 0xA1887F},  /* 14 Brown  */
    {0x1E2D33, 0x607D8B, 0x90A4AE},  /* 15 Gray   */
    {0x1A3D1A, 0xA5D6A7, 0x4CAF50},  /* 16 Sage   */
    {0x0A0A0A, 0x303030, 0x505050},  /* 17 Black  */
};

static const char *custom_color_names[CUSTOM_COLOR_COUNT] = {
    "Green", "Purple", "Blue", "Yellow",
    "Red", "Orange", "Cyan", "Pink",
    "Lime", "Indigo", "Rose", "White",
    "Teal", "Amber", "Brown", "Gray",
    "Sage", "Black",
};

// ---------- per-player color state (runtime only, lost on reboot) ----------
int player_color_index[MAX_DISPLAY_PLAYERS] = {0, 1, 2, 3};
bool player_life_color[MAX_DISPLAY_PLAYERS] = {false, false, false, false};
bool player_has_override[MAX_DISPLAY_PLAYERS] = {false, false, false, false};

lv_color_t get_player_color_vib(int index, int vibrancy)
{
    if (index < 0 || index >= MAX_GAME_PLAYERS) return lv_color_hex(0x303030);
    if (vibrancy < 0 || vibrancy >= LIFE_VIB_COUNT) vibrancy = LIFE_VIB_MID;
    return lv_color_hex(player_color_table[index][vibrancy]);
}

lv_color_t get_player_base_color(int index)
{
    return get_player_color_vib(index, LIFE_VIB_MID);
}

lv_color_t get_player_active_color(int index)
{
    return get_player_color_vib(index, LIFE_VIB_VIV);
}

lv_color_t get_player_text_color(int index)
{
    lv_color_t bg = get_player_base_color(index);
    return color_is_light(bg) ? lv_color_black() : lv_color_white();
}

lv_color_t get_player_preview_color(int index, int delta)
{
    if (index == 3) {
        /* P4 yellow: dark red / dark green for contrast */
        return (delta < 0) ? lv_color_hex(0x7A1020) : lv_color_hex(0x215A2A);
    }
    if (index == 0) {
        /* P1 green: bright red / white for contrast on green bg */
        return (delta < 0) ? lv_palette_main(LV_PALETTE_RED) : lv_color_white();
    }
    return (delta < 0) ? lv_palette_main(LV_PALETTE_RED) : lv_palette_main(LV_PALETTE_GREEN);
}

lv_color_t get_custom_color_vib(int index, int vibrancy)
{
    if (index < 0 || index >= CUSTOM_COLOR_COUNT) index = 0;
    if (vibrancy < 0 || vibrancy >= LIFE_VIB_COUNT) vibrancy = LIFE_VIB_MID;
    return lv_color_hex(custom_color_table[index][vibrancy]);
}

const char *get_custom_color_name(int index)
{
    if (index < 0 || index >= CUSTOM_COLOR_COUNT) index = 0;
    return custom_color_names[index];
}

lv_color_t get_effective_player_color(int player_i, int color_i, int vibrancy)
{
    /* Per-player override takes precedence over global mode */
    if (player_has_override[player_i]) {
        if (player_life_color[player_i]) {
            int life = player_life[player_i];
            int max_life = nvs_get_life_total();
            int tier = get_life_tier(life, max_life);
            return get_life_color_vib(tier, vibrancy);
        }
        return get_custom_color_vib(player_color_index[player_i], vibrancy);
    }

    /* No override: use global mode */
    if (nvs_get_color_mode() == COLOR_MODE_LIFE) {
        int life = player_life[player_i];
        int max_life = nvs_get_life_total();
        int tier = get_life_tier(life, max_life);
        return get_life_color_vib(tier, vibrancy);
    }

    /* COLOR_MODE_PLAYER: use position color */
    return get_player_color_vib(color_i, vibrancy);
}

int get_cmd_target_player_index(int row)
{
    int skip_player;
    int num = nvs_get_num_players();
    int count = 0;
    int i;

    if (row < 0 || row >= active_enemy_count) return row;

    if (cmd_damage_target >= 0) {
        skip_player = cmd_damage_target;
    } else {
        /* No explicit target — only hidden-screen repaints and the
           sim's direct navigation reach this (every live flow sets
           cmd_damage_target first): map as if the owner, player 0,
           were the target, so the enemy rows show players 1..n-1. */
        skip_player = 0;
    }

    for (i = 0; i < num; i++) {
        if (i == skip_player) continue;
        if (count == row) return i;
        count++;
    }

    return row;
}

const counter_definition_t *get_counter_definition(counter_type_t type)
{
    if (type < 0 || type >= COUNTER_TYPE_COUNT) return NULL;
    return &counter_definitions[type];
}

bool counter_type_is_enabled(counter_type_t type)
{
    const counter_definition_t *definition = get_counter_definition(type);

    return (definition != NULL) && definition->enabled;
}

int get_counter_value(int player, counter_type_t type)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return 0;
    if (type < 0 || type >= COUNTER_TYPE_COUNT) return 0;

    return player_counters[player][type];
}

void begin_counter_edit(int player, counter_type_t type)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;
    if (type < 0 || type >= COUNTER_TYPE_COUNT) return;

    menu_player = player;
    counter_edit_type = type;
    counter_edit_value = player_counters[player][type];
}

void change_counter_edit(int delta)
{
    counter_edit_value = clamp_counter(counter_edit_value + delta);
}

/* Knob turns since the editor opened, i.e. the delta apply_counter_edit()
   will commit against the player's live counter. */
int counter_edit_pending_delta(void)
{
    if (menu_player < 0 || menu_player >= MAX_DISPLAY_PLAYERS) return 0;
    if (counter_edit_type < 0 || counter_edit_type >= COUNTER_TYPE_COUNT) return 0;
    return counter_edit_value - player_counters[menu_player][counter_edit_type];
}

int apply_counter_edit(void)
{
    int player = menu_player;
    int old_value;
    int change_delta;

    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return 0;
    if (counter_edit_type < 0 || counter_edit_type >= COUNTER_TYPE_COUNT) return 0;
    /* Same remote-elimination race guard as damage_apply. */
    if (player_eliminated[player]) return 0;

    old_value = player_counters[player][counter_edit_type];
    counter_edit_value = clamp_counter(counter_edit_value);
    change_delta = counter_edit_value - old_value;
    player_counters[player][counter_edit_type] = counter_edit_value;

    if (change_delta != 0) {
        damage_log_add(player, change_delta, LOG_EVT_COUNTER, counter_edit_type);
        if (counter_edit_type == COUNTER_TYPE_POISON &&
            old_value < 10 && counter_edit_value >= 10) {
            set_player_elimination_action(player, LOG_EVT_COUNTER, counter_edit_type, change_delta);
        }
        if (counter_edit_type == COUNTER_TYPE_POISON) {
            check_player_elimination(player);
        }
        net_sync_commit_player(player);
    }

    return change_delta;
}

// ---------- player selection set ----------
int selection_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_DISPLAY_PLAYERS; i++)
        if (player_selected[i]) n++;
    return n;
}

bool is_player_selected(int player)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return false;
    return player_selected[player];
}

void selection_clear(void)
{
    int i;
    for (i = 0; i < MAX_DISPLAY_PLAYERS; i++)
        player_selected[i] = false;
}

void selection_toggle(int player)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;
    if (player_eliminated[player]) return;
    player_selected[player] = !player_selected[player];
}

void selection_set_single(int player)
{
    selection_clear();
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;
    if (player_eliminated[player]) return;
    player_selected[player] = true;
}

/* The single entry point for committing a life change as a game event:
   log + clamp + elimination-undo action + elimination check. Any path
   that applies life deltas (knob commit, All Damage) must use this so
   the elimination machinery can't be bypassed. */
void apply_life_delta(int player, int delta)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;
    if (player_eliminated[player]) return;
    damage_log_add(player, delta, LOG_EVT_LIFE, -1);
    player_life[player] = clamp_life(player_life[player] + delta);
    if (player_life[player] <= 0) {
        set_player_elimination_action(player, LOG_EVT_LIFE, -1, delta);
    }
    check_player_elimination(player);
    net_sync_commit_player(player);
}

// ---------- life preview ----------
void life_preview_commit_cb(lv_timer_t *timer)
{
    int track = nvs_get_players_to_track();
    int i;

    (void)timer;

    if (!life_preview_active || selection_count() == 0) {
        pending_life_delta = 0;
        life_preview_active = false;
        if (life_preview_timer != NULL) {
            lv_timer_pause(life_preview_timer);
        }
        return;
    }

    for (i = 0; i < track && i < MAX_DISPLAY_PLAYERS; i++) {
        if (!player_selected[i]) continue;
        apply_life_delta(i, pending_life_delta);
    }
    pending_life_delta = 0;
    life_preview_active = false;
    if (life_preview_timer != NULL) {
        lv_timer_pause(life_preview_timer);
    }
    /* Applying a life change ends the operation: in multi-select mode clear
       the selection so the next tap starts a fresh selection. Otherwise
       sequential per-player damage keeps stacking players into the set. */
    if (nvs_get_multi_select()) {
        selection_clear();
        select_kick_timer();
    }
    refresh_player_ui();
}

// ---------- life changes ----------
void damage_enter(void)
{
    if (selected_enemy >= 0 && selected_enemy < active_enemy_count)
        damage_start_value = enemies[selected_enemy].damage;
    else
        damage_start_value = 0;
}

void add_damage_to_selected_enemy(int delta)
{
    if (selected_enemy < 0 || selected_enemy >= active_enemy_count) return;

    enemies[selected_enemy].damage += delta;
    if (enemies[selected_enemy].damage < 0)
        enemies[selected_enemy].damage = 0;

    refresh_damage_ui();
}

/* Knob turns since the editor opened, i.e. the delta damage_apply()
   will commit. The staged value lives in enemies[].damage; this is the
   difference against the snapshot damage_enter() took. */
int damage_pending_delta(void)
{
    if (selected_enemy < 0 || selected_enemy >= active_enemy_count) return 0;
    return enemies[selected_enemy].damage - damage_start_value;
}

void damage_apply(void)
{
    int delta;
    int source;

    if (selected_enemy < 0 || selected_enemy >= active_enemy_count) return;
    if (cmd_damage_target < 0 || cmd_damage_target >= MAX_DISPLAY_PLAYERS) return;
    /* Unreachable locally (the editor only opens for a live target),
       but a remote elimination can race an open editor: eliminated
       players accrue no more (same rule as apply_life_delta), and
       committing here would overwrite their elimination-undo action. */
    if (player_eliminated[cmd_damage_target]) return;

    delta = enemies[selected_enemy].damage - damage_start_value;
    if (delta == 0) return;

    source = get_cmd_target_player_index(selected_enemy);
    cmd_damage_totals[source][cmd_damage_target] = enemies[selected_enemy].damage;
    damage_log_add(cmd_damage_target, -delta, LOG_EVT_CMD_DAMAGE, source);
    player_life[cmd_damage_target] = clamp_life(player_life[cmd_damage_target] - delta);
    if (cmd_damage_totals[source][cmd_damage_target] >= 21 || player_life[cmd_damage_target] <= 0) {
        set_player_elimination_action(cmd_damage_target, LOG_EVT_CMD_DAMAGE, source, -delta);
    }
    check_player_elimination(cmd_damage_target);
    net_sync_commit_player(cmd_damage_target);

    refresh_select_ui();
}

void damage_cancel(void)
{
    if (selected_enemy >= 0 && selected_enemy < active_enemy_count)
        enemies[selected_enemy].damage = damage_start_value;
}

void change_player_life(int delta)
{
    /* The shared delta applies to every currently-selected player. Clamp it
       to the headroom of the selected set so the previewed totals always
       equal what the commit will store and overshoot detents at the life
       cap are absorbed instead of accumulating. */
    int track = nvs_get_players_to_track();
    int max_up = LIFE_MAX;
    int min_down = LIFE_MIN;
    int i;

    /* The roulette walks the selection every tick, so a delta dialed
       mid-spin would land on whichever player the wheel stops at. */
    if (player_selection_animation_active()) return;

    if (selection_count() == 0) return;

    select_kick_timer();

    for (i = 0; i < track && i < MAX_DISPLAY_PLAYERS; i++) {
        if (!player_selected[i] || player_eliminated[i]) continue;
        if (LIFE_MAX - player_life[i] < max_up) max_up = LIFE_MAX - player_life[i];
        if (LIFE_MIN - player_life[i] > min_down) min_down = LIFE_MIN - player_life[i];
    }

    pending_life_delta += delta;
    if (pending_life_delta > max_up) pending_life_delta = max_up;
    if (pending_life_delta < min_down) pending_life_delta = min_down;
    life_preview_active = (pending_life_delta != 0);

    if (life_preview_timer != NULL) {
        if (life_preview_active) {
            lv_timer_reset(life_preview_timer);
            lv_timer_resume(life_preview_timer);
        } else {
            lv_timer_pause(life_preview_timer);
        }
    }

    refresh_player_ui();
}

void prepare_cmd_damage_for_player(int target)
{
    int i, row = 0;
    int num = nvs_get_num_players();

    cmd_damage_target = target;

    for (i = 0; i < num; i++) {
        if (i == target) continue;
        if (row < MAX_ENEMY_COUNT) {
            enemies[row].damage = cmd_damage_totals[i][target];
            row++;
        }
    }
}

void change_all_damage(int delta)
{
    all_damage_value += delta;
    if (all_damage_value < 0) all_damage_value = 0;
    refresh_all_damage_ui();
}

// ---------- undo ----------
void undo_life_change(int player, int delta)
{
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;

    player_life[player] = clamp_life(player_life[player] - delta);
    check_player_elimination(player);
    net_sync_commit_player(player);
    refresh_player_ui();
    refresh_select_ui();
}

void undo_cmd_damage(int source, int target, int delta)
{
    if (source < 0 || source >= MAX_GAME_PLAYERS) return;
    if (target < 0 || target >= MAX_DISPLAY_PLAYERS) return;
    cmd_damage_totals[source][target] += delta;
    if (cmd_damage_totals[source][target] < 0)
        cmd_damage_totals[source][target] = 0;
    check_player_elimination(target);
    net_sync_commit_player(target);
}

void undo_counter_change(int player, int counter_type, int delta)
{
    if (counter_type < 0 || counter_type >= COUNTER_TYPE_COUNT) return;
    if (player < 0 || player >= MAX_DISPLAY_PLAYERS) return;

    player_counters[player][counter_type] = clamp_counter(
        player_counters[player][counter_type] - delta
    );
    if (counter_type == COUNTER_TYPE_POISON) {
        check_player_elimination(player);
    }
    net_sync_commit_player(player);
    refresh_player_ui();
}

// ---------- reset ----------
void knob_life_reset(void)
{
    int starting_life = nvs_get_life_total();
    int num = nvs_get_num_players();
    int i;

    active_enemy_count = num - 1;
    if (active_enemy_count < 0) active_enemy_count = 0;
    if (active_enemy_count > MAX_ENEMY_COUNT) active_enemy_count = MAX_ENEMY_COUNT;

    damage_log_reset();

    pending_life_delta = 0;
    selection_clear();
    life_preview_active = false;
    selected_enemy = -1;
    dice_result = 0;

    for (i = 0; i < MAX_ENEMY_COUNT; i++) {
        enemies[i].damage = 0;
    }

    for (i = 0; i < MAX_DISPLAY_PLAYERS; i++) {
        player_life[i] = starting_life;
    }
    menu_player = 0;
    cmd_damage_target = -1;
    memset(cmd_damage_totals, 0, sizeof(cmd_damage_totals));
    memset(player_counters, 0, sizeof(player_counters));
    memset(player_eliminated, 0, sizeof(player_eliminated));
    memset(player_manually_eliminated, 0, sizeof(player_manually_eliminated));
    for (i = 0; i < MAX_DISPLAY_PLAYERS; i++) clear_player_elimination_action(i);
    all_damage_value = 0;
    counter_edit_type = COUNTER_TYPE_COMMANDER_TAX;
    counter_edit_value = 0;

    /* A reset starts a new game: bump the epoch (which outranks any
       version drift a strayed device accumulated) and broadcast once. */
    net_sync_begin_game();
    net_sync_send_state();

    if (life_preview_timer != NULL) {
        lv_timer_pause(life_preview_timer);
    }
}

// ---------- init ----------
void knob_life_init(void)
{
    int starting_life = nvs_get_life_total();
    int num = nvs_get_num_players();
    int i;

    active_enemy_count = num - 1;
    if (active_enemy_count < 0) active_enemy_count = 0;
    if (active_enemy_count > MAX_ENEMY_COUNT) active_enemy_count = MAX_ENEMY_COUNT;

    for (i = 0; i < MAX_DISPLAY_PLAYERS; i++) {
        player_life[i] = starting_life;
    }
    memset(player_counters, 0, sizeof(player_counters));
    counter_edit_type = COUNTER_TYPE_COMMANDER_TAX;
    counter_edit_value = 0;

    life_preview_timer = lv_timer_create(life_preview_commit_cb, 3000, NULL);
    if (life_preview_timer != NULL) {
        lv_timer_pause(life_preview_timer);
    }
}

// ---------- player selection animation ----------
static lv_timer_t *player_select_anim_timer = NULL;
static int player_select_anim_steps = 0;
static int player_select_anim_period = 0;
static int roulette_idx = 0;

static void player_select_anim_cb(lv_timer_t *timer)
{
    int track = nvs_get_players_to_track();
    (void)timer;

    if (track <= 1) {
        lv_timer_pause(player_select_anim_timer);
        return;
    }

    // Move to next player (clockwise logic mapping to bottom/left/top/right)
    roulette_idx = (roulette_idx + 1) % track;
    selection_set_single(roulette_idx);
    /* Restart the deselect-timeout countdown like any selection change,
       so a timer left running from before the reset can't fire mid-spin
       and blank the selection for a tick. */
    select_kick_timer();
    refresh_player_ui();

    player_select_anim_steps--;
    if (player_select_anim_steps <= 0) {
        lv_timer_pause(player_select_anim_timer);
        select_kick_timer();
    } else {
        // Linear deceleration
        player_select_anim_period += (200 / (player_select_anim_steps + 1));
        if (player_select_anim_period > 600) player_select_anim_period = 600;
        lv_timer_set_period(player_select_anim_timer, player_select_anim_period);
    }
}

void start_player_selection_animation(void)
{
    int track = nvs_get_players_to_track();
    int random_stops;

    if (track <= 1) return;
    if (!nvs_get_random_first()) return;

    if (player_select_anim_timer == NULL) {
        player_select_anim_timer = lv_timer_create(player_select_anim_cb, 50, NULL);
    }

    // Randomize length to ensure random landing
    random_stops = (int)(esp_random() % track) + (track * 3);
    random_stops += esp_random() % (track * 2);

    player_select_anim_steps = random_stops;
    player_select_anim_period = 40; // start fast

    roulette_idx = 0;
    selection_set_single(0);
    select_kick_timer();

    lv_timer_set_period(player_select_anim_timer, player_select_anim_period);
    lv_timer_resume(player_select_anim_timer);
}

void stop_player_selection_animation(void)
{
    player_select_anim_steps = 0;
    if (player_select_anim_timer != NULL) {
        lv_timer_del(player_select_anim_timer);
        player_select_anim_timer = NULL;
    }
}

bool player_selection_animation_active(void)
{
    return player_select_anim_timer != NULL && player_select_anim_steps > 0;
}

// ---------- table sync (ESP-NOW) ----------
_Static_assert(NET_SYNC_MAX_PLAYERS == MAX_DISPLAY_PLAYERS, "packet layout");
_Static_assert(NET_SYNC_MAX_SOURCES == MAX_GAME_PLAYERS, "packet layout");
_Static_assert(sizeof(((net_sync_player_t *)0)->counters) / sizeof(int16_t)
               == COUNTER_TYPE_COUNT, "packet layout");
_Static_assert(sizeof(((net_sync_names_t *)0)->names) == sizeof(player_names),
               "packet layout");

void net_sync_fill_names(net_sync_names_t *out)
{
    int i;
    /* Copy per-row through snprintf, not one memcpy: bytes past each
       name's NUL are residue from earlier longer names and must not
       go out on the air (also keeps roster comparison canonical). */
    memset(out, 0, sizeof(*out));
    out->version = names_version;
    for (i = 0; i < MAX_GAME_PLAYERS; i++)
        snprintf(out->names[i], NET_SYNC_NAME_LEN, "%s", player_names[i]);
}

/* Adopt a remote roster. Whole-set LWW on the roster version (serial
   arithmetic, MAC tiebreak), like a single state block. Runs on the
   main task, same as net_sync_apply_state. */
void net_sync_apply_names(const net_sync_names_t *in, int wins_ties)
{
    int16_t newer = (int16_t)(in->version - names_version);
    int i;

    if (newer < 0) {
        /* Same self-healing as state: answer a stale roster so the
           sender converges without waiting for a rename or invite. */
        net_sync_send_reply();
        return;
    }
    if (newer == 0 && !wins_ties) return;
    names_version = in->version;
    if (memcmp(player_names, in->names, sizeof(player_names)) == 0) return;
    memcpy(player_names, in->names, sizeof(player_names));
    /* Wire bytes are untrusted: every name must terminate. */
    for (i = 0; i < MAX_GAME_PLAYERS; i++)
        player_names[i][sizeof(player_names[i]) - 1] = '\0';
    /* Same refresh set as a local rename (rename.c). */
    refresh_player_ui();
    refresh_select_ui();
    refresh_damage_ui();
    refresh_rename_ui();
}

void net_sync_fill_state(net_sync_state_t *out)
{
    int p, s, c;

    memset(out, 0, sizeof(*out));
    out->epoch = game_epoch;
    for (p = 0; p < MAX_DISPLAY_PLAYERS; p++) {
        net_sync_player_t *rp = &out->players[p];
        rp->version = player_version[p];
        rp->life = (int16_t)player_life[p];
        for (c = 0; c < COUNTER_TYPE_COUNT; c++)
            rp->counters[c] = (int16_t)player_counters[p][c];
        for (s = 0; s < MAX_GAME_PLAYERS; s++) {
            int v = cmd_damage_totals[s][p];
            rp->cmd_damage[s] = (uint8_t)((v < 0) ? 0 : (v > 255) ? 255 : v);
        }
        if (player_eliminated[p]) rp->eliminated |= NET_SYNC_ELIM;
        if (player_manually_eliminated[p]) rp->eliminated |= NET_SYNC_ELIM_MANUAL;
    }
}

/* Adopt a remote state snapshot. Runs on the main (LVGL) task — packets
   are queued by the radio and drained from loop() — so UI refresh is
   safe here. The game epoch dominates: a newer epoch (new game started
   or reset elsewhere) is adopted wholesale, an older one is rejected
   and answered with our own state (anti-entropy: the stale device
   converges in one exchange instead of waiting out the beacon). Within
   the same epoch, a player's block is adopted iff its version is newer
   (serial arithmetic, so uint16 wrap is fine); equal versions defer to
   the sender with the higher MAC so both devices pick the same winner.

   State is adopted directly — never route remote state through the
   apply/undo helpers: they bump versions and re-broadcast, and they'd
   re-derive elimination that the sender already decided. The damage
   log stays a per-device view of local actions. */
void net_sync_apply_state(const net_sync_state_t *in, int wins_ties)
{
    bool changed = false;
    bool remote_stale = false;
    int16_t epoch_newer = (int16_t)(in->epoch - game_epoch);
    int p, s, c;

    if (epoch_newer < 0) {
        net_sync_send_reply();
        return;
    }
    if (epoch_newer > 0) {
        game_epoch = in->epoch;
        /* A new game from the table: local elimination-undo actions
           and the event log refer to a game that no longer exists
           (see net_sync_reset_versions). */
        for (p = 0; p < MAX_DISPLAY_PLAYERS; p++)
            clear_player_elimination_action(p);
        damage_log_reset();
    }

    for (p = 0; p < MAX_DISPLAY_PLAYERS; p++) {
        const net_sync_player_t *rp = &in->players[p];
        int16_t newer = (int16_t)(rp->version - player_version[p]);
        bool was_eliminated = player_eliminated[p];
        bool now_eliminated = (rp->eliminated & NET_SYNC_ELIM) != 0;
        bool p_changed = false;
        int life = clamp_life(rp->life);

        /* Same epoch: per-player Lamport rule. A newer epoch adopts
           every block regardless of version drift. */
        if (epoch_newer == 0 && (newer < 0 || (newer == 0 && !wins_ties))) {
            if (newer < 0) remote_stale = true;
            continue;
        }

        if (player_life[p] != life) {
            player_life[p] = life;
            /* A pending preview was dialed against a total that no
               longer exists: if this player is in the current
               selection, drop the whole group's proposal (it is one
               shared delta) so a duplicate entry can't silently
               double-apply — the user sees the total snap and can
               re-dial. Changes to unselected players compose as
               usual. The selection itself stays: the grouping is
               still valid intent, only the number was invalidated. */
            if (life_preview_active && player_selected[p]) {
                pending_life_delta = 0;
                life_preview_active = false;
                if (life_preview_timer != NULL) {
                    lv_timer_pause(life_preview_timer);
                }
                select_kick_timer();
            }
            p_changed = true;
        }
        for (c = 0; c < COUNTER_TYPE_COUNT; c++) {
            int v = clamp_counter(rp->counters[c]);
            if (player_counters[p][c] != v) {
                player_counters[p][c] = v;
                p_changed = true;
            }
        }
        for (s = 0; s < MAX_GAME_PLAYERS; s++) {
            if (cmd_damage_totals[s][p] != rp->cmd_damage[s]) {
                cmd_damage_totals[s][p] = rp->cmd_damage[s];
                p_changed = true;
            }
        }
        if (was_eliminated != now_eliminated) {
            player_eliminated[p] = now_eliminated;
            if (!now_eliminated) {
                clear_player_elimination_action(p);
            } else if (player_selected[p]) {
                /* Same rule as check_player_elimination: an eliminated
                   player leaves the selection set. */
                player_selected[p] = false;
                select_kick_timer();
            }
            p_changed = true;
        } else if (was_eliminated && p_changed) {
            /* Still eliminated but the block changed underneath (a
               missed revive+re-kill): the stored undo action belongs
               to the old elimination and would restore the wrong
               amount. Undo then falls back to the revive clamps. */
            clear_player_elimination_action(p);
        }
        player_manually_eliminated[p] =
            now_eliminated && (rp->eliminated & NET_SYNC_ELIM_MANUAL) != 0;
        player_version[p] = rp->version;
        changed = changed || p_changed;
    }

    if (changed) {
        refresh_player_ui();
        refresh_select_ui();
    }
    /* The sender is behind and we adopted nothing: answer immediately
       so its lost-update window is one exchange, not a 5s beacon. */
    if (remote_stale && !changed) {
        net_sync_send_reply();
    }
    /* A fresh joiner can adopt the table's epoch while still awaiting
       the roster (every invite-window names packet lost): announcing
       its version-0 roster makes any peer see it as stale and reply
       with the real one (see net_sync_apply_names). */
    if (epoch_newer > 0 && names_version == 0) {
        net_sync_send_names();
    }
}

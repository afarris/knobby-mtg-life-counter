#include "ui_mp.h"
#include "ui_player_menu.h"
#include "ui_1p.h"
#include "game.h"
#include "storage.h"
#include "hw.h"

static lv_obj_t *add_low_battery_icon(lv_obj_t *parent)
{
    lv_obj_t *batt = lv_label_create(parent);
    lv_label_set_text(batt, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_color(batt, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_font(batt, &lv_font_montserrat_22, 0);
    lv_obj_align(batt, LV_ALIGN_TOP_MID, 0, 28);
    battery_icon_register(batt);
    return batt;
}

static lv_obj_t *mp_battery_icon = NULL;

#include <string.h>

// ---------- screens ----------
lv_obj_t *screen_multiplayer = NULL;

// ---------- layout specs ----------
typedef struct {
    lv_coord_t x, y, w, h;
    lv_coord_t nudge_x;     /* x offset for life/name labels in non-centric modes */
    int player_index;       /* which player this panel displays */
    int color_index;        /* color slot (differs from player only for 2p) */
    /* Pie-slice panels: full-screen with an angular wedge mask. Angles use
       the LVGL arc convention (0 = 3 o'clock, clockwise, degrees). Both 0
       means a plain rectangular panel. All slice geometry — label anchors,
       text rotation, counter arc, separators — derives from these angles
       at layout-build time. */
    int16_t wedge_start;
    int16_t wedge_end;
} mp_panel_spec_t;

static bool spec_is_wedge(const mp_panel_spec_t *spec)
{
    return spec->wedge_start != spec->wedge_end;
}

/* ---------- wedge geometry, derived once per layout rebuild ----------
   Everything that depends on the slice angles (label anchors, text
   rotation, counter arc, separators) reads this cache, so the spec's
   wedge_start/wedge_end stay the single source of truth and refresh/draw
   paths do no trigonometry. */
#define WEDGE_CX 180
#define WEDGE_CY 180
#define WEDGE_LABEL_RADIUS 88

typedef struct {
    int16_t bis_deg;                /* slice bisector angle */
    lv_coord_t label_dx, label_dy;  /* label anchor offset from center */
} wedge_geom_t;

static wedge_geom_t wedge_geom[MULTIPLAYER_COUNT];
static lv_point_t wedge_sep_ends[MULTIPLAYER_COUNT];
static int wedge_sep_count = 0;

/* Round an lv_trigo_sin/cos value (scaled 1<<15) projected to a radius */
static lv_coord_t wedge_polar(int16_t trig, int radius)
{
    int32_t v = (int32_t)trig * radius;
    return (lv_coord_t)((v + (v >= 0 ? 16384 : -16384)) / 32768);
}

static int16_t wedge_bisector_deg(const mp_panel_spec_t *spec)
{
    int delta = (spec->wedge_end >= spec->wedge_start)
              ? spec->wedge_end - spec->wedge_start
              : 360 - spec->wedge_start + spec->wedge_end;
    return (int16_t)((spec->wedge_start + delta / 2) % 360);
}

static void wedge_compute_geometry(const mp_panel_spec_t *panels, int panel_count)
{
    int i;

    wedge_sep_count = 0;
    for (i = 0; i < panel_count && i < MULTIPLAYER_COUNT; i++) {
        const mp_panel_spec_t *spec = &panels[i];
        int16_t bis = wedge_bisector_deg(spec);

        wedge_geom[i].bis_deg = bis;
        wedge_geom[i].label_dx = wedge_polar(lv_trigo_cos(bis), WEDGE_LABEL_RADIUS);
        wedge_geom[i].label_dy = wedge_polar(lv_trigo_sin(bis), WEDGE_LABEL_RADIUS);

        /* One boundary per panel covers every separator exactly once */
        wedge_sep_ends[i].x = WEDGE_CX + wedge_polar(lv_trigo_cos(spec->wedge_start), 180);
        wedge_sep_ends[i].y = WEDGE_CY + wedge_polar(lv_trigo_sin(spec->wedge_start), 180);
        wedge_sep_count++;
    }
}

typedef struct {
    int panel_count;
    const mp_panel_spec_t *panels;
    int16_t (*angle_fn)(int orientation_mode, int panel_index);
    bool switch_font_by_orientation;
} mp_layout_spec_t;

/* ---------- shared widget state ---------- */
static struct {
    lv_obj_t *panels[MULTIPLAYER_COUNT];
    lv_obj_t *life_labels[MULTIPLAYER_COUNT];
    lv_obj_t *name_labels[MULTIPLAYER_COUNT];
    lv_obj_t *counter_rows[MULTIPLAYER_COUNT][COUNTER_TYPE_COUNT];
    lv_obj_t *counter_values[MULTIPLAYER_COUNT][COUNTER_TYPE_COUNT];
    const mp_layout_spec_t *layout;
} mp_state;

static lv_timer_t *select_timeout_timer = NULL;

/* ---------- small helpers ---------- */
static const lv_font_t *get_counter_badge_font(const counter_definition_t *definition)
{
    if (definition != NULL && definition->icon_text != NULL) {
        return &mana_counter_icons_16;
    }

    return &lv_font_montserrat_14;
}

static const char *get_counter_badge_text(const counter_definition_t *definition)
{
    if (definition == NULL) return "?";
    if (definition->icon_text != NULL) return definition->icon_text;
    if (definition->badge_text != NULL) return definition->badge_text;
    return "?";
}

static void apply_object_rotation(lv_obj_t *obj, int16_t angle, int pivot_x, int pivot_y)
{
    if (obj == NULL) return;

    lv_obj_set_style_transform_angle(obj, angle, 0);
    if (angle != 0) {
        lv_obj_update_layout(obj);
        lv_obj_set_style_transform_pivot_x(obj,
            lv_obj_get_width(obj) / 2 + pivot_x, 0);
        lv_obj_set_style_transform_pivot_y(obj,
            lv_obj_get_height(obj) / 2 + pivot_y, 0);
    }
}

static void create_counter_row(lv_obj_t *parent, counter_type_t type,
                               lv_obj_t **row_out, lv_obj_t **value_out, int player_index)
{
    const counter_definition_t *definition = get_counter_definition(type);
    lv_obj_t *row;
    lv_obj_t *glyph;

    row = make_plain_box(parent, 34, 34);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

    glyph = lv_label_create(row);
    lv_label_set_text(glyph, get_counter_badge_text(definition));
    lv_obj_set_style_text_color(glyph, get_player_text_color(player_index), 0);
    lv_obj_set_style_text_font(glyph,
        (type == COUNTER_TYPE_POISON) ? &mana_poison_icon_bold_16
                                      : get_counter_badge_font(definition), 0);
    lv_obj_align(glyph, LV_ALIGN_TOP_MID, 0, 0);

    *value_out = lv_label_create(row);
    lv_label_set_text(*value_out, "0");
    lv_obj_set_style_text_color(*value_out, get_player_text_color(player_index), 0);
    lv_obj_set_style_text_font(*value_out, &lv_font_montserrat_14, 0);
    lv_obj_align(*value_out, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_align(*value_out, LV_TEXT_ALIGN_CENTER, 0);

    *row_out = row;
}

static void apply_label_rotation(lv_obj_t *life_lbl, lv_obj_t *name_lbl,
                                  int16_t angle, int life_pivot_y, int name_pivot_y)
{
    apply_object_rotation(life_lbl, angle, 0, life_pivot_y);
    apply_object_rotation(name_lbl, angle, 0, name_pivot_y);
}

static void get_counter_equator_anchor(lv_obj_t *panel,
                                       lv_coord_t *anchor_x, lv_coord_t *anchor_y)
{
    lv_obj_t *parent;
    lv_coord_t panel_y;
    lv_coord_t panel_h;
    lv_coord_t parent_h;
    lv_coord_t panel_center_y;
    lv_coord_t target_world_y;
    const lv_coord_t equator_gap = 24;
    const lv_coord_t edge_margin = 24;

    if (anchor_x == NULL || anchor_y == NULL) return;

    *anchor_x = 0;
    *anchor_y = 0;
    if (panel == NULL) return;

    parent = lv_obj_get_parent(panel);
    if (parent == NULL) return;

    panel_y = lv_obj_get_y(panel);
    panel_h = lv_obj_get_height(panel);
    parent_h = lv_obj_get_height(parent);
    panel_center_y = panel_y + (panel_h / 2);

    target_world_y = (parent_h / 2) + ((panel_center_y < (parent_h / 2)) ? -equator_gap : equator_gap);
    if (target_world_y < panel_y + edge_margin) target_world_y = panel_y + edge_margin;
    if (target_world_y > panel_y + panel_h - edge_margin) target_world_y = panel_y + panel_h - edge_margin;

    *anchor_x = 0;
    *anchor_y = target_world_y - panel_center_y;
}

static int16_t get_counter_row_angle(int orientation_mode, const mp_panel_spec_t *spec,
                                     lv_obj_t *panel, int16_t panel_angle)
{
    /* Wedge panels: counters follow the slice angle in every orientation,
       matching the life/name labels */
    if (spec_is_wedge(spec)) return panel_angle;

    if (orientation_mode == ORIENTATION_MODE_CENTRIC) {
        lv_obj_t *parent;

        if (panel == NULL) return 0;

        parent = lv_obj_get_parent(panel);
        if (parent == NULL) return 0;

        if ((lv_obj_get_y(panel) + (lv_obj_get_height(panel) / 2)) < (lv_obj_get_height(parent) / 2)) {
            return 1800;
        }

        return 0;
    }

    return panel_angle;
}

/* ---------- per-mode angle functions ---------- */
static int16_t get_2p_orientation_angle(int mode, int panel_index)
{
    if (mode == ORIENTATION_MODE_ABSOLUTE) return 0;
    return (panel_index == 0) ? 1800 : 0;
}

static int16_t get_3p_orientation_angle(int mode, int panel_index)
{
    switch (mode) {
        case ORIENTATION_MODE_CENTRIC:
            /* Bisector minus 90 so text reads upright from each seat */
            return (int16_t)(((wedge_geom[panel_index].bis_deg + 270) % 360) * 10);
        case ORIENTATION_MODE_TABLETOP:
            return (panel_index < 2) ? 1800 : 0;
        default:
            return 0;
    }
}

static int16_t get_4p_orientation_angle(int mode, int panel_index)
{
    static const int16_t angled_rot[MULTIPLAYER_COUNT] = {450, 1350, 2250, 3150};

    switch (mode) {
        case ORIENTATION_MODE_CENTRIC:
            return angled_rot[panel_index];
        case ORIENTATION_MODE_TABLETOP:
            return (panel_index == 1 || panel_index == 2) ? 1800 : 0;
        default:
            return 0;
    }
}

/* ---------- per-mode panel specs ---------- */
/* 2p: top = P2 (player 1), bottom = P1 (player 0). Colors intentionally swapped. */
static const mp_panel_spec_t panels_2p[] = {
    {0,   0, 360, 178, 0, 1, 0},
    {0, 182, 360, 178, 0, 0, 1},
};

/* 3p: three equal 120-degree pie slices — top-left = P2, top-right = P3,
   bottom = P1. Bisectors at 210/330/90 degrees. */
static const mp_panel_spec_t panels_3p[] = {
    {0, 0, 360, 360, 0, 1, 1, 150, 270},
    {0, 0, 360, 360, 0, 2, 2, 270,  30},
    {0, 0, 360, 360, 0, 0, 0,  30, 150},
};

/* 4p: quadrants in order P1, P2, P3, P4 */
static const mp_panel_spec_t panels_4p[] = {
    {  0, 180, 180, 180,  10, 0, 0},
    {  0,   0, 180, 180,  10, 1, 1},
    {180,   0, 180, 180, -10, 2, 2},
    {180, 180, 180, 180, -10, 3, 3},
};

static const mp_layout_spec_t layout_2p = {
    .panel_count = 2,
    .panels = panels_2p,
    .angle_fn = get_2p_orientation_angle,
    .switch_font_by_orientation = false,
};

static const mp_layout_spec_t layout_3p = {
    .panel_count = 3,
    .panels = panels_3p,
    .angle_fn = get_3p_orientation_angle,
    .switch_font_by_orientation = true,
};

static const mp_layout_spec_t layout_4p = {
    .panel_count = 4,
    .panels = panels_4p,
    .angle_fn = get_4p_orientation_angle,
    .switch_font_by_orientation = true,
};

static const mp_layout_spec_t *get_layout(int track)
{
    if (track == 2) return &layout_2p;
    if (track == 3) return &layout_3p;
    return &layout_4p;
}

/* ---------- per-panel refresh ---------- */
static void refresh_counter_rows(const mp_panel_spec_t *spec, int16_t wedge_bis,
                                 lv_obj_t *panel, lv_obj_t **rows, lv_obj_t **value_labels,
                                 int player_index, lv_color_t text_color,
                                 int16_t panel_angle, int16_t row_angle)
{
    int type;
    int visible_count = 0;
    int visible_types[COUNTER_TYPE_COUNT];
    char buf[8];
    const lv_coord_t step = 30;
    lv_coord_t anchor_x = 0;
    lv_coord_t anchor_y = 0;

    (void)panel_angle;
    if (!spec_is_wedge(spec)) {
        get_counter_equator_anchor(panel, &anchor_x, &anchor_y);
    }

    for (type = 0; type < COUNTER_TYPE_COUNT; type++) {
        if (rows[type] == NULL || value_labels[type] == NULL) continue;

        if (!counter_type_is_enabled((counter_type_t)type) ||
            get_counter_value(player_index, (counter_type_t)type) <= 0) {
            lv_obj_add_flag(rows[type], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        visible_types[visible_count] = type;
        visible_count++;
    }

    for (type = 0; type < visible_count; type++) {
        int value;
        int counter_type = visible_types[type];
        lv_coord_t x_offset = (lv_coord_t)((type * step) - ((visible_count - 1) * step / 2));
        lv_coord_t local_x;
        lv_coord_t local_y;

        if (spec_is_wedge(spec)) {
            /* Badges on an arc near the rim, centered on the wedge
               bisector: constant clearance from both the rim and the
               life/name labels regardless of badge count. */
            const int radius = 152;
            const int step_deg = 12;
            int a = (wedge_bis + ((visible_count - 1) * step_deg / 2)
                     - (type * step_deg) + 360) % 360;
            local_x = wedge_polar(lv_trigo_cos((int16_t)a), radius);
            local_y = wedge_polar(lv_trigo_sin((int16_t)a), radius);
        } else {
            local_x = anchor_x + x_offset;
            local_y = anchor_y;
        }

        value = get_counter_value(player_index, (counter_type_t)counter_type);

        snprintf(buf, sizeof(buf), "%d", value);
        lv_label_set_text(value_labels[counter_type], buf);
        lv_obj_set_style_text_color(value_labels[counter_type], text_color, 0);
        {
            lv_obj_t *glyph = lv_obj_get_child(rows[counter_type], 0);
            if (glyph != NULL) {
                lv_obj_set_style_text_color(glyph, text_color, 0);
            }
        }
        lv_obj_clear_flag(rows[counter_type], LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(rows[counter_type], LV_ALIGN_CENTER, local_x, local_y);
        apply_object_rotation(rows[counter_type], row_angle, 0, 0);
    }
}

static lv_color_t refresh_mp_panel(lv_obj_t *panel, lv_obj_t *life_lbl, lv_obj_t *name_lbl, int i, int color_i)
{
    char buf[8];
    bool selected = is_player_selected(i);
    bool preview_here = life_preview_active && selected;
    lv_color_t bg_color;
    lv_color_t text_color;

    {
        int vib;
        if (selection_count() == 0) vib = LIFE_VIB_MID;
        else vib = selected ? LIFE_VIB_VIV : LIFE_VIB_DIM;
        bg_color = get_effective_player_color(i, color_i, vib);
        text_color = color_is_light(bg_color) ? lv_color_black() : lv_color_white();
    }

    if (player_eliminated[i]) {
        bg_color = lv_color_hex(0x404040);
        text_color = lv_color_hex(0x808080);
    }

    /* Only write the color when it actually changed: every style write
       invalidates the whole panel, which on full-screen wedge panels means
       a full-screen redraw. (bg_opa is set once at build time.) */
    if (panel != NULL &&
        lv_obj_get_style_bg_color(panel, LV_PART_MAIN).full != bg_color.full) {
        lv_obj_set_style_bg_color(panel, bg_color, 0);
    }

    if (life_lbl != NULL) {
        if (preview_here) {
            snprintf(buf, sizeof(buf), "%+d", pending_life_delta);
            lv_label_set_text(life_lbl, buf);
            {
                lv_color_t preview_c;
                if (nvs_get_color_mode() == COLOR_MODE_PLAYER && !player_has_override[i]) {
                    preview_c = get_player_preview_color(color_i, pending_life_delta);
                    if (color_is_light(bg_color) && color_is_light(preview_c))
                        preview_c = lv_color_black();
                    else if (!color_is_light(bg_color) && !color_is_light(preview_c))
                        preview_c = lv_color_white();
                } else {
                    preview_c = color_is_light(bg_color) ? lv_color_black() : lv_color_white();
                }
                lv_obj_set_style_text_color(life_lbl, preview_c, 0);
            }
        } else {
            snprintf(buf, sizeof(buf), "%d", player_life[i]);
            lv_label_set_text(life_lbl, buf);
            lv_obj_set_style_text_color(life_lbl, text_color, 0);
        }
    }

    if (name_lbl != NULL) {
        if (preview_here) {
            char total_buf[16];
            int new_total = player_life[i] + pending_life_delta;
            snprintf(total_buf, sizeof(total_buf), "= %d", new_total);
            lv_label_set_text(name_lbl, total_buf);
        } else {
            lv_label_set_text(name_lbl, player_names[i]);
        }
        lv_obj_set_style_text_color(name_lbl, text_color, 0);
    }

    return text_color;
}

/* ---------- unified refresh ---------- */
void refresh_multiplayer_ui(void)
{
    const mp_layout_spec_t *layout = mp_state.layout;
    int orientation_mode;
    int i;

    if (layout == NULL) return;
    orientation_mode = nvs_get_orientation();

    for (i = 0; i < layout->panel_count; i++) {
        const mp_panel_spec_t *spec = &layout->panels[i];
        lv_obj_t *panel = mp_state.panels[i];
        lv_obj_t *life_lbl = mp_state.life_labels[i];
        lv_obj_t *name_lbl = mp_state.name_labels[i];
        int16_t angle = layout->angle_fn(orientation_mode, i);
        int16_t counter_angle = get_counter_row_angle(orientation_mode, spec, panel, angle);
        lv_coord_t nx = (orientation_mode != ORIENTATION_MODE_CENTRIC) ? spec->nudge_x : 0;
        lv_coord_t bx = nx;
        lv_coord_t by = 0;
        lv_color_t text_color;

        if (spec_is_wedge(spec)) {
            /* Wedge panels are full-screen: anchor the label stack on the
               wedge bisector instead of the panel center. */
            bx = wedge_geom[i].label_dx;
            by = wedge_geom[i].label_dy;
            if (angle == 1800) {
                /* The 180-degree flip rotates the life/name pair around
                   their shared pivot, moving the name ~30px toward the
                   rim; pull the anchor inward to keep the same clearance
                   from the counter arc as in absolute mode. */
                bx = (lv_coord_t)((bx * 85) / 100);
                by = (lv_coord_t)((by * 85) / 100);
            }
        }

        text_color = refresh_mp_panel(panel, life_lbl, name_lbl,
                                      spec->player_index, spec->color_index);

        if (layout->switch_font_by_orientation) {
            const lv_font_t *life_font;
            lv_coord_t life_pivot_y;

            if (spec_is_wedge(spec)) {
                /* Pie slices have room for the big font in every
                   orientation; drop to the smaller one only when the
                   value is too wide and would reach into the counter
                   arc beside the number (3+ digits). */
                life_font = &lv_font_montserrat_bold_56;
                if (life_lbl != NULL) {
                    lv_point_t ts;
                    lv_txt_get_size(&ts, lv_label_get_text(life_lbl),
                                    life_font, 0, 0, LV_COORD_MAX,
                                    LV_TEXT_FLAG_NONE);
                    if (ts.x > 84) life_font = &lv_font_montserrat_bold_44;
                }
            } else if (orientation_mode == ORIENTATION_MODE_CENTRIC) {
                /* Rect quadrants: rotated bold-56 labels don't fit */
                life_font = &lv_font_montserrat_bold_44;
            } else {
                life_font = &lv_font_montserrat_bold_56;
            }
            life_pivot_y = (life_font == &lv_font_montserrat_bold_56) ? 12 : 10;

            if (life_lbl != NULL) {
                lv_obj_clear_flag(life_lbl, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_font(life_lbl, life_font, 0);
                lv_obj_align(life_lbl, LV_ALIGN_CENTER, bx, by - life_pivot_y);
            }
            if (name_lbl != NULL) {
                lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(name_lbl, LV_ALIGN_CENTER, bx, by + 30);
            }
            apply_label_rotation(life_lbl, name_lbl, angle, life_pivot_y, -30);
        } else {
            apply_label_rotation(life_lbl, name_lbl, angle, 10, -30);
        }

        refresh_counter_rows(spec, wedge_geom[i].bis_deg, panel,
                             mp_state.counter_rows[i], mp_state.counter_values[i],
                             spec->player_index, text_color, angle, counter_angle);
    }
}

/* ---------- events ---------- */
static void event_multiplayer_select(lv_event_t *e)
{
    int player = (int)(intptr_t)lv_event_get_user_data(e);
    bool had_pending;
    bool was_selected;

    if (player < 0 || player >= MULTIPLAYER_COUNT) return;
    if (player_eliminated[player]) return;

    /* A tap during the first-player roulette stops the spin and leaves
       nothing selected (the spinning highlight is not a real selection). */
    if (player_selection_animation_active()) {
        stop_player_selection_animation();
        selection_clear();
        refresh_multiplayer_ui();
        return;
    }

    /* Capture state before committing: the commit clears the selection in
       multi-select mode, so we can't read it afterwards. */
    had_pending = life_preview_active;
    was_selected = is_player_selected(player);

    /* Apply any pending delta to the current set before the selection
       changes. */
    if (life_preview_active) {
        life_preview_commit_cb(NULL);
    }

    if (nvs_get_multi_select()) {
        if (had_pending) {
            /* A pending change was just applied (and the set cleared).
               Tapping a player that was part of the set ends the operation
               with nothing selected; tapping a different player starts a
               fresh selection with just that player. */
            if (!was_selected) {
                selection_set_single(player);
            }
        } else {
            /* No pending change: tap toggles this player in/out of the set. */
            selection_toggle(player);
        }
    } else {
        /* Single-select (default): tapping the only selected player deselects;
           tapping any other player switches the selection to it. */
        if (was_selected && selection_count() == 1) {
            selection_clear();
        } else {
            selection_set_single(player);
        }
    }
    select_kick_timer();
    refresh_multiplayer_ui();
}

static void event_multiplayer_open_menu(lv_event_t *e)
{
    int player = (int)(intptr_t)lv_event_get_user_data(e);

    if (player < 0 || player >= MULTIPLAYER_COUNT) return;

    if (player_selection_animation_active()) {
        stop_player_selection_animation();
        selection_clear();
    }

    /* Resolve any pending dialed delta before leaving the screen, so the
       auto-commit can't fire later against a context the user left. */
    if (life_preview_active) {
        life_preview_commit_cb(NULL);
    }

    /* A long-press is a deliberate gesture, so it always opens the
       pressed player's menu (e.g. to apply commander damage), even when
       one or more players are selected for life changes; the selection
       is left untouched. */
    if (player_eliminated[player]) {
        menu_player = player;
        load_screen_if_needed(screen_eliminated_player_menu);
        lv_indev_wait_release(lv_indev_get_act());
        return;
    }

    open_player_menu(player);
    lv_indev_wait_release(lv_indev_get_act());
}

/* ---------- selection timeout ---------- */
static void select_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Apply a still-pending dialed delta rather than silently dropping it.
       (Today the 3s preview always commits before the >=5s timeout, but
       nothing else enforces that ordering.) */
    if (life_preview_active) {
        life_preview_commit_cb(NULL);
    }
    selection_clear();
    if (select_timeout_timer != NULL)
        lv_timer_pause(select_timeout_timer);
    refresh_multiplayer_ui();
}

void select_kick_timer(void)
{
    int idx = nvs_get_deselect_timeout();
    int ms = deselect_ms[idx];

    if (select_timeout_timer == NULL) {
        select_timeout_timer = lv_timer_create(select_timeout_cb, 15000, NULL);
        lv_timer_pause(select_timeout_timer);
    }
    if (selection_count() > 0 && ms > 0) {
        lv_timer_set_period(select_timeout_timer, (uint32_t)ms);
        lv_timer_reset(select_timeout_timer);
        lv_timer_resume(select_timeout_timer);
    } else {
        lv_timer_pause(select_timeout_timer);
    }
}

/* ---------- pie-slice (wedge) panels ----------
   Full-screen panels clipped to an angular wedge with a draw-time angle
   mask (the arc widget's primitive), and hit-tested by angle so taps land
   on the right slice. This avoids rotating containers, whose transform
   layers would not fit in the LVGL heap. */
static lv_draw_mask_angle_param_t wedge_mask_params[MULTIPLAYER_COUNT];
static int16_t wedge_mask_ids[MULTIPLAYER_COUNT];

static bool wedge_contains_angle(const mp_panel_spec_t *spec, int angle)
{
    if (spec->wedge_start <= spec->wedge_end)
        return angle >= spec->wedge_start && angle < spec->wedge_end;
    /* range wraps past 0 degrees */
    return angle >= spec->wedge_start || angle < spec->wedge_end;
}

static void event_wedge_panel(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const mp_panel_spec_t *spec;
    lv_event_code_t code = lv_event_get_code(e);

    if (mp_state.layout == NULL || idx < 0 || idx >= mp_state.layout->panel_count)
        return;
    spec = &mp_state.layout->panels[idx];

    if (code == LV_EVENT_COVER_CHECK) {
        /* The wedge mask means this panel does not fully cover its
           rectangle, so siblings underneath must still be drawn. */
        lv_cover_check_info_t *info = lv_event_get_param(e);
        info->res = LV_COVER_RES_MASKED;
    } else if (code == LV_EVENT_DRAW_MAIN_BEGIN) {
        lv_draw_mask_angle_init(&wedge_mask_params[idx], WEDGE_CX, WEDGE_CY,
                                spec->wedge_start, spec->wedge_end);
        wedge_mask_ids[idx] = lv_draw_mask_add(&wedge_mask_params[idx], NULL);
    } else if (code == LV_EVENT_DRAW_MAIN_END) {
        /* Remove before children draw so labels are not clipped */
        lv_draw_mask_remove_id(wedge_mask_ids[idx]);
    } else if (code == LV_EVENT_HIT_TEST) {
        lv_hit_test_info_t *info = lv_event_get_param(e);
        int dx = info->point->x - WEDGE_CX;
        int dy = info->point->y - WEDGE_CY;
        if (dx == 0 && dy == 0) dx = 1; /* lv_atan2 needs a non-zero vector */
        /* lv_atan2(y, x) matches the mask/arc angle convention
           (0 = 3 o'clock, clockwise) — same call order as lv_arc */
        info->res = wedge_contains_angle(spec, lv_atan2(dy, dx));
    }
}

static void event_wedge_separators(lv_event_t *e)
{
    static const lv_point_t sep_center = {WEDGE_CX, WEDGE_CY};
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_draw_line_dsc_t dsc;
    int s;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_black();
    dsc.width = 2;
    for (s = 0; s < wedge_sep_count; s++) {
        lv_draw_line(draw_ctx, &dsc, &sep_center, &wedge_sep_ends[s]);
    }
}

/* ---------- layout rebuild ---------- */
void rebuild_multiplayer_layout(int track)
{
    const mp_layout_spec_t *layout = get_layout(track);
    int i;

    if (screen_multiplayer == NULL) return;

    if (mp_battery_icon != NULL) {
        battery_icon_unregister(mp_battery_icon);
        mp_battery_icon = NULL;
    }

    lv_obj_clean(screen_multiplayer);
    memset(&mp_state, 0, sizeof(mp_state));
    mp_state.layout = layout;

    if (layout->panel_count > 0 && spec_is_wedge(&layout->panels[0])) {
        wedge_compute_geometry(layout->panels, layout->panel_count);
    }

    for (i = 0; i < layout->panel_count; i++) {
        const mp_panel_spec_t *spec = &layout->panels[i];
        int p = spec->player_index;
        lv_obj_t *panel;
        lv_obj_t *name_lbl;
        lv_obj_t *life_lbl;

        panel = lv_btn_create(screen_multiplayer);
        lv_obj_remove_style_all(panel);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_size(panel, spec->w, spec->h);
        lv_obj_set_pos(panel, spec->x, spec->y);
        lv_obj_set_style_radius(panel, 0, 0);
        lv_obj_set_style_shadow_width(panel, 0, 0);
        if (spec_is_wedge(spec)) {
            /* Wedges adjoin; separators are drawn as lines on top */
            lv_obj_set_style_border_width(panel, 0, 0);
            lv_obj_add_flag(panel, LV_OBJ_FLAG_ADV_HITTEST);
            /* Register per event code so the dispatcher filters the many
               events (presses, draw phases) the handler doesn't act on */
            lv_obj_add_event_cb(panel, event_wedge_panel, LV_EVENT_COVER_CHECK, (void *)(intptr_t)i);
            lv_obj_add_event_cb(panel, event_wedge_panel, LV_EVENT_DRAW_MAIN_BEGIN, (void *)(intptr_t)i);
            lv_obj_add_event_cb(panel, event_wedge_panel, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)i);
            lv_obj_add_event_cb(panel, event_wedge_panel, LV_EVENT_HIT_TEST, (void *)(intptr_t)i);
        } else {
            lv_obj_set_style_border_width(panel, 1, 0);
            lv_obj_set_style_border_color(panel, lv_color_black(), 0);
        }
        lv_obj_add_event_cb(panel, event_multiplayer_select, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)p);
        lv_obj_add_event_cb(panel, event_multiplayer_open_menu, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)p);
        mp_state.panels[i] = panel;

        name_lbl = lv_label_create(panel);
        lv_label_set_text(name_lbl, player_names[p]);
        lv_obj_set_style_text_color(name_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_22, 0);
        lv_obj_align(name_lbl, LV_ALIGN_CENTER, 0, 30);
        mp_state.name_labels[i] = name_lbl;

        life_lbl = lv_label_create(panel);
        lv_label_set_text(life_lbl, "40");
        lv_obj_set_style_text_color(life_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(life_lbl, &lv_font_montserrat_bold_56, 0);
        lv_obj_align(life_lbl, LV_ALIGN_CENTER, 0, -10);
        mp_state.life_labels[i] = life_lbl;

        create_counter_row(panel, COUNTER_TYPE_COMMANDER_TAX,
            &mp_state.counter_rows[i][COUNTER_TYPE_COMMANDER_TAX],
            &mp_state.counter_values[i][COUNTER_TYPE_COMMANDER_TAX], p);
        create_counter_row(panel, COUNTER_TYPE_PARTNER_TAX,
            &mp_state.counter_rows[i][COUNTER_TYPE_PARTNER_TAX],
            &mp_state.counter_values[i][COUNTER_TYPE_PARTNER_TAX], p);
        create_counter_row(panel, COUNTER_TYPE_POISON,
            &mp_state.counter_rows[i][COUNTER_TYPE_POISON],
            &mp_state.counter_values[i][COUNTER_TYPE_POISON], p);
        create_counter_row(panel, COUNTER_TYPE_EXPERIENCE,
            &mp_state.counter_rows[i][COUNTER_TYPE_EXPERIENCE],
            &mp_state.counter_values[i][COUNTER_TYPE_EXPERIENCE], p);
    }

    if (layout->panel_count > 0 && spec_is_wedge(&layout->panels[0])) {
        /* Transparent overlay that draws the separator lines between
           slices (LV_USE_LINE is disabled, so draw them directly). */
        lv_obj_t *sep = make_plain_box(screen_multiplayer, 360, 360);
        lv_obj_set_pos(sep, 0, 0);
        lv_obj_add_event_cb(sep, event_wedge_separators, LV_EVENT_DRAW_MAIN, NULL);
    }

    mp_battery_icon = add_low_battery_icon(screen_multiplayer);

    refresh_multiplayer_ui();
}

/* ---------- screen lifecycle ---------- */
void build_multiplayer_screen(void)
{
    screen_multiplayer = lv_obj_create(NULL);
    lv_obj_set_size(screen_multiplayer, 360, 360);
    lv_obj_set_style_bg_color(screen_multiplayer, lv_color_black(), 0);
    lv_obj_set_style_border_width(screen_multiplayer, 0, 0);
    lv_obj_set_scrollbar_mode(screen_multiplayer, LV_SCROLLBAR_MODE_OFF);

    rebuild_multiplayer_layout(nvs_get_players_to_track());
}


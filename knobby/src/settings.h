#ifndef _SETTINGS_H
#define _SETTINGS_H

#include "types.h"

// ---------- screens ----------
extern lv_obj_t *screen_quad_menu;
extern lv_obj_t *screen_tools_menu;
extern lv_obj_t *screen_settings;
extern lv_obj_t *screen_battery;
extern lv_obj_t *screen_rotate;
extern lv_obj_t *screen_table_sync;

// ---------- declarative settings ----------
/* One table in settings.c defines every user setting; pages and
   navigation are derived from it. */
typedef struct {
    const char *id;              /* stable id for sim navigation: "autodim" */
    const char *fixed_label;     /* used when label == NULL (navigation items) */
    const char *(*label)(int v); /* value -> text */
    uint32_t (*color)(int v);    /* value -> bg color; NULL = default */
    int (*get)(void);            /* NULL => navigation item */
    void (*set)(int v);          /* writes NVS + side effects */
    int count;                   /* cycle modulo (2 for ON/OFF toggles) */
    void (*navigate)(void);      /* non-NULL => click opens a sub-screen */
    lv_obj_t **nav_screen;       /* sub-screen global, for generic back-nav */
    lv_event_code_t event;       /* trigger; 0 = LV_EVENT_CLICKED (use
                                    LV_EVENT_LONG_PRESSED for "Hold" items) */
} setting_item_t;

extern lv_obj_t *settings_pages[];
extern int settings_page_count;

// ---------- functions ----------
void build_quad_screen(lv_obj_t **screen, quad_item_t items[4]);
void build_quad_menus(void);
void build_settings_screen(void);
void build_battery_screen(void);
void build_rotate_screen(void);
void build_table_sync_screen(void);

void refresh_settings_ui(void);
void refresh_settings_pages_ui(void);
void refresh_battery_ui(void);
void refresh_rotate_ui(void);
void refresh_table_sync_ui(void);

bool settings_handle_back(lv_obj_t *screen);
bool settings_knob_page(int dir);
int settings_item_page(const char *id);

void open_quad_menu(void);
void open_settings_screen(void);
void open_battery_screen(void);
void open_rotate_screen(void);
void change_display_rotation(int dir);
void menu_facing_refresh(void);
void open_table_sync_screen(void);

#endif // _SETTINGS_H

#ifndef _UI_TRACKER_H
#define _UI_TRACKER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl.h>

extern lv_obj_t *screen_tracker_url_entry;
extern lv_obj_t *screen_tracker_roster;

void build_tracker_screens(void);

void open_tracker_url_entry_screen(void);
void open_tracker_roster_screen(const char *base_url);

#ifdef __cplusplus
}
#endif

#endif /* _UI_TRACKER_H */

#ifndef _KNOB_H
#define _KNOB_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"
#include "bidi_switch_knob.h"

#define KNOB_SWIPE_THRESHOLD 80
#define KNOB_SWIPE_MAX_LATERAL 120
#define KNOB_SWIPE_LEFT_EDGE_ZONE 80
#define KNOB_SWIPE_RIGHT_EDGE_ZONE 80

void knob_gui(void);
void knob_cb(lv_event_t *e);

void knob_change(knob_event_t k,int cont);
void knob_process_pending(void);
bool activity_kick(void);
void knob_swipe_hint_update(int start_x, int start_y, int cur_x, int cur_y);
void knob_swipe_hint_clear(void);
void knob_notify_swipe_up(void);
void knob_notify_swipe_down(void);
void knob_notify_swipe_left(void);
void knob_notify_swipe_right(void);
float knob_read_battery_voltage(void);
void scr_display_on(void);


#ifdef __cplusplus
}
#endif

#endif

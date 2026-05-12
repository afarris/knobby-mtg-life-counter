#ifndef _KNOB_H
#define _KNOB_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"
#include "bidi_switch_knob.h"

typedef enum {
	KNOB_SWIPE_NONE = 0,
	KNOB_SWIPE_LEFT,
	KNOB_SWIPE_RIGHT,
	KNOB_SWIPE_UP,
	KNOB_SWIPE_DOWN
} knob_swipe_direction_t;

#define KNOB_TOUCH_JITTER_PX 14
#define KNOB_SWIPE_THRESHOLD 84
#define KNOB_SWIPE_HINT_REVEAL_START 28
#define KNOB_SWIPE_MAX_LATERAL 72
#define KNOB_SWIPE_MIN_DURATION_MS 100
#define KNOB_SWIPE_LEFT_EDGE_ZONE 56
#define KNOB_SWIPE_RIGHT_EDGE_ZONE 56
#define KNOB_SWIPE_TOP_EDGE_ZONE 56
#define KNOB_SWIPE_BOTTOM_EDGE_ZONE 56
#define KNOB_SWIPE_AXIS_BIAS_NUM 3
#define KNOB_SWIPE_AXIS_BIAS_DEN 2

void knob_gui(void);
void knob_cb(lv_event_t *e);

void knob_change(knob_event_t k,int cont);
void knob_process_pending(void);
bool activity_kick(void);
knob_swipe_direction_t knob_classify_swipe_direction(lv_obj_t *screen,
													 int start_x, int start_y,
													 int dx, int dy,
													 int min_travel);
void knob_swipe_hint_update(int start_x, int start_y, int cur_x, int cur_y);
void knob_swipe_hint_clear(void);
bool knob_swipe_hint_fully_revealed(lv_obj_t *screen,
									int start_x, int start_y,
									int cur_x, int cur_y);
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

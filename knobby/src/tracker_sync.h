#ifndef _TRACKER_SYNC_H
#define _TRACKER_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ---------- sizing ---------- */
#define TRACKER_URL_LEN          96   /* http://host:port form, plenty of room */
#define TRACKER_ROSTER_MAX        8   /* matches MAX_GAME_PLAYERS */
#define TRACKER_PLAYER_NAME_LEN  32   /* mtg-tracker names can be longer than knobby's 15 */

/* ---------- state machine ---------- */
typedef enum {
    TRACKER_DISCONNECTED,   /* not paired */
    TRACKER_CONNECTING,     /* HTTP or WebSocket handshake in flight */
    TRACKER_CONNECTED,      /* WS open, roster fetched; ready for claim or syncing */
    TRACKER_ERROR,          /* last attempt failed; user action needed */
} tracker_state_t;

/* ---------- roster entry (one player as known to the server) ---------- */
typedef struct {
    int  player_id;          /* server's 1-based id */
    char name[TRACKER_PLAYER_NAME_LEN];
    int  life;
} tracker_roster_entry_t;

/* ---------- top-level lifecycle ---------- */
void             tracker_sync_init(void);                /* idempotent; safe to call lazily */
tracker_state_t  tracker_state(void);
const char      *tracker_server_url(void);               /* "" when not connected */
int              tracker_my_player_id(void);             /* 0 = not claimed, 1..MAX = claimed */

/* ---------- connect / disconnect ---------- */
void tracker_connect(const char *base_url);
void tracker_disconnect(void);

/* ---------- roster (valid once CONNECTED) ---------- */
int  tracker_roster_count(void);
void tracker_roster_entry(int idx, tracker_roster_entry_t *out);
void tracker_claim_player(int player_id);   /* 1-based */

/* ---------- outbound (called from game.c mutation points; 0-based knobby indices) ---------- */
void tracker_send_life_delta(int player_0b, int delta);
void tracker_send_cmd_damage(int target_0b, int source_0b, int delta, bool is_partner);
void tracker_send_poison(int player_0b, int delta);

/* ---------- reconciliation guard ---------- */
bool tracker_is_applying_remote(void);

/* ============================================================================
 * Backend hooks — implemented by knobby/tracker_backend.cpp on firmware
 * (HTTPClient + WebSocketsClient + ESPmDNS) and stubbed in sim/sim_stubs.c.
 * ============================================================================ */

void tracker_hw_init(void);
void tracker_hw_connect(const char *base_url);
void tracker_hw_disconnect(void);
bool tracker_hw_is_open(void);
/* Must be called periodically while a session is active (WS event pump). */
void tracker_hw_loop(void);

/* HTTP POSTs. Return true on 2xx, false otherwise. Non-blocking only for
 * WebSocket; these are synchronous and may take ~100ms on real hardware. */
bool tracker_hw_post_update(int player_id, int delta);
bool tracker_hw_post_commander_damage(int target_id, int source_id, int delta, bool is_partner);
bool tracker_hw_post_poison(int player_id, int delta);

/* WS connection lifecycle from the backend. 0=closed 1=connecting 2=open 3=error. */
void tracker_sync_on_ws_state(int hw_state);

/* Inbound state-frame callbacks. The backend (or sim) parses the JSON snapshot
 * and emits these in order:
 *   begin_frame
 *   roster_clear
 *   roster_add(player_id, name, life)   -- once per server-side player
 *   (if my player_id is claimed, the matching player record also emits:)
 *     apply_my_life(life)
 *     apply_my_poison(poison)
 *     apply_my_cmd_damage(source_1b, value)  -- per non-partner source
 *   end_frame
 *
 * tracker_sync keeps `applying_remote` true for the whole sequence so local
 * mutation points don't re-POST during reconciliation. A single UI refresh
 * happens in end_frame.
 */
void tracker_sync_begin_frame(void);
void tracker_sync_roster_clear(void);
void tracker_sync_roster_add(int server_player_id_1b, const char *name, int life);
void tracker_sync_apply_my_life(int life);
void tracker_sync_apply_my_poison(int poison);
void tracker_sync_apply_my_cmd_damage(int source_1b, int value);
void tracker_sync_end_frame(void);

/* Trigger an HTTP GET /state and feed the result through the same parser the
 * WebSocket uses. Called after tracker_claim_player so inbound reconciliation
 * runs with my_player_id set, without having to wait for the next server-side
 * change. No-op in the simulator. */
void tracker_hw_fetch_and_apply_state(void);

/* printf-like logger routed to Serial on firmware, no-op in the sim.
 * Prefixed output so it's easy to filter in the Arduino monitor. */
void tracker_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* _TRACKER_SYNC_H */

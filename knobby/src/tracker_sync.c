#include "tracker_sync.h"
#include "types.h"
#include "game.h"
#include <string.h>
#include <stdio.h>

/* ---------- RAM-only state (phase 2: no NVS) ---------- */

static bool             initialized       = false;
static tracker_state_t  current_state     = TRACKER_DISCONNECTED;
static char             server_url[TRACKER_URL_LEN] = {0};
static int              my_player_id      = 0;             /* 0 = not claimed */
static bool             applying_remote   = false;

/* Roster as last seen from the server (valid once CONNECTED). */
static tracker_roster_entry_t roster[TRACKER_ROSTER_MAX];
static int                    roster_count = 0;

/* ---------- index helpers ----------
 * Every knobby↔server boundary goes through these. Never compute +1 / -1 inline
 * at call sites; this is the single likeliest source of phase 2 bugs.
 */

static inline int loc_to_srv(int loc_0b) { return loc_0b + 1; }
static inline int srv_to_loc(int srv_1b) { return srv_1b - 1; }

static inline bool srv_id_is_valid(int srv_1b)
{
    return srv_1b >= 1 && srv_1b <= MAX_GAME_PLAYERS;
}

/* cmd_damage_totals is [MAX_GAME_PLAYERS][MAX_DISPLAY_PLAYERS]; dimensions differ. */
static inline bool src_0b_is_valid(int src_0b)    { return src_0b >= 0 && src_0b <  MAX_GAME_PLAYERS; }
static inline bool target_0b_is_valid(int tgt_0b) { return tgt_0b >= 0 && tgt_0b <  MAX_DISPLAY_PLAYERS; }

/* ---------- init ---------- */

void tracker_sync_init(void)
{
    if (initialized) return;
    tracker_hw_init();
    initialized = true;
}

/* ---------- accessors ---------- */

tracker_state_t tracker_state(void)              { return current_state; }
const char     *tracker_server_url(void)         { return server_url; }
int             tracker_my_player_id(void)       { return my_player_id; }
bool            tracker_is_applying_remote(void) { return applying_remote; }

/* ---------- connect / disconnect ---------- */

void tracker_connect(const char *base_url)
{
    tracker_sync_init();
    if (base_url == NULL || base_url[0] == '\0') return;

    tracker_log("connect url=%s", base_url);

    /* Drop any prior session */
    tracker_hw_disconnect();
    my_player_id = 0;
    roster_count = 0;
    memset(roster, 0, sizeof(roster));

    snprintf(server_url, sizeof(server_url), "%s", base_url);
    current_state = TRACKER_CONNECTING;
    tracker_hw_connect(base_url);
}

void tracker_disconnect(void)
{
    tracker_hw_disconnect();
    server_url[0] = '\0';
    my_player_id = 0;
    roster_count = 0;
    memset(roster, 0, sizeof(roster));
    current_state = TRACKER_DISCONNECTED;
}

/* ---------- roster ---------- */

int tracker_roster_count(void) { return roster_count; }

void tracker_roster_entry(int idx, tracker_roster_entry_t *out)
{
    if (out == NULL) return;
    if (idx < 0 || idx >= roster_count) { memset(out, 0, sizeof(*out)); return; }
    *out = roster[idx];
}

void tracker_claim_player(int player_id)
{
    if (!srv_id_is_valid(player_id)) {
        tracker_log("claim: invalid id=%d (valid 1..%d)", player_id, MAX_GAME_PLAYERS);
        return;
    }
    my_player_id = player_id;
    tracker_log("claim id=%d, fetching /state...", player_id);
    /* Pull the current snapshot now that we know who we are. Without this,
     * local life stays at whatever the user's game-mode defaults to (e.g.,
     * 20) until the next server-side change pushes a WS frame. */
    tracker_hw_fetch_and_apply_state();
}

/* ---------- outbound ----------
 * All three follow the same guard: if we're currently applying a WS frame
 * we must NOT re-post, or we trigger an echo loop. The guard is checked by
 * callers in game.c as well, but belt-and-suspenders here too.
 */

/* Phase 2 MVP: "knobby represents the claimed tracker player." Every
 * outbound life/poison change posts as my_player_id regardless of which
 * local slot the user edited. This is simple, matches the intended UX
 * (knobby is one player at the table), and avoids the prior silent-drop
 * when a user in multi-p mode wasn't on local slot 0.
 *
 * Caveats: in multi-p mode, editing another player's slot will sync that
 * change AS the claimed player, which is wrong if the user meant to adjust
 * an opponent's life. Acceptable for MVP; fix is a per-slot → server-id
 * mapping. cmd-damage source has the same mapping gap (see below). */

void tracker_send_life_delta(int player_0b, int delta)
{
    (void)player_0b;
    if (applying_remote)                  { tracker_log("send_life skip: applying_remote"); return; }
    if (current_state != TRACKER_CONNECTED) { tracker_log("send_life skip: state=%d (need CONNECTED=3)", (int)current_state); return; }
    if (my_player_id == 0)                { tracker_log("send_life skip: not claimed"); return; }
    if (delta == 0)                       { return; }
    tracker_log("send_life POST id=%d delta=%d", my_player_id, delta);
    bool ok = tracker_hw_post_update(my_player_id, delta);
    tracker_log("send_life result=%s", ok ? "ok" : "FAIL");
}

void tracker_send_cmd_damage(int target_0b, int source_0b, int delta, bool is_partner)
{
    (void)target_0b;
    if (applying_remote)                  { tracker_log("send_cmd skip: applying_remote"); return; }
    if (current_state != TRACKER_CONNECTED) { tracker_log("send_cmd skip: state=%d", (int)current_state); return; }
    if (my_player_id == 0)                { tracker_log("send_cmd skip: not claimed"); return; }
    if (!src_0b_is_valid(source_0b))      { tracker_log("send_cmd skip: bad source=%d", source_0b); return; }
    if (delta == 0)                       { return; }
    tracker_log("send_cmd POST target=%d source=%d delta=%d partner=%d",
                my_player_id, loc_to_srv(source_0b), delta, (int)is_partner);
    (void)tracker_hw_post_commander_damage(my_player_id, loc_to_srv(source_0b), delta, is_partner);
}

void tracker_send_poison(int player_0b, int delta)
{
    (void)player_0b;
    if (applying_remote)                  { tracker_log("send_poison skip: applying_remote"); return; }
    if (current_state != TRACKER_CONNECTED) { tracker_log("send_poison skip: state=%d", (int)current_state); return; }
    if (my_player_id == 0)                { tracker_log("send_poison skip: not claimed"); return; }
    if (delta == 0)                       { return; }
    tracker_log("send_poison POST id=%d delta=%d", my_player_id, delta);
    (void)tracker_hw_post_poison(my_player_id, delta);
}

/* ---------- inbound (WS frame reconciliation) ---------- */

void tracker_sync_on_ws_state(int hw_state)
{
    tracker_log("ws state -> %d (0=closed 1=conn 2=open 3=err)", hw_state);
    /* 0=closed 1=connecting 2=open 3=error */
    switch (hw_state) {
        case 0:
            if (current_state == TRACKER_CONNECTED || current_state == TRACKER_CONNECTING)
                current_state = TRACKER_CONNECTING;   /* WS backoff will retry */
            break;
        case 1:
            current_state = TRACKER_CONNECTING;
            break;
        case 2:
            current_state = TRACKER_CONNECTED;
            /* First WS frame will populate roster — no HTTP GET /state needed. */
            break;
        case 3:
            current_state = TRACKER_ERROR;
            break;
        default:
            break;
    }
}

/* Refresh hook: the UI layer (game.c / ui_mp.c) centralises re-rendering of
 * life / counters through refresh_main_ui + refresh_multiplayer_ui. We keep
 * tracker_sync pure-C-module-free of LVGL by having the game.c side expose
 * these. Weak symbol so the simulator can override if needed.
 */
extern void refresh_main_ui(void);
extern void refresh_multiplayer_ui(void);
extern void refresh_player_ui(void);

void tracker_sync_begin_frame(void)
{
    applying_remote = true;
}

void tracker_sync_roster_clear(void)
{
    roster_count = 0;
    memset(roster, 0, sizeof(roster));
}

void tracker_sync_roster_add(int server_player_id_1b, const char *name, int life)
{
    if (roster_count >= TRACKER_ROSTER_MAX) return;
    if (!srv_id_is_valid(server_player_id_1b)) return;

    tracker_roster_entry_t *e = &roster[roster_count++];
    e->player_id = server_player_id_1b;
    e->life = life;
    if (name) {
        snprintf(e->name, sizeof(e->name), "%s", name);
    } else {
        snprintf(e->name, sizeof(e->name), "Player %d", server_player_id_1b);
    }
}

/* Phase 2 MVP: the claimed tracker player maps to knobby's local slot 0
 * regardless of server id. This works for 1p mode (only slot) and for
 * multi-p mode as long as the user occupies slot 0 on their device.
 * TODO: allow the user to pick which local slot is "me" for multi-p mode. */
static inline int my_local_slot_or_neg(void)
{
    if (my_player_id == 0) return -1;
    return 0;
}

void tracker_sync_apply_my_life(int life)
{
    int me = my_local_slot_or_neg();
    if (me < 0) return;
    if (life < LIFE_MIN) life = LIFE_MIN;
    if (life > LIFE_MAX) life = LIFE_MAX;
    player_life[me] = life;
}

void tracker_sync_apply_my_poison(int poison)
{
    int me = my_local_slot_or_neg();
    if (me < 0) return;
    if (poison < COUNTER_MIN) poison = COUNTER_MIN;
    if (poison > COUNTER_MAX) poison = COUNTER_MAX;
    player_counters[me][COUNTER_TYPE_POISON] = poison;
}

void tracker_sync_apply_my_cmd_damage(int source_1b, int value)
{
    int me = my_local_slot_or_neg();
    if (me < 0) return;
    if (!srv_id_is_valid(source_1b)) return;
    int src = srv_to_loc(source_1b);
    if (!src_0b_is_valid(src)) return;                  /* bounds on 8-wide first dim */
    if (value < 0) value = 0;
    if (value > 999) value = 999;
    cmd_damage_totals[src][me] = value;
}

void tracker_sync_end_frame(void)
{
    applying_remote = false;
    /* Elimination state is derived from life/poison/cmd_damage. */
    for (int p = 0; p < MAX_DISPLAY_PLAYERS; p++) {
        /* check_player_elimination is safe against out-of-range p internally. */
        extern void check_player_elimination(int player);
        check_player_elimination(p);
    }
    refresh_main_ui();
    refresh_multiplayer_ui();
    refresh_player_ui();
}

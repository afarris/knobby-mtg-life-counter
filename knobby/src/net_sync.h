#ifndef _NET_SYNC_H
#define _NET_SYNC_H

#include <stdint.h>

/* Table Sync: mirrors the whole game state between Knobby devices over
   ESP-NOW. Every packet carries a full state snapshot with a per-player
   Lamport version; receivers adopt a player's block iff its version is
   newer (ties broken by sender MAC), so lost packets self-heal on the
   next broadcast and devices converge instead of diverging.

   The radio side lives in knobby/knobby_net.cpp (firmware only); the
   simulator links the no-op bridges in sim/sim_stubs.c. The state
   builders/appliers live in game.c and must only run on the main
   (LVGL) task. */

#define NET_SYNC_MAX_PLAYERS 4  /* == MAX_DISPLAY_PLAYERS */
#define NET_SYNC_MAX_SOURCES 8  /* == MAX_GAME_PLAYERS    */

#define NET_SYNC_ELIM      0x01 /* eliminated flag bits */
#define NET_SYNC_ELIM_MANUAL 0x02

typedef struct __attribute__((packed)) {
    uint16_t version;    /* per-player Lamport version, wraps (serial arithmetic) */
    int16_t  life;
    int16_t  counters[4];                    /* == COUNTER_TYPE_COUNT */
    uint8_t  cmd_damage[NET_SYNC_MAX_SOURCES]; /* column of cmd_damage_totals */
    uint8_t  eliminated;                     /* NET_SYNC_ELIM* bits */
    uint8_t  reserved;                       /* keeps u16 fields even-aligned */
} net_sync_player_t;

typedef struct __attribute__((packed)) {
    /* Game epoch dominates the per-player version comparison: a newer
       epoch is adopted wholesale, an older one is rejected outright.
       Bumped by Start Game and by reset, zeroed on join — so versions
       left over from an earlier game can never outrank the current one. */
    uint16_t epoch;
    uint16_t reserved;
    net_sync_player_t players[NET_SYNC_MAX_PLAYERS];
} net_sync_state_t;

#define NET_SYNC_NAME_LEN 16 /* == sizeof(player_names[0]) */

typedef struct __attribute__((packed)) {
    /* One version for the whole roster (not per-name): renames are
       rare, setup-time edits, so set-level LWW is enough. Deliberately
       epoch-independent — names outlive game resets. */
    uint16_t version;
    char names[NET_SYNC_MAX_SOURCES][NET_SYNC_NAME_LEN];
} net_sync_names_t;

/* Implemented in game.c */
void net_sync_fill_state(net_sync_state_t *out);
void net_sync_apply_state(const net_sync_state_t *in, int wins_ties);
void net_sync_fill_names(net_sync_names_t *out);
void net_sync_apply_names(const net_sync_names_t *in, int wins_ties);
void net_sync_commit_names(void);   /* rename hook: bump + broadcast */
void net_sync_begin_game(void);     /* host: new epoch, fresh versions */
void net_sync_reset_versions(void); /* joiner: forget all, adopt the table */

/* Pairing: a game is identified by a random 32-bit session ID. "Start
   Game" generates one and broadcasts invites for a short window; "Join
   Game" listens for an invite and adopts its session. Only packets
   carrying the matching session are accepted, so tables in radio range
   don't merge. Sessions are RAM-only: a reboot always comes up with
   the radio off, and a rebooted device rejoins via the in-game Invite
   action (which re-opens the window for the same session). */
typedef enum {
    NET_SYNC_OFF = 0,
    NET_SYNC_JOINING,  /* listening for an invite */
    NET_SYNC_HOSTING,  /* in game, invite window still open */
    NET_SYNC_IN_GAME,
} net_sync_status_t;

/* Radio bridges: knobby_net.cpp on firmware, sim_stubs.c in the sim.
   start/join return nonzero if the radio came up. net_sync_send_reply
   is the rate-limited variant for anti-entropy answers (a reply can
   itself trigger a reply, so it must not be storm-capable). */
void net_sync_send_state(void);
void net_sync_send_names(void);
void net_sync_send_reply(void);
int net_sync_start_game(void);
int net_sync_join_game(void);
void net_sync_leave_game(void);
int net_sync_status(void);     /* net_sync_status_t */
int net_sync_code(void);       /* 4-digit game code, -1 when no session */

#endif

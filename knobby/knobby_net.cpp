/* Table Sync: broadcasts full game-state snapshots over ESP-NOW and
   mirrors snapshots from other Knobby devices in the same game session.
   Convergence rules (game epoch, per-player Lamport versions, MAC
   tiebreak) live in game.c; this file is the transport and the pairing
   state machine. Based on a prototype by kbratos.

   The session lives in RAM only — a reboot always comes up with the
   radio off. Deliberate: a persisted session would re-enable the radio
   (and disable light sleep) on every boot until the user found Leave,
   and would re-add the radio's load right after the low-battery boot
   gate passed on a rested cell. A device that reboots mid-game rejoins
   via Invite/Join instead.

   The ESP-NOW receive callback runs on the WiFi task, so packets are
   queued here and drained from loop() via knobby_net_process() — game
   state and LVGL must only be touched from the main task. */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "knobby_net.h"

extern "C" {
#include "src/net_sync.h"
}

#define KNOBBY_NET_MAGIC   0x4B4E4259u /* "KNBY": filters foreign ESP-NOW traffic */
/* Protocol version: an identifier, not a counter — receivers check
   strict equality and drop everything else, so bump it on ANY breaking
   wire-format change. 0 is invalid (an all-zero packet must never
   pass); 255 is reserved as an escape hatch ("extended version follows
   the prelude") should the space ever run out. */
#define KNOBBY_NET_VERSION 1
/* A state snapshot doubles as repair: a slow beacon re-broadcasts it so
   lost packets and rejoining devices converge without an ack layer. */
#define KNOBBY_NET_BEACON_MS 5000UL
/* Start Game / Invite broadcasts invites for this long; Join Game
   listens for as long before giving up. */
#define KNOBBY_NET_PAIR_WINDOW_MS 30000UL
#define KNOBBY_NET_INVITE_PERIOD_MS 1000UL

enum { KNOBBY_PKT_STATE = 0, KNOBBY_PKT_INVITE = 1, KNOBBY_PKT_NAMES = 2 };

typedef union {
  net_sync_state_t state;
  net_sync_names_t names;
} knobby_net_body_t;

typedef struct __attribute__((packed)) {
  /* ---- FROZEN PRELUDE ----------------------------------------------
     The first 6 bytes are an eternal contract shared by every protocol
     version, past and future: magic, version, type — in this order, at
     these offsets, forever. This is what lets any firmware recognize a
     packet from any other version well enough to know it can't speak
     it, instead of parsing another layout as game state. Never
     reorder, resize, or insert before these fields; evolve the
     protocol only below this line (and bump KNOBBY_NET_VERSION). */
  uint32_t magic;
  uint8_t  version;
  uint8_t  type;
  /* ---- v1 layout — may change freely in future versions ---- */
  uint16_t reserved;
  uint32_t session;
  knobby_net_body_t body; /* per-type payload; INVITE has none */
} knobby_net_packet_t;

static_assert(offsetof(knobby_net_packet_t, magic) == 0 &&
              offsetof(knobby_net_packet_t, version) == 4 &&
              offsetof(knobby_net_packet_t, type) == 5,
              "the 6-byte prelude is frozen across all protocol versions");

#define KNOBBY_NET_HDR_LEN   offsetof(knobby_net_packet_t, body)
#define KNOBBY_NET_STATE_LEN (KNOBBY_NET_HDR_LEN + sizeof(net_sync_state_t))
#define KNOBBY_NET_NAMES_LEN (KNOBBY_NET_HDR_LEN + sizeof(net_sync_names_t))

typedef struct {
  uint8_t type;
  uint8_t wins_ties;
  uint32_t session;
  knobby_net_body_t body;
} knobby_net_rx_t;

static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static QueueHandle_t rx_queue = NULL;
static uint8_t my_mac[6] = {0};
static uint32_t last_tx_ms = 0;
static uint32_t last_invite_ms = 0;
static uint32_t pair_window_end_ms = 0;
/* Written on the main task, read on the WiFi task. */
static volatile bool net_active = false;
static volatile int sync_status = NET_SYNC_OFF;
static volatile uint32_t session_id = 0;

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
  knobby_net_packet_t pkt = {}; /* INVITE fills only the header */
  knobby_net_rx_t rx;
  if (!net_active || rx_queue == NULL) return;
  if (len < (int)KNOBBY_NET_HDR_LEN || len > (int)sizeof(pkt)) return;
  memcpy(&pkt, data, len);
  if (pkt.magic != KNOBBY_NET_MAGIC || pkt.version != KNOBBY_NET_VERSION) return;

  if (pkt.type == KNOBBY_PKT_STATE) {
    /* Only state for the game we're in (a joiner has session 0, which
       matches nothing: senders never broadcast a zero session). */
    if (len != (int)KNOBBY_NET_STATE_LEN) return;
    if (session_id == 0 || pkt.session != session_id) return;
  } else if (pkt.type == KNOBBY_PKT_NAMES) {
    if (len != (int)KNOBBY_NET_NAMES_LEN) return;
    if (session_id == 0 || pkt.session != session_id) return;
  } else if (pkt.type == KNOBBY_PKT_INVITE) {
    if (len != (int)KNOBBY_NET_HDR_LEN) return;
    if (sync_status != NET_SYNC_JOINING || pkt.session == 0) return;
  } else {
    return;
  }

  rx.type = pkt.type;
  rx.session = pkt.session;
  rx.body = pkt.body;
  /* Both devices must pick the same winner on equal versions, so ties go
     to the higher MAC — each side compares the same two addresses. */
  rx.wins_ties = (memcmp(info->src_addr, my_mac, sizeof(my_mac)) > 0) ? 1 : 0;
  /* Drop when full rather than block the WiFi task; snapshots are
     self-healing, the next one carries everything. */
  xQueueSend(rx_queue, &rx, 0);
}

static bool knobby_net_radio_up(void)
{
  if (net_active) return true;
  if (rx_queue == NULL) {
    rx_queue = xQueueCreate(8, sizeof(knobby_net_rx_t));
    if (rx_queue == NULL) return false;
  }
  /* Stale packets from a previous enable must not apply now. */
  xQueueReset(rx_queue);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  /* ESP-NOW RX needs the receiver always listening; pin that intent even
     if a future core changes the disconnected-STA power-save default. */
  WiFi.setSleep(false);
  WiFi.macAddress(my_mac);
  if (esp_now_init() != ESP_OK) {
    WiFi.mode(WIFI_OFF);
    return false;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcast_mac, sizeof(broadcast_mac));
  peer.channel = 0; /* 0 = whatever channel the STA interface is on */
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK ||
      esp_now_register_recv_cb(on_data_recv) != ESP_OK) {
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
    return false;
  }
  net_active = true;
  return true;
}

static void knobby_net_radio_down(void)
{
  if (!net_active) return;
  net_active = false;
  esp_now_unregister_recv_cb();
  /* esp_wifi_stop() serializes with the WiFi task, so no receive
     callback can be in flight when ESP-NOW is torn down below. */
  esp_wifi_stop();
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
}

static void knobby_net_send(uint8_t type)
{
  knobby_net_packet_t pkt = {};
  if (!net_active || session_id == 0) return;
  pkt.magic = KNOBBY_NET_MAGIC;
  pkt.version = KNOBBY_NET_VERSION;
  pkt.type = type;
  pkt.session = session_id;
  /* Stamp timestamps only on accepted sends: a driver-rejected packet
     (e.g. ESP_ERR_ESPNOW_NO_MEM under bursts) must not silence the
     beacon or the reply throttle for a snapshot that never aired. */
  if (type == KNOBBY_PKT_STATE) {
    net_sync_fill_state(&pkt.body.state);
    if (esp_now_send(broadcast_mac, (const uint8_t *)&pkt,
                     KNOBBY_NET_STATE_LEN) == ESP_OK) {
      last_tx_ms = millis();
    }
  } else if (type == KNOBBY_PKT_NAMES) {
    net_sync_fill_names(&pkt.body.names);
    esp_now_send(broadcast_mac, (const uint8_t *)&pkt, KNOBBY_NET_NAMES_LEN);
  } else {
    if (esp_now_send(broadcast_mac, (const uint8_t *)&pkt,
                     KNOBBY_NET_HDR_LEN) == ESP_OK) {
      last_invite_ms = millis();
    }
  }
}

bool knobby_net_active(void)
{
  return net_active;
}

void knobby_net_process(void)
{
  knobby_net_rx_t rx;
  uint32_t now;

  if (!net_active || rx_queue == NULL) return;

  while (xQueueReceive(rx_queue, &rx, 0) == pdTRUE) {
    /* Re-validate against the CURRENT session — Start/Join may have
       changed it after this packet passed the callback's filter. */
    if (rx.type == KNOBBY_PKT_STATE) {
      if (rx.session == session_id && session_id != 0) {
        net_sync_apply_state(&rx.body.state, rx.wins_ties);
      }
    } else if (rx.type == KNOBBY_PKT_NAMES) {
      if (rx.session == session_id && session_id != 0) {
        net_sync_apply_names(&rx.body.names, rx.wins_ties);
      }
    } else if (rx.type == KNOBBY_PKT_INVITE &&
               sync_status == NET_SYNC_JOINING) {
      /* Adopt the table NOW, not at Join-press: wiping here means
         nothing armed during the join window (a reset, a rename, a
         game-mode Apply) can outrank the live game — and a join that
         times out has wiped nothing from the local game. */
      net_sync_reset_versions();
      session_id = rx.session;
      sync_status = NET_SYNC_IN_GAME;
    }
  }

  now = millis();
  if (sync_status == NET_SYNC_HOSTING) {
    if ((int32_t)(now - pair_window_end_ms) >= 0) {
      sync_status = NET_SYNC_IN_GAME;
      /* One last roster push for anyone who joined late in the window. */
      knobby_net_send(KNOBBY_PKT_NAMES);
    } else if (now - last_invite_ms >= KNOBBY_NET_INVITE_PERIOD_MS) {
      knobby_net_send(KNOBBY_PKT_INVITE);
      /* Names travel with invites instead of riding the steady-state
         beacon. A joiner always misses the companion of the invite it
         adopts (its session is set a beat later), so delivery is the
         NEXT 1s tick, the window-close push, or the anti-entropy
         reply to its first stale packet. */
      knobby_net_send(KNOBBY_PKT_NAMES);
    }
  } else if (sync_status == NET_SYNC_JOINING) {
    if ((int32_t)(now - pair_window_end_ms) >= 0) {
      net_sync_leave_game(); /* nothing found: back to off */
      return;
    }
  }
  if (session_id != 0 && now - last_tx_ms >= KNOBBY_NET_BEACON_MS) {
    knobby_net_send(KNOBBY_PKT_STATE);
  }
}

extern "C" void net_sync_send_state(void)
{
  knobby_net_send(KNOBBY_PKT_STATE);
}

extern "C" void net_sync_send_names(void)
{
  knobby_net_send(KNOBBY_PKT_NAMES);
}

extern "C" void net_sync_send_reply(void)
{
  /* Two devices can each judge the other stale (mutual-stale is
     reachable at exactly 0x8000 of version divergence), so replies
     are rate-limited; short of that pathological edge, the 5s beacon
     repairs everything. */
  if (millis() - last_tx_ms < 250) return;
  knobby_net_send(KNOBBY_PKT_STATE);
  /* The reply doubles as roster delivery: a joiner that lost the
     invite-window names packet beacons a stale epoch within 5s, and
     names are otherwise never repaired. */
  knobby_net_send(KNOBBY_PKT_NAMES);
}

extern "C" int net_sync_start_game(void)
{
  uint32_t id;
  if (!knobby_net_radio_up()) return 0;
  if (session_id != 0) {
    /* Already in a game: re-open the invite window for the SAME
       session so late joiners and rebooted devices can (re)join —
       re-keying here would silently orphan every existing peer. */
    sync_status = NET_SYNC_HOSTING;
    pair_window_end_ms = millis() + KNOBBY_NET_PAIR_WINDOW_MS;
    knobby_net_send(KNOBBY_PKT_INVITE);
    return 1;
  }
  do { id = esp_random(); } while (id == 0);
  xQueueReset(rx_queue); /* radio may already be up (e.g. was JOINING) */
  net_sync_begin_game();
  session_id = id;
  sync_status = NET_SYNC_HOSTING;
  pair_window_end_ms = millis() + KNOBBY_NET_PAIR_WINDOW_MS;
  knobby_net_send(KNOBBY_PKT_INVITE);
  knobby_net_send(KNOBBY_PKT_STATE);
  knobby_net_send(KNOBBY_PKT_NAMES);
  return 1;
}

extern "C" int net_sync_join_game(void)
{
  if (!knobby_net_radio_up()) return 0;
  session_id = 0; /* joining implies leaving the previous game */
  xQueueReset(rx_queue);
  /* Versions/bookkeeping are wiped at invite ADOPTION, not here — see
     knobby_net_process. */
  sync_status = NET_SYNC_JOINING;
  pair_window_end_ms = millis() + KNOBBY_NET_PAIR_WINDOW_MS;
  return 1;
}

extern "C" void net_sync_leave_game(void)
{
  knobby_net_radio_down();
  session_id = 0;
  sync_status = NET_SYNC_OFF;
}

extern "C" int net_sync_status(void)
{
  return sync_status;
}

extern "C" int net_sync_code(void)
{
  uint32_t id = session_id;
  return (id != 0) ? (int)(id % 10000u) : -1;
}

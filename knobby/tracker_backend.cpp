#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include <string.h>

extern "C" {
#include "src/tracker_sync.h"
}

/* ---------- logging ---------- */
extern "C" void tracker_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    Serial.print("[tracker] ");
    Serial.print(buf);
    if (buf[n - 1] != '\n') Serial.println();
}

/* ---------- WebSocket client ---------- */
static WebSocketsClient ws;
static bool ws_initialized = false;

/* Parsed base URL: we need host + port separately for WebSocketsClient.begin. */
static char ws_host[64] = {0};
static uint16_t ws_port = 8000;
static char current_base_url[TRACKER_URL_LEN] = {0};

/* ---------- URL parsing ----------
 * Accepts forms like "http://host:port" or "http://host.local:port" or
 * "host:port" or "host" (defaults to port 8000). Writes host + port into
 * the module statics. Returns true on parse success.
 */
static bool parse_base_url(const char *url)
{
    if (!url || !url[0]) return false;
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0)  p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;   /* TLS not supported yet; strip for now */
    if (!*p) return false;

    /* Scan for ':' (port) or '/' (path start) */
    const char *host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    size_t host_len = (size_t)(host_end - p);
    if (host_len == 0 || host_len >= sizeof(ws_host)) return false;

    memcpy(ws_host, p, host_len);
    ws_host[host_len] = '\0';

    if (*host_end == ':') {
        int port = atoi(host_end + 1);
        if (port <= 0 || port > 65535) return false;
        ws_port = (uint16_t)port;
    } else {
        ws_port = 8000;
    }
    return true;
}

/* Rebuild current_base_url in canonical "http://host:port" form so HTTPClient
 * doesn't silently fall back to port 80 when the user typed just "host". */
static void canonicalize_current_base_url(void)
{
    snprintf(current_base_url, sizeof(current_base_url),
             "http://%s:%u", ws_host, (unsigned)ws_port);
}

/* ---------- WS frame parsing ----------
 * Parse an mtg-tracker GameState snapshot and emit structured callbacks.
 * JSON shape (backend/game_state.py):
 *   { "players": { "<id>": { "id", "life", "name", "commander_damage": {...},
 *                            "poison", "energy", "rad", "speed", ... }, ... },
 *     "current_turn_id": int, "monarch_id": ..., ... }
 * We care about: players[*].{id, name, life, poison, commander_damage}.
 */
/* Run the state-application pipeline on an already-parsed JSON doc. Shared
 * by the WS and HTTP /state paths so the HTTP path doesn't need its own
 * intermediate buffer. */
static void apply_state_from_doc(JsonDocument &doc)
{
    tracker_sync_begin_frame();
    tracker_sync_roster_clear();

    JsonObject players = doc["players"].as<JsonObject>();
    int my_id = tracker_my_player_id();

    for (JsonPair kv : players) {
        JsonObject p = kv.value().as<JsonObject>();
        if (p.isNull()) continue;

        int id   = p["id"]   | 0;
        int life = p["life"] | 0;
        const char *name = p["name"] | (const char *)NULL;
        tracker_sync_roster_add(id, name, life);

        if (my_id > 0 && id == my_id) {
            tracker_log("  (me) id=%d life=%d poison=%d", id, life, (int)(p["poison"] | 0));
            tracker_sync_apply_my_life(life);
            tracker_sync_apply_my_poison(p["poison"] | 0);

            JsonObject cmd = p["commander_damage"].as<JsonObject>();
            for (JsonPair dmg_kv : cmd) {
                const char *key = dmg_kv.key().c_str();
                /* Ignore partner entries ("<id>_p") in phase 2 — knobby has
                 * no partner UI. TODO: sum both into the source's cell? */
                if (!key || !key[0]) continue;
                if (strchr(key, '_')) continue;
                int src = atoi(key);
                if (src <= 0) continue;
                int val = dmg_kv.value().as<int>();
                tracker_sync_apply_my_cmd_damage(src, val);
            }
        }
    }

    tracker_sync_end_frame();
}

static void parse_and_apply_ws_frame(const char *json, size_t len)
{
    tracker_log("ws frame %u bytes", (unsigned)len);
    /* 8KB budget covers ~8 players with art URLs (typical 2-4KB). */
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) { tracker_log("ws json parse FAIL: %s", err.c_str()); return; }
    apply_state_from_doc(doc);
}

static void ws_event_cb(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type) {
        case WStype_DISCONNECTED:
            tracker_sync_on_ws_state(0);
            break;
        case WStype_CONNECTED:
            tracker_sync_on_ws_state(2);
            break;
        case WStype_TEXT:
            parse_and_apply_ws_frame((const char *)payload, length);
            break;
        case WStype_ERROR:
            tracker_sync_on_ws_state(3);
            break;
        default:
            /* BIN / PING / PONG / fragments — irrelevant here */
            break;
    }
}

/* ---------- C API exports ---------- */

extern "C" void tracker_hw_init(void)
{
    /* Nothing eager; mDNS and WS come up lazily on first use. */
}

extern "C" void tracker_hw_connect(const char *base_url)
{
    if (!parse_base_url(base_url)) {
        tracker_sync_on_ws_state(3);
        return;
    }
    canonicalize_current_base_url();

    if (ws_initialized) {
        ws.disconnect();
    }

    tracker_sync_on_ws_state(1);  /* CONNECTING */
    ws.begin(ws_host, ws_port, "/ws");
    ws.onEvent(ws_event_cb);
    ws.setReconnectInterval(2000);   /* first retry after 2s; library handles backoff */
    ws_initialized = true;
}

extern "C" void tracker_hw_disconnect(void)
{
    if (ws_initialized) {
        ws.disconnect();
        ws_initialized = false;
    }
    current_base_url[0] = '\0';
    ws_host[0] = '\0';
    tracker_sync_on_ws_state(0);
}

extern "C" bool tracker_hw_is_open(void)
{
    return ws_initialized && ws.isConnected();
}

/* Called from the main loop() to pump WebSocket events + reconnect backoff. */
extern "C" void tracker_hw_loop(void)
{
    if (ws_initialized) ws.loop();
}

/* ---------- HTTP ----------
 * Standard Arduino HTTPClient, single-arg begin(url), local per call.
 * Keep it boring; no timeouts/reuse/external-client tweaks. */

static bool http_post_json(const char *path, const char *body)
{
    HTTPClient http;
    http.begin(String(current_base_url) + path);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    http.end();
    tracker_log("POST %s -> %d", path, code);
    return code >= 200 && code < 300;
}

extern "C" bool tracker_hw_post_update(int player_id, int delta)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"player_id\":%d,\"delta\":%d}", player_id, delta);
    return http_post_json("/update", body);
}

extern "C" bool tracker_hw_post_commander_damage(int target_id, int source_id, int delta, bool is_partner)
{
    char body[96];
    snprintf(body, sizeof(body),
             "{\"target_id\":%d,\"source_id\":%d,\"delta\":%d,\"is_partner\":%s}",
             target_id, source_id, delta, is_partner ? "true" : "false");
    return http_post_json("/commander_damage", body);
}

extern "C" bool tracker_hw_post_poison(int player_id, int delta)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"player_id\":%d,\"delta\":%d}", player_id, delta);
    return http_post_json("/poison", body);
}

extern "C" void tracker_hw_fetch_and_apply_state(void)
{
    HTTPClient http;
    http.begin(String(current_base_url) + "/state");
    int code = http.GET();
    tracker_log("GET /state -> %d", code);
    if (code == 200) {
        DynamicJsonDocument doc(8192);
        if (!deserializeJson(doc, http.getStream()))
            apply_state_from_doc(doc);
    }
    http.end();
}


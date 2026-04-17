/* Phase 3: heartbeat JSON rendering + permission-prompt modal.
 *
 * Architecture:
 *   - BLE RX callback (BLE thread) pushes raw bytes into a FuriStreamBuffer.
 *   - A dedicated drain thread accumulates bytes into a line buffer, parses
 *     one JSON object per '\n' with jsmn, and updates shared app state
 *     under a mutex.
 *   - The main thread drives the GUI: a redraw timer at 4 Hz polls state
 *     and repaints, and an input queue handles button events.
 *   - When the parsed heartbeat contains a `prompt` object, the app enters
 *     ModePrompt and renders a modal with tool/hint + keybindings. OK and
 *     Left send the permission response back via TX; the desktop clears
 *     the prompt from subsequent heartbeats, which drops us back to
 *     ModeNormal.
 */

#include "claude_buddy_profile.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <bt/bt_service/bt.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#define JSMN_STATIC
#include "jsmn.h"

#include <stdlib.h>
#include <string.h>

#define TAG "ClaudeBuddyApp"

#define REDRAW_PERIOD_MS    250
#define LINE_BUF_SIZE       1024
#define RX_STREAM_SIZE      2048
#define RX_STREAM_TRIGGER   1
#define RX_THREAD_STACK     2048
#define MAX_TOKENS          64

typedef enum {
    ClaudeBuddyModeNormal,
    ClaudeBuddyModePrompt,
} ClaudeBuddyMode;

typedef struct {
    FuriMutex* mtx;

    /* BT status */
    BtStatus bt_status;
    bool cccd_enabled;

    /* Parsed heartbeat fields */
    int hb_total;
    int hb_running;
    int hb_waiting;
    char hb_msg[64];

    /* Prompt state (valid when mode == Prompt) */
    ClaudeBuddyMode mode;
    char prompt_id[48];
    char prompt_tool[32];
    char prompt_hint[96];

    /* GUI */
    FuriMessageQueue* input_queue;
    ViewPort* view_port;
    Gui* gui;
    FuriTimer* redraw_timer;

    /* RX pipeline */
    FuriStreamBuffer* rx_stream;
    FuriThread* rx_thread;
    volatile bool running;
    uint8_t line_buf[LINE_BUF_SIZE];
    size_t line_len;

    /* BT service + active profile */
    Bt* bt;
    FuriHalBleProfileBase* profile;
} ClaudeBuddyApp;

/* ============================================================
 *  JSON helpers — small wrappers on jsmn
 * ============================================================ */

static bool tok_streq(const char* json, const jsmntok_t* t, const char* s) {
    int len = t->end - t->start;
    return (int)strlen(s) == len && strncmp(json + t->start, s, len) == 0;
}

/* Linear key search: find the first STRING token matching `key` and
 * return the index of the immediately following (value) token. Good
 * enough for the Hardware Buddy heartbeat since all keys we care about
 * are globally unique across the object (total/running/waiting/msg at
 * top level; id/tool/hint only under `prompt`). Returns -1 if not
 * found. */
static int json_find_key(const char* json, const jsmntok_t* tokens, int n, const char* key) {
    for(int i = 0; i < n - 1; i++) {
        if(tokens[i].type == JSMN_STRING && tok_streq(json, &tokens[i], key)) {
            return i + 1;
        }
    }
    return -1;
}

static int json_tok_int(const char* json, const jsmntok_t* t) {
    char buf[16] = {0};
    int len = t->end - t->start;
    if(len < 0 || len >= (int)sizeof(buf)) return 0;
    memcpy(buf, json + t->start, len);
    return atoi(buf);
}

static void
    json_tok_strcpy(const char* json, const jsmntok_t* t, char* dst, size_t dst_size) {
    int len = t->end - t->start;
    if(len < 0) len = 0;
    if(len >= (int)dst_size) len = dst_size - 1;
    memcpy(dst, json + t->start, len);
    dst[len] = '\0';
}

static const char* bt_status_str(BtStatus s) {
    switch(s) {
    case BtStatusUnavailable: return "Unavailable";
    case BtStatusOff: return "Off";
    case BtStatusAdvertising: return "Advertising";
    case BtStatusConnected: return "Connected";
    default: return "?";
    }
}

/* ============================================================
 *  Heartbeat ingest
 * ============================================================ */

static void handle_rx_line(ClaudeBuddyApp* app, const char* line, size_t line_len) {
    jsmn_parser parser;
    jsmntok_t tokens[MAX_TOKENS];
    jsmn_init(&parser);
    int n = jsmn_parse(&parser, line, line_len, tokens, MAX_TOKENS);
    if(n < 1 || tokens[0].type != JSMN_OBJECT) return;

    furi_mutex_acquire(app->mtx, FuriWaitForever);

    int v;
    if((v = json_find_key(line, tokens, n, "total")) >= 0)
        app->hb_total = json_tok_int(line, &tokens[v]);
    if((v = json_find_key(line, tokens, n, "running")) >= 0)
        app->hb_running = json_tok_int(line, &tokens[v]);
    if((v = json_find_key(line, tokens, n, "waiting")) >= 0)
        app->hb_waiting = json_tok_int(line, &tokens[v]);
    if((v = json_find_key(line, tokens, n, "msg")) >= 0)
        json_tok_strcpy(line, &tokens[v], app->hb_msg, sizeof(app->hb_msg));

    int p_idx = json_find_key(line, tokens, n, "prompt");
    if(p_idx >= 0 && tokens[p_idx].type == JSMN_OBJECT) {
        if((v = json_find_key(line, tokens, n, "id")) >= 0)
            json_tok_strcpy(line, &tokens[v], app->prompt_id, sizeof(app->prompt_id));
        if((v = json_find_key(line, tokens, n, "tool")) >= 0)
            json_tok_strcpy(line, &tokens[v], app->prompt_tool, sizeof(app->prompt_tool));
        if((v = json_find_key(line, tokens, n, "hint")) >= 0)
            json_tok_strcpy(line, &tokens[v], app->prompt_hint, sizeof(app->prompt_hint));
        app->mode = ClaudeBuddyModePrompt;
    } else {
        /* Desktop cleared the prompt — drop the modal. */
        app->mode = ClaudeBuddyModeNormal;
    }

    furi_mutex_release(app->mtx);
    view_port_update(app->view_port);
}

/* ============================================================
 *  RX drain thread
 * ============================================================ */

static int32_t claude_buddy_rx_thread(void* ctx) {
    ClaudeBuddyApp* app = ctx;
    uint8_t chunk[64];
    while(app->running) {
        size_t n =
            furi_stream_buffer_receive(app->rx_stream, chunk, sizeof(chunk), 250);
        for(size_t i = 0; i < n; i++) {
            if(app->line_len < LINE_BUF_SIZE - 1) {
                app->line_buf[app->line_len++] = chunk[i];
            } else {
                /* Over-long line — reset and drop until the next newline. */
                app->line_len = 0;
            }
            if(chunk[i] == '\n') {
                handle_rx_line(app, (const char*)app->line_buf, app->line_len);
                app->line_len = 0;
            }
        }
    }
    return 0;
}

/* ============================================================
 *  Drawing
 * ============================================================ */

static void claude_buddy_draw(Canvas* canvas, void* ctx) {
    ClaudeBuddyApp* app = ctx;

    /* Snapshot shared state under mutex, then draw unlocked. */
    furi_mutex_acquire(app->mtx, FuriWaitForever);
    ClaudeBuddyMode mode = app->mode;
    BtStatus bt_status = app->bt_status;
    bool cccd = app->cccd_enabled;
    int t = app->hb_total, r = app->hb_running, w = app->hb_waiting;
    char hb_msg[64];
    strlcpy(hb_msg, app->hb_msg, sizeof(hb_msg));
    char prompt_tool[32];
    strlcpy(prompt_tool, app->prompt_tool, sizeof(prompt_tool));
    char prompt_hint[96];
    strlcpy(prompt_hint, app->prompt_hint, sizeof(prompt_hint));
    furi_mutex_release(app->mtx);

    canvas_clear(canvas);

    if(mode == ClaudeBuddyModePrompt) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Permission?");

        canvas_set_font(canvas, FontSecondary);
        char line[40];
        snprintf(line, sizeof(line), "Tool: %s", prompt_tool);
        canvas_draw_str(canvas, 2, 24, line);

        /* Hint can be longer than the screen — strlcpy truncates cleanly. */
        strlcpy(line, prompt_hint, sizeof(line));
        canvas_draw_str(canvas, 2, 36, line);

        canvas_draw_str(canvas, 2, 62, "OK=approve  Left=deny");
        return;
    }

    /* Normal mode */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Claude Buddy");

    canvas_set_font(canvas, FontSecondary);
    char line[48];
    snprintf(line, sizeof(line), "BT: %s  sub:%c", bt_status_str(bt_status), cccd ? 'y' : 'n');
    canvas_draw_str(canvas, 2, 24, line);

    snprintf(line, sizeof(line), "T:%d  R:%d  W:%d", t, r, w);
    canvas_draw_str(canvas, 2, 36, line);

    canvas_draw_str(canvas, 2, 50, hb_msg[0] ? hb_msg : "(waiting for heartbeat)");
}

/* ============================================================
 *  Input + BT callbacks
 * ============================================================ */

static void claude_buddy_input(InputEvent* event, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

static void claude_buddy_redraw_tick(void* ctx) {
    ClaudeBuddyApp* app = ctx;
    view_port_update(app->view_port);
}

static void claude_buddy_on_bt_status(BtStatus status, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    furi_mutex_acquire(app->mtx, FuriWaitForever);
    app->bt_status = status;
    furi_mutex_release(app->mtx);
}

static void claude_buddy_on_rx(const uint8_t* data, uint16_t len, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    /* BLE thread context — push bytes and return fast. */
    furi_stream_buffer_send(app->rx_stream, data, len, 0);
}

static void claude_buddy_on_conn(bool connected, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    furi_mutex_acquire(app->mtx, FuriWaitForever);
    app->cccd_enabled = connected;
    furi_mutex_release(app->mtx);
}

/* ============================================================
 *  Entry point
 * ============================================================ */

int32_t claude_buddy_app(void* p) {
    UNUSED(p);
    ClaudeBuddyApp* app = malloc(sizeof(ClaudeBuddyApp));
    memset(app, 0, sizeof(*app));

    app->mtx = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->rx_stream = furi_stream_buffer_alloc(RX_STREAM_SIZE, RX_STREAM_TRIGGER);
    app->running = true;

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, claude_buddy_draw, app);
    view_port_input_callback_set(app->view_port, claude_buddy_input, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    app->redraw_timer = furi_timer_alloc(claude_buddy_redraw_tick, FuriTimerTypePeriodic, app);
    furi_timer_start(app->redraw_timer, REDRAW_PERIOD_MS);

    /* RX drain thread */
    app->rx_thread = furi_thread_alloc_ex("ClaudeBuddyRx", RX_THREAD_STACK, claude_buddy_rx_thread, app);
    furi_thread_start(app->rx_thread);

    /* Bring up BLE */
    app->bt = furi_record_open(RECORD_BT);
    bt_set_status_changed_callback(app->bt, claude_buddy_on_bt_status, app);
    app->profile = bt_profile_start(app->bt, ble_profile_claude_buddy, NULL);
    if(!app->profile) {
        FURI_LOG_E(TAG, "bt_profile_start failed");
    } else {
        claude_buddy_profile_set_rx_callback(app->profile, claude_buddy_on_rx, app);
        claude_buddy_profile_set_conn_callback(app->profile, claude_buddy_on_conn, app);
    }

    /* Main input loop */
    InputEvent event;
    while(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type != InputTypeShort) continue;

        /* Back always exits, regardless of mode. */
        if(event.key == InputKeyBack) break;

        /* Snapshot mode + prompt id for the handler below. */
        furi_mutex_acquire(app->mtx, FuriWaitForever);
        ClaudeBuddyMode mode = app->mode;
        char id[48];
        strlcpy(id, app->prompt_id, sizeof(id));
        furi_mutex_release(app->mtx);

        if(mode != ClaudeBuddyModePrompt) continue;
        if(id[0] == '\0') continue;

        const char* decision = NULL;
        if(event.key == InputKeyOk) decision = "once";
        else if(event.key == InputKeyLeft) decision = "deny";
        if(!decision) continue;

        bool tx_ok = false;
        if(app->profile) {
            char json[160];
            int n = snprintf(
                json,
                sizeof(json),
                "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}\n",
                id,
                decision);
            if(n > 0 && (size_t)n < sizeof(json)) {
                tx_ok = claude_buddy_profile_tx(
                    app->profile, (const uint8_t*)json, (uint16_t)n);
            }
        }

        /* Dismiss modal locally regardless of TX result — user gets
         * immediate visual ack. Note the result on the status line so
         * they can distinguish "sent" from "tried but BLE refused". */
        furi_mutex_acquire(app->mtx, FuriWaitForever);
        app->mode = ClaudeBuddyModeNormal;
        app->prompt_id[0] = '\0';
        snprintf(
            app->hb_msg,
            sizeof(app->hb_msg),
            "sent: %s (%s)",
            decision,
            tx_ok ? "ok" : "fail");
        furi_mutex_release(app->mtx);
        view_port_update(app->view_port);
    }

    /* Teardown: stop drain thread, then tear down BLE, then GUI. */
    app->running = false;
    furi_thread_join(app->rx_thread);
    furi_thread_free(app->rx_thread);

    if(app->profile) {
        bt_profile_restore_default(app->bt);
    }
    bt_set_status_changed_callback(app->bt, NULL, NULL);
    furi_record_close(RECORD_BT);

    furi_timer_stop(app->redraw_timer);
    furi_timer_free(app->redraw_timer);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_stream_buffer_free(app->rx_stream);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mtx);
    free(app);
    return 0;
}

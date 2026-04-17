/* Phase 1 (pieces 1+2): BLE profile started via bt_service, GATT service
 * + RX/TX characteristics live, event dispatcher delivering RX writes and
 * CCCD flips. UI is diagnostic-only — it renders BT status, a running RX
 * byte counter, and a hex preview of the most recent payload. No JSON
 * parse yet (piece 3+). No TX yet (piece 3). */

#include "claude_buddy_profile.h"

#include <furi.h>
#include <bt/bt_service/bt.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#define TAG "ClaudeBuddyApp"

#define RX_PREVIEW_BYTES (12)
#define REDRAW_PERIOD_MS (250)

typedef struct {
    FuriMutex* mtx;
    BtStatus bt_status;
    bool cccd_enabled;
    size_t rx_total;
    uint8_t rx_preview[RX_PREVIEW_BYTES];
    uint8_t rx_preview_len;

    FuriMessageQueue* input_queue;
    ViewPort* view_port;
    Gui* gui;
    FuriTimer* redraw_timer;

    Bt* bt;
    FuriHalBleProfileBase* profile;
} ClaudeBuddyApp;

static const char* bt_status_str(BtStatus s) {
    switch(s) {
    case BtStatusUnavailable: return "Unavailable";
    case BtStatusOff: return "Off";
    case BtStatusAdvertising: return "Advertising";
    case BtStatusConnected: return "Connected";
    default: return "?";
    }
}

static void claude_buddy_draw(Canvas* canvas, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    furi_mutex_acquire(app->mtx, FuriWaitForever);
    BtStatus status = app->bt_status;
    bool cccd = app->cccd_enabled;
    size_t total = app->rx_total;
    uint8_t preview[RX_PREVIEW_BYTES];
    uint8_t preview_len = app->rx_preview_len;
    memcpy(preview, app->rx_preview, preview_len);
    furi_mutex_release(app->mtx);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Claude Buddy");

    canvas_set_font(canvas, FontSecondary);

    char line[40];
    snprintf(line, sizeof(line), "BT: %s", bt_status_str(status));
    canvas_draw_str(canvas, 2, 24, line);

    snprintf(line, sizeof(line), "Subscribed: %s", cccd ? "yes" : "no");
    canvas_draw_str(canvas, 2, 36, line);

    snprintf(line, sizeof(line), "RX: %lu bytes", (unsigned long)total);
    canvas_draw_str(canvas, 2, 48, line);

    if(preview_len > 0) {
        char hex[40] = {0};
        char* p = hex;
        for(uint8_t i = 0; i < preview_len && (p - hex) < (int)sizeof(hex) - 4; i++) {
            p += snprintf(p, sizeof(hex) - (p - hex), "%02x ", preview[i]);
        }
        canvas_draw_str(canvas, 2, 60, hex);
    } else {
        canvas_draw_str(canvas, 2, 60, "(no data yet)");
    }
}

static void claude_buddy_input(InputEvent* event, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

static void claude_buddy_redraw_tick(void* ctx) {
    ClaudeBuddyApp* app = ctx;
    view_port_update(app->view_port);
}

/* Invoked from BT service thread on status changes. */
static void claude_buddy_on_bt_status(BtStatus status, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    furi_mutex_acquire(app->mtx, FuriWaitForever);
    app->bt_status = status;
    furi_mutex_release(app->mtx);
}

/* Invoked from BLE event dispatcher thread when desktop writes to RX. */
static void claude_buddy_on_rx(const uint8_t* data, uint16_t len, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    furi_mutex_acquire(app->mtx, FuriWaitForever);
    app->rx_total += len;
    uint8_t n = len < RX_PREVIEW_BYTES ? (uint8_t)len : RX_PREVIEW_BYTES;
    memcpy(app->rx_preview, data, n);
    app->rx_preview_len = n;
    furi_mutex_release(app->mtx);
}

/* Invoked from BLE event dispatcher thread on CCCD subscribe/unsubscribe. */
static void claude_buddy_on_conn(bool connected, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    furi_mutex_acquire(app->mtx, FuriWaitForever);
    app->cccd_enabled = connected;
    furi_mutex_release(app->mtx);
}

int32_t claude_buddy_app(void* p) {
    UNUSED(p);
    ClaudeBuddyApp* app = malloc(sizeof(ClaudeBuddyApp));
    memset(app, 0, sizeof(*app));

    app->mtx = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, claude_buddy_draw, app);
    view_port_input_callback_set(app->view_port, claude_buddy_input, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    app->redraw_timer =
        furi_timer_alloc(claude_buddy_redraw_tick, FuriTimerTypePeriodic, app);
    furi_timer_start(app->redraw_timer, REDRAW_PERIOD_MS);

    /* Bring up BLE: open bt record, register status callback, swap profile. */
    app->bt = furi_record_open(RECORD_BT);
    bt_set_status_changed_callback(app->bt, claude_buddy_on_bt_status, app);
    app->profile = bt_profile_start(app->bt, ble_profile_claude_buddy, NULL);
    if(!app->profile) {
        FURI_LOG_E(TAG, "bt_profile_start failed");
    } else {
        claude_buddy_profile_set_rx_callback(app->profile, claude_buddy_on_rx, app);
        claude_buddy_profile_set_conn_callback(app->profile, claude_buddy_on_conn, app);
        FURI_LOG_I(TAG, "Profile started, advertising as Claude-<device>");
    }

    InputEvent event;
    while(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type == InputTypeShort && event.key == InputKeyBack) break;
    }

    /* Restore the Serial profile so the mobile app link comes back. */
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
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mtx);
    free(app);
    return 0;
}

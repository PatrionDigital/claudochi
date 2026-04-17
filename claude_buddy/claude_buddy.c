/* Minimal FAP entry point — Phase 1 skeleton.
 * Enough to prove the profile compiles and links. Does NOT yet start BLE or
 * render UI; those wire up after the profile skeleton is reviewed. */

#include "claude_buddy_profile.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

typedef struct {
    FuriMessageQueue* input_queue;
    Gui* gui;
    ViewPort* view_port;
} ClaudeBuddyApp;

static void claude_buddy_draw(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Claude Buddy");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 28, "Phase 1 skeleton");
    canvas_draw_str(canvas, 2, 42, "BLE: not wired yet");
    canvas_draw_str(canvas, 2, 62, "Press Back to exit");
}

static void claude_buddy_input(InputEvent* event, void* ctx) {
    ClaudeBuddyApp* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

int32_t claude_buddy_app(void* p) {
    UNUSED(p);
    /* Reference the profile symbol so the linker keeps it. */
    (void)ble_profile_claude_buddy;

    ClaudeBuddyApp* app = malloc(sizeof(ClaudeBuddyApp));
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, claude_buddy_draw, app);
    view_port_input_callback_set(app->view_port, claude_buddy_input, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    InputEvent event;
    while(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type == InputTypeShort && event.key == InputKeyBack) break;
        view_port_update(app->view_port);
    }

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->input_queue);
    free(app);
    return 0;
}

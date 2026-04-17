#pragma once

#include <furi_ble/profile_interface.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Claude Buddy profile template — pass to furi_hal_bt_start_app(). */
extern const FuriHalBleProfileTemplate* const ble_profile_claude_buddy;

/* Invoked from BLE thread context on connect/disconnect.
 * Keep the body short; do not block. */
typedef void (*ClaudeBuddyConnCallback)(bool connected, void* ctx);

/* Invoked from BLE thread context when desktop writes to the RX characteristic.
 * Push bytes into a stream buffer and return — do not parse here. */
typedef void (*ClaudeBuddyRxCallback)(const uint8_t* data, uint16_t len, void* ctx);

void claude_buddy_profile_set_rx_callback(
    FuriHalBleProfileBase* profile,
    ClaudeBuddyRxCallback cb,
    void* ctx);

void claude_buddy_profile_set_conn_callback(
    FuriHalBleProfileBase* profile,
    ClaudeBuddyConnCallback cb,
    void* ctx);

/* Send a single line to the desktop via the TX notify characteristic.
 * Caller must include the trailing '\n'. Returns false if not connected,
 * notifications not subscribed (CCCD off), or len > ATT_MTU-3 (~244 bytes).
 * Phase 1 does not chunk — larger payloads (folder push etc.) are Phase 2+. */
bool claude_buddy_profile_tx(
    FuriHalBleProfileBase* profile,
    const uint8_t* data,
    uint16_t len);

#ifdef __cplusplus
}
#endif

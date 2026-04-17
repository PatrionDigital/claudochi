#include "claude_buddy_profile.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <furi_hal_bt.h>
#include <furi_ble/gatt.h>
#include <furi_ble/event_dispatcher.h>

#include <ble/core/auto/ble_types.h>
#include <ble/core/ble_defs.h>

#include <string.h>

#define TAG "ClaudeBuddyProfile"

/* =========================================================================
 *  Nordic UART Service UUIDs
 *  Wire format is little-endian (LSB first), so the UUID
 *    6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *  is stored as the reverse of its big-endian byte sequence.
 * ========================================================================= */
static const uint8_t NUS_SVC_UUID[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e,
};
static const uint8_t NUS_RX_UUID[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e,
};
static const uint8_t NUS_TX_UUID[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e,
};

/* ATT_MTU=247 on WB55 (default post-exchange) → 244 payload bytes per notify. */
#define CLAUDE_BUDDY_MAX_PAYLOAD (244)

/* Connection interval: 7.5 ms .. 45 ms, same window Unleashed's serial profile uses. */
#define CONN_INTERVAL_MIN (0x06)
#define CONN_INTERVAL_MAX (0x24)

/* =========================================================================
 *  Profile instance
 * ========================================================================= */
typedef struct {
    FuriHalBleProfileBase base;

    /* GATT */
    uint16_t svc_handle;
    BleGattCharacteristicInstance rx_char;
    BleGattCharacteristicInstance tx_char;

    /* Registered with the central BLE event dispatcher so we can catch
     * attribute-modified events (writes to RX, CCCD flip on TX). */
    GapSvcEventHandler* evt_handler;

    /* App callbacks (set after furi_hal_bt_start_app returns). */
    ClaudeBuddyRxCallback rx_cb;
    void* rx_ctx;
    ClaudeBuddyConnCallback conn_cb;
    void* conn_ctx;

    bool cccd_enabled; /* True once desktop has written 0x0001 to TX CCCD */
} ClaudeBuddyProfile;
_Static_assert(offsetof(ClaudeBuddyProfile, base) == 0, "base must be first");

/* =========================================================================
 *  BLE event handler  —  SKELETON ONLY
 *  Full wiring (parsing hci_uart_pckt, matching attribute handles, invoking
 *  callbacks) comes in the next round after you approve this skeleton.
 * ========================================================================= */
static BleEventAckStatus claude_buddy_on_ble_event(void* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    /* TODO (GATT wiring):
     *   - cast event to hci_uart_pckt* → evt_le_meta_event / evt_blue_aci
     *   - if ACI_GATT_ATTRIBUTE_MODIFIED_EVENT and Attr_Handle == rx_char.handle+1
     *         → p->rx_cb(data, len, p->rx_ctx)
     *   - if Attr_Handle == tx_char.handle+2  (CCCD)
     *         → p->cccd_enabled = (value & 0x0001) != 0
     *   - return BleEventAckFlowEnable on handled, BleEventNotAck otherwise
     */
    return BleEventNotAck;
}

/* =========================================================================
 *  Profile lifecycle
 * ========================================================================= */
static FuriHalBleProfileBase* claude_buddy_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    ClaudeBuddyProfile* p = malloc(sizeof(ClaudeBuddyProfile));
    memset(p, 0, sizeof(*p));
    p->base.config = ble_profile_claude_buddy;

    /* Silence -Werror=unused while the GATT wiring is stubbed. These
     * references disappear naturally when the TODO below is implemented. */
    (void)NUS_SVC_UUID;
    (void)NUS_RX_UUID;
    (void)NUS_TX_UUID;
    (void)claude_buddy_on_ble_event;

    /* ---- TODO (GATT wiring, next round) ----------------------------------
     * 1. Service:
     *      Service_UUID_t svc_uuid;
     *      memcpy(svc_uuid.Service_UUID_128, NUS_SVC_UUID, 16);
     *      ble_gatt_service_add(UUID_TYPE_128, &svc_uuid,
     *                           PRIMARY_SERVICE, 8, &p->svc_handle);
     *    (8 attr records: 1 svc decl + 2 char decls + 2 values + 1 CCCD + slack)
     *
     * 2. RX char (desktop → flipper):
     *      BleGattCharacteristicParams rx = {
     *        .name = "claude_rx",
     *        .uuid_type = UUID_TYPE_128,
     *        .uuid = { ...NUS_RX_UUID... },
     *        .char_properties = CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP,
     *        .security_permissions = CLAUDE_BUDDY_ATTR_PERM_RX,
     *        .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
     *        .is_variable = CHAR_VALUE_LEN_VARIABLE,
     *        .data_prop_type = FlipperGattCharacteristicDataFixed,
     *        .data.fixed = { .ptr = NULL, .length = CLAUDE_BUDDY_MAX_PAYLOAD },
     *      };
     *      ble_gatt_characteristic_init(p->svc_handle, &rx, &p->rx_char);
     *
     * 3. TX char (flipper → desktop):
     *      BleGattCharacteristicParams tx = {
     *        .name = "claude_tx",
     *        .uuid_type = UUID_TYPE_128,
     *        .uuid = { ...NUS_TX_UUID... },
     *        .char_properties = CHAR_PROP_NOTIFY,
     *        .security_permissions = CLAUDE_BUDDY_ATTR_PERM_TX,
     *        .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE, // for CCCD changes
     *        .is_variable = CHAR_VALUE_LEN_VARIABLE,
     *        .data_prop_type = FlipperGattCharacteristicDataCallback,
     *        .data.callback = { .fn = claude_buddy_tx_data_cb, .context = p },
     *      };
     *      ble_gatt_characteristic_init(p->svc_handle, &tx, &p->tx_char);
     *
     * 4. Register for BLE events so we can catch writes + CCCD flips:
     *      p->evt_handler = ble_event_dispatcher_register_svc_handler(
     *                         claude_buddy_on_ble_event, p);
     *
     * Phase-2 encryption flips CLAUDE_BUDDY_ATTR_PERM_{RX,TX} from
     * ATTR_PERMISSION_NONE to ATTR_PERMISSION_ENCRY_WRITE / _READ.
     * Gated by the #ifdef below.
     * --------------------------------------------------------------------- */

    return &p->base;
}

static void claude_buddy_profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base);
    furi_check(base->config == ble_profile_claude_buddy);
    ClaudeBuddyProfile* p = (ClaudeBuddyProfile*)base;

    /* ---- TODO (GATT wiring, next round) ----
     * if(p->evt_handler)  ble_event_dispatcher_unregister_svc_handler(p->evt_handler);
     * ble_gatt_characteristic_delete(p->svc_handle, &p->rx_char);
     * ble_gatt_characteristic_delete(p->svc_handle, &p->tx_char);
     * if(p->svc_handle)   ble_gatt_service_delete(p->svc_handle);
     */

    free(p);
}

/* =========================================================================
 *  GAP config  —  advertising, pairing, connection params
 * ========================================================================= */
static const GapConfig claude_buddy_gap_template = {
    .adv_service = {
        .UUID_Type = UUID_TYPE_128,
        .Service_UUID_128 = {
            0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
            0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e,
        },
    },
    .appearance_char = 0x0000, /* Generic / Unknown */
#ifdef CLAUDE_BUDDY_ENCRYPTED
    /* Phase 2: mirrors the ESP32 reference — bonded, MITM, DisplayOnly. */
    .bonding_mode = true,
    .pairing_method = GapPairingPinCodeShow,
#else
    /* Phase 1: open link, no pairing. If the desktop picker refuses a
     * non-bonded peripheral, rebuild with CLAUDE_BUDDY_ENCRYPTED. */
    .bonding_mode = false,
    .pairing_method = GapPairingNone,
#endif
    .conn_param = {
        .conn_int_min = CONN_INTERVAL_MIN,
        .conn_int_max = CONN_INTERVAL_MAX,
        .slave_latency = 0,
        .supervisor_timeout = 0,
    },
};

static void claude_buddy_profile_get_gap_config(GapConfig* cfg, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(cfg);
    memcpy(cfg, &claude_buddy_gap_template, sizeof(GapConfig));

    /* Use a distinct BLE MAC from the Serial profile (stock MAC) and the HID
     * profile (stock MAC with byte 2 bumped by 1). We bump byte 2 by 2. */
    memcpy(cfg->mac_address, furi_hal_version_get_ble_mac(), GAP_MAC_ADDR_SIZE);
    cfg->mac_address[2] += 2;

    snprintf(
        cfg->adv_name,
        FURI_HAL_VERSION_DEVICE_NAME_LENGTH,
        "Claude-%s",
        furi_hal_version_get_name_ptr());
}

/* =========================================================================
 *  Template export
 * ========================================================================= */
static const FuriHalBleProfileTemplate claude_buddy_profile_template = {
    .start = claude_buddy_profile_start,
    .stop = claude_buddy_profile_stop,
    .get_gap_config = claude_buddy_profile_get_gap_config,
};

const FuriHalBleProfileTemplate* const ble_profile_claude_buddy = &claude_buddy_profile_template;

/* =========================================================================
 *  Public surface  (stubs until GATT wiring round)
 * ========================================================================= */
void claude_buddy_profile_set_rx_callback(
    FuriHalBleProfileBase* base,
    ClaudeBuddyRxCallback cb,
    void* ctx) {
    furi_check(base && base->config == ble_profile_claude_buddy);
    ClaudeBuddyProfile* p = (ClaudeBuddyProfile*)base;
    p->rx_cb = cb;
    p->rx_ctx = ctx;
}

void claude_buddy_profile_set_conn_callback(
    FuriHalBleProfileBase* base,
    ClaudeBuddyConnCallback cb,
    void* ctx) {
    furi_check(base && base->config == ble_profile_claude_buddy);
    ClaudeBuddyProfile* p = (ClaudeBuddyProfile*)base;
    p->conn_cb = cb;
    p->conn_ctx = ctx;
}

bool claude_buddy_profile_tx(FuriHalBleProfileBase* base, const uint8_t* data, uint16_t len) {
    furi_check(base && base->config == ble_profile_claude_buddy);
    ClaudeBuddyProfile* p = (ClaudeBuddyProfile*)base;
    UNUSED(data);
    UNUSED(len);
    if(!p->cccd_enabled) return false;
    if(len > CLAUDE_BUDDY_MAX_PAYLOAD) return false;
    /* TODO: stash (data, len) in a per-profile outbox, then
     * ble_gatt_characteristic_update(p->svc_handle, &p->tx_char, p)
     * where the TX data callback reads the outbox. The update signature
     * doesn't take a length, so the callback is how variable-length TX
     * payloads flow through the existing primitive. */
    return false;
}

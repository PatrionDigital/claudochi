#include "claude_buddy_profile.h"
#include "ble_stack_shim.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <furi_hal_bt.h>
#include <furi_ble/gatt.h>
#include <furi_ble/event_dispatcher.h>

#include <ble/core/auto/ble_types.h>
#include <ble/core/ble_defs.h>
#include <ble/core/ble_std.h>

#include <string.h>

#define TAG "ClaudeBuddyProfile"

/* =========================================================================
 *  Nordic UART Service UUIDs — little-endian (ST BLE stack wire format).
 *  Canonical:  6e400001/2/3-b5a3-f393-e0a9-e50e24dcca9e
 * ========================================================================= */
#define NUS_UUID128(_last4) \
    { 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
      0x93, 0xf3, 0xa3, 0xb5, (_last4), 0x00, 0x40, 0x6e }

static const Service_UUID_t service_uuid = {.Service_UUID_128 = NUS_UUID128(0x01)};

/* ATT_MTU=247 default on WB55 → 244 payload bytes per notification. */
#define CLAUDE_BUDDY_MAX_PAYLOAD (244)

/* Connection interval window — 7.5 ms .. 45 ms. Same as serial_profile.c:46. */
#define CONN_INTERVAL_MIN (0x06)
#define CONN_INTERVAL_MAX (0x24)

/* =========================================================================
 *  Characteristic descriptors
 * ========================================================================= */
typedef enum {
    ClaudeBuddyCharRx = 0,
    ClaudeBuddyCharTx,
    ClaudeBuddyCharCount,
} ClaudeBuddyCharId;

#ifdef CLAUDE_BUDDY_ENCRYPTED
#define CLAUDE_BUDDY_PERM_WRITE (ATTR_PERMISSION_AUTHEN_WRITE)
#define CLAUDE_BUDDY_PERM_READ  (ATTR_PERMISSION_AUTHEN_READ)
#else
#define CLAUDE_BUDDY_PERM_WRITE (ATTR_PERMISSION_NONE)
#define CLAUDE_BUDDY_PERM_READ  (ATTR_PERMISSION_NONE)
#endif

/* Forward declaration so the chars[] array can reference the callback. */
static bool claude_buddy_tx_data_cb(
    const void* context,
    const uint8_t** data,
    uint16_t* data_len);

static const BleGattCharacteristicParams claude_buddy_chars[ClaudeBuddyCharCount] = {
    [ClaudeBuddyCharRx] =
        {.name = "claude_rx",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed = {.ptr = NULL, .length = CLAUDE_BUDDY_MAX_PAYLOAD},
         .uuid.Char_UUID_128 = NUS_UUID128(0x02),
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP,
         .security_permissions = CLAUDE_BUDDY_PERM_WRITE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [ClaudeBuddyCharTx] =
        {.name = "claude_tx",
         /* Callback-type data lets us support variable-length TX payloads
          * through ble_gatt_characteristic_update — which doesn't take a
          * length argument. The callback reads (data, len) from a
          * per-profile outbox that claude_buddy_profile_tx populates
          * before each update call. See claude_buddy_tx_data_cb below. */
         .data_prop_type = FlipperGattCharacteristicDataCallback,
         .data.callback = {.fn = claude_buddy_tx_data_cb, .context = NULL},
         .uuid.Char_UUID_128 = NUS_UUID128(0x03),
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_NOTIFY,
         .security_permissions = CLAUDE_BUDDY_PERM_READ,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE, /* reports CCCD writes */
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
};

/* =========================================================================
 *  Profile instance
 * ========================================================================= */
typedef struct {
    FuriHalBleProfileBase base;

    uint16_t svc_handle;
    BleGattCharacteristicInstance chars[ClaudeBuddyCharCount];
    GapSvcEventHandler* evt_handler;

    ClaudeBuddyRxCallback rx_cb;
    void* rx_ctx;
    ClaudeBuddyConnCallback conn_cb;
    void* conn_ctx;
    bool cccd_enabled;

    /* TX outbox. claude_buddy_profile_tx copies the caller's bytes here,
     * then ble_gatt_characteristic_update → claude_buddy_tx_data_cb reads
     * them out. Mutex guards against concurrent TX calls (main thread vs
     * prompt-handler thread, if any). */
    FuriMutex* tx_mtx;
    uint8_t tx_outbox[CLAUDE_BUDDY_MAX_PAYLOAD];
    uint16_t tx_outbox_len;
} ClaudeBuddyProfile;
_Static_assert(offsetof(ClaudeBuddyProfile, base) == 0, "base must be first");

/* Called twice by the stack:
 *   (1) at ble_gatt_characteristic_init time with data == NULL, to learn
 *       the characteristic's maximum length. Return the variable-length cap.
 *   (2) at each ble_gatt_characteristic_update(..., source=profile_ptr) call,
 *       to read the current notification payload from the profile's outbox. */
static bool claude_buddy_tx_data_cb(
    const void* context,
    const uint8_t** data,
    uint16_t* data_len) {
    if(data == NULL) {
        *data_len = CLAUDE_BUDDY_MAX_PAYLOAD;
        return false;
    }
    const ClaudeBuddyProfile* p = (const ClaudeBuddyProfile*)context;
    *data = p->tx_outbox;
    *data_len = p->tx_outbox_len;
    return false; /* buffer is profile-owned; do not free */
}

/* =========================================================================
 *  BLE event handler — runs on the BLE event dispatcher thread
 * ========================================================================= */
static BleEventAckStatus claude_buddy_on_ble_event(void* event, void* context) {
    ClaudeBuddyProfile* p = (ClaudeBuddyProfile*)context;
    hci_event_pckt* event_pckt = (hci_event_pckt*)(((hci_uart_pckt*)event)->data);
    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;

    evt_blecore_aci* blecore_evt = (evt_blecore_aci*)event_pckt->data;
    if(blecore_evt->ecode != ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) return BleEventNotAck;

    aci_gatt_attribute_modified_event_rp0* mod =
        (aci_gatt_attribute_modified_event_rp0*)blecore_evt->data;

    const uint16_t rx_value_h = p->chars[ClaudeBuddyCharRx].handle + 1;
    const uint16_t tx_cccd_h = p->chars[ClaudeBuddyCharTx].handle + 2;

    if(mod->Attr_Handle == rx_value_h) {
        if(p->rx_cb && mod->Attr_Data_Length > 0) {
            p->rx_cb(mod->Attr_Data, mod->Attr_Data_Length, p->rx_ctx);
        }
        return BleEventAckFlowEnable;
    }

    if(mod->Attr_Handle == tx_cccd_h) {
        bool was_enabled = p->cccd_enabled;
        p->cccd_enabled = (mod->Attr_Data_Length >= 1) && ((mod->Attr_Data[0] & 0x01) != 0);
        if(p->conn_cb && p->cccd_enabled != was_enabled) {
            p->conn_cb(p->cccd_enabled, p->conn_ctx);
        }
        return BleEventAckFlowEnable;
    }

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
    p->tx_mtx = furi_mutex_alloc(FuriMutexTypeNormal);

    p->evt_handler = ble_event_dispatcher_register_svc_handler(claude_buddy_on_ble_event, p);

    /* 6 attribute records: svc + RX(decl,value) + TX(decl,value,CCCD) + slack. */
    if(!ble_gatt_service_add(UUID_TYPE_128, &service_uuid, PRIMARY_SERVICE, 8, &p->svc_handle)) {
        FURI_LOG_E(TAG, "ble_gatt_service_add failed");
        ble_event_dispatcher_unregister_svc_handler(p->evt_handler);
        free(p);
        return NULL;
    }

    for(uint8_t i = 0; i < ClaudeBuddyCharCount; i++) {
        ble_gatt_characteristic_init(p->svc_handle, &claude_buddy_chars[i], &p->chars[i]);
    }

    return &p->base;
}

static void claude_buddy_profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base);
    furi_check(base->config == ble_profile_claude_buddy);
    ClaudeBuddyProfile* p = (ClaudeBuddyProfile*)base;

    if(p->evt_handler) ble_event_dispatcher_unregister_svc_handler(p->evt_handler);
    for(uint8_t i = 0; i < ClaudeBuddyCharCount; i++) {
        ble_gatt_characteristic_delete(p->svc_handle, &p->chars[i]);
    }
    if(p->svc_handle) ble_gatt_service_delete(p->svc_handle);
    if(p->tx_mtx) furi_mutex_free(p->tx_mtx);
    free(p);
}

/* =========================================================================
 *  GAP config — advertising, pairing, connection params
 * ========================================================================= */
/* NOTE on adv_service — authoritative budget math.
 *
 * aci_gap_set_discoverable AUTO-INJECTS two AD fields, per ST's doxygen
 * at stm32wb_copro/wpan/ble/core/auto/ble_gap_aci.h L127-152:
 *   - Flags AD            (3 bytes on wire: 1 len + 1 type 0x01 + 1 value)
 *   - TX Power Level AD   (3 bytes on wire: 1 len + 1 type 0x0A + 1 value)
 * Plus a Peripheral Connection Interval Range AD (6 bytes) IF both
 * Slave_Conn_Interval_Min and _Max are non-zero. Flipper's gap.c passes
 * 0/0 so that field is absent here.
 *
 * Legacy adv PDU cap is 31 bytes. With zero conn-intervals, that leaves
 *   31 - 3 - 3 = 25 bytes for Name + ServiceUUID together.
 *
 * Each AD is (1 wire-len byte) + (Length_param bytes caller supplies).
 * Length_param itself counts the AD type byte the caller prepends. So:
 *   Local_Name_Length  = strlen("<0x09><chars>")   = 1 + char_count
 *   Service_Uuid_Length = 1 (type=0x07) + 16 (128-bit UUID data) = 17
 *                     or 1 (type=0x03) + 2  (16-bit UUID data)  = 3
 *
 * Fit check (zero conn-intervals):
 *   (1 + Local_Name_Length) + (1 + Service_Uuid_Length) <= 25
 *   Local_Name_Length + Service_Uuid_Length <= 23
 *
 *   128-bit in adv:  Local_Name_Length <= 6  →  0x09 + 5 name chars max
 *    16-bit in adv:  Local_Name_Length <= 20 →  0x09 + 19 name chars max
 *
 * Over-budget returns BLE_STATUS_INVALID_PARAMS (0x92), which Flipper's
 * gap.c:470 silently logs; the state machine transitions to Advertising
 * anyway, so our UI reads "BT: Advertising" while nothing is on air.
 *
 * Claude Desktop's Hardware Buddy picker filters ONLY by device-name
 * prefix "claude" or "nibblet" (case-insensitive) — confirmed by
 * reading the main-process event handler in /Applications/Claude.app's
 * extracted asar. It calls Chromium's requestDevice with
 * acceptAllDevices:true and lists the NUS UUID under optionalServices
 * (which authorizes post-connect GATT access, not scan filtering).
 * Therefore we DO NOT need the 128-bit NUS UUID in the adv packet;
 * the 16-bit placeholder is fine, and the full NUS UUID is discovered
 * over GATT after the central connects. */
static const GapConfig claude_buddy_gap_template = {
    .adv_service =
        {
            .UUID_Type = UUID_TYPE_16,
            .Service_UUID_16 = 0x0000, /* placeholder — real NUS UUID is in GATT */
        },
    .appearance_char = 0x0000, /* Generic / Unknown */
#ifdef CLAUDE_BUDDY_ENCRYPTED
    .bonding_mode = true,
    .pairing_method = GapPairingPinCodeShow,
#else
    .bonding_mode = false,
    .pairing_method = GapPairingNone,
#endif
    .conn_param =
        {
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

    /* Distinct BLE MAC from Serial (stock) and HID (stock+1). */
    memcpy(cfg->mac_address, furi_hal_version_get_ble_mac(), GAP_MAC_ADDR_SIZE);
    cfg->mac_address[2] += 2;

    /* adv_name construction mirrors hid_profile.c:420 exactly — the only
     * change is substituting "Claude" for "Control". That profile
     * advertises successfully as e.g. "Control <device_name>" and is
     * visible in macOS Bluetooth settings, so we follow its pattern
     * 1:1 rather than recombine fragments from other sources.
     *
     * The visible adv name is "Claude " + furi_hal_version_get_name_ptr()
     * — so each Flipper shows up as "Claude <its own device name>".
     * Claude Desktop's Hardware Buddy picker filters by the "claude"
     * prefix (case-insensitive), which matches regardless of device
     * name.
     *
     *   - First byte of adv_name is 0x09 (AD_TYPE_COMPLETE_LOCAL_NAME),
     *     copied from furi_hal_version_get_ble_local_device_name_ptr()[0]
     *     which furi_hal_version.c:106 explicitly initializes to
     *     AD_TYPE_COMPLETE_LOCAL_NAME.
     *   - Format "%c%s %s" with space separator, matching HID.
     *
     * Note: the rendered device name comes from the Flipper's own
     * "Device Name" setting (Settings → System → Device Name), not
     * from this code. The stock default is a preset list; power
     * users rename theirs. "Claude" is the only fixed substring we
     * contribute to the adv packet. */
    snprintf(
        cfg->adv_name,
        FURI_HAL_VERSION_DEVICE_NAME_LENGTH,
        "%c%s %s",
        furi_hal_version_get_ble_local_device_name_ptr()[0],
        "Claude",
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
 *  Public surface
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
    if(len == 0 || len > CLAUDE_BUDDY_MAX_PAYLOAD) return false;

    /* Deliberately do NOT short-circuit on our tracked cccd_enabled flag.
     * On a bonded reconnect the stack restores the CCCD value from the
     * bond without emitting an ACI_GATT_ATTRIBUTE_MODIFIED event, so
     * our flag can stay false while notifications would in fact be
     * delivered. Call through and let the stack be the authority — it
     * silently no-ops the notify if no central is subscribed, rather
     * than returning an error. */

    furi_mutex_acquire(p->tx_mtx, FuriWaitForever);
    memcpy(p->tx_outbox, data, len);
    p->tx_outbox_len = len;
    /* ble_gatt_characteristic_update returns true on FAILURE
     * (see gatt.c:144: `return result != BLE_STATUS_SUCCESS`). Invert. */
    bool failed = ble_gatt_characteristic_update(p->svc_handle, &p->chars[ClaudeBuddyCharTx], p);
    furi_mutex_release(p->tx_mtx);
    return !failed;
}

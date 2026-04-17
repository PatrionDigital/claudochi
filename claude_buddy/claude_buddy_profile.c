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
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed = {.ptr = NULL, .length = CLAUDE_BUDDY_MAX_PAYLOAD},
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
} ClaudeBuddyProfile;
_Static_assert(offsetof(ClaudeBuddyProfile, base) == 0, "base must be first");

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

    FURI_LOG_I(
        TAG,
        "NUS service up: svc=%u rx=%u tx=%u",
        p->svc_handle,
        p->chars[ClaudeBuddyCharRx].handle,
        p->chars[ClaudeBuddyCharTx].handle);

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
    free(p);
}

/* =========================================================================
 *  GAP config — advertising, pairing, connection params
 * ========================================================================= */
/* NOTE on adv_service:
 *
 * The Flipper's GAP layer supports advertising one service UUID, and our
 * first attempt used 128-bit NUS (0x6e400001-...) to match what the Claude
 * desktop picker probably filters on. With a 128-bit UUID (18 bytes) +
 * flags (3 bytes) + name AD, the 31-byte legacy-adv budget is tight and
 * in practice our peripheral never appeared in LightBlue's scan.
 *
 * Both in-tree reference profiles (serial_profile.c, hid_profile.c)
 * advertise 16-bit UUIDs only. We follow their known-working pattern here:
 * advertise with a 16-bit placeholder, and keep the full 128-bit NUS
 * service at the GATT layer so centrals discover it post-connection. If
 * Claude Desktop's picker turns out to require the 128-bit NUS in adv
 * data itself, we'll revisit — but we can't test that path until the
 * server-side Hardware Buddy gate is flipped for our account. */
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

    /* With a 16-bit placeholder UUID in adv we have plenty of room in
     * the 31-byte budget for the full name:
     *   Flags AD:              3 bytes
     *   16-bit UUID AD:        4 bytes
     *   Name AD:               ≤ 24 bytes → up to 22 chars of name
     *
     * Name: "Claude-<flipper_name>" per REFERENCE.md. "Claude-Raderado"
     * is 15 chars, fits comfortably. */
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
    UNUSED(data);
    UNUSED(len);
    if(!p->cccd_enabled) return false;
    if(len > CLAUDE_BUDDY_MAX_PAYLOAD) return false;
    /* Piece 3 (next round): outbox + callback-type char data descriptor. */
    return false;
}

#pragma once

/* Types and constants from the STM32WB BLE stack that are used by
 * Unleashed's own services (serial_service.c, hid_service.c) via the
 * <ble/ble.h> umbrella header, but are NOT re-exported in the ufbt SDK.
 *
 * Sources (verified against flipperdevices/stm32wb_copro@dev):
 *   wpan/ble/core/auto/ble_vs_codes.h   — ACI_*_VSEVT_CODE defines
 *   wpan/interface/patterns/ble_thread/tl/tl.h — HCI packet typedefs
 *
 * These are ABI-stable — the BLE coprocessor firmware defines them. */

#include <stdint.h>

#ifndef PACKED_STRUCT
#define PACKED_STRUCT struct __attribute__((packed))
#endif

typedef PACKED_STRUCT {
    uint8_t type;
    uint8_t data[1];
} hci_uart_pckt;

typedef PACKED_STRUCT {
    uint8_t evt;
    uint8_t plen;
    uint8_t data[1];
} hci_event_pckt;

typedef PACKED_STRUCT {
    uint16_t ecode;
    uint8_t data[1];
} evt_blecore_aci;

/* Vendor-specific GATT server event codes. Only the ones we actually
 * use are pulled in — add more from ble_vs_codes.h as needed. */
#define ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE 0x0C01U

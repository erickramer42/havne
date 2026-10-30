// File/layer: app/blepicker.h | Qt6 Core, app layer, C++17
// Testing: UNIT: BridgeTest.BlePicker* (test_bridge): the fixed list's
//          order over made-up service lists, a service missing its
//          characteristics passed over, the first service with a notifying
//          and a writable characteristic last, and none; BENCH: none
//          until a BLE adapter is on hand (ADR-0028 §4)

#pragma once

#include <QString>

#include <vector>

// What a BLE adapter's GATT services look like once discovered, as plain
// data: the app's link (btstream.cpp) fills it from QtBluetooth, and the
// picker below decides which characteristics carry the ELM327 text, so
// the choice is testable without hardware. UUIDs are 128-bit, lower case,
// without braces; bleUuid() expands a 16-bit one.
struct BleCharacteristic {
    QString uuid;
    bool write = false;           // write with response
    bool writeNoResponse = false; // write without response
    bool notify = false;
    bool indicate = false;
};

struct BleService {
    QString uuid;
    std::vector<BleCharacteristic> characteristics;
};

// Indices into the services and the chosen service's characteristics;
// all -1 when nothing fits
struct BlePick {
    int service = -1;
    int write = -1;
    int notify = -1;
    bool found() const { return service >= 0; }
};

// "fff0" → "0000fff0-0000-1000-8000-00805f9b34fb"; a 128-bit UUID comes
// back lower case without braces
QString bleUuid(const QString& uuid);

// ADR-0028 §2's list, the first match in this order:
//   1. the Nordic UART service: write 6E400002, notify 6E400003
//   2. FFF0: write FFF2, notify FFF1
//   3. FFE0: FFE1 both writes and notifies
//   4. the first service with a notifying characteristic and a writable
//      one, the first of each
// A listed service whose characteristics are missing, or can't write or
// notify, is passed over for the next rule.
BlePick pickBleService(const std::vector<BleService>& services);

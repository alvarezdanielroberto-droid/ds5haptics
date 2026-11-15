// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// Tests unitarios: batería, reportes de salida y traducción de entradas (virtual_pads.h).
#include "virtual_pads.h"
#include "ds5_bt_haptics.h"
#include "virtual_dualsense.h"
#include <cstdio>
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FALLO: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

int main() {
    uint8_t ds[64] = {};
    // Batería
    ds[53] = 0x01; auto b = ds5::parseBattery(ds);
    CHECK(b.percent == 15 && !b.charging && !b.full, "nivel 1 = 15%%");
    ds[53] = 0x00; CHECK(ds5::parseBattery(ds).percent == 5, "nivel 0 = 5%%");
    ds[53] = 0x0A; CHECK(ds5::parseBattery(ds).percent == 100, "nivel 10 = 100%%");
    ds[53] = 0x14; b = ds5::parseBattery(ds); CHECK(b.percent == 45 && b.charging, "cargando 45%%");
    ds[53] = 0x2A; b = ds5::parseBattery(ds); CHECK(b.percent == 100 && b.full && !b.charging, "carga completa");

    // El byte de estado sobrevive la conversión Bluetooth -> USB (offset 54 en BT, 53 en USB)
    uint8_t bt[78] = {0x31}; bt[54] = 0x13; uint8_t usb[64];
    CHECK(ds5::btInputToUsb(bt, sizeof(bt), usb) && usb[53] == 0x13, "estado de batería en el reporte USB");

    // LED del micrófono: al convertir a Bluetooth queda en el lugar que espera el mando
    uint8_t out[ds5::kUsbOutSize], btOut[ds5::kMainReportSize];
    ds5::buildMicLedReport(out, 2);
    CHECK(ds5::usbOutputToBt(out, sizeof(out), 0, btOut), "mic LED a BT");
    CHECK(btOut[0] == 0x31 && btOut[3] == 0 && btOut[4] == 0x01 && btOut[11] == 2, "mic LED: bandera y modo pulsante");
    uint32_t crc = ds5::crc32_bt(btOut, 74);
    CHECK((btOut[74] | (btOut[75] << 8) | (btOut[76] << 16) | (uint32_t(btOut[77]) << 24)) == crc, "mic LED: CRC");

    // Vibración clásica: firmware viejo y nuevo
    ds5::buildRumbleReport(out, 200, 100, false);
    ds5::usbOutputToBt(out, sizeof(out), 1, btOut);
    CHECK(btOut[3] == 0x03 && btOut[5] == 100 && btOut[6] == 200 && btOut[3 + 38] == 0, "vibración v1");
    ds5::buildRumbleReport(out, 200, 100, true);
    ds5::usbOutputToBt(out, sizeof(out), 2, btOut);
    CHECK(btOut[3] == 0x02 && btOut[3 + 38] == 0x04 && btOut[5] == 100 && btOut[6] == 200, "vibración v2");

    // Barra de luz
    ds5::buildLightbarUsbReport(out, 1, 2, 3);
    ds5::usbOutputToBt(out, sizeof(out), 3, btOut);
    CHECK(btOut[4] == 0x04 && btOut[3 + 44] == 1 && btOut[3 + 45] == 2 && btOut[3 + 46] == 3, "barra de luz");

    // Xbox: cruceta en las 8 direcciones y centro de los sticks
    const uint8_t expectDpad[9] = {0x01, 0x09, 0x08, 0x0A, 0x02, 0x06, 0x04, 0x05, 0x00};
    for (int hat = 0; hat <= 8; ++hat) {
        std::memset(ds, 0, sizeof(ds));
        ds[1] = ds[2] = ds[3] = ds[4] = 0x80;
        ds[8] = uint8_t(hat);
        uint8_t x[20];
        ds5::dualSenseToXbox(ds, x);
        CHECK((x[2] & 0x0F) == expectDpad[hat], "cruceta %d -> %02X", hat, x[2] & 0x0F);
        int lx = int16_t(x[6] | (x[7] << 8)), ly = int16_t(x[8] | (x[9] << 8));
        CHECK(lx >= 0 && lx <= 256 && ly >= -256 && ly <= 0, "stick centrado: %d %d", lx, ly);
    }
    // Xbox: cada botón por separado
    struct { int byte; uint8_t bit; int outByte; uint8_t outBit; const char* name; } map[] = {
        {8, 0x20, 3, 0x10, "cruz->A"}, {8, 0x40, 3, 0x20, "círculo->B"}, {8, 0x10, 3, 0x40, "cuadrado->X"},
        {8, 0x80, 3, 0x80, "triángulo->Y"}, {9, 0x01, 3, 0x01, "L1->LB"}, {9, 0x02, 3, 0x02, "R1->RB"},
        {9, 0x10, 2, 0x20, "create->back"}, {9, 0x20, 2, 0x10, "options->start"}, {9, 0x40, 2, 0x40, "L3"},
        {9, 0x80, 2, 0x80, "R3"}, {10, 0x01, 3, 0x04, "PS->guía"}, {10, 0x02, 2, 0x20, "panel->back"}};
    for (auto& m : map) {
        std::memset(ds, 0, sizeof(ds));
        ds[8] = 8;   // cruceta suelta
        ds[m.byte] |= m.bit;
        uint8_t x[20];
        ds5::dualSenseToXbox(ds, x);
        uint8_t e2 = m.outByte == 2 ? m.outBit : 0, e3 = m.outByte == 3 ? m.outBit : 0;
        CHECK(x[2] == e2 && x[3] == e3, "%s: %02X %02X", m.name, x[2], x[3]);
    }
    // Extremos de los sticks
    std::memset(ds, 0, sizeof(ds)); ds[8] = 8; ds[1] = 0xFF; ds[2] = 0x00;
    uint8_t x[20]; ds5::dualSenseToXbox(ds, x);
    CHECK(int16_t(x[6] | (x[7] << 8)) == 32767 && int16_t(x[8] | (x[9] << 8)) == 32767, "derecha y arriba = +32767");

    // DS4: batería completa y tamaño del descriptor
    std::memset(ds, 0, sizeof(ds)); ds[53] = 0x2A;
    uint8_t d4[64]; ds5::dualSenseToDs4(ds, d4, 5);
    CHECK(d4[0] == 1 && d4[30] == 0x1B && (d4[7] >> 2) == 5, "DS4 batería completa y contador");
    auto in = ds5::hidReportSizes(ds5::kDs4ReportDescriptor, sizeof(ds5::kDs4ReportDescriptor), 0x80);
    CHECK(in.size() == 1 && in[1] == 63, "DS4 reporte de entrada de 63+1 bytes");

    if (!failures) std::printf("OK: batería, vibración, LED y traducciones PS4/Xbox correctas\n");
    return failures ? 1 : 0;
}

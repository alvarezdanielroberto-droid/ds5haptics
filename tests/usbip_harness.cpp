// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// Banco de pruebas: DualSense virtual servido por USB/IP con un mando Bluetooth simulado.
// Lo usan tests/test_usbip_client.py y tests/test_pads.py.
// Uso: usbip_harness <puerto> [ps5|ps4|xbox|switch]
//   switch: arranca como DualSense y cambia de mando al leer "ps4" / "xbox" / "ps5" por stdin.
#include "virtual_dualsense.h"
#include "virtual_pads.h"
#include <iostream>
#include <chrono>
#include <csignal>
#include <cstdio>

static std::atomic<bool> stopFlag{false};

// Reporte BT 0x31 sintético: byte i = (i * 7 + contador) & 0xFF
static void makeBt(uint8_t bt[78], uint8_t counter) {
    bt[0] = 0x31;
    for (int i = 1; i < 78; ++i) bt[i] = uint8_t(i * 7 + counter);
}

int main(int argc, char** argv) {
    uint16_t port = uint16_t(argc > 1 ? std::atoi(argv[1]) : 3240);
    std::signal(SIGTERM, [](int) { stopFlag = true; });
    setvbuf(stdout, nullptr, _IOLBF, 0);

    ds5::VirtualDualSense::Callbacks cb;
    cb.getFeature = [](uint8_t id, std::vector<uint8_t>& out) {
        if (id != 0x05 && id != 0x20) return false;
        out.assign(547, 0);  // como lo devuelve Windows por BT
        out[0] = id;
        size_t sz = ds5::usbFeatureSize(id);
        for (size_t i = 1; i < sz; ++i) out[i] = uint8_t(0xA0 + i);
        return true;
    };
    uint8_t seq = 0;
    cb.output = [&seq](const uint8_t* usb, size_t len) {
        uint8_t bt[ds5::kMainReportSize];
        if (!ds5::usbOutputToBt(usb, len, seq++, bt)) { std::printf("OUTPUT_BAD\n"); return; }
        uint32_t crc = ds5::crc32_bt(bt, 74);
        uint32_t st = bt[74] | (bt[75] << 8) | (bt[76] << 16) | (uint32_t(bt[77]) << 24);
        bool ok = crc == st && bt[0] == 0x31 && bt[2] == 0x10 && std::memcmp(bt + 3, usb + 1, 47) == 0;
        std::printf("OUTPUT %s len=%zu\n", ok ? "OK" : "BAD", len);
    };
    cb.setFeature = [](uint8_t id, const uint8_t*, size_t len) { std::printf("SETFEATURE %02X %zu\n", id, len); };
    cb.log = [](const std::string& s) { std::printf("LOG %s\n", s.c_str()); };
    // El cliente de prueba manda: canal 1 = canal 2 = 1000, canal 3 = k, canal 4 = -k
    std::atomic<size_t> audioFrames{0};
    std::atomic<bool> audioOk{true};
    cb.audioOut = [&](const int16_t* f, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const int16_t* fr = f + i * 4;
            if (fr[0] != 1000 || fr[1] != 1000 || fr[2] != int16_t(-fr[3])) audioOk = false;
        }
        audioFrames += n;
    };

    ds5::VirtualDualSense dev(cb);

    // Mandos de compatibilidad (PS4 / Xbox 360)
    std::string mode = argc > 2 ? argv[2] : "ps5";
    ds5::SimplePad::Callbacks pcb;
    pcb.rumble = [](uint8_t s, uint8_t w) { std::printf("RUMBLE %u %u\n", s, w); };
    pcb.lightbar = [](uint8_t r, uint8_t g, uint8_t b) { std::printf("LIGHTBAR %u %u %u\n", r, g, b); };
    pcb.getFeature = [](uint8_t id, std::vector<uint8_t>& out) {
        out.assign(64, 0);
        out[0] = id;
        if (id == 0x05) { for (int i = 1; i < 41; ++i) out[i] = uint8_t(0xA0 + i); return true; }
        if (id == 0x09) { const uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}; std::memcpy(out.data() + 1, mac, 6); return true; }
        return false;
    };
    ds5::VirtualDS4 ps4(pcb);
    ds5::VirtualXbox360 xbox(pcb);
    std::atomic<int> active{mode == "ps4" ? 1 : mode == "xbox" ? 2 : 0};

    usbip::Server srv;
    usbip::Device* first = active == 1 ? static_cast<usbip::Device*>(&ps4) : active == 2 ? static_cast<usbip::Device*>(&xbox) : &dev;
    if (!srv.start(*first, port, [](const std::string& s) { std::printf("LOG %s\n", s.c_str()); })) {
        std::printf("NO_LISTEN\n");
        return 1;
    }
    std::printf("READY\n");

    std::thread stdinThread;
    if (mode == "switch") {
        stdinThread = std::thread([&] {
            std::string line;
            while (std::getline(std::cin, line)) {
                int m = line == "ps4" ? 1 : line == "xbox" ? 2 : line == "ps5" ? 0 : -1;
                if (m < 0) continue;
                active = m;
                srv.setDevice(m == 1 ? static_cast<usbip::Device&>(ps4) : m == 2 ? static_cast<usbip::Device&>(xbox) : dev);
                srv.dropClient();
                std::printf("SWITCHED %s\n", line.c_str());
            }
        });
        stdinThread.detach();
    }

    // Entrada fija del DualSense para los mandos de compatibilidad
    uint8_t ds[64] = {};
    ds[0] = 0x01; ds[1] = 0x00; ds[2] = 0xFF; ds[3] = 0x80; ds[4] = 0x40;   // sticks
    ds[5] = 0x11; ds[6] = 0x22;                                              // L2, R2
    ds[8] = 0x20 | 0x02;      // cruz + cruceta a la derecha
    ds[9] = 0x01 | 0x20;      // L1 + Options
    ds[10] = 0x01;            // PS
    ds[16] = 0x34; ds[17] = 0x12; ds[22] = 0x78; ds[23] = 0x56;              // giroscopio X, acelerómetro X
    ds[33] = 0x05; ds[34] = 0xE8; ds[35] = 0xC3; ds[36] = 0x21;              // toque: x=1000, y=540
    ds[37] = 0x80;                                                           // segundo dedo inactivo
    ds[53] = 0x01;            // batería nivel 1 (15%), descargando

    uint8_t counter = 0;
    while (!stopFlag) {
        if (active == 0) {
            uint8_t bt[78], usb[64];
            makeBt(bt, counter++);
            if (ds5::btInputToUsb(bt, sizeof(bt), usb)) dev.pushInput(usb);
        } else if (active == 1) {
            ps4.pushInput(ds);
        } else {
            xbox.pushInput(ds);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    srv.stop();
    std::printf("AUDIO frames=%zu %s\n", audioFrames.load(), audioOk ? "OK" : "BAD");
    std::printf("STOPPED\n");
    return 0;
}

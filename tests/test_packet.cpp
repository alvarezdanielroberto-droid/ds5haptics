// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// Verifica que ds5::HapticsReport produce exactamente los mismos bytes que el
// layout original de SAxense (structs packed con bitfields, compilado con GCC/Clang).
// Compilar en Linux:  g++ -std=c++17 -O2 -I../src test_packet.cpp -o test_packet
#include "ds5_bt_haptics.h"
#include "haptic_audio.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

// ---- Réplica del layout de referencia de SAxense (C con extensiones GNU) ----
// Esta sección reproduce la estructura del reporte y la función CRC de SAxense
// (Copyright Egor Vorontsov / Sdore, MPL-2.0, https://github.com/egormanga/SAxense)
// para comparar byte a byte contra la implementación original.
namespace ref {
#define REPORT_SIZE 141
#define SAMPLE_SIZE 64
static uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = ~0xEADA2D49u;
    while (size--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; i++) crc = ((crc >> 1) ^ (0xEDB88320u & -(crc & 1)));
    }
    return ~crc;
}
struct __attribute__((packed)) packet_t { uint8_t pid : 6; bool unk : 1, sized : 1; uint8_t length; uint8_t data[]; };
struct __attribute__((packed)) report_t {
    uint8_t report_id;
    union {
        struct __attribute__((packed)) { uint8_t tag : 4, seq : 4; uint8_t data[]; };
        struct __attribute__((packed)) { uint8_t payload[REPORT_SIZE - sizeof(uint32_t)]; uint32_t crc; };
    };
};
}  // namespace ref

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FALLO: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

int main() {
    // 1) Tamaño total
    CHECK(sizeof(ref::report_t) == ds5::kHapticsReportSize, "tamaño ref=%zu nuestro=%zu",
          sizeof(ref::report_t), ds5::kHapticsReportSize);

    // 2) La constante de semilla de SAxense == CRC tras procesar 0xA2 desde 0xFFFFFFFF
    {
        uint32_t c = 0xFFFFFFFFu; c ^= 0xA2;
        for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        CHECK(c == ~0xEADA2D49u, "semilla: %08X vs %08X", c, ~0xEADA2D49u);
    }

    // 3) Construcción de referencia, igual que main() de SAxense
    auto* r = static_cast<ref::report_t*>(std::calloc(1, sizeof(ref::report_t)));
    r->report_id = 0x32; r->tag = 0;
    auto* p0 = reinterpret_cast<ref::packet_t*>(r->data + 0);
    p0->pid = 0x11; p0->sized = true; p0->length = 7;
    const uint8_t d0[7] = {0xFE, 0, 0, 0, 0, 0xFF, 0};
    std::memcpy(p0->data, d0, 7);
    auto* p1 = reinterpret_cast<ref::packet_t*>(r->data + 2 + 7);
    p1->pid = 0x12; p1->sized = true; p1->length = SAMPLE_SIZE;
    uint8_t* ii = &p0->data[6];
    uint8_t* sample = p1->data;

    ds5::HapticsReport ours;
    auto audio = ds5::genSweep(40, 400, 2.0, 0.8f);

    // Muchos ciclos para cubrir el desborde del contador (>255)
    for (size_t blk = 0; blk < 600; ++blk) {
        int8_t chunk[64];
        for (int i = 0; i < 64; ++i) {
            size_t k = (blk * 64 + i) % audio.size();
            chunk[i] = audio[k];
        }
        // Referencia (secuencia de proc() en SAxense)
        std::memcpy(sample, chunk, 64);
        (*ii)++;
        r->crc = ref::crc32(reinterpret_cast<uint8_t*>(r), 1 + sizeof(r->payload));
        // Nuestra
        ours.setSamples(chunk);
        if (std::memcmp(r, ours.data(), ds5::kHapticsReportSize) != 0) {
            const uint8_t* a = reinterpret_cast<uint8_t*>(r);
            for (size_t i = 0; i < ds5::kHapticsReportSize; ++i)
                if (a[i] != ours.data()[i]) { CHECK(false, "bloque %zu byte %zu: ref=%02X nuestro=%02X", blk, i, a[i], ours.data()[i]); break; }
            break;
        }
    }

    // 4) Offsets documentados
    CHECK(ours.data()[0] == 0x32 && ours.data()[2] == 0x91 && ours.data()[3] == 7 &&
          ours.data()[11] == 0x92 && ours.data()[12] == 64, "cabeceras inesperadas");
    CHECK(std::fabs(ds5::kReportPeriodSec * 1000 - 10.6667) < 0.001, "periodo %.4f ms", ds5::kReportPeriodSec * 1000);

    // 5) Remuestreo: seno de 150 Hz a 48 kHz -> 3 kHz conserva frecuencia y amplitud
    {
        std::vector<float> in(48000 * 2);
        for (size_t i = 0; i < 48000; ++i) in[i * 2] = in[i * 2 + 1] = 0.5f * std::sin(2 * M_PI * 150 * i / 48000.0);
        auto out = ds5::resampleStereo(in, 48000, 3000);
        CHECK(out.size() == 3000 * 2, "frames remuestreados %zu", out.size() / 2);
        double err = 0;
        for (size_t i = 200; i < 2800; ++i) err = std::max(err, std::fabs(out[i * 2] - 0.5 * std::sin(2 * M_PI * 150 * i / 3000.0)));
        CHECK(err < 0.01, "error de remuestreo %.4f", err);
        // Un tono de 2500 Hz (sobre Nyquist de 1500) debe quedar atenuado
        for (size_t i = 0; i < 48000; ++i) in[i * 2] = in[i * 2 + 1] = 0.5f * std::sin(2 * M_PI * 2500 * i / 48000.0);
        out = ds5::resampleStereo(in, 48000, 3000);
        double peak = 0;
        for (size_t i = 200; i < 2800; ++i) peak = std::max(peak, double(std::fabs(out[i * 2])));
        CHECK(peak < 0.01, "aliasing sin filtrar, pico %.4f", peak);
    }

    // 6) Reporte 0x31 de barra de luz: tamaño, cabecera y CRC al final
    {
        uint8_t lb[ds5::kMainReportSize];
        ds5::buildLightbarReport(lb, 0, 0x80, 0x00, 0xFF, 0x04);
        uint32_t crc = ds5::crc32_bt(lb, ds5::kMainReportSize - 4);
        uint32_t stored = lb[74] | (lb[75] << 8) | (lb[76] << 16) | (uint32_t(lb[77]) << 24);
        CHECK(lb[0] == 0x31 && lb[2] == 0x10 && lb[47] == 0x80 && lb[49] == 0xFF, "layout 0x31");
        CHECK(crc == stored, "CRC 0x31");
    }

    std::free(r);
    if (failures == 0) std::printf("OK: 600 reportes idénticos a SAxense, CRC y remuestreo correctos\n");
    return failures ? 1 : 0;
}

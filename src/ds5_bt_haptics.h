// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// ds5_bt_haptics.h — Constructor portable del reporte de háptica del DualSense por Bluetooth.
//
// Formato del reporte obtenido de la investigación de SAxense (Egor Vorontsov / Sdore):
//   https://github.com/egormanga/SAxense  —  https://apps.sdore.me/SAxense
// Esta es una reimplementación propia (sin bitfields, independiente del compilador),
// verificada byte a byte contra el layout original en tests/test_packet.cpp.
//
// Layout del reporte (142 bytes en total, incluido el Report ID):
//   [0]        0x32                     Report ID
//   [1]        tag(4 bits bajos) | seq(4 bits altos)   (SAxense usa 0)
//   [2..10]    sub-paquete 0x11 (control): cabecera 0x91, largo 7,
//              datos FE 00 00 00 00 FF <contador>
//   [11..76]   sub-paquete 0x12 (muestras): cabecera 0x92, largo 64,
//              64 bytes = 32 frames estéreo s8 (L,R) a 3000 Hz
//   [77..137]  ceros
//   [138..141] CRC32 (little-endian) sobre los bytes [0..137] con semilla BT 0xA2
//
// Cada reporte lleva 32 frames => hay que enviar uno cada 32/3000 s = 10,667 ms.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace ds5 {

constexpr uint8_t  kHapticsReportId   = 0x32;
constexpr size_t   kHapticsReportSize = 142;   // incluye Report ID y CRC
constexpr size_t   kSampleBytes       = 64;    // bytes de muestras por reporte
constexpr unsigned kSampleRate        = 3000;  // Hz
constexpr unsigned kChannels          = 2;
constexpr size_t   kFramesPerReport   = kSampleBytes / kChannels;  // 32
constexpr double   kReportPeriodSec   = double(kFramesPerReport) / kSampleRate;

constexpr size_t kCtrlPacketOffset   = 2;
constexpr size_t kCounterOffset      = kCtrlPacketOffset + 2 + 6;   // 10
constexpr size_t kSamplePacketOffset = kCtrlPacketOffset + 2 + 7;   // 11
constexpr size_t kSampleDataOffset   = kSamplePacketOffset + 2;     // 13
constexpr size_t kCrcOffset          = kHapticsReportSize - 4;      // 138

// CRC32 reflejado (polinomio 0xEDB88320) como lo usa el DualSense por BT:
// estado inicial = CRC tras procesar el byte semilla 0xA2 (salida de reportes).
inline uint32_t crc32_bt(const uint8_t* data, size_t size, uint8_t seed = 0xA2) {
    uint32_t crc = 0xFFFFFFFFu;
    auto step = [&crc](uint8_t b) {
        crc ^= b;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    };
    step(seed);
    while (size--) step(*data++);
    return ~crc;
}

class HapticsReport {
public:
    HapticsReport() { reset(); }

    void reset() {
        std::memset(buf_, 0, sizeof(buf_));
        buf_[0] = kHapticsReportId;
        buf_[1] = 0x00;  // tag=0, seq=0

        // Sub-paquete 0x11: pid=0x11, sized=1 -> 0x91
        uint8_t* p = buf_ + kCtrlPacketOffset;
        p[0] = 0x11 | 0x80;
        p[1] = 7;
        const uint8_t ctrl[7] = {0xFE, 0, 0, 0, 0, 0xFF, 0};
        std::memcpy(p + 2, ctrl, sizeof(ctrl));

        // Sub-paquete 0x12: pid=0x12, sized=1 -> 0x92
        uint8_t* s = buf_ + kSamplePacketOffset;
        s[0] = 0x12 | 0x80;
        s[1] = static_cast<uint8_t>(kSampleBytes);

        updateCrc();
    }

    // Carga 64 bytes s8 intercalados (L,R,L,R...), avanza el contador y recalcula el CRC.
    // Experimental: 'n' < 64 manda menos muestras por reporte (reportes más seguidos).
    void setSamples(const int8_t* samples, size_t n = kSampleBytes) {
        if (n > kSampleBytes) n = kSampleBytes;
        n &= ~size_t(1);
        buf_[kSamplePacketOffset + 1] = static_cast<uint8_t>(n);
        std::memset(buf_ + kSampleDataOffset, 0, kSampleBytes);
        std::memcpy(buf_ + kSampleDataOffset, samples, n);
        buf_[kCounterOffset] = static_cast<uint8_t>(buf_[kCounterOffset] + 1);
        updateCrc();
    }

    // Opcional (experimental): número de secuencia en el nibble alto del byte 1.
    void setSeq(uint8_t seq) {
        buf_[1] = static_cast<uint8_t>((buf_[1] & 0x0F) | ((seq & 0x0F) << 4));
        updateCrc();
    }

    const uint8_t* data() const { return buf_; }
    static constexpr size_t size() { return kHapticsReportSize; }

private:
    void updateCrc() {
        uint32_t crc = crc32_bt(buf_, kCrcOffset);
        buf_[kCrcOffset + 0] = static_cast<uint8_t>(crc);
        buf_[kCrcOffset + 1] = static_cast<uint8_t>(crc >> 8);
        buf_[kCrcOffset + 2] = static_cast<uint8_t>(crc >> 16);
        buf_[kCrcOffset + 3] = static_cast<uint8_t>(crc >> 24);
    }

    uint8_t buf_[kHapticsReportSize];
};

// Reporte de salida principal por Bluetooth (0x31, 78 bytes), layout del driver
// hid-playstation de Linux. Se usa para fijar el color de la barra de luz y así
// confirmar que la escritura por BT funciona antes de probar la háptica.
constexpr size_t kMainReportSize = 78;

inline void buildLightbarReport(uint8_t out[kMainReportSize], uint8_t seq,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t playerLeds) {
    std::memset(out, 0, kMainReportSize);
    out[0] = 0x31;
    out[1] = static_cast<uint8_t>((seq & 0x0F) << 4);  // seq_tag
    out[2] = 0x10;                                     // tag
    uint8_t* c = out + 3;                              // bloque "common" (47 bytes)
    c[1]  = (1u << 2) | (1u << 4);   // valid_flag1: barra de luz + LEDs de jugador
    c[38] = (1u << 1);               // valid_flag2: control de setup de la barra
    c[41] = (1u << 1);               // lightbar_setup: apagar animación azul de inicio
    c[43] = playerLeds;
    c[44] = r; c[45] = g; c[46] = b;
    uint32_t crc = crc32_bt(out, kMainReportSize - 4);
    out[74] = uint8_t(crc); out[75] = uint8_t(crc >> 8);
    out[76] = uint8_t(crc >> 16); out[77] = uint8_t(crc >> 24);
}

}  // namespace ds5

// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// gestures.h — Gestos del mando que usa ds5haptics (portable):
//   - Toque del botón PS (para mostrar u ocultar las estadísticas).
//   - Agitar el mando con fuerza (para cambiar de modo).
//
// Trabaja sobre el reporte de entrada en formato USB (0x01, 64 bytes):
//   byte 10, bit 0 = botón PS; bytes 22..27 = acelerómetro X/Y/Z (int16, ~8192 por g).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ds5 {

class Gestures {
public:
    struct Events {
        bool psPressed = false;   // el botón PS se acaba de apretar
        bool shaken = false;      // se agitó el mando
    };

    // Llamar al (re)conectar: durante un rato se ignora el botón PS (con él se prende el mando)
    void reset(uint32_t nowMs) {
        psDown_ = true;
        ignoreUntil_ = nowMs + 1500;
        peaks_ = 0;
        lastPeak_ = 0;
        firstPeak_ = 0;
        lastSign_ = 0;
        cooldownUntil_ = nowMs + 1500;
    }

    Events feed(const uint8_t usb[64], uint32_t nowMs) {
        Events ev;
        // ---- botón PS: flanco de subida ----
        bool ps = (usb[10] & 0x01) != 0;
        if (ps && !psDown_ && int32_t(nowMs - ignoreUntil_) >= 0) ev.psPressed = true;
        psDown_ = ps;

        // ---- sacudida: varios picos de aceleración fuertes, en sentidos alternados, en poco tiempo ----
        float ax = s16(usb + 22) / kPerG, ay = s16(usb + 24) / kPerG, az = s16(usb + 26) / kPerG;
        float mag = std::sqrt(ax * ax + ay * ay + az * az);
        float dev = mag - 1.0f;   // en reposo la gravedad da ~1 g
        if (firstPeak_ && nowMs - firstPeak_ > kWindowMs) { peaks_ = 0; firstPeak_ = 0; lastSign_ = 0; }
        if (int32_t(nowMs - cooldownUntil_) >= 0 && std::fabs(dev) > kPeakG && nowMs - lastPeak_ >= kMinGapMs) {
            // Sentido del pico: el eje con más aceleración (al agitar, va y viene)
            float big = ax;
            if (std::fabs(ay) > std::fabs(big)) big = ay;
            if (std::fabs(az) > std::fabs(big)) big = az;
            int sign = big >= 0 ? 1 : -1;
            if (sign != lastSign_) {
                if (!peaks_) firstPeak_ = nowMs;
                ++peaks_;
                lastSign_ = sign;
                lastPeak_ = nowMs;
                if (peaks_ >= kPeaksNeeded) {
                    ev.shaken = true;
                    peaks_ = 0;
                    firstPeak_ = 0;
                    lastSign_ = 0;
                    cooldownUntil_ = nowMs + kCooldownMs;
                }
            }
        }
        return ev;
    }

    static constexpr float kPerG = 8192.0f;
    static constexpr float kPeakG = 1.6f;        // pico: 1,6 g por encima o por debajo de la gravedad
    static constexpr uint32_t kMinGapMs = 60;    // entre picos
    static constexpr uint32_t kWindowMs = 1200;  // todos los picos dentro de 1,2 s
    static constexpr int kPeaksNeeded = 5;       // ida y vuelta, ida y vuelta, ida
    static constexpr uint32_t kCooldownMs = 2500;

private:
    static int16_t s16(const uint8_t* p) { return int16_t(uint16_t(p[0]) | uint16_t(p[1]) << 8); }

    bool psDown_ = true;
    uint32_t ignoreUntil_ = 0;
    int peaks_ = 0;
    uint32_t lastPeak_ = 0, firstPeak_ = 0, cooldownUntil_ = 0;
    int lastSign_ = 0;
};

}  // namespace ds5

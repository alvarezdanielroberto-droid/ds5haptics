// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// ambient_color.h — Color de la barra de luz según lo que se ve en pantalla (portable).
//
//   dominantColor(): de una imagen chica de la pantalla (BGRA), el color que más "pesa": se
//     agrupan los píxeles por tono (24 grupos) y cada píxel cuenta más cuanto más saturado y
//     brillante es (los grises, el negro y el texto blanco casi no cuentan). Si la pantalla es
//     casi toda gris o negra, sale un blanco tenue.
//   AmbientLight: suaviza los cambios (~150 ms) y, con "ritmo", hace latir el brillo con la
//     intensidad de la háptica / el sonido del juego.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ds5 {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
};

struct ScreenColor {
    float r = 1, g = 1, b = 1;   // color a brillo pleno (el canal mayor = 1)
    float brightness = 0.3f;     // 0..1, según qué tan clara es la pantalla
    float colorful = 0;          // 0..1: qué parte de la pantalla tiene color (diagnóstico)
};

// px: píxeles de 4 bytes (BGRA si bgra, si no RGBA), 'stride' bytes por fila
inline ScreenColor dominantColor(const uint8_t* px, int w, int h, int stride, bool bgra = true) {
    constexpr int kBins = 24;
    double bw[kBins] = {}, br[kBins] = {}, bg[kBins] = {}, bb[kBins] = {};
    double luma = 0;
    long n = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = px + size_t(y) * size_t(stride);
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + x * 4;
            const float r = (bgra ? p[2] : p[0]) / 255.0f, g = p[1] / 255.0f, b = (bgra ? p[0] : p[2]) / 255.0f;
            const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
            luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
            ++n;
            if (mx < 0.06f) continue;   // negro
            const float s = (mx - mn) / mx;
            const double wgt = double(s) * s * mx;   // color fuerte y brillante pesa mucho más
            if (wgt <= 0) continue;
            float hue;   // 0..6
            const float d = mx - mn;
            if (mx == r) hue = std::fmod((g - b) / d + 6.0f, 6.0f);
            else if (mx == g) hue = (b - r) / d + 2.0f;
            else hue = (r - g) / d + 4.0f;
            int k = int(hue / 6.0f * kBins);
            if (k < 0) k = 0;
            if (k >= kBins) k = kBins - 1;
            bw[k] += wgt;
            br[k] += wgt * r;
            bg[k] += wgt * g;
            bb[k] += wgt * b;
        }
    }
    ScreenColor out;
    if (n == 0) return out;
    luma /= double(n);
    // Grupo de tonos dominante (con sus vecinos)
    int best = 0;
    double bestW = -1, total = 0;
    for (int k = 0; k < kBins; ++k) {
        total += bw[k];
        const double s3 = bw[(k + kBins - 1) % kBins] + bw[k] + bw[(k + 1) % kBins];
        if (s3 > bestW) { bestW = s3, best = k; }
    }
    out.colorful = float(std::min(1.0, total / double(n) * 4.0));
    // Brillo: pantallas oscuras, luz tenue; claras, luz fuerte
    out.brightness = float(std::clamp(0.15 + 1.6 * luma, 0.15, 1.0));
    if (total / double(n) < 0.01) {   // casi sin color: blanco tenue
        out.r = out.g = out.b = 1.0f;
        out.brightness = std::min(out.brightness, 0.35f);
        return out;
    }
    double r = 0, g = 0, b = 0, wsum = 0;
    for (int d = -1; d <= 1; ++d) {
        const int k = (best + d + kBins) % kBins;
        r += br[k], g += bg[k], b += bb[k], wsum += bw[k];
    }
    r /= wsum, g /= wsum, b /= wsum;
    // A brillo pleno y más saturado (en un LED los colores lavados se ven blancos)
    const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    if (mx <= 0) return out;
    auto sat = [&](double v) { return std::max(0.0, (v - 0.6 * mn) / (mx - 0.6 * mn)); };
    out.r = float(sat(r)), out.g = float(sat(g)), out.b = float(sat(b));
    return out;
}

class AmbientLight {
public:
    // Ritmo: el brillo late con 'pulse' (0..1, la intensidad del momento)
    void setRhythm(bool on) { rhythm_ = on; }

    // Avanza 'dtMs' hacia el color de la pantalla; devuelve el color a mandar
    Rgb update(const ScreenColor& target, float pulse, float dtMs) {
        const float a = 1.0f - std::exp(-dtMs / 150.0f);   // color: ~150 ms
        r_ += (target.r * target.brightness - r_) * a;
        g_ += (target.g * target.brightness - g_) * a;
        b_ += (target.b * target.brightness - b_) * a;
        // Latido: sube enseguida con un golpe y baja en ~250 ms
        pulse = std::clamp(pulse, 0.0f, 1.0f);
        beat_ = pulse > beat_ ? pulse : beat_ * std::exp(-dtMs / 250.0f);
        const float k = rhythm_ ? 0.45f + 0.55f * beat_ : 1.0f;
        Rgb o;
        o.r = uint8_t(std::lround(std::clamp(r_ * k, 0.0f, 1.0f) * 255.0f));
        o.g = uint8_t(std::lround(std::clamp(g_ * k, 0.0f, 1.0f) * 255.0f));
        o.b = uint8_t(std::lround(std::clamp(b_ * k, 0.0f, 1.0f) * 255.0f));
        return o;
    }

    // ¿Vale la pena mandarlo? (cambió lo suficiente desde el último envío)
    bool changed(const Rgb& c) {
        const int d = std::abs(c.r - last_.r) + std::abs(c.g - last_.g) + std::abs(c.b - last_.b);
        if (d < 4 && sent_) return false;
        last_ = c;
        sent_ = true;
        return true;
    }

    void reset() { r_ = g_ = b_ = 0, beat_ = 0, sent_ = false; }

private:
    float r_ = 0, g_ = 0, b_ = 0, beat_ = 0;
    bool rhythm_ = true, sent_ = false;
    Rgb last_;
};

}  // namespace ds5

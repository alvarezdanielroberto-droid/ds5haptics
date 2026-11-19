// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// sim_fx.h — Háptica y parlante "simulados" para juegos sin soporte de DualSense (modo
// Xbox/PS4). Portable.
//
// Esos juegos no mandan háptica ni sonido para el mando, así que se inventan a partir del
// sonido del juego:
//   - Todo el sonido, filtrado a graves, se vuelve una háptica suave (pisadas, lluvia,
//     viento, motores). Se normaliza contra el volumen reciente del juego, así no depende
//     del volumen de Windows.
//   - Un golpe fuerte y repentino (explosión, choque de espadas, disparo) se detecta porque
//     el volumen sube de golpe muy por encima del promedio: la háptica sube a fondo y, si
//     está activado, ese sonido sale también por el parlante del mando (~0,3 s).
//   - La vibración clásica que pide el juego (motores grande y chico) se suma como ondas de
//     40 y 130 Hz, para que no se pierda mientras hay háptica.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ds5 {

class SimFx {
public:
    struct Config {
        bool haptics = true;
        bool speaker = true;
        int sensitivity = 5;    // 1..10
        float hapticGain = 1.0f;
        float speakerGain = 1.0f;
    };

    void configure(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }

    void setRumble(uint8_t strong, uint8_t weak) {
        rumbleStrong_.store(strong / 255.0f, std::memory_order_relaxed);
        rumbleWeak_.store(weak / 255.0f, std::memory_order_relaxed);
    }

    unsigned impacts() const { return impacts_.load(std::memory_order_relaxed); }
    float level() const { return levelNow_.load(std::memory_order_relaxed); }

    // Entrada: estéreo intercalado a 'rate'. Salida: háptica estéreo a la misma tasa (se
    // reemplaza el contenido de 'hap') y parlante estéreo a 48 kHz (se agrega a 'spk48').
    void process(const float* in, size_t frames, unsigned rate, std::vector<float>& hap, std::vector<float>& spk48) {
        if (rate != rate_) setup(rate);
        hap.resize(frames * 2);
        const float sens = float(std::clamp(cfg_.sensitivity, 1, 10));
        const float kTrans = 4.3f - 0.27f * sens;          // cuánto tiene que superar al promedio (1 -> 4,0x, 10 -> 1,6x)
        const float kLoud = 0.62f - 0.035f * sens;         // y qué tan cerca del máximo reciente
        const float rs = rumbleStrong_.load(std::memory_order_relaxed);
        const float rw = rumbleWeak_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; ++i) {
            const float l = in[i * 2], r = in[i * 2 + 1];
            const float m = 0.5f * (l + r);
            const float p = m * m;
            // Energía: rápida (~10 ms, golpes), lenta (~0,7 s, promedio). Se compara su raíz
            // (nivel RMS), que en un sonido parejo como la lluvia casi no fluctúa.
            pFast_ += (p - pFast_) * (p > pFast_ ? atkFast_ : relFast_);
            pSlow_ += (pFast_ - pSlow_) * slow_;
            eFast_ = std::sqrt(pFast_);
            eSlow_ = std::sqrt(pSlow_);
            ref_ = eFast_ > ref_ ? eFast_ : std::max(ref_ * relRef_, kRefMin);
            const bool loudEnough = eFast_ > kFloor && eFast_ > kLoud * ref_;
            if (warmup_ > 0) --warmup_;   // al empezar a capturar no hay historial: no contar golpes
            else if (loudEnough && eFast_ > kTrans * std::max(eSlow_, kFloor)) {
                if (hold_ <= 0) impacts_.fetch_add(1, std::memory_order_relaxed);
                hold_ = holdLen_;
            }
            if (hold_ > 0) --hold_;
            const float gTarget = hold_ > 0 ? 1.0f : 0.0f;
            gate_ += (gTarget - gate_) * (gTarget > gate_ ? gateAtk_ : gateRel_);

            const float norm = 1.0f / ref_;
            float hl = 0, hr = 0;
            if (cfg_.haptics && eFast_ > kFloor) {
                const float amount = (kAmbient + (kDrive - kAmbient) * gate_) * cfg_.hapticGain;
                hl = lpL2_.run(lpL_.run(l)) * norm * amount;
                hr = lpR2_.run(lpR_.run(r)) * norm * amount;
            } else {
                lpL2_.run(lpL_.run(l));
                lpR2_.run(lpR_.run(r));
            }
            // Vibración clásica del juego
            if (rs > 0.01f || rw > 0.01f) {
                ph1_ += w1_;
                ph2_ += w2_;
                if (ph1_ > kTwoPi) ph1_ -= kTwoPi;
                if (ph2_ > kTwoPi) ph2_ -= kTwoPi;
                const float v = 0.8f * rs * std::sin(ph1_) + 0.5f * rw * std::sin(ph2_);
                hl += v;
                hr += v;
            }
            hap[i * 2] = std::tanh(hl);
            hap[i * 2 + 1] = std::tanh(hr);

            // Parlante: el sonido del juego (con 5 ms de atraso, para no cortar el principio
            // del golpe) mientras la compuerta está abierta
            dl_[dpos_] = l;
            dr_[dpos_] = r;
            dpos_ = (dpos_ + 1) % dl_.size();
            float sl = 0, sr = 0;
            if (cfg_.speaker && gate_ > 0.001f) {
                const float g = gate_ * norm * 0.8f;
                sl = soft(dl_[dpos_] * g, cfg_.speakerGain);
                sr = soft(dr_[dpos_] * g, cfg_.speakerGain);
            }
            pushSpeaker(sl, sr, spk48);
        }
        levelNow_.store(std::min(1.0f, eFast_ / ref_), std::memory_order_relaxed);
    }

private:
    struct Lp {   // pasa-bajos de 1 polo
        float a = 0, z = 0;
        float run(float x) { z += a * (x - z); return z; }
    };

    void setup(unsigned rate) {
        rate_ = rate;
        const float fs = float(rate);
        auto coef = [fs](float ms) { return 1.0f - std::exp(-1.0f / (ms * 0.001f * fs)); };
        atkFast_ = coef(4.0f);
        relFast_ = coef(40.0f);
        slow_ = coef(700.0f);
        relRef_ = std::exp(-1.0f / (4.0f * fs));
        gateAtk_ = coef(3.0f);
        gateRel_ = coef(120.0f);
        holdLen_ = int(0.30f * fs);
        warmup_ = int(1.5f * fs);
        const float lpa = 1.0f - std::exp(-kTwoPi * 500.0f / fs);   // ~500 Hz, dos etapas
        lpL_.a = lpR_.a = lpL2_.a = lpR2_.a = lpa;
        w1_ = kTwoPi * 40.0f / fs;
        w2_ = kTwoPi * 130.0f / fs;
        dl_.assign(size_t(std::max(1.0f, 0.005f * fs)), 0.0f);
        dr_.assign(dl_.size(), 0.0f);
        dpos_ = 0;
        step_ = double(rate) / 48000.0;
        rpos_ = 0;
        havePrev_ = false;
    }

    static float soft(float x, float gain) {
        x *= gain;
        return std::tanh(x);
    }

    // Remuestreo lineal a 48 kHz para el parlante
    void pushSpeaker(float l, float r, std::vector<float>& out) {
        if (rate_ == 48000) { out.push_back(l); out.push_back(r); return; }
        if (!havePrev_) { pl_ = l; pr_ = r; havePrev_ = true; return; }
        // Emitir las muestras de salida que caen entre la anterior (t=0) y esta (t=1)
        while (rpos_ < 1.0) {
            const float t = float(rpos_);
            out.push_back(pl_ + (l - pl_) * t);
            out.push_back(pr_ + (r - pr_) * t);
            rpos_ += step_;
        }
        rpos_ -= 1.0;
        pl_ = l;
        pr_ = r;
    }

    static constexpr float kTwoPi = 6.28318530718f;
    static constexpr float kFloor = 0.0007f;   // nivel RMS por debajo del cual es silencio
    static constexpr float kRefMin = 0.005f;   // amplificación máxima ~200x
    static constexpr float kAmbient = 0.5f;    // háptica para lo suave
    static constexpr float kDrive = 1.8f;      // háptica en un golpe (satura suave: a fondo)

    Config cfg_;
    unsigned rate_ = 0;
    float atkFast_ = 0, relFast_ = 0, slow_ = 0, relRef_ = 0, gateAtk_ = 0, gateRel_ = 0;
    float pFast_ = 0, pSlow_ = 0, eFast_ = 0, eSlow_ = 0, ref_ = kRefMin, gate_ = 0;
    int hold_ = 0, holdLen_ = 0, warmup_ = 0;
    Lp lpL_, lpR_, lpL2_, lpR2_;
    float ph1_ = 0, ph2_ = 0, w1_ = 0, w2_ = 0;
    std::vector<float> dl_{0.0f}, dr_{0.0f};
    size_t dpos_ = 0;
    double step_ = 1.0, rpos_ = 0;
    float pl_ = 0, pr_ = 0;
    bool havePrev_ = false;
    std::atomic<float> rumbleStrong_{0}, rumbleWeak_{0}, levelNow_{0};
    std::atomic<unsigned> impacts_{0};
};

}  // namespace ds5

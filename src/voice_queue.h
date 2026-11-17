// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// voice_queue.h — Cola de avisos hablados (audio mono de 48 kHz) que se mezclan con el
// sonido del juego antes de comprimirlo para el parlante del mando. Portable.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

namespace ds5 {

class VoiceQueue {
public:
    // Agrega una frase (con 60 ms de silencio antes, para que el mando "despierte" el parlante).
    // La voz de Windows sale bajita: se normaliza al máximo y 'loudness' (1 a 10) la comprime
    // con un limitador suave para que suene más fuerte en el parlante chiquito del mando.
    void push(const std::vector<float>& mono, float loudness = 1.0f) {
        float peak = 0.0f;
        for (float s : mono) peak = std::max(peak, std::fabs(s));
        const float norm = peak > 1e-4f ? 1.0f / peak : 1.0f;
        const float g = std::clamp(loudness, 1.0f, 10.0f);
        const float out = g > 1.0f ? 1.0f / std::tanh(g) : 1.0f;
        std::lock_guard<std::mutex> lk(mu_);
        buf_.insert(buf_.end(), 48000 * 60 / 1000, 0.0f);
        for (float s : mono) {
            float x = s * norm;
            if (g > 1.0f) x = std::tanh(x * g) * out;
            buf_.push_back(std::clamp(x * 0.98f, -1.0f, 1.0f));
        }
        pending_ = buf_.size();
    }

    // ¿Queda voz por reproducir?
    bool active() const { return pending_.load(std::memory_order_relaxed) > 0; }

    // Suma 'frames' muestras de voz (en los dos canales) al audio estéreo intercalado.
    // Mientras habla, el sonido del juego baja a la mitad para que se entienda.
    void mixInto(float* stereo, size_t frames) {
        std::lock_guard<std::mutex> lk(mu_);
        if (buf_.empty()) return;
        size_t n = std::min(frames, buf_.size());
        for (size_t i = 0; i < frames; ++i) {
            float v = i < n ? buf_[i] : 0.0f;
            for (int c = 0; c < 2; ++c) {
                float& s = stereo[i * 2 + c];
                s = std::clamp(s * 0.5f + v, -1.0f, 1.0f);
            }
        }
        buf_.erase(buf_.begin(), buf_.begin() + long(n));
        pending_ = buf_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        buf_.clear();
        pending_ = 0;
    }

private:
    std::mutex mu_;
    std::deque<float> buf_;
    std::atomic<size_t> pending_{0};
};

}  // namespace ds5

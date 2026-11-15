// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// speaker_bt.h — Parlante del DualSense por Bluetooth (reporte 0x36).
//
// Por Bluetooth, el audio del parlante viaja junto con la háptica en el reporte 0x36,
// comprimido en Opus. Formato según DS5Dongle (https://deepwiki.com/awalol/DS5Dongle):
//
//   [0]        0x36                     Report ID
//   [1]        secuencia << 4
//   [2..10]    sub-paquete 0x11 (control): 91 07 FE 00 00 00 00 FF <contador>
//   [11..75]   sub-paquete 0x10 (estado):  90 3F + 63 bytes (todo en cero = no cambia nada;
//              los gatillos y las luces del juego siguen yendo por el reporte 0x31)
//   [76..141]  sub-paquete 0x12 (háptica):  92 40 + 64 bytes (32 frames estéreo s8, 3 kHz)
//   [142..343] sub-paquete 0x13 (parlante): 93 C8 + 200 bytes de Opus
//   [344..393] ceros
//   [394..397] CRC32 sobre [0..393] (semilla Bluetooth 0xA2)
//
// Cada reporte cubre 10,667 ms (32 frames de háptica = 512 muestras a 48 kHz), pero el
// mando reproduce cada paquete Opus como 480 muestras: hay que comprimir 512 -> 480,
// igual que DS5Dongle, o el audio sonaría desafinado.
#pragma once
#include "ds5_bt_haptics.h"
#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace ds5 {

constexpr uint8_t kAudioReportId = 0x36;
constexpr size_t kAudioReportSize = 398;
constexpr size_t kOpusBytes = 200;
constexpr size_t kSpeakerInFrames = 512;   // 10,667 ms a 48 kHz
constexpr size_t kOpusFrames = 480;        // lo que el mando reproduce por paquete

// Comprime 512 frames estéreo float (48 kHz) en un paquete Opus CBR de exactamente 200 bytes.
class SpeakerEncoder {
public:
    ~SpeakerEncoder() { if (enc_) opus_encoder_destroy(enc_); }

    bool init() {
        int err = 0;
        enc_ = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (err != OPUS_OK || !enc_) { enc_ = nullptr; return false; }
        opus_encoder_ctl(enc_, OPUS_SET_BITRATE(int(kOpusBytes * 8 * 100)));  // 160 kbps
        opus_encoder_ctl(enc_, OPUS_SET_VBR(0));                              // tasa fija
        opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(8));
        return true;
    }

    // in: 512 frames estéreo intercalados. out: 200 bytes. Devuelve false si falló.
    bool encode(const float* in, uint8_t out[kOpusBytes]) {
        if (!enc_) return false;
        // 512 -> 480 por interpolación lineal (el mando lo reproduce más lento: ~45 kHz)
        float rs[kOpusFrames * 2];
        const double step = double(kSpeakerInFrames - 1) / double(kOpusFrames - 1);
        for (size_t i = 0; i < kOpusFrames; ++i) {
            double x = i * step;
            size_t k = size_t(x);
            double f = x - double(k);
            size_t k2 = std::min(k + 1, kSpeakerInFrames - 1);
            for (int c = 0; c < 2; ++c)
                rs[i * 2 + c] = float(in[k * 2 + c] * (1.0 - f) + in[k2 * 2 + c] * f);
        }
        std::memset(out, 0, kOpusBytes);
        int n = opus_encode_float(enc_, rs, int(kOpusFrames), out, int(kOpusBytes));
        lastSize_ = n;
        return n == int(kOpusBytes);
    }

    int lastSize() const { return lastSize_; }

private:
    OpusEncoder* enc_ = nullptr;
    int lastSize_ = 0;
};

// Arma el reporte 0x36 con háptica + parlante.
class AudioHapticsReport {
public:
    AudioHapticsReport() {
        std::memset(buf_, 0, sizeof(buf_));
        buf_[0] = kAudioReportId;
        uint8_t* p = buf_ + 2;                  // 0x11 control
        p[0] = 0x11 | 0x80; p[1] = 7;
        const uint8_t ctrl[7] = {0xFE, 0, 0, 0, 0, 0xFF, 0};
        std::memcpy(p + 2, ctrl, 7);
        buf_[11] = 0x10 | 0x80; buf_[12] = 63;  // 0x10 estado (en cero)
        buf_[76] = 0x12 | 0x80; buf_[77] = 64;  // 0x12 háptica
        buf_[142] = 0x13 | 0x80; buf_[143] = uint8_t(kOpusBytes);  // 0x13 parlante
    }

    void build(const int8_t haptics[64], const uint8_t opus[kOpusBytes]) {
        buf_[1] = uint8_t((seq_++ & 0x0F) << 4);
        buf_[10] = uint8_t(buf_[10] + 1);
        std::memcpy(buf_ + 78, haptics, 64);
        std::memcpy(buf_ + 144, opus, kOpusBytes);
        uint32_t crc = crc32_bt(buf_, kAudioReportSize - 4);
        buf_[394] = uint8_t(crc); buf_[395] = uint8_t(crc >> 8);
        buf_[396] = uint8_t(crc >> 16); buf_[397] = uint8_t(crc >> 24);
    }

    const uint8_t* data() const { return buf_; }
    static constexpr size_t size() { return kAudioReportSize; }

private:
    uint8_t buf_[kAudioReportSize];
    uint8_t seq_ = 0;
};

// Cola del audio del parlante (canales 1 y 2) entre el hilo de USB y el de envío.
class SpeakerFifo {
public:
    void push(const float* stereo, size_t frames) {
        std::lock_guard<std::mutex> lk(mu_);
        buf_.insert(buf_.end(), stereo, stereo + frames * 2);
        // Acotar la demora: si se acumula más de ~43 ms, descartar lo viejo
        const size_t maxSamples = kSpeakerInFrames * 4 * 2;
        if (buf_.size() > maxSamples) {
            buf_.erase(buf_.begin(), buf_.begin() + (buf_.size() - kSpeakerInFrames * 2 * 2));
            ++drops_;
        }
    }
    // Saca 512 frames; si no hay suficientes, completa con silencio.
    bool pop(float* out) {
        std::lock_guard<std::mutex> lk(mu_);
        const size_t need = kSpeakerInFrames * 2;
        size_t n = std::min(need, buf_.size());
        std::copy(buf_.begin(), buf_.begin() + n, out);
        std::fill(out + n, out + need, 0.0f);
        buf_.erase(buf_.begin(), buf_.begin() + n);
        return n == need;
    }
    void clear() { std::lock_guard<std::mutex> lk(mu_); buf_.clear(); }
    unsigned drops() const { return drops_; }

private:
    std::mutex mu_;
    std::deque<float> buf_;
    unsigned drops_ = 0;
};

}  // namespace ds5

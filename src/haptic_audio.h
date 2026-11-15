// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// haptic_audio.h — Carga de audio y conversión al formato de háptica del DualSense
// (estéreo, int8 con signo, 3000 Hz). Portable, sin dependencias.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace ds5 {

struct AudioBuffer {
    unsigned sampleRate = 0;
    unsigned channels   = 0;
    std::vector<float> samples;  // intercaladas, rango [-1, 1]
    size_t frames() const { return channels ? samples.size() / channels : 0; }
};

namespace detail {
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}  // namespace detail

// Lee WAV PCM 8/16/24/32 bits o float 32/64 bits (incluye WAVE_FORMAT_EXTENSIBLE).
inline bool loadWav(const std::string& path, AudioBuffer& out, std::string& err) {
    using namespace detail;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "no se pudo abrir el archivo"; return false; }
    std::vector<uint8_t> d;
    uint8_t tmp[65536];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) d.insert(d.end(), tmp, tmp + n);
    std::fclose(f);

    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
        err = "no es un archivo WAV (RIFF/WAVE)"; return false;
    }
    uint16_t fmtTag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmLen = 0;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const uint8_t* c = d.data() + pos;
        uint32_t len = rd32(c + 4);
        size_t body = pos + 8;
        size_t avail = std::min<size_t>(len, d.size() - body);
        if (!std::memcmp(c, "fmt ", 4) && avail >= 16) {
            fmtTag = rd16(c + 8); ch = rd16(c + 10); rate = rd32(c + 12); bits = rd16(c + 22);
            if (fmtTag == 0xFFFE && avail >= 26) fmtTag = rd16(c + 8 + 24);  // SubFormat
        } else if (!std::memcmp(c, "data", 4)) {
            pcm = d.data() + body; pcmLen = avail;
        }
        pos = body + len + (len & 1);
    }
    if (!pcm || !ch || !rate) { err = "WAV sin bloques fmt/data válidos"; return false; }
    if (!((fmtTag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (fmtTag == 3 && (bits == 32 || bits == 64)))) {
        err = "formato WAV no soportado (usá PCM 8/16/24/32 o float)"; return false;
    }
    size_t bps = bits / 8;
    size_t count = pcmLen / bps;
    count -= count % ch;
    out.sampleRate = rate; out.channels = ch;
    out.samples.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = pcm + i * bps;
        float v = 0;
        if (fmtTag == 3) {
            if (bits == 32) { float x; std::memcpy(&x, p, 4); v = x; }
            else { double x; std::memcpy(&x, p, 8); v = float(x); }
        } else switch (bits) {
            case 8:  v = (int(p[0]) - 128) / 128.0f; break;
            case 16: v = int16_t(rd16(p)) / 32768.0f; break;
            case 24: v = (int32_t(uint32_t(p[0] << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8) / 8388608.0f; break;
            case 32: v = int32_t(rd32(p)) / 2147483648.0f; break;
        }
        out.samples[i] = v;
    }
    return true;
}

// Archivo crudo s8 estéreo a 3000 Hz (lo mismo que genera:
//   ffmpeg -i in.mp3 -ac 2 -ar 3000 -f s8 salida.raw)
inline bool loadRawS8(const std::string& path, std::vector<int8_t>& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "no se pudo abrir el archivo"; return false; }
    int8_t tmp[65536];
    size_t n;
    out.clear();
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) out.insert(out.end(), tmp, tmp + n);
    std::fclose(f);
    if (out.size() % 2) out.pop_back();
    return true;
}

// Elige dos canales: con 4 canales usa 3 y 4 (la disposición USB del DualSense:
// 1-2 parlante/auriculares, 3-4 actuadores). Con 1 canal lo duplica.
inline void pickStereo(const AudioBuffer& in, int chL, int chR, std::vector<float>& out) {
    size_t fr = in.frames();
    out.resize(fr * 2);
    for (size_t i = 0; i < fr; ++i) {
        const float* f = &in.samples[i * in.channels];
        out[i * 2 + 0] = f[std::min<int>(chL, in.channels - 1)];
        out[i * 2 + 1] = f[std::min<int>(chR, in.channels - 1)];
    }
}

// Remuestreo con sinc con ventana (Blackman) y filtro anti-aliasing incluido.
inline std::vector<float> resampleStereo(const std::vector<float>& in, unsigned srcRate, unsigned dstRate) {
    size_t inFrames = in.size() / 2;
    if (srcRate == dstRate || inFrames == 0) return in;
    const double ratio = double(dstRate) / srcRate;
    const double fc = 0.45 * std::min(1.0, ratio);   // corte en ciclos/muestra de origen
    const double halfWidth = 8.0 / (2.0 * fc);        // 8 cruces por cero por lado
    size_t outFrames = size_t(std::floor(inFrames * ratio));
    std::vector<float> out(outFrames * 2);
    const double pi = 3.14159265358979323846;
    for (size_t o = 0; o < outFrames; ++o) {
        double t = o / ratio;
        long lo = long(std::ceil(t - halfWidth)), hi = long(std::floor(t + halfWidth));
        double accL = 0, accR = 0, wsum = 0;
        for (long n = std::max(0L, lo); n <= hi && n < long(inFrames); ++n) {
            double x = n - t;
            double a = 2.0 * pi * fc * x;
            double s = (std::fabs(a) < 1e-9) ? 1.0 : std::sin(a) / a;
            double u = (x / halfWidth + 1.0) * 0.5;   // 0..1 dentro de la ventana
            double w = 0.42 - 0.5 * std::cos(2 * pi * u) + 0.08 * std::cos(4 * pi * u);
            double h = s * w;
            accL += h * in[n * 2]; accR += h * in[n * 2 + 1]; wsum += h;
        }
        if (std::fabs(wsum) > 1e-12) { accL /= wsum; accR /= wsum; }
        out[o * 2] = float(accL); out[o * 2 + 1] = float(accR);
    }
    return out;
}

inline std::vector<int8_t> toS8(const std::vector<float>& in, float gain) {
    std::vector<int8_t> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        float v = std::clamp(in[i] * gain, -1.0f, 1.0f);
        out[i] = int8_t(std::lround(v * 127.0f));
    }
    return out;
}

// --- Señales de prueba, generadas directamente a 3000 Hz estéreo ---
// Los actuadores responden mejor aprox. entre 50 y 400 Hz.
inline std::vector<int8_t> genSine(double hz, double seconds, float amp, int pan /*-1 L,0 ambos,1 R*/) {
    size_t fr = size_t(seconds * 3000);
    std::vector<float> f(fr * 2);
    for (size_t i = 0; i < fr; ++i) {
        float v = amp * float(std::sin(2 * 3.14159265358979 * hz * i / 3000.0));
        f[i * 2]     = pan <= 0 ? v : 0.0f;
        f[i * 2 + 1] = pan >= 0 ? v : 0.0f;
    }
    return toS8(f, 1.0f);
}

inline std::vector<int8_t> genSweep(double f0, double f1, double seconds, float amp) {
    size_t fr = size_t(seconds * 3000);
    std::vector<float> f(fr * 2);
    double phase = 0;
    for (size_t i = 0; i < fr; ++i) {
        double hz = f0 * std::pow(f1 / f0, double(i) / fr);
        phase += 2 * 3.14159265358979 * hz / 3000.0;
        f[i * 2] = f[i * 2 + 1] = amp * float(std::sin(phase));
    }
    return toS8(f, 1.0f);
}

// Golpes cortos alternando izquierda/derecha (para verificar los dos actuadores).
inline std::vector<int8_t> genPulses(double seconds, float amp) {
    size_t fr = size_t(seconds * 3000);
    std::vector<float> f(fr * 2, 0.0f);
    const size_t period = 1500, len = 120;  // cada 0,5 s, 40 ms de pulso
    for (size_t i = 0; i < fr; ++i) {
        size_t k = i % period;
        if (k >= len) continue;
        float env = 1.0f - float(k) / len;
        float v = amp * env * float(std::sin(2 * 3.14159265358979 * 170.0 * k / 3000.0));
        bool left = ((i / period) % 2) == 0;
        f[i * 2 + (left ? 0 : 1)] = v;
    }
    return toS8(f, 1.0f);
}

}  // namespace ds5

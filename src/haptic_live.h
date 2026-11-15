// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// haptic_live.h — Procesamiento en tiempo real del audio capturado hacia háptica.
// Portable (sin dependencias de Windows) para poder probarlo en cualquier plataforma.
//
//   hilo de captura                        hilo de envío (cada 10,667 ms)
//   feed(float estéreo, tasa X) ──► remuestreo a 3 kHz ──► pasa-bajos ──► compuerta
//        ──► ganancia ──► int8 ──► [búfer circular SPSC] ──► pull(64 bytes) ──► reporte 0x32
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace ds5 {

// ---------------------------------------------------------------------------
// Remuestreo por streaming con sinc + ventana Blackman (tabla precalculada).
// Equivalente a resampleStereo() de haptic_audio.h, pero por bloques.
class StreamResampler {
public:
    void configure(unsigned srcRate, unsigned dstRate) {
        src_ = srcRate; dst_ = dstRate;
        ratio_ = double(dstRate) / srcRate;
        step_ = 1.0 / ratio_;
        double fc = 0.45 * std::min(1.0, ratio_);
        hw_ = 8.0 / (2.0 * fc);
        const double pi = 3.14159265358979323846;
        size_t n = size_t(std::ceil(hw_ * kOver)) + 2;
        table_.assign(n, 0.0f);
        for (size_t k = 0; k < n; ++k) {
            double x = double(k) / kOver;
            if (x > hw_) break;
            double a = 2.0 * pi * fc * x;
            double s = (a < 1e-9) ? 1.0 : std::sin(a) / a;
            double u = (x / hw_ + 1.0) * 0.5;
            double w = 0.42 - 0.5 * std::cos(2 * pi * u) + 0.08 * std::cos(4 * pi * u);
            table_[k] = float(s * w);
        }
        buf_.clear();
        base_ = 0;
        t_ = 0.0;
    }

    unsigned srcRate() const { return src_; }
    double latencySeconds() const { return hw_ / src_; }  // muestras "futuras" necesarias

    // Agrega frames estéreo intercalados y llama emit(l, r) por cada frame de salida.
    template <class Emit>
    void push(const float* frames, size_t count, Emit&& emit) {
        buf_.insert(buf_.end(), frames, frames + count * 2);
        const long long avail = base_ + (long long)(buf_.size() / 2);
        while (std::floor(t_ + hw_) < double(avail)) {
            long long lo = std::max<long long>(base_, (long long)std::ceil(t_ - hw_));
            long long hi = std::min<long long>(avail - 1, (long long)std::floor(t_ + hw_));
            double accL = 0, accR = 0, wsum = 0;
            for (long long n = lo; n <= hi; ++n) {
                double h = kernel(std::fabs(double(n) - t_));
                const float* f = &buf_[size_t(n - base_) * 2];
                accL += h * f[0]; accR += h * f[1]; wsum += h;
            }
            if (std::fabs(wsum) > 1e-12) { accL /= wsum; accR /= wsum; }
            emit(float(accL), float(accR));
            t_ += step_;
        }
        // Descartar lo que ya no se usa (en tandas, para no mover memoria siempre)
        long long keepFrom = (long long)std::floor(t_ - hw_) - 1;
        if (keepFrom - base_ > 4096) {
            size_t drop = size_t(keepFrom - base_);
            buf_.erase(buf_.begin(), buf_.begin() + drop * 2);
            base_ += (long long)drop;
        }
    }

private:
    static constexpr int kOver = 64;
    float kernel(double x) const {
        double p = x * kOver;
        size_t i = size_t(p);
        if (i + 1 >= table_.size()) return 0.0f;
        float fr = float(p - double(i));
        return table_[i] + (table_[i + 1] - table_[i]) * fr;
    }

    unsigned src_ = 0, dst_ = 0;
    double ratio_ = 1, step_ = 1, hw_ = 1, t_ = 0;
    long long base_ = 0;
    std::vector<float> buf_;
    std::vector<float> table_;
};

// ---------------------------------------------------------------------------
// Biquad pasa-bajos (Butterworth, fórmulas RBJ).
class Biquad {
public:
    void lowpass(double fs, double fc, double q = 0.70710678) {
        if (fc <= 0 || fc >= fs * 0.49) { b0_ = 1; b1_ = b2_ = a1_ = a2_ = 0; return; }
        double w = 2 * 3.14159265358979323846 * fc / fs;
        double alpha = std::sin(w) / (2 * q), c = std::cos(w);
        double a0 = 1 + alpha;
        b0_ = (1 - c) / 2 / a0; b1_ = (1 - c) / a0; b2_ = b0_;
        a1_ = -2 * c / a0; a2_ = (1 - alpha) / a0;
    }
    float process(float x) {
        double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return float(y);
    }
private:
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

struct LiveConfig {
    float gain       = 2.0f;    // ganancia final
    float lowpassHz  = 300.0f;  // 0 = sin filtro (todo el ancho de banda hasta ~1350 Hz)
    float gate       = 0.01f;   // por debajo de este nivel, silencio (0 = desactivada)
    float latencyMs  = 25.0f;   // pre-búfer objetivo
    // Control automático de ganancia: lleva los picos siempre a 'level',
    // sin importar el volumen de Windows, del reproductor o del juego.
    bool  agc        = true;
    float level      = 0.9f;    // nivel objetivo de los picos (0..1)
    float agcMaxDb   = 30.0f;   // amplificación máxima (evita levantar ruido de fondo)
    float agcAttackMs  = 5.0f;  // qué tan rápido baja la ganancia ante un golpe fuerte
    float agcReleaseMs = 1000.0f; // qué tan rápido sube cuando el audio se vuelve más bajo
};

// ---------------------------------------------------------------------------
// Pipeline completo. feed() desde el hilo de captura, pull() desde el de envío.
class LivePipeline {
public:
    static constexpr unsigned kRate = 3000;
    static constexpr size_t kReportBytes = 64;

    explicit LivePipeline(const LiveConfig& cfg = {}) : cfg_(cfg) {
        lpL_.lowpass(kRate, cfg.lowpassHz);
        lpR_.lowpass(kRate, cfg.lowpassHz);
        target_ = std::max<size_t>(kReportBytes, size_t(std::lround(cfg.latencyMs * kRate / 1000.0)) * 2);
        maxFill_ = target_ + kReportBytes * 4;
        releaseCoef_ = float(std::exp(-1.0 / (0.050 * kRate)));  // liberación 50 ms
        smoothCoef_ = float(std::exp(-1.0 / (0.005 * kRate)));   // rampa 5 ms
        agcAttack_ = float(std::exp(-1.0 / (std::max(0.1f, cfg.agcAttackMs) * 0.001 * kRate)));
        agcRelease_ = float(std::exp(-1.0 / (std::max(1.0f, cfg.agcReleaseMs) * 0.001 * kRate)));
        agcFloor_ = cfg.level / float(std::pow(10.0, cfg.agcMaxDb / 20.0));
        agcEnv_ = agcFloor_;
    }

    // Ganancia que está aplicando el AGC ahora (1.0 si está desactivado).
    float agcGain() const { return agcGainNow_.load(std::memory_order_relaxed); }

    // ---- productor ----
    void feed(const float* stereo, size_t frames, unsigned rate) {
        if (rate != rs_.srcRate()) rs_.configure(rate, kRate);
        rs_.push(stereo, frames, [this](float l, float r) { produce(l, r); });
    }

    // ---- consumidor: siempre llena 64 bytes; devuelve false si fue silencio de relleno ----
    bool pull(int8_t out[kReportBytes]) {
        size_t w = w_.load(std::memory_order_acquire);
        size_t r = r_.load(std::memory_order_relaxed);
        size_t avail = w - r;
        if (!primed_) {
            if (avail < target_) { std::memset(out, 0, kReportBytes); return false; }
            primed_ = true;
        }
        if (avail > maxFill_) {                // demasiada latencia acumulada: saltar adelante
            r = w - target_;
            drops_.fetch_add(1, std::memory_order_relaxed);
            avail = target_;
        }
        if (avail < kReportBytes) {            // se vació: volver a pre-cargar
            primed_ = false;
            underruns_.fetch_add(1, std::memory_order_relaxed);
            r_.store(r, std::memory_order_release);
            std::memset(out, 0, kReportBytes);
            return false;
        }
        for (size_t i = 0; i < kReportBytes; ++i) out[i] = ring_[(r + i) & kMask];
        r_.store(r + kReportBytes, std::memory_order_release);
        return true;
    }

    // ---- consumidor, modo directo: entrega 'n' bytes solo si ya están disponibles ----
    // Sin pre-búfer: el que llama manda un reporte apenas se junta el audio.
    bool pullExact(int8_t* out, size_t n) {
        size_t w = w_.load(std::memory_order_acquire);
        size_t r = r_.load(std::memory_order_relaxed);
        if (w - r > maxFill_) {                // atraso acumulado: descartar lo viejo
            r = w - ((target_ + n - 1) / n) * n;
            drops_.fetch_add(1, std::memory_order_relaxed);
        }
        if (w - r < n) { r_.store(r, std::memory_order_release); return false; }
        for (size_t i = 0; i < n; ++i) out[i] = ring_[(r + i) & kMask];
        r_.store(r + n, std::memory_order_release);
        return true;
    }

    // ---- estadísticas (cualquier hilo) ----
    double bufferedMs() const {
        size_t w = w_.load(std::memory_order_acquire), r = r_.load(std::memory_order_acquire);
        return double(w - r) / 2.0 * 1000.0 / kRate;
    }
    float takePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    unsigned underruns() const { return underruns_.load(std::memory_order_relaxed); }
    unsigned drops() const { return drops_.load(std::memory_order_relaxed); }
    unsigned overruns() const { return overruns_.load(std::memory_order_relaxed); }
    double resamplerLatencyMs() const { return rs_.srcRate() ? rs_.latencySeconds() * 1000.0 : 0.0; }

private:
    static constexpr size_t kCap = 1 << 14;   // 16 KB = 2,7 s a 3 kHz estéreo
    static constexpr size_t kMask = kCap - 1;

    void produce(float l, float r) {
        l = lpL_.process(l);
        r = lpR_.process(r);
        // Compuerta de ruido con envolvente (ataque instantáneo, liberación 50 ms)
        float a = std::max(std::fabs(l), std::fabs(r));
        env_ = a > env_ ? a : env_ * releaseCoef_;
        float g = (cfg_.gate <= 0.0f || env_ >= cfg_.gate) ? 1.0f : 0.0f;
        gateGain_ = g + (gateGain_ - g) * smoothCoef_;

        float agc = 1.0f;
        if (cfg_.agc) {
            // Envolvente de picos: ataque rápido, liberación lenta. Solo se adapta con la
            // compuerta abierta, para que el silencio no lleve la ganancia al máximo.
            if (g > 0.0f) {
                float c = a > agcEnv_ ? agcAttack_ : agcRelease_;
                agcEnv_ = a + (agcEnv_ - a) * c;
            }
            agc = cfg_.level / std::max(agcEnv_, agcFloor_);
            agcGainNow_.store(agc, std::memory_order_relaxed);
        }
        l *= agc * cfg_.gain * gateGain_;
        r *= agc * cfg_.gain * gateGain_;
        float pk = std::max(std::fabs(l), std::fabs(r));
        float prev = peak_.load(std::memory_order_relaxed);
        if (pk > prev) peak_.store(pk, std::memory_order_relaxed);

        size_t w = w_.load(std::memory_order_relaxed);
        size_t rd = r_.load(std::memory_order_acquire);
        if (w - rd + 2 > kCap) { overruns_.fetch_add(1, std::memory_order_relaxed); return; }
        ring_[w & kMask]       = toS8(l);
        ring_[(w + 1) & kMask] = toS8(r);
        w_.store(w + 2, std::memory_order_release);
    }
    static int8_t toS8(float v) {
        v = std::clamp(v, -1.0f, 1.0f);
        return int8_t(std::lround(v * 127.0f));
    }

    LiveConfig cfg_;
    StreamResampler rs_;
    Biquad lpL_, lpR_;
    float env_ = 0, gateGain_ = 0, releaseCoef_ = 0, smoothCoef_ = 0;
    float agcEnv_ = 0, agcFloor_ = 0, agcAttack_ = 0, agcRelease_ = 0;
    std::atomic<float> agcGainNow_{1.0f};
    size_t target_ = 128, maxFill_ = 384;
    bool primed_ = false;  // solo lo toca el consumidor
    int8_t ring_[kCap] = {};
    std::atomic<size_t> w_{0}, r_{0};
    std::atomic<float> peak_{0.0f};
    std::atomic<unsigned> underruns_{0}, drops_{0}, overruns_{0};
};

}  // namespace ds5

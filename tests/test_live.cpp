// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// Tests del pipeline en tiempo real (haptic_live.h).
// Compilar:  g++ -std=c++17 -O2 -I../src test_live.cpp -o test_live -pthread
#include "haptic_audio.h"
#include "haptic_live.h"
#include <cstdio>
#include <cmath>
#include <random>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FALLO: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)
static const double PI = 3.14159265358979323846;

static std::vector<float> sine(double hz, unsigned rate, double sec, float amp) {
    size_t n = size_t(rate * sec);
    std::vector<float> v(n * 2);
    for (size_t i = 0; i < n; ++i) v[i * 2] = v[i * 2 + 1] = amp * float(std::sin(2 * PI * hz * i / rate));
    return v;
}

// 1) El remuestreo por bloques coincide con el remuestreo offline
static void testStreamingMatchesOffline(unsigned rate) {
    auto in = ds5::genSweep(40, 1200, 2.0, 1.0f);  // solo para tener algo variado...
    std::vector<float> src(size_t(rate * 2.0) * 2);
    double ph = 0;
    for (size_t i = 0; i < src.size() / 2; ++i) {
        double hz = 40 * std::pow(30.0, double(i) / (src.size() / 2));
        ph += 2 * PI * hz / rate;
        src[i * 2] = 0.7f * float(std::sin(ph));
        src[i * 2 + 1] = 0.4f * float(std::cos(ph * 0.5));
    }
    auto ref = ds5::resampleStereo(src, rate, 3000);

    ds5::StreamResampler rs;
    rs.configure(rate, 3000);
    std::vector<float> out;
    std::mt19937 rng(42);
    size_t pos = 0, frames = src.size() / 2;
    while (pos < frames) {
        size_t n = std::min<size_t>(frames - pos, 1 + rng() % 900);
        rs.push(&src[pos * 2], n, [&](float l, float r) { out.push_back(l); out.push_back(r); });
        pos += n;
    }
    size_t cmp = std::min(out.size(), ref.size());
    double maxErr = 0;
    for (size_t i = 0; i < cmp; ++i) maxErr = std::max(maxErr, double(std::fabs(out[i] - ref[i])));
    CHECK(cmp > ref.size() - 40, "%u Hz: pocas muestras %zu de %zu", rate, cmp / 2, ref.size() / 2);
    CHECK(maxErr < 0.003, "%u Hz: streaming difiere del offline, error %.5f", rate, maxErr);
    (void)in;
}

// 2) Simulación en tiempo real: ráfagas de WASAPI vs envío cada 10,667 ms
struct SimResult { unsigned underruns, drops, overruns; double minBuf, maxBuf; double peakOut; size_t silentAfterStart; };

static SimResult simulate(double driftPpm, double seconds, ds5::LiveConfig cfg, double hz = 150, float amp = 0.5f) {
    ds5::LivePipeline pipe(cfg);
    const unsigned rate = 48000;
    const double prodPeriod = 0.010;                 // WASAPI entrega cada 10 ms
    const double consPeriod = 32.0 / 3000.0;         // 10,667 ms
    const double prodRate = rate * (1.0 + driftPpm * 1e-6);  // reloj de la placa de audio
    auto src = sine(hz, rate, seconds + 1, amp);
    double nextProd = 0, nextCons = 0, srcPos = 0;
    size_t srcIdx = 0;
    SimResult res{0, 0, 0, 1e9, 0, 0, 0};
    bool started = false;
    int8_t buf[64];
    for (double t = 0; t < seconds;) {
        if (nextProd <= nextCons) {
            t = nextProd;
            srcPos += prodRate * prodPeriod;
            size_t upto = std::min(size_t(srcPos), src.size() / 2);
            if (upto > srcIdx) pipe.feed(&src[srcIdx * 2], upto - srcIdx, rate);
            srcIdx = upto;
            nextProd += prodPeriod;
        } else {
            t = nextCons;
            bool real = pipe.pull(buf);
            if (real) started = true;
            if (started && t > 1.0) {
                if (!real) ++res.silentAfterStart;
                double b = pipe.bufferedMs();
                res.minBuf = std::min(res.minBuf, b);
                res.maxBuf = std::max(res.maxBuf, b);
                for (int i = 0; i < 64; ++i) res.peakOut = std::max(res.peakOut, std::fabs(double(buf[i])) / 127.0);
            }
            nextCons += consPeriod;
        }
    }
    res.underruns = pipe.underruns();
    res.drops = pipe.drops();
    res.overruns = pipe.overruns();
    return res;
}


// Alimenta un tono con cambio de volumen a mitad y devuelve el pico de salida
// en ventanas de 1 s (sin simular tiempos: se consume todo lo producido).
static std::vector<double> agcRun(ds5::LiveConfig cfg, float amp1, float amp2, double stepAt, double total) {
    ds5::LivePipeline pipe(cfg);
    const unsigned rate = 48000;
    size_t n = size_t(rate * total);
    std::vector<float> src(n * 2);
    for (size_t i = 0; i < n; ++i) {
        float a = (i < size_t(rate * stepAt)) ? amp1 : amp2;
        src[i * 2] = src[i * 2 + 1] = a * float(std::sin(2 * PI * 120 * i / rate));
    }
    std::vector<double> peaks(size_t(total) + 1, 0.0);
    size_t outFrames = 0;
    int8_t buf[64];
    for (size_t pos = 0; pos < n; pos += 480) {
        pipe.feed(&src[pos * 2], std::min<size_t>(480, n - pos), rate);
        while (pipe.pull(buf)) {
            size_t sec = outFrames / 3000;
            for (int k = 0; k < 64; ++k)
                peaks[std::min(sec, peaks.size() - 1)] = std::max(peaks[std::min(sec, peaks.size() - 1)], std::fabs(double(buf[k])) / 127.0);
            outFrames += 32;
        }
    }
    return peaks;
}

int main() {
    testStreamingMatchesOffline(48000);
    testStreamingMatchesOffline(44100);
    testStreamingMatchesOffline(96000);

    ds5::LiveConfig flat;
    flat.gain = 1.0f; flat.lowpassHz = 0; flat.gate = 0; flat.latencyMs = 25; flat.agc = false;

    // Relojes iguales: ni cortes ni saltos, latencia estable
    auto r0 = simulate(0, 30, flat);
    CHECK(r0.underruns == 0 && r0.drops == 0 && r0.silentAfterStart == 0,
          "sin deriva: underruns %u drops %u silencios %zu", r0.underruns, r0.drops, r0.silentAfterStart);
    CHECK(r0.maxBuf < 50, "sin deriva: búfer máximo %.1f ms", r0.maxBuf);
    CHECK(std::fabs(r0.peakOut - 0.5) < 0.03, "amplitud %.3f (esperado 0,5)", r0.peakOut);
    std::printf("sin deriva:     búfer %.1f-%.1f ms, underruns %u, drops %u\n", r0.minBuf, r0.maxBuf, r0.underruns, r0.drops);

    // Placa de audio 500 ppm más rápida: el búfer crece y se recorta, nunca se dispara
    auto r1 = simulate(+500, 120, flat);
    CHECK(r1.maxBuf < 120, "deriva +: búfer máximo %.1f ms", r1.maxBuf);
    CHECK(r1.overruns == 0, "deriva +: overruns %u", r1.overruns);
    std::printf("deriva +500ppm: búfer %.1f-%.1f ms, underruns %u, drops %u\n", r1.minBuf, r1.maxBuf, r1.underruns, r1.drops);

    // Placa 500 ppm más lenta: algún corte breve cada tanto, se recupera
    auto r2 = simulate(-500, 120, flat);
    CHECK(r2.underruns < 10, "deriva -: underruns %u", r2.underruns);
    CHECK(r2.silentAfterStart < 100, "deriva -: %zu reportes en silencio", r2.silentAfterStart);
    std::printf("deriva -500ppm: búfer %.1f-%.1f ms, underruns %u, drops %u\n", r2.minBuf, r2.maxBuf, r2.underruns, r2.drops);

    // Pasa-bajos 300 Hz: un tono de 1000 Hz queda muy atenuado, uno de 100 Hz pasa
    ds5::LiveConfig lp = flat; lp.lowpassHz = 300;
    auto hi = simulate(0, 5, lp, 1000, 0.5f);
    auto lo = simulate(0, 5, lp, 100, 0.5f);
    CHECK(hi.peakOut < 0.07, "pasa-bajos deja pasar 1 kHz: %.3f", hi.peakOut);
    CHECK(lo.peakOut > 0.45, "pasa-bajos atenúa 100 Hz: %.3f", lo.peakOut);
    std::printf("pasa-bajos 300 Hz: 100 Hz -> %.3f, 1000 Hz -> %.3f\n", lo.peakOut, hi.peakOut);

    // Compuerta: ruido muy bajo queda en silencio total
    ds5::LiveConfig gt = flat; gt.gate = 0.01f;
    auto quiet = simulate(0, 5, gt, 150, 0.004f);
    CHECK(quiet.peakOut == 0.0, "compuerta no silencia ruido bajo: %.4f", quiet.peakOut);
    auto loud = simulate(0, 5, gt, 150, 0.3f);
    CHECK(loud.peakOut > 0.28, "compuerta corta señal fuerte: %.3f", loud.peakOut);


    // AGC: el nivel de salida no depende del volumen de entrada
    ds5::LiveConfig agc;
    agc.gain = 1.0f; agc.lowpassHz = 300; agc.gate = 0.001f; agc.agc = true; agc.level = 0.9f;
    for (float amp : {0.8f, 0.3f, 0.05f, 0.03f}) {
        auto pk = agcRun(agc, amp, amp, 99, 6);
        CHECK(pk[4] > 0.82 && pk[4] <= 1.0, "AGC con entrada %.2f: salida %.3f (esperado ~0,9)", amp, pk[4]);
        std::printf("AGC entrada %.2f -> salida %.3f\n", amp, pk[4]);
    }
    // Entrada demasiado baja: se amplifica como máximo 30 dB (no levanta ruido)
    {
        auto pk = agcRun(agc, 0.005f, 0.005f, 99, 6);
        CHECK(pk[4] < 0.2, "AGC amplifica de más una señal casi inaudible: %.3f", pk[4]);
        std::printf("AGC entrada 0.005 -> salida %.3f (tope de 30 dB)\n", pk[4]);
    }
    // Bajar el volumen al 10% a mitad de camino: se recupera solo
    {
        auto pk = agcRun(agc, 0.6f, 0.06f, 4, 12);
        CHECK(pk[3] > 0.82, "antes del cambio %.3f", pk[3]);
        CHECK(pk[10] > 0.82, "después de bajar el volumen no se recuperó: %.3f", pk[10]);
        std::printf("Volumen 100%% -> 10%%: segundo 3 = %.3f, segundo 5 = %.3f, 6 = %.3f, 7 = %.3f, 10 = %.3f\n", pk[3], pk[5], pk[6], pk[7], pk[10]);
    }
    // Subir el volumen de golpe: la ganancia baja enseguida (sin quedar saturado)
    {
        auto pk = agcRun(agc, 0.06f, 0.6f, 4, 8);
        CHECK(pk[6] <= 1.0 && pk[6] > 0.82, "después de subir el volumen: %.3f", pk[6]);
    }
    // Ruido de fondo bajo la compuerta: sigue en silencio aunque el AGC esté activo
    {
        auto pk = agcRun(agc, 0.0005f, 0.0005f, 99, 4);
        CHECK(pk[3] == 0.0, "el AGC hace vibrar el ruido de fondo: %.4f", pk[3]);
    }

    if (failures == 0) std::printf("OK: pipeline en tiempo real correcto\n");
    return failures ? 1 : 0;
}

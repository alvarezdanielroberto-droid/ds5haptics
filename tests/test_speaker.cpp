// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// Test del parlante por Bluetooth (reporte 0x36 + Opus).
// Linux: g++ -std=c++17 -Isrc -Itests/opus_shim tests/test_speaker.cpp /lib/x86_64-linux-gnu/libopus.so.0
#include "speaker_bt.h"
#include <cstdio>
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FALLO: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

int main() {
    const double PI = 3.14159265358979323846;
    ds5::SpeakerEncoder enc;
    CHECK(enc.init(), "no se pudo crear el codificador Opus");
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(48000, 2, &err);
    CHECK(dec && err == 0, "decodificador");

    // 1 s de tono de 440 Hz (izquierda) y 660 Hz (derecha) a 48 kHz, en bloques de 512
    const size_t blocks = 94;
    std::vector<float> decoded;
    size_t badSize = 0;
    float in[ds5::kSpeakerInFrames * 2];
    for (size_t b = 0; b < blocks; ++b) {
        for (size_t i = 0; i < ds5::kSpeakerInFrames; ++i) {
            size_t n = b * ds5::kSpeakerInFrames + i;
            in[i * 2] = 0.5f * float(std::sin(2 * PI * 440 * n / 48000.0));
            in[i * 2 + 1] = 0.5f * float(std::sin(2 * PI * 660 * n / 48000.0));
        }
        uint8_t pkt[ds5::kOpusBytes];
        if (!enc.encode(in, pkt)) ++badSize;
        float out[ds5::kOpusFrames * 2];
        int n = opus_decode_float(dec, pkt, int(ds5::kOpusBytes), out, int(ds5::kOpusFrames), 0);
        CHECK(n == int(ds5::kOpusFrames), "decodificó %d frames", n);
        decoded.insert(decoded.end(), out, out + ds5::kOpusFrames * 2);
    }
    CHECK(badSize == 0, "%zu paquetes no midieron exactamente 200 bytes (último: %d)", badSize, enc.lastSize());

    // El mando reproduce 480 frames por cada 10,667 ms (~45 kHz): medir la frecuencia
    // en esa escala de tiempo. Debe dar 440 y 660 Hz.
    auto freqAt45k = [&](int ch) {
        size_t start = 20 * ds5::kOpusFrames, crossings = 0, last = 0, first = 0;
        for (size_t i = start + 1; i < decoded.size() / 2; ++i) {
            if (decoded[(i - 1) * 2 + ch] < 0 && decoded[i * 2 + ch] >= 0) {
                if (!crossings) first = i;
                last = i;
                ++crossings;
            }
        }
        double seconds = double(last - first) / 45000.0;
        return (crossings - 1) / seconds;
    };
    double fl = freqAt45k(0), fr = freqAt45k(1);
    CHECK(std::fabs(fl - 440) < 3, "frecuencia izquierda %.1f Hz", fl);
    CHECK(std::fabs(fr - 660) < 3, "frecuencia derecha %.1f Hz", fr);
    double peak = 0;
    for (size_t i = 20 * ds5::kOpusFrames * 2; i < decoded.size(); ++i) peak = std::max(peak, double(std::fabs(decoded[i])));
    CHECK(peak > 0.45 && peak < 0.56, "amplitud %.3f (esperado ~0,5)", peak);
    std::printf("Opus: %zu paquetes de 200 bytes, tonos a %.1f y %.1f Hz (reproducidos a 45 kHz), pico %.3f\n",
                blocks, fl, fr, peak);

    // Silencio también debe dar 200 bytes
    std::fill(in, in + ds5::kSpeakerInFrames * 2, 0.0f);
    uint8_t pkt[ds5::kOpusBytes];
    CHECK(enc.encode(in, pkt), "silencio: %d bytes", enc.lastSize());

    // Formato del reporte 0x36
    ds5::AudioHapticsReport rep;
    int8_t hapt[64];
    for (int i = 0; i < 64; ++i) hapt[i] = int8_t(i - 32);
    rep.build(hapt, pkt);
    rep.build(hapt, pkt);
    const uint8_t* d = rep.data();
    CHECK(rep.size() == 398, "tamaño");
    CHECK(d[0] == 0x36 && d[1] == 0x10 && d[2] == 0x91 && d[3] == 7 && d[10] == 2, "cabecera y contador");
    CHECK(d[11] == 0x90 && d[12] == 63, "sub-paquete de estado");
    bool stateZero = true;
    for (int i = 13; i < 76; ++i) stateZero &= d[i] == 0;
    CHECK(stateZero, "el estado debe ir en cero (no tocar luces ni gatillos)");
    CHECK(d[76] == 0x92 && d[77] == 64 && std::memcmp(d + 78, hapt, 64) == 0, "háptica");
    CHECK(d[142] == 0x93 && d[143] == 200 && std::memcmp(d + 144, pkt, 200) == 0, "parlante");
    uint32_t crc = ds5::crc32_bt(d, 394);
    uint32_t st = d[394] | (d[395] << 8) | (d[396] << 16) | (uint32_t(d[397]) << 24);
    CHECK(crc == st, "CRC");

    // Cola: completa con silencio si falta audio, y acota la demora
    ds5::SpeakerFifo fifo;
    std::vector<float> chunk(100 * 2, 0.25f);
    fifo.push(chunk.data(), 100);
    float out[ds5::kSpeakerInFrames * 2];
    CHECK(!fifo.pop(out) && out[0] == 0.25f && out[199] == 0.25f && out[200] == 0.0f, "cola con faltante");
    std::vector<float> big(5000 * 2, 0.1f);
    fifo.push(big.data(), 5000);
    CHECK(fifo.drops() == 1 && fifo.pop(out), "cola acotada");

    opus_decoder_destroy(dec);
    if (!failures) std::printf("OK: parlante por Bluetooth (0x36 + Opus) correcto\n");
    return failures ? 1 : 0;
}

// SPDX-License-Identifier: MPL-2.0
// Prueba de la cola de avisos hablados: mezcla, silencio inicial y que no sature.
#include "voice_queue.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FALLO: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

int main() {
    ds5::VoiceQueue q;
    CHECK(!q.active(), "vacía al empezar");
    std::vector<float> tone(4800, 0.5f);   // 100 ms
    q.push(tone, 1.0f);
    CHECK(q.active(), "activa con voz");
    // 60 ms de silencio antes de la voz: el primer bloque deja el audio del juego a la mitad
    std::vector<float> st(512 * 2, 0.4f);
    q.mixInto(st.data(), 512);
    CHECK(std::fabs(st[0] - 0.2f) < 1e-6f, "silencio inicial: juego a la mitad (%.3f)", st[0]);
    // Avanzar hasta pasar el silencio (2880 frames)
    for (int i = 0; i < 5; ++i) { std::vector<float> z(512 * 2, 0.0f); q.mixInto(z.data(), 512); }
    std::vector<float> v(512 * 2, 0.8f);
    q.mixInto(v.data(), 512);
    CHECK(std::fabs(v[2 * 300] - 0.9f) < 1e-6f && std::fabs(v[2 * 300 + 1] - 0.9f) < 1e-6f, "voz mezclada en los dos canales (%.3f)", v[600]);
    std::vector<float> loud(512 * 2, 1.0f);
    q.mixInto(loud.data(), 512);
    CHECK(loud[0] <= 1.0f, "no satura");
    // Consumir todo: 2880 + 4800 = 7680 frames = 15 bloques
    for (int i = 0; i < 20; ++i) { std::vector<float> z(512 * 2, 0.0f); q.mixInto(z.data(), 512); }
    CHECK(!q.active(), "termina");
    std::vector<float> after(512 * 2, 0.3f);
    q.mixInto(after.data(), 512);
    CHECK(after[10] == 0.3f, "sin voz no toca el audio del juego");
    q.push(tone);
    q.clear();
    CHECK(!q.active(), "clear");
    if (!failures) std::printf("OK: cola de avisos hablados correcta\n");
    return failures ? 1 : 0;
}

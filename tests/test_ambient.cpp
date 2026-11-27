// SPDX-License-Identifier: MPL-2.0
// Pruebas del color de la barra de luz según la pantalla (ambient_color.h).
#include <cstdio>
#include <vector>

#include "ambient_color.h"

static int fails = 0;
#define CHECK(c, ...)                                \
    do {                                             \
        if (!(c)) {                                  \
            std::printf("FALLA: " __VA_ARGS__);      \
            std::printf("  (línea %d)\n", __LINE__); \
            ++fails;                                 \
        }                                            \
    } while (0)

struct Img {
    int w = 64, h = 36;
    std::vector<uint8_t> px = std::vector<uint8_t>(size_t(64) * 36 * 4, 0);
    void fill(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                uint8_t* p = &px[size_t(y * w + x) * 4];
                p[0] = b, p[1] = g, p[2] = r, p[3] = 255;   // BGRA
            }
    }
    ds5::ScreenColor color() const { return ds5::dominantColor(px.data(), w, h, w * 4); }
};

int main() {
    {
        Img i;
        i.fill(0, 0, 64, 36, 200, 20, 20);   // pantalla roja
        auto c = i.color();
        CHECK(c.r > 0.95f && c.g < 0.15f && c.b < 0.15f, "roja -> rojo (%.2f %.2f %.2f)", c.r, c.g, c.b);
    }
    {
        Img i;
        i.fill(0, 0, 64, 36, 20, 40, 160);   // menú azul
        auto c = i.color();
        CHECK(c.b > 0.95f && c.r < 0.2f, "azul -> azul (%.2f %.2f %.2f)", c.r, c.g, c.b);
    }
    {
        Img i;
        i.fill(0, 0, 64, 36, 10, 10, 14);      // menú oscuro
        i.fill(10, 10, 54, 14, 40, 120, 255);  // con texto/botones azules
        auto c = i.color();
        CHECK(c.b > 0.95f && c.r < 0.3f, "menú oscuro con azul -> azul (%.2f %.2f %.2f)", c.r, c.g, c.b);
        CHECK(c.brightness < 0.4f, "pantalla oscura -> luz tenue (%.2f)", c.brightness);
    }
    {
        Img i;
        i.fill(0, 0, 64, 36, 128, 128, 128);   // gris
        auto c = i.color();
        CHECK(c.r == 1.0f && c.g == 1.0f && c.b == 1.0f && c.brightness <= 0.35f, "gris -> blanco tenue");
    }
    {
        Img i;
        i.fill(0, 0, 64, 36, 110, 105, 100);   // escena gris-marrón
        i.fill(40, 5, 60, 25, 220, 30, 10);    // con un fuego / luz roja
        auto c = i.color();
        CHECK(c.r > 0.95f && c.b < 0.3f, "escena gris con algo rojo -> rojo (%.2f %.2f %.2f)", c.r, c.g, c.b);
    }
    {
        Img i;
        i.fill(0, 0, 64, 36, 30, 160, 40);     // pasto verde
        i.fill(0, 0, 64, 12, 120, 170, 230);   // cielo celeste arriba
        auto c = i.color();
        CHECK(c.g > 0.95f && c.r < 0.4f, "más pasto que cielo -> verde (%.2f %.2f %.2f)", c.r, c.g, c.b);
    }
    {
        Img i;   // negro total
        auto c = i.color();
        CHECK(c.brightness <= 0.2f, "negro -> casi apagado (%.2f)", c.brightness);
    }
    // Suavizado y ritmo
    {
        ds5::AmbientLight a;
        a.setRhythm(false);
        ds5::ScreenColor red;
        red.r = 1, red.g = 0, red.b = 0, red.brightness = 1;
        ds5::Rgb c{};
        for (int t = 0; t < 2; ++t) c = a.update(red, 0, 33);
        CHECK(c.r > 40 && c.r < 200, "el cambio es gradual (%d)", c.r);
        for (int t = 0; t < 40; ++t) c = a.update(red, 0, 33);
        CHECK(c.r >= 250 && c.g == 0, "llega al color (%d)", c.r);
        CHECK(a.changed(c) && !a.changed(c), "no reenvía lo mismo");
        a.setRhythm(true);
        ds5::Rgb quiet = a.update(red, 0, 33);
        for (int t = 0; t < 30; ++t) quiet = a.update(red, 0, 33);
        ds5::Rgb hit = a.update(red, 1.0f, 33);
        CHECK(quiet.r < 140 && hit.r > 240, "ritmo: late con los golpes (%d -> %d)", quiet.r, hit.r);
        ds5::Rgb later = hit;
        for (int t = 0; t < 30; ++t) later = a.update(red, 0, 33);
        CHECK(later.r < 140, "ritmo: después baja (%d)", later.r);
    }
    if (fails) { std::printf("%d prueba(s) fallaron\n", fails); return 1; }
    std::printf("OK: luz según la pantalla (color dominante, suavizado y ritmo) correcta\n");
    return 0;
}

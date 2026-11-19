// SPDX-License-Identifier: MPL-2.0
// Pruebas de la v1.1: gestos (botón PS, agitar), háptica/parlante simulados, atajo y perfiles.
#include "gestures.h"
#include "profiles.h"
#include "sim_fx.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FALLO: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

static void setAccel(uint8_t* u, float gx, float gy, float gz) {
    auto put = [](uint8_t* p, float g) { int16_t v = int16_t(std::lround(g * 8192.0f)); p[0] = uint8_t(v); p[1] = uint8_t(uint16_t(v) >> 8); };
    put(u + 22, gx); put(u + 24, gy); put(u + 26, gz);
}

int main() {
    // ---------------- botón PS ----------------
    {
        ds5::Gestures g;
        uint8_t u[64] = {};
        setAccel(u, 0, 0, 1);
        g.reset(0);
        u[10] = 0x01;   // PS apretado al prender el mando: no cuenta
        CHECK(!g.feed(u, 10).psPressed, "PS del encendido ignorado");
        u[10] = 0;
        g.feed(u, 100);
        u[10] = 0x01;
        CHECK(!g.feed(u, 900).psPressed, "PS durante los primeros 1,5 s ignorado");
        u[10] = 0;
        g.feed(u, 1600);
        u[10] = 0x01;
        CHECK(g.feed(u, 2000).psPressed, "toque de PS detectado");
        CHECK(!g.feed(u, 2004).psPressed, "mantenerlo no repite");
        u[10] = 0;
        g.feed(u, 2100);
        u[10] = 0x01;
        CHECK(g.feed(u, 2200).psPressed, "segundo toque");
    }
    // ---------------- agitar ----------------
    {
        ds5::Gestures g;
        uint8_t u[64] = {};
        g.reset(0);
        uint32_t t = 2000;
        bool shaken = false;
        // En reposo y movimientos normales de juego (hasta ~1,3 g): nada
        for (int i = 0; i < 1000; ++i, t += 4) {
            float wob = 0.3f * std::sin(i * 0.05f);
            setAccel(u, wob, 0, 1.0f + wob);
            shaken |= g.feed(u, t).shaken;
        }
        CHECK(!shaken, "reposo y movimiento suave no cuentan como agitar");
        // Un solo golpe fuerte (se cae el mando, un golpe en la mesa): nada
        setAccel(u, 3.5f, 0, 1); shaken |= g.feed(u, t += 4).shaken;
        setAccel(u, 0, 0, 1); for (int i = 0; i < 400; ++i) shaken |= g.feed(u, t += 4).shaken;
        CHECK(!shaken, "un golpe aislado no cuenta");
        // Agitar: ida y vuelta fuerte cada ~120 ms
        int found = -1;
        for (int k = 0; k < 10 && found < 0; ++k) {
            float dir = (k % 2) ? -3.0f : 3.0f;
            for (int i = 0; i < 30; ++i) {
                setAccel(u, i < 10 ? dir : 0.0f, 0, 1);
                if (g.feed(u, t += 4).shaken) { found = k; break; }
            }
        }
        CHECK(found >= 4 && found <= 6, "agitar detectado tras ~5 picos (pico %d)", found);
        // Inmediatamente después, otra sacudida no repite (pausa)
        bool again = false;
        for (int k = 0; k < 8; ++k) {
            float dir = (k % 2) ? -3.0f : 3.0f;
            for (int i = 0; i < 30; ++i) { setAccel(u, i < 10 ? dir : 0.0f, 0, 1); again |= g.feed(u, t += 4).shaken; }
        }
        CHECK(!again, "pausa después de cambiar de modo");
    }
    // ---------------- háptica y parlante simulados ----------------
    {
        ds5::SimFx fx;
        ds5::SimFx::Config c;
        fx.configure(c);
        const unsigned rate = 48000;
        std::vector<float> in, hap, spk;
        // 2 s de "lluvia": ruido suave
        unsigned seed = 1;
        auto noise = [&] { seed = seed * 1103515245u + 12345u; return ((seed >> 9) & 0xFFFF) / 32768.0f - 1.0f; };
        in.resize(rate * 2 * 2);
        for (size_t i = 0; i < in.size(); ++i) in[i] = 0.02f * noise();
        fx.process(in.data(), rate * 2, rate, hap, spk);
        float hapRain = 0, spkRain = 0;
        for (size_t i = hap.size() / 2; i < hap.size(); ++i) hapRain = std::max(hapRain, std::fabs(hap[i]));
        for (size_t i = spk.size() / 2; i < spk.size(); ++i) spkRain = std::max(spkRain, std::fabs(spk[i]));
        CHECK(hapRain > 0.05f && hapRain < 0.8f, "lluvia: háptica suave (%.3f)", hapRain);
        CHECK(spkRain < 0.01f, "lluvia: el parlante no suena (%.3f)", spkRain);
        CHECK(fx.impacts() == 0, "lluvia: sin golpes");
        CHECK(spk.size() == in.size(), "parlante a 48 kHz: misma cantidad de frames");
        // Explosión: golpe fuerte de 150 ms con graves
        std::vector<float> boom(rate * 2 * 1 / 2, 0.0f);   // 0,5 s
        for (size_t i = 0; i < rate * 15 / 100; ++i) {
            float v = 0.8f * std::sin(6.2831853f * 60.0f * float(i) / rate) * std::exp(-float(i) / (rate * 0.08f)) + 0.2f * noise();
            boom[i * 2] = boom[i * 2 + 1] = v;
        }
        for (size_t i = rate * 15 / 100; i < boom.size() / 2; ++i) boom[i * 2] = boom[i * 2 + 1] = 0.02f * noise();
        spk.clear();
        fx.process(boom.data(), boom.size() / 2, rate, hap, spk);
        float hapBoom = 0, spkBoom = 0;
        for (size_t i = 0; i < hap.size() / 3; ++i) hapBoom = std::max(hapBoom, std::fabs(hap[i]));
        for (float v : spk) spkBoom = std::max(spkBoom, std::fabs(v));
        CHECK(fx.impacts() == 1, "explosión: un golpe (%u)", fx.impacts());
        CHECK(hapBoom > 0.9f, "explosión: háptica a fondo (%.3f)", hapBoom);
        CHECK(spkBoom < 0.1f && fx.speakerHits() == 0, "explosión (grave): no va al parlante (%.3f)", spkBoom);
        // Voz: armónicos de 150 Hz con poca energía aguda, en frases -> solo háptica
        {
            std::vector<float> voice(rate * 2 * 2, 0.0f);   // 2 s
            for (size_t i = 0; i < voice.size() / 2; ++i) {
                bool on = (i / (rate / 4)) % 2 == 0;   // 250 ms hablando, 250 ms pausa
                float t = float(i) / rate, v = 0;
                for (int h = 1; h <= 20; ++h) v += std::sin(6.2831853f * 150.0f * h * t) / float(h * h);
                voice[i * 2] = voice[i * 2 + 1] = on ? 0.4f * v : 0.02f * noise();
            }
            spk.clear();
            unsigned before = fx.speakerHits();
            fx.process(voice.data(), voice.size() / 2, rate, hap, spk);
            float sv = 0;
            for (float x : spk) sv = std::max(sv, std::fabs(x));
            CHECK(fx.speakerHits() == before && sv < 0.1f, "voz: no va al parlante (%u, %.3f)", fx.speakerHits() - before, sv);
        }
        // Mecanismo / chirrido: ráfaga aguda de 4 kHz -> parlante
        {
            std::vector<float> sq(rate * 2 / 2, 0.0f);   // 0,5 s
            for (size_t i = 0; i < sq.size() / 2; ++i) {
                float v = (i < rate * 8 / 100) ? 0.3f * std::sin(6.2831853f * 4000.0f * float(i) / rate) : 0.0f;
                sq[i * 2] = sq[i * 2 + 1] = v + 0.02f * noise();
            }
            spk.clear();
            unsigned before = fx.speakerHits();
            fx.process(sq.data(), sq.size() / 2, rate, hap, spk);
            float ss = 0, tail = 0;
            for (size_t i = 0; i < spk.size() / 3; ++i) ss = std::max(ss, std::fabs(spk[i]));
            for (size_t i = spk.size() - 4800; i < spk.size(); ++i) tail = std::max(tail, std::fabs(spk[i]));
            CHECK(fx.speakerHits() == before + 1 && ss > 0.3f, "chirrido agudo: suena por el parlante (%u, %.3f)", fx.speakerHits() - before, ss);
            CHECK(tail < 0.05f, "el parlante se cierra después (%.3f)", tail);
        }
        // Volumen de Windows al 20%: la misma escena se siente parecida (normalización)
        ds5::SimFx fx2;
        fx2.configure(c);
        std::vector<float> quiet(in.size());
        for (size_t i = 0; i < in.size(); ++i) quiet[i] = in[i] * 0.5f;
        std::vector<float> hap2, spk2;
        fx2.process(quiet.data(), rate * 2, rate, hap2, spk2);
        float hq = 0;
        for (size_t i = hap2.size() / 2; i < hap2.size(); ++i) hq = std::max(hq, std::fabs(hap2[i]));
        CHECK(hq > hapRain * 0.6f, "independiente del volumen (%.3f vs %.3f)", hq, hapRain);
        // Sin parlante simulado: nada por el parlante
        ds5::SimFx fx3;
        c.speaker = false;
        fx3.configure(c);
        std::vector<float> hap3, spk3;
        fx3.process(in.data(), rate * 2, rate, hap3, spk3);
        std::vector<float> click(rate, 0.0f);   // 0,5 s con un chirrido
        for (size_t i = 0; i < rate / 2; ++i) click[i * 2] = click[i * 2 + 1] = i < 4000 ? 0.3f * std::sin(6.2831853f * 4000.0f * float(i) / rate) : 0.0f;
        fx3.process(click.data(), click.size() / 2, rate, hap3, spk3);
        float s3 = 0;
        for (float v : spk3) s3 = std::max(s3, std::fabs(v));
        CHECK(s3 == 0.0f && fx3.speakerHits() == 1, "parlante_simulado = no");
        // Vibración clásica del juego en silencio
        ds5::SimFx fx4;
        fx4.configure({});
        fx4.setRumble(255, 0);
        std::vector<float> sil(4800 * 2, 0.0f), hap4, spk4;
        fx4.process(sil.data(), 4800, rate, hap4, spk4);
        float r4 = 0;
        for (float v : hap4) r4 = std::max(r4, std::fabs(v));
        CHECK(r4 > 0.5f, "la vibración del juego se suma a la háptica (%.3f)", r4);
        // 44,1 kHz -> el parlante sale a 48 kHz
        ds5::SimFx fx5;
        fx5.configure({});
        std::vector<float> in44(44100 * 2, 0.0f), hap5, spk5;
        fx5.process(in44.data(), 44100, 44100, hap5, spk5);
        CHECK(std::abs(int(spk5.size() / 2) - 48000) <= 2, "remuestreo a 48 kHz (%zu)", spk5.size() / 2);
    }
    // ---------------- atajo ----------------
    {
        unsigned m = 0, vk = 0;
        std::string lab;
        CHECK(ds5::parseHotkey("alt+f11", m, vk, &lab) && m == 1 && vk == 0x7A && lab == "Alt+F11", "alt+f11 (%u %u %s)", m, vk, lab.c_str());
        CHECK(ds5::parseHotkey("Ctrl + Alt + D", m, vk, &lab) && m == 3 && vk == 'D' && lab == "Ctrl+Alt+D", "ctrl+alt+d");
        CHECK(ds5::parseHotkey("alt+insert", m, vk) && vk == 0x2D, "alt+insert");
        CHECK(!ds5::parseHotkey("alt", m, vk), "solo modificador: inválido");
        CHECK(!ds5::parseHotkey("alt+f30", m, vk), "f30 inválido");
        CHECK(!ds5::parseHotkey("a+b", m, vk), "dos teclas: inválido");
    }
    // ---------------- perfiles ----------------
    {
        auto d = ds5::ProfileSet::parse(ds5::kDefaultIniEs);
        CHECK(d.autoWaitSec == 50 && d.shake && d.psButtonStats && d.statsHotkey == "alt+f11", "valores nuevos por defecto (es)");
        CHECK(d.general.simHaptics && d.general.simSpeaker && d.general.sensitivity == 5, "simulados por defecto (es)");
        auto e = ds5::ProfileSet::parse(ds5::kDefaultIniEn);
        CHECK(e.autoWaitSec == 50 && e.shake && e.general.simHaptics && e.statsHotkey == "alt+f11", "valores nuevos por defecto (en)");
        auto p = ds5::ProfileSet::parse("[general]\nespera_automatico = 90\nagitar = no\nboton_ps_estadisticas = no\natajo_estadisticas = Ctrl+Alt+D\n"
                                        "[mafia]\nmodo = xbox\nhaptica_simulada = no\nsensibilidad = 8\n");
        CHECK(p.autoWaitSec == 90 && !p.shake && !p.psButtonStats && p.statsHotkey == "ctrl+alt+d", "claves de [general]");
        auto v = p.resolve("mafia3definitiveedition.exe");
        CHECK(!v.simHaptics && v.simSpeaker && v.sensitivity == 8 && v.mode == ds5::kModeXbox, "perfil por juego");
        // Agitar anota el modo en el perfil: la sección repetida gana
        auto q = ds5::ProfileSet::parse(std::string("[batman]\nmodo = xbox\n") + ds5::shakeProfileBlock("batman", ds5::kModePs4));
        CHECK(q.resolve("BatmanAK.exe").mode == ds5::kModePs4, "modo elegido agitando");
        auto u = ds5::ProfileSet::parse(std::string("[general]\nvoz = si\nrestaurar_audio = si\n[pbz]\nparlante = 7\n") + ds5::kIniUpgradeBlock5);
        CHECK(u.autoWaitSec == 50 && u.shake && u.resolve("PBZ.exe").speakerGain == 7.0f, "actualización 5");
    }
    if (!failures) std::printf("OK: gestos, háptica/parlante simulados, atajo y perfiles v1.1 correctos\n");
    return failures ? 1 : 0;
}

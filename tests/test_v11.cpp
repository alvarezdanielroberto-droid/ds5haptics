// SPDX-License-Identifier: MPL-2.0
// Pruebas de la v1.1: gestos (botón de micrófono, agitar), háptica/parlante simulados, atajo y perfiles.
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
    // ---------------- botón de micrófono ----------------
    {
        ds5::Gestures g;
        uint8_t u[64] = {};
        setAccel(u, 0, 0, 1);
        g.reset(0);
        u[10] = 0x01;   // el botón PS (con el que se prende) no cuenta
        CHECK(!g.feed(u, 10).micPressed, "PS no abre las estadísticas");
        u[10] = 0x04;
        CHECK(!g.feed(u, 12).micPressed, "botón al conectar ignorado");
        u[10] = 0;
        g.feed(u, 100);
        u[10] = 0x04;
        CHECK(!g.feed(u, 900).micPressed, "durante los primeros 1,5 s ignorado");
        u[10] = 0;
        g.feed(u, 1600);
        u[10] = 0x04;
        CHECK(g.feed(u, 2000).micPressed, "toque del botón de micrófono detectado");
        CHECK(!g.feed(u, 2004).micPressed, "mantenerlo no repite");
        u[10] = 0;
        g.feed(u, 2100);
        u[10] = 0x05;   // con PS apretado a la vez también cuenta
        CHECK(g.feed(u, 2200).micPressed, "segundo toque");
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
        CHECK(hapRain > 0.03f && hapRain < 0.4f, "lluvia: háptica suave (%.3f)", hapRain);
        CHECK(spkRain < 0.15f, "lluvia: el parlante queda bajo (%.3f)", spkRain);
        CHECK(fx.impacts() == 0, "lluvia: sin golpes");
        // El parlante sale por saltos de 512 frames y la primera salida espera una ventana entera
        CHECK(spk.size() <= in.size() && in.size() - spk.size() <= size_t(ds5::PercussiveSpeaker::kN) * 2, "parlante a 48 kHz: misma cantidad de frames (%zu de %zu)", spk.size(), in.size());
        // Explosión: golpe fuerte de 150 ms con graves
        std::vector<float> boom(rate * 2 * 1 / 2, 0.0f);   // 0,5 s
        for (size_t i = 0; i < rate * 15 / 100; ++i) {
            float v = 0.8f * std::sin(6.2831853f * 60.0f * float(i) / rate) * std::exp(-float(i) / (rate * 0.08f)) + 0.2f * noise();
            boom[i * 2] = boom[i * 2 + 1] = v;
        }
        for (size_t i = rate * 15 / 100; i < boom.size() / 2; ++i) boom[i * 2] = boom[i * 2 + 1] = 0.02f * noise();
        spk.clear();
        fx.process(boom.data(), boom.size() / 2, rate, hap, spk);
        float hapBoom = 0;
        for (size_t i = 0; i < hap.size() / 3; ++i) hapBoom = std::max(hapBoom, std::fabs(hap[i]));
        float spkMax = 0;
        for (float v : spk) spkMax = std::max(spkMax, std::fabs(v));
        CHECK(fx.impacts() == 1, "explosión: un golpe (%u)", fx.impacts());
        CHECK(hapBoom > 0.45f && hapBoom <= 0.5f, "explosión: háptica a fondo, sin pasar de 0,5 (%.3f)", hapBoom);
        CHECK(spkMax < 1.0f, "el parlante no se pasa de 1 (%.3f)", spkMax);
        // Parlante (golpes): pisadas sí; voz, música y lluvia no
        {
            const size_t n = rate * 6;   // 6 s
            auto runSpk = [&](std::vector<float>& sig, unsigned& hits, float& rmsOut) {
                ds5::SimFx f;
                f.configure(c);
                std::vector<float> h, o;
                f.process(sig.data(), sig.size() / 2, rate, h, o);
                hits = f.speakerHits();
                double e = 0;
                for (size_t i = rate * 2; i < o.size(); ++i) e += double(o[i]) * o[i];   // sin los primeros 1 s
                rmsOut = float(std::sqrt(e / double(std::max<size_t>(1, o.size() - rate * 2))));
            };
            auto norm = [](std::vector<float>& v, float target) {
                double e = 0;
                for (float x : v) e += double(x) * x;
                const float r = float(std::sqrt(e / double(v.size())));
                for (float& x : v) x *= target / r;
            };
            // Pisadas: ráfagas de ruido de ~25 ms cada 0,5 s
            std::vector<float> steps(n * 2, 0.0f);
            {
                float lp = 0, lp2 = 0;
                for (size_t i = 0; i < n; ++i) {
                    const float ph = float(i % (rate / 2)) / rate;
                    const float env = ph < 0.12f ? std::exp(-ph / 0.025f) : 0.0f;
                    const float x = noise() * env;
                    lp += 0.4f * (x - lp);
                    lp2 += 0.02f * (lp - lp2);
                    steps[i * 2] = steps[i * 2 + 1] = lp - lp2;
                }
                norm(steps, 0.1f);
            }
            // Voz: armónicos de un tono que se mueve (como al hablar), sílabas de 250 ms
            std::vector<float> voice(n * 2, 0.0f);
            {
                double ph = 0;
                for (size_t i = 0; i < n; ++i) {
                    const double t = double(i) / rate;
                    const double f0 = 150 + 50 * std::sin(6.2831853 * 0.7 * t) + 20 * std::sin(6.2831853 * 3 * t);
                    ph += 6.2831853 * f0 / rate;
                    const double sp = std::fmod(t, 0.25);
                    double env = sp < 0.02 ? sp / 0.02 : sp < 0.18 ? 1 : sp < 0.22 ? (0.22 - sp) / 0.04 : 0;
                    if (std::fmod(t, 2.0) > 1.6) env = 0;
                    double v = 0;
                    for (int h = 1; h <= 30 && f0 * h < 8000; ++h) {
                        const double f = f0 * h;
                        const double fa = 1 / (1 + std::pow((f - 700) / 300, 2)) + 0.6 / (1 + std::pow((f - 1800) / 400, 2)) +
                                          0.3 / (1 + std::pow((f - 2800) / 500, 2));
                        v += fa * std::sin(h * ph) / h;
                    }
                    voice[i * 2] = voice[i * 2 + 1] = float(env * v);
                }
                norm(voice, 0.1f);
            }
            // Música: acordes que cambian cada 1 s
            std::vector<float> music(n * 2, 0.0f);
            {
                const double fr[4][3] = {{261.6, 329.6, 392}, {293.7, 370, 440}, {246.9, 311.1, 370}, {220, 277.2, 329.6}};
                for (size_t i = 0; i < n; ++i) {
                    const double t = double(i) / rate, tt = std::fmod(t, 1.0);
                    const int ch = int(t) % 4;
                    const double env = std::min(1.0, tt / 0.01) * std::exp(-tt * 0.8);
                    double v = 0;
                    for (int k = 0; k < 3; ++k)
                        for (int h = 1; h <= 8; ++h) v += std::sin(6.2831853 * fr[ch][k] * h * t) / (h * h);
                    music[i * 2] = music[i * 2 + 1] = float(env * v);
                }
                norm(music, 0.1f);
            }
            std::vector<float> rain(n * 2);
            for (float& x : rain) x = noise();
            norm(rain, 0.1f);

            unsigned hS, hV, hM, hR, hVS, hMS;
            float rS, rV, rM, rR, rVS, rMS;
            runSpk(steps, hS, rS);
            runSpk(voice, hV, rV);
            runSpk(music, hM, rM);
            runSpk(rain, hR, rR);
            std::vector<float> vs(voice), ms(music);
            for (size_t i = 0; i < vs.size(); ++i) { vs[i] += 0.3f * steps[i]; ms[i] += 0.3f * steps[i]; }
            runSpk(vs, hVS, rVS);
            runSpk(ms, hMS, rMS);
            CHECK(hS >= 11 && hS <= 12 && rS > 0.05f, "pisadas: cada una al parlante (%u, %.3f)", hS, rS);
            CHECK(hM <= 1 && rM < 0.01f, "música: no pasa (%u, %.3f)", hM, rM);
            CHECK(hR <= 1 && rR < 0.01f, "lluvia pareja: no pasa (%u, %.3f)", hR, rR);
            CHECK(hV <= 6 && rV < 0.4f * rS, "voz: casi no pasa (%u, %.3f)", hV, rV);
            CHECK(hMS >= 10 && rMS > 0.03f, "pisadas bajo la música: se oyen (%u, %.3f)", hMS, rMS);
            CHECK(hVS >= 10 && rVS > 0.03f, "pisadas bajo la voz: se oyen (%u, %.3f)", hVS, rVS);
            // Ganancia (volumen del parlante del perfil): más volumen, más señal
            {
                ds5::SimFx::Config c2 = c;
                c2.speakerGain = 0.3f;
                ds5::SimFx f;
                f.configure(c2);
                std::vector<float> h, o;
                f.process(steps.data(), steps.size() / 2, rate, h, o);
                double e = 0;
                for (size_t i = rate * 2; i < o.size(); ++i) e += double(o[i]) * o[i];
                const float r = float(std::sqrt(e / double(o.size() - rate * 2)));
                CHECK(r < 0.6f * rS, "volumen del parlante del perfil (%.3f vs %.3f)", r, rS);
            }
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
        std::vector<float> click(rate, 0.0f);   // 0,5 s con un golpe
        for (size_t i = 0; i < rate / 2; ++i) click[i * 2] = click[i * 2 + 1] = i < 2000 ? 0.3f * noise() * std::exp(-float(i) / 400.0f) : 0.0f;
        fx3.process(click.data(), click.size() / 2, rate, hap3, spk3);
        float s3 = 0;
        for (float v : spk3) s3 = std::max(s3, std::fabs(v));
        CHECK(s3 == 0.0f && spk3.empty() && fx3.speakerHits() == 0, "parlante_simulado = no");
        // Vibración clásica del juego en silencio
        ds5::SimFx fx4;
        fx4.configure({});
        fx4.setRumble(255, 0);
        std::vector<float> sil(4800 * 2, 0.0f), hap4, spk4;
        fx4.process(sil.data(), 4800, rate, hap4, spk4);
        float r4 = 0;
        for (float v : hap4) r4 = std::max(r4, std::fabs(v));
        CHECK(r4 > 0.25f, "la vibración del juego se suma a la háptica (%.3f)", r4);
        // 44,1 kHz -> el parlante sale a 48 kHz
        ds5::SimFx fx5;
        fx5.configure({});
        std::vector<float> in44(44100 * 2, 0.0f), hap5, spk5;
        fx5.process(in44.data(), 44100, 44100, hap5, spk5);
        CHECK(std::abs(int(spk5.size() / 2) - 48000) <= ds5::PercussiveSpeaker::kN, "remuestreo a 48 kHz (%zu)", spk5.size() / 2);
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
        CHECK(d.autoWaitSec == 50 && d.shake && d.micButtonStats && d.statsHotkey.empty(), "valores nuevos por defecto (es): sin atajo");
        CHECK(d.general.simHaptics && !d.general.simSpeaker && d.general.sensitivity == 5 && !d.fastHaptics, "simulados por defecto (es): parlante apagado");
        auto e = ds5::ProfileSet::parse(ds5::kDefaultIniEn);
        CHECK(e.autoWaitSec == 50 && e.shake && e.general.simHaptics && e.statsHotkey.empty() && e.micButtonStats, "valores nuevos por defecto (en)");
        auto p = ds5::ProfileSet::parse("[general]\nespera_automatico = 90\nagitar = no\nboton_microfono_estadisticas = no\natajo_estadisticas = Ctrl+Alt+D\n"
                                        "[mafia]\nmodo = xbox\nhaptica_simulada = no\nsensibilidad = 8\n");
        CHECK(p.autoWaitSec == 90 && !p.shake && !p.micButtonStats && p.statsHotkey == "ctrl+alt+d", "claves de [general]");
        auto v = p.resolve("mafia3definitiveedition.exe");
        CHECK(!v.simHaptics && !v.simSpeaker && v.sensitivity == 8 && v.mode == ds5::kModeXbox, "perfil por juego");
        // Agitar anota el modo en el perfil: la sección repetida gana
        auto q = ds5::ProfileSet::parse(std::string("[batman]\nmodo = xbox\n") + ds5::shakeProfileBlock("batman", ds5::kModePs4));
        CHECK(q.resolve("BatmanAK.exe").mode == ds5::kModePs4, "modo elegido agitando");
        auto u = ds5::ProfileSet::parse(std::string("[general]\nvoz = si\nrestaurar_audio = si\n[pbz]\nparlante = 7\n") + ds5::kIniUpgradeBlock5);
        CHECK(u.autoWaitSec == 50 && u.shake && u.resolve("PBZ.exe").speakerGain == 7.0f, "actualización 5");
    }
    if (!failures) std::printf("OK: gestos, háptica/parlante simulados, atajo y perfiles v1.1 correctos\n");
    return failures ? 1 : 0;
}

// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// dsx_protocol.h — EXPERIMENTAL: entiende los mensajes que mandan los mods hechos para DSX
// (DualSenseX) y los traduce a reportes de salida del DualSense (portable, sin Windows).
//
// Los mods mandan JSON por UDP a 127.0.0.1:6969, por ejemplo:
//   {"instructions":[{"type":1,"parameters":[0,2,16,2,6,8]},{"type":2,"parameters":[0,255,0,0]}]}
//   type 1 = gatillo: [mando, gatillo (1 = L2, 2 = R2), modo, parámetros del modo...]
//   type 2 = color de la barra de luz: [mando, R, G, B]
//   type 7 = volver a lo normal (gatillos sin efecto)
// Otros tipos (LED de jugador, umbral de gatillo, LED del micrófono...) se ignoran.
//
// Los efectos se arman con el formato de los gatillos del DualSense (el mismo que describe
// la documentación libre de Nielk1): 0x05 sin efecto, 0x21 resistencia, 0x22 arco,
// 0x23 galope, 0x25 arma, 0x26 vibración, 0x27 máquina. Los modos "simples" de DSX
// (Soft, Hard, Rigid...) se aproximan con una resistencia de distinta fuerza.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ds5 {
namespace dsx {

// Modos de gatillo de DSX (TriggerMode)
enum Mode {
    kNormal = 0, kGameCube = 1, kVerySoft = 2, kSoft = 3, kHard = 4, kVeryHard = 5, kHardest = 6,
    kRigid = 7, kVibrateTrigger = 8, kChoppy = 9, kMedium = 10, kVibrateTriggerPulse = 11,
    kCustomTriggerValue = 12, kResistance = 13, kBow = 14, kGalloping = 15, kSemiAutomaticGun = 16,
    kAutomaticGun = 17, kMachine = 18,
};

struct Instruction {
    int type = 0;
    std::vector<double> params;
};

// ------------------------------------------------------------------ JSON mínimo ----
// Solo lo necesario: objetos, arreglos, números, true/false/null y cadenas (que en los
// parámetros valen 0). Tolera espacios y basura al final; si algo no cierra, devuelve false.
class Json {
public:
    static bool parse(const char* data, size_t len, std::vector<Instruction>& out) {
        Json j(data, len);
        out.clear();
        return j.top(out);
    }

private:
    Json(const char* d, size_t n) : p_(d), e_(d + n) {}

    void ws() { while (p_ < e_ && std::isspace(uint8_t(*p_))) ++p_; }
    bool eat(char c) { ws(); if (p_ < e_ && *p_ == c) { ++p_; return true; } return false; }
    bool peek(char c) { ws(); return p_ < e_ && *p_ == c; }

    bool str(std::string* s) {
        if (!eat('"')) return false;
        while (p_ < e_ && *p_ != '"') {
            if (*p_ == '\\') { ++p_; if (p_ >= e_) return false; }
            if (s) s->push_back(*p_);
            ++p_;
        }
        if (p_ >= e_) return false;
        ++p_;
        return true;
    }
    bool num(double& v) {
        ws();
        const char* s = p_;
        while (p_ < e_ && (std::isdigit(uint8_t(*p_)) || *p_ == '-' || *p_ == '+' || *p_ == '.' || *p_ == 'e' || *p_ == 'E')) ++p_;
        if (p_ == s) return false;
        v = std::strtod(std::string(s, p_).c_str(), nullptr);
        return true;
    }
    bool word(const char* w) {
        ws();
        size_t n = std::strlen(w);
        if (size_t(e_ - p_) >= n && std::memcmp(p_, w, n) == 0) { p_ += n; return true; }
        return false;
    }
    // Valor como número (true = 1; cadenas, null, objetos y arreglos = 0)
    bool value(double& v, int depth = 0) {
        if (depth > 16) return false;
        v = 0;
        ws();
        if (p_ >= e_) return false;
        if (*p_ == '"') return str(nullptr);
        if (*p_ == '{') {
            ++p_;
            if (eat('}')) return true;
            do {
                double d;
                if (!str(nullptr) || !eat(':') || !value(d, depth + 1)) return false;
            } while (eat(','));
            return eat('}');
        }
        if (*p_ == '[') {
            ++p_;
            if (eat(']')) return true;
            do {
                double d;
                if (!value(d, depth + 1)) return false;
            } while (eat(','));
            return eat(']');
        }
        if (word("true")) { v = 1; return true; }
        if (word("false") || word("null")) return true;
        return num(v);
    }
    bool instruction(Instruction& ins) {
        if (!eat('{')) return false;
        if (eat('}')) return true;
        do {
            std::string key;
            if (!str(&key) || !eat(':')) return false;
            if (key == "type") {
                double d;
                if (!value(d)) return false;
                ins.type = d >= 0 && d < 1000 ? int(d) : -1;
            } else if (key == "parameters" && peek('[')) {
                eat('[');
                if (!eat(']')) {
                    do {
                        double d;
                        if (!value(d)) return false;
                        ins.params.push_back(d);
                    } while (eat(','));
                    if (!eat(']')) return false;
                }
            } else {
                double d;
                if (!value(d)) return false;
            }
        } while (eat(','));
        return eat('}');
    }
    bool top(std::vector<Instruction>& out) {
        if (!eat('{')) return false;
        if (eat('}')) return true;
        do {
            std::string key;
            if (!str(&key) || !eat(':')) return false;
            if (key == "instructions" && peek('[')) {
                eat('[');
                if (!eat(']')) {
                    do {
                        Instruction ins;
                        if (!instruction(ins)) return false;
                        out.push_back(ins);
                    } while (eat(','));
                    if (!eat(']')) return false;
                }
            } else {
                double d;
                if (!value(d)) return false;
            }
        } while (eat(','));
        return eat('}');
    }

    const char* p_;
    const char* e_;
};

// --------------------------------------------------------- efectos de gatillo ----
// 11 bytes: modo + 10 parámetros (bytes 11..21 del reporte USB para R2, 22..32 para L2)
using Effect = uint8_t[11];

inline int clampi(double v, int lo, int hi) {
    if (!(v >= lo)) return lo;   // también NaN
    if (v >= hi) return hi;
    return int(v + 0.5);
}

inline void effOff(Effect e) { std::memset(e, 0, 11); e[0] = 0x05; }

// Resistencia desde 'start' (0-9) con fuerza 'strength' (1-8; 0 = sin efecto)
inline void effFeedback(Effect e, int start, int strength) {
    std::memset(e, 0, 11);
    if (strength <= 0) { effOff(e); return; }
    uint32_t force = 0;
    uint16_t active = 0;
    for (int i = start; i < 10; ++i) {
        force |= uint32_t((strength - 1) & 7) << (3 * i);
        active |= uint16_t(1u << i);
    }
    e[0] = 0x21;
    e[1] = uint8_t(active); e[2] = uint8_t(active >> 8);
    e[3] = uint8_t(force); e[4] = uint8_t(force >> 8); e[5] = uint8_t(force >> 16); e[6] = uint8_t(force >> 24);
}

// Arma semiautomática: resistencia entre start (2-7) y end (start+1..8) que "rompe" al pasar
inline void effWeapon(Effect e, int start, int end, int strength) {
    std::memset(e, 0, 11);
    if (strength <= 0) { effOff(e); return; }
    start = start < 2 ? 2 : start > 7 ? 7 : start;
    end = end <= start ? start + 1 : end > 8 ? 8 : end;
    uint16_t zones = uint16_t((1u << start) | (1u << end));
    e[0] = 0x25;
    e[1] = uint8_t(zones); e[2] = uint8_t(zones >> 8);
    e[3] = uint8_t((strength - 1) & 7);
}

// Vibración desde 'start' (0-9), amplitud 1-8, frecuencia en Hz
inline void effVibration(Effect e, int start, int amplitude, int freq) {
    std::memset(e, 0, 11);
    if (amplitude <= 0 || freq <= 0) { effOff(e); return; }
    uint32_t force = 0;
    uint16_t active = 0;
    for (int i = start; i < 10; ++i) {
        force |= uint32_t((amplitude - 1) & 7) << (3 * i);
        active |= uint16_t(1u << i);
    }
    e[0] = 0x26;
    e[1] = uint8_t(active); e[2] = uint8_t(active >> 8);
    e[3] = uint8_t(force); e[4] = uint8_t(force >> 8); e[5] = uint8_t(force >> 16); e[6] = uint8_t(force >> 24);
    e[9] = uint8_t(freq);
}

// Arco: se tensa entre start (0-8) y end (start+1..8) y "suelta" con snap
inline void effBow(Effect e, int start, int end, int strength, int snap) {
    std::memset(e, 0, 11);
    if (strength <= 0 || snap <= 0) { effOff(e); return; }
    end = end <= start ? start + 1 : end > 8 ? 8 : end;
    uint16_t zones = uint16_t((1u << start) | (1u << end));
    uint16_t pair = uint16_t(((strength - 1) & 7) | (((snap - 1) & 7) << 3));
    e[0] = 0x22;
    e[1] = uint8_t(zones); e[2] = uint8_t(zones >> 8);
    e[3] = uint8_t(pair); e[4] = uint8_t(pair >> 8);
}

// Galope (como un caballo): golpecitos entre start (0-8) y end (start+1..9)
inline void effGalloping(Effect e, int start, int end, int firstFoot, int secondFoot, int freq) {
    std::memset(e, 0, 11);
    if (freq <= 0) { effOff(e); return; }
    end = end <= start ? start + 1 : end > 9 ? 9 : end;
    secondFoot = secondFoot <= firstFoot ? firstFoot + 1 : secondFoot > 7 ? 7 : secondFoot;
    uint16_t zones = uint16_t((1u << start) | (1u << end));
    e[0] = 0x23;
    e[1] = uint8_t(zones); e[2] = uint8_t(zones >> 8);
    e[3] = uint8_t((secondFoot & 7) | ((firstFoot & 7) << 3));
    e[4] = uint8_t(freq);
}

// Máquina: vibración entre start (1-8) y end (start+1..9) que alterna dos fuerzas
inline void effMachine(Effect e, int start, int end, int ampA, int ampB, int freq, int period) {
    std::memset(e, 0, 11);
    if (freq <= 0) { effOff(e); return; }
    end = end <= start ? start + 1 : end > 9 ? 9 : end;
    uint16_t zones = uint16_t((1u << start) | (1u << end));
    e[0] = 0x27;
    e[1] = uint8_t(zones); e[2] = uint8_t(zones >> 8);
    e[3] = uint8_t((ampA & 7) | ((ampB & 7) << 3));
    e[4] = uint8_t(freq);
    e[5] = uint8_t(period);
}

// Modo de DSX + parámetros -> efecto del DualSense. p = parámetros del modo (después del modo).
inline void translate(int mode, const std::vector<double>& p, Effect e) {
    auto P = [&](size_t i, double def) { return i < p.size() ? p[i] : def; };
    switch (mode) {
        case kNormal: effOff(e); break;
        case kGameCube: effWeapon(e, 6, 8, 8); break;   // tope al final, como el gatillo de GameCube
        case kVerySoft: effFeedback(e, 0, 1); break;
        case kSoft: effFeedback(e, 0, 3); break;
        case kMedium: effFeedback(e, 0, 5); break;
        case kHard: effFeedback(e, 0, 6); break;
        case kVeryHard: effFeedback(e, 0, 7); break;
        case kHardest: effFeedback(e, 0, 8); break;
        case kRigid: effFeedback(e, 0, 8); break;
        case kVibrateTrigger: effVibration(e, 0, 8, clampi(P(0, 10), 1, 255)); break;
        case kChoppy: effGalloping(e, 0, 9, 2, 4, 12); break;
        case kVibrateTriggerPulse: effVibration(e, 2, 5, 40); break;
        case kResistance: effFeedback(e, clampi(P(0, 0), 0, 9), clampi(P(1, 8), 0, 8)); break;
        case kBow:
            effBow(e, clampi(P(0, 0), 0, 8), clampi(P(1, 8), 0, 8), clampi(P(2, 8), 0, 8), clampi(P(3, 8), 0, 8));
            break;
        case kGalloping:
            effGalloping(e, clampi(P(0, 0), 0, 8), clampi(P(1, 9), 0, 9), clampi(P(2, 2), 0, 6), clampi(P(3, 4), 0, 7),
                         clampi(P(4, 10), 0, 255));
            break;
        case kSemiAutomaticGun:
            effWeapon(e, clampi(P(0, 2), 2, 7), clampi(P(1, 6), 3, 8), clampi(P(2, 8), 0, 8));
            break;
        case kAutomaticGun:
            effVibration(e, clampi(P(0, 0), 0, 9), clampi(P(1, 8), 0, 8), clampi(P(2, 10), 0, 255));
            break;
        case kMachine:
            effMachine(e, clampi(P(0, 1), 0, 8), clampi(P(1, 9), 1, 9), clampi(P(2, 7), 0, 7), clampi(P(3, 3), 0, 7),
                       clampi(P(4, 10), 0, 255), clampi(P(5, 5), 0, 255));
            break;
        case kCustomTriggerValue: {
            // [submodo, 7 valores]: modos "crudos" del DualSense (resistencia y pulso)
            static const uint8_t raw[] = {0x05, 0x01, 0x21, 0x05, 0x25, 0x02, 0x22, 0x06, 0x26,
                                          0x26, 0x26, 0x26, 0x26, 0x27, 0x27, 0x27, 0x27};
            int sub = clampi(P(0, 0), 0, 16);
            std::memset(e, 0, 11);
            e[0] = raw[sub];
            for (int i = 0; i < 7; ++i) e[1 + i] = uint8_t(clampi(P(size_t(1 + i), 0), 0, 255));
            if (sub == 0) effOff(e);
            break;
        }
        default: effOff(e); break;
    }
}

// --------------------------------------------------------------- reportes USB ----
constexpr size_t kOutSize = 48;

struct Result {
    std::vector<std::vector<uint8_t>> reports;   // reportes USB 0x02 de 48 bytes
    unsigned triggers = 0, lights = 0, ignored = 0;
};

// Instrucciones -> reportes de salida USB (uno por gatillo o color; así cada uno queda
// guardado por separado para volver a mandarlo si el mando se reconecta)
inline Result toReports(const std::vector<Instruction>& ins) {
    Result r;
    for (const auto& in : ins) {
        std::vector<uint8_t> o(kOutSize, 0);
        o[0] = 0x02;
        if (in.type == 1 && in.params.size() >= 3) {
            int trig = clampi(in.params[1], 0, 3);
            if (trig != 1 && trig != 2) { ++r.ignored; continue; }
            std::vector<double> rest(in.params.begin() + 3, in.params.end());
            Effect e;
            translate(clampi(in.params[2], 0, 255), rest, e);
            if (trig == 2) { o[1] = 0x04; std::memcpy(&o[11], e, 11); }
            else { o[1] = 0x08; std::memcpy(&o[22], e, 11); }
            r.reports.push_back(o);
            ++r.triggers;
        } else if (in.type == 2 && in.params.size() >= 4) {
            o[2] = 0x04;   // valid_flag1: barra de luz
            o[45] = uint8_t(clampi(in.params[1], 0, 255));
            o[46] = uint8_t(clampi(in.params[2], 0, 255));
            o[47] = uint8_t(clampi(in.params[3], 0, 255));
            r.reports.push_back(o);
            ++r.lights;
        } else if (in.type == 7) {
            Effect e;
            effOff(e);
            o[1] = 0x04 | 0x08;
            std::memcpy(&o[11], e, 11);
            std::memcpy(&o[22], e, 11);
            r.reports.push_back(o);
            ++r.triggers;
        } else {
            ++r.ignored;
        }
    }
    return r;
}

// Respuesta al estilo de DSX (algunos mods la esperan para saber que hay "servidor")
inline std::string serverResponse(int battery, bool connected) {
    return std::string("{\"Status\":\"DSX Received UDP Instructions\",\"TimeReceived\":\"\",\"isControllerConnected\":") +
           (connected ? "true" : "false") + ",\"BatteryLevel\":" + std::to_string(battery < 0 ? 0 : battery) + "}";
}

}  // namespace dsx
}  // namespace ds5

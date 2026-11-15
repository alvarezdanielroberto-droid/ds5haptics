// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// virtual_pads.h — Modos de compatibilidad: el DualSense se presenta como otro mando.
//
//   VirtualDS4      DualShock 4 por USB (VID 054C, PID 05C4), para juegos con soporte de PS4.
//   VirtualXbox360  Mando de Xbox 360 por USB (VID 045E, PID 028E): Windows le carga su
//                   driver XInput, así que lo acepta prácticamente cualquier juego.
//
// Los dos reciben las entradas del DualSense (en su formato USB de 64 bytes) y las traducen.
// La vibración que piden los juegos vuelve por el callback rumble() y se manda al DualSense
// como vibración clásica. En estos modos no hay háptica por audio ni gatillos adaptativos.
//
// También: lectura de batería y armado de reportes de salida del DualSense (vibración,
// LED del micrófono, barra de luz). Portable, para probarlo con un cliente USB/IP simulado.
#pragma once
#include "usbip_server.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ds5 {

// ------------------------------------------------------------------ batería ----

struct BatteryInfo {
    int percent = -1;        // 5, 15, ... 95, 100 (el mando informa en pasos de 10)
    bool charging = false;
    bool full = false;
    bool operator==(const BatteryInfo& o) const { return percent == o.percent && charging == o.charging && full == o.full; }
    bool operator!=(const BatteryInfo& o) const { return !(*this == o); }
};

// Byte de estado del reporte de entrada USB del DualSense (offset 53):
// nibble bajo = nivel 0..10, nibble alto = 0 descargando, 1 cargando, 2 completa.
inline BatteryInfo parseBattery(const uint8_t ds[64]) {
    BatteryInfo b;
    int level = ds[53] & 0x0F, state = ds[53] >> 4;
    b.percent = std::min(level * 10 + 5, 100);
    b.charging = state == 1;
    b.full = state == 2;
    if (b.full) b.percent = 100;
    return b;
}

// --------------------------------------- reportes de salida (formato USB, 48 bytes) ----
// Mismo layout que manda un juego por USB (ID 0x02 + 47 bytes); main los convierte a Bluetooth.

constexpr size_t kUsbOutSize = 48;

// Vibración clásica emulada. 'v2' = firmware nuevo del mando (usa otra bandera).
inline void buildRumbleReport(uint8_t out[kUsbOutSize], uint8_t strong, uint8_t weak, bool v2) {
    std::memset(out, 0, kUsbOutSize);
    out[0] = 0x02;
    out[1] = v2 ? 0x02 : 0x03;      // valid_flag0: selección de vibración (+ vibración compatible)
    if (v2) out[39] = 0x04;         // valid_flag2: vibración compatible v2
    out[3] = weak;                  // motor derecho (chico)
    out[4] = strong;                // motor izquierdo (grande)
}

// LED del botón de micrófono: 0 apagado, 1 encendido, 2 pulsando.
inline void buildMicLedReport(uint8_t out[kUsbOutSize], uint8_t mode) {
    std::memset(out, 0, kUsbOutSize);
    out[0] = 0x02;
    out[2] = 0x01;                  // valid_flag1: control del LED del micrófono
    out[9] = mode;
}

inline void buildLightbarUsbReport(uint8_t out[kUsbOutSize], uint8_t r, uint8_t g, uint8_t b) {
    std::memset(out, 0, kUsbOutSize);
    out[0] = 0x02;
    out[2] = 0x04;                  // valid_flag1: control de la barra de luz
    out[45] = r; out[46] = g; out[47] = b;
}

// ------------------------------------------------------ traducción de entradas ----

// DualSense USB (0x01, 64 bytes) -> DualShock 4 USB (0x01, 64 bytes)
inline void dualSenseToDs4(const uint8_t ds[64], uint8_t out[64], uint8_t counter) {
    std::memset(out, 0, 64);
    out[0] = 0x01;
    out[1] = ds[1]; out[2] = ds[2]; out[3] = ds[3]; out[4] = ds[4];   // sticks
    out[5] = ds[8];                                // cruceta + cuadrado/cruz/círculo/triángulo
    out[6] = ds[9];                                // L1 R1 L2 R2 Share Options L3 R3
    out[7] = uint8_t((ds[10] & 0x03) | (counter << 2));   // PS, clic del panel táctil, contador
    out[8] = ds[5]; out[9] = ds[6];                // gatillos analógicos
    uint32_t ts = uint32_t(ds[28]) | (uint32_t(ds[29]) << 8) | (uint32_t(ds[30]) << 16) | (uint32_t(ds[31]) << 24);
    uint16_t t16 = uint16_t(ts / 16);              // 0,33 us -> 5,33 us
    out[10] = uint8_t(t16); out[11] = uint8_t(t16 >> 8);
    std::memcpy(out + 13, ds + 16, 6);             // giroscopio
    std::memcpy(out + 19, ds + 22, 6);             // acelerómetro
    int level = ds[53] & 0x0F, state = ds[53] >> 4;
    out[30] = uint8_t(0x10 | (state == 2 ? 11 : std::min(level, 10)));   // "con cable" + batería
    out[33] = 1;                                   // un paquete táctil
    out[34] = counter;
    auto touch = [](const uint8_t* s, uint8_t* d) {
        unsigned x = s[1] | ((s[2] & 0x0Fu) << 8);
        unsigned y = (s[2] >> 4) | (unsigned(s[3]) << 4);
        y = std::min(y * 942u / 1080u, 941u);      // el panel del DS4 es 1920x942
        d[0] = s[0];
        d[1] = uint8_t(x);
        d[2] = uint8_t(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
        d[3] = uint8_t(y >> 4);
    };
    touch(ds + 33, out + 35);
    touch(ds + 37, out + 39);
}

// DualSense USB -> reporte de Xbox 360 (20 bytes)
inline void dualSenseToXbox(const uint8_t ds[64], uint8_t out[20]) {
    std::memset(out, 0, 20);
    out[1] = 0x14;
    const uint8_t hat = ds[8] & 0x0F;
    const bool up = hat == 7 || hat == 0 || hat == 1, right = hat >= 1 && hat <= 3;
    const bool down = hat >= 3 && hat <= 5, left = hat >= 5 && hat <= 7;
    uint8_t b2 = 0, b3 = 0;
    if (up) b2 |= 0x01;
    if (down) b2 |= 0x02;
    if (left) b2 |= 0x04;
    if (right) b2 |= 0x08;
    if (ds[9] & 0x20) b2 |= 0x10;                       // Options -> Start
    if ((ds[9] & 0x10) || (ds[10] & 0x02)) b2 |= 0x20;  // Create o clic del panel -> Back
    if (ds[9] & 0x40) b2 |= 0x40;                       // L3
    if (ds[9] & 0x80) b2 |= 0x80;                       // R3
    if (ds[9] & 0x01) b3 |= 0x01;                       // L1 -> LB
    if (ds[9] & 0x02) b3 |= 0x02;                       // R1 -> RB
    if (ds[10] & 0x01) b3 |= 0x04;                      // PS -> Guía
    if (ds[8] & 0x20) b3 |= 0x10;                       // Cruz -> A
    if (ds[8] & 0x40) b3 |= 0x20;                       // Círculo -> B
    if (ds[8] & 0x10) b3 |= 0x40;                       // Cuadrado -> X
    if (ds[8] & 0x80) b3 |= 0x80;                       // Triángulo -> Y
    out[2] = b2; out[3] = b3;
    out[4] = ds[5]; out[5] = ds[6];                     // L2 -> LT, R2 -> RT
    auto put = [&](int at, int v) { out[at] = uint8_t(v); out[at + 1] = uint8_t(v >> 8); };
    auto wide = [](uint8_t v) { return int((v << 8) | v); };   // 0..255 -> 0..65535
    put(6, wide(ds[1]) - 32768);                        // izquierda X
    put(8, 32767 - wide(ds[2]));                        // izquierda Y (arriba = positivo)
    put(10, wide(ds[3]) - 32768);
    put(12, 32767 - wide(ds[4]));
}

// Tamaño en bytes (sin el Report ID) de cada reporte de un tipo: 0x80 entrada, 0x90 salida, 0xB0 feature.
inline std::map<uint8_t, size_t> hidReportSizes(const uint8_t* d, size_t len, uint8_t mainTag) {
    std::map<uint8_t, size_t> bits;
    unsigned id = 0, size = 0, count = 0;
    for (size_t i = 0; i < len;) {
        uint8_t p = d[i];
        size_t n = (p & 3) == 3 ? 4 : (p & 3);
        uint32_t v = 0;
        for (size_t k = 0; k < n && i + 1 + k < len; ++k) v |= uint32_t(d[i + 1 + k]) << (8 * k);
        switch (p & 0xFC) {
            case 0x84: id = v; break;      // Report ID
            case 0x74: size = v; break;    // Report Size
            case 0x94: count = v; break;   // Report Count
            default:
                if ((p & 0xFC) == mainTag) bits[uint8_t(id)] += size * count;
        }
        i += 1 + n;
    }
    std::map<uint8_t, size_t> bytes;
    for (auto& kv : bits) bytes[kv.first] = (kv.second + 7) / 8;
    return bytes;
}

inline std::vector<uint8_t> padString(const char* s) {
    std::vector<uint8_t> d{0, 0x03};
    for (const char* p = s; *p; ++p) { d.push_back(uint8_t(*p)); d.push_back(0); }
    d[0] = uint8_t(d.size());
    return d;
}

// --------------------------------------------------------------- base común ----

class SimplePad : public usbip::Device {
public:
    struct Callbacks {
        std::function<void(uint8_t strong, uint8_t weak)> rumble;
        std::function<void(uint8_t r, uint8_t g, uint8_t b)> lightbar;
        // Feature report del DualSense real (con el Report ID en el byte 0)
        std::function<bool(uint8_t id, std::vector<uint8_t>& out)> getFeature;
        std::function<void(const std::string&)> log;
    };

    void attached(usbip::Connection* c) override {
        std::lock_guard<std::mutex> lk(mu_);
        conn_ = c;
        pending_.clear();
        configured_ = false;
        lastStrong_ = lastWeak_ = 0;
    }

    bool unlink(uint32_t seqnum) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = pending_.begin(); it != pending_.end(); ++it)
            if (it->seqnum == seqnum) { pending_.erase(it); return true; }
        return false;
    }

    void submit(usbip::Connection& c, usbip::Submit&& q) override {
        if (q.ep == 0) { control(c, q); return; }
        if (q.ep == epIn_ && q.in()) {
            std::lock_guard<std::mutex> lk(mu_);
            if (fresh_) { fresh_ = false; c.retSubmit(q, 0, latest_.data(), std::min<size_t>(latest_.size(), q.length)); }
            else pending_.push_back(std::move(q));
            return;
        }
        if (q.ep == epOut_ && !q.in()) {
            ++outputs_;
            if (!q.data.empty()) onOutput(q.data.data(), q.data.size());
            c.retSubmit(q, 0, nullptr, q.data.size());
            return;
        }
        c.retSubmit(q, usbip::kEPIPE, nullptr, 0);
    }

    bool configured() const { return configured_; }
    unsigned outputs() const { return outputs_; }

protected:
    SimplePad(Callbacks cb, uint8_t epIn, uint8_t epOut) : cb_(std::move(cb)), epIn_(epIn), epOut_(epOut) {}

    virtual void control(usbip::Connection& c, const usbip::Submit& q) = 0;
    virtual void onOutput(const uint8_t* d, size_t n) = 0;

    // Entrega un reporte de entrada al juego (o lo guarda hasta que lo pida).
    void pushReport(const uint8_t* rep, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        latest_.assign(rep, rep + n);
        if (conn_ && !pending_.empty()) {
            usbip::Submit q = std::move(pending_.front());
            pending_.pop_front();
            fresh_ = false;
            conn_->retSubmit(q, 0, latest_.data(), std::min<size_t>(n, q.length));
        } else {
            fresh_ = true;
        }
    }

    void reply(usbip::Connection& c, const usbip::Submit& q, const uint8_t* d, size_t n) {
        uint16_t wLength = uint16_t(q.setup[6] | (q.setup[7] << 8));
        c.retSubmit(q, 0, d, std::min<size_t>({n, wLength, q.length}));
    }
    void stall(usbip::Connection& c, const usbip::Submit& q) { c.retSubmit(q, usbip::kEPIPE, nullptr, 0); }
    void ack(usbip::Connection& c, const usbip::Submit& q) { c.retSubmit(q, 0, nullptr, q.in() ? 0 : q.data.size()); }

    // Peticiones estándar de USB. Devuelve true si la atendió.
    bool standard(usbip::Connection& c, const usbip::Submit& q, const uint8_t* devDesc,
                  const std::vector<uint8_t>& cfg, const std::vector<const char*>& strings) {
        const uint8_t rt = q.setup[0], req = q.setup[1];
        if (((rt >> 5) & 3) != 0) return false;
        const uint8_t type = q.setup[3], index = q.setup[2];
        switch (req) {
        case 0x06:
            if (rt != 0x80) return false;          // los de interfaz (HID) los maneja la clase
            if (type == 0x01) { reply(c, q, devDesc, 18); return true; }
            if (type == 0x02) { reply(c, q, cfg.data(), cfg.size()); return true; }
            if (type == 0x03) {
                std::vector<uint8_t> s;
                if (index == 0) s = {4, 0x03, 0x09, 0x04};
                else if (index <= strings.size()) s = padString(strings[index - 1]);
                else { stall(c, q); return true; }
                reply(c, q, s.data(), s.size());
                return true;
            }
            stall(c, q);                           // device qualifier, etc.: no aplica (full speed)
            return true;
        case 0x09: configured_ = index != 0; ack(c, q); return true;
        case 0x08: { uint8_t v = configured_ ? 1 : 0; reply(c, q, &v, 1); return true; }
        case 0x0B: ack(c, q); return true;
        case 0x0A: { uint8_t v = 0; reply(c, q, &v, 1); return true; }
        case 0x00: { uint8_t st[2] = {0, 0}; reply(c, q, st, 2); return true; }
        case 0x01: case 0x03: ack(c, q); return true;
        default: stall(c, q); return true;
        }
    }

    void setRumble(uint8_t strong, uint8_t weak) {
        if (strong == lastStrong_ && weak == lastWeak_) return;
        lastStrong_ = strong; lastWeak_ = weak;
        if (cb_.rumble) cb_.rumble(strong, weak);
    }

    Callbacks cb_;
    std::mutex mu_;
    std::vector<uint8_t> latest_;

private:
    uint8_t epIn_, epOut_;
    usbip::Connection* conn_ = nullptr;
    std::deque<usbip::Submit> pending_;
    bool fresh_ = false;
    std::atomic<bool> configured_{false};
    std::atomic<unsigned> outputs_{0};
    uint8_t lastStrong_ = 0, lastWeak_ = 0;
};

// ------------------------------------------------------------- DualShock 4 ----

// Descriptor de reporte HID con la estructura del DualShock 4 por USB: mismos controles
// estándar (sticks, cruceta, 14 botones, gatillos) y mismos tamaños de reporte
// (entrada 0x01 de 64 bytes, salida 0x05 de 32, calibración 0x02 de 37...).
static const uint8_t kDs4ReportDescriptor[] = {
    0x05,0x01, 0x09,0x05, 0xA1,0x01, 0x85,0x01,
    0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x35, 0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x04, 0x81,0x02,
    0x09,0x39, 0x15,0x00, 0x25,0x07, 0x35,0x00, 0x46,0x3B,0x01, 0x65,0x14, 0x75,0x04, 0x95,0x01, 0x81,0x42, 0x65,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x0E, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x0E, 0x81,0x02,
    0x06,0x00,0xFF, 0x09,0x20, 0x75,0x06, 0x95,0x01, 0x15,0x00, 0x25,0x7F, 0x81,0x02,
    0x05,0x01, 0x09,0x33, 0x09,0x34, 0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x02, 0x81,0x02,
    0x06,0x00,0xFF, 0x09,0x21, 0x95,0x36, 0x81,0x02,
    0x85,0x05, 0x09,0x22, 0x95,0x1F, 0x91,0x02,
    0x85,0x04, 0x09,0x23, 0x95,0x24, 0xB1,0x02,
    0x85,0x02, 0x09,0x24, 0x95,0x24, 0xB1,0x02,
    0x85,0x08, 0x09,0x25, 0x95,0x03, 0xB1,0x02,
    0x85,0x10, 0x09,0x26, 0x95,0x04, 0xB1,0x02,
    0x85,0x11, 0x09,0x27, 0x95,0x02, 0xB1,0x02,
    0x85,0x12, 0x06,0x02,0xFF, 0x09,0x21, 0x95,0x0F, 0xB1,0x02,
    0x85,0x13, 0x09,0x22, 0x95,0x16, 0xB1,0x02,
    0x85,0x14, 0x06,0x05,0xFF, 0x09,0x20, 0x95,0x10, 0xB1,0x02,
    0x85,0x15, 0x09,0x21, 0x95,0x2C, 0xB1,0x02,
    0x06,0x80,0xFF, 0x85,0x80, 0x09,0x20, 0x95,0x06, 0xB1,0x02,
    0x85,0x81, 0x09,0x21, 0x95,0x06, 0xB1,0x02,
    0x85,0x82, 0x09,0x22, 0x95,0x05, 0xB1,0x02,
    0x85,0x83, 0x09,0x23, 0x95,0x01, 0xB1,0x02,
    0x85,0x84, 0x09,0x24, 0x95,0x04, 0xB1,0x02,
    0x85,0x85, 0x09,0x25, 0x95,0x06, 0xB1,0x02,
    0x85,0x86, 0x09,0x26, 0x95,0x06, 0xB1,0x02,
    0x85,0x87, 0x09,0x27, 0x95,0x23, 0xB1,0x02,
    0x85,0x88, 0x09,0x28, 0x95,0x22, 0xB1,0x02,
    0x85,0x89, 0x09,0x29, 0x95,0x02, 0xB1,0x02,
    0x85,0x90, 0x09,0x30, 0x95,0x05, 0xB1,0x02,
    0x85,0x91, 0x09,0x31, 0x95,0x03, 0xB1,0x02,
    0x85,0x92, 0x09,0x32, 0x95,0x03, 0xB1,0x02,
    0x85,0x93, 0x09,0x33, 0x95,0x0C, 0xB1,0x02,
    0x85,0xA0, 0x09,0x40, 0x95,0x06, 0xB1,0x02,
    0x85,0xA1, 0x09,0x41, 0x95,0x01, 0xB1,0x02,
    0x85,0xA2, 0x09,0x42, 0x95,0x01, 0xB1,0x02,
    0x85,0xA3, 0x09,0x43, 0x95,0x30, 0xB1,0x02,
    0x85,0xA4, 0x09,0x44, 0x95,0x0D, 0xB1,0x02,
    0x85,0xA5, 0x09,0x45, 0x95,0x15, 0xB1,0x02,
    0x85,0xA6, 0x09,0x46, 0x95,0x15, 0xB1,0x02,
    0x85,0xF0, 0x09,0x47, 0x95,0x3F, 0xB1,0x02,
    0x85,0xF1, 0x09,0x48, 0x95,0x3F, 0xB1,0x02,
    0x85,0xF2, 0x09,0x49, 0x95,0x0F, 0xB1,0x02,
    0x85,0xA7, 0x09,0x4A, 0x95,0x01, 0xB1,0x02,
    0x85,0xA8, 0x09,0x4B, 0x95,0x01, 0xB1,0x02,
    0x85,0xA9, 0x09,0x4C, 0x95,0x08, 0xB1,0x02,
    0x85,0xAA, 0x09,0x4E, 0x95,0x01, 0xB1,0x02,
    0x85,0xAB, 0x09,0x4F, 0x95,0x39, 0xB1,0x02,
    0x85,0xAC, 0x09,0x50, 0x95,0x39, 0xB1,0x02,
    0x85,0xAD, 0x09,0x51, 0x95,0x0B, 0xB1,0x02,
    0x85,0xAE, 0x09,0x52, 0x95,0x01, 0xB1,0x02,
    0x85,0xAF, 0x09,0x53, 0x95,0x02, 0xB1,0x02,
    0x85,0xB0, 0x09,0x54, 0x95,0x3F, 0xB1,0x02,
    0xC0,
};

class VirtualDS4 : public SimplePad {
public:
    explicit VirtualDS4(Callbacks cb) : SimplePad(std::move(cb), 4, 3) {
        const uint16_t rd = uint16_t(sizeof(kDs4ReportDescriptor));
        cfg_ = {
            9, 0x02, 41, 0, 1, 1, 0, 0xC0, 250,                     // configuración: 1 interfaz
            9, 0x04, 0, 0, 2, 0x03, 0x00, 0x00, 0,                  // interfaz HID
            9, 0x21, 0x11, 0x01, 0x00, 1, 0x22, uint8_t(rd), uint8_t(rd >> 8),
            7, 0x05, 0x84, 0x03, 64, 0, 5,                          // EP 0x84 IN, interrupción, 5 ms
            7, 0x05, 0x03, 0x03, 64, 0, 5,                          // EP 0x03 OUT
        };
        featureSizes_ = hidReportSizes(kDs4ReportDescriptor, sizeof(kDs4ReportDescriptor), 0xB0);
    }

    usbip::DeviceInfo info() const override {
        usbip::DeviceInfo d;
        d.vid = 0x054C; d.pid = 0x05C4; d.bcdDevice = 0x0100;
        d.speed = usbip::kSpeedFull;
        d.interfaces = {{0x03, 0x00, 0x00}};
        return d;
    }

    // Entrada del DualSense real (formato USB, 64 bytes)
    void pushInput(const uint8_t ds[64]) {
        uint8_t rep[64];
        dualSenseToDs4(ds, rep, counter_++);
        pushReport(rep, sizeof(rep));
    }

protected:
    void control(usbip::Connection& c, const usbip::Submit& q) override {
        static const uint8_t dev[18] = {18, 0x01, 0x00, 0x02, 0, 0, 0, 64, 0x4C, 0x05, 0xC4, 0x05,
                                        0x00, 0x01, 1, 2, 0, 1};
        if (standard(c, q, dev, cfg_, {"Sony Computer Entertainment", "Wireless Controller"})) return;
        const uint8_t rt = q.setup[0], req = q.setup[1], id = q.setup[2], type = q.setup[3];
        if (rt == 0x81 && req == 0x06) {                       // descriptores HID
            if (type == 0x22) return reply(c, q, kDs4ReportDescriptor, sizeof(kDs4ReportDescriptor));
            if (type == 0x21) return reply(c, q, cfg_.data() + 18, 9);
            return stall(c, q);
        }
        if (((rt >> 5) & 3) != 1) return stall(c, q);
        switch (req) {
        case 0x0A: case 0x0B: return ack(c, q);                // SET_IDLE, SET_PROTOCOL
        case 0x02: { uint8_t v = 0; return reply(c, q, &v, 1); }
        case 0x03: { uint8_t v = 1; return reply(c, q, &v, 1); }
        case 0x01:                                             // GET_REPORT
            if (type == 1) {
                uint8_t rep[64] = {0x01, 0x80, 0x80, 0x80, 0x80, 0x08};
                std::lock_guard<std::mutex> lk(mu_);
                if (latest_.size() == 64) std::memcpy(rep, latest_.data(), 64);
                return reply(c, q, rep, 64);
            }
            if (type == 3) return feature(c, q, id);
            return stall(c, q);
        case 0x09:                                             // SET_REPORT
            if (type == 2 && !q.data.empty()) onOutput(q.data.data(), q.data.size());
            return ack(c, q);
        default: return stall(c, q);
        }
    }

    // Salida 0x05: [1] banderas (1 vibración, 2 luz), [4] motor chico, [5] motor grande, [6..8] RGB
    void onOutput(const uint8_t* d, size_t n) override {
        if (n < 9 || d[0] != 0x05) return;
        if (d[1] & 0x01) setRumble(d[5], d[4]);
        if ((d[1] & 0x02) && cb_.lightbar) {
            uint32_t rgb = (uint32_t(d[6]) << 16) | (uint32_t(d[7]) << 8) | d[8];
            if (rgb != lastRgb_) { lastRgb_ = rgb; cb_.lightbar(d[6], d[7], d[8]); }
        }
    }

private:
    void feature(usbip::Connection& c, const usbip::Submit& q, uint8_t id) {
        auto it = featureSizes_.find(id);
        if (it == featureSizes_.end()) return stall(c, q);
        std::vector<uint8_t> out(it->second + 1, 0);
        out[0] = id;
        std::vector<uint8_t> real;
        if (id == 0x02) {
            // Calibración de sensores: por USB el DS4 usa el mismo orden que el DualSense (0x05)
            if (cb_.getFeature && cb_.getFeature(0x05, real) && real.size() >= 35) {
                std::memcpy(out.data() + 1, real.data() + 1, 34);
            } else {
                const int16_t def[17] = {0, 0, 0, 8839, -8839, 8839, -8839, 8839, -8839, 540, 540,
                                         8192, -8192, 8192, -8192, 8192, -8192};
                for (int i = 0; i < 17; ++i) { out[1 + i * 2] = uint8_t(def[i]); out[2 + i * 2] = uint8_t(uint16_t(def[i]) >> 8); }
            }
        } else if (id == 0x12 || id == 0x81) {
            // Dirección MAC del mando (la del DualSense real, feature 0x09)
            if (cb_.getFeature && cb_.getFeature(0x09, real) && real.size() >= 7) std::memcpy(out.data() + 1, real.data() + 1, 6);
            if (id == 0x12) { out[7] = 0x08; out[8] = 0x25; }
        }
        reply(c, q, out.data(), out.size());
    }

    std::vector<uint8_t> cfg_;
    std::map<uint8_t, size_t> featureSizes_;
    uint8_t counter_ = 0;
    uint32_t lastRgb_ = 0xFFFFFFFF;
};

// ---------------------------------------------------------------- Xbox 360 ----

class VirtualXbox360 : public SimplePad {
public:
    explicit VirtualXbox360(Callbacks cb) : SimplePad(std::move(cb), 1, 2) {
        cfg_ = {
            9, 0x02, 49, 0, 1, 1, 0, 0xA0, 250,                     // configuración: 1 interfaz
            9, 0x04, 0, 0, 2, 0xFF, 0x5D, 0x01, 0,                  // interfaz de control XInput
            17, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x02, 0x08, 0x00, 0x00,
            7, 0x05, 0x81, 0x03, 32, 0, 4,                          // EP 0x81 IN, interrupción, 4 ms
            7, 0x05, 0x02, 0x03, 32, 0, 8,                          // EP 0x02 OUT
        };
    }

    usbip::DeviceInfo info() const override {
        usbip::DeviceInfo d;
        d.vid = 0x045E; d.pid = 0x028E; d.bcdDevice = 0x0114;
        d.speed = usbip::kSpeedFull;
        d.devClass = 0xFF; d.devSubClass = 0xFF; d.devProtocol = 0xFF;
        d.interfaces = {{0xFF, 0x5D, 0x01}};
        return d;
    }

    void pushInput(const uint8_t ds[64]) {
        uint8_t rep[20];
        dualSenseToXbox(ds, rep);
        pushReport(rep, sizeof(rep));
    }

protected:
    void control(usbip::Connection& c, const usbip::Submit& q) override {
        static const uint8_t dev[18] = {18, 0x01, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 8, 0x5E, 0x04, 0x8E, 0x02,
                                        0x14, 0x01, 1, 2, 3, 1};
        if (standard(c, q, dev, cfg_, {"Microsoft Corporation", "Controller", "0000001"})) return;
        stall(c, q);   // peticiones propias de XInput: el driver de Windows sigue sin ellas
    }

    // Vibración: 00 08 00 <motor grande> <motor chico> 00 00 00
    void onOutput(const uint8_t* d, size_t n) override {
        if (n >= 5 && d[0] == 0x00 && d[1] == 0x08) setRumble(d[3], d[4]);
    }

private:
    std::vector<uint8_t> cfg_;
};

}  // namespace ds5

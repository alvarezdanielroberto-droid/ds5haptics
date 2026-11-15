// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// virtual_dualsense.h — DualSense USB virtual (fases 3a y 3b).
//
// Se presenta como un DualSense conectado por cable (VID 054C, PID 0CE6), con las mismas
// 4 interfaces que el mando real: control de audio, parlante de 4 canales (los canales
// 3 y 4 son la háptica), micrófono de 2 canales y HID. Traduce:
//   entrada   Bluetooth 0x31 (78 bytes)  ->  USB 0x01 (64 bytes)
//   salida    USB 0x02 (48 bytes)        ->  Bluetooth 0x31 (78 bytes, con CRC)
//   features  GET_REPORT del juego       ->  se piden al mando real por Bluetooth
//   audio     paquetes isócronos de 1 ms ->  callback audioOut (a ritmo real)
//
// Portable (sin Windows) para poder probarlo con un cliente USB/IP simulado.
#pragma once
#include "usbip_server.h"
#include "i18n.h"
#include "ds5_bt_haptics.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ds5 {

// ---------------------------------------------------------------- descriptores ----

// Descriptor de reporte HID del DualSense por USB (273 bytes), idéntico al del mando real.
// Verificado contra https://github.com/nondebug/dualsense (report-descriptor-usb).
static const uint8_t kUsbReportDescriptor[] = {
    0x05,0x01,0x09,0x05,0xa1,0x01,0x85,0x01,0x09,0x30,0x09,0x31,0x09,0x32,0x09,0x35,
    0x09,0x33,0x09,0x34,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x06,0x81,0x02,0x06,
    0x00,0xff,0x09,0x20,0x95,0x01,0x81,0x02,0x05,0x01,0x09,0x39,0x15,0x00,0x25,0x07,
    0x35,0x00,0x46,0x3b,0x01,0x65,0x14,0x75,0x04,0x95,0x01,0x81,0x42,0x65,0x00,0x05,
    0x09,0x19,0x01,0x29,0x0f,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x0f,0x81,0x02,0x06,
    0x00,0xff,0x09,0x21,0x95,0x0d,0x81,0x02,0x06,0x00,0xff,0x09,0x22,0x15,0x00,0x26,
    0xff,0x00,0x75,0x08,0x95,0x34,0x81,0x02,0x85,0x02,0x09,0x23,0x95,0x2f,0x91,0x02,
    0x85,0x05,0x09,0x33,0x95,0x28,0xb1,0x02,0x85,0x08,0x09,0x34,0x95,0x2f,0xb1,0x02,
    0x85,0x09,0x09,0x24,0x95,0x13,0xb1,0x02,0x85,0x0a,0x09,0x25,0x95,0x1a,0xb1,0x02,
    0x85,0x20,0x09,0x26,0x95,0x3f,0xb1,0x02,0x85,0x21,0x09,0x27,0x95,0x04,0xb1,0x02,
    0x85,0x22,0x09,0x40,0x95,0x3f,0xb1,0x02,0x85,0x80,0x09,0x28,0x95,0x3f,0xb1,0x02,
    0x85,0x81,0x09,0x29,0x95,0x3f,0xb1,0x02,0x85,0x82,0x09,0x2a,0x95,0x09,0xb1,0x02,
    0x85,0x83,0x09,0x2b,0x95,0x3f,0xb1,0x02,0x85,0x84,0x09,0x2c,0x95,0x3f,0xb1,0x02,
    0x85,0x85,0x09,0x2d,0x95,0x02,0xb1,0x02,0x85,0xa0,0x09,0x2e,0x95,0x01,0xb1,0x02,
    0x85,0xe0,0x09,0x2f,0x95,0x3f,0xb1,0x02,0x85,0xf0,0x09,0x30,0x95,0x3f,0xb1,0x02,
    0x85,0xf1,0x09,0x31,0x95,0x3f,0xb1,0x02,0x85,0xf2,0x09,0x32,0x95,0x0f,0xb1,0x02,
    0x85,0xf4,0x09,0x35,0x95,0x3f,0xb1,0x02,0x85,0xf5,0x09,0x36,0x95,0x03,0xb1,0x02,
    0xc0,
};
static_assert(sizeof(kUsbReportDescriptor) == 273, "descriptor HID del DualSense: 273 bytes");

// Tamaño (incluido el Report ID) de cada feature report según el descriptor USB.
inline size_t usbFeatureSize(uint8_t id) {
    static const std::map<uint8_t, size_t> sizes = {
        {0x05, 41}, {0x08, 48}, {0x09, 20}, {0x0a, 27}, {0x20, 64}, {0x21, 5}, {0x22, 64},
        {0x80, 64}, {0x81, 64}, {0x82, 10}, {0x83, 64}, {0x84, 64}, {0x85, 3}, {0xa0, 2},
        {0xe0, 64}, {0xf0, 64}, {0xf1, 64}, {0xf2, 16}, {0xf4, 64}, {0xf5, 4}};
    auto it = sizes.find(id);
    return it == sizes.end() ? 0 : it->second;
}

constexpr uint16_t kVid = 0x054C, kPid = 0x0CE6;
constexpr uint8_t kEpIn = 4, kEpOut = 3;   // 0x84 IN, 0x03 OUT (como el mando real)

static const uint8_t kDeviceDescriptor[18] = {
    18, 0x01, 0x00, 0x02,   // bcdUSB 2.00
    0x00, 0x00, 0x00, 64,   // clase por interfaz, EP0 de 64 bytes
    0x4C, 0x05, 0xE6, 0x0C, // VID 054C, PID 0CE6
    0x00, 0x01,             // bcdDevice 1.00
    1, 2, 0,                // fabricante, producto, sin número de serie
    1,                      // 1 configuración
};

static const uint8_t kQualifierDescriptor[10] = {10, 0x06, 0x00, 0x02, 0, 0, 0, 64, 1, 0};

// Configuración completa, igual a la del mando real (227 bytes, 4 interfaces).
// Fuente: https://github.com/nondebug/dualsense (lsusb-descriptor-info).
constexpr uint8_t kIfAudioControl = 0, kIfSpeaker = 1, kIfMic = 2, kIfHid = 3;
constexpr uint8_t kEpSpeaker = 1, kEpMic = 2;            // 0x01 OUT, 0x82 IN (isócronos)
constexpr uint8_t kUnitSpeaker = 2, kUnitMic = 5;         // Feature Units (volumen/mute)
constexpr unsigned kSpeakerChannels = 4, kMicChannels = 2, kAudioRate = 48000;

static const uint8_t kConfigDescriptor[] = {
    // Configuración: 227 bytes, 4 interfaces, autoalimentado, 500 mA
    9, 0x02, 0xE3, 0x00, 4, 1, 0, 0xC0, 250,

    // ---- Interfaz 0: control de audio (UAC 1.0) ----
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    10, 0x24, 0x01, 0x00, 0x01, 0x49, 0x00, 2, 1, 2,              // cabecera: 73 bytes, interfaces 1 y 2
    12, 0x24, 0x02, 1, 0x01, 0x01, 6, 4, 0x33, 0x00, 0, 0,        // IT 1: USB streaming, 4 canales (L R Ls Rs)
    12, 0x24, 0x06, 2, 1, 1, 0x03, 0, 0, 0, 0, 0,                 // FU 2: mute + volumen general
    9, 0x24, 0x03, 3, 0x01, 0x03, 4, 2, 0,                        // OT 3: parlante
    12, 0x24, 0x02, 4, 0x02, 0x04, 3, 2, 0x03, 0x00, 0, 0,        // IT 4: micrófono de auricular, 2 canales
    9, 0x24, 0x06, 5, 4, 1, 0x03, 0, 0,                           // FU 5: mute + volumen general
    9, 0x24, 0x03, 6, 0x01, 0x01, 1, 5, 0,                        // OT 6: USB streaming

    // ---- Interfaz 1: parlante (4 canales, 16 bits, 48 kHz) ----
    9, 0x04, 1, 0, 0, 0x01, 0x02, 0x00, 0,                        // alt 0: sin ancho de banda
    9, 0x04, 1, 1, 1, 0x01, 0x02, 0x00, 0,                        // alt 1: transmitiendo
    7, 0x24, 0x01, 1, 1, 0x01, 0x00,                              // AS general: terminal 1, PCM
    11, 0x24, 0x02, 0x01, 4, 2, 16, 1, 0x80, 0xBB, 0x00,          // formato tipo I: 4 canales, 48000 Hz
    9, 0x05, 0x01, 0x09, 0x88, 0x01, 4, 0, 0,                     // EP 0x01 OUT isócrono adaptativo, 392 bytes, 1 ms
    7, 0x25, 0x01, 0x00, 0, 0x00, 0x00,

    // ---- Interfaz 2: micrófono (2 canales, 16 bits, 48 kHz) ----
    9, 0x04, 2, 0, 0, 0x01, 0x02, 0x00, 0,
    9, 0x04, 2, 1, 1, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 6, 1, 0x01, 0x00,
    11, 0x24, 0x02, 0x01, 2, 2, 16, 1, 0x80, 0xBB, 0x00,
    9, 0x05, 0x82, 0x05, 0xC4, 0x00, 4, 0, 0,                     // EP 0x82 IN isócrono asíncrono, 196 bytes, 1 ms
    7, 0x25, 0x01, 0x00, 0, 0x00, 0x00,

    // ---- Interfaz 3: HID ----
    9, 0x04, 3, 0, 2, 0x03, 0x00, 0x00, 0,
    9, 0x21, 0x11, 0x01, 0x00, 1, 0x22, 0x11, 0x01,               // HID 1.11, reporte de 273 bytes
    7, 0x05, 0x84, 0x03, 64, 0, 6,                                // EP 0x84 IN interrupción, 4 ms
    7, 0x05, 0x03, 0x03, 64, 0, 6,                                // EP 0x03 OUT interrupción
};
static_assert(sizeof(kConfigDescriptor) == 227, "wTotalLength debe ser 227 como en el mando real");
constexpr size_t kHidDescriptorOffset = 227 - 32 + 9;

inline std::vector<uint8_t> usbString(const char* s) {
    std::vector<uint8_t> d{0, 0x03};
    for (const char* p = s; *p; ++p) { d.push_back(uint8_t(*p)); d.push_back(0); }
    d[0] = uint8_t(d.size());
    return d;
}

// ---------------------------------------------------------------- traducción ----

// Bluetooth 0x31 (modo completo) -> USB 0x01. Los datos son los mismos, corridos un byte.
inline bool btInputToUsb(const uint8_t* bt, size_t len, uint8_t usb[64]) {
    if (len < 65 || bt[0] != 0x31) return false;
    usb[0] = 0x01;
    std::memcpy(usb + 1, bt + 2, 63);
    return true;
}

// USB 0x02 (ID + 47 bytes) -> Bluetooth 0x31 (78 bytes con CRC).
inline bool usbOutputToBt(const uint8_t* usb, size_t len, uint8_t seq, uint8_t bt[kMainReportSize]) {
    if (len < 2 || usb[0] != 0x02) return false;
    std::memset(bt, 0, kMainReportSize);
    bt[0] = 0x31;
    bt[1] = uint8_t((seq & 0x0F) << 4);
    bt[2] = 0x10;
    std::memcpy(bt + 3, usb + 1, std::min<size_t>(len - 1, 47));
    uint32_t crc = crc32_bt(bt, kMainReportSize - 4);
    bt[74] = uint8_t(crc); bt[75] = uint8_t(crc >> 8); bt[76] = uint8_t(crc >> 16); bt[77] = uint8_t(crc >> 24);
    return true;
}

// ------------------------------------------------------------------ dispositivo ----

class VirtualDualSense : public usbip::Device {
public:
    struct Callbacks {
        // Pedir un feature report al mando real. 'out' debe incluir el Report ID.
        std::function<bool(uint8_t id, std::vector<uint8_t>& out)> getFeature;
        // El juego envió un reporte de salida USB 0x02 (gatillos, luces, vibración).
        std::function<void(const uint8_t* usb, size_t len)> output;
        // El juego quiso escribir un feature report (se informa, no se reenvía).
        std::function<void(uint8_t id, const uint8_t* data, size_t len)> setFeature;
        std::function<void(const std::string&)> log;
        // Audio que el juego manda al parlante del mando: frames de 4 canales int16,
        // entregado a ritmo real (1 ms por paquete). Los canales 3 y 4 son la háptica.
        std::function<void(const int16_t* frames, size_t count)> audioOut;
    };

    explicit VirtualDualSense(Callbacks cb) : cb_(std::move(cb)) {
        isoThread_ = std::thread([this] { isoLoop(); });
    }
    ~VirtualDualSense() override {
        {
            std::lock_guard<std::mutex> lk(isoMu_);
            isoStop_ = true;
        }
        isoCv_.notify_all();
        if (isoThread_.joinable()) isoThread_.join();
    }

    usbip::DeviceInfo info() const override {
        usbip::DeviceInfo d;
        d.vid = kVid; d.pid = kPid; d.bcdDevice = 0x0100;
        d.speed = usbip::kSpeedHigh;
        d.interfaces = {{0x01, 0x01, 0x00}, {0x01, 0x02, 0x00}, {0x01, 0x02, 0x00}, {0x03, 0x00, 0x00}};
        return d;
    }

    void attached(usbip::Connection* c) override {
        std::lock_guard<std::mutex> lk(mu_);
        conn_ = c;
        pendingIn_.clear();
        configured_ = false;
        altSpeaker_ = altMic_ = 0;
        std::lock_guard<std::mutex> il(isoMu_);
        isoSpeaker_.clear();
        isoMic_.clear();
    }

    bool unlink(uint32_t seqnum) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto it = pendingIn_.begin(); it != pendingIn_.end(); ++it)
                if (it->seqnum == seqnum) { pendingIn_.erase(it); return true; }
        }
        std::lock_guard<std::mutex> il(isoMu_);
        for (auto* qd : {&isoSpeaker_, &isoMic_})
            for (auto it = qd->begin(); it != qd->end(); ++it)
                if (it->q.seqnum == seqnum) { qd->erase(it); return true; }
        return false;
    }

    void submit(usbip::Connection& c, usbip::Submit&& q) override {
        if (q.ep == 0) { control(c, q); return; }
        if ((q.ep == kEpSpeaker && !q.in()) || (q.ep == kEpMic && q.in())) {
            if (q.numPackets <= 0) { c.retSubmit(q, usbip::kEPIPE, nullptr, 0); return; }
            // El audio se entrega a la háptica apenas llega (no cuando "termina de sonar"):
            // así no se suma la cola que Windows manda por adelantado.
            if (q.ep == kEpSpeaker) deliverAudio(q);
            queueIso(std::move(q));
            return;
        }
        if (q.ep == kEpIn && q.in()) {
            std::lock_guard<std::mutex> lk(mu_);
            if (haveFresh_) {                       // hay un reporte sin entregar: completar ya
                haveFresh_ = false;
                completeIn(c, q, latest_);
            } else {
                pendingIn_.push_back(std::move(q));
            }
            return;
        }
        if (q.ep == kEpOut && !q.in()) {
            ++outputs_;
            if (cb_.output && !q.data.empty()) cb_.output(q.data.data(), q.data.size());
            c.retSubmit(q, 0, nullptr, q.data.size());
            return;
        }
        c.retSubmit(q, usbip::kEPIPE, nullptr, 0);   // endpoint desconocido: stall
    }

    // Llamar con cada reporte de entrada USB (64 bytes) traducido desde Bluetooth.
    void pushInput(const uint8_t usb[64]) {
        std::lock_guard<std::mutex> lk(mu_);
        std::memcpy(latest_, usb, 64);
        haveLatest_ = true;
        if (conn_ && !pendingIn_.empty()) {
            usbip::Submit q = std::move(pendingIn_.front());
            pendingIn_.pop_front();
            haveFresh_ = false;
            completeIn(*conn_, q, latest_);
        } else {
            haveFresh_ = true;
        }
    }

    // Estadísticas
    unsigned outputs() const { return outputs_; }
    unsigned featureRequests() const { return features_; }
    bool configured() const { return configured_; }
    bool speakerStreaming() const { return altSpeaker_ == 1; }
    unsigned audioPackets() const { return audioPackets_; }
    int16_t speakerVolume() const { return volume_[0]; }
    // Cuántos ms de audio tiene Windows en cola por adelantado en el parlante.
    double speakerQueueMs() {
        std::lock_guard<std::mutex> lk(isoMu_);
        if (isoSpeaker_.empty()) return 0.0;
        auto ahead = isoSpeaker_.back().due - Clock::now();
        return std::max(0.0, std::chrono::duration<double, std::milli>(ahead).count());
    }

private:
    void log(const std::string& s) { if (cb_.log) cb_.log(s); }

    // Llamar con mu_ tomado
    void completeIn(usbip::Connection& c, const usbip::Submit& q, const uint8_t* rep) {
        size_t n = std::min<size_t>(64, q.length);
        c.retSubmit(q, 0, rep, n);
        ++inputs_;
    }

    void reply(usbip::Connection& c, const usbip::Submit& q, const uint8_t* d, size_t n) {
        uint16_t wLength = uint16_t(q.setup[6] | (q.setup[7] << 8));
        n = std::min<size_t>({n, wLength, q.length});
        c.retSubmit(q, 0, d, n);
    }
    void stall(usbip::Connection& c, const usbip::Submit& q) { c.retSubmit(q, usbip::kEPIPE, nullptr, 0); }
    void ack(usbip::Connection& c, const usbip::Submit& q) { c.retSubmit(q, 0, nullptr, q.in() ? 0 : q.data.size()); }

    void control(usbip::Connection& c, const usbip::Submit& q) {
        const uint8_t rt = q.setup[0], req = q.setup[1];
        const uint16_t wValue = uint16_t(q.setup[2] | (q.setup[3] << 8));
        const uint8_t type = uint8_t(wValue >> 8), index = uint8_t(wValue);
        const uint8_t reqType = (rt >> 5) & 3;   // 0 estándar, 1 clase

        if (reqType == 0) {
            switch (req) {
            case 0x06:  // GET_DESCRIPTOR
                if (rt == 0x80) {
                    if (type == 0x01) return reply(c, q, kDeviceDescriptor, sizeof(kDeviceDescriptor));
                    if (type == 0x02) return reply(c, q, kConfigDescriptor, sizeof(kConfigDescriptor));
                    if (type == 0x06) return reply(c, q, kQualifierDescriptor, sizeof(kQualifierDescriptor));
                    if (type == 0x03) {
                        std::vector<uint8_t> s;
                        if (index == 0) s = {4, 0x03, 0x09, 0x04};
                        else if (index == 1) s = usbString("Sony Interactive Entertainment");
                        else if (index == 2) s = usbString("DualSense Wireless Controller");
                        else return stall(c, q);
                        return reply(c, q, s.data(), s.size());
                    }
                } else if (rt == 0x81) {
                    if (type == 0x22) return reply(c, q, kUsbReportDescriptor, sizeof(kUsbReportDescriptor));
                    if (type == 0x21) return reply(c, q, kConfigDescriptor + kHidDescriptorOffset, 9);
                }
                return stall(c, q);
            case 0x09:  // SET_CONFIGURATION
                configured_ = (index != 0);
                return ack(c, q);
            case 0x08: { uint8_t v = configured_ ? 1 : 0; return reply(c, q, &v, 1); }  // GET_CONFIGURATION
            case 0x0B: {                                                                 // SET_INTERFACE
                const uint8_t iface = q.setup[4];
                if (iface == kIfSpeaker) altSpeaker_ = uint8_t(index);
                else if (iface == kIfMic) altMic_ = uint8_t(index);
                if (cb_.log && (iface == kIfSpeaker || iface == kIfMic))
                    cb_.log(std::string(iface == kIfSpeaker ? tr("Parlante", "Speaker") : tr("Micrófono", "Microphone")) +
                            (index ? tr(": el juego empezó a transmitir audio", ": the game started streaming audio") : tr(": transmisión detenida", ": streaming stopped")));
                return ack(c, q);
            }
            case 0x0A: {                                                                 // GET_INTERFACE
                const uint8_t iface = q.setup[4];
                uint8_t v = iface == kIfSpeaker ? altSpeaker_.load() : iface == kIfMic ? altMic_.load() : 0;
                return reply(c, q, &v, 1);
            }
            case 0x00: {                                                                 // GET_STATUS
                uint8_t st[2] = {uint8_t(rt == 0x80 ? 1 : 0), 0};  // autoalimentado
                return reply(c, q, st, 2);
            }
            case 0x01: case 0x03: return ack(c, q);                                      // CLEAR/SET_FEATURE
            default: return stall(c, q);
            }
        }

        if (reqType == 1) {
            const uint8_t recipient = rt & 0x1F;
            const uint8_t iface = q.setup[4];
            if (recipient == 1 && iface == kIfHid) return hidClass(c, q, req, type, index);
            if (recipient == 1 && iface == kIfAudioControl) return audioClass(c, q, req);
            if (recipient == 2) return stall(c, q);   // sin controles de endpoint (como el real)
            return stall(c, q);
        }
        stall(c, q);
    }

    void hidClass(usbip::Connection& c, const usbip::Submit& q, uint8_t req, uint8_t type, uint8_t index) {
        switch (req) {
        case 0x0A: return ack(c, q);                                     // SET_IDLE
        case 0x0B: return ack(c, q);                                     // SET_PROTOCOL
        case 0x02: { uint8_t v = 0; return reply(c, q, &v, 1); }         // GET_IDLE
        case 0x03: { uint8_t v = 1; return reply(c, q, &v, 1); }         // GET_PROTOCOL
        case 0x01:                                                       // GET_REPORT
            if (type == 1) {
                uint8_t rep[64] = {0x01, 0x80, 0x80, 0x80, 0x80, 0, 0, 0, 0x08};
                std::lock_guard<std::mutex> lk(mu_);
                if (haveLatest_) std::memcpy(rep, latest_, 64);
                return reply(c, q, rep, 64);
            }
            if (type == 3) return getFeatureReport(c, q, index);
            return stall(c, q);
        case 0x09:                                                       // SET_REPORT
            if (type == 2) {
                ++outputs_;
                if (cb_.output && !q.data.empty()) cb_.output(q.data.data(), q.data.size());
            } else if (type == 3) {
                if (cb_.setFeature) cb_.setFeature(index, q.data.data(), q.data.size());
            }
            return ack(c, q);
        default: return stall(c, q);
        }
    }

    // Peticiones de audio UAC1 a las Feature Units: mute (selector 1) y volumen (selector 2).
    // Se aceptan pero NO se aplican al audio: así el volumen de Windows no afecta la háptica.
    void audioClass(usbip::Connection& c, const usbip::Submit& q, uint8_t req) {
        const uint8_t selector = q.setup[3];
        const uint8_t unit = q.setup[5];
        if (unit != kUnitSpeaker && unit != kUnitMic) return stall(c, q);
        const int u = unit == kUnitSpeaker ? 0 : 1;
        if (selector == 1) {                                             // mute
            if (req == 0x01) { if (!q.data.empty()) mute_[u] = q.data[0]; return ack(c, q); }
            if (req == 0x81) { uint8_t v = mute_[u]; return reply(c, q, &v, 1); }
            return stall(c, q);
        }
        if (selector == 2) {                                             // volumen (1/256 dB)
            int16_t v;
            switch (req) {
            case 0x01:
                if (q.data.size() >= 2) volume_[u] = int16_t(q.data[0] | (q.data[1] << 8));
                return ack(c, q);
            case 0x81: v = volume_[u]; break;              // GET_CUR
            case 0x82: v = int16_t(-100 * 256); break;     // GET_MIN: -100 dB
            case 0x83: v = 0; break;                       // GET_MAX: 0 dB
            case 0x84: v = 256; break;                     // GET_RES: 1 dB
            default: return stall(c, q);
            }
            uint8_t b[2] = {uint8_t(v), uint8_t(uint16_t(v) >> 8)};
            return reply(c, q, b, 2);
        }
        stall(c, q);
    }

    void getFeatureReport(usbip::Connection& c, const usbip::Submit& q, uint8_t id) {
        ++features_;
        size_t usbSize = usbFeatureSize(id);
        if (!usbSize) return stall(c, q);
        std::vector<uint8_t> data;
        if (!cb_.getFeature || !cb_.getFeature(id, data) || data.empty()) {
            log(tr("Feature 0x", "Feature report 0x") + hex(id) + tr(" no disponible en el mando", " not available on the controller"));
            return stall(c, q);
        }
        // Por Bluetooth los últimos 4 bytes de estos reportes son un CRC: por USB no existen.
        data.resize(usbSize, 0);
        data[0] = id;
        if (usbSize > 8) std::fill(data.end() - 4, data.end(), 0);
        reply(c, q, data.data(), data.size());
    }

    // ------------------------------------------------------------ isócronos ----
    // Cada paquete dura 1 ms (alta velocidad, bInterval 4). Los URBs se completan a ritmo
    // real, como lo haría el hardware, para que Windows no mande el audio más rápido.
    using Clock = std::chrono::steady_clock;
    struct IsoUrb { usbip::Submit q; Clock::time_point due; };

    void queueIso(usbip::Submit&& q) {
        std::lock_guard<std::mutex> lk(isoMu_);
        const bool speaker = q.ep == kEpSpeaker;
        auto& qd = speaker ? isoSpeaker_ : isoMic_;
        auto& clk = speaker ? clockSpeaker_ : clockMic_;
        auto now = Clock::now();
        // Si la transmisión estuvo parada, el reloj del dispositivo arranca desde ahora
        if (qd.empty() && clk < now - std::chrono::milliseconds(5)) clk = now;
        clk += std::chrono::milliseconds(q.numPackets);
        qd.push_back({std::move(q), clk});
        isoCv_.notify_all();
    }

    void isoLoop() {
        std::unique_lock<std::mutex> lk(isoMu_);
        while (!isoStop_) {
            Clock::time_point next = Clock::time_point::max();
            for (auto* qd : {&isoSpeaker_, &isoMic_})
                if (!qd->empty()) next = std::min(next, qd->front().due);
            if (next == Clock::time_point::max()) { isoCv_.wait(lk); continue; }
            if (Clock::now() < next) { isoCv_.wait_until(lk, next); continue; }
            for (auto* qd : {&isoSpeaker_, &isoMic_}) {
                while (!qd->empty() && qd->front().due <= Clock::now()) {
                    IsoUrb u = std::move(qd->front());
                    qd->pop_front();
                    lk.unlock();
                    completeIso(u.q);
                    lk.lock();
                }
            }
        }
    }

    void deliverAudio(const usbip::Submit& q) {
        std::vector<int16_t> frames;
        const size_t n = size_t(q.numPackets);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* d = q.isoDesc.data() + i * 16;
            uint32_t off = usbip::get32(d), len = usbip::get32(d + 4);
            if (off + len > q.data.size()) continue;
            const size_t fr = len / (2 * kSpeakerChannels);
            const uint8_t* p = q.data.data() + off;
            for (size_t k = 0; k < fr * kSpeakerChannels; ++k)
                frames.push_back(int16_t(p[k * 2] | (p[k * 2 + 1] << 8)));
        }
        audioPackets_ += unsigned(n);
        if (!frames.empty() && cb_.audioOut) cb_.audioOut(frames.data(), frames.size() / kSpeakerChannels);
    }

    void completeIso(const usbip::Submit& q) {
        const size_t n = size_t(q.numPackets);
        std::vector<uint8_t> desc(n * 16, 0);
        std::vector<uint8_t> inData;
        size_t total = 0;
        const bool speaker = q.ep == kEpSpeaker;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* d = q.isoDesc.data() + i * 16;
            uint32_t off = usbip::get32(d), len = usbip::get32(d + 4);
            uint32_t actual = len;
            if (speaker) {
                if (off + len > q.data.size()) actual = 0;
            } else {
                // Micrófono: 48 frames de silencio por milisegundo (empaquetados, como USB/IP)
                actual = std::min<uint32_t>(len, (kAudioRate / 1000) * kMicChannels * 2);
                inData.resize(inData.size() + actual, 0);
            }
            usbip::put32(desc.data() + i * 16 + 0, off);
            usbip::put32(desc.data() + i * 16 + 4, len);
            usbip::put32(desc.data() + i * 16 + 8, actual);
            usbip::put32(desc.data() + i * 16 + 12, 0);
            total += actual;
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (conn_) conn_->retSubmit(q, 0, inData.data(), speaker ? total : inData.size(), &desc);
    }

    static std::string hex(uint8_t v) {
        const char* d = "0123456789ABCDEF";
        return std::string{d[v >> 4], d[v & 15]};
    }

    Callbacks cb_;
    std::mutex mu_;
    usbip::Connection* conn_ = nullptr;
    std::deque<usbip::Submit> pendingIn_;
    uint8_t latest_[64] = {};
    bool haveLatest_ = false, haveFresh_ = false;
    std::atomic<bool> configured_{false};
    std::atomic<uint8_t> altSpeaker_{0}, altMic_{0};
    uint8_t mute_[2] = {0, 0};
    int16_t volume_[2] = {0, 0};
    std::atomic<unsigned> audioPackets_{0};
    std::mutex isoMu_;
    std::condition_variable isoCv_;
    std::deque<IsoUrb> isoSpeaker_, isoMic_;
    Clock::time_point clockSpeaker_{}, clockMic_{};
    bool isoStop_ = false;
    std::thread isoThread_;
    std::atomic<unsigned> outputs_{0}, features_{0}, inputs_{0};
};

}  // namespace ds5

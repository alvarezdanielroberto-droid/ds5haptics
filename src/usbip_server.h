// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// usbip_server.h — Servidor USB/IP mínimo para exportar un dispositivo USB virtual.
//
// El cliente (usbip-win2 en Windows) se conecta por TCP, importa el dispositivo y
// Windows lo trata como si estuviera enchufado por USB, con sus propios drivers.
// Protocolo: https://docs.kernel.org/usb/usbip_protocol.html (todo en big-endian).
//
// Portable: Winsock en Windows, sockets POSIX en Linux (para los tests).
// En Windows, incluir este archivo ANTES de <windows.h> (o incluir <winsock2.h> antes).
#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include "i18n.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace usbip {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
inline void closeSock(sock_t s) { closesocket(s); }
#else
using sock_t = int;
constexpr sock_t kBadSock = -1;
inline void closeSock(sock_t s) { ::close(s); }
#endif

// Códigos de error de Linux que viajan en el protocolo
constexpr int32_t kEPIPE = -32;       // stall
constexpr int32_t kECONNRESET = -104; // URB cancelado
constexpr int32_t kESHUTDOWN = -108;

constexpr uint32_t kSpeedFull = 2, kSpeedHigh = 3;

inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline uint16_t get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

struct InterfaceInfo { uint8_t cls, subclass, protocol; };

struct DeviceInfo {
    std::string path = "/sys/devices/platform/ds5haptics/usb1/1-1";
    std::string busid = "1-1";
    uint32_t busnum = 1, devnum = 1, speed = kSpeedHigh;
    uint16_t vid = 0, pid = 0, bcdDevice = 0;
    uint8_t devClass = 0, devSubClass = 0, devProtocol = 0;
    uint8_t configValue = 1, numConfigs = 1;
    std::vector<InterfaceInfo> interfaces;
};

// Petición USB que llega del cliente (USBIP_CMD_SUBMIT)
struct Submit {
    uint32_t seqnum = 0, devid = 0, direction = 0, ep = 0;
    uint32_t flags = 0, length = 0, interval = 0;
    int32_t startFrame = 0;
    int32_t numPackets = -1;          // -1 = no isócrono
    uint8_t setup[8] = {};
    std::vector<uint8_t> data;        // datos OUT
    std::vector<uint8_t> isoDesc;     // descriptores isócronos crudos (16 bytes c/u)
    bool in() const { return direction == 1; }
};

// Envía respuestas al cliente; seguro para usar desde varios hilos.
class Connection {
public:
    explicit Connection(sock_t s) : s_(s) {}

    bool sendAll(const uint8_t* p, size_t n) {
        while (n > 0) {
            int r = ::send(s_, reinterpret_cast<const char*>(p), int(n), 0);
            if (r <= 0) { alive_ = false; return false; }
            p += r; n -= size_t(r);
        }
        return true;
    }

    // USBIP_RET_SUBMIT. 'data' solo se envía si la petición era IN.
    bool retSubmit(const Submit& req, int32_t status, const uint8_t* data, size_t len,
                   const std::vector<uint8_t>* isoDesc = nullptr, uint32_t errorCount = 0) {
        uint8_t h[48] = {};
        put32(h + 0, 3);
        put32(h + 4, req.seqnum);
        put32(h + 20, uint32_t(status));
        put32(h + 24, uint32_t(len));
        put32(h + 28, uint32_t(req.numPackets >= 0 ? req.startFrame : 0));
        put32(h + 32, uint32_t(req.numPackets));
        put32(h + 36, errorCount);
        std::lock_guard<std::mutex> lk(mu_);
        if (!alive_) return false;
        if (!sendAll(h, sizeof(h))) return false;
        if (req.in() && len && !sendAll(data, len)) return false;
        if (isoDesc && !isoDesc->empty() && !sendAll(isoDesc->data(), isoDesc->size())) return false;
        return true;
    }

    // USBIP_RET_UNLINK
    bool retUnlink(uint32_t seqnum, int32_t status) {
        uint8_t h[48] = {};
        put32(h + 0, 4);
        put32(h + 4, seqnum);
        put32(h + 20, uint32_t(status));
        std::lock_guard<std::mutex> lk(mu_);
        if (!alive_) return false;
        return sendAll(h, sizeof(h));
    }

    bool alive() const { return alive_; }

private:
    sock_t s_;
    std::mutex mu_;
    std::atomic<bool> alive_{true};
};

// Interfaz del dispositivo virtual.
class Device {
public:
    virtual ~Device() = default;
    virtual DeviceInfo info() const = 0;
    // Debe responder con conn.retSubmit() ahora o más tarde (salvo que se cancele).
    virtual void submit(Connection& conn, Submit&& req) = 0;
    // true si la petición estaba pendiente y se canceló (no se responderá).
    virtual bool unlink(uint32_t seqnum) = 0;
    virtual void attached(Connection* conn) = 0;   // conn = nullptr al desconectar
};

class Server {
public:
    using LogFn = std::function<void(const std::string&)>;

    ~Server() { stop(); }

    // Escucha solo en 127.0.0.1 (no expone el mando a la red).
    bool start(Device& dev, uint16_t port, LogFn log) {
        dev_ = &dev;
        log_ = std::move(log);
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == kBadSock) return false;
        int one = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(listen_, 4) != 0) {
            closeSock(listen_);
            listen_ = kBadSock;
            return false;
        }
        running_ = true;
        thread_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (listen_ != kBadSock) {
#ifdef _WIN32
            ::shutdown(listen_, SD_BOTH);
#else
            ::shutdown(listen_, SHUT_RDWR);
#endif
            closeSock(listen_);
            listen_ = kBadSock;
        }
        sock_t c = client_.exchange(kBadSock);
        if (c != kBadSock) {
#ifdef _WIN32
            ::shutdown(c, SD_BOTH);
#else
            ::shutdown(c, SHUT_RDWR);
#endif
        }
        if (thread_.joinable()) thread_.join();
    }

    bool imported() const { return imported_; }

    // Cambia el dispositivo que se exporta. Vale para la próxima importación:
    // llamar después a dropClient() para que el cliente suelte el dispositivo anterior.
    void setDevice(Device& dev) { dev_ = &dev; }

    // Corta la conexión con el cliente actual (Windows ve que el dispositivo se desenchufó).
    void dropClient() {
        sock_t c = client_.load();
        if (c != kBadSock) {
#ifdef _WIN32
            ::shutdown(c, SD_BOTH);
#else
            ::shutdown(c, SHUT_RDWR);
#endif
        }
    }

private:
    void log(const std::string& s) { if (log_) log_(s); }

    static bool recvAll(sock_t s, uint8_t* p, size_t n) {
        while (n > 0) {
            int r = ::recv(s, reinterpret_cast<char*>(p), int(n), 0);
            if (r <= 0) return false;
            p += r; n -= size_t(r);
        }
        return true;
    }

    // Bloque de dispositivo común a OP_REP_DEVLIST y OP_REP_IMPORT (312 bytes)
    static void deviceBlock(const DeviceInfo& d, std::vector<uint8_t>& out) {
        size_t o = out.size();
        out.resize(o + 0x138, 0);
        uint8_t* p = out.data() + o;
        std::memcpy(p, d.path.c_str(), std::min<size_t>(d.path.size(), 255));
        std::memcpy(p + 256, d.busid.c_str(), std::min<size_t>(d.busid.size(), 31));
        put32(p + 0x120, d.busnum);
        put32(p + 0x124, d.devnum);
        put32(p + 0x128, d.speed);
        put16(p + 0x12C, d.vid);
        put16(p + 0x12E, d.pid);
        put16(p + 0x130, d.bcdDevice);
        p[0x132] = d.devClass;
        p[0x133] = d.devSubClass;
        p[0x134] = d.devProtocol;
        p[0x135] = d.configValue;
        p[0x136] = d.numConfigs;
        p[0x137] = uint8_t(d.interfaces.size());
    }

    void acceptLoop() {
        while (running_) {
            sock_t c = ::accept(listen_, nullptr, nullptr);
            if (c == kBadSock) { if (!running_) break; continue; }
            int one = 1;
            ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
            client_ = c;
            handleClient(c);
            client_ = kBadSock;
            closeSock(c);
        }
    }

    void handleClient(sock_t s) {
        uint8_t op[8];
        if (!recvAll(s, op, 8)) return;
        uint16_t code = get16(op + 2);
        Device* dev = dev_.load();   // el dispositivo de esta conexión (puede cambiarse para la próxima)
        const DeviceInfo d = dev->info();

        if (code == 0x8005) {                         // OP_REQ_DEVLIST
            std::vector<uint8_t> r(12, 0);
            put16(r.data(), 0x0111);
            put16(r.data() + 2, 0x0005);
            put32(r.data() + 8, 1);
            deviceBlock(d, r);
            for (auto& i : d.interfaces) { r.push_back(i.cls); r.push_back(i.subclass); r.push_back(i.protocol); r.push_back(0); }
            Connection(s).sendAll(r.data(), r.size());
            return;
        }
        if (code != 0x8003) { log(ds5::tr("USB/IP: comando desconocido ", "USB/IP: unknown command ") + std::to_string(code)); return; }

        uint8_t busid[32];                            // OP_REQ_IMPORT
        if (!recvAll(s, busid, 32)) return;
        busid[31] = 0;
        bool ok = d.busid == reinterpret_cast<char*>(busid);
        std::vector<uint8_t> r(8, 0);
        put16(r.data(), 0x0111);
        put16(r.data() + 2, 0x0003);
        put32(r.data() + 4, ok ? 0 : 1);
        if (ok) {
            // OP_REP_IMPORT lleva el bloque sin la lista de interfaces
            deviceBlock(d, r);
        }
        Connection conn(s);
        if (!conn.sendAll(r.data(), r.size()) || !ok) return;

        log(ds5::tr("USB/IP: dispositivo importado por el cliente", "USB/IP: device imported by the client"));
        imported_ = true;
        dev->attached(&conn);
        urbLoop(s, conn, dev);
        dev->attached(nullptr);
        imported_ = false;
        log(ds5::tr("USB/IP: cliente desconectado", "USB/IP: client disconnected"));
    }

    void urbLoop(sock_t s, Connection& conn, Device* dev) {
        uint8_t h[48];
        while (running_ && recvAll(s, h, 48)) {
            uint32_t cmd = get32(h);
            if (cmd == 1) {                           // USBIP_CMD_SUBMIT
                Submit q;
                q.seqnum = get32(h + 4);
                q.devid = get32(h + 8);
                q.direction = get32(h + 12);
                q.ep = get32(h + 16);
                q.flags = get32(h + 20);
                q.length = get32(h + 24);
                q.startFrame = int32_t(get32(h + 28));
                q.numPackets = int32_t(get32(h + 32));
                q.interval = get32(h + 36);
                std::memcpy(q.setup, h + 40, 8);
                if (q.length > 16 * 1024 * 1024) break;   // protección
                if (!q.in() && q.length) {
                    q.data.resize(q.length);
                    if (!recvAll(s, q.data.data(), q.length)) break;
                }
                if (q.numPackets > 0) {
                    q.isoDesc.resize(size_t(q.numPackets) * 16);
                    if (!recvAll(s, q.isoDesc.data(), q.isoDesc.size())) break;
                }
                dev->submit(conn, std::move(q));
            } else if (cmd == 2) {                    // USBIP_CMD_UNLINK
                uint32_t seq = get32(h + 4);
                uint32_t target = get32(h + 20);
                bool removed = dev->unlink(target);
                conn.retUnlink(seq, removed ? kECONNRESET : 0);
            } else {
                log(ds5::tr("USB/IP: comando de URB desconocido ", "USB/IP: unknown URB command ") + std::to_string(cmd));
                break;
            }
            if (!conn.alive()) break;
        }
    }

    std::atomic<Device*> dev_{nullptr};
    LogFn log_;
    sock_t listen_ = kBadSock;
    std::atomic<sock_t> client_{kBadSock};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> imported_{false};
};

}  // namespace usbip

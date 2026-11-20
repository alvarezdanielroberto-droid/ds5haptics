// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// dsx_udp.h — EXPERIMENTAL: escucha en 127.0.0.1:6969 (UDP) los mensajes de los mods
// hechos para DSX. Solo acepta mensajes de la misma PC. En Windows necesita Winsock ya
// iniciado. Portable (sockets POSIX en Linux, para los tests).
#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>

namespace ds5 {

class DsxUdp {
#ifdef _WIN32
    using sock_t = SOCKET;
    static constexpr sock_t kBad = INVALID_SOCKET;
    static void closeSock(sock_t s) { closesocket(s); }
    using addrlen_t = int;
#else
    using sock_t = int;
    static constexpr sock_t kBad = -1;
    static void closeSock(sock_t s) { ::close(s); }
    using addrlen_t = socklen_t;
#endif

public:
    // Recibe el mensaje y devuelve la respuesta (vacía = no responder)
    using Handler = std::function<std::string(const char*, size_t)>;

    ~DsxUdp() { stop(); }

    bool start(unsigned short port, Handler h) {
        stop();
        sock_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kBad) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
        DWORD tmo = 200;   // para poder cerrar el hilo
#else
        timeval tmo{0, 200000};
#endif
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { closeSock(s); return false; }
        sock_ = s;
        handler_ = std::move(h);
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (sock_ != kBad) { closeSock(sock_); sock_ = kBad; }
    }

    bool running() const { return running_; }

private:
    void run() {
        char buf[8192];
        while (running_) {
            sockaddr_in from{};
            addrlen_t fl = sizeof(from);
            int n = int(::recvfrom(sock_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl));
            if (n <= 0) continue;   // tiempo agotado o error pasajero (p. ej. el mod se cerró)
            std::string reply = handler_ ? handler_(buf, size_t(n)) : std::string();
            if (!reply.empty())
                ::sendto(sock_, reply.data(), int(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), fl);
        }
    }

    sock_t sock_ = kBad;
    Handler handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace ds5

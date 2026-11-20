// SPDX-License-Identifier: MPL-2.0
// Pruebas del soporte experimental de mods para DSX: JSON, efectos de gatillo y UDP.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "dsx_protocol.h"
#include "dsx_udp.h"

static int fails = 0;
#define CHECK(c, ...)                              \
    do {                                           \
        if (!(c)) {                                \
            std::printf("FALLA: " __VA_ARGS__);    \
            std::printf("  (línea %d)\n", __LINE__); \
            ++fails;                               \
        }                                          \
    } while (0)

using namespace ds5::dsx;

static bool parse(const char* s, std::vector<Instruction>& v) { return Json::parse(s, std::strlen(s), v); }

int main() {
    // ---------------- JSON ----------------
    {
        std::vector<Instruction> v;
        // El mensaje que manda el mod de RDR2 al cerrarse (los dos gatillos a "Normal")
        CHECK(parse("{\"instructions\":[{\"type\":1,\"parameters\":[0,1,0]},{\"type\":1,\"parameters\":[0,2,0]}]}", v), "mensaje de cierre");
        CHECK(v.size() == 2 && v[0].type == 1 && v[0].params.size() == 3 && v[1].params[1] == 2, "dos instrucciones");

        CHECK(parse(" { \"instructions\" : [ { \"parameters\" : [ 0 , 255 , 128.0 , 0 ] , \"type\" : 2 } ] } \n", v), "espacios y orden");
        CHECK(v.size() == 1 && v[0].type == 2 && v[0].params[2] == 128, "luz");

        // Otros campos, cadenas, booleanos y objetos que no importan
        CHECK(parse("{\"x\":\"a\\\"b\",\"instructions\":[{\"type\":1,\"note\":{\"k\":[1,2]},\"parameters\":[0,2,true,\"s\",null]}],\"z\":false}", v), "campos extra");
        CHECK(v.size() == 1 && v[0].params.size() == 5 && v[0].params[2] == 1 && v[0].params[3] == 0, "valores raros");

        CHECK(parse("{\"instructions\":[]}", v) && v.empty(), "vacío");
        CHECK(!parse("{\"instructions\":[{\"type\":1,\"parameters\":[0,2,", v), "cortado");
        CHECK(!parse("hola", v), "no es JSON");
        CHECK(!parse("", v), "nada");
        std::string deep(100, '[');
        CHECK(!parse(("{\"a\":" + deep).c_str(), v), "anidado de más");
    }

    // ---------------- efectos ----------------
    {
        Effect e;
        translate(kNormal, {}, e);
        CHECK(e[0] == 0x05 && e[1] == 0 && e[5] == 0, "normal = sin efecto");

        translate(kResistance, {3, 8}, e);   // desde la zona 3, fuerza máxima
        uint16_t act = uint16_t(e[1] | e[2] << 8);
        uint32_t force = uint32_t(e[3]) | uint32_t(e[4]) << 8 | uint32_t(e[5]) << 16 | uint32_t(e[6]) << 24;
        CHECK(e[0] == 0x21 && act == 0x3F8, "resistencia: zonas 3..9 (%04X)", act);
        CHECK(((force >> 9) & 7) == 7 && ((force >> 27) & 7) == 7 && (force & 0x1FF) == 0, "resistencia: fuerza 7 en cada zona (%08X)", force);

        translate(kSemiAutomaticGun, {2, 6, 8}, e);
        CHECK(e[0] == 0x25 && (e[1] | e[2] << 8) == ((1 << 2) | (1 << 6)) && e[3] == 7, "arma semiautomática");

        translate(kSemiAutomaticGun, {9, 1, 4}, e);   // fuera de rango: se acomoda
        CHECK(e[0] == 0x25 && (e[1] | e[2] << 8) == ((1 << 7) | (1 << 8)) && e[3] == 3, "arma: rangos corregidos");

        translate(kBow, {1, 4, 6, 3}, e);
        CHECK(e[0] == 0x22 && (e[1] | e[2] << 8) == ((1 << 1) | (1 << 4)) && e[3] == (5 | (2 << 3)), "arco");

        translate(kAutomaticGun, {0, 8, 15}, e);
        CHECK(e[0] == 0x26 && e[9] == 15 && (e[1] | e[2] << 8) == 0x3FF, "automática = vibración");

        translate(kGalloping, {0, 9, 2, 4, 10}, e);
        CHECK(e[0] == 0x23 && e[3] == (4 | (2 << 3)) && e[4] == 10, "galope");

        translate(kMachine, {1, 9, 7, 3, 20, 5}, e);
        CHECK(e[0] == 0x27 && e[3] == (7 | (3 << 3)) && e[4] == 20 && e[5] == 5, "máquina");

        for (int m : {kVerySoft, kSoft, kMedium, kHard, kVeryHard, kHardest, kRigid}) {
            translate(m, {}, e);
            CHECK(e[0] == 0x21, "modo simple %d = resistencia", m);
        }
        translate(kVibrateTrigger, {}, e);
        CHECK(e[0] == 0x26 && e[9] > 0, "vibrar");
        translate(kGameCube, {}, e);
        CHECK(e[0] == 0x25, "gamecube");
        translate(kCustomTriggerValue, {1, 0, 255, 0, 0, 0, 0, 0}, e);
        CHECK(e[0] == 0x01 && e[2] == 255, "valor a medida (rígido)");
        translate(99, {}, e);
        CHECK(e[0] == 0x05, "modo desconocido = sin efecto");

        // Fuerza 0 = sin efecto
        translate(kResistance, {0, 0}, e);
        CHECK(e[0] == 0x05, "fuerza 0");
    }

    // ---------------- reportes ----------------
    {
        std::vector<Instruction> v;
        parse("{\"instructions\":[{\"type\":1,\"parameters\":[0,1,13,2,5]},{\"type\":1,\"parameters\":[0,2,16,2,6,8]},"
              "{\"type\":2,\"parameters\":[0,10,20,30]},{\"type\":3,\"parameters\":[0,1]},{\"type\":1,\"parameters\":[0,5,13]}]}",
              v);
        Result r = toReports(v);
        CHECK(r.reports.size() == 3 && r.triggers == 2 && r.lights == 1 && r.ignored == 2, "tres reportes (%zu, %u ignorados)", r.reports.size(), r.ignored);
        if (r.reports.size() == 3) {
            const auto& L = r.reports[0];
            const auto& R = r.reports[1];
            const auto& C = r.reports[2];
            CHECK(L.size() == 48 && L[0] == 0x02 && L[1] == 0x08 && L[22] == 0x21 && L[11] == 0, "L2 en los bytes 22..32");
            CHECK(R[1] == 0x04 && R[11] == 0x25 && R[22] == 0, "R2 en los bytes 11..21");
            CHECK(C[1] == 0 && C[2] == 0x04 && C[45] == 10 && C[46] == 20 && C[47] == 30, "color");
        }
        Result z = toReports({Instruction{7, {}}});
        CHECK(z.reports.size() == 1 && z.reports[0][1] == 0x0C && z.reports[0][11] == 0x05 && z.reports[0][22] == 0x05, "volver a lo normal");
        CHECK(serverResponse(80, true).find("\"BatteryLevel\":80") != std::string::npos, "respuesta");
    }

    // ---------------- UDP ----------------
    {
        const unsigned short port = 46969;   // otro puerto, por si algo usa el 6969
        std::atomic<int> got{0};
        ds5::DsxUdp srv;
        bool ok = srv.start(port, [&](const char* d, size_t n) {
            std::vector<Instruction> v;
            if (Json::parse(d, n, v) && v.size() == 1) ++got;
            return serverResponse(50, true);
        });
        CHECK(ok, "abrir el puerto");
        ds5::DsxUdp second;
        CHECK(!second.start(port, nullptr), "el puerto ocupado se detecta");

        int c = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        timeval tv{1, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const char* msg = "{\"instructions\":[{\"type\":2,\"parameters\":[0,1,2,3]}]}";
        sendto(c, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        char buf[512] = {};
        ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
        CHECK(n > 0 && std::strstr(buf, "isControllerConnected") != nullptr, "el servidor responde");
        CHECK(got == 1, "llegó el mensaje");
        close(c);
        auto t0 = std::chrono::steady_clock::now();
        srv.stop();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2), "se cierra rápido");
        CHECK(!srv.running(), "detenido");
    }

    if (fails) { std::printf("%d prueba(s) fallaron\n", fails); return 1; }
    std::printf("OK: mods para DSX (JSON, gatillos, reportes y UDP) correctos\n");
    return 0;
}

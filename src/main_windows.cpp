// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// ds5haptics — el DualSense por Bluetooth en Windows como si estuviera por cable:
// háptica, gatillos adaptativos, parlante, modos PS4/Xbox, perfiles y bandeja del sistema.
//
// Envía audio de háptica (WAV, crudo s8 o señales de prueba) al mando como
// reportes HID 0x32 por el dongle Bluetooth común, sin cable USB.
//
// Basado en la investigación de SAxense — https://apps.sdore.me/SAxense
//
// Compilar (MSVC, "x64 Native Tools Command Prompt"):  build.bat
// Compilar (MinGW-w64):  g++ -std=c++17 -O2 -municode -Isrc src/main_windows.cpp -o ds5haptics.exe -lhid -lsetupapi -lwinmm -lole32 -lws2_32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Winsock tiene que incluirse antes que <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2   // K32EnumProcessModulesEx / K32GetModuleBaseNameW de kernel32
#endif
#include <psapi.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <mmsystem.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cctype>

#include <cstring>
#include <memory>

#include "ds5_bt_haptics.h"
#include "haptic_audio.h"
#include "haptic_live.h"
#include "wasapi_capture.h"
#include "usbip_server.h"
#include "virtual_dualsense.h"
#include "virtual_pads.h"
#include "profiles.h"
#include "tray_ui.h"
#include "i18n.h"
#include "audio_default.h"
#include "voice_tts.h"
#include "voice_queue.h"
#include "gestures.h"
#include "sim_fx.h"
#include "process_capture.h"

using ds5::tr;
using ds5::tra;

#ifndef DS5_VERSION
#define DS5_VERSION "dev"   // GitHub Actions lo reemplaza por la versión publicada (ej. v1.0.0)
#endif
#ifdef DS5_WITH_OPUS
#include "speaker_bt.h"
#endif

#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <cwchar>

#ifdef _MSC_VER
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static std::atomic<bool> g_stop{false};

static BOOL WINAPI onCtrl(DWORD) { g_stop = true; return TRUE; }

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::string lastErr() {
    DWORD e = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string s = "error " + std::to_string(e);
    if (msg) { s += ": " + narrow(msg); LocalFree(msg); }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// ------------------------------------------------------------------ HID ----

struct DeviceInfo {
    std::wstring path;
    USHORT vid = 0, pid = 0;
    std::wstring product;
    bool bluetooth = false;
    USHORT inLen = 0, outLen = 0, featLen = 0;
    std::wstring instanceId;   // ruta de instancia (la que usa HidHide)
};

static bool isDualSense(USHORT vid, USHORT pid) {
    return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2);  // DualSense / DualSense Edge
}

static std::vector<DeviceInfo> enumerateDualSense() {
    std::vector<DeviceInfo> found;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devs = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return found;

    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<uint8_t> raw(need);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifd, det, need, nullptr, &devInfo)) continue;
        wchar_t instBuf[512] = {};
        std::wstring instanceId;
        if (SetupDiGetDeviceInstanceIdW(devs, &devInfo, instBuf, 512, nullptr)) instanceId = instBuf;

        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr) && isDualSense(attr.VendorID, attr.ProductID)) {
            DeviceInfo di;
            di.path = det->DevicePath;
            di.instanceId = instanceId;
            di.vid = attr.VendorID;
            di.pid = attr.ProductID;
            wchar_t name[128] = {};
            if (HidD_GetProductString(h, name, sizeof(name))) di.product = name;
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (HidD_GetPreparsedData(h, &pp)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                    di.inLen = caps.InputReportByteLength;
                    di.outLen = caps.OutputReportByteLength;
                    di.featLen = caps.FeatureReportByteLength;
                }
                HidD_FreePreparsedData(pp);
            }
            std::wstring low = di.path;
            std::transform(low.begin(), low.end(), low.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
            // GUID del servicio HID de Bluetooth clásico, o reportes de entrada > 64 bytes
            di.bluetooth = low.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos || di.inLen > 64;
            found.push_back(di);
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

class HidDevice {
public:
    ~HidDevice() { close(); }

    bool open(const DeviceInfo& di) {
        info_ = di;
        h_ = CreateFileW(di.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        wev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        rev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return wev_ && rev_;
    }

    void close() {
        if (h_ != INVALID_HANDLE_VALUE) { CancelIo(h_); CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
        if (wev_) { CloseHandle(wev_); wev_ = nullptr; }
        if (rev_) { CloseHandle(rev_); rev_ = nullptr; }
    }

    // Pedir el reporte de calibración (feature 0x05) pasa el mando del modo
    // Bluetooth "simple" (reporte 0x01) al modo completo (reporte 0x31).
    bool enableFullMode() {
        std::vector<uint8_t> buf(std::max<USHORT>(info_.featLen, 64), 0);
        buf[0] = 0x05;
        bool ok = HidD_GetFeature(h_, buf.data(), ULONG(buf.size())) != FALSE;
        // Windows guarda en cola reportes de entrada viejos (modo simple): descartarlos.
        HidD_FlushQueue(h_);
        return ok;
    }

    // Lee un reporte de entrada y devuelve su ID (0x01 = simple, 0x31 = completo), o -1.
    int readReportId(DWORD timeoutMs) {
        std::vector<uint8_t> buf(std::max<USHORT>(info_.inLen, 80), 0);
        OVERLAPPED ov{};
        ov.hEvent = rev_;
        ResetEvent(rev_);
        DWORD n = 0;
        if (!ReadFile(h_, buf.data(), DWORD(info_.inLen ? info_.inLen : buf.size()), nullptr, &ov) &&
            GetLastError() != ERROR_IO_PENDING) return -1;
        if (WaitForSingleObject(rev_, timeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(h_, &ov);
            GetOverlappedResult(h_, &ov, &n, TRUE);
            return -1;
        }
        if (!GetOverlappedResult(h_, &ov, &n, FALSE) || n == 0) return -1;
        return buf[0];
    }

    // Lee un reporte de entrada completo. Devuelve su largo, 0 si no llegó nada a tiempo, -1 si hubo error.
    int readReport(std::vector<uint8_t>& buf, DWORD timeoutMs) {
        size_t want = info_.inLen ? info_.inLen : 80;
        if (buf.size() < want) buf.resize(want);
        OVERLAPPED ov{};
        ov.hEvent = rev_;
        ResetEvent(rev_);
        DWORD n = 0;
        if (!ReadFile(h_, buf.data(), DWORD(want), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) return -1;
        if (WaitForSingleObject(rev_, timeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(h_, &ov);
            GetOverlappedResult(h_, &ov, &n, TRUE);
            return 0;
        }
        if (!GetOverlappedResult(h_, &ov, &n, FALSE)) return -1;
        return int(n);
    }

    // Pide un feature report al mando. 'out' incluye el Report ID.
    bool getFeature(uint8_t id, std::vector<uint8_t>& out) {
        out.assign(std::max<USHORT>(info_.featLen, 64), 0);
        out[0] = id;
        return HidD_GetFeature(h_, out.data(), ULONG(out.size())) != FALSE;
    }

    // Windows exige que el buffer mida OutputReportByteLength: se rellena con ceros
    // (el driver HID usa el tamaño real del Report ID según el descriptor).
    // Seguro para llamar desde varios hilos.
    bool write(const uint8_t* data, size_t len, bool useSetOutputReport, DWORD timeoutMs, DWORD* elapsedUs = nullptr) {
        std::lock_guard<std::mutex> lk(writeMu_);
        size_t total = std::max<size_t>(len, info_.outLen);
        if (scratch_.size() != total) scratch_.assign(total, 0);
        std::copy(data, data + len, scratch_.begin());
        std::fill(scratch_.begin() + len, scratch_.end(), 0);

        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceCounter(&t0);
        bool ok;
        if (useSetOutputReport) {
            ok = HidD_SetOutputReport(h_, scratch_.data(), ULONG(total)) != FALSE;
        } else {
            OVERLAPPED ov{};
            ov.hEvent = wev_;
            ResetEvent(wev_);
            DWORD n = 0;
            ok = WriteFile(h_, scratch_.data(), DWORD(total), nullptr, &ov) != FALSE;
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(wev_, timeoutMs) == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(h_, &ov, &n, FALSE) != FALSE;
                } else {
                    CancelIoEx(h_, &ov);
                    GetOverlappedResult(h_, &ov, &n, TRUE);
                    SetLastError(WAIT_TIMEOUT);
                    ok = false;
                }
            }
        }
        DWORD err = ok ? 0 : GetLastError();
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        if (elapsedUs) *elapsedUs = DWORD((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart);
        if (!ok) SetLastError(err);
        return ok;
    }

    const DeviceInfo& info() const { return info_; }

private:
    DeviceInfo info_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE wev_ = nullptr, rev_ = nullptr;
    std::vector<uint8_t> scratch_;
    std::mutex writeMu_;
};

// --------------------------------------------------------- temporización ----

class PreciseClock {
public:
    PreciseClock() {
        QueryPerformanceFrequency(&freq_);
        timeBeginPeriod(1);
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    ~PreciseClock() { if (timer_) CloseHandle(timer_); timeEndPeriod(1); }

    long long now() const { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
    long long ticks(double sec) const { return (long long)(sec * double(freq_.QuadPart)); }
    double toMs(long long t) const { return t * 1000.0 / double(freq_.QuadPart); }

    // Duerme con el temporizador hasta ~1 ms antes y termina con espera activa.
    void sleepUntil(long long deadline) {
        long long rem = deadline - now();
        long long margin = ticks(0.0012);
        if (rem > margin && timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -(long long)((rem - margin) * 10000000LL / freq_.QuadPart);  // 100 ns relativos
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(timer_, INFINITE);
        }
        while (now() < deadline) YieldProcessor();
    }

private:
    LARGE_INTEGER freq_{};
    HANDLE timer_ = nullptr;
};

// ------------------------------------------------------------- streaming ----

struct Options {
    int device = -1;
    float gain = 1.0f;
    bool setOutputReport = false;
    bool loop = false;
    bool seq = false;
    int chL = -1, chR = -1;   // -1 = automático
    bool noLightbar = false;
    bool gainSet = false;
    // Fase 2 (en vivo)
    int source = -1;          // -1 = salida predeterminada
    float lowpass = 300.0f;
    float gate = 0.01f;
    float latency = 25.0f;
    bool gateSet = false;
    bool agc = true;          // control automático de ganancia
    float level = 0.9f;       // nivel objetivo del AGC
    float agcRelease = 1000.0f;
    bool volComp = true;      // compensar el volumen de Windows
    bool noAttach = false;    // fase 3a: no ejecutar 'usbip attach' automáticamente
    bool noOutput = false;    // fase 3a: no reenviar al mando las salidas del juego (diagnóstico)
    bool lowpassSet = false;
    bool latencySet = false;
    bool stripAudioFlags = false;  // fase 3b: no reenviar los ajustes de audio del juego al mando
    bool noHaptics = false;        // fase 3b: no convertir el audio del juego en háptica
    bool fast = true;              // modo directo (predeterminado): cada reporte sale apenas hay audio
    bool speaker = false;          // mandar también el audio del parlante del mando (reporte 0x36 + Opus)
    int reportBytes = 64;          // experimental: bytes de muestras por reporte (16-64)
    int mode = -1;                 // --mode ps5|ps4|xbox: fuerza el modo (si no, lo deciden los perfiles)
    bool noTray = false;           // sin ícono en la bandeja ni estadísticas
    bool wait = false;             // esperar a que aparezca el mando en vez de salir
    bool quiet = false;            // sin línea de estado (modo en segundo plano)
    std::string logFile;           // escribir la salida en un archivo
};

static bool openDevice(const Options& opt, HidDevice& dev) {
    auto list = enumerateDualSense();
    if (list.empty()) {
        std::printf(tr("No se encontró ningún DualSense.\n"
                    " - ¿Está encendido? Apretá el botón PS.\n"
                    " - ¿Está emparejado por Bluetooth en Configuración de Windows?\n"
                    " - Si usás HidHide: ¿está autorizada ESTA ruta de ds5haptics.exe en 'Applications'?\n", "No DualSense found.\n - Is it on? Press the PS button.\n - Is it paired over Bluetooth in Windows Settings?\n - If you use HidHide: is THIS path of ds5haptics.exe allowed under 'Applications'?\n"));
        return false;
    }
    int idx = -1;
    if (opt.device >= 0) {
        if (opt.device >= int(list.size())) { std::printf(tr("--device %d no existe (hay %zu).\n", "--device %d does not exist (there are %zu).\n"), opt.device, list.size()); return false; }
        idx = opt.device;
    } else {
        for (size_t i = 0; i < list.size(); ++i) if (list[i].bluetooth) { idx = int(i); break; }
        if (idx < 0) {
            std::printf(tr("Hay un DualSense pero no por Bluetooth (¿está conectado por USB?). Desconectá el cable.\n", "A DualSense is connected, but not over Bluetooth (is it on USB?). Unplug the cable.\n"));
            return false;
        }
    }
    {
        int bt = 0;
        for (const auto& d : list) bt += d.bluetooth ? 1 : 0;
        if (bt > 1)
            std::printf(tr("AVISO: hay %d DualSense conectados por Bluetooth; ds5haptics por ahora usa uno solo.\n", "WARNING: %d DualSense controllers are connected over Bluetooth; ds5haptics currently uses only one.\n"), bt);
    }
    const auto& di = list[idx];
    std::printf(tr("Usando [%d] %s  VID %04X PID %04X  %s  (in %u / out %u / feat %u bytes)\n", "Using [%d] %s  VID %04X PID %04X  %s  (in %u / out %u / feat %u bytes)\n"),
                idx, narrow(di.product).c_str(), di.vid, di.pid, di.bluetooth ? "Bluetooth" : "USB",
                di.inLen, di.outLen, di.featLen);
    if (!di.bluetooth) std::printf(tr("AVISO: este programa es para Bluetooth; por USB la háptica va por audio.\n", "WARNING: this program is for Bluetooth; over USB, haptics go through audio.\n"));
    if (di.outLen && di.outLen < ds5::kHapticsReportSize)
        std::printf(tr("AVISO: el descriptor declara salida de %u bytes (< %zu). El reporte 0x32 podría no aceptarse.\n", "WARNING: the descriptor declares %u-byte output reports (< %zu). Report 0x32 may be rejected.\n"),
                    di.outLen, ds5::kHapticsReportSize);

    if (!dev.open(di)) {
        std::printf(tr("No se pudo abrir el mando (%s).\nCerrá DSX, DS4Windows u otro programa que lo tenga tomado.\n", "Could not open the controller (%s).\nClose DSX, DS4Windows or any other program that is using it.\n"), lastErr().c_str());
        return false;
    }
    if (!dev.enableFullMode()) std::printf(tr("AVISO: no se pudo leer el feature 0x05 (%s).\n", "WARNING: could not read feature report 0x05 (%s).\n"), lastErr().c_str());
    return true;
}

static void setLightbar(HidDevice& dev, const Options& opt, uint8_t r, uint8_t g, uint8_t b) {
    if (opt.noLightbar) return;
    uint8_t rep[ds5::kMainReportSize];
    ds5::buildLightbarReport(rep, 0, r, g, b, 0x04 /* LED central */);
    if (!dev.write(rep, sizeof(rep), opt.setOutputReport, 100))
        std::printf(tr("AVISO: falló el reporte de barra de luz 0x31 (%s).\n", "WARNING: light bar report 0x31 failed (%s).\n"), lastErr().c_str());
}

// Fuente de muestras para el bucle de envío: entrega 64 bytes por reporte.
class HapticSource {
public:
    virtual ~HapticSource() = default;
    virtual bool next(int8_t* out64) = 0;      // false = terminó
    virtual std::string status() { return {}; }
};

class BufferSource : public HapticSource {
public:
    BufferSource(const std::vector<int8_t>& pcm, bool loop) : pcm_(pcm), loop_(loop) {}
    bool next(int8_t* out) override {
        if (pos_ >= pcm_.size()) {
            if (!loop_) return false;
            pos_ = 0;
        }
        size_t n = std::min(ds5::kSampleBytes, pcm_.size() - pos_);
        std::memset(out, 0, ds5::kSampleBytes);
        std::memcpy(out, pcm_.data() + pos_, n);
        pos_ += ds5::kSampleBytes;
        return true;
    }
private:
    const std::vector<int8_t>& pcm_;
    bool loop_;
    size_t pos_ = 0;
};

class LiveSource : public HapticSource {
public:
    LiveSource(ds5::LivePipeline& p, const ds5::LoopbackCapture& c, bool agc) : p_(p), c_(c), agc_(agc) {}
    bool next(int8_t* out) override { p_.pull(out); return true; }
    std::string status() override {
        float pk = std::min(1.0f, p_.takePeak());
        int bars = int(pk * 20 + 0.5f);
        std::string meter = std::string(size_t(bars), '#') + std::string(size_t(20 - bars), '.');
        char agc[32] = "";
        if (agc_) std::snprintf(agc, sizeof(agc), "  AGC x%-5.1f", p_.agcGain());
        char vol[40] = "";
        if (c_.muted()) std::snprintf(vol, sizeof(vol), tr("  Win SILENCIADO", "  Win MUTED"));
        else if (c_.volumeKnown()) std::snprintf(vol, sizeof(vol), "  Win %5.1f dB", c_.volumeDb());
        char b[220];
        std::snprintf(b, sizeof(b), tr(" nivel [%s]%s%s%s  búfer %4.1f ms  cortes %u", " level [%s]%s%s%s  buffer %4.1f ms  dropouts %u"),
                      meter.c_str(), pk >= 1.0f ? tr(" SATURA", " CLIPS") : "       ", agc, vol,
                      p_.bufferedMs(), p_.underruns());
        return b;
    }
private:
    ds5::LivePipeline& p_;
    const ds5::LoopbackCapture& c_;
    bool agc_;
};

static int stream(HidDevice& dev, HapticSource& src, const Options& opt) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    PreciseClock clk;
    ds5::HapticsReport rep;
    const long long period = clk.ticks(ds5::kReportPeriodSec);

    size_t sent = 0, failed = 0, late = 0;
    DWORD maxWriteUs = 0;
    unsigned long long sumWriteUs = 0;
    uint8_t seq = 0;
    long long next = clk.now();
    long long lastPrint = next;

    while (!g_stop) {
        int8_t chunk[ds5::kSampleBytes];
        if (!src.next(chunk)) break;
        rep.setSamples(chunk);
        if (opt.seq) rep.setSeq(seq++);

        clk.sleepUntil(next);
        DWORD us = 0;
        if (dev.write(rep.data(), rep.size(), opt.setOutputReport, 30, &us)) {
            ++sent;
            sumWriteUs += us;
            maxWriteUs = std::max(maxWriteUs, us);
        } else {
            if (failed++ == 0) std::printf(tr("\nError al escribir el reporte 0x32: %s\n", "\nError writing report 0x32: %s\n"), lastErr().c_str());
            if (failed > 50 && sent == 0) {
                std::printf(tr("Ningún reporte se pudo enviar. Probá con --setreport.\n", "No report could be sent. Try --setreport.\n"));
                break;
            }
        }
        next += period;
        long long now = clk.now();
        if (now - next > 3 * period) { next = now; ++late; }  // nos atrasamos: re-sincronizar
        if (now - lastPrint > clk.ticks(0.25)) {
            lastPrint = now;
            std::string st = src.status();
            if (!st.empty())
                std::printf("\r%s%s   ", st.c_str(), failed ? (tr("  fallidos ", "  failed ") + std::to_string(failed)).c_str() : "");
            else
                std::printf(tr("\r enviados %zu  fallidos %zu  escritura %.2f/%.2f ms   ", "\r sent %zu  failed %zu  write %.2f/%.2f ms   "),
                            sent, failed, sent ? sumWriteUs / 1000.0 / sent : 0.0, maxWriteUs / 1000.0);
            std::fflush(stdout);
        }
    }

    // Silencio para que los actuadores se detengan
    int8_t zero[ds5::kSampleBytes] = {};
    for (int i = 0; i < 8; ++i) {
        rep.setSamples(zero);
        clk.sleepUntil(next);
        dev.write(rep.data(), rep.size(), opt.setOutputReport, 30);
        next += period;
    }
    std::printf(tr("\nListo: enviados %zu, fallidos %zu, atrasos %zu.\n", "\nDone: sent %zu, failed %zu, late %zu.\n"), sent, failed, late);
    return failed && !sent ? 1 : 0;
}

static int playBuffer(const std::vector<int8_t>& pcm, const Options& opt) {
    HidDevice dev;
    if (!openDevice(opt, dev)) return 1;
    setLightbar(dev, opt, 0x80, 0x00, 0xFF);  // violeta = reproduciendo archivo
    std::printf(tr("Reproduciendo %.2f s de háptica (reportes cada %.3f ms)%s. Ctrl+C para cortar.\n", "Playing %.2f s of haptics (a report every %.3f ms)%s. Ctrl+C to stop.\n"),
                pcm.size() / 2.0 / ds5::kSampleRate, ds5::kReportPeriodSec * 1000, opt.loop ? tr(" en bucle", " in a loop") : "");
    BufferSource src(pcm, opt.loop);
    return stream(dev, src, opt);
}

// --------------------------------------------------------------- comandos ----

static int cmdList() {
    auto list = enumerateDualSense();
    if (list.empty()) { std::printf(tr("No se encontró ningún DualSense.\n", "No DualSense found.\n")); return 1; }
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& d = list[i];
        std::printf("[%zu] %s  VID %04X PID %04X  %-9s  in %u  out %u  feat %u\n    %s\n", i,
                    narrow(d.product).c_str(), d.vid, d.pid, d.bluetooth ? "Bluetooth" : "USB",
                    d.inLen, d.outLen, d.featLen, narrow(d.path).c_str());
    }
    return 0;
}

static int cmdInfo(const Options& opt) {
    HidDevice dev;
    if (!openDevice(opt, dev)) return 1;
    int simple = 0, full = 0, other = 0, last = -1;
    for (int i = 0; i < 20; ++i) {
        int id = dev.readReportId(500);
        if (id < 0) break;
        last = id;
        if (id == 0x31) ++full; else if (id == 0x01) ++simple; else ++other;
    }
    if (last < 0) std::printf(tr("No llegó ningún reporte de entrada (%s). Mové un stick y repetí.\n", "No input report arrived (%s). Move a stick and try again.\n"), lastErr().c_str());
    else std::printf(tr("Reportes de entrada: %d completos (0x31), %d simples (0x01), %d otros. Último: 0x%02X -> %s\n", "Input reports: %d full (0x31), %d basic (0x01), %d other. Last: 0x%02X -> %s\n"),
                     full, simple, other, last,
                     last == 0x31 ? tr("modo completo, correcto", "full mode, OK") : last == 0x01 ? tr("sigue en modo simple", "still in basic mode") : "desconocido");
    setLightbar(dev, opt, 0x00, 0xFF, 0x40);
    std::printf(tr("Si la barra de luz se puso verde, la escritura por Bluetooth funciona.\n", "If the light bar turned green, writing over Bluetooth works.\n"));
    return 0;
}

static int cmdSources() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto outs = ds5::LoopbackCapture::listOutputs();
    if (outs.empty()) { std::printf(tr("No se encontraron dispositivos de salida de audio.\n", "No audio output devices found.\n")); return 1; }
    std::printf(tr("Dispositivos de salida (lo que suena por ellos se puede convertir en háptica):\n", "Output devices (whatever plays on them can be turned into haptics):\n"));
    for (size_t i = 0; i < outs.size(); ++i)
        std::printf("  [%zu] %s%s\n", i, narrow(outs[i].name).c_str(), outs[i].isDefault ? tr("   <- predeterminado", "   <- default") : "");
    return 0;
}

static int cmdLive(const Options& opt) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring sourceId;  // vacío = seguir a la salida predeterminada
    if (opt.source >= 0) {
        auto outs = ds5::LoopbackCapture::listOutputs();
        if (opt.source >= int(outs.size())) {
            std::printf(tr("--source %d no existe. Usá 'ds5haptics sources' para ver la lista.\n", "--source %d does not exist. Use 'ds5haptics sources' to see the list.\n"), opt.source);
            return 2;
        }
        sourceId = outs[size_t(opt.source)].id;
    }

    HidDevice dev;
    if (!openDevice(opt, dev)) return 1;
    setLightbar(dev, opt, 0xFF, 0x50, 0x00);  // naranja = modo en vivo

    ds5::LiveConfig cfg;
    cfg.agc = opt.agc;
    cfg.level = opt.level;
    cfg.agcReleaseMs = opt.agcRelease;
    // Con AGC el nivel ya queda fijo en --level; sin AGC se usa la ganancia clásica.
    cfg.gain = opt.gainSet ? opt.gain : (opt.agc ? 1.0f : 2.0f);
    // Con AGC la compuerta va más baja: el volumen de entrada puede ser mucho menor.
    cfg.gate = opt.gateSet ? opt.gate : (opt.agc ? 0.001f : 0.01f);
    cfg.lowpassHz = opt.lowpass;
    cfg.latencyMs = opt.latency;
    auto pipe = std::make_unique<ds5::LivePipeline>(cfg);

    ds5::LoopbackCapture capture;
    bool ok = capture.start(
        sourceId, opt.chL, opt.chR, opt.volComp,
        [&pipe](const float* f, size_t n, unsigned rate) { pipe->feed(f, n, rate); },
        [](const std::string& msg) { std::printf("\n%s\n", msg.c_str()); });
    if (!ok) {
        std::printf(tr("No se pudo capturar el audio de la salida elegida.\n", "Could not capture audio from the selected output.\n"));
        return 1;
    }
    if (cfg.agc)
        std::printf(tr("Intensidad fija al %.0f%% (AGC) | volumen de Windows %s | ganancia extra %.2f\n", "Fixed intensity at %.0f%% (AGC) | Windows volume %s | extra gain %.2f\n"),
                    cfg.level * 100, opt.volComp ? "compensado" : tr("sin compensar", "not compensated"), cfg.gain);
    else
        std::printf(tr("Ganancia %.2f (AGC desactivado) | volumen de Windows %s\n", "Gain %.2f (AGC off) | Windows volume %s\n"), cfg.gain,
                    opt.volComp ? "compensado" : tr("sin compensar", "not compensated"));
    std::printf(tr("Pasa-bajos %s | compuerta %.3f | pre-búfer %.0f ms\n", "Low-pass %s | gate %.3f | pre-buffer %.0f ms\n"),
                cfg.lowpassHz > 0 ? (std::to_string(int(cfg.lowpassHz)) + " Hz").c_str() : tr("desactivado", "off"),
                cfg.gate, cfg.latencyMs);
    std::printf(tr("Poné música, un video o un juego. Ctrl+C para cortar.\n", "Play music, a video or a game. Ctrl+C to stop.\n"));

    LiveSource src(*pipe, capture, cfg.agc);
    int rc = stream(dev, src, opt);
    capture.stop();
    return rc;
}

// ------------------------------------------------------- fase 3a: virtual ----

static std::string hexBytes(const uint8_t* d, size_t n) {
    std::string r;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02X", d[i]);
        if (i) r += (i % 16 == 0) ? "\n      " : " ";
        r += b;
    }
    return r;
}

// Busca usbip.exe (cliente de usbip-win2) en el PATH y en las carpetas habituales.
static std::wstring findUsbip() {
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"usbip.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    const wchar_t* vars[] = {L"ProgramFiles", L"ProgramW6432"};
    const wchar_t* dirs[] = {L"\\USBip\\usbip.exe", L"\\usbip-win2\\usbip.exe", L"\\usbip\\usbip.exe"};
    for (auto v : vars) {
        wchar_t base[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(v, base, MAX_PATH);
        if (!n || n >= MAX_PATH) continue;
        for (auto d : dirs) {
            std::wstring p = std::wstring(base) + d;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }
    return {};
}

// Ejecuta un programa, captura su salida y devuelve el código de salida (-1 si no arrancó).
static int runProgram(const std::wstring& exe, const std::wstring& args, std::string& output, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    std::thread reader([&] {
        char b[512];
        DWORD n;
        while (ReadFile(rd, b, sizeof(b), &n, nullptr) && n) output.append(b, n);
    });
    if (WaitForSingleObject(pi.hProcess, timeoutMs) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    reader.join();
    CloseHandle(rd);
    return int(code);
}

// ------------------------------------------------- segundo plano / inicio ----

static const wchar_t* kStopEventName = L"Local\\ds5haptics-stop";
static const wchar_t* kInstanceMutexName = L"Local\\ds5haptics-virtual";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"ds5haptics";

static std::wstring selfPath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}
static std::wstring selfDir() {
    std::wstring p = selfPath();
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : p.substr(0, k);
}
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// Lanza 'ds5haptics virtual --speaker --wait' sin ninguna ventana y con registro en archivo.
static int cmdBackground(const std::vector<std::string>& extra) {
    std::wstring log = selfDir() + L"\\ds5haptics.log";
    std::wstring args = L"virtual --speaker --wait --log \"" + log + L"\"";
    for (auto& e : extra) args += L" \"" + widen(e) + L"\"";
    std::wstring cmd = L"\"" + selfPath() + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, selfDir().c_str(), &si, &pi)) {
        std::printf(tr("No se pudo iniciar en segundo plano (%s).\n", "Could not start in the background (%s).\n"), lastErr().c_str());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::printf(tr("ds5haptics está corriendo en segundo plano (sin ventana).\n"
                "Registro: %s\nPara detenerlo: ds5haptics stop\n", "ds5haptics is running in the background (no window).\nLog: %s\nTo stop it: ds5haptics stop\n"), narrow(log).c_str());
    return 0;
}

static int cmdStop() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (!ev) { std::printf(tr("ds5haptics no está corriendo.\n", "ds5haptics is not running.\n")); return 1; }
    SetEvent(ev);
    CloseHandle(ev);
    std::printf(tr("Se pidió a ds5haptics que se detenga.\n", "ds5haptics was asked to stop.\n"));
    return 0;
}

static bool autostartIsOn() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return on;
}

static bool autostartSet(bool on) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok;
    if (on) {
        std::wstring v = L"\"" + selfPath() + L"\" background";
        ok = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                            DWORD((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LONG r = RegDeleteValueW(key, kRunValue);
        ok = r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    return ok;
}

static int cmdAutostart(const std::string& what) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        std::printf(tr("No se pudo abrir el registro.\n", "Could not open the registry.\n"));
        return 1;
    }
    int rc = 0;
    if (what == "on") {
        std::wstring v = L"\"" + selfPath() + L"\" background";
        rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                            DWORD((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS ? 0 : 1;
        std::printf(rc ? tr("No se pudo activar el inicio automático.\n", "Could not enable start with Windows.\n")
                       : tr("Listo: ds5haptics arranca solo al iniciar sesión en Windows (sin ventana).\n"
                         "Si movés el programa a otra carpeta, ejecutá esto de nuevo.\n", "Done: ds5haptics now starts by itself when you sign in to Windows (no window).\nIf you move the program to another folder, run this again.\n"));
    } else if (what == "off") {
        LONG r = RegDeleteValueW(key, kRunValue);
        rc = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) ? 0 : 1;
        std::printf(rc ? tr("No se pudo desactivar el inicio automático.\n", "Could not disable start with Windows.\n") : tr("Inicio automático desactivado.\n", "Start with Windows disabled.\n"));
    } else {
        wchar_t buf[1024];
        DWORD size = sizeof(buf), type = 0;
        if (RegQueryValueExW(key, kRunValue, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS)
            std::printf(tr("Inicio automático: ACTIVADO\n  %s\n", "Start with Windows: ON\n  %s\n"), narrow(buf).c_str());
        else
            std::printf(tr("Inicio automático: desactivado\n", "Start with Windows: off\n"));
    }
    RegCloseKey(key);
    return rc;
}

static std::wstring findHidHideCli() {
    const wchar_t* vars[] = {L"ProgramFiles", L"ProgramW6432"};
    const wchar_t* rel[] = {L"\\Nefarius Software Solutions\\HidHide\\HidHideCLI.exe",
                            L"\\Nefarius Software Solutions\\HidHide\\x64\\HidHideCLI.exe"};
    for (auto v : vars) {
        wchar_t base[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(v, base, MAX_PATH);
        if (!n || n >= MAX_PATH) continue;
        for (auto r : rel) {
            std::wstring p = std::wstring(base) + r;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }
    return {};
}

// Autoriza este programa en HidHide, oculta el DualSense Bluetooth y activa el ocultamiento.
static int cmdHidHideSetup() {
    std::wstring cli = findHidHideCli();
    if (cli.empty()) { std::printf(tr("No encontré HidHide instalado (HidHideCLI.exe).\n", "HidHide is not installed (HidHideCLI.exe not found).\n")); return 1; }
    std::wstring args = L"--app-reg \"" + selfPath() + L"\"";
    int hidden = 0;
    for (const auto& d : enumerateDualSense()) {
        if (!d.bluetooth || d.instanceId.empty()) continue;
        args += L" --dev-hide \"" + d.instanceId + L"\"";
        std::printf(tr("Ocultando a los juegos: %s\n", "Hiding from games: %s\n"), narrow(d.instanceId).c_str());
        ++hidden;
    }
    if (!hidden) {
        std::printf(tr("No encontré el DualSense por Bluetooth: prendelo (botón PS) y repetí 'ds5haptics hidhide-setup'.\n"
                    "Igual autorizo el programa en HidHide.\n", "The DualSense was not found over Bluetooth: turn it on (PS button) and run 'ds5haptics hidhide-setup' again.\nThe program is allowed in HidHide anyway.\n"));
    }
    args += L" --cloak-on";
    std::string out;
    int rc = runProgram(cli, args, out, 20000);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (!out.empty()) std::printf("HidHide: %s\n", out.c_str());
    if (rc != 0) {
        std::printf(tr("HidHideCLI terminó con código %d. Cerrá la ventana de configuración de HidHide si está abierta\n"
                    "y repetí, o configuralo a mano (README).\n", "HidHideCLI exited with code %d. Close the HidHide configuration window if it is open\nand try again, or set it up by hand (README).\n"), rc);
        return 1;
    }
    std::printf(tr("Listo: programa autorizado en HidHide (%s)%s.\n", "Done: program allowed in HidHide (%s)%s.\n"), narrow(selfPath()).c_str(),
                hidden ? tr(", mando Bluetooth oculto y ocultamiento activado", ", Bluetooth controller hidden and hiding enabled") : "");
    std::printf(tr("Apagá y prendé el mando una vez para que el cambio tenga efecto.\n", "Turn the controller off and on once for the change to take effect.\n"));
    return hidden ? 0 : 2;
}

// ------------------------------------------------------- perfiles por juego ----

static std::wstring iniPath() { return selfDir() + L"\\ds5haptics.ini"; }

// Crea ds5haptics.ini con valores por defecto si no existe.
// Si es de una versión anterior, le agrega al final las opciones nuevas (modo, bateria_baja).
static void ensureIni() {
    std::wstring p = iniPath();
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string text;
        if (FILE* r = _wfopen(p.c_str(), L"rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), r)) > 0) text.append(buf, n);
            std::fclose(r);
        }
        if (text.empty()) return;
        const std::string low = ds5::ProfileSet::lower(text);
        // Un .ini en inglés (creado por esta versión) ya trae todo
        const bool englishIni = low.find("low_battery") != std::string::npos;
        const bool need1 = !englishIni && low.find("bateria_baja") == std::string::npos;
        const bool need2 = !englishIni && low.find("xbox_automatico") == std::string::npos;
        const bool need3 = low.find("idioma") == std::string::npos && low.find("language") == std::string::npos;
        const bool need4 = low.find("restaurar_audio") == std::string::npos && low.find("restore_audio") == std::string::npos;
        const bool need5 = low.find("espera_automatico") == std::string::npos && low.find("auto_wait") == std::string::npos;
        if (!need1 && !need2 && !need3 && !need4 && !need5) return;
        if (FILE* a = _wfopen(p.c_str(), L"ab")) {
            if (need1) std::fputs(ds5::kIniUpgradeBlock, a);
            if (need2) std::fputs(ds5::kIniUpgradeBlock2, a);
            if (need3) std::fputs(ds5::kIniUpgradeBlock3, a);
            if (need4) std::fputs(ds5::kIniUpgradeBlock4, a);
            if (need5) std::fputs(ds5::kIniUpgradeBlock5, a);
            std::fclose(a);
        }
        return;
    }
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return;
    std::fputs(ds5::defaultIni(), f);
    std::fclose(f);
}

static bool appendIni(const std::string& block) {
    FILE* a = _wfopen(iniPath().c_str(), L"ab");
    if (!a) return false;
    std::fputs(block.c_str(), a);
    std::fclose(a);
    return true;
}

static bool readIni(std::string& text, FILETIME& mtime) {
    HANDLE h = CreateFileW(iniPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME c, a;
    GetFileTime(h, &c, &a, &mtime);
    char buf[16384];
    DWORD n = 0;
    text.clear();
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) text.append(buf, n);
    CloseHandle(h);
    return true;
}

// Nombre del .exe de la ventana en primer plano (ej. "PhantomBladeZero.exe").
// 'fullscreen' indica si la ventana cubre todo el monitor (como los juegos).
static std::string foregroundExe(bool* fullscreen = nullptr, DWORD* pidOut = nullptr) {
    HWND w = GetForegroundWindow();
    if (fullscreen) *fullscreen = false;
    if (pidOut) *pidOut = 0;
    if (!w) return {};
    if (fullscreen) {
        RECT wr{};
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetWindowRect(w, &wr) && GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi))
            *fullscreen = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
                          wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid || pid == GetCurrentProcessId()) return {};
    if (pidOut) *pidOut = pid;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = DWORD(sizeof(buf) / sizeof(buf[0]));
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        name.assign(buf, n);
        size_t k = name.find_last_of(L"\\/");
        if (k != std::wstring::npos) name = name.substr(k + 1);
    }
    CloseHandle(h);
    return narrow(name);
}

// ¿Sigue abierto el proceso? (para volver al modo general cuando se cierra el juego)
constexpr DWORD STILL_ACTIVE_CODE = 259;
static bool processAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    // Algunos juegos con antitrampas no dejan consultarlos: "acceso denegado" = sigue abierto
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE_CODE;
    CloseHandle(h);
    return alive;
}

// ¿El proceso cargó una librería de mandos (XInput, DirectInput, GameInput)? Un juego que no
// usa el DualSense pero sí mandos de Xbox la carga; un reproductor de video no.
// Si no se puede mirar (antitrampas), se asume que sí.
static bool usesGamepadApi(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return true;
    std::vector<HMODULE> mods(1024);
    DWORD need = 0;
    bool found = false, ok = false;
    if (K32EnumProcessModulesEx(h, mods.data(), DWORD(mods.size() * sizeof(HMODULE)), &need, LIST_MODULES_ALL)) {
        ok = true;
        size_t n = std::min<size_t>(mods.size(), need / sizeof(HMODULE));
        for (size_t i = 0; i < n && !found; ++i) {
            wchar_t name[MAX_PATH];
            if (!K32GetModuleBaseNameW(h, mods[i], name, MAX_PATH)) continue;
            std::string m = ds5::ProfileSet::lower(narrow(name));
            found = m.rfind("xinput", 0) == 0 || m == "dinput8.dll" || m == "dinput.dll" ||
                    m == "gameinput.dll" || m == "windows.gaming.input.dll";
        }
    }
    CloseHandle(h);
    return ok ? found : true;
}

// Hace cuántos milisegundos arrancó el proceso (0 si no se sabe)
static unsigned long long processAgeMs(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    unsigned long long age = 0;
    if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        auto u = [](const FILETIME& f) { return (unsigned long long)f.dwHighDateTime << 32 | f.dwLowDateTime; };
        if (u(now) > u(created)) age = (u(now) - u(created)) / 10000;
    }
    CloseHandle(h);
    return age;
}

// Procesos que no son juegos (escritorio, navegadores, videos, launchers): no cambian el perfil
static bool ignoredForeground(const std::string& exe) {
    return ds5::isNonGameApp(exe);
}

// Muestra la batería del mando (sirve también con ds5haptics corriendo en segundo plano).
static int cmdBattery(const Options& opt) {
    HidDevice dev;
    if (!openDevice(opt, dev)) return 1;
    std::vector<uint8_t> buf;
    for (int i = 0; i < 300; ++i) {
        int n = dev.readReport(buf, 200);
        uint8_t usb[64];
        if (n > 0 && ds5::btInputToUsb(buf.data(), size_t(n), usb)) {
            ds5::BatteryInfo b = ds5::parseBattery(usb);
            std::printf(tr("Batería del mando: %d%%%s\n", "Controller battery: %d%%%s\n"), b.percent,
                        b.full ? tr(" (carga completa)", " (fully charged)") : b.charging ? tr(" (cargando)", " (charging)") : "");
            return 0;
        }
    }
    std::printf(tr("No se pudo leer la batería (el mando no mandó reportes completos).\n", "Could not read the battery (the controller sent no full reports).\n"));
    return 1;
}

static int cmdConfig() {
    ensureIni();
    std::wstring p = iniPath();
    std::printf(tr("Perfiles por juego: %s\n", "Per-game profiles: %s\n"), narrow(p).c_str());
    std::wstring cmd = L"notepad.exe \"" + p + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::printf(tr("Se abrió en el Bloc de notas. Al guardar, los cambios se aplican solos.\n", "Opened in Notepad. Changes apply by themselves when you save.\n"));
    } else {
        std::printf(tr("Abrilo a mano con el Bloc de notas.\n", "Open it with Notepad yourself.\n"));
    }
    return 0;
}

// Verifica que el mando Bluetooth siga oculto en HidHide. Si al reconectarse Windows le dio
// otra ruta de instancia, HidHide ya no lo oculta y el juego podría leer el mando real.
static std::string ensureHiddenInHidHide(const std::wstring& instanceId) {
    if (instanceId.empty()) return tr("sin ruta de instancia", "no instance path");
    std::wstring cli = findHidHideCli();
    if (cli.empty()) return tr("HidHide no instalado", "HidHide not installed");
    std::string out;
    if (runProgram(cli, L"--dev-list", out, 10000) != 0) return tr("no se pudo consultar HidHide", "could not query HidHide");
    std::string want = narrow(instanceId), hay = out;
    auto lower = [](std::string& x) { for (auto& ch : x) ch = char(std::tolower((unsigned char)ch)); };
    lower(want);
    lower(hay);
    // HidHideCLI puede escapar las barras en JSON: comparar también con barras dobles
    std::string wantEsc;
    for (char ch : want) { wantEsc += ch; if (ch == '\\') wantEsc += '\\'; }
    if (hay.find(want) != std::string::npos || hay.find(wantEsc) != std::string::npos) return tr("ya estaba oculto", "already hidden");
    std::string out2;
    int rc = runProgram(cli, L"--dev-hide \"" + instanceId + L"\" --cloak-on", out2, 10000);
    return rc == 0 ? tr("NO estaba oculto: se ocultó ahora (", "was NOT hidden: hidden now (") + narrow(instanceId) + ")"
                   : tr("NO está oculto y no se pudo ocultar (¿ventana de HidHide abierta?)", "is NOT hidden and could not be hidden (is the HidHide window open?)");
}

static int cmdVirtual(const Options& opt) {
    // Una sola instancia (el puerto 3240 y el mando no se pueden compartir)
    HANDLE instance = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::printf(tr("ds5haptics ya está corriendo (quizás en segundo plano). Para detenerlo: ds5haptics stop\n", "ds5haptics is already running (maybe in the background). To stop it: ds5haptics stop\n"));
        if (instance) CloseHandle(instance);
        return 1;
    }
    // 'ds5haptics stop' señala este evento
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
    std::thread stopWatcher([stopEvent] {
        while (!g_stop) {
            if (stopEvent && WaitForSingleObject(stopEvent, 200) == WAIT_OBJECT_0) g_stop = true;
            else if (!stopEvent) Sleep(200);
        }
    });
    struct Cleanup {
        std::thread& w; HANDLE ev; HANDLE mx;
        ~Cleanup() { g_stop = true; if (w.joinable()) w.join(); if (ev) CloseHandle(ev); if (mx) { ReleaseMutex(mx); CloseHandle(mx); } }
    } cleanup{stopWatcher, stopEvent, instance};

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::printf(tr("No se pudo iniciar Winsock.\n", "Could not start Winsock.\n")); return 1; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // para el audio de Windows (restaurar la salida)

    // Opciones de [general] que se usan antes de cargar los perfiles (se actualizan al recargar)
    std::atomic<bool> voiceOn{true}, restoreAudioOn{true};
    std::atomic<float> voiceVolume{2.0f};
    std::atomic<bool> shakeOn{true}, psStatsOn{true};
    std::string statsHotkey = "alt+f11";
    {
        ensureIni();
        std::string text;
        FILETIME t{};
        if (readIni(text, t)) {
            ds5::ProfileSet p0 = ds5::ProfileSet::parse(text);
            voiceOn = p0.voice;
            voiceVolume = p0.voiceVolume;
            restoreAudioOn = p0.restoreAudio;
            shakeOn = p0.shake;
            psStatsOn = p0.psButtonStats;
            statsHotkey = p0.statsHotkey;
        }
    }
    ds5::AudioDefaultGuard audioGuard;
    audioGuard.snapshot();   // la salida y el micrófono de antes de que aparezca el mando

    if (opt.wait) {
        bool told = false;
        while (!g_stop) {
            bool found = false;
            for (const auto& d : enumerateDualSense()) found |= d.bluetooth;
            if (found) break;
            if (!told) { std::printf(tr("Esperando a que prendas el DualSense (botón PS)...\n", "Waiting for you to turn on the DualSense (PS button)...\n")); std::fflush(stdout); told = true; }
            Sleep(2000);
        }
        if (g_stop) return 0;
    }

    HidDevice dev;
    if (!openDevice(opt, dev)) return 1;
    setLightbar(dev, opt, 0x00, 0x30, 0xFF);  // azul = modo mando virtual
    if (!findHidHideCli().empty())
        std::printf("HidHide: %s.\n", ensureHiddenInHidHide(dev.info().instanceId).c_str());

    // Features que los juegos piden al empezar: se leen una vez del mando real
    std::mutex featMu;
    std::map<uint8_t, std::vector<uint8_t>> featCache;
    for (uint8_t id : {uint8_t(0x05), uint8_t(0x09), uint8_t(0x20)}) {
        std::vector<uint8_t> f;
        if (dev.getFeature(id, f)) featCache[id] = f;
        else std::printf(tr("AVISO: no se pudo leer el feature 0x%02X del mando.\n", "WARNING: could not read feature report 0x%02X from the controller.\n"), id);
    }

    // El mando real puede desconectarse y volver: se protege con un candado
    // (lectura compartida para escribir/pedir features; exclusiva para reabrirlo).
    std::shared_mutex devMu;
    std::atomic<uint8_t> outSeq{0};
    std::atomic<unsigned> outErrors{0};
    std::atomic<unsigned> outLogged{0};
    std::atomic<bool> btLost{false};
    std::mutex logMu;
    std::map<uint8_t, bool> featLogged;
    ds5::VirtualDualSense::Callbacks cb;
    cb.getFeature = [&](uint8_t id, std::vector<uint8_t>& out) {
        bool cached = false;
        {
            std::lock_guard<std::mutex> lk(featMu);
            auto it = featCache.find(id);
            if (it != featCache.end()) { out = it->second; cached = true; }
        }
        bool ok = cached;
        if (!cached) {
            std::shared_lock<std::shared_mutex> lk(devMu);
            ok = !btLost && dev.getFeature(id, out);
        }
        std::lock_guard<std::mutex> lk(logMu);
        if (!featLogged[id]) {
            featLogged[id] = true;
            std::printf(tr("\n[diag] El juego pidió el feature 0x%02X: %s\n", "\n[diag] The game requested feature report 0x%02X: %s\n"), id,
                        cached ? tr("respondido desde la caché", "answered from cache") : ok ? tr("leído del mando", "read from the controller") : tr("NO disponible", "NOT available"));
        }
        return ok;
    };
    // Las salidas del juego configuran el mando por partes (luces, gatillos, audio) y casi
    // siempre una sola vez. Se guardan las más recientes para volver a mandarlas si el mando
    // se apaga y se prende (al reconectarse arranca "de fábrica").
    // Se guarda la última salida de cada "tipo" (según qué partes declara que cambia: luces,
    // gatillos, audio...), así un ajuste que el juego mandó una sola vez (el LED del medio)
    // no se pierde aunque después mande miles de ajustes de gatillos.
    // Perfil por juego en uso (lo actualiza el bucle principal; lo leen los callbacks)
    std::atomic<float> profSpeakerGain{1.0f}, profHapticGain{1.0f};
    std::atomic<bool> profMaxVolume{false};
    std::atomic<int> lastGameVolume{-1};
    std::atomic<int> trigR{-1}, trigL{-1};   // modo de efecto de los gatillos que pidió el juego
    // Batería: con poca carga, el LED del micrófono queda pulsando
    std::atomic<int> lowBatteryPct{15};
    std::atomic<bool> micPulsing{false};
    std::atomic<int> batteryPct{-1};
    std::atomic<bool> batteryCharging{false};
    std::mutex histMu;
    std::map<uint32_t, std::pair<uint64_t, std::vector<uint8_t>>> outByKind;
    uint64_t outCounter = 0;
    auto forwardOutput = [&](const uint8_t* usb, size_t len) -> bool {
        uint8_t bt[ds5::kMainReportSize];
        if (!ds5::usbOutputToBt(usb, len, outSeq++, bt)) return false;
        // valid_flag0 bit 0x80 = el reporte trae el volumen del parlante (byte 6 por USB)
        if (len > 6 && (usb[1] & 0x80)) {
            int vol = usb[6];
            if (lastGameVolume.exchange(vol) != vol) {
                std::lock_guard<std::mutex> lg(logMu);
                std::printf(tr("\n[diag] El juego fijó el volumen del parlante del mando en %d (0x%02X)%s\n", "\n[diag] The game set the controller speaker volume to %d (0x%02X)%s\n"), vol, vol,
                            profMaxVolume ? tr(" -> forzado al máximo por el perfil", " -> forced to maximum by the profile") : "");
            }
            if (profMaxVolume && bt[3 + 5] != 0x64) {
                bt[3 + 5] = 0x64;   // máximo
                uint32_t crc = ds5::crc32_bt(bt, ds5::kMainReportSize - 4);
                bt[74] = uint8_t(crc); bt[75] = uint8_t(crc >> 8); bt[76] = uint8_t(crc >> 16); bt[77] = uint8_t(crc >> 24);
            }
        }
        if (opt.stripAudioFlags) {
            // valid_flag0: quitar selección de háptica y ajustes de volumen/ruta de audio
            bt[3] &= uint8_t(~(0x02 | 0x10 | 0x20 | 0x40 | 0x80));
            uint32_t crc = ds5::crc32_bt(bt, ds5::kMainReportSize - 4);
            bt[74] = uint8_t(crc); bt[75] = uint8_t(crc >> 8); bt[76] = uint8_t(crc >> 16); bt[77] = uint8_t(crc >> 24);
        }
        // Batería baja: si el juego cambia el LED del micrófono, sigue pulsando igual
        if (micPulsing && (bt[3 + 1] & 0x01) && bt[3 + 8] != 2) {
            bt[3 + 8] = 2;
            uint32_t crc = ds5::crc32_bt(bt, ds5::kMainReportSize - 4);
            bt[74] = uint8_t(crc); bt[75] = uint8_t(crc >> 8); bt[76] = uint8_t(crc >> 16); bt[77] = uint8_t(crc >> 24);
        }
        std::shared_lock<std::shared_mutex> lk(devMu);
        if (btLost) return false;
        return dev.write(bt, sizeof(bt), opt.setOutputReport, 30);
    };
    cb.output = [&](const uint8_t* usb, size_t len) {
        // valid_flag0: 0x04 gatillo derecho, 0x08 gatillo izquierdo (modo en los bytes 11 y 22)
        if (len > 22 && usb[0] == 0x02) {
            if (usb[1] & 0x04) trigR = usb[11];
            if (usb[1] & 0x08) trigL = usb[22];
        }
        unsigned k = outLogged++;
        if (k < 5) {
            std::lock_guard<std::mutex> lk(logMu);
            std::printf(tr("\n[diag] Salida del juego #%u (%zu bytes)%s:\n      %s\n", "\n[diag] Game output #%u (%zu bytes)%s:\n      %s\n"), k + 1, len,
                        opt.noOutput ? tr(" — NO reenviada (--no-output)", " — NOT forwarded (--no-output)") : "",
                        hexBytes(usb, std::min<size_t>(len, 48)).c_str());
        }
        if (len >= 40) {
            std::lock_guard<std::mutex> lk(histMu);
            // Tipo = banderas de validez: valid_flag0, valid_flag1 y valid_flag2
            uint32_t kind = uint32_t(usb[1]) | (uint32_t(usb[2]) << 8) | (uint32_t(usb[39]) << 16);
            outByKind[kind] = {++outCounter, std::vector<uint8_t>(usb, usb + len)};
            if (outByKind.size() > 64) {          // protección: quitar el tipo más viejo
                auto oldest = outByKind.begin();
                for (auto it = outByKind.begin(); it != outByKind.end(); ++it)
                    if (it->second.first < oldest->second.first) oldest = it;
                outByKind.erase(oldest);
            }
        }
        if (opt.noOutput) return;
        if (!forwardOutput(usb, len) && !btLost) {
            if (outErrors++ == 0) {
                std::lock_guard<std::mutex> lg(logMu);
                std::printf(tr("\n[diag] Error al reenviar la salida al mando: %s\n", "\n[diag] Error forwarding the output to the controller: %s\n"), lastErr().c_str());
            }
        }
    };
    // Volver a mandar al mando todo lo que configuró el juego (en el mismo orden)
    auto replayGameState = [&]() -> size_t {
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> copy;
        {
            std::lock_guard<std::mutex> lk(histMu);
            for (const auto& kv : outByKind) copy.push_back(kv.second);
        }
        if (opt.noOutput) return 0;
        std::sort(copy.begin(), copy.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& e : copy) {
            auto& v = e.second;
            v[3] = v[4] = 0;   // motores de vibración clásica: no repetir una vibración vieja
            forwardOutput(v.data(), v.size());
            Sleep(4);
        }
        return copy.size();
    };
    cb.setFeature = [&](uint8_t id, const uint8_t* d, size_t len) {
        std::lock_guard<std::mutex> lk(logMu);
        std::printf(tr("\n[diag] El juego intentó escribir el feature 0x%02X (%zu bytes), ignorado:\n      %s\n", "\n[diag] The game tried to write feature report 0x%02X (%zu bytes), ignored:\n      %s\n"),
                    id, len, hexBytes(d, std::min<size_t>(len, 48)).c_str());
    };
    cb.log = [&](const std::string& m) {
        std::lock_guard<std::mutex> lg(logMu);
        std::printf("\n%s\n", m.c_str());
    };

    // Avisos hablados: se piden desde cualquier hilo y los dice un hilo aparte
    enum SayKind { kSayOn = 0, kSayMode = 1, kSayBattery = 2, kSayDesktop = 3 };
    std::mutex sayMu;
    std::condition_variable sayCv;
    std::deque<std::pair<int, int>> sayQueue;
    auto say = [&](int kind, int arg) {
        if (!voiceOn) return;
        std::lock_guard<std::mutex> lk(sayMu);
        // Si se acumulan (cambios de modo seguidos), quedarse con el último de cada tipo
        for (auto it = sayQueue.begin(); it != sayQueue.end();)
            it = it->first == kind ? sayQueue.erase(it) : it + 1;
        sayQueue.emplace_back(kind, arg);
        sayCv.notify_one();
    };

    // Fase 3b: la háptica nativa llega como audio por los canales 3 y 4 del parlante.
    // Ya viene diseñada para los actuadores: sin AGC, sin compuerta y sin pasa-bajos.
    ds5::LiveConfig hcfg;
    hcfg.agc = false;
    hcfg.gain = 1.0f;   // la ganancia (--gain y perfiles) se aplica al recibir el audio
    hcfg.gate = 0.0f;
    hcfg.lowpassHz = opt.lowpassSet ? opt.lowpass : 0.0f;
    // Modo normal: pre-búfer de 20 ms (Windows manda el audio justo a tiempo, 12 ms daba cortes).
    // Modo directo (--fast): sin pre-búfer; este valor solo limita el atraso máximo.
    hcfg.latencyMs = opt.latencySet ? opt.latency : (opt.fast ? 11.0f : 20.0f);
    auto haptics = std::make_unique<ds5::LivePipeline>(hcfg);
    std::vector<float> hbuf;
    ds5::VoiceQueue voiceQ;   // avisos hablados, se mezclan con el sonido del parlante
    // Modo Xbox/PS4: háptica y parlante simulados a partir del sonido del juego
    ds5::SimFx simFx;
    std::atomic<bool> simActive{false}, simHapOn{true}, simSpkOn{false};
    std::atomic<bool> fastSim{true};          // haptica_rapida
    // Bytes de háptica por reporte 0x32. Con menos de 64 el mando no rellena: reproduce el
    // reporte completo con ceros y suena un zumbido intermitente (probado). Quedan 64.
    std::atomic<int> fastSimBytes{64};
    std::atomic<int> simSens{5};
#ifdef DS5_WITH_OPUS
    const bool speakerOn = opt.speaker;
    ds5::SpeakerFifo speakerFifo;
    std::vector<float> sbuf;
#else
    const bool speakerOn = false;
    if (opt.speaker)
        std::printf(tr("AVISO: esta compilación no incluye el parlante (falta libopus). "
                    "Compilá con build.bat teniendo CMake instalado.\n", "WARNING: this build has no speaker support (libopus missing). Build with build.bat with CMake installed.\n"));
#endif
    cb.audioOut = [&](const int16_t* f, size_t n) {
#ifdef DS5_WITH_OPUS
        if (speakerOn) {
            sbuf.resize(n * 2);
            const float sg = profSpeakerGain.load();
            for (size_t i = 0; i < n; ++i) {
                sbuf[i * 2]     = ds5::softGain(f[i * 4] / 32768.0f, sg);       // canal 1
                sbuf[i * 2 + 1] = ds5::softGain(f[i * 4 + 1] / 32768.0f, sg);   // canal 2
            }
            speakerFifo.push(sbuf.data(), n);
        }
#endif
        if (opt.noHaptics && !speakerOn) return;
        hbuf.resize(n * 2);
        const float hg = opt.noHaptics ? 0.0f : profHapticGain.load();
        for (size_t i = 0; i < n; ++i) {
            hbuf[i * 2]     = f[i * 4 + 2] / 32768.0f * hg;   // canal 3 -> actuador izquierdo
            hbuf[i * 2 + 1] = f[i * 4 + 3] / 32768.0f * hg;   // canal 4 -> actuador derecho
        }
        haptics->feed(hbuf.data(), n, ds5::kAudioRate);
    };

    ds5::VirtualDualSense vds(cb);

    // ---- Modos de compatibilidad: el mando se presenta como DualShock 4 o Xbox 360 ----
    bool rumbleV2 = false;   // los DualSense con firmware nuevo usan otra bandera para la vibración
    {
        auto it = featCache.find(0x20);
        if (it != featCache.end() && it->second.size() >= 46)
            rumbleV2 = (it->second[44] | (it->second[45] << 8)) >= 0x0215;
    }
    std::atomic<int> padMode{ds5::kModePs5};
    ds5::SimplePad::Callbacks pcb;
    pcb.rumble = [&](uint8_t strong, uint8_t weak) {
        simFx.setRumble(strong, weak);
        if (opt.noOutput) return;
        uint8_t r[ds5::kUsbOutSize];
        ds5::buildRumbleReport(r, strong, weak, rumbleV2);
        forwardOutput(r, sizeof(r));
    };
    pcb.lightbar = [&](uint8_t r, uint8_t g, uint8_t b) {
        uint8_t o[ds5::kUsbOutSize];
        ds5::buildLightbarUsbReport(o, r, g, b);
        cb.output(o, sizeof(o));   // queda guardado: se reenvía si el mando se reconecta
    };
    pcb.getFeature = [&](uint8_t id, std::vector<uint8_t>& out) {
        {
            std::lock_guard<std::mutex> lk(featMu);
            auto it = featCache.find(id);
            if (it != featCache.end()) { out = it->second; return true; }
        }
        std::shared_lock<std::shared_mutex> lk(devMu);
        return !btLost && dev.getFeature(id, out);
    };
    ds5::VirtualDS4 vps4(pcb);
    ds5::VirtualXbox360 vxbox(pcb);
    // Color de la barra de luz según el modo: azul PS5, violeta PS4, verde Xbox
    auto modeLight = [&] {
        int m = padMode;
        if (m == ds5::kModePs4) setLightbar(dev, opt, 0x80, 0x00, 0xFF);
        else if (m == ds5::kModeXbox) setLightbar(dev, opt, 0x10, 0xD0, 0x10);
        else setLightbar(dev, opt, 0x00, 0x30, 0xFF);
    };

    usbip::Server server;
    if (!server.start(vds, 3240, [](const std::string& m) { std::printf("\n%s\n", m.c_str()); })) {
        std::printf(tr("No se pudo abrir el puerto 3240 en 127.0.0.1 (¿otro programa USB/IP lo está usando?).\n", "Could not open port 3240 on 127.0.0.1 (is another USB/IP program using it?).\n"));
        WSACleanup();
        return 1;
    }

    // Lector del mando real por Bluetooth (se reconecta solo si el mando se va y vuelve)
    std::atomic<unsigned> inputs{0};
    std::atomic<ds5ui::Tray*> trayPtr{nullptr};   // para que el botón PS abra las estadísticas
    std::atomic<unsigned> shakeRequests{0};
    std::thread btThread([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        std::vector<uint8_t> buf;
        DWORD lastFix = 0, lostAt = 0;
        int diagLeft = 0, diagFull = 0, diagSimple = 0;
        ds5::BatteryInfo battShown, battCandidate;
        DWORD battSince = 0;
        ds5::Gestures gestures;
        gestures.reset(GetTickCount());
        while (!g_stop) {
            if (btLost) {
                Sleep(1000);
                for (const auto& di : enumerateDualSense()) {
                    if (!di.bluetooth) continue;
                    std::unique_lock<std::shared_mutex> lk(devMu);
                    dev.close();
                    if (dev.open(di)) {
                        dev.enableFullMode();
                        btLost = false;
                        lk.unlock();
                        {
                            std::lock_guard<std::mutex> lg(logMu);
                            std::printf(tr("\n[diag] Mando Bluetooth reconectado después de %.1f s (%s).\n", "\n[diag] Bluetooth controller reconnected after %.1f s (%s).\n"),
                                        (GetTickCount() - lostAt) / 1000.0, narrow(di.instanceId).c_str());
                        }
                        std::string hh = ensureHiddenInHidHide(di.instanceId);
                        Sleep(300);   // dar tiempo al mando a terminar de conectarse
                        size_t replayed = replayGameState();
                        if (!replayed) modeLight();
                        gestures.reset(GetTickCount());      // el PS con que se prendió no cuenta
                        micPulsing = false;                  // el mando arranca con el LED normal:
                        battShown = ds5::BatteryInfo();      // se vuelve a evaluar la batería
                        std::lock_guard<std::mutex> lg(logMu);
                        std::printf(tr("[diag] HidHide: %s. Configuración del juego reenviada: %zu reportes.\n", "[diag] HidHide: %s. Game settings re-sent: %zu reports.\n"),
                                    hh.c_str(), replayed);
                        say(kSayOn, 0);
                        diagLeft = 500;   // contar los tipos de reporte de entrada que llegan
                        diagFull = diagSimple = 0;
                    }
                    break;
                }
                continue;
            }
            int n = dev.readReport(buf, 100);
            if (n < 0) {
                std::string why = lastErr();
                lostAt = GetTickCount();
                btLost = true;
                std::lock_guard<std::mutex> lg(logMu);
                std::printf(tr("\n[diag] Se perdió la conexión con el mando Bluetooth (%s). "
                            "Esperando a que vuelva...\n", "\n[diag] Lost the connection to the Bluetooth controller (%s). Waiting for it to come back...\n"), why.c_str());
                continue;
            }
            if (n == 0) continue;
            if (diagLeft > 0) {
                if (buf[0] == 0x31) ++diagFull; else if (buf[0] == 0x01) ++diagSimple;
                if (--diagLeft == 0) {
                    std::lock_guard<std::mutex> lg(logMu);
                    std::printf(tr("\n[diag] Después de reconectar: %d reportes completos (0x31), %d simples (0x01)%s\n", "\n[diag] After reconnecting: %d full reports (0x31), %d basic (0x01)%s\n"),
                                diagFull, diagSimple, diagSimple > diagFull ? tr(" — QUEDÓ EN MODO SIMPLE", " — STUCK IN BASIC MODE") : "");
                }
            }
            uint8_t usb[64];
            if (ds5::btInputToUsb(buf.data(), size_t(n), usb)) {
                // Gestos: el botón PS muestra las estadísticas (y el juego también lo recibe);
                // agitar el mando cambia de modo (lo resuelve el hilo principal)
                ds5::Gestures::Events ge = gestures.feed(usb, GetTickCount());
                if (ge.psPressed && psStatsOn) {
                    if (ds5ui::Tray* tp = trayPtr.load()) tp->toggleStats();
                }
                if (ge.shaken && shakeOn) ++shakeRequests;
                switch (padMode.load()) {
                    case ds5::kModePs4: vps4.pushInput(usb); break;
                    case ds5::kModeXbox: vxbox.pushInput(usb); break;
                    default: vds.pushInput(usb);
                }
                ++inputs;
                // Batería (el valor tiene que mantenerse 3 s, para no oscilar entre dos niveles)
                ds5::BatteryInfo b = ds5::parseBattery(usb);
                DWORD nowB = GetTickCount();
                if (b != battCandidate) { battCandidate = b; battSince = nowB; }
                else if (b != battShown && nowB - battSince > 3000) {
                    battShown = b;
                    batteryPct = b.percent;
                    batteryCharging = b.charging || b.full;
                    std::lock_guard<std::mutex> lg(logMu);
                    std::printf(tr("\nBatería del mando: %d%%%s\n", "\nController battery: %d%%%s\n"), b.percent,
                                b.full ? tr(" (carga completa)", " (fully charged)") : b.charging ? tr(" (cargando)", " (charging)") : "");
                    std::fflush(stdout);
                }
                int thr = lowBatteryPct.load();
                bool low = thr > 0 && battShown.percent >= 0 && !battShown.charging && !battShown.full &&
                           battShown.percent <= thr;
                if (low != micPulsing.load() && !opt.noOutput) {
                    micPulsing = low;
                    uint8_t r[ds5::kUsbOutSize];
                    ds5::buildMicLedReport(r, low ? 2 : 0);
                    forwardOutput(r, sizeof(r));
                    std::lock_guard<std::mutex> lg(logMu);
                    std::printf(low ? tra("\nBatería baja (%d%%): la luz del micrófono queda parpadeando.\n", "\nLow battery (%d%%): the mic button light will keep pulsing.\n",
                                          "\nChe, el joystick está en las últimas (%d%%): la luz del micrófono queda parpadeando. Enchufalo.\n")
                                    : tr("\nBatería: la luz del micrófono vuelve a la normalidad (%d%%).\n", "\nBattery: the mic button light is back to normal (%d%%).\n"), battShown.percent);
                    std::fflush(stdout);
                    if (low) say(kSayBattery, battShown.percent);
                }
            } else if (buf[0] == 0x01 && GetTickCount() - lastFix > 1000) {
                lastFix = GetTickCount();   // volvió al modo simple: reactivar el completo
                std::shared_lock<std::shared_mutex> lk(devMu);
                dev.enableFullMode();
            }
        }
    });

    // Hilo de envío de háptica: un reporte 0x32 cada 10,667 ms mientras el juego mande audio
    std::atomic<unsigned> hapticReports{0};
    std::atomic<bool> hapticActive{false};
    std::atomic<unsigned> hapticGaps{0};
    std::atomic<unsigned> speakerErrors{0};
    (void)speakerErrors;
    std::atomic<unsigned long long> writeUsSum{0};
    std::atomic<unsigned> writeCount{0};
    std::thread hapticThread([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        PreciseClock clk;
        ds5::HapticsReport rep;
        // Con parlante, cada reporte 0x36 lleva siempre 64 bytes de háptica
        const size_t nbytes = (opt.fast && !speakerOn) ? size_t(opt.reportBytes) : ds5::kSampleBytes;
        const long long period = clk.ticks(double(nbytes / 2) / ds5::kSampleRate);
        long long next = clk.now();
        long long lastReal = next - clk.ticks(10.0);
        long long nextVoice = next;
        bool inGap = false;
        (void)nextVoice;
#ifdef DS5_WITH_OPUS
        ds5::SpeakerEncoder speakerEnc;
        ds5::AudioHapticsReport arep;
        bool encOk = speakerOn && speakerEnc.init();
        if (speakerOn && !encOk) std::printf(tr("\nAVISO: no se pudo iniciar el codificador Opus; sigue solo la háptica.\n", "\nWARNING: could not start the Opus encoder; haptics only.\n"));
        std::vector<float> spk(ds5::kSpeakerInFrames * 2);
        uint8_t opusPkt[ds5::kOpusBytes];
#endif
        // Háptica simulada sin parlante: reportes 0x32 (solo háptica, 142 bytes) en vez de los
        // 0x36 con audio Opus (398 bytes): sin codificar audio y menos tiempo en el aire
        const size_t liteBytes = size_t(std::clamp(fastSimBytes.load(), 16, 64)) & ~size_t(1);
        auto liteNow = [&] {
            return fastSim && padMode != ds5::kModePs5 && simActive && !simSpkOn && !voiceQ.active();
        };
        auto send = [&](const int8_t* chunk, size_t n = 0, bool lite = false) {
            if (!n) n = nbytes;
            (void)lite;
#ifdef DS5_WITH_OPUS
            if (encOk && !lite) {
                speakerFifo.pop(spk.data());
                voiceQ.mixInto(spk.data(), ds5::kSpeakerInFrames);
                if (!speakerEnc.encode(spk.data(), opusPkt)) ++speakerErrors;
                arep.build(chunk, opusPkt);
                std::shared_lock<std::shared_mutex> lk(devMu);
                DWORD us = 0;
                if (!btLost && dev.write(arep.data(), arep.size(), opt.setOutputReport, 30, &us)) {
                    ++hapticReports; writeUsSum += us; ++writeCount;
                }
                return;
            }
#endif
            rep.setSamples(chunk, n);
            std::shared_lock<std::shared_mutex> lk(devMu);
            DWORD us = 0;
            if (!btLost && dev.write(rep.data(), rep.size(), opt.setOutputReport, 30, &us)) {
                ++hapticReports; writeUsSum += us; ++writeCount;
            }
        };
        while (!g_stop) {
            int8_t chunk[ds5::kSampleBytes];
            if (opt.fast) {
                // Modo directo: revisar cada 1 ms y mandar todo lo que ya esté completo
                clk.sleepUntil(clk.now() + clk.ticks(0.001));
                bool any = false;
                const bool lite = liteNow();
                const size_t nb = lite ? liteBytes : nbytes;
                while (haptics->pullExact(chunk, nb)) { send(chunk, nb, lite); any = true; }
                long long now = clk.now();
                if (any) { lastReal = now; inGap = false; nextVoice = now + period; }
#ifdef DS5_WITH_OPUS
                // Un aviso hablado sin audio del juego: mandar reportes al ritmo normal (háptica en cero)
                else if (encOk && voiceQ.active() && now >= nextVoice) {
                    std::memset(chunk, 0, sizeof(chunk));
                    send(chunk);
                    nextVoice = (now - nextVoice > 3 * period) ? now + period : nextVoice + period;
                }
#endif
                bool streaming = vds.speakerStreaming() && now - lastReal < clk.ticks(1.0);
                hapticActive = streaming;
                // Hueco: el juego transmite pero no llegó audio a tiempo para el siguiente reporte
                if (streaming && !any && !inGap && now - lastReal > period + period / 2) { inGap = true; ++hapticGaps; }
                continue;
            }
            bool real = haptics->pull(chunk);
            long long now = clk.now();
            if (real) lastReal = now;
            // Seguir mandando 1 s después del último audio (silencio), después callar
            bool active = now - lastReal < clk.ticks(1.0) || voiceQ.active();
            hapticActive = active && real;
            clk.sleepUntil(next);
            if (active) send(chunk);
            next += period;
            if (clk.now() - next > 3 * period) next = clk.now();
        }
    });

    // Hilo de los avisos hablados: genera la frase con la voz de Windows y la manda al parlante
    auto phrase = [&](int kind, int arg, bool es) -> std::wstring {
        const bool ar = es && ds5::isArgento();
        if (kind == kSayOn)
            return !es ? L"D S 5 haptics on." : ar ? L"De ese cinco h\u00e1ptics activado. A darle." : L"De ese cinco h\u00e1ptics activado.";
        if (kind == kSayDesktop)   // se cerró el juego: vuelve el modo de todos los días
            return !es ? L"Game closed. Desktop mode." : ar ? L"Se termin\u00f3 el juego. Modo escritorio." : L"Juego cerrado. Modo escritorio.";
        if (kind == kSayMode) {
            if (arg == ds5::kModeXbox)
                return !es ? L"Xbox mode." : ar ? L"Modo Xbox. El juego ni se va a dar cuenta." : L"Modo Xbox.";
            if (arg == ds5::kModePs4) return !es ? L"P S 4 mode." : L"Modo pe ese cuatro.";
            return !es ? L"P S 5 mode." : ar ? L"Modo pe ese cinco, con todo." : L"Modo pe ese cinco.";
        }
        wchar_t b[200];
        if (!es) std::swprintf(b, 200, L"Low battery. %d percent left.", arg);
        else if (ar) std::swprintf(b, 200, L"Che, te queda el %d por ciento de bater\u00eda. Enchufalo antes de que te deje a gamba.", arg);
        else std::swprintf(b, 200, L"Bater\u00eda baja. Queda el %d por ciento.", arg);
        return b;
    };
    // Volumen y salida del parlante del mando (si ningún juego los configuró, el aviso no sonaría)
    auto routeSpeaker = [&] {
        if (opt.noOutput) return;
        uint8_t r[ds5::kUsbOutSize] = {};
        r[0] = 0x02;
        r[1] = 0x20 | 0x80;   // valid_flag0: volumen del parlante + control de audio
        r[6] = 0x64;          // volumen del parlante: el máximo (como volumen_mando_max)
        r[8] = 0x30;          // control de audio: salida por el parlante del mando
        uint8_t bt[ds5::kMainReportSize];
        if (!ds5::usbOutputToBt(r, sizeof(r), outSeq++, bt)) return;
        std::shared_lock<std::shared_mutex> lk(devMu);
        if (!btLost) dev.write(bt, sizeof(bt), opt.setOutputReport, 30);
    };
    (void)phrase;
    (void)routeSpeaker;
    std::thread voiceThread([&] {
#ifdef DS5_WITH_OPUS
        if (!speakerOn) return;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ttsReady = false, ttsTried = false;
        auto tts = std::make_unique<ds5::Speaker>();
        while (!g_stop) {
            std::pair<int, int> req;
            {
                std::unique_lock<std::mutex> lk(sayMu);
                sayCv.wait_for(lk, std::chrono::milliseconds(300), [&] { return !sayQueue.empty() || g_stop.load(); });
                if (g_stop) break;
                if (sayQueue.empty()) continue;
                req = sayQueue.front();
                sayQueue.pop_front();
            }
            if (!voiceOn) continue;
            if (!ttsTried) {   // se prepara recién al primer aviso
                ttsTried = true;
                ttsReady = tts->init(!ds5::isEnglish());
                std::lock_guard<std::mutex> lg(logMu);
                if (!ttsReady) std::printf(tr("\nAVISO: no se pudo iniciar la voz de Windows; sin avisos hablados.\n", "\nWARNING: could not start the Windows voice; no spoken notices.\n"));
                else if (!ds5::isEnglish() && !tts->spanish())
                    std::printf(tr("\nNo hay voz de Windows en español instalada: los avisos se dicen en inglés.\n", "\nNo Spanish Windows voice installed: notices are spoken in English.\n"));
            }
            if (!ttsReady) continue;
            std::vector<float> pcm;
            if (!tts->synthesize(phrase(req.first, req.second, tts->spanish()), pcm)) continue;
            routeSpeaker();
            voiceQ.push(pcm, voiceVolume.load());
            for (int i = 0; i < 300 && voiceQ.active() && !g_stop; ++i) Sleep(50);
            Sleep(200);
            // Devolver al parlante el volumen y la salida que había puesto el juego (si puso)
            bool hadGame;
            { std::lock_guard<std::mutex> lk(histMu); hadGame = !outByKind.empty(); }
            if (hadGame) replayGameState();
        }
        tts.reset();
        CoUninitialize();
#endif
    });

    if (speakerOn)
        std::printf(tr("Parlante activado (experimental): háptica + sonido del parlante en el reporte 0x36 (Opus).\n", "Speaker enabled (experimental): haptics + speaker sound in report 0x36 (Opus).\n"));
    else if (opt.fast)
        std::printf(tr("Modo directo: cada reporte de háptica (%d bytes, %.1f ms) sale apenas hay audio.\n", "Direct mode: each haptics report (%d bytes, %.1f ms) is sent as soon as the audio is there.\n"),
                    opt.reportBytes, opt.reportBytes / 2.0 / ds5::kSampleRate * 1000);
    std::printf(tr("Servidor USB/IP escuchando en 127.0.0.1:3240 (dispositivo 1-1).\n", "USB/IP server listening on 127.0.0.1:3240 (device 1-1).\n"));
    auto attachVirtual = [&](int attempts) {
        if (opt.noAttach) return;
        std::wstring usbip = findUsbip();
        if (usbip.empty()) {
            std::printf(tr("No encontré usbip.exe. Instalá usbip-win2 o conectalo a mano con:\n"
                        "  usbip attach -r 127.0.0.1 -b 1-1\n", "usbip.exe not found. Install usbip-win2 or attach it by hand with:\n  usbip attach -r 127.0.0.1 -b 1-1\n"));
            return;
        }
        std::printf(tr("Conectando con usbip-win2...\n", "Connecting through usbip-win2...\n"));
        std::string out;
        int rc = 1;
        // Al iniciar Windows el servicio de usbip-win2 puede tardar: reintentar
        for (int attempt = 0; attempt < attempts && !g_stop; ++attempt) {
            if (attempt) Sleep(3000);
            out.clear();
            rc = runProgram(usbip, L"attach -r 127.0.0.1 -b 1-1", out, 20000);
            if (rc == 0 || server.imported()) break;
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (!out.empty()) std::printf("usbip: %s\n", out.c_str());
        if (rc != 0 && !server.imported())
            std::printf(tr("usbip attach terminó con código %d. Si dice 'acceso denegado', "
                        "ejecutá ds5haptics como administrador.\n", "usbip attach exited with code %d. If it says 'access denied', run ds5haptics as administrator.\n"), rc);
        std::fflush(stdout);
    };
    // Desde cuándo está el DualSense virtual "enchufado" (para no juzgar juegos que no lo vieron)
    std::atomic<DWORD> dsPresentSince{GetTickCount()};
    auto deviceFor = [&](int m) -> usbip::Device& {
        if (m == ds5::kModePs4) return vps4;
        if (m == ds5::kModeXbox) return vxbox;
        return vds;
    };
    auto describeMode = [](int m) {
        return m == ds5::kModePs4 ? tr("PS4 (DualShock 4): vibración clásica, sin háptica ni gatillos adaptativos", "PS4 (DualShock 4): classic rumble, no haptics or adaptive triggers")
             : m == ds5::kModeXbox ? tr("Xbox 360: vibración clásica, sin háptica ni gatillos adaptativos", "Xbox 360: classic rumble, no haptics or adaptive triggers")
                                   : tr("PS5 (DualSense completo)", "PS5 (full DualSense)");
    };
    // Cambia en caliente el mando que ve Windows: "desenchufa" el actual y "enchufa" el nuevo
    bool closingGame = false;   // el cambio de modo es porque se cerró el juego
    auto switchMode = [&](int m) {
        if (m == padMode) return;
        std::printf(tr("\nCambiando el mando virtual a modo %s...\n", "\nSwitching the virtual controller to %s mode...\n"), describeMode(m));
        std::fflush(stdout);
        { std::lock_guard<std::mutex> lk(histMu); outByKind.clear(); }
        if (!opt.noOutput) {
            uint8_t r[ds5::kUsbOutSize];
            ds5::buildRumbleReport(r, 0, 0, rumbleV2);       // cortar cualquier vibración en curso
            if (padMode != ds5::kModePs5) forwardOutput(r, sizeof(r));
            if (m == ds5::kModePs5) {                         // sin banderas de vibración: vuelve la háptica por audio
                r[1] = 0; r[39] = 0;
                forwardOutput(r, sizeof(r));
            }
        }
        padMode = m;
        server.setDevice(deviceFor(m));
        server.dropClient();
        for (int i = 0; i < 50 && server.imported(); ++i) Sleep(100);
        // Esperar a que Windows quite el mando anterior (y por si usbip-win2 reconecta solo)
        for (int i = 0; i < 20 && !server.imported(); ++i) Sleep(100);
        if (!server.imported()) attachVirtual(5);
        if (m == ds5::kModePs5) dsPresentSince = GetTickCount();
        modeLight();
        if (closingGame) say(kSayDesktop, m); else say(kSayMode, m);
    };

    // Modo inicial: el de la línea de comandos o el de [general] en ds5haptics.ini
    {
        ensureIni();
        std::string text;
        FILETIME t{};
        int m0 = opt.mode;
        if (m0 < 0 && readIni(text, t)) m0 = ds5::ProfileSet::parse(text).general.mode;
        if (m0 > 0) {
            padMode = m0;
            server.setDevice(deviceFor(m0));
            modeLight();
        }
        std::printf(tr("Modo del mando virtual: %s.\n", "Virtual controller mode: %s.\n"), describeMode(padMode));
    }
    attachVirtual(opt.wait ? 10 : 1);
    say(kSayOn, 0);
    if (padMode != ds5::kModePs5)
        std::printf(tr("Mientras esto esté abierto, Windows ve el mando de compatibilidad elegido.\n"
                    "Ctrl+C para cortar.\n", "While this is open, Windows sees the selected compatibility controller.\nCtrl+C to stop.\n"));
    else
    std::printf(tr("Mientras esto esté abierto, Windows ve un DualSense por cable, con parlante.\n"
                "El micrófono del mando NO funciona por Bluetooth: en Discord y otros programas elegí tu micrófono.\n"
                "La háptica nativa del juego (canales 3 y 4 del parlante) se manda al mando por Bluetooth.\n"
                "Si Windows elige al mando como salida de audio o micrófono, ds5haptics vuelve a poner el que\n"
                "tenías (restaurar_audio en ds5haptics.ini).\n"
                "Ctrl+C para cortar.\n"
                "Ocultá el mando Bluetooth real con HidHide para que los juegos no vean dos mandos.\n\n",
                "While this is open, Windows sees a wired DualSense, with speaker.\n"
                "The controller mic does NOT work over Bluetooth: pick your own mic in Discord and other apps.\n"
                "The game's native haptics (speaker channels 3 and 4) are sent to the controller over Bluetooth.\n"
                "If Windows picks the controller as the audio output or mic, ds5haptics switches back to the one\n"
                "you had (restore_audio in ds5haptics.ini).\n"
                "Ctrl+C to stop.\n"
                "Hide the real Bluetooth controller with HidHide so games don't see two controllers.\n\n"));

    unsigned lastInputs = 0;
    DWORD lastT = GetTickCount();
    std::fflush(stdout);

    // Ícono en la bandeja y cuadro de estadísticas (atajo de teclado o botón PS)
    ds5ui::Tray tray;
    auto applyHotkey = [&](const std::string& text) {
        unsigned mods = 0, vk = 0;
        std::string label;
        if (!ds5::parseHotkey(text, mods, vk, &label)) {
            std::printf(tr("\natajo_estadisticas = \"%s\" no se entiende; uso Alt+F11.\n", "\nstats_hotkey = \"%s\" not understood; using Alt+F11.\n"), text.c_str());
            ds5::parseHotkey("alt+f11", mods, vk, &label);
        }
        tray.setHotkey(mods, vk, label);
    };
    if (!opt.noTray) {
        applyHotkey(statsHotkey);
        ds5ui::Tray::Callbacks tcb;
        tcb.exit = [] { g_stop = true; };
        tcb.autostartOn = [] { return autostartIsOn(); };
        tcb.setAutostart = [&](bool on) {
            bool ok = autostartSet(on);
            std::lock_guard<std::mutex> lg(logMu);
            std::printf(tr("\nInicio con Windows %s%s.\n", "\nStart with Windows %s%s.\n"), on ? tr("activado", "on") : tr("desactivado", "off"), ok ? "" : tr(" (no se pudo cambiar)", " (could not be changed)"));
            std::fflush(stdout);
        };
        tcb.log = [&](const std::string& m) {
            std::lock_guard<std::mutex> lg(logMu);
            std::printf("\n%s\n", m.c_str());
            std::fflush(stdout);
        };
        tray.start(tcb);
        trayPtr = &tray;
    }
    // Perfiles: se recarga ds5haptics.ini si cambia y se mira el juego en primer plano
    ensureIni();
    ds5::ProfileSet profiles;
    FILETIME iniTime{};
    std::string currentGame, currentProfile = "?";
    DWORD currentPid = 0;
    unsigned tick = 0;
    // Xbox automático: ¿el juego le habló al DualSense virtual (calibración, luces, gatillos)?
    DWORD lastDsActivity = 0;                 // última vez que alguien usó el DualSense virtual
    unsigned lastDsCount = vds.outputs() + vds.featureRequests();
    bool autoWatching = false;                // esperando a ver si el juego usa el DualSense
    DWORD autoSince = 0;
    int autoOverride = -1;                    // modo decidido automáticamente para el juego actual
    DWORD kAutoWaitMs = 50000;                // espera_automatico (se toma del ini)
    std::map<std::string, bool> usedDualSense;   // juegos que en esta sesión usaron el DualSense
    auto applyProfile = [&](bool force) {
        std::string matched;
        ds5::ProfileValues v = profiles.resolve(currentGame, &matched);
        if (autoOverride >= 0) v.mode = autoOverride;
        if (!force && matched == currentProfile) return;
        currentProfile = matched;
        // Los valores de línea de comandos (--gain) multiplican al perfil
        profSpeakerGain = v.speakerGain;
        profHapticGain = v.hapticGain * (opt.gainSet ? opt.gain : 1.0f);
        bool maxChanged = profMaxVolume.exchange(v.maxSpeakerVolume) != v.maxSpeakerVolume;
        lowBatteryPct = profiles.lowBattery;
        kAutoWaitMs = DWORD(profiles.autoWaitSec) * 1000;
        shakeOn = profiles.shake;
        psStatsOn = profiles.psButtonStats;
        if (!opt.noTray && profiles.statsHotkey != statsHotkey) { statsHotkey = profiles.statsHotkey; applyHotkey(statsHotkey); }
        simHapOn = v.simHaptics && !opt.noHaptics;
        simSpkOn = v.simSpeaker && speakerOn;
        simSens = v.sensitivity;
        fastSim = profiles.fastHaptics;
        voiceOn = profiles.voice;
        voiceVolume = profiles.voiceVolume;
        restoreAudioOn = profiles.restoreAudio;
        const int wantMode = opt.mode >= 0 ? opt.mode : v.mode;
        std::printf(tr("\nPerfil %s%s%s: modo %s, parlante x%.1f, háptica x%.1f%s\n", "\nProfile %s%s%s: mode %s, speaker x%.1f, haptics x%.1f%s\n"),
                    matched.empty() ? "general" : ("[" + matched + "]").c_str(),
                    currentGame.empty() ? "" : tr(" para ", " for "), currentGame.c_str(), ds5::padModeName(wantMode),
                    v.speakerGain, v.hapticGain, v.maxSpeakerVolume ? tr(", volumen del mando al máximo", ", controller volume at maximum") : "");
        std::fflush(stdout);
        if (wantMode != padMode) switchMode(wantMode);
        else if (maxChanged) replayGameState();   // reenviar el volumen con el ajuste nuevo
    };
    {
        std::string text;
        if (readIni(text, iniTime)) profiles = ds5::ProfileSet::parse(text);
        applyProfile(true);
    }
    // Captura del sonido del juego para la háptica y el parlante simulados (modo Xbox/PS4)
    ds5::ProcessCapture procCap;
    ds5::LoopbackCapture outCap;
    bool simRunning = false;
    DWORD simPid = 0, simFailedPid = 0;
    std::string simSource;
    std::vector<float> simHap, simSpk;
    auto simFeed = [&](const float* f, size_t n, unsigned rate) {
        if (!simActive) return;
        ds5::SimFx::Config c;
        c.haptics = simHapOn;
        c.speaker = simSpkOn;
        c.sensitivity = simSens;
        c.hapticGain = profHapticGain;
        c.speakerGain = profSpeakerGain;
        simFx.configure(c);
        simSpk.clear();
        simFx.process(f, n, rate, simHap, simSpk);
        haptics->feed(simHap.data(), n, rate);
#ifdef DS5_WITH_OPUS
        // Mismo margen que con un juego de DualSense: con menos, los paquetes de audio que llegan
        // juntos se descartaban y el sonido se entrecortaba
        if (c.speaker && !simSpk.empty()) speakerFifo.push(simSpk.data(), simSpk.size() / 2);
#endif
    };
    auto stopSim = [&] {
        if (!simRunning) return;
        simActive = false;
        procCap.stop();
        outCap.stop();
        simRunning = false;
    };
    auto updateSim = [&] {
        const bool want = padMode != ds5::kModePs5 && (simHapOn || simSpkOn) && currentPid != 0 && !btLost;
        if (want && simRunning && simPid == currentPid) return;
        stopSim();
        if (!want || currentPid == simFailedPid) return;
        simPid = currentPid;
        simActive = true;
        if (procCap.start(simPid, simFeed)) {
            simSource = tr("solo el sonido del juego", "game sound only");
        } else if (outCap.start(L"", -1, -1, false, simFeed, [&](const std::string& m) {
                       std::lock_guard<std::mutex> lg(logMu);
                       std::printf("\n%s\n", m.c_str());
                   })) {
            simSource = tr("todo el sonido de Windows (este Windows no permite capturar un solo programa)",
                           "all Windows sound (this Windows can't capture a single program)");
        } else {
            simActive = false;
            simFailedPid = simPid;
            std::lock_guard<std::mutex> lg(logMu);
            std::printf(tr("\nNo se pudo capturar el sonido del juego: sin háptica ni parlante simulados.\n",
                           "\nCould not capture the game's sound: no simulated haptics or speaker.\n"));
            return;
        }
        simRunning = true;
        if (simSpkOn) routeSpeaker();
        std::lock_guard<std::mutex> lg(logMu);
        std::printf(tr("\nHáptica simulada%s a partir de %s (sensibilidad %d).\n", "\nSimulated haptics%s from %s (sensitivity %d).\n"),
                    simSpkOn ? tr(", y los sonidos agudos por el parlante,", ", plus high-pitched sounds on the speaker,") : "", simSource.c_str(), simSens.load());
    };

    bool wasImported = false;
    DWORD importedSince = 0;
    while (!g_stop) {
        Sleep(500);
        // Agitar el mando: siguiente modo (xbox -> ps4 -> ps5 -> xbox), anotado en el perfil del juego
        if (shakeRequests.exchange(0) && shakeOn && opt.mode < 0) {
            const int cur = padMode;
            const int next = cur == ds5::kModeXbox ? ds5::kModePs4 : cur == ds5::kModePs4 ? ds5::kModePs5 : ds5::kModeXbox;
            autoOverride = next;
            autoWatching = false;
            std::printf(tra("\nMando agitado: paso a modo %s", "\nController shaken: switching to %s mode", "\nAgitaste el joystick: pasamos a modo %s"),
                        ds5::padModeName(next));
            if (!currentGame.empty()) {
                std::string sec = (!currentProfile.empty() && currentProfile != "?") ? currentProfile : ds5::ProfileSet::baseName(currentGame);
                if (appendIni(ds5::shakeProfileBlock(sec, next))) {
                    std::printf(tr(" y lo anoto en el perfil [%s].\n", " and saving it in the [%s] profile.\n"), sec.c_str());
                    std::string t2;
                    if (readIni(t2, iniTime)) profiles = ds5::ProfileSet::parse(t2);
                } else {
                    std::printf(".\n");
                }
                std::printf(tr("Si el juego no toma el mando, reinicialo.\n", "If the game does not pick up the controller, restart it.\n"));
            } else {
                std::printf(".\n");
            }
            applyProfile(true);
        }
        updateSim();
        {
            // Restaurar la salida/micrófono de audio si Windows eligió al mando recién aparecido
            bool imp = server.imported();
            if (imp && !wasImported) importedSince = GetTickCount();
            wasImported = imp;
        }
        if (tick % 4 == 2 && restoreAudioOn) {
            bool watching = importedSince && GetTickCount() - importedSince < 60000;
            std::wstring done = audioGuard.poll(watching);
            if (!done.empty())
                std::printf(tr("\nWindows había elegido al mando como dispositivo de audio: vuelvo a %s.\n", "\nWindows had picked the controller as an audio device: switching back to %s.\n"),
                            narrow(done).c_str());
        }
        if (++tick % 4 == 0) {   // cada 2 s
            std::string text;
            FILETIME t{};
            if (readIni(text, t) && CompareFileTime(&t, &iniTime) != 0) {
                iniTime = t;
                profiles = ds5::ProfileSet::parse(text);
                std::printf(tr("\nds5haptics.ini cambió: perfiles recargados.\n", "\nds5haptics.ini changed: profiles reloaded.\n"));
                applyProfile(true);
            }
            // Se cerró el juego: volver al modo general (DualSense, barra azul)
            if (currentPid && !processAlive(currentPid)) {
                std::printf(tr("\nSe cerró %s: vuelvo al perfil general.\n", "\n%s closed: back to the general profile.\n"), currentGame.c_str());
                closingGame = true;
                currentGame.clear();
                currentPid = 0;
                autoWatching = false;
                autoOverride = -1;
                applyProfile(true);
                closingGame = false;
            }
            bool full = false;
            DWORD fgPid = 0;
            std::string fg = foregroundExe(&full, &fgPid);
            std::string fgMatch;
            profiles.resolve(fg, &fgMatch);
            // Cambiar solo por una app con perfil o a pantalla completa (un juego): pasar un
            // momento al navegador no le quita el perfil al juego que está corriendo.
            if (!ignoredForeground(fg) && (full || !fgMatch.empty()) &&
                (ds5::ProfileSet::lower(fg) != ds5::ProfileSet::lower(currentGame) || fgPid != currentPid)) {
                currentGame = fg;
                currentPid = fgPid;
                autoOverride = -1;
                autoWatching = false;
                std::printf(tr("\nJuego en primer plano: %s\n", "\nGame in the foreground: %s\n"), fg.c_str());
                applyProfile(true);
                // Modo automático (xbox o ps4): solo para juegos sin "modo" en su perfil, con el modo general en ps5
                if (profiles.autoXbox && opt.mode < 0 && profiles.explicitMode(fg) < 0 &&
                    profiles.general.mode == ds5::kModePs5 && padMode == ds5::kModePs5 &&
                    !usedDualSense.count(ds5::ProfileSet::baseName(fg))) {
                    // ¿Usó el DualSense desde que arrancó el juego? Entonces lo admite.
                    unsigned long long age = processAgeMs(fgPid);
                    DWORD nowA = GetTickCount();
                    bool usedSinceStart = lastDsActivity && age && (nowA - lastDsActivity) <= age + 2000;
                    // Si el DualSense virtual apareció después de que arrancó el juego (por un cambio
                    // de modo), el juego pudo no verlo: no se puede juzgar, mejor no tocar nada.
                    bool dsSinceStart = age && (nowA - dsPresentSince.load()) >= age + 3000;
                    if (usedSinceStart) usedDualSense[ds5::ProfileSet::baseName(fg)] = true;
                    else if (dsSinceStart) {
                        autoWatching = true;
                        autoSince = nowA;
                        std::printf(tr("Juego sin perfil: miro %u s si usa el DualSense; si no, paso a modo %s.\n", "Game without a profile: watching for %u s whether it uses the DualSense; if not, switching to %s mode.\n"),
                                    unsigned(kAutoWaitMs / 1000), ds5::padModeName(profiles.autoMode));
                    }
                }
            }
        }
        // Actividad del juego sobre el DualSense virtual (pedidos de calibración, luces, gatillos)
        {
            unsigned c = vds.outputs() + vds.featureRequests();
            if (c != lastDsCount) { lastDsCount = c; lastDsActivity = GetTickCount(); }
        }
        if (autoWatching) {
            DWORD nowA = GetTickCount();
            if (lastDsActivity && lastDsActivity >= autoSince) {
                autoWatching = false;
                usedDualSense[ds5::ProfileSet::baseName(currentGame)] = true;
                std::printf(tr("\n%s usa el DualSense: queda en modo PS5.\n", "\n%s uses the DualSense: staying in PS5 mode.\n"), currentGame.c_str());
            } else if (nowA - autoSince >= kAutoWaitMs && !usesGamepadApi(currentPid)) {
                autoWatching = false;
                std::printf(tr("\n%s no usa mandos: no cambio nada.\n", "\n%s does not use controllers: nothing changed.\n"), currentGame.c_str());
            } else if (nowA - autoSince >= kAutoWaitMs) {
                autoWatching = false;
                autoOverride = profiles.autoMode == ds5::kModePs4 ? ds5::kModePs4 : ds5::kModeXbox;
                std::printf(tra("\n%s no usó el DualSense en %u s: paso a modo %s", "\n%s did not use the DualSense in %u s: switching to %s mode",
                                "\n%s no banca el DualSense (%u s sin usarlo): lo hacemos pasar por un mando %s"), currentGame.c_str(),
                            unsigned(kAutoWaitMs / 1000), ds5::padModeName(autoOverride));
                if (appendIni(ds5::autoProfileBlock(currentGame, autoOverride))) {
                    std::printf(tr(" y lo anoto en ds5haptics.ini como [%s].\n", " and saving it in ds5haptics.ini as [%s].\n"),
                                ds5::ProfileSet::baseName(currentGame).c_str());
                    // Evitar que la recarga del archivo cuente como un cambio hecho a mano
                    std::string t2;
                    if (readIni(t2, iniTime)) profiles = ds5::ProfileSet::parse(t2);
                } else {
                    std::printf(tr(" (no pude anotarlo en ds5haptics.ini).\n", " (could not save it in ds5haptics.ini).\n"));
                }
                std::printf(tr("Si el juego no toma el mando, reinicialo.\n", "If the game does not pick up the controller, restart it.\n"));
                applyProfile(true);
            }
        }
        DWORD now = GetTickCount();
        unsigned in = inputs.load();
        double hz = (in - lastInputs) * 1000.0 / std::max<DWORD>(1, now - lastT);
        lastInputs = in;
        lastT = now;
        float pk = std::min(1.0f, haptics->takePeak());
        if (!opt.noTray) {
            ds5ui::Stats st;
            const int pmS = padMode;
            const bool confS = pmS == ds5::kModePs4 ? vps4.configured() : pmS == ds5::kModeXbox ? vxbox.configured() : vds.configured();
            st.winState = server.imported() ? (confS ? tr("mando virtual conectado", "virtual controller connected") : tr("conectando el mando virtual...", "connecting the virtual controller..."))
                                            : tr("esperando a usbip-win2", "waiting for usbip-win2");
            st.btOk = !btLost;
            st.inputHz = btLost ? 0 : hz;
            st.mode = pmS;
            st.game = currentGame;
            st.profile = currentProfile == "?" ? "" : currentProfile;
            st.battery = batteryPct;
            st.charging = batteryCharging;
            st.full = batteryCharging && batteryPct >= 100;
            st.lowWarn = micPulsing;
            st.hapticState = opt.noHaptics ? "desactivada"
                             : (pmS != ds5::kModePs5 && simRunning) ? tr("simulada", "simulated")
                             : vds.speakerStreaming() ? (hapticActive ? "activa" : tr("en silencio", "silent")) : tr("el juego no manda", "game sends none");
            st.hapticLevel = (pmS != ds5::kModePs5 && simRunning) ? simFx.level() : pk;
            st.hapticGain = profHapticGain;
            unsigned wc = writeCount.exchange(0);
            unsigned long long ws = writeUsSum.exchange(0);
            st.writeMs = wc ? ws / 1000.0 / wc : -1;
            if (pmS == ds5::kModePs5 && vds.speakerStreaming() && hapticActive) {
                // Lo que agrega ds5haptics: lo que espera en el búfer + juntar un reporte + enviarlo
                double fill = opt.fast ? ds5::kReportPeriodSec * 1000 / 2 : 0;
                st.ownLatencyMs = haptics->bufferedMs() + fill + (st.writeMs >= 0 ? st.writeMs : 0);
            }
            st.gaps = hapticGaps;
            st.speakerEnabled = speakerOn;
            st.speakerStreaming = vds.speakerStreaming() || (pmS != ds5::kModePs5 && simRunning && simSpkOn);
            st.gameVolume = lastGameVolume;
            st.volumeForced = profMaxVolume;
            st.speakerGain = profSpeakerGain;
            st.trigR = trigR;
            st.trigL = trigL;
            st.gameOutputs = vds.outputs();
            tray.update(st);
        }
        if (opt.quiet) continue;
        int bars = int(pk * 10 + 0.5f);
        std::string meter = std::string(size_t(bars), '#') + std::string(size_t(10 - bars), '.');
        const char* hstate = opt.noHaptics ? "desactivada"
                             : vds.speakerStreaming() ? (hapticActive ? "activa" : "silencio")
                                                      : tr("sin audio", "no audio");
        char lat[64] = "";
        if (vds.speakerStreaming()) {
            if (opt.fast) std::snprintf(lat, sizeof(lat), tr(" | huecos %u%s", " | gaps %u%s"), hapticGaps.load(),
                                        speakerOn ? (speakerErrors ? tr(" | parlante: ERROR Opus", " | speaker: Opus ERROR") : tr(" | parlante", " | speaker")) : "");
            else std::snprintf(lat, sizeof(lat), tr(" | búfer %2.0fms cortes %u", " | buffer %2.0fms dropouts %u"), haptics->bufferedMs(), haptics->underruns());
        }
        const int pm = padMode;
        const bool conf = pm == ds5::kModePs4 ? vps4.configured() : pm == ds5::kModeXbox ? vxbox.configured() : vds.configured();
        const unsigned outs = pm == ds5::kModePs4 ? vps4.outputs() : pm == ds5::kModeXbox ? vxbox.outputs() : vds.outputs();
        char batt[16] = "";
        if (batteryPct >= 0) std::snprintf(batt, sizeof(batt), " %d%%%s", batteryPct.load(), batteryCharging ? "+" : "");
        const char* state = server.imported() ? (conf ? "conectado" : "enumerando") : tr("esperando", "waiting");
        // Línea corta (cabe en 100 columnas) para que la consola no la parta
        if (pm != ds5::kModePs5)
            std::printf(tr("\r %s | modo %s | BT %s %3.0fHz%s | salidas %-5u%s | %s golpes %u parlante %u      ", "\r %s | mode %s | BT %s %3.0fHz%s | outputs %-5u%s | %s hits %u speaker %u      "),
                        state, ds5::padModeName(pm), btLost ? "PERDIDO" : "ok", hz, batt, outs,
                        outErrors ? (" err " + std::to_string(outErrors.load())).c_str() : "",
                        simRunning ? tr("háptica simulada", "simulated haptics") : tr("vibración clásica", "classic rumble"), simFx.impacts(), simFx.speakerHits());
        else
            std::printf(tr("\r %s | BT %s %3.0fHz%s | salidas %-5u%s | háptica [%s] %-9s%s    ", "\r %s | BT %s %3.0fHz%s | outputs %-5u%s | haptics [%s] %-9s%s    "),
                        state, btLost ? "PERDIDO" : "ok", hz, batt, outs,
                        outErrors ? (" err " + std::to_string(outErrors.load())).c_str() : "",
                        meter.c_str(), hstate, lat);
        std::fflush(stdout);
    }

    std::printf(tr("\nCerrando el mando virtual...\n", "\nClosing the virtual controller...\n"));
    simActive = false;
    procCap.stop();
    outCap.stop();
    trayPtr = nullptr;
    tray.stop();
    server.stop();
    btThread.join();
    hapticThread.join();
    sayCv.notify_all();
    voiceThread.join();
    CoUninitialize();
    WSACleanup();
    return 0;
}

static void usage() {
    std::printf("ds5haptics %s\n", DS5_VERSION);
    std::printf(
        tr("ds5haptics — háptica del DualSense por Bluetooth\n\n"
        "Uso:\n"
        "  ds5haptics list                      Lista los DualSense conectados\n"
        "  ds5haptics info                      Verifica modo completo y escritura (barra verde)\n"
        "  ds5haptics test sine [Hz] [seg]      Seno (por defecto 150 Hz, 3 s)\n"
        "  ds5haptics test sweep [seg]          Barrido 40-400 Hz\n"
        "  ds5haptics test pulses [seg]         Golpes alternando izquierda/derecha\n"
        "  ds5haptics play archivo.wav          Reproduce un WAV como háptica\n"
        "  ds5haptics raw archivo.raw           Crudo s8 estéreo 3000 Hz (formato SAxense)\n"
        "  ds5haptics sources                   Lista las salidas de audio de Windows\n"
        "  ds5haptics live                      Convierte en vivo lo que suena en la PC en háptica\n"
        "  ds5haptics virtual                   Crea un DualSense USB virtual (requiere usbip-win2)\n"
        "  ds5haptics background               Igual que 'virtual --speaker', sin ventana\n"
        "  ds5haptics stop                      Detiene el que corre en segundo plano\n"
        "  ds5haptics autostart on|off|status   Arrancar solo al iniciar Windows\n"
        "  ds5haptics hidhide-setup             Configura HidHide automáticamente\n"
        "  ds5haptics config                    Abre los perfiles por juego (modo, parlante, háptica, batería)\n"
        "  ds5haptics battery                   Muestra la batería del mando\n\n"
        "Opciones generales:\n"
        "  --device N      Usar el mando N de 'list'\n"
        "  --gain G        Ganancia extra (1.0; en vivo sin AGC: 2.0)\n"
        "  --ch L,R        Canales a usar, desde 1 (WAV de 4 canales: 3,4; en vivo: 1,2)\n"
        "  --loop          Repetir el archivo hasta Ctrl+C\n"
        "  --setreport     Enviar con HidD_SetOutputReport en vez de WriteFile\n"
        "  --seq           Experimental: numerar reportes en el nibble alto del byte 1\n"
        "  --no-lightbar   No cambiar el color de la barra de luz\n"
        "  --lang es|en    Idioma (por defecto, el de Windows)\n\n"
        "Opciones de 'live':\n"
        "  --source N      Salida a capturar, de 'sources' (por defecto la predeterminada)\n"
        "  --lowpass Hz    Pasa-bajos para quedarse con graves y golpes (300; 0 = desactivado)\n"
        "  --gate X        Por debajo de este nivel no vibra (0.001 con AGC, 0.01 sin; 0 = desactivada)\n"
        "  --latency ms    Pre-búfer: menos = más rápido, más = menos cortes (25)\n"
        "  --level X       Intensidad fija de la háptica, 0.05-1.0 (0.9 = 90%%)\n"
        "  --agc-release ms  Qué tan rápido se adapta cuando baja el volumen (1000)\n"
        "  --no-agc        Sin intensidad fija: la háptica sigue al volumen del audio\n"
        "  --no-volcomp    No compensar el volumen de Windows\n\n"
        "Opciones de 'virtual':\n"
        "  --mode M        ps5, ps4 o xbox: cómo ve el juego al mando (si no, lo deciden los perfiles)\n"
        "  --no-attach     No ejecutar 'usbip attach' (conectarlo a mano)\n"
        "  --wait          Esperar a que se prenda el mando en vez de salir\n"
        "  --no-tray       Sin ícono en la bandeja ni cuadro de estadísticas\n"
        "  --log archivo   Escribir la salida en un archivo (sin línea de estado)\n"
        "  --no-output     Diagnóstico: no reenviar al mando las salidas del juego\n"
        "  --no-haptics    No convertir el audio del juego en háptica\n"
        "  --strip-audio-flags  Diagnóstico: no reenviar los ajustes de audio del juego\n"
        "  --gain G        Ganancia de la háptica nativa (1.0)\n"
        "  --speaker       Experimental: también el sonido del parlante del mando\n"
        "  --normal        Modo con pre-búfer (el predeterminado es el directo, de menor latencia)\n"
        "  --report-bytes N  Experimental, modo directo: 32 = reportes cada 5,3 ms (por defecto 64)\n"
        "  --latency ms    Pre-búfer del modo normal (20)\n", "ds5haptics — DualSense over Bluetooth with haptics\n\nUsage:\n  ds5haptics list                      List connected DualSense controllers\n  ds5haptics info                      Check full mode and writing (green light bar)\n  ds5haptics test sine [Hz] [sec]      Sine (default 150 Hz, 3 s)\n  ds5haptics test sweep [sec]          40-400 Hz sweep\n  ds5haptics test pulses [sec]         Taps alternating left/right\n  ds5haptics play file.wav             Play a WAV file as haptics\n  ds5haptics raw file.raw              Raw s8 stereo 3000 Hz (SAxense format)\n  ds5haptics sources                   List Windows audio outputs\n  ds5haptics live                      Turn whatever plays on the PC into haptics\n  ds5haptics virtual                   Create a virtual USB DualSense (needs usbip-win2)\n  ds5haptics background               Same as 'virtual --speaker', without a window\n  ds5haptics stop                      Stop the background instance\n  ds5haptics autostart on|off|status   Start by itself with Windows\n  ds5haptics hidhide-setup             Set up HidHide automatically\n  ds5haptics config                    Open per-game profiles (mode, speaker, haptics, battery)\n  ds5haptics battery                   Show the controller battery\n\nGeneral options:\n  --device N      Use controller N from 'list'\n  --gain G        Extra gain (1.0; live without AGC: 2.0)\n  --ch L,R        Channels to use, from 1 (4-channel WAV: 3,4; live: 1,2)\n  --loop          Repeat the file until Ctrl+C\n  --setreport     Send with HidD_SetOutputReport instead of WriteFile\n  --seq           Experimental: number reports in the high nibble of byte 1\n  --no-lightbar   Do not change the light bar color\n  --lang es|en    Language (default: the Windows language)\n\n'live' options:\n  --source N      Output to capture, from 'sources' (default: the default output)\n  --lowpass Hz    Low-pass to keep bass and hits (300; 0 = off)\n  --gate X        Below this level there is no vibration (0.001 with AGC, 0.01 without; 0 = off)\n  --latency ms    Pre-buffer: less = faster, more = fewer dropouts (25)\n  --level X       Fixed haptics intensity, 0.05-1.0 (0.9 = 90%%)\n  --agc-release ms  How fast it adapts when the volume drops (1000)\n  --no-agc        No fixed intensity: haptics follow the audio volume\n  --no-volcomp    Do not compensate the Windows volume\n\n'virtual' options:\n  --mode M        ps5, ps4 or xbox: how games see the controller (otherwise profiles decide)\n  --no-attach     Do not run 'usbip attach' (attach it by hand)\n  --wait          Wait for the controller to be turned on instead of exiting\n  --no-tray       No tray icon or stats panel\n  --log file      Write the output to a file (no status line)\n  --no-output     Diagnostics: do not forward game outputs to the controller\n  --no-haptics    Do not turn game audio into haptics\n  --strip-audio-flags  Diagnostics: do not forward the game's audio settings\n  --gain G        Native haptics gain (1.0)\n  --speaker       Experimental: the controller speaker sound too\n  --normal        Pre-buffered mode (the default is direct mode, with lower latency)\n  --report-bytes N  Experimental, direct mode: 32 = a report every 5.3 ms (default 64)\n  --latency ms    Pre-buffer for normal mode (20)\n"));
}

int wmain(int argc, wchar_t** wargv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onCtrl, TRUE);

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(narrow(wargv[i]));

    // Idioma: --lang, si no "idioma" en ds5haptics.ini, si no el de Windows
    {
        int lang = -1;
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--lang") {
                std::string l = ds5::ProfileSet::lower(args[i + 1]);
                lang = l.rfind("en", 0) == 0 ? 1 : (l == "argento" || l == "es-ar" || l == "ar") ? 2 : 0;
            }
        if (lang < 0) {
            std::string text;
            FILETIME t{};
            if (readIni(text, t)) lang = ds5::ProfileSet::parse(text).language;
        }
        if (lang < 0) lang = (GetUserDefaultUILanguage() & 0x3FF) == 0x0A ? 0 : 1;   // 0x0A = LANG_SPANISH
        ds5::setEnglish(lang == 1);
        ds5::setArgento(lang == 2);
    }

    Options opt;
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) { std::printf(tr("Falta el valor de %s\n", "Missing value for %s\n"), name); std::exit(2); }
            return args[++i];
        };
        if (a == "--device") opt.device = std::atoi(next("--device").c_str());
        else if (a == "--gain") { opt.gain = float(std::atof(next("--gain").c_str())); opt.gainSet = true; }
        else if (a == "--source") opt.source = std::atoi(next("--source").c_str());
        else if (a == "--lowpass") { opt.lowpass = float(std::atof(next("--lowpass").c_str())); opt.lowpassSet = true; }
        else if (a == "--gate") { opt.gate = float(std::atof(next("--gate").c_str())); opt.gateSet = true; }
        else if (a == "--no-agc") opt.agc = false;
        else if (a == "--level") opt.level = std::clamp(float(std::atof(next("--level").c_str())), 0.05f, 1.0f);
        else if (a == "--agc-release") opt.agcRelease = std::max(50.0f, float(std::atof(next("--agc-release").c_str())));
        else if (a == "--no-volcomp") opt.volComp = false;
        else if (a == "--no-attach") opt.noAttach = true;
        else if (a == "--no-output") opt.noOutput = true;
        else if (a == "--strip-audio-flags") opt.stripAudioFlags = true;
        else if (a == "--no-haptics") opt.noHaptics = true;
        else if (a == "--fast") opt.fast = true;
        else if (a == "--normal") opt.fast = false;
        else if (a == "--speaker") opt.speaker = true;
        else if (a == "--mode") {
            opt.mode = ds5::parsePadMode(next("--mode"));
            if (opt.mode < 0) { std::printf(tr("--mode debe ser ps5, ps4 o xbox\n", "--mode must be ps5, ps4 or xbox\n")); return 2; }
        }
        else if (a == "--wait") opt.wait = true;
        else if (a == "--no-tray") opt.noTray = true;
        else if (a == "--lang") next("--lang");   // ya se aplicó al arrancar
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--log") { opt.logFile = next("--log"); opt.quiet = true; }
        else if (a == "--report-bytes") {
            opt.reportBytes = std::atoi(next("--report-bytes").c_str()) & ~1;
            if (opt.reportBytes < 16 || opt.reportBytes > 64) { std::printf(tr("--report-bytes debe estar entre 16 y 64\n", "--report-bytes must be between 16 and 64\n")); return 2; }
        }
        else if (a == "--latency") { opt.latency = std::max(11.0f, float(std::atof(next("--latency").c_str()))); opt.latencySet = true; }
        else if (a == "--ch") {
            std::string v = next("--ch");
            if (std::sscanf(v.c_str(), "%d,%d", &opt.chL, &opt.chR) != 2) { std::printf(tr("--ch espera L,R\n", "--ch expects L,R\n")); return 2; }
            opt.chL--; opt.chR--;
        }
        else if (a == "--loop") opt.loop = true;
        else if (a == "--setreport") opt.setOutputReport = true;
        else if (a == "--seq") opt.seq = true;
        else if (a == "--no-lightbar") opt.noLightbar = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else pos.push_back(a);
    }
    if (pos.empty()) { usage(); return 0; }

    const std::string& cmd = pos[0];
    auto num = [&](size_t i, double def) { return pos.size() > i ? std::atof(pos[i].c_str()) : def; };
    float amp = std::min(1.0f, 0.8f * opt.gain);

    if (cmd == "list") return cmdList();
    if (cmd == "info") return cmdInfo(opt);
    if (cmd == "sources") return cmdSources();
    if (cmd == "live") return cmdLive(opt);
    if (cmd == "virtual") {
        if (!opt.logFile.empty()) {
            FILE* f = _wfreopen(widen(opt.logFile).c_str(), L"w", stdout);
            if (f) { setvbuf(stdout, nullptr, _IOLBF, 1024); std::printf(tr("ds5haptics en segundo plano\n", "ds5haptics in the background\n")); }
        }
        return cmdVirtual(opt);
    }
    if (cmd == "background") return cmdBackground(std::vector<std::string>(pos.begin() + 1, pos.end()));
    if (cmd == "stop") return cmdStop();
    if (cmd == "autostart") return cmdAutostart(pos.size() > 1 ? pos[1] : "status");
    if (cmd == "hidhide-setup") return cmdHidHideSetup();
    if (cmd == "config") return cmdConfig();
    if (cmd == "version" || cmd == "--version") { std::printf("ds5haptics %s\n", DS5_VERSION); return 0; }
    if (cmd == "battery" || cmd == "bateria") return cmdBattery(opt);

    std::vector<int8_t> pcm;
    if (cmd == "test") {
        std::string kind = pos.size() > 1 ? pos[1] : "sine";
        if (kind == "sine") pcm = ds5::genSine(num(2, 150), num(3, 3), amp, 0);
        else if (kind == "sweep") pcm = ds5::genSweep(40, 400, num(2, 4), amp);
        else if (kind == "pulses") pcm = ds5::genPulses(num(2, 4), amp);
        else { std::printf(tr("Prueba desconocida: %s\n", "Unknown test: %s\n"), kind.c_str()); return 2; }
    } else if (cmd == "play" && pos.size() > 1) {
        ds5::AudioBuffer ab;
        std::string err;
        if (!ds5::loadWav(pos[1], ab, err)) { std::printf("WAV: %s\n", err.c_str()); return 1; }
        int l = opt.chL, r = opt.chR;
        if (l < 0) { if (ab.channels >= 4) { l = 2; r = 3; } else { l = 0; r = ab.channels > 1 ? 1 : 0; } }
        std::printf(tr("WAV: %u Hz, %u canales, %.2f s -> usando canales %d y %d\n", "WAV: %u Hz, %u channels, %.2f s -> using channels %d and %d\n"),
                    ab.sampleRate, ab.channels, double(ab.frames()) / ab.sampleRate, l + 1, r + 1);
        std::vector<float> st;
        ds5::pickStereo(ab, l, r, st);
        pcm = ds5::toS8(ds5::resampleStereo(st, ab.sampleRate, ds5::kSampleRate), opt.gain);
    } else if (cmd == "raw" && pos.size() > 1) {
        std::string err;
        if (!ds5::loadRawS8(pos[1], pcm, err)) { std::printf("RAW: %s\n", err.c_str()); return 1; }
        if (opt.gain != 1.0f) for (auto& s : pcm) s = int8_t(std::clamp(int(s * opt.gain), -127, 127));
    } else {
        usage();
        return 2;
    }
    if (pcm.empty()) { std::printf(tr("No hay muestras para enviar.\n", "No samples to send.\n")); return 1; }
    return playBuffer(pcm, opt);
}

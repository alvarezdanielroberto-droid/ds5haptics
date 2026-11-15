// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// wasapi_capture.h — Captura "loopback" de un dispositivo de salida de Windows (WASAPI).
// Graba lo que suena por ese dispositivo, sin instalar drivers ni cables virtuales.
// Solo Windows. Incluir después de <windows.h>.
#pragma once
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmreg.h>
#include <propidl.h>

#include <algorithm>
#include "i18n.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#endif

namespace ds5 {

// Puntero COM mínimo (evita depender de WRL/ATL, compila también con MinGW).
template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T** put() { reset(); return &p_; }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

struct AudioEndpoint {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

// PKEY_Device_FriendlyName definido acá para no depender de initguid/devpkey.
static const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

class LoopbackCapture {
public:
    // Recibe frames estéreo float intercalados, su cantidad y la tasa de muestreo.
    using Callback = std::function<void(const float*, size_t, unsigned)>;
    using LogFn = std::function<void(const std::string&)>;

    ~LoopbackCapture() { stop(); }

    static std::vector<AudioEndpoint> listOutputs() {
        std::vector<AudioEndpoint> out;
        Com<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), en.putVoid()))) return out;
        std::wstring defId;
        {
            Com<IMMDevice> def;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, def.put()))) defId = deviceId(def.get());
        }
        Com<IMMDeviceCollection> col;
        if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.put()))) return out;
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            Com<IMMDevice> dev;
            if (FAILED(col->Item(i, dev.put()))) continue;
            AudioEndpoint ep;
            ep.id = deviceId(dev.get());
            ep.name = friendlyName(dev.get());
            ep.isDefault = (ep.id == defId);
            out.push_back(ep);
        }
        return out;
    }

    // deviceId vacío = dispositivo de salida predeterminado (sigue al predeterminado si cambia).
    // chL/chR: canales a usar (desde 0); -1 = automático (0 y 1).
    // compensateVolume: deshace el volumen de Windows de esa salida, para que la
    // háptica no cambie al subir o bajar el volumen del sistema.
    bool start(const std::wstring& deviceId, int chL, int chR, bool compensateVolume, Callback cb, LogFn log) {
        stop();
        deviceId_ = deviceId; chL_ = chL; chR_ = chR; cb_ = std::move(cb); log_ = std::move(log);
        compensate_ = compensateVolume;
        running_ = true;
        ready_ = 0;
        thread_ = std::thread([this] { run(); });
        while (ready_ == 0) Sleep(5);   // esperar el primer intento de apertura
        return ready_ > 0;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    std::string formatDescription() const { return formatDesc_; }

    // Volumen actual de Windows para la salida capturada, en dB (0 = máximo).
    float volumeDb() const { return volumeDb_.load(std::memory_order_relaxed); }
    bool muted() const { return muted_.load(std::memory_order_relaxed); }
    bool volumeKnown() const { return volumeKnown_.load(std::memory_order_relaxed); }

private:
    static std::wstring deviceId(IMMDevice* dev) {
        LPWSTR s = nullptr;
        std::wstring r;
        if (SUCCEEDED(dev->GetId(&s)) && s) { r = s; CoTaskMemFree(s); }
        return r;
    }
    static std::wstring friendlyName(IMMDevice* dev) {
        std::wstring r = L"(sin nombre)";
        Com<IPropertyStore> props;
        if (FAILED(dev->OpenPropertyStore(STGM_READ, props.put()))) return r;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) r = pv.pwszVal;
        PropVariantClear(&pv);
        return r;
    }

    void log(const std::string& s) { if (log_) log_(s); }

    // Abre el dispositivo y captura hasta error o stop(). Devuelve false si no pudo abrir.
    bool session() {
        Com<IMMDeviceEnumerator> en;
        Com<IMMDevice> dev;
        Com<IAudioClient> client;
        Com<IAudioCaptureClient> cap;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), en.putVoid());
        if (FAILED(hr)) { log(tr("WASAPI: no se pudo crear el enumerador", "WASAPI: could not create the device enumerator")); return false; }
        hr = deviceId_.empty() ? en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put())
                               : en->GetDevice(deviceId_.c_str(), dev.put());
        if (FAILED(hr)) { log(tr("WASAPI: dispositivo de salida no disponible", "WASAPI: output device not available")); return false; }
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
        if (FAILED(hr)) { log(tr("WASAPI: Activate falló", "WASAPI: Activate failed")); return false; }

        WAVEFORMATEX* wfx = nullptr;
        if (FAILED(client->GetMixFormat(&wfx)) || !wfx) { log(tr("WASAPI: GetMixFormat falló", "WASAPI: GetMixFormat failed")); return false; }
        const unsigned rate = wfx->nSamplesPerSec;
        const unsigned ch = wfx->nChannels;
        const unsigned bits = wfx->wBitsPerSample;
        const unsigned block = wfx->nBlockAlign;
        bool isFloat = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfx);
            isFloat = ext->SubFormat.Data1 == 3;  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
        }
        // 20 ms de búfer compartido en modo loopback
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, wfx, nullptr);
        CoTaskMemFree(wfx);
        if (FAILED(hr)) { log(tr("WASAPI: Initialize (loopback) falló", "WASAPI: Initialize (loopback) failed")); return false; }
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), cap.putVoid()))) {
            log(tr("WASAPI: GetService falló", "WASAPI: GetService failed")); return false;
        }
        if (!isFloat && bits != 16 && bits != 24 && bits != 32) { log(tr("WASAPI: formato no soportado", "WASAPI: unsupported format")); return false; }

        int l = chL_ >= 0 ? chL_ : 0;
        int r = chR_ >= 0 ? chR_ : (ch > 1 ? 1 : 0);
        if (l >= int(ch)) l = int(ch) - 1;
        if (r >= int(ch)) r = int(ch) - 1;

        formatDesc_ = narrowW(friendlyName(dev.get())) + " — " + std::to_string(rate) + " Hz, " +
                      std::to_string(ch) + " canales, " + (isFloat ? "float" : std::to_string(bits) + " bits") +
                      tr(" (usando canales ", " (using channels ") + std::to_string(l + 1) + tr(" y ", " and ") + std::to_string(r + 1) + ")";
        // Volumen del sistema para esta salida (opcional: si falla, se sigue sin compensar)
        Com<IAudioEndpointVolume> vol;
        if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, vol.putVoid()))) vol.reset();
        volumeKnown_ = bool(vol);
        float comp = 1.0f;          // compensación aplicada ahora
        float compTarget = 1.0f;    // compensación hacia la que se va (rampa, sin chasquidos)
        int volPoll = 0;
        auto readVolume = [&]() {
            if (!vol) return;
            float db = 0.0f;
            BOOL m = FALSE;
            if (SUCCEEDED(vol->GetMasterVolumeLevel(&db))) volumeDb_ = db;
            if (SUCCEEDED(vol->GetMute(&m))) muted_ = (m != FALSE);
            // Deshacer la atenuación, con tope de +40 dB
            compTarget = compensate_ ? std::min(100.0f, std::pow(10.0f, -std::min(0.0f, db) / 20.0f)) : 1.0f;
        };
        readVolume();
        comp = compTarget;

        if (FAILED(client->Start())) { log(tr("WASAPI: Start falló", "WASAPI: Start failed")); return false; }
        if (ready_ == 0) ready_ = 1;
        log(tr("Capturando: ", "Capturing: ") + formatDesc_);

        std::vector<float> st;
        while (running_) {
            Sleep(2);
            if (++volPoll >= 25) { volPoll = 0; readVolume(); }  // cada ~50 ms
            UINT32 pkt = 0;
            hr = cap->GetNextPacketSize(&pkt);
            if (FAILED(hr)) break;
            while (pkt > 0 && running_) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                st.resize(size_t(frames) * 2);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    std::fill(st.begin(), st.end(), 0.0f);
                } else {
                    // Rampa lineal de la compensación a lo largo del paquete
                    float c0 = comp, dc = frames ? (compTarget - comp) / float(frames) : 0.0f;
                    for (UINT32 i = 0; i < frames; ++i) {
                        const BYTE* f = data + size_t(i) * block;
                        float c = c0 + dc * float(i);
                        st[i * 2] = sampleAt(f, l, bits, isFloat) * c;
                        st[i * 2 + 1] = sampleAt(f, r, bits, isFloat) * c;
                    }
                }
                comp = compTarget;
                cap->ReleaseBuffer(frames);
                if (cb_) cb_(st.data(), frames, rate);
                hr = cap->GetNextPacketSize(&pkt);
                if (FAILED(hr)) break;
            }
            if (FAILED(hr)) break;
        }
        client->Stop();
        if (running_) log(tr("WASAPI: se perdió el dispositivo (¿cambió la salida predeterminada?). Reintentando...", "WASAPI: device lost (did the default output change?). Retrying..."));
        return true;
    }

    static float sampleAt(const BYTE* frame, int c, unsigned bits, bool isFloat) {
        const BYTE* p = frame + size_t(c) * (bits / 8);
        if (isFloat) { float v; std::memcpy(&v, p, 4); return v; }
        switch (bits) {
            case 16: { int16_t v; std::memcpy(&v, p, 2); return v / 32768.0f; }
            case 24: { int32_t v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8; return v / 8388608.0f; }
            default: { int32_t v; std::memcpy(&v, p, 4); return v / 2147483648.0f; }
        }
    }

    static std::string narrowW(const std::wstring& w) {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        bool first = true;
        while (running_) {
            bool opened = session();
            if (first && !opened) { ready_ = -1; break; }  // la primera apertura falló: avisar
            first = false;
            for (int i = 0; i < 20 && running_; ++i) Sleep(50);  // esperar 1 s y reabrir
        }
        CoUninitialize();
    }

    std::wstring deviceId_;
    int chL_ = -1, chR_ = -1;
    Callback cb_;
    LogFn log_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> ready_{0};
    bool compensate_ = true;
    std::atomic<float> volumeDb_{0.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> volumeKnown_{false};
    std::string formatDesc_;
};

}  // namespace ds5

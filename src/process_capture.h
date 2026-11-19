// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// process_capture.h — Captura el sonido de UN programa (el juego), sin la música, Discord ni
// el resto de Windows. Usa el "process loopback" de WASAPI (Windows 10 reciente y 11).
//
// La función ActivateAudioInterfaceAsync se carga en tiempo de ejecución desde
// Mmdevapi.dll: si la versión de Windows no la tiene o no admite el loopback por proceso,
// start() devuelve false y el que llama usa la captura de toda la salida de audio.
#pragma once
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace ds5 {

namespace pcap {
// Estructuras de audioclientactivationparams.h (copiadas para no depender de un SDK nuevo)
struct LoopbackParams { DWORD targetProcessId; int mode; };   // mode 0 = incluir el árbol del proceso
struct ActivationParams { int activationType; LoopbackParams loopback; };   // tipo 1 = loopback por proceso

static const IID kIidAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIidCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const IID kIidCompletion = {0x41D949AB, 0x9862, 0x444A, {0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB}};
static const IID kIidAgile = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
static const IID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

typedef HRESULT(WINAPI* ActivateFn)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                    IActivateAudioInterfaceAsyncOperation**);

class Completion : public IActivateAudioInterfaceCompletionHandler {
public:
    Completion() { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    virtual ~Completion() { if (done_) CloseHandle(done_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, kIidUnknown) || IsEqualIID(riid, kIidCompletion) || IsEqualIID(riid, kIidAgile)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hr = E_FAIL;
        IUnknown* unk = nullptr;
        if (SUCCEEDED(op->GetActivateResult(&hr, &unk)) && SUCCEEDED(hr) && unk) {
            unk->QueryInterface(kIidAudioClient, reinterpret_cast<void**>(&client_));
            unk->Release();
        }
        SetEvent(done_);
        return S_OK;
    }

    HANDLE done() const { return done_; }
    IAudioClient* take() { IAudioClient* c = client_; client_ = nullptr; return c; }

private:
    std::atomic<ULONG> refs_{1};
    HANDLE done_ = nullptr;
    IAudioClient* client_ = nullptr;
};
}  // namespace pcap

class ProcessCapture {
public:
    // Estéreo float intercalado, cantidad de frames, tasa (siempre 48000)
    using Callback = std::function<void(const float*, size_t, unsigned)>;

    ~ProcessCapture() { stop(); }

    // Empieza a capturar el sonido del proceso 'pid' (y de los que él lanzó).
    bool start(DWORD pid, Callback cb) {
        stop();
        cb_ = std::move(cb);
        pid_ = pid;
        running_ = true;
        ready_ = 0;
        thread_ = std::thread([this] { run(); });
        while (ready_ == 0) Sleep(5);
        if (ready_ < 0) { stop(); return false; }
        return true;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    DWORD pid() const { return pid_; }

private:
    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IAudioClient* client = activate();
        IAudioCaptureClient* cap = nullptr;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bool ok = false;
        if (client && ev) {
            WAVEFORMATEX f{};
            f.wFormatTag = WAVE_FORMAT_PCM;
            f.nChannels = 2;
            f.nSamplesPerSec = 48000;
            f.wBitsPerSample = 16;
            f.nBlockAlign = 4;
            f.nAvgBytesPerSec = 48000 * 4;
            const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | 0x80000000u;   // AUTOCONVERTPCM
            ok = SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &f, nullptr)) &&
                 SUCCEEDED(client->SetEventHandle(ev)) &&
                 SUCCEEDED(client->GetService(pcap::kIidCaptureClient, reinterpret_cast<void**>(&cap))) && cap &&
                 SUCCEEDED(client->Start());
        }
        ready_ = ok ? 1 : -1;
        std::vector<float> st;
        while (ok && running_) {
            if (WaitForSingleObject(ev, 100) != WAIT_OBJECT_0) continue;
            UINT32 pkt = 0;
            while (running_ && SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD fl = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &fl, nullptr, nullptr))) { ok = false; break; }
                st.resize(size_t(frames) * 2);
                if ((fl & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    std::fill(st.begin(), st.end(), 0.0f);
                } else {
                    const int16_t* s = reinterpret_cast<const int16_t*>(data);
                    for (size_t i = 0; i < st.size(); ++i) st[i] = s[i] / 32768.0f;
                }
                cap->ReleaseBuffer(frames);
                if (cb_ && frames) cb_(st.data(), frames, 48000);
            }
        }
        if (client) client->Stop();
        if (cap) cap->Release();
        if (client) client->Release();
        if (ev) CloseHandle(ev);
        CoUninitialize();
    }

    IAudioClient* activate() {
        HMODULE mod = LoadLibraryW(L"Mmdevapi.dll");
        if (!mod) return nullptr;
        auto fn = reinterpret_cast<pcap::ActivateFn>(reinterpret_cast<void*>(GetProcAddress(mod, "ActivateAudioInterfaceAsync")));
        if (!fn) return nullptr;
        pcap::ActivationParams params{};
        params.activationType = 1;          // AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
        params.loopback.targetProcessId = pid_;
        params.loopback.mode = 0;           // PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(params);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
        auto* done = new pcap::Completion();
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        IAudioClient* client = nullptr;
        if (SUCCEEDED(fn(L"VAD\\Process_Loopback", pcap::kIidAudioClient, &pv, done, &op)) &&
            WaitForSingleObject(done->done(), 3000) == WAIT_OBJECT_0)
            client = done->take();
        if (op) op->Release();
        done->Release();
        return client;   // PropVariantClear no: el blob apunta a 'params' (en la pila)
    }

    Callback cb_;
    DWORD pid_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> ready_{0};
    std::thread thread_;
};

}  // namespace ds5

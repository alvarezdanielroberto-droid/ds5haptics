// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// audio_default.h — Que Windows no se quede con el mando como salida o micrófono.
//
// Cuando aparece el DualSense virtual, Windows lo ve como una placa de sonido nueva y a
// veces lo elige solo como salida (el sonido de la PC se va al mando) o como micrófono
// (Discord queda en silencio: el micrófono del mando no funciona por Bluetooth).
// AudioDefaultGuard recuerda la salida y el micrófono que se usaban y, si en el rato
// siguiente a que aparece el mando Windows los cambia por el mando, los vuelve a poner.
//
// Para cambiar el dispositivo predeterminado se usa IPolicyConfig: no está documentada,
// pero es la que usa el propio panel de sonido de Windows (y programas como SoundSwitch
// o EarTrumpet). Si algo falla, no se cambia nada.
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <propidl.h>

#include <cwctype>
#include <string>

namespace ds5 {

// {f8679f50-850a-41cf-9c72-430f290290c8}
struct IPolicyConfigDs5 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
static const IID kIidPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
// PKEY_Device_FriendlyName (sin depender de initguid/devpkey)
static const PROPERTYKEY kPkeyEndpointName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// MMDeviceEnumerator / IMMDeviceEnumerator (sin __uuidof, para MinGW)
static const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIidMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
// CPolicyConfigClient {870af99c-171d-4f9e-af0d-e63df40c2bc9}
static const CLSID kClsidPolicyConfig = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

class AudioDefaultGuard {
public:
    // Recuerda lo que Windows usa ahora (llamar antes de "enchufar" el mando virtual).
    void snapshot() {
        for (int f = 0; f < 2; ++f)
            for (int r = 0; r < 2; ++r) {
                std::wstring id = defaultId(flow(f), role(r));
                if (!id.empty() && !isController(id)) saved_[f][r] = id;
            }
    }

    // Llamar cada tanto. 'watching' = el mando virtual apareció hace poco: si Windows lo
    // eligió, se vuelve al anterior. Si no, se toma nota de lo que elija la persona.
    // Devuelve un texto para el registro si cambió algo ("" si no).
    std::wstring poll(bool watching) {
        std::wstring done;
        for (int f = 0; f < 2; ++f)
            for (int r = 0; r < 2; ++r) {
                std::wstring cur = defaultId(flow(f), role(r));
                if (cur.empty()) continue;
                if (!isController(cur)) { saved_[f][r] = cur; continue; }
                if (!watching || saved_[f][r].empty() || !isActive(saved_[f][r])) continue;
                if (setDefault(saved_[f][r], role(r))) {
                    if (!done.empty()) done += L", ";
                    done += (f == 0 ? L"out: " : L"in: ") + friendlyName(saved_[f][r]);
                }
            }
        return done;
    }

    static std::wstring friendlyName(const std::wstring& id) {
        std::wstring name;
        IMMDeviceEnumerator* en = enumerator();
        if (!en) return name;
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDevice(id.c_str(), &dev)) && dev) {
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kPkeyEndpointName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                    name = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
            dev->Release();
        }
        en->Release();
        return name;
    }

    // El DualSense virtual se llama "... (DualSense Wireless Controller)" en Windows
    static bool isController(const std::wstring& id) {
        std::wstring n = friendlyName(id);
        for (auto& c : n) c = wchar_t(std::towlower(c));
        return n.find(L"dualsense") != std::wstring::npos;
    }

private:
    static EDataFlow flow(int f) { return f == 0 ? eRender : eCapture; }
    static ERole role(int r) { return r == 0 ? eConsole : eCommunications; }

    static IMMDeviceEnumerator* enumerator() {
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidMMDeviceEnumerator,
                                    reinterpret_cast<void**>(&en))))
            return nullptr;
        return en;
    }

    static std::wstring defaultId(EDataFlow f, ERole r) {
        std::wstring id;
        IMMDeviceEnumerator* en = enumerator();
        if (!en) return id;
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(f, r, &dev)) && dev) {
            LPWSTR s = nullptr;
            if (SUCCEEDED(dev->GetId(&s)) && s) { id = s; CoTaskMemFree(s); }
            dev->Release();
        }
        en->Release();
        return id;
    }

    static bool isActive(const std::wstring& id) {
        bool ok = false;
        IMMDeviceEnumerator* en = enumerator();
        if (!en) return false;
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDevice(id.c_str(), &dev)) && dev) {
            DWORD st = 0;
            ok = SUCCEEDED(dev->GetState(&st)) && st == DEVICE_STATE_ACTIVE;
            dev->Release();
        }
        en->Release();
        return ok;
    }

    static bool setDefault(const std::wstring& id, ERole r) {
        IPolicyConfigDs5* pc = nullptr;
        if (FAILED(CoCreateInstance(kClsidPolicyConfig, nullptr, CLSCTX_ALL, kIidPolicyConfig,
                                    reinterpret_cast<void**>(&pc))) || !pc)
            return false;
        bool ok = SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), r));
        if (ok && r == eConsole) pc->SetDefaultEndpoint(id.c_str(), eMultimedia);   // van juntos
        pc->Release();
        return ok;
    }

    std::wstring saved_[2][2];   // [salida/entrada][consola/comunicaciones]
};

}  // namespace ds5

// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// voice_tts.h — Avisos hablados con la voz de Windows (SAPI, la misma del Narrador).
//
// Genera la frase como audio (48 kHz, mono) en memoria; después se manda al parlante del
// mando como cualquier otro sonido. No hace falta descargar ni grabar nada. Se busca una
// voz del idioma pedido (las de escritorio y, si no hay, las "OneCore" de Windows 10/11);
// si no hay ninguna en español, Speaker::spanish() lo indica para hablar en inglés.
#pragma once
#include <windows.h>
#include <objbase.h>
#include <sapi.h>

#include <string>
#include <vector>

namespace ds5 {

namespace sapi_ids {
static const CLSID kSpVoice = {0x96749377, 0x3391, 0x11D2, {0x9E, 0xE3, 0x00, 0xC0, 0x4F, 0x79, 0x73, 0x96}};
static const CLSID kSpStream = {0x715D9C59, 0x4442, 0x11D2, {0x96, 0x05, 0x00, 0xC0, 0x4F, 0x8E, 0xE6, 0x28}};
static const CLSID kSpObjectTokenCategory = {0xA910187F, 0x0C7A, 0x45AC, {0x92, 0xCC, 0x59, 0xED, 0xAF, 0xB7, 0x7B, 0x53}};
static const IID kISpVoice = {0x6C44DF74, 0x72B9, 0x4992, {0xA1, 0xEC, 0xEF, 0x99, 0x6E, 0x04, 0x22, 0xD4}};
static const IID kISpStream = {0x12E3CCA9, 0x7518, 0x44C5, {0xA5, 0xE7, 0xBA, 0x5A, 0x79, 0xCB, 0x92, 0x9E}};
static const IID kISpObjectTokenCategory = {0x2D3D3845, 0x39AF, 0x4850, {0xBB, 0xF9, 0x40, 0xB4, 0x97, 0x80, 0x01, 0x1D}};
static const GUID kWaveFormatEx = {0xC31ADBAE, 0x527F, 0x4FF5, {0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C}};
}  // namespace sapi_ids

class Speaker {
public:
    ~Speaker() { if (voice_) voice_->Release(); }

    // Llamar desde el hilo que va a hablar (con COM ya iniciado en ese hilo).
    bool init(bool wantSpanish) {
        using namespace sapi_ids;
        if (FAILED(CoCreateInstance(kSpVoice, nullptr, CLSCTX_ALL, kISpVoice, reinterpret_cast<void**>(&voice_))) || !voice_)
            return false;
        const WORD want = wantSpanish ? 0x0A : 0x09;   // LANG_SPANISH / LANG_ENGLISH
        ISpObjectToken* tok = findVoice(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices", want);
        if (!tok) tok = findVoice(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices", want);
        if (tok) {
            voice_->SetVoice(tok);
            tok->Release();
            spanish_ = wantSpanish;
        } else {
            // Sin voz del idioma: la predeterminada (casi siempre una en inglés)
            spanish_ = false;
            if (wantSpanish) {
                ISpObjectToken* en = findVoice(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices", 0x09);
                if (!en) en = findVoice(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices", 0x09);
                if (en) { voice_->SetVoice(en); en->Release(); }
            }
        }
        voice_->SetVolume(100);
        return true;
    }

    bool spanish() const { return spanish_; }

    // Frase -> audio mono de 48 kHz entre -1 y 1. Tarda unos cientos de ms.
    bool synthesize(const std::wstring& text, std::vector<float>& out) {
        using namespace sapi_ids;
        out.clear();
        if (!voice_) return false;
        IStream* mem = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) || !mem) return false;
        ISpStream* st = nullptr;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(kSpStream, nullptr, CLSCTX_ALL, kISpStream, reinterpret_cast<void**>(&st))) && st) {
            WAVEFORMATEX wfx{};
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nChannels = 1;
            wfx.nSamplesPerSec = 48000;
            wfx.wBitsPerSample = 16;
            wfx.nBlockAlign = 2;
            wfx.nAvgBytesPerSec = 96000;
            if (SUCCEEDED(st->SetBaseStream(mem, kWaveFormatEx, &wfx)) && SUCCEEDED(voice_->SetOutput(st, TRUE)) &&
                SUCCEEDED(voice_->Speak(text.c_str(), SPF_DEFAULT | SPF_IS_NOT_XML, nullptr))) {
                LARGE_INTEGER zero{};
                ULARGE_INTEGER end{};
                mem->Seek(zero, STREAM_SEEK_CUR, &end);
                HGLOBAL h = nullptr;
                if (SUCCEEDED(GetHGlobalFromStream(mem, &h)) && h) {
                    const int16_t* p = static_cast<const int16_t*>(GlobalLock(h));
                    if (p) {
                        size_t n = size_t(end.QuadPart / 2);
                        out.resize(n);
                        for (size_t i = 0; i < n; ++i) out[i] = p[i] / 32768.0f;
                        GlobalUnlock(h);
                        ok = n > 0;
                    }
                }
            }
            voice_->SetOutput(nullptr, TRUE);
            st->Close();
            st->Release();
        }
        mem->Release();
        return ok;
    }

private:
    // Primera voz de la categoría cuyo atributo "Language" (ej. "c0a;80a") sea del idioma pedido
    static ISpObjectToken* findVoice(const wchar_t* category, WORD primaryLang) {
        using namespace sapi_ids;
        ISpObjectTokenCategory* cat = nullptr;
        if (FAILED(CoCreateInstance(kSpObjectTokenCategory, nullptr, CLSCTX_ALL, kISpObjectTokenCategory,
                                    reinterpret_cast<void**>(&cat))) || !cat)
            return nullptr;
        ISpObjectToken* found = nullptr;
        IEnumSpObjectTokens* en = nullptr;
        if (SUCCEEDED(cat->SetId(category, FALSE)) && SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &en)) && en) {
            ISpObjectToken* t = nullptr;
            ULONG got = 0;
            while (!found && en->Next(1, &t, &got) == S_OK && t) {
                if (tokenLanguageIs(t, primaryLang)) found = t;
                else t->Release();
                t = nullptr;
            }
            en->Release();
        }
        cat->Release();
        return found;
    }

    static bool tokenLanguageIs(ISpObjectToken* t, WORD primaryLang) {
        ISpDataKey* attrs = nullptr;
        if (FAILED(t->OpenKey(L"Attributes", &attrs)) || !attrs) return false;
        LPWSTR lang = nullptr;
        bool match = false;
        if (SUCCEEDED(attrs->GetStringValue(L"Language", &lang)) && lang) {
            // Lista de LANGID en hexadecimal separados por ';'
            const wchar_t* p = lang;
            while (*p && !match) {
                wchar_t* end = nullptr;
                unsigned long id = wcstoul(p, &end, 16);
                if (end == p) break;
                if ((id & 0x3FF) == primaryLang) match = true;
                p = end;
                while (*p == L';' || *p == L' ') ++p;
            }
            CoTaskMemFree(lang);
        }
        attrs->Release();
        return match;
    }

    ISpVoice* voice_ = nullptr;
    bool spanish_ = false;
};

}  // namespace ds5

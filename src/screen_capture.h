// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// screen_capture.h — Una imagen chiquita (~64 px de ancho) de la pantalla donde está el juego,
// para el color de la barra de luz. Usa la duplicación de escritorio de DXGI (la misma que usan
// los programas de grabación): la pantalla se achica en la placa de video (mipmaps) y solo se
// leen unos miles de píxeles, así que casi no gasta CPU.
//
// No ve contenido protegido (videos con DRM salen negros). Si cambia la resolución, se pasa a
// pantalla completa exclusiva o se bloquea la PC, se vuelve a abrir solo.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif

namespace ds5 {

class ScreenSampler {
public:
    ~ScreenSampler() { close(); }

    // Toma una imagen nueva si la pantalla cambió. Devuelve true y deja los píxeles en
    // pixels()/width()/height()/stride() (BGRA). false = sin cambios o no disponible.
    bool grab() {
        if (!dup_) {
            const DWORD now = GetTickCount();
            if (now - lastOpenTry_ < 1000) return false;   // no reintentar más de una vez por segundo
            lastOpenTry_ = now;
            if (!open()) return false;
        }
        // ¿El juego se movió a otro monitor?
        if (GetTickCount() - lastMonCheck_ > 2000) {
            lastMonCheck_ = GetTickCount();
            if (MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY) != monitor_) { close(); return false; }
        }
        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource* res = nullptr;
        HRESULT hr = dup_->AcquireNextFrame(0, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (FAILED(hr)) { close(); return false; }   // acceso perdido: se reabre después
        bool ok = false;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
            ok = shrink(tex);
            tex->Release();
        }
        res->Release();
        dup_->ReleaseFrame();
        return ok && read();
    }

    const uint8_t* pixels() const { return buf_.data(); }
    int width() const { return w_; }
    int height() const { return h_; }
    int stride() const { return stride_; }
    bool bgra() const { return bgra_; }

    void close() {
        rel(staging_);
        rel(srv_);
        rel(mip_);
        rel(dup_);
        rel(ctx_);
        rel(dev_);
        mipW_ = mipH_ = 0;
        level_ = 0;
    }

private:
    template <class T>
    static void rel(T*& p) {
        if (p) { p->Release(); p = nullptr; }
    }

    bool open() {
        close();
        monitor_ = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
        lastMonCheck_ = GetTickCount();
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
        // Buscar la salida (monitor) donde está la ventana del juego; si no, la primera
        IDXGIAdapter1* useAd = nullptr;
        IDXGIOutput* useOut = nullptr;
        IDXGIAdapter1* ad = nullptr;
        for (UINT a = 0; factory->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; ++a) {
            IDXGIOutput* out = nullptr;
            for (UINT o = 0; ad->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
                DXGI_OUTPUT_DESC d{};
                out->GetDesc(&d);
                const bool match = d.Monitor == monitor_;
                if (match || !useOut) {
                    rel(useOut);
                    rel(useAd);
                    useOut = out;
                    useAd = ad;
                    useAd->AddRef();
                } else {
                    out->Release();
                }
                out = nullptr;
                if (match) break;
            }
            DXGI_OUTPUT_DESC d{};
            const bool found = useOut && SUCCEEDED(useOut->GetDesc(&d)) && d.Monitor == monitor_;
            ad->Release();
            ad = nullptr;
            if (found) break;
        }
        factory->Release();
        if (!useOut || !useAd) { rel(useOut); rel(useAd); return false; }
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = D3D11CreateDevice(useAd, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        IDXGIOutput1* out1 = nullptr;
        if (SUCCEEDED(hr)) hr = useOut->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&out1));
        if (SUCCEEDED(hr) && out1) hr = out1->DuplicateOutput(dev_, &dup_);
        rel(out1);
        rel(useOut);
        rel(useAd);
        if (FAILED(hr) || !dup_) { close(); return false; }
        return true;
    }

    // Copia la pantalla a una textura con mipmaps, los genera y copia uno chico (~64 px)
    bool shrink(ID3D11Texture2D* src) {
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM && sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
        if (!mip_ || sd.Width != mipW_ || sd.Height != mipH_ || sd.Format != fmt_) {
            rel(staging_);
            rel(srv_);
            rel(mip_);
            D3D11_TEXTURE2D_DESC md = sd;
            md.MipLevels = 0;   // la cadena completa
            md.ArraySize = 1;
            md.SampleDesc.Count = 1;
            md.SampleDesc.Quality = 0;
            md.Usage = D3D11_USAGE_DEFAULT;
            md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            md.CPUAccessFlags = 0;
            md.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(dev_->CreateTexture2D(&md, nullptr, &mip_))) return false;
            if (FAILED(dev_->CreateShaderResourceView(mip_, nullptr, &srv_))) return false;
            mip_->GetDesc(&md);
            mipW_ = sd.Width;
            mipH_ = sd.Height;
            fmt_ = sd.Format;
            level_ = 0;
            while ((mipW_ >> (level_ + 1)) >= 48 && level_ + 1 < md.MipLevels) ++level_;   // ~64 px de ancho
            D3D11_TEXTURE2D_DESC td = sd;
            td.Width = std::max<UINT>(1, mipW_ >> level_);
            td.Height = std::max<UINT>(1, mipH_ >> level_);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.SampleDesc.Count = 1;
            td.SampleDesc.Quality = 0;
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging_))) return false;
            w_ = int(td.Width);
            h_ = int(td.Height);
        }
        ctx_->CopySubresourceRegion(mip_, 0, 0, 0, 0, src, 0, nullptr);
        ctx_->GenerateMips(srv_);
        ctx_->CopySubresourceRegion(staging_, 0, 0, 0, 0, mip_, level_, nullptr);
        bgra_ = sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        return true;
    }

    // Copia la imagen chica a memoria propia (y suelta la textura enseguida)
    bool read() {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(staging_, 0, D3D11_MAP_READ, 0, &m))) return false;
        stride_ = w_ * 4;
        buf_.resize(size_t(stride_) * size_t(h_));
        for (int y = 0; y < h_; ++y)
            std::memcpy(&buf_[size_t(y) * size_t(stride_)], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(stride_));
        ctx_->Unmap(staging_, 0);
        return true;
    }

    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGIOutputDuplication* dup_ = nullptr;
    ID3D11Texture2D* mip_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    UINT mipW_ = 0, mipH_ = 0, level_ = 0;
    DXGI_FORMAT fmt_ = DXGI_FORMAT_UNKNOWN;
    HMONITOR monitor_ = nullptr;
    DWORD lastOpenTry_ = 0, lastMonCheck_ = 0;
    std::vector<uint8_t> buf_;
    int w_ = 0, h_ = 0, stride_ = 0;
    bool bgra_ = true;
};

}  // namespace ds5

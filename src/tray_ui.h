// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// tray_ui.h — Ícono en la bandeja del sistema y cuadro de estadísticas (solo Windows).
//
//   - Ícono "ds5haptics" junto al reloj. Clic izquierdo o Alt+Insert: muestra u oculta las
//     estadísticas. Clic derecho: Ver estadísticas / Iniciar con Windows / Salir.
//   - Cuadro semitransparente arriba a la derecha, siempre visible, que deja pasar los clics
//     al juego y no le quita el foco.
//
// Hecho con la API nativa de Windows (GDI), sin frameworks: usa muy poca memoria.
// Nada depende solo del color: cada estado se dice con texto.
// Incluir después de <windows.h>.
#pragma once
#include <shellapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "i18n.h"

#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

namespace ds5ui {

using ds5::tr;
using ds5::trw;

struct Stats {
    // Conexión
    std::string winState = "esperando";   // estado del mando virtual en Windows
    bool btOk = false;
    double inputHz = 0;
    int mode = 0;                          // 0 PS5, 1 PS4, 2 Xbox
    std::string game, profile;
    // Batería
    int battery = -1;
    bool charging = false, full = false, lowWarn = false;
    // Háptica
    std::string hapticState = "sin audio";
    float hapticLevel = 0;
    float hapticGain = 1;
    double ownLatencyMs = -1, writeMs = -1;
    unsigned gaps = 0;
    // Parlante
    bool speakerEnabled = false, speakerStreaming = false, volumeForced = false;
    int gameVolume = -1;
    float speakerGain = 1;
    // Gatillos adaptativos (modo de efecto que pidió el juego; -1 = nunca los configuró)
    int trigR = -1, trigL = -1;
    unsigned gameOutputs = 0;
};

class Tray {
public:
    struct Callbacks {
        std::function<void()> exit;
        std::function<bool()> autostartOn;
        std::function<void(bool)> setAutostart;
        std::function<void(const std::string&)> log;
    };

    ~Tray() { stop(); }

    void start(Callbacks cb) {
        cb_ = std::move(cb);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        HWND h = hidden_.load();
        if (h) PostMessageW(h, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }

    void update(const Stats& s) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_ = s;
    }

private:
    enum : UINT { kTrayMsg = WM_APP + 1, kIdStats = 101, kIdAutostart = 102, kIdExit = 103,
                  kTimerOverlay = 1, kTimerTip = 2, kHotkey = 1 };

    static std::wstring w(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring r(n > 0 ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &r[0], n);
        return r;
    }
    void log(const std::string& m) { if (cb_.log) cb_.log(m); }

    Stats snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }

    // ------------------------------------------------------------------ ícono ----
    // Silueta de un mando dibujada con GDI (sin archivos .ico). El relleno cambia según el
    // modo, pero la forma es siempre la misma; con batería baja se le dibuja una raya debajo.
    HICON makeIcon(int mode, bool low) {
        int sz = GetSystemMetrics(SM_CXSMICON);
        if (sz <= 0) sz = 16;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP color = CreateCompatibleBitmap(screen, sz, sz);
        HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
        ReleaseDC(nullptr, screen);
        COLORREF fill = mode == 1 ? RGB(150, 90, 230) : mode == 2 ? RGB(60, 190, 80) : RGB(40, 120, 240);
        auto shape = [&](HDC d, HBRUSH body, HBRUSH dots, bool drawLow) {
            const double k = sz / 16.0;
            auto R = [&](double v) { return int(std::lround(v * k)); };
            HGDIOBJ oldPen = SelectObject(d, GetStockObject(NULL_PEN));
            HGDIOBJ oldBr = SelectObject(d, body);
            RoundRect(d, R(1), R(4), R(15) + 1, R(10) + 1, R(5), R(5));   // cuerpo
            Ellipse(d, R(0), R(6), R(6) + 1, R(14) + 1);                 // empuñadura izquierda
            Ellipse(d, R(10), R(6), R(16) + 1, R(14) + 1);               // empuñadura derecha
            if (dots) {
                SelectObject(d, dots);
                Ellipse(d, R(3), R(6), R(5) + 1, R(8) + 1);              // cruceta
                Ellipse(d, R(11), R(6), R(13) + 1, R(8) + 1);            // botones
            }
            if (drawLow) {
                SelectObject(d, body);
                Rectangle(d, R(2), R(15), R(14) + 1, R(16) + 1);         // batería baja
            }
            SelectObject(d, oldBr);
            SelectObject(d, oldPen);
        };
        // Máscara: negro = opaco, blanco = transparente
        HGDIOBJ old = SelectObject(dc, mask);
        PatBlt(dc, 0, 0, sz, sz, WHITENESS);
        shape(dc, (HBRUSH)GetStockObject(BLACK_BRUSH), nullptr, low);
        // Color: la figura sobre fondo negro
        SelectObject(dc, color);
        PatBlt(dc, 0, 0, sz, sz, BLACKNESS);
        HBRUSH body = CreateSolidBrush(fill);
        HBRUSH dots = CreateSolidBrush(RGB(255, 255, 255));
        shape(dc, body, dots, low);
        SelectObject(dc, old);
        DeleteObject(body);
        DeleteObject(dots);
        DeleteDC(dc);
        ICONINFO ii{};
        ii.fIcon = TRUE;
        ii.hbmMask = mask;
        ii.hbmColor = color;
        HICON icon = CreateIconIndirect(&ii);
        DeleteObject(mask);
        DeleteObject(color);
        return icon;
    }

    std::wstring tipText(const Stats& s) {
        std::string t = "ds5haptics - ";
        t += s.mode == 1 ? tr("modo PS4", "PS4 mode") : s.mode == 2 ? tr("modo Xbox", "Xbox mode") : tr("modo PS5", "PS5 mode");
        if (s.battery >= 0) {
            t += tr(" - batería ", " - battery ") + std::to_string(s.battery) + "%";
            if (s.full) t += tr(" (completa)", " (full)");
            else if (s.charging) t += tr(" (cargando)", " (charging)");
            else if (s.lowWarn) t += tr(" (BAJA)", " (LOW)");
        }
        if (!s.btOk) t += tr(" - mando desconectado", " - controller disconnected");
        std::wstring r = w(t);
        if (r.size() > 127) r.resize(127);
        return r;
    }

    void addIcon() {
        Stats s = snapshot();
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hidden_;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = kTrayMsg;
        iconMode_ = s.mode;
        iconLow_ = s.lowWarn;
        if (icon_) DestroyIcon(icon_);
        icon_ = makeIcon(iconMode_, iconLow_);
        nid.hIcon = icon_;
        std::wstring tip = tipText(s);
        wcsncpy(nid.szTip, tip.c_str(), 127);
        lastTip_ = tip;
        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    void refreshIcon() {
        Stats s = snapshot();
        std::wstring tip = tipText(s);
        bool iconChanged = s.mode != iconMode_ || s.lowWarn != iconLow_;
        if (!iconChanged && tip == lastTip_) return;
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hidden_;
        nid.uID = 1;
        nid.uFlags = NIF_TIP;
        if (iconChanged) {
            iconMode_ = s.mode;
            iconLow_ = s.lowWarn;
            HICON old = icon_;
            icon_ = makeIcon(iconMode_, iconLow_);
            nid.uFlags |= NIF_ICON;
            nid.hIcon = icon_;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            if (old) DestroyIcon(old);
            nid.uFlags = NIF_TIP;
        }
        wcsncpy(nid.szTip, tip.c_str(), 127);
        lastTip_ = tip;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void removeIcon() {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hidden_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
    }

    void showMenu() {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | (overlayVisible_ ? MF_CHECKED : 0), kIdStats, trw(L"Ver estadísticas\tAlt+Insert", L"Show stats\tAlt+Insert"));
        bool autoOn = cb_.autostartOn && cb_.autostartOn();
        AppendMenuW(m, MF_STRING | (autoOn ? MF_CHECKED : 0), kIdAutostart, trw(L"Iniciar con Windows", L"Start with Windows"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kIdExit, trw(L"Salir", L"Exit"));
        POINT p;
        GetCursorPos(&p);
        SetForegroundWindow(hidden_);   // si no, el menú no se cierra al hacer clic afuera
        UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, hidden_, nullptr);
        PostMessageW(hidden_, WM_NULL, 0, 0);
        DestroyMenu(m);
        if (cmd == kIdStats) toggleOverlay();
        else if (cmd == kIdAutostart && cb_.setAutostart) cb_.setAutostart(!autoOn);
        else if (cmd == kIdExit && cb_.exit) cb_.exit();
    }

    // -------------------------------------------------------------- estadísticas ----
    int px(int v) const { return MulDiv(v, dpi_, 96); }

    void toggleOverlay() {
        overlayVisible_ = !overlayVisible_;
        if (overlayVisible_) {
            placeOverlay();
            ShowWindow(overlay_, SW_SHOWNOACTIVATE);
            InvalidateRect(overlay_, nullptr, FALSE);
            SetTimer(overlay_, kTimerOverlay, 500, nullptr);
        } else {
            KillTimer(overlay_, kTimerOverlay);
            ShowWindow(overlay_, SW_HIDE);
        }
    }

    // Arriba a la derecha del monitor donde está el juego (la ventana en primer plano)
    void placeOverlay() {
        HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        const int width = px(340), height = px(42) + lineH() * kLines + px(34);
        SetWindowPos(overlay_, HWND_TOPMOST, mi.rcMonitor.right - width - px(16), mi.rcMonitor.top + px(16),
                     width, height, SWP_NOACTIVATE);
    }

    static constexpr int kLines = 8;   // filas debajo del título
    int lineH() const { return px(21); }

    void paintOverlay(HDC target, const RECT& rc) {
        Stats s = snapshot();
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        const COLORREF bg = RGB(18, 20, 26), fg = RGB(240, 242, 246), dim = RGB(150, 156, 168);
        HBRUSH bgBr = CreateSolidBrush(bg);
        FillRect(dc, &rc, bgBr);
        DeleteObject(bgBr);
        HPEN border = CreatePen(PS_SOLID, 1, RGB(70, 76, 90));
        HGDIOBJ oldPen = SelectObject(dc, border);
        HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, rc.right, rc.bottom);
        SetBkMode(dc, TRANSPARENT);

        const int x0 = px(14), x1 = px(110);
        int y = px(12);
        HGDIOBJ oldFont = SelectObject(dc, fontTitle_);
        SetTextColor(dc, fg);
        TextOutW(dc, x0, y, L"ds5haptics", 10);
        std::wstring badge = s.mode == 1 ? trw(L"MODO PS4", L"PS4 MODE") : s.mode == 2 ? trw(L"MODO XBOX", L"XBOX MODE")
                                                                    : trw(L"MODO PS5", L"PS5 MODE");
        SIZE bs;
        SelectObject(dc, fontBold_);
        GetTextExtentPoint32W(dc, badge.c_str(), int(badge.size()), &bs);
        TextOutW(dc, rc.right - x0 - bs.cx, y + px(3), badge.c_str(), int(badge.size()));
        y += px(30);

        auto row = [&](const wchar_t* label, const std::wstring& value, int indent = 0) {
            SelectObject(dc, fontNormal_);
            SetTextColor(dc, dim);
            TextOutW(dc, x0, y, label, int(wcslen(label)));
            SetTextColor(dc, fg);
            RECT r{x1 + indent, y, rc.right - x0, y + lineH()};
            DrawTextW(dc, value.c_str(), int(value.size()), &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += lineH();
        };

        // Batería: dibujo + texto
        {
            const int bw = px(34), bh = px(14), by = y + px(3);
            HPEN pen = CreatePen(PS_SOLID, px(1) < 1 ? 1 : px(1), fg);
            SelectObject(dc, pen);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, x1, by, x1 + bw, by + bh);
            HBRUSH fgBr = CreateSolidBrush(fg);
            RECT tip{x1 + bw, by + bh / 4, x1 + bw + px(3), by + bh - bh / 4};
            FillRect(dc, &tip, fgBr);
            if (s.battery >= 0) {
                int fillW = (bw - px(4)) * std::max(0, std::min(100, s.battery)) / 100;
                RECT f{x1 + px(2), by + px(2), x1 + px(2) + std::max(fillW, px(2)), by + bh - px(2)};
                FillRect(dc, &f, fgBr);
            }
            DeleteObject(fgBr);
            SelectObject(dc, border);
            DeleteObject(pen);
            std::string t = s.battery < 0 ? std::string(tr("sin dato", "unknown")) : std::to_string(s.battery) + "%";
            if (s.full) t += tr("  carga completa", "  fully charged");
            else if (s.charging) t += tr("  cargando", "  charging");
            else if (s.lowWarn) t += tr("  BATERÍA BAJA", "  LOW BATTERY");
            SelectObject(dc, fontNormal_);
            SetTextColor(dc, dim);
            const wchar_t* lb = trw(L"Batería", L"Battery");
            TextOutW(dc, x0, y, lb, int(wcslen(lb)));
            SetTextColor(dc, fg);
            if (s.lowWarn) SelectObject(dc, fontBold_);
            std::wstring wt = w(t);
            TextOutW(dc, x1 + bw + px(10), y, wt.c_str(), int(wt.size()));
            y += lineH();
        }

        std::string game = s.game.empty() ? std::string(tr("ninguno", "none")) : s.game;
        if (!s.profile.empty()) game += "  [" + s.profile + "]";
        row(trw(L"Juego", L"Game"), w(game));
        row(trw(L"Mando", L"Controller"), w(s.btOk ? "Bluetooth ok, " + std::to_string(int(s.inputHz + 0.5)) + " Hz"
                                                   : std::string(tr("Bluetooth DESCONECTADO", "Bluetooth DISCONNECTED"))));
        row(L"Windows", w(s.winState));

        if (s.mode != 0) {
            row(trw(L"Háptica", L"Haptics"), trw(L"no disponible (solo vibración clásica)", L"not available (classic rumble only)"));
            row(trw(L"Gatillos", L"Triggers"), trw(L"no disponibles en este modo", L"not available in this mode"));
            row(trw(L"Parlante", L"Speaker"), trw(L"no disponible en este modo", L"not available in this mode"));
            row(trw(L"Latencia", L"Latency"), L"-");
        } else {
            // Háptica con barra de nivel
            {
                std::string h = s.hapticState;
                if (s.hapticGain != 1.0f) { char b[24]; std::snprintf(b, sizeof(b), "  x%.1f", s.hapticGain); h += b; }
                SelectObject(dc, fontNormal_);
                SetTextColor(dc, dim);
                const wchar_t* lh = trw(L"Háptica", L"Haptics");
                TextOutW(dc, x0, y, lh, int(wcslen(lh)));
                const int mw = px(60), mh = px(8), my = y + px(6);
                HBRUSH fgBr = CreateSolidBrush(fg);
                HBRUSH dimBr = CreateSolidBrush(RGB(60, 64, 76));
                RECT back{x1, my, x1 + mw, my + mh};
                FillRect(dc, &back, dimBr);
                RECT lvl{x1, my, x1 + int(mw * std::max(0.0f, std::min(1.0f, s.hapticLevel))), my + mh};
                FillRect(dc, &lvl, fgBr);
                DeleteObject(fgBr);
                DeleteObject(dimBr);
                SetTextColor(dc, fg);
                std::wstring wh = w(h);
                RECT r{x1 + mw + px(10), y, rc.right - x0, y + lineH()};
                DrawTextW(dc, wh.c_str(), int(wh.size()), &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                y += lineH();
            }
            auto trig = [](int m) {
                if (m < 0) return std::string(tr("sin configurar", "not set"));
                if (m == 0 || m == 5) return std::string(tr("sin efecto", "no effect"));
                return std::string(tr("con efecto", "effect on"));
            };
            std::string trg = s.trigL < 0 && s.trigR < 0 ? std::string(tr("el juego no los usó", "not used by the game"))
                              : "L2 " + trig(s.trigL) + ", R2 " + trig(s.trigR);
            row(trw(L"Gatillos", L"Triggers"), w(trg));
            std::string sp;
            if (!s.speakerEnabled) sp = tr("desactivado", "off");
            else {
                sp = s.speakerStreaming ? tr("activo", "on") : tr("sin audio", "no audio");
                if (s.gameVolume >= 0)
                    sp += tr(", volumen ", ", volume ") + std::to_string(s.volumeForced ? 100 : s.gameVolume) + "%" +
                          (s.volumeForced ? tr(" (forzado)", " (forced)") : "");
                if (s.speakerGain != 1.0f) { char b[24]; std::snprintf(b, sizeof(b), ", extra x%.0f", s.speakerGain); sp += b; }
            }
            row(trw(L"Parlante", L"Speaker"), w(sp));
            std::string lat;
            if (s.ownLatencyMs >= 0) {
                char b[96];
                std::snprintf(b, sizeof(b), tr("~%.0f ms propios", "~%.0f ms added"), s.ownLatencyMs);
                lat = b;
                if (s.writeMs >= 0) { std::snprintf(b, sizeof(b), tr(", envío %.1f ms", ", send %.1f ms"), s.writeMs); lat += b; }
                if (s.gaps) lat += tr(", huecos ", ", gaps ") + std::to_string(s.gaps);
            } else {
                lat = tr("sin háptica en curso", "no haptics right now");
            }
            row(trw(L"Latencia", L"Latency"), w(lat));
        }

        SelectObject(dc, fontSmall_);
        SetTextColor(dc, dim);
        const wchar_t* foot = trw(L"Alt+Insert para ocultar", L"Alt+Insert to hide");
        TextOutW(dc, x0, rc.bottom - px(22), foot, int(wcslen(foot)));

        SelectObject(dc, oldFont);
        SelectObject(dc, oldBr);
        SelectObject(dc, oldPen);
        DeleteObject(border);
        BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
    }

    void makeFonts() {
        auto font = [&](int pt, int weight) {
            return CreateFontW(-MulDiv(pt, dpi_, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        fontTitle_ = font(12, FW_SEMIBOLD);
        fontBold_ = font(10, FW_BOLD);
        fontNormal_ = font(10, FW_NORMAL);
        fontSmall_ = font(8, FW_NORMAL);
    }
    void deleteFonts() {
        for (HFONT* f : {&fontTitle_, &fontBold_, &fontNormal_, &fontSmall_})
            if (*f) { DeleteObject(*f); *f = nullptr; }
    }

    // ------------------------------------------------------------ ventanas ----
    static LRESULT CALLBACK hiddenProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
        Tray* t = reinterpret_cast<Tray*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!t) return DefWindowProcW(h, m, wp, lp);
        if (m == t->taskbarCreated_ && m != 0) { t->addIcon(); return 0; }   // se reinició el Explorador
        switch (m) {
        case kTrayMsg:
            if (LOWORD(lp) == WM_LBUTTONUP) t->toggleOverlay();
            else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) t->showMenu();
            return 0;
        case WM_HOTKEY:
            if (wp == kHotkey) t->toggleOverlay();
            return 0;
        case WM_TIMER:
            if (wp == kTimerTip) t->refreshIcon();
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, m, wp, lp);
    }

    static LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
        Tray* t = reinterpret_cast<Tray*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            if (t) t->paintOverlay(dc, rc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (t && wp == kTimerOverlay) {
                // Algunos juegos se ponen encima: volver a quedar arriba, sin tomar el foco
                SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        }
        return DefWindowProcW(h, m, wp, lp);
    }

    void run() {
        SetProcessDPIAware();
        HDC screen = GetDC(nullptr);
        dpi_ = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(nullptr, screen);
        if (dpi_ <= 0) dpi_ = 96;
        makeFonts();
        HINSTANCE inst = GetModuleHandleW(nullptr);
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSW wc{};
        wc.lpfnWndProc = hiddenProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"ds5haptics-tray";
        RegisterClassW(&wc);
        WNDCLASSW oc{};
        oc.lpfnWndProc = overlayProc;
        oc.hInstance = inst;
        oc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        oc.lpszClassName = L"ds5haptics-stats";
        RegisterClassW(&oc);

        HWND hidden = CreateWindowExW(0, wc.lpszClassName, L"ds5haptics", WS_POPUP, 0, 0, 0, 0,
                                      nullptr, nullptr, inst, nullptr);
        overlay_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                                   oc.lpszClassName, L"ds5haptics", WS_POPUP, 0, 0, 10, 10,
                                   nullptr, nullptr, inst, nullptr);
        if (!hidden || !overlay_) {
            log(tr("No se pudo crear el ícono de la bandeja.", "Could not create the tray icon."));
            deleteFonts();
            return;
        }
        SetWindowLongPtrW(hidden, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetWindowLongPtrW(overlay_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetLayeredWindowAttributes(overlay_, 0, 215, LWA_ALPHA);   // semitransparente
        hidden_ = hidden;

        addIcon();
        SetTimer(hidden, kTimerTip, 2000, nullptr);
        if (!RegisterHotKey(hidden, kHotkey, MOD_ALT | MOD_NOREPEAT, VK_INSERT))
            log(tr("No se pudo registrar Alt+Insert (otro programa lo usa). Las estadísticas se abren con clic en el ícono.",
                   "Could not register Alt+Insert (another program uses it). Click the icon to open the stats."));

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        UnregisterHotKey(nullptr, kHotkey);
        removeIcon();
        if (overlay_) { DestroyWindow(overlay_); overlay_ = nullptr; }
        hidden_ = nullptr;
        deleteFonts();
    }

    Callbacks cb_;
    std::thread thread_;
    std::mutex mu_;
    Stats stats_;
    std::atomic<HWND> hidden_{nullptr};
    HWND overlay_ = nullptr;
    HICON icon_ = nullptr;
    int iconMode_ = -1;
    bool iconLow_ = false;
    std::wstring lastTip_;
    bool overlayVisible_ = false;
    UINT taskbarCreated_ = 0;
    int dpi_ = 96;
    HFONT fontTitle_ = nullptr, fontBold_ = nullptr, fontNormal_ = nullptr, fontSmall_ = nullptr;
};

}  // namespace ds5ui

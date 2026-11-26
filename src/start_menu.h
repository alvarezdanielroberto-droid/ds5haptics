// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// start_menu.h — Acceso directo "ds5haptics" en el menú Inicio (del usuario, sin permisos de
// administrador). Sirve para volver a abrir el programa después de "Salir" sin reiniciar.
#pragma once
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <string>

namespace ds5 {
namespace startmenu {

// GUID propios (así no hace falta uuid.lib)
static const CLSID kClsidShellLink = {0x00021401, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID kIidShellLinkW = {0x000214F9, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID kIidPersistFile = {0x0000010B, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// Ruta del acceso directo: ...\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\ds5haptics.lnk
inline std::wstring linkPath() {
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, dir))) return {};
    return std::wstring(dir) + L"\\ds5haptics.lnk";
}

inline bool exists() {
    std::wstring p = linkPath();
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Crea (o actualiza) el acceso directo al programa 'exe', sin argumentos: al abrirlo, el
// programa arranca en segundo plano. Llamar con COM ya iniciado.
inline bool create(const std::wstring& exe, const std::wstring& workDir, const std::wstring& description) {
    std::wstring path = linkPath();
    if (path.empty()) return false;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(kClsidShellLink, nullptr, CLSCTX_INPROC_SERVER, kIidShellLinkW, reinterpret_cast<void**>(&link))))
        return false;
    bool ok = false;
    link->SetPath(exe.c_str());
    link->SetArguments(L"");
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(exe.c_str(), 0);
    link->SetShowCmd(SW_SHOWMINNOACTIVE);   // la consola no aparece en primer plano
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(kIidPersistFile, reinterpret_cast<void**>(&file)))) {
        ok = SUCCEEDED(file->Save(path.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

inline bool remove() {
    std::wstring p = linkPath();
    return !p.empty() && (DeleteFileW(p.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND);
}

}  // namespace startmenu
}  // namespace ds5

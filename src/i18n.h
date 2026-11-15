// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// i18n.h — Idioma de los mensajes: español o inglés.
// tr("texto en español", "English text") devuelve el del idioma activo.
// El idioma se elige al arrancar (Windows, ds5haptics.ini o --lang) y no cambia después.
#pragma once
#include <atomic>

namespace ds5 {

inline std::atomic<bool>& englishFlag() {
    static std::atomic<bool> en{false};
    return en;
}
inline void setEnglish(bool en) { englishFlag() = en; }
inline bool isEnglish() { return englishFlag().load(std::memory_order_relaxed); }

// format_arg: el compilador revisa los %d/%s del texto en español en cada printf.
// (Que el inglés tenga los mismos los verificó el script que generó las traducciones.)
#if defined(__GNUC__) || defined(__clang__)
#define DS5_FORMAT_ARG(n) __attribute__((format_arg(n)))
#else
#define DS5_FORMAT_ARG(n)
#endif
DS5_FORMAT_ARG(1) inline const char* tr(const char* es, const char* en) { return isEnglish() ? en : es; }
inline const wchar_t* trw(const wchar_t* es, const wchar_t* en) { return isEnglish() ? en : es; }

}  // namespace ds5

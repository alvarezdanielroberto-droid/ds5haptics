// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// profiles.h — Perfiles por juego (ds5haptics.ini). Portable, sin dependencias.
//
// Formato:
//   [general]                 valores por defecto
//   parlante = 1.0            volumen extra del parlante del mando (1 = sin cambio)
//   haptica  = 1.0            intensidad extra de la háptica
//   volumen_mando_max = no    forzar al máximo el volumen del parlante que pide el juego
//
//   [phantom]                 se aplica cuando el juego en primer plano tiene "phantom"
//   parlante = 3              en el nombre de su .exe (sin distinguir mayúsculas)
//
// Lo que no se define en un perfil se toma de [general].
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "i18n.h"

namespace ds5 {

enum PadMode { kModePs5 = 0, kModePs4 = 1, kModeXbox = 2 };

inline const char* padModeName(int m) { return m == kModePs4 ? "ps4" : m == kModeXbox ? "xbox" : "ps5"; }

// Acepta "ps5", "ps4", "xbox" (y variantes); -1 si no se reconoce.
inline int parsePadMode(std::string v) {
    for (auto& c : v) c = char(std::tolower((unsigned char)c));
    size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
    v = a == std::string::npos ? "" : v.substr(a, b - a + 1);
    if (v == "ps5" || v == "dualsense") return kModePs5;
    if (v == "ps4" || v == "ds4" || v == "dualshock4" || v == "dualshock") return kModePs4;
    if (v == "xbox" || v == "xbox360" || v == "360" || v == "xinput") return kModeXbox;
    return -1;
}

struct ProfileValues {
    float speakerGain = 1.0f;
    float hapticGain = 1.0f;
    bool maxSpeakerVolume = false;
    int mode = kModePs5;        // cómo se presenta el mando ante el juego
};

struct Profile {
    std::string match;          // texto a buscar en el nombre del .exe (minúsculas)
    float speakerGain = -1;     // -1 = heredar de [general]
    float hapticGain = -1;
    int maxSpeakerVolume = -1;
    int mode = -1;
};

struct ProfileSet {
    ProfileValues general;
    std::vector<Profile> games;
    int lowBattery = 15;        // % desde el que parpadea el LED del micrófono (0 = desactivado)
    bool autoXbox = true;       // juegos sin perfil que no usan el DualSense cambian de modo solos
    int autoMode = kModeXbox;   // a qué modo pasan esos juegos (xbox o ps4)
    int language = -1;          // -1 = el de Windows, 0 = español, 1 = inglés, 2 = argento
    bool voice = true;          // avisos hablados por el parlante del mando
    float voiceVolume = 2.0f;   // volumen de los avisos (1 = como sale de Windows, hasta 10)
    bool restoreAudio = true;   // si Windows elige al mando como salida/entrada de audio, volver a la anterior

    // Nombre del programa sin carpeta ni ".exe", en minúsculas ("BatmanAK.exe" -> "batmanak")
    static std::string baseName(const std::string& exeName) {
        std::string exe = lower(exeName);
        size_t slash = exe.find_last_of("\\/");
        if (slash != std::string::npos) exe = exe.substr(slash + 1);
        if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0) exe.resize(exe.size() - 4);
        return exe;
    }

    // Modo que fija el perfil de ese juego (-1 si no tiene perfil o el perfil no dice "modo")
    int explicitMode(const std::string& exeName) const {
        std::string exe = baseName(exeName);
        for (const auto& g : games)
            if (!g.match.empty() && exe.find(g.match) != std::string::npos) return g.mode;
        return -1;
    }

    // Devuelve los valores para un .exe y el nombre del perfil aplicado ("" = general)
    ProfileValues resolve(const std::string& exeName, std::string* matched = nullptr) const {
        // Comparar solo contra el nombre del programa: sin carpeta ni ".exe"
        // (si no, un perfil [x] coincidiría con cualquier "algo.exe").
        std::string exe = baseName(exeName);
        ProfileValues v = general;
        if (matched) matched->clear();
        for (const auto& g : games) {
            if (g.match.empty() || exe.find(g.match) == std::string::npos) continue;
            if (g.speakerGain >= 0) v.speakerGain = g.speakerGain;
            if (g.hapticGain >= 0) v.hapticGain = g.hapticGain;
            if (g.maxSpeakerVolume >= 0) v.maxSpeakerVolume = g.maxSpeakerVolume != 0;
            if (g.mode >= 0) v.mode = g.mode;
            if (matched) *matched = g.match;
            break;   // el primero que coincide
        }
        return v;
    }

    static std::string lower(std::string s) {
        for (auto& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    }
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
    static bool parseBool(const std::string& v) {
        std::string x = lower(trim(v));
        return x == "si" || x == "sí" || x == "s\xc3\xad" || x == "1" || x == "yes" || x == "true" || x == "on";
    }
    static float parseGain(const std::string& v) {
        std::string x = trim(v);
        std::replace(x.begin(), x.end(), ',', '.');   // aceptar "1,5"
        float g = float(std::atof(x.c_str()));
        return std::clamp(g, 0.0f, 10.0f);
    }

    static ProfileSet parse(const std::string& text) {
        ProfileSet ps;
        Profile* cur = nullptr;
        bool inGeneral = true;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);  // BOM
            size_t hash = line.find_first_of("#;");
            if (hash != std::string::npos) line = line.substr(0, hash);
            line = trim(line);
            if (line.empty()) continue;
            if (line.front() == '[' && line.back() == ']') {
                std::string name = lower(trim(line.substr(1, line.size() - 2)));
                if (name == "general") { inGeneral = true; cur = nullptr; }
                else {
                    // Una sección repetida ([PBZ] y más abajo [pbz]) se une a la primera
                    inGeneral = false;
                    cur = nullptr;
                    for (auto& g : ps.games) if (g.match == name) { cur = &g; break; }
                    if (!cur) { ps.games.push_back({name}); cur = &ps.games.back(); }
                }
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower(trim(line.substr(0, eq))), val = trim(line.substr(eq + 1));
            if (key == "parlante" || key == "speaker") {
                if (inGeneral) ps.general.speakerGain = parseGain(val); else if (cur) cur->speakerGain = parseGain(val);
            } else if (key == "haptica" || key == "háptica" || key == "haptics") {
                if (inGeneral) ps.general.hapticGain = parseGain(val); else if (cur) cur->hapticGain = parseGain(val);
            } else if (key == "volumen_mando_max" || key == "max_controller_volume") {
                if (inGeneral) ps.general.maxSpeakerVolume = parseBool(val); else if (cur) cur->maxSpeakerVolume = parseBool(val) ? 1 : 0;
            } else if (key == "modo" || key == "mode") {
                int m = parsePadMode(val);
                if (m >= 0) { if (inGeneral) ps.general.mode = m; else if (cur) cur->mode = m; }
            } else if (key == "bateria_baja" || key == "batería_baja" || key == "low_battery") {
                if (inGeneral) ps.lowBattery = std::clamp(std::atoi(val.c_str()), 0, 95);
            } else if (key == "xbox_automatico" || key == "xbox_automático" || key == "auto_xbox") {
                if (inGeneral) ps.autoXbox = parseBool(val);
            } else if (key == "modo_automatico" || key == "modo_automático" || key == "auto_mode") {
                if (inGeneral) {
                    int m = parsePadMode(val);
                    if (m == kModeXbox || m == kModePs4) { ps.autoMode = m; ps.autoXbox = true; }
                    else if (!parseBool(val)) ps.autoXbox = false;   // "no"
                }
            } else if (key == "voz" || key == "voice") {
                if (inGeneral) ps.voice = parseBool(val);
            } else if (key == "volumen_voz" || key == "voice_volume") {
                if (inGeneral) ps.voiceVolume = std::max(1.0f, parseGain(val));
            } else if (key == "restaurar_audio" || key == "restore_audio") {
                if (inGeneral) ps.restoreAudio = parseBool(val);
            } else if (key == "idioma" || key == "language") {
                std::string v = lower(trim(val));
                if (inGeneral) ps.language = (v == "en" || v == "english" || v == "ingles" || v == "inglés" || v == "ingl\xc3\xa9s") ? 1
                                           : (v == "argento" || v == "es-ar" || v == "ar" || v == "rioplatense") ? 2
                                           : (v == "es" || v == "spanish" || v == "espanol" || v == "español" || v == "espa\xc3\xb1ol") ? 0 : -1;
            }
        }
        return ps;
    }
};

// Programas que no son juegos aunque estén a pantalla completa (videos, navegadores,
// launchers): no cambian el perfil ni pasan a modo Xbox.
inline bool isNonGameApp(const std::string& exeName) {
    static const char* names[] = {
        // escritorio y sistema
        "explorer", "searchhost", "startmenuexperiencehost", "shellexperiencehost", "textinputhost",
        "lockapp", "gamebar", "gamebarftserver", "taskmgr", "notepad", "cmd", "powershell", "pwsh",
        "windowsterminal", "systemsettings", "applicationframehost", "mstsc", "magnify", "snippingtool",
        "screenclippinghost", "claude",
        // navegadores
        "brave", "chrome", "msedge", "firefox", "opera", "opera_gx", "vivaldi", "iexplore", "librewolf",
        "waterfox", "thorium", "yandex", "arc",
        // video, música e imágenes
        "vlc", "mpc-hc", "mpc-hc64", "mpc-be", "mpc-be64", "potplayer", "potplayermini", "potplayermini64",
        "potplayer64", "wmplayer", "video.ui", "microsoft.media.player", "mpv", "kodi", "plex",
        "plex media player", "stremio", "spotify", "itunes", "photos", "microsoft.photos", "smplayer",
        "kmplayer", "gom", "mediaplayer", "netflix",
        // comunicación, grabación y oficina
        "discord", "obs64", "obs32", "zoom", "teams", "ms-teams", "slack", "whatsapp", "telegram",
        "powerpnt", "acrord32", "acrobat",
        // tiendas y launchers
        "steam", "steamwebhelper", "epicgameslauncher", "galaxyclient", "eadesktop", "origin",
        "ubisoftconnect", "upc", "battle.net", "playnite.fullscreenapp", "playnite.desktopapp",
        "xboxpcapp", "gamelaunchhelper", "gamedesk",
    };
    std::string b = ProfileSet::baseName(exeName);
    if (b.empty()) return true;
    for (auto n : names) if (b == n) return true;
    return false;
}

// Ganancia con limitador suave: lo bajo sube de forma lineal, lo fuerte se redondea
// en vez de recortarse en seco (evita la distorsión áspera al subir el volumen).
inline float softGain(float x, float gain) {
    if (gain <= 1.0f) return x * gain;
    return std::tanh(x * gain);
}

static const char* const kDefaultIniEs =
    "# ds5haptics.ini - perfiles por juego\r\n"
    "# Los cambios se aplican solos al guardar (no hace falta reiniciar nada).\r\n"
    "#\r\n"
    "# parlante = volumen extra del parlante del mando (1 = normal, 2 = el doble, hasta 10)\r\n"
    "# haptica  = intensidad extra de la haptica (1 = normal)\r\n"
    "# volumen_mando_max = si/no: ignora el volumen de parlante que pide el juego y usa el maximo\r\n"
    "# modo = ps5 / ps4 / xbox: como ve el juego al mando. ps5 = DualSense completo (haptica,\r\n"
    "#        gatillos, parlante). ps4 y xbox = para juegos que no admiten el DualSense\r\n"
    "#        (solo vibracion clasica). Probar primero ps4; si no anda, xbox.\r\n"
    "# bateria_baja = porcentaje desde el que parpadea la luz del microfono (0 = nunca).\r\n"
    "#        Solo vale en [general].\r\n"
    "# xbox_automatico = si/no: un juego sin perfil que en 20 s no usa el DualSense (pero si usa\r\n"
    "#        mandos de Xbox) pasa solo a modo xbox, y se anota al final de este archivo.\r\n"
    "#        Navegadores y reproductores de video no cuentan. Solo vale en [general].\r\n"
    "# modo_automatico = xbox / ps4: a que modo pasan esos juegos (ps4 = iconos de PlayStation\r\n"
    "#        en los juegos que admiten el mando de PS4). Solo en [general].\r\n"
    "# voz = si/no: avisos hablados por el parlante del mando (modo, bateria baja).\r\n"
    "# volumen_voz = volumen de esos avisos (1 a 10). Solo en [general].\r\n"
    "# restaurar_audio = si/no: si Windows elige al mando como salida o microfono, vuelve\r\n"
    "#        al que tenias. Solo en [general].\r\n"
    "# idioma = es / en / argento / auto: idioma de los mensajes (auto = el de Windows).\r\n"
    "#        Solo en [general].\r\n"
    "\r\n"
    "[general]\r\n"
    "parlante = 1\r\n"
    "haptica = 1\r\n"
    "volumen_mando_max = no\r\n"
    "modo = ps5\r\n"
    "bateria_baja = 15\r\n"
    "xbox_automatico = si\r\n"
    "modo_automatico = xbox\r\n"
    "voz = si\r\n"
    "volumen_voz = 2\r\n"
    "restaurar_audio = si\r\n"
    "idioma = auto\r\n"
    "\r\n"
    "# Un perfil por juego: entre corchetes, parte del nombre del .exe del juego.\r\n"
    "# El nombre exacto aparece en ds5haptics.log (\"Juego en primer plano: ...\").\r\n"
    "# Ejemplo para un juego sin soporte de DualSense (quitar los # y poner su .exe):\r\n"
    "# [nombredeljuego]\r\n"
    "# modo = xbox\r\n";

static const char* const kDefaultIniEn =
    "# ds5haptics.ini - per-game profiles\r\n"
    "# Changes apply by themselves when you save (no need to restart anything).\r\n"
    "#\r\n"
    "# speaker = extra volume for the controller speaker (1 = normal, 2 = double, up to 10)\r\n"
    "# haptics = extra haptics intensity (1 = normal)\r\n"
    "# max_controller_volume = yes/no: ignore the speaker volume the game asks for and use the maximum\r\n"
    "# mode = ps5 / ps4 / xbox: how games see the controller. ps5 = full DualSense (haptics,\r\n"
    "#        triggers, speaker). ps4 and xbox = for games without DualSense support\r\n"
    "#        (classic rumble only). Try ps4 first; if it doesn't work, xbox.\r\n"
    "# low_battery = percentage at which the mic button light starts pulsing (0 = never).\r\n"
    "#        Only in [general].\r\n"
    "# auto_xbox = yes/no: a game without a profile that doesn't use the DualSense within 20 s\r\n"
    "#        (but does use Xbox controllers) switches to xbox mode by itself and is saved at the\r\n"
    "#        end of this file. Browsers and video players don't count. Only in [general].\r\n"
    "# auto_mode = xbox / ps4: which mode those games switch to (ps4 = PlayStation icons in\r\n"
    "#        games that support the PS4 controller). Only in [general].\r\n"
    "# voice = yes/no: spoken notices through the controller speaker (mode, low battery).\r\n"
    "# voice_volume = volume of those notices (1 to 10). Only in [general].\r\n"
    "# restore_audio = yes/no: if Windows picks the controller as the audio output or mic,\r\n"
    "#        switch back to the one you had. Only in [general].\r\n"
    "# language = en / es / argento / auto: message language (auto = the Windows language).\r\n"
    "#        Only in [general].\r\n"
    "\r\n"
    "[general]\r\n"
    "speaker = 1\r\n"
    "haptics = 1\r\n"
    "max_controller_volume = no\r\n"
    "mode = ps5\r\n"
    "low_battery = 15\r\n"
    "auto_xbox = yes\r\n"
    "auto_mode = xbox\r\n"
    "voice = yes\r\n"
    "voice_volume = 2\r\n"
    "restore_audio = yes\r\n"
    "language = auto\r\n"
    "\r\n"
    "# One profile per game: in brackets, part of the game's .exe name.\r\n"
    "# The exact name shows up in ds5haptics.log (\"Game in the foreground: ...\").\r\n"
    "# Example for a game without DualSense support (remove the # and use its .exe):\r\n"
    "# [gamename]\r\n"
    "# mode = xbox\r\n";

inline const char* defaultIni() { return tr(kDefaultIniEs, kDefaultIniEn); }
// Compatibilidad con el nombre anterior (tests)
static const char* const kDefaultIni = kDefaultIniEs;

// Se agrega al final de un ds5haptics.ini de una versión anterior (que no tenía estas opciones).
// Un segundo bloque [general] es válido: sus valores se suman a los del primero.
static const char* const kIniUpgradeBlock =
    "\r\n"
    "# --- Opciones nuevas (agregadas automaticamente al actualizar ds5haptics) ---\r\n"
    "# modo = ps5 / ps4 / xbox: como ve el juego al mando. ps5 = DualSense completo.\r\n"
    "#        ps4 y xbox = para juegos que no admiten el DualSense (solo vibracion clasica).\r\n"
    "#        Se puede poner aca (para todo) o dentro del perfil de un juego, por ejemplo:\r\n"
    "#            [nombredeljuego]\r\n"
    "#            modo = ps4\r\n"
    "# bateria_baja = porcentaje desde el que parpadea la luz del microfono (0 = nunca).\r\n"
    "[general]\r\n"
    "modo = ps5\r\n"
    "bateria_baja = 15\r\n";

// Segunda actualización: documenta xbox_automatico (que viene activado igual aunque falte).
static const char* const kIniUpgradeBlock2 =
    "\r\n"
    "# --- Opcion nueva (agregada automaticamente al actualizar ds5haptics) ---\r\n"
    "# xbox_automatico = si/no: un juego sin perfil que en 20 s no usa el DualSense pasa solo\r\n"
    "#        a modo xbox, y se anota al final de este archivo para la proxima vez.\r\n"
    "#        Si un juego quedo mal anotado, cambia su modo o borra su perfil.\r\n"
    "[general]\r\n"
    "xbox_automatico = si\r\n";

// Tercera actualización: el idioma (los .ini en inglés nunca necesitan las anteriores)
static const char* const kIniUpgradeBlock3 =
    "\r\n"
    "# --- Opcion nueva / New option ---\r\n"
    "# idioma = es / en / auto: idioma de los mensajes (auto = el de Windows).\r\n"
    "# language = es / en / auto: message language (auto = the Windows language).\r\n"
    "[general]\r\n"
    "idioma = auto\r\n";

// Cuarta actualización (v1.0.1): voz, restaurar el audio y a qué modo pasan los juegos
static const char* const kIniUpgradeBlock4 =
    "\r\n"
    "# --- Opciones nuevas / New options (v1.0.1) ---\r\n"
    "# voz = si/no: avisos hablados por el parlante del mando (modo, bateria baja).\r\n"
    "# volumen_voz = volumen de esos avisos (1 a 10). Solo en [general].\r\n"
    "# restaurar_audio = si/no: si Windows elige al mando como salida o microfono, vuelve al anterior.\r\n"
    "# modo_automatico = xbox / ps4: a que modo pasan los juegos que no usan el DualSense.\r\n"
    "# idioma tambien acepta: argento\r\n"
    "# voice / restore_audio / auto_mode = the same, in English.\r\n"
    "[general]\r\n"
    "voz = si\r\n"
    "volumen_voz = 2\r\n"
    "restaurar_audio = si\r\n"
    "modo_automatico = xbox\r\n";

// Perfil que se agrega solo cuando un juego no usó el DualSense.
inline std::string autoProfileBlock(const std::string& exeName, int mode = kModeXbox) {
    const std::string m = padModeName(mode == kModePs4 ? kModePs4 : kModeXbox);
    if (isEnglish())
        return "\r\n# Added automatically: " + exeName + " did not use the DualSense\r\n[" +
               ProfileSet::baseName(exeName) + "]\r\nmode = " + m + "\r\n";
    return "\r\n# Agregado automaticamente: " + exeName + " no uso el DualSense\r\n[" +
           ProfileSet::baseName(exeName) + "]\r\nmodo = " + m + "\r\n";
}

}  // namespace ds5

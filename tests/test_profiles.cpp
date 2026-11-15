// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// Test de perfiles por juego y del limitador suave.
#include "profiles.h"
#include <cstdio>
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FALLO: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)
int main() {
    auto ps = ds5::ProfileSet::parse(std::string(ds5::kDefaultIni) + "\n[phantom]\nparlante = 3\n\n[stellar]\nparlante = 1\n");
    std::string m;
    auto v = ps.resolve("PhantomBladeZero-Win64-Shipping.exe", &m);
    CHECK(m == "phantom" && v.speakerGain == 3.0f && v.hapticGain == 1.0f && !v.maxSpeakerVolume, "perfil phantom");
    v = ps.resolve("SB-Win64-Shipping.exe", &m);
    CHECK(m.empty() && v.speakerGain == 1.0f, "sin perfil -> general");
    v = ps.resolve("StellarBlade.exe", &m);
    CHECK(m == "stellar" && v.speakerGain == 1.0f, "perfil stellar");

    auto p2 = ds5::ProfileSet::parse(
        "\xEF\xBB\xBF[general]\nparlante = 1,5  # comentario\nhaptica=2\nvolumen_mando_max = sí\n\n"
        "[Juego Raro]\r\nhaptica = 0.5\r\nvolumen_mando_max = no\r\n[x]\nparlante = 99\n");
    v = p2.resolve("c:/x/JUEGO RARO.exe", &m);
    CHECK(m == "juego raro" && v.speakerGain == 1.5f && v.hapticGain == 0.5f && !v.maxSpeakerVolume, "herencia, coma decimal, mayúsculas");
    v = p2.resolve("otro.exe");
    CHECK(v.speakerGain == 1.5f && v.hapticGain == 2.0f && v.maxSpeakerVolume, "general con sí y BOM");
    v = p2.resolve("xyz.exe", &m);
    CHECK(m == "x" && v.speakerGain == 10.0f, "tope de 10");

    // Modo de compatibilidad y batería baja
    auto p3 = ds5::ProfileSet::parse("[general]\nmodo = PS5\nbateria_baja = 25\n[viejo]\nmodo = xbox\n[otro]\nmodo = DualShock4\n[raro]\nmodo = wii\n");
    CHECK(p3.lowBattery == 25 && p3.general.mode == ds5::kModePs5, "general: modo y batería");
    CHECK(p3.resolve("JuegoViejo.exe").mode == ds5::kModeXbox, "modo xbox por juego");
    CHECK(p3.resolve("otro.exe").mode == ds5::kModePs4, "modo ps4 por juego");
    CHECK(p3.resolve("raro.exe").mode == ds5::kModePs5, "modo desconocido -> hereda el general");
    CHECK(ps.lowBattery == 15 && ps.general.mode == ds5::kModePs5 && ps.resolve("x").mode == ds5::kModePs5, "valores por defecto");
    CHECK(ds5::ProfileSet::parse("[general]\nbateria_baja = 0\n").lowBattery == 0, "batería baja desactivada");

    // Ini de la versión anterior + bloque de actualización al final
    {
        std::string oldIni = "[general]\nparlante = 2\n[PBZ]\nparlante = 7\nvolumen_mando_max = si\n";
        auto up = ds5::ProfileSet::parse(oldIni + ds5::kIniUpgradeBlock);
        auto pv = up.resolve("PBZ.exe");
        CHECK(up.general.speakerGain == 2.0f && up.general.mode == ds5::kModePs5 && up.lowBattery == 15, "actualización: general conserva sus valores");
        CHECK(pv.speakerGain == 7.0f && pv.maxSpeakerVolume && pv.mode == ds5::kModePs5, "actualización: el perfil del juego no cambia");
        auto up2 = ds5::ProfileSet::parse(oldIni + ds5::kIniUpgradeBlock + "[otrojuego]\nmodo = xbox\n");
        CHECK(up2.resolve("OtroJuego.exe").mode == ds5::kModeXbox && up2.resolve("PBZ.exe").mode == ds5::kModePs5, "perfil agregado después del bloque");
    }

    // Modo explícito y Xbox automático
    {
        auto pa = ds5::ProfileSet::parse("[general]\nmodo = ps5\n[batman]\nmodo = xbox\n[pbz]\nparlante = 7\n");
        CHECK(pa.autoXbox, "xbox automático activado por defecto");
        CHECK(pa.explicitMode("C:/x/BatmanAK.exe") == ds5::kModeXbox, "modo explícito del perfil");
        CHECK(pa.explicitMode("PBZ.exe") == -1, "perfil sin modo -> -1");
        CHECK(pa.explicitMode("Otro.exe") == -1, "sin perfil -> -1");
        CHECK(!ds5::ProfileSet::parse("[general]\nxbox_automatico = no\n").autoXbox, "xbox automático desactivable");
        std::string ini = std::string(ds5::kDefaultIni) + ds5::autoProfileBlock("Juego-Win64-Shipping.exe");
        auto pb = ds5::ProfileSet::parse(ini);
        CHECK(pb.explicitMode("Juego-Win64-Shipping.exe") == ds5::kModeXbox && pb.resolve("Juego-Win64-Shipping.exe").mode == ds5::kModeXbox,
              "el perfil automático se lee bien");
        CHECK(pb.resolve("StellarBlade.exe").mode == ds5::kModePs5, "el perfil automático no afecta a otros");
        auto pc = ds5::ProfileSet::parse(std::string("[general]\nparlante=2\n") + ds5::kIniUpgradeBlock + ds5::kIniUpgradeBlock2);
        CHECK(pc.general.speakerGain == 2.0f && pc.autoXbox && pc.lowBattery == 15, "actualización 2");
    }

    // Secciones repetidas se unen; programas que no son juegos
    {
        auto pd = ds5::ProfileSet::parse("[PBZ]\nparlante = 7\nvolumen_mando_max = si\n[stellar]\nparlante=1\n[pbz]\nmodo = ps5\n[pbz]\nhaptica = 2\n");
        auto v = pd.resolve("PBZ.exe");
        CHECK(pd.games.size() == 2, "secciones [PBZ]/[pbz] unidas (%zu perfiles)", pd.games.size());
        CHECK(v.speakerGain == 7.0f && v.maxSpeakerVolume && v.hapticGain == 2.0f && pd.explicitMode("PBZ.exe") == ds5::kModePs5,
              "el perfil unido conserva todo");
        CHECK(ds5::isNonGameApp("brave.exe") && ds5::isNonGameApp("C:/Program Files/VideoLAN/VLC/vlc.exe") &&
              ds5::isNonGameApp("Spotify.exe") && ds5::isNonGameApp(""), "navegadores y reproductores excluidos");
        CHECK(!ds5::isNonGameApp("PBZ.exe") && !ds5::isNonGameApp("BatmanAK.exe") && !ds5::isNonGameApp("NFS11Remastered.exe"),
              "los juegos no se excluyen");
    }

    // Idioma y archivo en inglés
    {
        auto pe = ds5::ProfileSet::parse(ds5::kDefaultIniEn);
        CHECK(pe.general.speakerGain == 1.0f && pe.general.mode == ds5::kModePs5 && pe.lowBattery == 15 &&
              pe.autoXbox && pe.language == -1, "ini en inglés por defecto");
        auto pf = ds5::ProfileSet::parse("[general]\nlanguage = en\nauto_xbox = no\nlow_battery = 20\n[game]\nspeaker = 4\nmode = xbox\nmax_controller_volume = yes\n");
        auto v = pf.resolve("MyGame.exe");
        CHECK(pf.language == 1 && !pf.autoXbox && pf.lowBattery == 20, "claves en inglés en [general]");
        CHECK(v.speakerGain == 4.0f && v.mode == ds5::kModeXbox && v.maxSpeakerVolume, "claves en inglés en un perfil");
        CHECK(ds5::ProfileSet::parse("[general]\nidioma = es\n").language == 0, "idioma = es");
        CHECK(ds5::ProfileSet::parse("[general]\nidioma = auto\n").language == -1, "idioma = auto");
        ds5::setEnglish(true);
        CHECK(std::string(ds5::defaultIni()) == ds5::kDefaultIniEn && ds5::autoProfileBlock("X.exe").find("mode = xbox") != std::string::npos,
              "textos en inglés");
        ds5::setEnglish(false);
        CHECK(ds5::autoProfileBlock("X.exe").find("modo = xbox") != std::string::npos, "textos en español");
    }

    // Limitador suave: lo bajo sube casi lineal, lo fuerte no pasa de 1
    CHECK(std::fabs(ds5::softGain(0.05f, 3) - 0.15f) < 0.002f, "bajo x3 = %.4f", ds5::softGain(0.05f, 3));
    CHECK(ds5::softGain(0.9f, 3) < 1.0f && ds5::softGain(0.9f, 3) > 0.99f, "fuerte limitado");
    CHECK(ds5::softGain(-0.5f, 1) == -0.5f && ds5::softGain(0.5f, 0.5f) == 0.25f, "sin ganancia / atenuar");
    if (!failures) std::printf("OK: perfiles por juego y limitador correctos\n");
    return failures ? 1 : 0;
}

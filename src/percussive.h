// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// percussive.h — EXPERIMENTAL: saca del sonido del juego solo los chasquidos intensos (disparos,
// latigazos, clics metálicos, vidrios, golpes secos) para el parlante del mando. Las pisadas,
// las voces y la música no pasan. Portable, sin dependencias.
//
// Cómo: separación armónico/percusiva (HPSS, Fitzgerald 2010) en tiempo real.
//   - El sonido se analiza en ventanas de 2048 muestras (43 ms) cada 512 (10,7 ms). Con
//     ventanas largas se separan bien los armónicos de una voz.
//   - Un sonido que dura (voz, música, motor, sirena) forma líneas "horizontales" en el
//     espectro: lo estima la mediana de cada frecuencia a lo largo del tiempo (5 ventanas
//     centradas: hace falta mirar 2 ventanas hacia adelante, ~21 ms).
//   - Un golpe es una línea "vertical": todas las frecuencias a la vez, por poco tiempo. Lo
//     estima la mediana de las frecuencias vecinas en la misma ventana.
//   - Cada frecuencia se reparte según cuál de las dos gana (máscara de Wiener).
//   - Lo percusivo que queda pasa por una "compuerta": solo se abre si sube de golpe por
//     encima de su fondo (así la lluvia o el viento, que son parejos, no pasan), si es buena
//     parte del sonido o salta de golpe, y si no es periódico (una voz o una nota repiten un
//     tono aunque se mueva; una pisada o un golpe son ruido). Además el pico agudo tiene que
//     sobresalir mucho del volumen promedio del juego (~12 veces) y el golpe tiene que ser
//     agudo (centroide > ~3,7 kHz): así quedan los chasquidos y no las pisadas.
//   - Sin graves (< 250 Hz, eso va a la háptica) y normalizado contra el volumen del juego.
// Atraso total ~53 ms (ventana + lo que mira hacia adelante). Un golpe es corto: se escucha
// como parte de la acción.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace ds5 {

class PercussiveSpeaker {
public:
    static constexpr int kN = 2048;          // tamaño de ventana (48 kHz, 43 ms): separa bien los armónicos de una voz
    static constexpr int kHop = 512;          // 10,7 ms
    static constexpr int kBins = kN / 2 + 1;
    static constexpr int kAhead = 2;          // ventanas hacia adelante (y hacia atrás) de la mediana
    static constexpr int kHist = 2 * kAhead + 1;
    static constexpr int kVert = 16;          // bins vecinos a cada lado (±375 Hz)

    PercussiveSpeaker() {
        win_.resize(kN);
        for (int i = 0; i < kN; ++i) win_[i] = float(std::sqrt(0.5 - 0.5 * std::cos(2.0 * kPi * i / kN)));   // raíz de Hann
        tw_.resize(kN / 2);
        for (int i = 0; i < kN / 2; ++i) tw_[i] = std::polar(1.0f, float(-2.0 * kPi * i / kN));
        rev_.resize(kN);
        for (int i = 0, j = 0; i < kN; ++i) {
            rev_[i] = j;
            int bit = kN >> 1;
            while (j & bit) { j ^= bit; bit >>= 1; }
            j |= bit;
        }
        // Corrección de la autocorrelación por la ventana (Hann al cuadrado de la raíz = Hann):
        // a mayor retardo, menos superposición
        winAc_.assign(size_t(kN), 0.0f);
        {
            std::vector<double> w2(kN);
            for (int i = 0; i < kN; ++i) w2[size_t(i)] = double(win_[size_t(i)]) * win_[size_t(i)];
            double a0 = 0;
            for (int i = 0; i < kN; ++i) a0 += w2[size_t(i)] * w2[size_t(i)];
            for (int lag = kLagMin; lag <= kLagMax; ++lag) {
                double a = 0;
                for (int i = 0; i + lag < kN; ++i) a += w2[size_t(i)] * w2[size_t(i + lag)];
                winAc_[size_t(lag)] = float(a > 0 ? a0 / a : 0);
            }
        }
        const double hz = 48000.0 / kN;
        lo_ = int(std::ceil(250.0 / hz));
        hi_ = int(std::floor(10000.0 / hz));
        reset();
    }

    void reset() {
        in_.assign(size_t(kN) * 2, 0.0f);
        fill_ = 0;
        out_.assign(size_t(kN) * 2, 0.0f);
        mag_.assign(size_t(kHist) * kBins, 0.0f);
        spec_.assign(size_t(kAhead + 1) * kN, {});
        frames_ = 0;
        z_.assign(size_t(kN), {});
        y_.assign(size_t(kN), {});
        ac_.assign(size_t(kN), {});
        mask_.assign(size_t(kBins), 0.0f);
        bg_ = 0;
        ref_ = kRefMin;
        gate_ = 0;
        open_ = false;
    }

    // sensibilidad 1..10 (10 = deja pasar más); ganancia final (como el volumen del parlante)
    void configure(int sensitivity, float gain) {
        sens_ = float(std::clamp(sensitivity, 1, 10));
        gain_ = gain;
    }

    unsigned hits() const { return hits_; }
    float lastPeriodicity() const { return periodic_; }
    float lastPercussiveFraction() const { return percFrac_; }
    float lastBurstRatio() const { return burstRatio_; }
    float lastFractionJump() const { return fracJump_; }
    float lastCentroid() const { return centroid_; }
    float lastLoudness() const { return loudness_; }

    // Entrada estéreo 48 kHz intercalada; agrega a 'out' la misma cantidad de frames.
    void process(const float* st, size_t frames, std::vector<float>& out) {
        for (size_t i = 0; i < frames; ++i) {
            in_[size_t(fill_) * 2] = st[i * 2];
            in_[size_t(fill_) * 2 + 1] = st[i * 2 + 1];
            {   // pico de la parte aguda (> ~1,5 kHz) y volumen promedio, para medir "chasquidos"
                const float m = 0.5f * (st[i * 2] + st[i * 2 + 1]);
                hpZ_ += 0.18f * (m - hpZ_);
                const float h = m - hpZ_;
                hopPeak_ = std::max(hopPeak_, std::fabs(h));
                hopPow_ += m * m;
            }
            ++fill_;
            if (fill_ == kN) {
                frame();
                // correr la entrada un salto
                std::copy(in_.begin() + kHop * 2, in_.end(), in_.begin());
                fill_ = kN - kHop;
                // entregar un salto de salida
                for (int k = 0; k < kHop * 2; ++k) {   // que no sature: recorte suave
                    const float v = out_[size_t(k)];
                    out.push_back(v == 0.0f ? 0.0f : std::tanh(v));
                }
                std::copy(out_.begin() + kHop * 2, out_.end(), out_.begin());
                std::fill(out_.end() - kHop * 2, out_.end(), 0.0f);
            }
        }
    }

    static constexpr int latencyFrames() { return kN - kHop + kAhead * kHop; }

private:
    using cf = std::complex<float>;
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr float kRefMin = 0.01f;   // amplificación máxima ~70x
    static constexpr float kPeak = 0.8f;      // volumen de un golpe al máximo reciente del juego

    void fft(std::vector<cf>& a, bool inverse) const {
        for (int i = 0; i < kN; ++i)
            if (i < rev_[i]) std::swap(a[size_t(i)], a[size_t(rev_[i])]);
        for (int len = 2; len <= kN; len <<= 1) {
            const int step = kN / len;
            for (int i = 0; i < kN; i += len)
                for (int j = 0; j < len / 2; ++j) {
                    const cf w = tw_[size_t(j * step)];
                    const float wi = inverse ? -w.imag() : w.imag();
                    const cf b = a[size_t(i + j + len / 2)];
                    // multiplicación a mano (la de std::complex revisa NaN e infinitos: lenta)
                    const cf u = a[size_t(i + j)], v(b.real() * w.real() - b.imag() * wi, b.real() * wi + b.imag() * w.real());
                    a[size_t(i + j)] = u + v;
                    a[size_t(i + j + len / 2)] = u - v;
                }
        }
    }

    // Periodicidad (0..1) del espectro de magnitud 'mag' (con la máscara, si hay)
    float periodicity(const float* mask, const float* mag) {
        std::vector<cf>& ac = ac_;
        std::fill(ac.begin(), ac.end(), cf(0, 0));
        for (int k = lo_; k <= hi_; ++k) {
            const float e = (mask ? mask[k] : 1.0f) * mag[k];
            ac[size_t(k)] = cf(e * e, 0);
            if (k > 0 && k < kN / 2) ac[size_t(kN - k)] = ac[size_t(k)];
        }
        fft(ac, true);
        float best = 0;
        if (ac[0].real() > 0)
            for (int lag = kLagMin; lag <= kLagMax; ++lag)
                best = std::max(best, ac[size_t(lag)].real() / ac[0].real() * winAc_[size_t(lag)]);
        return best;
    }

    void frame() {
        // Espectro de L + jR (las dos señales en una sola FFT)
        const size_t slot = size_t(frames_ % (kAhead + 1)) * kN;
        std::vector<cf>& z = z_;
        float pin = 0;
        for (int i = 0; i < kN; ++i) {
            const float l = in_[size_t(i) * 2], r = in_[size_t(i) * 2 + 1];
            z[size_t(i)] = cf(l * win_[size_t(i)], r * win_[size_t(i)]);
            if (i >= kN - kHop) pin += 0.25f * (l + r) * (l + r);
        }
        fft(z, false);
        std::copy(z.begin(), z.end(), spec_.begin() + long(slot));
        // Magnitud del centro (L+R)/2 en cada frecuencia
        float* m = &mag_[size_t(frames_ % kHist) * kBins];
        for (int k = 0; k < kBins; ++k) {
            const cf a = z[size_t(k)], b = std::conj(z[size_t((kN - k) % kN)]);
            const cf xl = 0.5f * (a + b), xr = cf(0, -0.5f) * (a - b);
            const cf c = 0.5f * (xl + xr);
            m[k] = std::sqrt(c.real() * c.real() + c.imag() * c.imag());   // std::abs usa hypot: lento
        }
        // Pico de este salto (para medir chasquidos) y volumen promedio
        peaks_[size_t(frames_ % kPk)] = hopPeak_;
        {
            const float hp = hopPow_ / kHop;
            if (hp > 1e-9f) powAvg_ += (hp - powAvg_) * kAvgSlow;
        }
        hopPeak_ = 0;
        hopPow_ = 0;
        // Volumen del juego (para normalizar): sube enseguida y baja en ~4 s
        const float rms = std::sqrt(pin / kHop);
        ref_ = rms > ref_ ? rms : std::max(ref_ * kRefRel, kRefMin);
        ++frames_;
        if (frames_ <= kHist) return;   // hace falta el historial completo

        // La ventana que sale ahora es la del medio del historial (kAhead ventanas atrás)
        const long center = frames_ - 1 - kAhead;
        const float* mc = &mag_[size_t(center % kHist) * kBins];
        std::vector<float>& mask = mask_;
        std::fill(mask.begin(), mask.end(), 0.0f);
        float tmp[kHist > 2 * kVert + 1 ? kHist : 2 * kVert + 1];
        float ep = 0;
        for (int k = lo_; k <= hi_; ++k) {
            for (int t = 0; t < kHist; ++t) tmp[t] = mag_[size_t(t) * kBins + size_t(k)];
            std::nth_element(tmp, tmp + kHist / 2, tmp + kHist);
            const float h = tmp[kHist / 2];
            int n = 0;
            for (int j = std::max(0, k - kVert); j <= std::min(kBins - 1, k + kVert); ++j) tmp[n++] = mc[j];
            std::nth_element(tmp, tmp + n / 2, tmp + n);
            const float p = tmp[n / 2];
            const float mk = (p * p) / (p * p + h * h + 1e-20f);
            mask[size_t(k)] = mk;
            const float e = mk * mc[k];
            ep += e * e;
        }
        // ¿Lo percusivo es periódico? Una voz o una nota repiten un tono aunque el tono se mueva
        // (y entonces no forman líneas horizontales); una pisada o un golpe son ruido.
        // Autocorrelación = transformada inversa del espectro de potencia.
        float etot = 0;
        for (int k = lo_; k <= hi_; ++k) etot += mc[k] * mc[k];
        percFrac_ = etot > 0 ? ep / etot : 0;
        // Brillo de lo percusivo (centroide, Hz) y qué tan fuerte es contra el volumen
        // promedio del juego (~3 s): un chasquido intenso es agudo y sobresale mucho
        {
            float num = 0;
            for (int k = lo_; k <= hi_; ++k) {
                const float e = mask[size_t(k)] * mc[k];
                num += float(k) * e * e;
            }
            centroid_ = ep > 0 ? num / ep * (48000.0f / kN) : 0;
        }
        // Pico agudo de las ventanas que forman la ventana del centro, contra el volumen
        // promedio del juego (RMS de ~3 s)
        {
            float pk = 0;
            for (int j = 0; j < kN / kHop; ++j) pk = std::max(pk, peaks_[size_t((center - j + kPk * 4) % kPk)]);
            loudness_ = pk / std::max(std::sqrt(powAvg_), 1e-4f);
        }

        // Compuerta: lo percusivo tiene que subir de golpe por encima de su fondo y no ser un tono
        const float thr = 6.0f - 0.4f * sens_;   // 1 -> 5,6x ; 10 -> 2x (en energía)
        const float floorE = 1e-7f * kN;
        burstRatio_ = ep / std::max(bg_, floorE);
        // La PARTE percusiva del sonido también tiene que saltar: en una voz o una música es
        // siempre más o menos la misma (sube y baja con cada sílaba); una pisada la hace saltar.
        const float fracJump = percFrac_ / std::max(fracBg_, 0.02f);
        fracJump_ = fracJump;
        // Solo chasquidos intensos: el pico agudo tiene que sobresalir mucho del volumen promedio
        // del juego y el golpe tiene que ser agudo (las pisadas, más graves y suaves, no pasan)
        const float crestThr = 17.0f - 1.0f * sens_;      // 1 -> 16x ; 5 -> 12x ; 10 -> 7x
        const float centroidMin = 4500.0f - 150.0f * sens_; // 1 -> 4,35 kHz ; 5 -> 3,75 ; 10 -> 3
        bool burst = ep > floorE && ep > thr * std::max(bg_, floorE) && (percFrac_ > kFracMin || fracJump > kFracJump) &&
                     loudness_ > crestThr && centroid_ > centroidMin;
        if (burst) {   // la periodicidad cuesta una FFT más: solo si hace falta
            periodic_ = periodicity(mask.data(), mc);
            burst = periodic_ < kPeriodicMax;
        }
        if (etot > floorE) fracBg_ += (percFrac_ - fracBg_) * kFracAvg;   // en silencio se mantiene
        // Fondo: baja enseguida (~30 ms) y sube despacio (~1 s), como en la háptica
        bg_ += (ep - bg_) * (ep < bg_ ? kBgDown : kBgUp);
        if (burst) {
            if (!open_) ++hits_;
            open_ = true;
            hold_ = kHoldHops;
        } else if (hold_ > 0) {
            --hold_;
        } else {
            open_ = false;
        }
        const float target = open_ ? 1.0f : 0.0f;
        gate_ += (target - gate_) * (target > gate_ ? 1.0f : kGateRel);   // abre ya, cierra en ~45 ms

        // Aplicar la máscara (simétrica) a L + jR y volver al tiempo (con la compuerta cerrada,
        // que es casi siempre, no hace falta: ahorra la FFT inversa)
        if (gate_ < 1e-4f) { gate_ = 0; return; }
        const float g = gate_ * kPeak * gain_ / ref_;
        std::vector<cf>& y = y_;
        const cf* zc = &spec_[size_t(center % (kAhead + 1)) * kN];
        for (int k = 0; k < kBins; ++k) {
            const float mk = mask[size_t(k)] * g;
            y[size_t(k)] = zc[k] * mk;
            if (k > 0 && k < kN / 2) y[size_t(kN - k)] = zc[kN - k] * mk;
        }
        fft(y, true);
        const float scale = 0.5f / kN;   // raíz de Hann al 75 %: la suma de ventanas da 2
        for (int i = 0; i < kN; ++i) {
            const float w = win_[size_t(i)] * scale;
            out_[size_t(i) * 2] += y[size_t(i)].real() * w;
            out_[size_t(i) * 2 + 1] += y[size_t(i)].imag() * w;
        }
    }

    static constexpr int kLagMin = 48;          // tonos de 1 kHz ...
    static constexpr int kLagMax = 600;         // ... a 80 Hz (voz grave)
    static constexpr float kPeriodicMax = 0.2f; // más que esto es un tono (voz, música, motor)
    static constexpr float kFracMin = 0.15f;    // o lo percusivo ya es buena parte del sonido
    static constexpr float kFracJump = 1.8f;    // la parte percusiva tiene que casi duplicarse
    // Constantes de tiempo por salto (10,7 ms): 1 - e^(-salto/tiempo)
    static constexpr float kFracAvg = 0.035f;    // promedio de la parte percusiva: ~300 ms
    static constexpr float kBgDown = 0.30f;      // fondo: baja en ~30 ms...
    static constexpr float kBgUp = 0.0106f;      // ... y sube en ~1 s
    static constexpr float kGateRel = 0.21f;     // la compuerta se cierra en ~45 ms
    static constexpr float kAvgSlow = 0.0036f;   // volumen promedio del juego: ~3 s
    static constexpr float kRefRel = 0.99734f;   // volumen del juego: baja en ~4 s
    static constexpr int kHoldHops = 6;          // la compuerta queda abierta ~64 ms

    std::vector<float> win_, winAc_;
    float centroid_ = 0, loudness_ = 0, powAvg_ = 0, hpZ_ = 0, hopPeak_ = 0, hopPow_ = 0;
    static constexpr int kPk = 8;   // picos de los últimos saltos (ventana del centro + lo que mira adelante)
    float peaks_[kPk] = {};
    float periodic_ = 0, percFrac_ = 0, burstRatio_ = 0, fracBg_ = 0.1f, fracJump_ = 0;
    std::vector<cf> tw_;
    std::vector<int> rev_;
    int lo_ = 0, hi_ = 0;
    std::vector<float> in_, out_, mag_;
    std::vector<cf> spec_, z_, y_, ac_;
    std::vector<float> mask_;
    int fill_ = 0;
    long frames_ = 0;
    float bg_ = 0, ref_ = kRefMin, gate_ = 0, sens_ = 5, gain_ = 1;
    bool open_ = false;
    int hold_ = 0;
    unsigned hits_ = 0;
};

}  // namespace ds5

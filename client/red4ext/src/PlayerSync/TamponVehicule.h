#pragma once
// ADR 0054 §4 — lissage d'une voiture conduite chez le TEMOIN. Arithmetique pure, aucun type
// moteur (testable hors jeu : tests/verif_tampon_vehicule.cpp). Le tampon des avatars
// (TamponInterpolation.h) n'est pas reutilisable tel quel : il est cadence en ticks et ne porte
// qu'un cap (yaw) ; une voiture a besoin du quaternion complet (tangage, roulis) et d'un temps en
// ms serveur (`Snapshot.ts_ms`).
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Tessera::Sync
{
inline constexpr double kExtrapolationVehiculeMaxMs = 250.0;
inline constexpr std::size_t kProfondeurVehicule = 16;

struct EchantillonVehicule
{
    double tMs = 0.0;
    float pos[3] = {0, 0, 0};
    float q[4] = {0, 0, 0, 1}; // x, y, z, w
    float vit[3] = {0, 0, 0};
};

struct PoseVehicule
{
    float pos[3] = {0, 0, 0};
    float q[4] = {0, 0, 0, 1};
    bool extrapolee = false; // au-dela du dernier echantillon
};

class TamponVehicule
{
public:
    /// Un echantillon dont la date ne depasse pas le dernier est ignore (reordonnancement UDP).
    void Empiler(const EchantillonVehicule& e) noexcept
    {
        if (m_n > 0 && e.tMs <= Dernier().tMs)
        {
            return;
        }
        if (m_n == kProfondeurVehicule)
        {
            for (std::size_t i = 1; i < m_n; ++i) m_e[i - 1] = m_e[i];
            --m_n;
        }
        m_e[m_n++] = e;
    }

    bool Vide() const noexcept { return m_n == 0; }

    /// Pose a la date `tMs`. false si le tampon est vide.
    bool Rendre(double tMs, PoseVehicule& out) const noexcept
    {
        if (m_n == 0) return false;
        out = PoseVehicule{};
        const auto& d = Dernier();
        if (tMs >= d.tMs)
        {
            double dt = tMs - d.tMs;
            out.extrapolee = dt > 0.0;
            if (dt > kExtrapolationVehiculeMaxMs) dt = kExtrapolationVehiculeMaxMs; // puis gel
            for (int i = 0; i < 3; ++i) out.pos[i] = d.pos[i] + d.vit[i] * static_cast<float>(dt / 1000.0);
            for (int i = 0; i < 4; ++i) out.q[i] = d.q[i];
            return true;
        }
        if (tMs <= m_e[0].tMs)
        {
            Copier(m_e[0], out);
            return true;
        }
        std::size_t i = 1;
        while (m_e[i].tMs < tMs) ++i;
        const auto& a = m_e[i - 1];
        const auto& b = m_e[i];
        const float t = static_cast<float>((tMs - a.tMs) / (b.tMs - a.tMs));
        for (int k = 0; k < 3; ++k) out.pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * t;
        Slerp(a.q, b.q, t, out.q);
        return true;
    }

private:
    const EchantillonVehicule& Dernier() const noexcept { return m_e[m_n - 1]; }

    static void Copier(const EchantillonVehicule& s, PoseVehicule& out) noexcept
    {
        for (int i = 0; i < 3; ++i) out.pos[i] = s.pos[i];
        for (int i = 0; i < 4; ++i) out.q[i] = s.q[i];
    }

    static void Slerp(const float* a, const float* b, float t, float* out) noexcept
    {
        float bb[4] = {b[0], b[1], b[2], b[3]};
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        if (dot < 0.0f) // plus court chemin
        {
            dot = -dot;
            for (float& c : bb) c = -c;
        }
        float ka, kb;
        if (dot > 0.9995f) { ka = 1.0f - t; kb = t; } // quasi colineaires : lerp
        else
        {
            const float th = std::acos(dot);
            const float s = std::sin(th);
            ka = std::sin((1.0f - t) * th) / s;
            kb = std::sin(t * th) / s;
        }
        float n = 0.0f;
        for (int i = 0; i < 4; ++i) { out[i] = ka * a[i] + kb * bb[i]; n += out[i] * out[i]; }
        n = std::sqrt(n);
        if (n > 0.0f) for (int i = 0; i < 4; ++i) out[i] /= n;
    }

    EchantillonVehicule m_e[kProfondeurVehicule]{};
    std::size_t m_n = 0;
};
} // namespace Tessera::Sync

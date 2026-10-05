#pragma once
// =====================================================================================
// Suivi direct d'un corps distant en marche au sol, et garde de marche plantee.
// ZERO DEPENDANCE AU MOTEUR (testable hors du jeu : tests/verif_suivi_direct.cpp).
//
// Pourquoi : sur un corps en marche, la correction douce (AITeleportCommand) ne porte presque
// jamais, et un palier unique n'est qu'un eclair d'une image. Seule l'ecriture directe REPETEE
// A CHAQUE IMAGE porte le corps (comme la branche « en l'air »). `PasSuiviDirect` rend la
// position a ecrire : X/Y rapproches d'une fraction de l'ecart (plafonnee), Z = Z voulu (la
// commande de marche vise a la hauteur du corps, donc seule cette ecriture suit une pente).
// =====================================================================================

#include <algorithm>
#include <cmath>

namespace Tessera::Sync {

struct PositionSuivi
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

/// Position a ecrire cette image. `lue` = position du corps, `voulue` = cible.
/// Si l'ecart horizontal depasse `ecartMaxM`, on revient EXACTEMENT a ce plafond (jamais au-dela
/// de la cible), sinon on prend `fraction` de l'ecart. NaN/inf -> `lue` inchangee.
inline PositionSuivi PasSuiviDirect(const PositionSuivi& lue, const PositionSuivi& voulue,
                                    float fraction, float ecartMaxM = 1.5f)
{
    const float dx = voulue.x - lue.x;
    const float dy = voulue.y - lue.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(d) || !std::isfinite(voulue.z) || !std::isfinite(lue.z))
    {
        return lue;
    }
    // Part de l'ecart a parcourir : la fraction, ou plus s'il faut rentrer sous le plafond.
    float part = fraction;
    if (d > ecartMaxM)
    {
        part = std::max(fraction, (d - ecartMaxM) / d);
    }
    return {lue.x + dx * part, lue.y + dy * part, voulue.z};
}

/// Detecte une commande de marche qui n'a jamais demarre (corps immobile alors qu'un deplacement
/// est commande). `Avancer` rend vrai UNE fois apres `kImmobileS` sous `kVitesseMiniMs`, puis se
/// rearme (au plus une reemission par `kImmobileS`).
struct GardeMarchePlantee
{
    static constexpr float kVitesseMiniMs = 0.5f;
    static constexpr float kImmobileS = 0.25f;
    float immobileS = 0.0f;

    bool Avancer(float dt, bool enMouvementCommande, float vitesseCorpsMs)
    {
        if (!enMouvementCommande || !std::isfinite(vitesseCorpsMs) || vitesseCorpsMs >= kVitesseMiniMs)
        {
            immobileS = 0.0f;
            return false;
        }
        immobileS += dt;
        if (immobileS >= kImmobileS)
        {
            immobileS = 0.0f;
            return true;
        }
        return false;
    }
};

} // namespace Tessera::Sync

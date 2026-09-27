#pragma once
// =====================================================================================
// Est-ce bien LE MEME PNJ ? — avant d'appliquer une apparence relayee par le halo.
//
// ⛔ RETOURS DU PLAYTEST 2 (2026-09-26, R9/R10 : des PNJ decoratifs « se dechirent »). Le relais
// d'apparences suppose qu'un `EntityID` de statique designe le meme PNJ sur toutes les machines
// (F-PNJ-128). Le journal client du 2026-09-25 le dement au moins parfois : 11 verdicts `AUTRE`
// ou un corps d'HOMME recevait une apparence de FEMME (`citizen__corporat_ma` <-
// `queen_of_the_stoop_wa_07`). On rhabillait donc un PNJ avec la fiche d'un autre.
//
// Le halo porte deja de quoi verifier : le record et la position du PNJ tels que le rapporteur
// les a vus. On n'applique que si le PNJ local a le MEME record et se tient AU MEME ENDROIT.
//
// Arithmetique pure, sans type moteur : verifiee par `tests/verif_identite_statique.cpp`.
// =====================================================================================

#include <cstdint>

namespace Tessera::Sync
{
enum class IdentiteStatique
{
    Meme,          // meme record, meme endroit : on peut appliquer
    AutreRecord,   // un autre personnage porte cet identifiant ici
    AutreEndroit,  // meme record, mais pas au meme endroit : un autre exemplaire
    SansReference, // le halo ne nous a rien dit de ce PNJ : on ne peut rien verifier
};

/// Tolerance de position. Un statique ne se promene pas, mais il se tourne, s'assied, change de
/// pied : 3 m couvrent un PNJ qui bouge sur place sans confondre deux voisins de trottoir
/// (ceux-ci sont rarement a moins de 3 m l'un de l'autre ET du meme record).
constexpr float kToleranceIdentiteM = 3.0f;

/// `recordRecu == 0` = le rapporteur ne l'a pas donne : seule la position tranche alors.
inline IdentiteStatique JugerIdentiteStatique(bool referenceConnue, std::uint64_t recordRecu,
                                             std::uint64_t recordLocal, float dx, float dy,
                                             float dz)
{
    if (!referenceConnue)
    {
        return IdentiteStatique::SansReference;
    }
    if (recordRecu != 0 && recordRecu != recordLocal)
    {
        return IdentiteStatique::AutreRecord;
    }
    const float d2 = dx * dx + dy * dy + dz * dz;
    // `!(d2 <= ...)` et non `d2 > ...` : un NaN (position illisible) doit REFUSER.
    if (!(d2 <= kToleranceIdentiteM * kToleranceIdentiteM))
    {
        return IdentiteStatique::AutreEndroit;
    }
    return IdentiteStatique::Meme;
}
} // namespace Tessera::Sync

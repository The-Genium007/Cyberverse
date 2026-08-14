#pragma once

// ─────────────────────────────────────────────────────────────────────────────────────────────
// JOUEUR ROBOT — le scénario reproductible qui rend la mesure comparable d'une session à l'autre
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// POURQUOI. Sans lui, chaque mesure porte sur un geste différent : Lucas ne marche jamais deux fois
// exactement pareil, et deux sessions ne se comparent donc pas. « La latence est passée de 400 à
// 250 ms » ne veut rien dire si la deuxième session comportait moins de démarrages que la première.
//
// C'est la pièce qui rend l'autonomie possible — plus encore que la télémétrie. La télémétrie donne
// des chiffres ; le robot fait qu'ils veulent dire quelque chose.
//
// ── CE QUE LE SCÉNARIO EXERCE, ET POURQUOI CES GESTES-LÀ ──────────────────────────────────────
//
// Chaque phase correspond à un symptôme rapporté, pour qu'un seul passage les couvre tous :
//
//   1. immobile          — l'avatar doit rester INERTE (le PNJ ne doit pas « reprendre ses droits »)
//   2. marche avant      — le régime nominal
//   3. ARRÊT NET         — le cas qui a révélé le gel d'une seconde : s'arrêter puis repartir
//   4. redémarrage       — « une à deux secondes avant que l'animation se lance »
//   5. course            — l'allure, et la dérive qui se creuse quand elle est mal commandée
//   6. pas de côté       — cap FIXE, déplacement latéral (F-PNJ-152)
//   7. marche arrière    — cap fixe, déplacement opposé
//   8. saut              — l'arc vertical, jamais rendu jusqu'ici
//   9. retour            — referme la boucle sur l'origine (voir `kDureeCycleS`)
//
// ── CE QUE LE ROBOT NE PROUVE PAS ─────────────────────────────────────────────────────────────
//
// ponytail: le robot ANNONCE une pose, il ne pilote pas les entrées du jeu. Son propre pantin ne
// joue donc aucune animation locale, et sa vitesse n'est pas produite par le moteur. C'est sans
// effet sur ce qu'on mesure — la latence, la gigue, les téléports et l'allure RENDUE chez l'autre
// client se lisent toutes chez l'OBSERVATEUR — mais ça écarte deux choses : le comportement du
// moteur sous une vraie entrée (collision, pente, escalier) et la fidélité du champ `locomotion`,
// que le robot déclare au lieu de le lire. Injecter de vraies entrées est un autre chantier.

#include <cmath>
#include <cstdint>

namespace Tessera::Sync
{

/// Une pose du scénario, en coordonnées RELATIVES au point de départ.
struct PoseRobot
{
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float yaw = 0.0f;
    std::uint8_t locomotion = 0;
    /// Numéro de phase (1..9), pour que le journal dise quel geste était en cours.
    int phase = 0;
};

/// Durée d'un tour complet, en secondes. Le scénario boucle indéfiniment.
///
/// ⚠️ LE CYCLE DOIT SE REFERMER SUR L'ORIGINE, et c'est la vérification hors-jeu qui l'a imposé.
/// Une première version s'arrêtait après les sauts, à 23,5 m du départ : au bouclage, le robot
/// se téléportait de 23,5 m. Le tampon d'interpolation du client aurait coupé son historique
/// (discontinuité franche), le verdict aurait compté un téléport, et les deux se seraient produits
/// UNE FOIS PAR CYCLE — un artefact périodique de l'instrument, imputé au réseau. D'où la phase 9.
inline constexpr double kDureeCycleS = 40.0;

/// Codes de locomotion, mêmes valeurs que `ReadLocomotionPacked` (redscript).
inline constexpr std::uint8_t kRobotIdle = 0;
inline constexpr std::uint8_t kRobotMarche = 1;
inline constexpr std::uint8_t kRobotCourse = 2;
inline constexpr std::uint8_t kRobotEnLair = 6;

/// La pose du scénario à l'instant `t` (secondes depuis le début de la session).
///
/// Fonction PURE : pas d'état, pas d'horloge, pas de moteur. C'est ce qui permet de la vérifier
/// sans lancer le jeu — et une trajectoire fausse serait indétectable en jeu, puisqu'on n'a rien
/// à quoi la comparer.
[[nodiscard]] inline PoseRobot PoseDuRobot(double t) noexcept
{
    const double c = std::fmod(t < 0.0 ? 0.0 : t, kDureeCycleS);
    PoseRobot p;

    // Le cap reste à 0 pendant TOUT le cycle : c'est ce qui rend les phases 6 et 7 lisibles. Un
    // robot qui tournerait vers sa direction de marche ne testerait jamais le pas de côté.
    p.yaw = 0.0f;

    if (c < 3.0)
    {
        // 1. immobile — 3 s. L'avatar distant doit être parfaitement fixe.
        p.phase = 1;
        p.locomotion = kRobotIdle;
        return p;
    }
    if (c < 9.0)
    {
        // 2. marche avant — 6 s à 1,5 m/s, soit 9 m.
        p.phase = 2;
        p.locomotion = kRobotMarche;
        p.dy = static_cast<float>((c - 3.0) * 1.5);
        return p;
    }
    if (c < 12.0)
    {
        // 3. ARRÊT NET — 3 s. Le cas qui a révélé le gel d'une seconde.
        p.phase = 3;
        p.locomotion = kRobotIdle;
        p.dy = 9.0f;
        return p;
    }
    if (c < 16.0)
    {
        // 4. redémarrage immédiat, marche — 4 s à 1,5 m/s.
        p.phase = 4;
        p.locomotion = kRobotMarche;
        p.dy = static_cast<float>(9.0 + (c - 12.0) * 1.5);
        return p;
    }
    if (c < 20.0)
    {
        // 5. course — 4 s à 4 m/s. L'allure la plus exigeante pour le recalage.
        p.phase = 5;
        p.locomotion = kRobotCourse;
        p.dy = static_cast<float>(15.0 + (c - 16.0) * 4.0);
        return p;
    }
    if (c < 26.0)
    {
        // 6. pas de côté — 6 s, 3 m à droite puis 3 m à gauche, CAP INCHANGÉ.
        p.phase = 6;
        p.locomotion = kRobotMarche;
        p.dy = 31.0f;
        const double u = c - 20.0;
        p.dx = static_cast<float>(u < 3.0 ? u * 1.0 : (6.0 - u) * 1.0);
        return p;
    }
    if (c < 31.0)
    {
        // 7. marche arrière — 5 s à 1,5 m/s, CAP INCHANGÉ : on recule vraiment.
        p.phase = 7;
        p.locomotion = kRobotMarche;
        p.dy = static_cast<float>(31.0 - (c - 26.0) * 1.5);
        return p;
    }

    if (c < 34.0)
    {
        // 8. sauts — 3 s, trois arcs d'une seconde. `dz` décrit une parabole ; `locomotion`
        // passe à `kRobotEnLair`, seul signal qui décrive un saut sur le fil.
        p.phase = 8;
        const double u = std::fmod(c - 31.0, 1.0);
        p.dy = 23.5f;
        // 4·h·u·(1−u) : parabole valant 0 aux extrémités et `h` au sommet. h = 1,4 m.
        p.dz = static_cast<float>(4.0 * 1.4 * u * (1.0 - u));
        p.locomotion = p.dz > 0.05f ? kRobotEnLair : kRobotIdle;
        return p;
    }

    // 9. RETOUR À L'ORIGINE — 6 s de course, de 23,5 m à 0. Sans cette phase, le bouclage serait
    // un saut de 23,5 m : voir l'avertissement sur `kDureeCycleS`.
    p.phase = 9;
    p.locomotion = kRobotCourse;
    p.dy = static_cast<float>(23.5 * (1.0 - (c - 34.0) / 6.0));
    return p;
}

} // namespace Tessera::Sync

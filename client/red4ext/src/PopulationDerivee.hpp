// Population dérivée — portage C++ de l'implémentation de référence Rust.
//
// Référence : tessera-core/server/src/population_derivee.rs
// Spec      : docs/superpowers/specs/2026-09-20-population-ambiante-deterministe-design.md
// Vecteurs  : tessera-core/server/population-derivee-vecteurs.json
//
// ── POURQUOI CE FICHIER EXISTE, ET POURQUOI IL EST EN-TÊTE SEUL ───────────────────────────────
//
// La dérivation s'exécute dans les hooks du plugin RED4ext, chez chaque joueur. Le serveur Rust en
// porte l'implémentation de référence. Deux implémentations d'une même formule qui divergent d'un
// seul bit produisent deux foules différentes — et le défaut serait indiscernable d'une panne
// réseau, ce qui est exactement le mode d'erreur le plus coûteux de ce chantier.
//
// D'où ce fichier : une seule unité de traduction, aucune dépendance, et un test hors jeu
// (test_vecteurs.cpp) qui le confronte au MÊME fichier de vecteurs que le test Rust. Si les deux
// tests passent, les deux implémentations s'accordent — vérifié sans lancer le jeu.
//
// ⚠️ TOUTE MODIFICATION ICI DOIT ÊTRE FAITE EN MIROIR DANS LE MODULE RUST, et le test de vecteurs
// doit rougir entre les deux. S'il ne rougit pas, c'est le test qui est cassé, pas le code qui est
// juste.

#pragma once

#include <cstdint>
#include <cstring>

namespace tessera::population {

// Les usages d'une même clé. Les valeurs sont figées : les changer rebat la ville entière.
// Miroir de `Selecteur` côté Rust — MÊMES VALEURS, obligatoirement.
enum class Selecteur : std::uint32_t {
    Abscisse       = 1,
    Archetype      = 2,
    Apparence      = 3,
    Vitesse        = 4,
    Chemin         = 5,
    Trajet         = 6,  // le n-ieme prolongement (140408398 / 1405fd0d8 -> 1405fdbe4)
    PresenceGaree  = 7,  // porte de Bernoulli des voitures garees (14083b614)
    DelaiConduiteA = 8,  // 14088a908 x2
    DelaiConduiteB = 9,
    Conduite       = 10, // 14088d44c
};

// FNV-1a 64 — le hachage de CName et de ResourcePath dans Cyberpunk 2077.
// ⚠️ Le nombre premier s'écrit sur 16 chiffres hexadécimaux : 0x00000100000001b3. Groupé sur 11,
// il devient 0x1000000001b3, un premier FNV qui n'existe pas. L'erreur a déjà été commise côté
// Rust et attrapée par les vecteurs canoniques.
inline std::uint64_t fnv1a64(const std::uint8_t* octets, std::size_t taille) {
    constexpr std::uint64_t BASE   = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t PREMIER = 0x00000100000001b3ULL;
    std::uint64_t h = BASE;
    for (std::size_t i = 0; i < taille; ++i) {
        h ^= static_cast<std::uint64_t>(octets[i]);
        h *= PREMIER; // le débordement est voulu — c'est `wrapping_mul` côté Rust
    }
    return h;
}

// splitmix64. Décorrèle des entrées très structurées (indices consécutifs, identifiants voisins).
// Sans lui, deux passants de rangs voisins tireraient des valeurs corrélées, et la foule ferait
// des « paquets » de personnages identiques.
inline std::uint64_t melanger(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// La clé d'un créneau : l'endroit et le moment où des passants naissent.
// ⚠️ La quantification de l'abscisse est reprise du moteur (`(s0 + s1) * 0.1` tronqué) : deux
// clients qui liraient des flottants infinitésimalement différents doivent obtenir la MÊME clé.
inline std::uint32_t cle_creneau(std::uint64_t zone_id, std::uint16_t sous_a, std::uint16_t sous_b,
                                 std::uint8_t sous_c, float s0, float s1) {
    const std::int64_t milieu = static_cast<std::int64_t>((s0 + s1) * 0.1f);
    std::uint8_t octets[8 + 2 + 2 + 1 + 8];
    std::memcpy(octets + 0, &zone_id, 8);   // little-endian : le plugin ne cible que x86-64
    std::memcpy(octets + 8, &sous_a, 2);
    std::memcpy(octets + 10, &sous_b, 2);
    octets[12] = sous_c;
    std::memcpy(octets + 13, &milieu, 8);
    return static_cast<std::uint32_t>(fnv1a64(octets, sizeof octets) >> 32);
}

// La clé d'un individu — identique sur toutes les machines.
inline std::uint64_t cle_individu(std::uint64_t graine_serveur, std::uint32_t cle_du_creneau,
                                  std::uint32_t rang) {
    std::uint8_t octets[8];
    std::memcpy(octets + 0, &cle_du_creneau, 4);
    std::memcpy(octets + 4, &rang, 4);
    return melanger(fnv1a64(octets, sizeof octets) ^ graine_serveur);
}

// Un flottant dans [0, 1) pour l'usage désigné.
// ⚠️ La conversion reproduit celle du moteur : 23 bits de mantisse, via (bits | 0x3F800000) − 1.0.
// Une conversion « plus fine » donnerait des valeurs voisines mais pas égales — et une valeur
// voisine suffit à faire basculer un tirage pondéré d'une entrée à l'autre près d'une borne.
inline float fraction(std::uint64_t cle, Selecteur selecteur) {
    const std::uint64_t bits =
        melanger(cle ^ (static_cast<std::uint64_t>(selecteur) * 0x9E3779B97F4A7C15ULL));
    const std::uint32_t mantisse = static_cast<std::uint32_t>(bits >> 32) & 0x007FFFFFu;
    const std::uint32_t motif = mantisse | 0x3F800000u;
    float f;
    std::memcpy(&f, &motif, 4); // pas de type-punning par union ni par cast : c'est le seul moyen
    return f - 1.0f;            // portable, et le compilateur l'élimine
}

// La roue de loterie pondérée du moteur, reproduite à l'identique (voir `1404062b8`).
//
// ⚠️ DEUX COMPORTEMENTS QU'ON SERAIT TENTÉ DE « CORRIGER », ET QU'IL NE FAUT PAS TOUCHER :
//   1. aucune normalisation — les poids sont soustraits tels quels ;
//   2. repli sur la DERNIÈRE entrée quand f n'a pas été consommé.
// Les entrées de poids nul ne sortent jamais, ce qui est correct et concerne des milliers d'entrées du
// corpus (6 745 sur les deux viviers, F-PNJ-194). Ne pas les filtrer en amont : les retirer décalerait les indices.
//
// Rend -1 si la liste est vide.
inline int tirage_pondere(const float* poids, std::size_t nombre, std::uint64_t cle,
                          Selecteur selecteur) {
    if (nombre == 0) {
        return -1;
    }
    float f = fraction(cle, selecteur);
    const std::size_t dernier = nombre - 1;
    for (std::size_t i = 0; i < dernier; ++i) {
        if (f < poids[i]) {
            return static_cast<int>(i);
        }
        f -= poids[i];
    }
    return static_cast<int>(dernier);
}

// Un index uniforme dans [0, n) — le tirage d'apparence du moteur.
// ⚠️ La liste doit être ordonnée de la même façon des deux côtés (trier par nom).
inline int index_uniforme(std::size_t n, std::uint64_t cle, Selecteur selecteur) {
    if (n == 0) {
        return -1;
    }
    const std::uint64_t bits =
        melanger(cle ^ (static_cast<std::uint64_t>(selecteur) * 0xD6E8FEB86659FD93ULL));
    return static_cast<int>(bits % static_cast<std::uint64_t>(n));
}

// L'abscisse de naissance, dans la fenêtre [s0, s1] du créneau.
// ⚠️ Cette valeur s'écrit dans la COPIE DE PILE du créneau, jamais dans l'entrée vivante : une
// fenêtre nulle dans l'entrée vivante met la priorité du créneau à zéro, et il cesse de faire
// naître qui que ce soit — sans un mot.
inline float abscisse_naissance(float s0, float s1, std::uint64_t cle) {
    return s0 + (s1 - s0) * fraction(cle, Selecteur::Abscisse);
}

// La vitesse individuelle, entre les bornes du record `Crowds.DefaultMovement`.
inline float vitesse(float min, float max, std::uint64_t cle) {
    return min + (max - min) * fraction(cle, Selecteur::Vitesse);
}

// L'abscisse de référence à un instant donné — le cœur du rappel.
// ⚠️ `dt_ms` se compte sur l'horloge MURALE du serveur (`Snapshot.ts_ms`), jamais sur l'heure de
// jeu : celle-ci ne traverse qu'à la minute, avec 3 minutes de bande morte.
inline float abscisse_a_instant(float s0, float vitesse_m_s, std::int64_t dt_ms) {
    return s0 + vitesse_m_s * (static_cast<float>(dt_ms) / 1000.0f);
}

// ── Lots 3 a 6 (2026-09-29) — miroir de la section du meme nom cote Rust ─────────────────────

// La cle d'un STATIQUE : son EntityID est deja commun aux machines (F-PNJ-128).
inline std::uint64_t cle_statique(std::uint64_t graine_serveur, std::uint64_t entity_id) {
    std::uint8_t octets[8];
    std::memcpy(octets, &entity_id, 8);
    return melanger(fnv1a64(octets, sizeof octets) ^ graine_serveur);
}

constexpr std::uint64_t TESSERA_CYCLE_GAREE_MS = 30ULL * 60ULL * 1000ULL;

inline std::uint64_t cycle_garee(std::uint64_t t_ms) {
    return t_ms / TESSERA_CYCLE_GAREE_MS;
}

// La cle d'une PLACE de parking pour un cycle (parkingSpaceId du noeud de secteur, F-VEH-060).
inline std::uint64_t cle_place_garee(std::uint64_t graine_serveur, std::uint64_t parking_space_id,
                                     std::uint64_t cycle) {
    std::uint8_t octets[16];
    std::memcpy(octets + 0, &parking_space_id, 8);
    std::memcpy(octets + 8, &cycle, 8);
    return melanger(fnv1a64(octets, sizeof octets) ^ graine_serveur);
}

// Rampe d'effectif de la porte des voitures garees (14083b614) : clamp((50 - effectif) * 0.04, 0, 1).
inline float rampe_garee(std::uint32_t effectif_local) {
    const float v = (50.0f - static_cast<float>(effectif_local)) * 0.04f;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Porte de Bernoulli, comparaison INCLUSIVE : f <= densite * rampe.
inline bool presence_garee(std::uint64_t graine_serveur, std::uint64_t parking_space_id, std::uint64_t t_ms,
                           float densite, std::uint32_t effectif_local) {
    const std::uint64_t cle = cle_place_garee(graine_serveur, parking_space_id, cycle_garee(t_ms));
    return fraction(cle, Selecteur::PresenceGaree) <= densite * rampe_garee(effectif_local);
}

// La cle du n-ieme prolongement du trajet (tous les 70 m, F-PNJ-211). `n + 1` : le
// prolongement 0 doit deja differer de la cle nue.
inline std::uint64_t cle_prolongement(std::uint64_t cle, std::uint32_t n) {
    return melanger(cle ^ ((static_cast<std::uint64_t>(n) + 1ULL) * 0xA0761D6478BD642FULL));
}

// La SECONDE roue du moteur, 1405fdbe4, celle qui NORMALISE (F-PNJ-229) :
//   total = somme des scores (de gauche a droite) ; u = fraction * total ;
//   cumul += score_i ; si u <= cumul (INCLUSIF) : i.
// Les scores viennent d'un foncteur du descripteur de requete, pas du fichier.
// Rend -1 si la liste est vide.
inline int tirage_pondere_normalise(const float* scores, std::size_t nombre, std::uint64_t cle,
                                    Selecteur selecteur) {
    if (nombre == 0) {
        return -1;
    }
    float total = 0.0f;
    for (std::size_t i = 0; i < nombre; ++i) {
        total += scores[i];
    }
    const float u = fraction(cle, selecteur) * total;
    float cumul = 0.0f;
    for (std::size_t i = 0; i < nombre; ++i) {
        cumul += scores[i];
        if (u <= cumul) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(nombre - 1);
}

// Un EMETTEUR : un troncon de foule qui fait naitre des marcheurs a un rythme derive de sa
// densite — la « place » que le jeu ne porte pas, fabriquee comme une fonction du temps.
// `n_attendu` : l'effectif demande par la donnee pour ce troncon a cette phase.
struct Emetteur {
    std::uint64_t voie;
    float x1;
    float x2;
    float n_attendu;
    float v_min;
    float v_max;
};

constexpr std::uint64_t TESSERA_PERIODE_MIN_MS = 200;
constexpr std::uint64_t TESSERA_CANDIDATS_MAX = 4096;

struct Individu {
    std::uint64_t k;
    std::uint64_t cle;
    std::uint64_t naissance_ms;
    float vitesse_m_s;
};

// Rend false si le troncon n'emet jamais.
inline bool periode_ms(const Emetteur& e, std::uint64_t& sortie) {
    const float longueur = e.x2 - e.x1;
    const float v_moy = (e.v_min + e.v_max) * 0.5f;
    if (!(longueur > 0.0f) || !(e.n_attendu > 0.0f) || !(e.v_min > 0.0f) || !(v_moy > 0.0f)) {
        return false;
    }
    const float secondes = longueur / (v_moy * e.n_attendu);
    float ms = secondes * 1000.0f;
    if (ms > 1.0e15f) {
        ms = 1.0e15f; // borne AVANT la conversion : static_cast hors plage est indefini
    }
    const std::uint64_t brut = static_cast<std::uint64_t>(ms);
    sortie = brut < TESSERA_PERIODE_MIN_MS ? TESSERA_PERIODE_MIN_MS : brut;
    return true;
}

inline std::uint64_t cle_emetteur(std::uint64_t graine_serveur, std::uint64_t voie, float x1,
                                  std::uint64_t k) {
    const std::int64_t x1_q = static_cast<std::int64_t>(x1 * 10.0f); // dixieme de metre, tronque
    std::uint8_t octets[24];
    std::memcpy(octets + 0, &voie, 8);
    std::memcpy(octets + 8, &x1_q, 8);
    std::memcpy(octets + 16, &k, 8);
    return melanger(fnv1a64(octets, sizeof octets) ^ graine_serveur);
}

inline Individu individu(const Emetteur& e, std::uint64_t graine_serveur, std::uint64_t periode,
                         std::uint64_t k) {
    Individu i;
    i.k = k;
    i.cle = cle_emetteur(graine_serveur, e.voie, e.x1, k);
    // Saturant, comme `saturating_mul` cote Rust.
    i.naissance_ms = (periode != 0 && k > (~0ULL) / periode) ? ~0ULL : k * periode;
    i.vitesse_m_s = vitesse(e.v_min, e.v_max, i.cle);
    return i;
}

inline float abscisse_reference(const Emetteur& e, const Individu& i, std::uint64_t t_ms) {
    std::uint64_t dt = t_ms > i.naissance_ms ? t_ms - i.naissance_ms : 0;
    if (dt > 0x7fffffffffffffffULL) {
        dt = 0x7fffffffffffffffULL;
    }
    return abscisse_a_instant(e.x1, i.vitesse_m_s, static_cast<std::int64_t>(dt));
}

// Ce que le hook A' ecrit quand le moteur veut faire naitre dans [s0, s1] : le plus ancien
// candidat que ce client n'a pas encore fait naitre. Rend false si aucun (le moteur tire
// nativement, l'individu ne est « hors cle »).
inline bool choisir(const Emetteur& e, std::uint64_t graine_serveur, std::uint64_t t_ms, float s0,
                    float s1, const std::uint64_t* deja_vivants, std::size_t nb_vivants,
                    Individu& choix, float& abscisse) {
    std::uint64_t p = 0;
    if (!periode_ms(e, p)) {
        return false;
    }
    const std::uint64_t k_max = t_ms / p;
    float d = s1 - e.x1;
    if (!(d > 0.0f)) {
        d = 0.0f;
    }
    float duree = (d / e.v_min) * 1000.0f;
    if (duree > 1.0e15f) {
        duree = 1.0e15f;
    }
    const std::uint64_t duree_max_ms = static_cast<std::uint64_t>(duree);
    std::uint64_t k_min = (t_ms > duree_max_ms ? t_ms - duree_max_ms : 0) / p;
    const std::uint64_t plancher = k_max > TESSERA_CANDIDATS_MAX ? k_max - TESSERA_CANDIDATS_MAX : 0;
    if (k_min < plancher) {
        k_min = plancher;
    }
    for (std::uint64_t k = k_min; k <= k_max; ++k) {
        const Individu i = individu(e, graine_serveur, p, k);
        const float s = abscisse_reference(e, i, t_ms);
        if (!(s0 <= s && s <= s1)) {
            continue;
        }
        bool vivant = false;
        for (std::size_t j = 0; j < nb_vivants; ++j) {
            if (deja_vivants[j] == i.cle) {
                vivant = true;
                break;
            }
        }
        if (vivant) {
            continue;
        }
        choix = i;
        abscisse = s;
        return true;
    }
    return false;
}


// ── Encodage de la pose, pour la porte de teleportation du moteur ─────────────────────────────
//
// Le moteur stocke la position d'un stub en POINT FIXE : 1 unite = 2^-17 m (~7,6 um). C'est la
// meme convention que `AISpotPersistentData.worldPosition` (Bits/131072), ce qui est un
// recoupement, pas une coincidence.
//
// ⚠️ C'est la conversion ou une erreur est SILENCIEUSE et spectaculaire : un facteur faux ne
// provoque aucune erreur, il repose le passant a des kilometres, ou le deplace de quelques
// microns. D'ou une fonction nommee et testee plutot qu'un `* 131072.f` dissemine.
constexpr float TESSERA_POINT_FIXE_PAR_METRE = 131072.0f; // 2^17

inline std::int32_t metres_vers_point_fixe(float metres) {
    return static_cast<std::int32_t>(metres * TESSERA_POINT_FIXE_PAR_METRE);
}

inline float point_fixe_vers_metres(std::int32_t unites) {
    return static_cast<float>(unites) / TESSERA_POINT_FIXE_PAR_METRE;
}

// Le seuil sous lequel la porte de teleportation SAUTE la re-indexation spatiale du stub
// (garde a 0,01 m² sur la distance², soit 0,1 m). Corriger en dessous deplace le pantin en
// laissant l'index perime : un demi-effet, et de ceux qu'on ne diagnostique qu'apres des heures.
constexpr float TESSERA_PLANCHER_REINDEXATION_M = 0.1f;

} // namespace tessera::population

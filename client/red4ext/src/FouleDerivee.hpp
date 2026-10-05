// Foule derivee — la LOGIQUE PURE des hooks A' a H du monde partage (Tessera, lots 3 a 6).
//
// Aucun type moteur, aucun <windows.h> : tout ce fichier se compile et se teste HORS JEU
// (`tests/verif_foule_derivee.cpp`). Les detours (`TesseraFouleDerivee.cpp`) ne font que lire
// des arguments, appeler ces fonctions, et ecrire le resultat.
//
// Spec : Tessera docs/superpowers/specs/2026-09-29-monde-partage-determinisme-lots-3-6-design.md
// Faits : F-PNJ-177 (PCG par thread), 178, 187, 188, 189, 202, 205, 211, 220, 221, 229, 231,
//         F-PNJ-250 a 259 (ce lot).
//
// ⚠️ L'EFFET EN JEU DE TOUT CECI EST NON MESURE (2026-10-05) : compile, teste hors jeu, jamais
// charge dans le jeu. Voir C:\tw\_etat\campagne-jeu.md, bloc 4 ter.

#pragma once

#include "PopulationDerivee.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tessera::foule {

using tessera::population::Selecteur;

// ── Le generateur du moteur ────────────────────────────────────────────────────────────────
//
// Etat 64 bits par thread (F-PNJ-177), avance `s * MULT + 1`. La SORTIE, relue au desassemblage
// de `1405fdc9b..1405fdcc8` le 2026-10-05, n'est PAS la rotation canonique de PCG-XSH-RR :
//   x = (u32)(s >> 45) ^ (u32)(s >> 27) ; r = s >> 59 ; sortie = (x >> r) | (x << (31 - r))
// — un decalage a gauche de `31 - r`, pas de `32 - r` (F-PNJ-251). Sans consequence pour la
// derivation (elle n'utilise pas ce generateur), decisive pour IMPOSER un tirage.
constexpr std::uint64_t PCG_MULT = 0x5851f42d4c957f2dULL;

inline std::uint64_t pcg_suivant(std::uint64_t s) { return s * PCG_MULT + 1ULL; }

inline std::uint32_t pcg_sortie(std::uint64_t s) {
    const std::uint32_t x = static_cast<std::uint32_t>(s >> 45) ^ static_cast<std::uint32_t>(s >> 27);
    const std::uint32_t r = static_cast<std::uint32_t>(s >> 59);
    return (x >> r) | (x << (31u - r));
}

// Le flottant [0,1) que le moteur tire de l'etat `s` (meme construction que `fraction`).
inline float pcg_fraction(std::uint64_t s) {
    const std::uint32_t motif = (pcg_sortie(s) & 0x007FFFFFu) | 0x3F800000u;
    float f;
    std::memcpy(&f, &motif, 4);
    return f - 1.0f;
}

// L'etat qui fait SORTIR `voulu` (< 2^31) au prochain tirage du moteur.
//
// C'est le levier des tirages INLINE (A''', C, H, G) : la fonction du moteur tire au milieu de
// son corps, on ne peut ni remplacer sa valeur de retour ni reecrire un champ apres coup. On pose
// donc l'etat du generateur juste avant de l'appeler : le moteur fait LUI-MEME son calcul, avec
// notre nombre. Rien a recopier de sa formule, donc rien a recopier de travers.
//
// Construction : r = 31 (bits 59..63 a 1) → sortie = x | (x >> 31) = x pour x < 2^31. Puis
// x[i] = s[45+i] ^ s[27+i] (s[45+i] = 0 au-dela du bit 63) se resout du bit haut vers le bas.
// Les 27 bits bas sont libres : on y garde ceux de l'etat d'origine, pour que la suite du flux
// reste dispersee.
inline std::uint64_t pcg_etat_imposant(std::uint32_t voulu, std::uint64_t etat_origine) {
    std::uint64_t s = (0x1FULL << 59) | (etat_origine & ((1ULL << 27) - 1ULL));
    for (int i = 31; i >= 0; --i) {
        const std::uint64_t haut = (45 + i <= 63) ? ((s >> (45 + i)) & 1ULL) : 0ULL;
        const std::uint64_t bit = ((voulu >> i) & 1u) ^ haut;
        if (27 + i <= 58) {
            s |= bit << (27 + i);
        }
        // i >= 32 n'existe pas ; 27 + 31 = 58 : jamais dans les bits de rotation.
    }
    return s;
}

// La mantisse (23 bits) d'une fraction rendue par `population::fraction` — ce qu'il faut imposer
// pour que le moteur relise EXACTEMENT cette fraction. `f + 1.0f` est exact : f = m / 2^23.
inline std::uint32_t mantisse_de(float f) {
    const float g = f + 1.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &g, 4);
    return bits & 0x007FFFFFu;
}

// ── Interrupteurs ──────────────────────────────────────────────────────────────────────────
struct Interrupteurs {
    bool derivee = false;          // TESSERA_FOULE_DERIVEE=1 : actif sans MondePartage.mode == 1
    char controle = 0;             // TESSERA_FOULE_CONTROLE=<A|B|C|D|E|P|H|G> : valeur ABSURDE
    bool score_pur = false;        // TESSERA_FOULE_SCORE_PUR=1 : +INF dans posA du score pieton
    bool emetteur_creneau = false; // TESSERA_FOULE_EMETTEUR=creneau : emetteur = le creneau (repli H2)
    bool journal = false;          // TESSERA_FOULE_JOURNAL=1, ou implicite des que l'un des autres
};

inline Interrupteurs lire_interrupteurs(const char* derivee, const char* controle,
                                        const char* score_pur, const char* emetteur,
                                        const char* journal) {
    Interrupteurs i;
    i.derivee = derivee != nullptr && derivee[0] == '1';
    if (controle != nullptr && controle[0] != '\0') {
        const char c = static_cast<char>(controle[0] & ~0x20); // majuscule ASCII
        if (c == 'A' || c == 'B' || c == 'C' || c == 'D' || c == 'E' || c == 'P' || c == 'H' ||
            c == 'G') {
            i.controle = c;
        }
    }
    i.score_pur = score_pur != nullptr && score_pur[0] == '1';
    i.emetteur_creneau = emetteur != nullptr && std::strcmp(emetteur, "creneau") == 0;
    i.journal = (journal != nullptr && journal[0] == '1') || i.derivee || i.controle != 0 ||
                i.score_pur;
    return i;
}

// ── La table membre → cle ──────────────────────────────────────────────────────────────────
//
// Remplie a la naissance (sortie de `14041dd50`), videe au despawn (`140887bb8`, F-PNJ-225 : les
// identifiants ET les adresses sont recycles). Bornee a 4096 : au-dela, la plus ancienne sort.
struct Fiche {
    std::uint64_t cle = 0;
    std::uint64_t emetteur = 0;      // identite de l'emetteur, pour « deja vivants »
    std::uint32_t prolongements = 0; // tirages de trajet deja faits (par TENTATIVE, F-PNJ-231)
    std::uint8_t famille = 0;        // 1 pieton, 2 vehicule
    std::uint64_t ordre = 0;
};

class TableMembres {
public:
    static constexpr std::size_t CAPACITE = 4096;

    void poser(std::uint64_t membre, const Fiche& f) {
        std::lock_guard<std::mutex> g(m_);
        if (t_.size() >= CAPACITE && t_.find(membre) == t_.end()) {
            // ponytail: balayage lineaire a l'eviction (4096 entrees, seulement quand la table
            // deborde — donc quand le hook de despawn a manque des sorties). Liste chainee si ca
            // se voit au profileur.
            auto vieux = t_.begin();
            for (auto it = t_.begin(); it != t_.end(); ++it) {
                if (it->second.ordre < vieux->second.ordre) {
                    vieux = it;
                }
            }
            t_.erase(vieux);
            ++evictions_;
        }
        Fiche copie = f;
        copie.ordre = ++ordre_;
        t_[membre] = copie;
    }

    bool trouver(std::uint64_t membre, Fiche& sortie) const {
        std::lock_guard<std::mutex> g(m_);
        const auto it = t_.find(membre);
        if (it == t_.end()) {
            return false;
        }
        sortie = it->second;
        return true;
    }

    // Rend le numero de la tentative (0, 1, 2…) et l'incremente. false si le membre est inconnu.
    bool tentative_suivante(std::uint64_t membre, std::uint64_t& cle, std::uint32_t& n) {
        std::lock_guard<std::mutex> g(m_);
        const auto it = t_.find(membre);
        if (it == t_.end()) {
            return false;
        }
        cle = it->second.cle;
        n = it->second.prolongements++;
        return true;
    }

    bool retirer(std::uint64_t membre) {
        std::lock_guard<std::mutex> g(m_);
        return t_.erase(membre) != 0;
    }

    void cles_de_l_emetteur(std::uint64_t emetteur, std::vector<std::uint64_t>& sortie) const {
        std::lock_guard<std::mutex> g(m_);
        for (const auto& [membre, f] : t_) {
            (void)membre;
            if (f.emetteur == emetteur) {
                sortie.push_back(f.cle);
            }
        }
    }

    std::size_t taille() const {
        std::lock_guard<std::mutex> g(m_);
        return t_.size();
    }
    std::uint64_t evictions() const {
        std::lock_guard<std::mutex> g(m_);
        return evictions_;
    }
    void vider() {
        std::lock_guard<std::mutex> g(m_);
        t_.clear();
    }

private:
    mutable std::mutex m_;
    std::unordered_map<std::uint64_t, Fiche> t_;
    std::uint64_t ordre_ = 0;
    std::uint64_t evictions_ = 0;
};

// ── Les decisions, une par hook ────────────────────────────────────────────────────────────

// B — archetype / modele. `poids` : les flottants en `+8` des entrees de 12 octets, dans l'ordre.
inline int decider_archetype(const float* poids, std::size_t n, std::uint64_t cle, char controle) {
    if (n == 0) {
        return -1;
    }
    if (controle == 'B') {
        return 0;
    }
    return tessera::population::tirage_pondere(poids, n, cle, Selecteur::Archetype);
}

// C — apparence. Le tirage du moteur est `sortie % N` au milieu de `1405e7974`, avec un N qui
// depend de la branche prise (trois listes) : N n'est pas connu AVANT l'appel. On impose donc une
// sortie `bits % PPCM(1..22)` : pour tout N <= 22, `sortie % N == bits % N`, c'est-a-dire
// EXACTEMENT `index_uniforme(N, cle, Apparence)`. Au-dela de 22 apparences, l'indice reste
// deterministe et commun aux clients, mais n'est plus celui de la reference Rust (F-PNJ-254).
constexpr std::uint64_t PPCM_1_A_22 = 232792560ULL;

inline std::uint32_t sortie_apparence(std::uint64_t cle, char controle) {
    if (controle == 'C') {
        return 0;
    }
    // miroir de population::index_uniforme : MEME melange, MEME constante.
    const std::uint64_t bits = tessera::population::melanger(
        cle ^ (static_cast<std::uint64_t>(Selecteur::Apparence) * 0xD6E8FEB86659FD93ULL));
    return static_cast<std::uint32_t>(bits % PPCM_1_A_22);
}

// D — vitesse individuelle. Controle : la borne haute.
inline float decider_vitesse(float min, float max, std::uint64_t cle, char controle) {
    if (controle == 'D') {
        return max;
    }
    return tessera::population::vitesse(min, max, cle);
}

// E — voie suivante du trajet, par TENTATIVE (F-PNJ-231).
inline int decider_trajet(const float* scores, std::size_t n, std::uint64_t cle,
                          std::uint32_t tentative, char controle) {
    if (n == 0) {
        return -1;
    }
    if (controle == 'E') {
        return 0;
    }
    return tessera::population::tirage_pondere_normalise(
        scores, n, tessera::population::cle_prolongement(cle, tentative), Selecteur::Trajet);
}

// A''' — la fraction de presence d'une voiture garee pour le cycle courant. Le moteur la compare
// lui-meme a `densite × rampe` (`f <= seuil`, F-PNJ-210) : on ne recopie pas le seuil.
inline float fraction_garee(std::uint64_t graine, std::uint64_t place, std::uint64_t t_ms,
                            char controle) {
    if (controle == 'P') {
        return 0.0f; // 0 <= tout seuil : presente des que la porte est atteinte
    }
    const std::uint64_t cle = tessera::population::cle_place_garee(
        graine, place, tessera::population::cycle_garee(t_ms));
    return tessera::population::fraction(cle, Selecteur::PresenceGaree);
}

// H — « ce vehicule reevalue-t-il son creneau » : la meme fraction pendant tout un seau de 100 ms
// d'horloge SERVEUR. Le moteur fait `f * 0.9 < 0.3` lui-meme.
inline float fraction_reevaluation(std::uint64_t cle, std::uint64_t t_ms, char controle) {
    if (controle == 'H') {
        return 0.0f;
    }
    const std::uint32_t seau = static_cast<std::uint32_t>(t_ms / 100ULL);
    return tessera::population::fraction(tessera::population::cle_prolongement(cle, seau),
                                         Selecteur::Trajet);
}

// G — les deux delais de conduite (`14088a908`, appelee deux fois de suite).
inline float fraction_delai(std::uint64_t cle, int indice, char controle) {
    if (controle == 'G') {
        return 0.0f;
    }
    return tessera::population::fraction(
        cle, indice == 0 ? Selecteur::DelaiConduiteA : Selecteur::DelaiConduiteB);
}

// L'identite d'une voie pour l'emetteur : les 13 octets utiles de la cle de voie du moteur
// (u64 nodeRefHash F-PNJ-246, deux u16, un u8 — les memes champs que `cle_creneau`).
// ⚠️ A REPORTER COTE RUST quand le serveur lira les tables (aucun miroir aujourd'hui).
inline std::uint64_t identite_voie(std::uint64_t zone, std::uint16_t a, std::uint16_t b,
                                   std::uint8_t c) {
    std::uint8_t o[13];
    std::memcpy(o + 0, &zone, 8);
    std::memcpy(o + 8, &a, 2);
    std::memcpy(o + 10, &b, 2);
    o[12] = c;
    return tessera::population::fnv1a64(o, sizeof o);
}

// Ce que A' lit du creneau (copie de pile de 0x30 octets).
struct Creneau {
    std::uint64_t zone = 0;
    std::uint16_t a = 0, b = 0;
    std::uint8_t c = 0;
    float s0 = 0, s1 = 0, longueur = 0, cible = 0;
    std::uint32_t effectif = 0;
};

inline Creneau lire_creneau(const std::uint8_t* p) {
    Creneau k;
    std::memcpy(&k.zone, p + 0x00, 8);
    std::memcpy(&k.a, p + 0x08, 2);
    std::memcpy(&k.b, p + 0x0a, 2);
    k.c = p[0x0c];
    std::memcpy(&k.s0, p + 0x10, 4);
    std::memcpy(&k.s1, p + 0x14, 4);
    std::memcpy(&k.longueur, p + 0x18, 4);
    std::memcpy(&k.cible, p + 0x1c, 4);
    std::memcpy(&k.effectif, p + 0x20, 4);
    return k;
}

// L'emetteur d'un creneau. `x1`/`x2` : le fragment de voie qui contient le creneau (H2), ou le
// creneau lui-meme en mode `emetteur_creneau`. `n` : l'effectif cible du creneau ramene a la
// longueur de l'emetteur (H1 : la densite lineique du creneau vaut celle du fragment).
inline tessera::population::Emetteur emetteur_du_creneau(const Creneau& k, float x1, float x2,
                                                         float v_min, float v_max) {
    tessera::population::Emetteur e;
    e.voie = identite_voie(k.zone, k.a, k.b, k.c);
    e.x1 = x1;
    e.x2 = x2;
    const float fenetre = k.s1 - k.s0;
    e.n_attendu = fenetre > 0.005f ? k.cible * ((x2 - x1) / fenetre) : 0.0f;
    e.v_min = v_min;
    e.v_max = v_max;
    return e;
}

inline std::uint64_t identite_emetteur(const tessera::population::Emetteur& e) {
    return tessera::population::cle_emetteur(0, e.voie, e.x1, 0);
}

struct Naissance {
    bool a_cle = false;
    std::uint64_t cle = 0;
    std::uint64_t emetteur = 0;
    float abscisse = 0;
    float vitesse = 0;
    std::uint64_t k = 0;
};

// A' — qui nait dans ce creneau, et ou. `interdits` : les cles deja vivantes chez ce client
// pour cet emetteur, plus les cles suspendues (promues, lot 5). Sans candidat : « hors cle ».
inline Naissance preparer_naissance(const tessera::population::Emetteur& e, const Creneau& k,
                                    std::uint64_t graine, std::uint64_t t_ms,
                                    const std::vector<std::uint64_t>& interdits) {
    Naissance n;
    n.emetteur = identite_emetteur(e);
    if (t_ms == 0) {
        return n; // pas d'horloge serveur : pas d'instant commun, donc pas de cle
    }
    tessera::population::Individu i{};
    float s = 0;
    if (!tessera::population::choisir(e, graine, t_ms, k.s0, k.s1, interdits.data(),
                                      interdits.size(), i, s)) {
        return n;
    }
    n.a_cle = true;
    n.cle = i.cle;
    n.abscisse = s;
    n.vitesse = i.vitesse_m_s;
    n.k = i.k;
    return n;
}

// ── Le contexte d'un thread ────────────────────────────────────────────────────────────────
//
// Les fonctions de tirage ne recoivent pas l'individu (F-PNJ-187) : un detour d'ENTREE le pose
// ici, les detours de decision le lisent, et la portee le remet en etat a la sortie.
struct Contexte {
    // Naissance en cours (entre l'entree et la sortie de `14041dd50`).
    bool naissance = false;
    bool a_cle = false;
    bool imbrique = false; // une naissance recursive a deja range le membre rendu
    std::uint64_t cle = 0;
    std::uint64_t emetteur = 0;
    std::uint32_t prolongements = 0;
    std::uint8_t famille = 0; // 1 pieton, 2 vehicule
    std::uint64_t mgr = 0;
    Creneau creneau;
    // Portee membre : constructeur d'etat, re-tirage, tick de trajet, porte H.
    std::uint64_t membre = 0;
    bool vehicule = false;  // dans le constructeur ou le re-tirage d'un etat VEHICULE
    bool ctor_base = false; // dans le constructeur d'etat commun (`1405fc0c8`)
    int delai = 0;          // rang du prochain `14088a908`
    // Porte des voitures garees.
    bool garee = false;
    std::uint64_t cle_garee = 0;
};

enum class Source { Aucune, Naissance, Membre, Garee };

// La cle de l'individu que ce thread est en train de traiter. Une naissance en cours fait
// AUTORITE, y compris quand elle est « hors cle » : dans ce cas la reponse est Aucune, jamais la
// cle d'un autre (le membre n'est pas encore dans la table, un vieux `membre` serait un intrus).
inline Source resoudre_cle(const Contexte& ctx, const TableMembres& table, std::uint64_t& cle) {
    if (ctx.naissance) {
        if (ctx.a_cle) {
            cle = ctx.cle;
            return Source::Naissance;
        }
        return Source::Aucune;
    }
    if (ctx.membre != 0) {
        Fiche f;
        if (table.trouver(ctx.membre, f)) {
            cle = f.cle;
            return Source::Membre;
        }
        return Source::Aucune;
    }
    if (ctx.garee) {
        cle = ctx.cle_garee;
        return Source::Garee;
    }
    return Source::Aucune;
}

// Le numero de la prochaine tentative de trajet de l'individu courant (hook E), et sa cle.
inline bool tentative_trajet(Contexte& ctx, TableMembres& table, std::uint64_t& cle,
                             std::uint32_t& n) {
    if (ctx.naissance) {
        if (!ctx.a_cle) {
            return false;
        }
        cle = ctx.cle;
        n = ctx.prolongements++;
        return true;
    }
    return ctx.membre != 0 && table.tentative_suivante(ctx.membre, cle, n);
}

// ── Compteurs ──────────────────────────────────────────────────────────────────────────────
struct Compteur {
    std::atomic<std::uint64_t> appels{0};   // le detour a tourne
    std::atomic<std::uint64_t> derives{0};  // une cle existait : la valeur derivee a ete rendue
    std::atomic<std::uint64_t> changes{0};  // … et elle DIFFERAIT du vanilla (le temoin)
    std::atomic<std::uint64_t> hors_cle{0}; // pas de cle : vanilla laisse passer
};

} // namespace tessera::foule

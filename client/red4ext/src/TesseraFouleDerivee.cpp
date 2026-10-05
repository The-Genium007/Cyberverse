// Foule derivee — les DETOURS des hooks A' a H du monde partage (Tessera, lots 3 a 6).
//
// La logique est dans FouleDerivee.hpp (pure, testee hors jeu). Ici : lire des arguments, appeler
// la logique, ecrire le resultat. Binaire epingle : Cyberpunk2077.exe 2.31 (ADR 0001) ; toutes
// les adresses sont dans la base de Ghidra (0x140000000) et chaque signature a ete relue au
// decompile le 2026-10-05 (F-PNJ-250 a 259).
//
// ⚠️⚠️ EFFET EN JEU NON MESURE. Cette unite a ete COMPILEE, jamais chargee dans le jeu. Rien de
// ce qui suit n'est « fait » au sens de D1 tant que le bloc 4 ter de la campagne n'a pas tourne.
//
// TROIS FACONS DE PRENDRE UNE DECISION, selon la forme de la fonction du moteur :
//   1. elle REND sa decision (B, E) → on appelle l'original (il consomme son tirage, F-PNJ-205,
//      et donne le temoin vanilla), puis on rend la valeur derivee ;
//   2. elle ECRIT sa decision dans un champ (D, 14088d44c) → original, puis reecriture ;
//   3. elle tire AU MILIEU de son corps (A''', C, H, 14088a908) → on IMPOSE la prochaine sortie
//      du generateur du thread juste avant de l'appeler (`pcg_etat_imposant`) : le moteur fait
//      lui-meme son calcul avec notre nombre. Le temoin vanilla est la sortie qu'aurait donnee
//      l'etat d'origine, calculee sans le consommer.
// Sans cle (individu inconnu) : vanilla, et un compteur « hors cle ».
//
// HORS PERIMETRE, vus dans le binaire et NON hookes (recensement : F-PNJ-264) :
// `1408f2648` (Bernoulli sur le chemin de foule), `14012c9f8` x2 (reglages
// d'evitement « marbles »), `140417665` (victime au hasard quand un creneau deborde),
// `140421e98` (idem, spawner de communaute), `140416c74`/`1405e7704`/`1405e7b58` (record au
// hasard dans un vivier), `140411dcc` (table de paires du vehicule), `140887fbc` (position dans
// l'espace libre, F-PNJ-230). Et F (`140425018`, deux distances d'anticipation) : tirages INLINE
// dans une fonction d'activation de 300 lignes, de bas rang — ils n'agissent que sur l'evitement,
// que le rappel de position absorbe (spec §3, ligne 8). Deux hooks de plus pour un ecart que la
// porte B n'observe pas : non.
#include "TesseraFouleDerivee.h"

#include "FouleDerivee.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>

namespace
{
using namespace tessera::foule;
namespace pop = tessera::population;

const RED4ext::v1::Sdk* g_sdk = nullptr;
RED4ext::v1::PluginHandle g_handle = nullptr;

Interrupteurs g_inter;
TableMembres g_table;

std::atomic<std::uint64_t> g_graine{0};
std::atomic<std::uint8_t> g_mode{0};
std::atomic<std::uint64_t> g_empreinteServeur{0};
std::atomic<bool> g_mondeRecu{false};
std::mutex g_suspendusVerrou;
std::set<std::uint64_t> g_suspendus;

std::atomic<std::int64_t> g_decalageServeurMs{0};
std::atomic<bool> g_horlogePosee{false};

// Bornes de vitesse apprises du moteur lui-meme (arguments de `1405fc200`) : [1] pieton,
// [2] vehicule. Tant qu'elles ne sont pas vues, pas d'emetteur (la 1re naissance est hors cle).
std::atomic<float> g_vMin[3];
std::atomic<float> g_vMax[3];

std::atomic<std::uintptr_t> g_mgrPietons{0};
std::atomic<std::uintptr_t> g_mgrVehicules{0};

thread_local Contexte t_ctx;
thread_local std::uint32_t t_occAppels = 0;  // appels du filtre d'occupation depuis le dernier E
thread_local std::uint32_t t_occRend1 = 0;   // … dont ceux qui ont rendu 1

struct Compteurs
{
    Compteur entree, abscisse, roue, apparence, vitesse, trajet, garee, reeval, delai, conduite;
    std::atomic<std::uint64_t> naissancesCle{0}, naissancesHorsCle{0}, sansFragment{0},
        sansBornes{0}, sansHorloge{0}, sansCandidat{0}, despawns{0}, despawnsConnus{0};
} g_c;

int g_hooksOk = 0;
int g_hooksTotal = 0;

// ── Adresses (base Ghidra) ─────────────────────────────────────────────────────────────────
constexpr std::uint64_t kVaPcg = 0x14013bdb8;             // &TLS[0xB18], seme a la demande
constexpr std::uint64_t kVaEntree = 0x14041dd50;          // Tessera_CrowdSpawnStubFromSlot
constexpr std::uint64_t kVaNaissancePieton = 0x1408f32a0; // A'
constexpr std::uint64_t kVaNaissanceVehicule = 0x140407c1c; // A'' (cle seulement)
constexpr std::uint64_t kVaRoue = 0x1404062b8;            // B
constexpr std::uint64_t kVaApparence = 0x1405e7974;       // C
constexpr std::uint64_t kVaVitesse = 0x1405fc200;         // D
constexpr std::uint64_t kVaCtorBase = 0x1405fc0c8;        // constructeur d'etat commun
constexpr std::uint64_t kVaRoueNormalisee = 0x1405fdbe4;  // E
constexpr std::uint64_t kVaFiltreOccupation = 0x141ca8ef0;
constexpr std::uint64_t kVaTickTrajetPieton = 0x1408f18dc;
constexpr std::uint64_t kVaReplanPieton = 0x1408f215c;
constexpr std::uint64_t kVaTickTrajetVehicule = 0x14012bc94;
constexpr std::uint64_t kVaPorteGaree = 0x14083b614;      // A'''
constexpr std::uint64_t kVaPorteReeval = 0x141cc9e00;     // H
constexpr std::uint64_t kVaCtorVehicule = 0x140889c70;    // G (naissance)
constexpr std::uint64_t kVaReroll = 0x140889bd4;          // G (re-tirage)
constexpr std::uint64_t kVaDelai = 0x14088a908;           // G
constexpr std::uint64_t kVaConduite = 0x14088d44c;        // G
constexpr std::uint64_t kVaDespawn = 0x140887bb8;         // Tessera_CrowdMgr_DespawnMember
constexpr std::uint64_t kVaStubTeleport = 0x1403beea0;    // T14
constexpr std::uint64_t kVaVerrouiller = 0x1402f0968;     // prise du verrou a octet du moteur
constexpr std::uint64_t kVaVtableScorePieton = 0x143335808;
constexpr std::uint64_t kVaVtableScoreVehicule = 0x14332c7d0;

std::uintptr_t Adresse(std::uint64_t aVaGhidra)
{
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) +
           static_cast<std::uintptr_t>(aVaGhidra - 0x140000000ull);
}

// ── Lecture gardee (SEH) — pour ce qu'on suit SANS que le moteur nous l'ait passe ──────────
template <typename T> bool LireSur(std::uintptr_t aOu, T& aSortie) noexcept
{
    __try
    {
        aSortie = *reinterpret_cast<const T*>(aOu);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T> T Champ(const void* aBase, std::size_t aDecalage)
{
    T v;
    std::memcpy(&v, static_cast<const std::uint8_t*>(aBase) + aDecalage, sizeof(T));
    return v;
}

// ── Horloge, mode, journal ─────────────────────────────────────────────────────────────────
std::int64_t LocalMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::uint64_t HeureServeurMs()
{
    if (!g_horlogePosee.load(std::memory_order_relaxed))
    {
        return 0;
    }
    const std::int64_t t = LocalMs() + g_decalageServeurMs.load(std::memory_order_relaxed);
    return t > 0 ? static_cast<std::uint64_t>(t) : 0;
}

bool Actif()
{
    return g_inter.derivee || g_mode.load(std::memory_order_relaxed) == 1;
}

// Au plus `kLignesParSeconde` lignes par site et par seconde ; le reste vit dans les compteurs.
constexpr int kLignesParSeconde = 4;
struct Debit
{
    std::atomic<std::int64_t> seconde{0};
    std::atomic<int> n{0};
};

bool Autorise(Debit& aDebit)
{
    if (!g_inter.journal || g_sdk == nullptr)
    {
        return false;
    }
    const std::int64_t s = LocalMs() / 1000;
    if (aDebit.seconde.exchange(s, std::memory_order_relaxed) != s)
    {
        aDebit.n.store(0, std::memory_order_relaxed);
    }
    return aDebit.n.fetch_add(1, std::memory_order_relaxed) < kLignesParSeconde;
}

#define FOULE_LOG(debit, ...)                                                                      \
    do                                                                                             \
    {                                                                                              \
        if (Autorise(debit))                                                                       \
        {                                                                                          \
            g_sdk->logger->InfoF(g_handle, __VA_ARGS__);                                           \
        }                                                                                          \
    } while (false)

// ── Le generateur du thread, et l'imposition d'un tirage ───────────────────────────────────
std::uint64_t* EtatPcg()
{
    using Fn = std::uint64_t* (*)();
    return reinterpret_cast<Fn>(Adresse(kVaPcg))();
}

// Fiche (ADR 0034) de l'ecriture dans l'etat du generateur :
//   Cible        : l'etat PCG du thread, `TLS[0xB18]`, rendu par `14013bdb8` (F-PNJ-177).
//   Lecteurs     : 293 sites d'appel dans tout le jeu (xrefs du 2026-10-05 ; F-PNJ-212 en
//                  comptait 200) — d'ou la portee d'UN appel.
//   Alimente     : le prochain tirage du thread, c'est-a-dire celui de la fonction qu'on appelle
//                  juste apres (verifie pour chacune : c'est son PREMIER tirage).
//   Domaine      : 64 bits quelconques ; sortie relue au desassemblage (F-PNJ-251).
//   Hors domaine : sans objet.
//   Qui d'AUTRE  : tout tirage du meme thread, a chaque appel. Si la fonction appelee ne tire
//                  pas (garde en amont), l'etat d'origine est REMIS : rien n'est consomme.
class TirageImpose
{
public:
    explicit TirageImpose(std::uint32_t aVoulu)
        : m_etat(EtatPcg()), m_origine(*m_etat), m_impose(pcg_etat_imposant(aVoulu, m_origine))
    {
        *m_etat = m_impose;
    }
    // true si la fonction a tire. Sinon l'etat d'origine est restaure.
    bool Fin()
    {
        if (*m_etat == m_impose)
        {
            *m_etat = m_origine;
            return false;
        }
        return true;
    }
    std::uint32_t SortieVanilla() const { return pcg_sortie(m_origine); }
    float FractionVanilla() const { return pcg_fraction(m_origine); }

private:
    std::uint64_t* m_etat;
    std::uint64_t m_origine;
    std::uint64_t m_impose;
};

// ── Le fragment de voie d'un creneau (emetteur, spec §1) ───────────────────────────────────
//
// laneSys = mgr+0x140 ; table de hachage cle-de-voie -> voie (F-PNJ-244, memes decalages que
// TesseraVoiesFoule.cpp) ; voie+0x60 : fragments de 0x40 o., bornes d'abscisse en +0x38/+0x3c.
// La fonction de hachage du moteur n'est pas lue : on balaie les seaux UNE fois par voie, puis
// on garde l'adresse du noeud (revalidee a chaque usage — une voie destreamee change de noeud).
struct NoeudVu
{
    std::uintptr_t noeud = 0;
    std::int64_t jusqua = 0; // un « introuvable » n'est retente qu'apres cette date
};
std::mutex g_voiesVerrou;
std::unordered_map<std::uint64_t, NoeudVu> g_voies;

bool NoeudCorrespond(std::uintptr_t aNoeud, const Creneau& aK)
{
    std::uint64_t cle = 0;
    std::uint16_t a = 0, b = 0;
    std::uint8_t c = 0;
    return aNoeud != 0 && LireSur(aNoeud + 8, cle) && LireSur(aNoeud + 0x10, a) &&
           LireSur(aNoeud + 0x12, b) && LireSur(aNoeud + 0x14, c) && cle == aK.zone && a == aK.a &&
           b == aK.b && c == aK.c;
}

std::uintptr_t ChercherNoeud(std::uintptr_t aMgr, const Creneau& aK)
{
    std::uintptr_t lanes = 0, index = 0, noeuds = 0;
    std::uint32_t capacite = 0, pas = 0;
    if (!LireSur(aMgr + 0x140, lanes) || lanes == 0 || !LireSur(lanes, index) ||
        !LireSur(lanes + 0xc, capacite) || !LireSur(lanes + 0x10, noeuds) ||
        !LireSur(lanes + 0x1c, pas) || pas < 0x20 || pas > 0x100 || capacite == 0 ||
        capacite > (1u << 22))
    {
        return 0;
    }
    for (std::uint32_t s = 0; s < capacite; ++s)
    {
        std::uint32_t i = 0;
        if (!LireSur(index + s * 4ull, i))
        {
            return 0;
        }
        for (std::uint32_t garde = 0; i != 0xFFFFFFFFu && garde < 4096; ++garde)
        {
            const auto noeud = noeuds + static_cast<std::uintptr_t>(i) * pas;
            if (NoeudCorrespond(noeud, aK))
            {
                return noeud;
            }
            if (!LireSur(noeud, i))
            {
                return 0;
            }
        }
    }
    return 0;
}

bool TrouverFragment(std::uintptr_t aMgr, const Creneau& aK, float& aX1, float& aX2)
{
    const std::uint64_t id = identite_voie(aK.zone, aK.a, aK.b, aK.c);
    const std::int64_t maintenant = LocalMs();
    std::uintptr_t noeud = 0;
    {
        std::lock_guard<std::mutex> g(g_voiesVerrou);
        const auto it = g_voies.find(id);
        if (it != g_voies.end())
        {
            if (it->second.noeud == 0 && maintenant < it->second.jusqua)
            {
                return false;
            }
            noeud = it->second.noeud;
        }
    }
    if (!NoeudCorrespond(noeud, aK))
    {
        noeud = ChercherNoeud(aMgr, aK);
        std::lock_guard<std::mutex> g(g_voiesVerrou);
        if (g_voies.size() > 8192)
        {
            g_voies.clear(); // ponytail: purge totale ; une voie retrouvee coute un balayage
        }
        g_voies[id] = NoeudVu{noeud, maintenant + 5000};
    }
    std::uintptr_t voie = 0, frags = 0;
    std::uint32_t nb = 0;
    if (noeud == 0 || !LireSur(noeud + 0x18, voie) || voie == 0 || !LireSur(voie + 0x60, frags) ||
        !LireSur(voie + 0x6c, nb) || nb == 0 || nb > 256)
    {
        return false;
    }
    const float milieu = (aK.s0 + aK.s1) * 0.5f;
    for (std::uint32_t f = 0; f < nb; ++f)
    {
        float d = 0, e = 0;
        if (!LireSur(frags + f * 0x40ull + 0x38, d) || !LireSur(frags + f * 0x40ull + 0x3c, e))
        {
            return false;
        }
        if (d <= milieu && milieu <= e)
        {
            aX1 = d;
            aX2 = e;
            return true;
        }
    }
    return false;
}

// Qui nait dans ce creneau ? (hook A'/A'')
Naissance Preparer(std::uintptr_t aMgr, const Creneau& aK, std::uint8_t aFamille, float& aX1,
                   float& aX2)
{
    Naissance rien;
    const float vMin = g_vMin[aFamille].load(std::memory_order_relaxed);
    const float vMax = g_vMax[aFamille].load(std::memory_order_relaxed);
    if (!(vMin > 0.0f) || !(vMax >= vMin))
    {
        ++g_c.sansBornes;
        return rien;
    }
    aX1 = aK.s0;
    aX2 = aK.s1;
    if (!g_inter.emetteur_creneau && !TrouverFragment(aMgr, aK, aX1, aX2))
    {
        ++g_c.sansFragment;
        return rien;
    }
    const std::uint64_t t = HeureServeurMs();
    if (t == 0)
    {
        ++g_c.sansHorloge;
        return rien;
    }
    const pop::Emetteur e = emetteur_du_creneau(aK, aX1, aX2, vMin, vMax);
    std::vector<std::uint64_t> interdits;
    g_table.cles_de_l_emetteur(identite_emetteur(e), interdits);
    {
        std::lock_guard<std::mutex> g(g_suspendusVerrou);
        interdits.insert(interdits.end(), g_suspendus.begin(), g_suspendus.end());
    }
    const Naissance n =
        preparer_naissance(e, aK, g_graine.load(std::memory_order_relaxed), t, interdits);
    if (!n.a_cle)
    {
        ++g_c.sansCandidat;
    }
    return n;
}

// Portee membre : pose l'individu courant du thread, et remet ce qu'elle a touche en sortant.
class PorteeMembre
{
public:
    PorteeMembre(std::uint64_t aMembre, bool aVehicule, bool aCtorBase, bool aRemettreDelai)
        : m_membre(t_ctx.membre), m_vehicule(t_ctx.vehicule), m_ctorBase(t_ctx.ctor_base),
          m_delai(t_ctx.delai)
    {
        t_ctx.membre = aMembre;
        t_ctx.vehicule = t_ctx.vehicule || aVehicule;
        t_ctx.ctor_base = aCtorBase;
        if (aRemettreDelai)
        {
            t_ctx.delai = 0;
        }
    }
    ~PorteeMembre()
    {
        t_ctx.membre = m_membre;
        t_ctx.vehicule = m_vehicule;
        t_ctx.ctor_base = m_ctorBase;
        t_ctx.delai = m_delai;
    }

private:
    std::uint64_t m_membre;
    bool m_vehicule;
    bool m_ctorBase;
    int m_delai;
};

// Le membre d'un etat de deplacement : `+0x18` (pointeur brut), a defaut l'instance du lien
// faible en `+8`. Sert de CLE DE TABLE seulement — jamais dereference par nous.
std::uint64_t MembreDeLEtat(const void* aEtat)
{
    const auto m = Champ<std::uint64_t>(aEtat, 0x18);
    return m != 0 ? m : Champ<std::uint64_t>(aEtat, 0x08);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// ENTREE — `14041dd50` (mgr, sortie, creneau VIVANT, contexte) → sortie. F-PNJ-187.
//
// Lecture seule : le creneau recu ici est l'entree VIVANTE de la table (ou de la file) — y
// ecrire tue le creneau en silence (F-PNJ-189). Ce detour ouvre le contexte de naissance, puis
// range `membre → cle` quand le moteur a rendu un membre.
//   Cible        : notre table `membre → cle`, pas le moteur.
//   Lecteurs     : B, C, D, E, G, H (par `resoudre_cle`).
//   Alimente     : toutes les decisions de l'individu apres sa naissance.
//   Domaine      : adresse du membre simule rendu dans `*sortie` (non nul = ne).
//   Hors domaine : naissance ratee → rien n'est range.
//   Qui d'AUTRE  : `140887bb8` (despawn) retire ; la recursion sur la fenetre residuelle range
//                  elle-meme (drapeau `imbrique`), l'appel externe ne reecrit pas par-dessus.
// ═══════════════════════════════════════════════════════════════════════════════════════════
using Entree_t = void* (*)(void*, void*, std::uint8_t*, void*);
Entree_t o_Entree = nullptr;

void* D_Entree(void* aMgr, void* aSortie, std::uint8_t* aCreneau, void* aP4)
{
    ++g_c.entree.appels;
    const Contexte parent = t_ctx;
    t_ctx = Contexte{};
    t_ctx.naissance = true;
    t_ctx.mgr = reinterpret_cast<std::uint64_t>(aMgr);
    t_ctx.creneau = lire_creneau(aCreneau);

    void* r = o_Entree(aMgr, aSortie, aCreneau, aP4);

    const std::uint64_t membre = aSortie != nullptr ? Champ<std::uint64_t>(aSortie, 0) : 0;
    if (membre != 0 && !t_ctx.imbrique)
    {
        if (t_ctx.a_cle)
        {
            Fiche f;
            f.cle = t_ctx.cle;
            f.emetteur = t_ctx.emetteur;
            f.prolongements = t_ctx.prolongements;
            f.famille = t_ctx.famille;
            g_table.poser(membre, f);
            ++g_c.naissancesCle;
        }
        else
        {
            g_table.retirer(membre); // adresse recyclee : pas de fiche perimee (F-PNJ-225)
            ++g_c.naissancesHorsCle;
        }
    }
    t_ctx = parent;
    if (parent.naissance && membre != 0)
    {
        t_ctx.imbrique = true;
    }
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// A' — `1408f32a0` (mgr, sortie, id, COPIE DE PILE du creneau, contexte). F-PNJ-189.
//   Cible        : `copie+0x10` et `copie+0x14` (fenetre [s0, s1]), dans la copie de 0x30 o.
//                  que `14041dd50` vient de faire sur SA pile — jamais l'entree vivante (R8 de
//                  l'entree), dont une fenetre nulle met la priorite a zero (F-PNJ-189).
//   Lecteurs     : 2, relus au decompile ce jour — `1408f3358` (chemin de foule, recoit s0, s1
//                  et effectif+1) et la naissance sur voie (`lerp(s0, s1, pcg)`, F-PNJ-188).
//   Alimente     : l'abscisse de naissance, et par elle le fragment, la position, le chemin.
//   Domaine      : un flottant dans la fenetre d'origine [s0, s1] (c'est `choisir` qui le
//                  garantit) ; s0 == s1 → `lerp(a, a, x) = a` quel que soit le tirage.
//   Hors domaine : non mesure — hypothese : une abscisse hors de la voie fait echouer la
//                  resolution du fragment, donc la naissance. Sonde : T4 (controle A, s0).
//   Qui d'AUTRE  : `14041dd50`, une fois par appel (la copie) ; et il RELIT s0/s1 de cette
//                  copie apres notre retour pour calculer la fenetre de la recursion — d'ou la
//                  REMISE des deux valeurs d'origine a la sortie du detour.
// ⚠️ H1-H3 (spec §1) NON MESUREES : la formule de `n`, « la fenetre tient dans un fragment »,
// « creneau+0x00 est la cle de la voie ». Le journal S-L4f ci-dessous est la sonde.
// ═══════════════════════════════════════════════════════════════════════════════════════════
using NaissancePieton_t = void* (*)(void*, void*, std::uint64_t, std::uint8_t*, void*);
NaissancePieton_t o_NaissancePieton = nullptr;
Debit g_debitAbscisse;

void OuvrirNaissance(void* aMgr, std::uint8_t* aCopie, std::uint8_t aFamille, bool aEcrire,
                     float& aS0, float& aS1, bool& aEcrit)
{
    ++g_c.abscisse.appels;
    t_ctx.famille = aFamille;
    (aFamille == 1 ? g_mgrPietons : g_mgrVehicules)
        .store(reinterpret_cast<std::uintptr_t>(aMgr), std::memory_order_relaxed);
    const Creneau k = lire_creneau(aCopie);
    aS0 = k.s0;
    aS1 = k.s1;
    aEcrit = false;
    float x1 = 0, x2 = 0, abscisse = 0;
    Naissance n;
    if (g_inter.controle == 'A' && aEcrire)
    {
        abscisse = k.s0;
        aEcrit = true;
    }
    else if (Actif())
    {
        n = Preparer(reinterpret_cast<std::uintptr_t>(aMgr), k, aFamille, x1, x2);
        if (n.a_cle)
        {
            t_ctx.a_cle = true;
            t_ctx.cle = n.cle;
            t_ctx.emetteur = n.emetteur;
            abscisse = n.abscisse;
            aEcrit = aEcrire;
            ++g_c.abscisse.derives;
        }
        else
        {
            ++g_c.abscisse.hors_cle;
        }
    }
    else
    {
        ++g_c.abscisse.hors_cle;
    }
    if (aEcrit)
    {
        std::memcpy(aCopie + 0x10, &abscisse, 4);
        std::memcpy(aCopie + 0x14, &abscisse, 4);
        ++g_c.abscisse.changes; // une fenetre non nulle devient un point : toujours un changement
    }
    // S-L4f : tout ce que le creneau porte, et ce qu'on en a tire.
    FOULE_LOG(g_debitAbscisse,
              "[foule] A%s voie=%llx/%u/%u/%u s0=%.2f s1=%.2f L=%.2f cible=%.3f effectif=%u "
              "fragment=%.2f-%.2f cle=%llx k=%llu vanilla=[%.2f,%.2f] derive=%.2f ecrit=%d",
              aFamille == 1 ? "'" : "''", static_cast<unsigned long long>(k.zone), k.a, k.b, k.c,
              k.s0, k.s1, k.longueur, k.cible, k.effectif, x1, x2,
              static_cast<unsigned long long>(n.cle), static_cast<unsigned long long>(n.k), k.s0,
              k.s1, abscisse, aEcrit ? 1 : 0);
}

void* D_NaissancePieton(void* aMgr, void* aSortie, std::uint64_t aId, std::uint8_t* aCopie,
                        void* aP5)
{
    float s0 = 0, s1 = 0;
    bool ecrit = false;
    OuvrirNaissance(aMgr, aCopie, 1, true, s0, s1, ecrit);
    void* r = o_NaissancePieton(aMgr, aSortie, aId, aCopie, aP5);
    if (ecrit)
    {
        std::memcpy(aCopie + 0x10, &s0, 4);
        std::memcpy(aCopie + 0x14, &s1, 4);
    }
    return r;
}

// A'' — `140407c1c` (mgr, sortie, id, COPIE DE PILE). La CLE seulement, pas l'abscisse :
// la fenetre d'un vehicule sert aussi d'« espace libre » a `140887fbc` (F-PNJ-230), et une
// fenetre reduite a un point y laisserait zero metre pour poser une voiture — non mesure,
// hypothese ; la position des vehicules est le sujet du lot 6 (branche `+0x1c` ou hook I).
using NaissanceVehicule_t = void* (*)(void*, void*, std::uint64_t, std::uint8_t*);
NaissanceVehicule_t o_NaissanceVehicule = nullptr;

void* D_NaissanceVehicule(void* aMgr, void* aSortie, std::uint64_t aId, std::uint8_t* aCopie)
{
    float s0 = 0, s1 = 0;
    bool ecrit = false;
    OuvrirNaissance(aMgr, aCopie, 2, false, s0, s1, ecrit);
    return o_NaissanceVehicule(aMgr, aSortie, aId, aCopie);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// B — `1404062b8` (liste {base, _, nombre en +0xc}) → entree de 12 o. {valeur 8 o., poids}.
//   Cible        : la valeur de RETOUR (pointeur sur l'entree retenue). Rien n'est ecrit.
//   Lecteurs     : les 4 appelants (F-PNJ-178) : pieton sur voie, vehicule sur voie
//                  (`140407c1c`), voiture garee (`14264e748`, F-PNJ-221), chemin de foule.
//   Alimente     : le record du personnage / le modele du vehicule.
//   Domaine      : `base + i × 12`, i dans [0, nombre) — la roue soustractive SANS
//                  normalisation de F-PNJ-202, copiee telle quelle par `tirage_pondere`.
//   Hors domaine : liste vide ou > 256 entrees → vanilla (on ne rend jamais hors de la liste).
//   Qui d'AUTRE  : personne — c'est un retour, pas un champ.
// ═══════════════════════════════════════════════════════════════════════════════════════════
using Roue_t = std::uint8_t* (*)(void*);
Roue_t o_Roue = nullptr;
Debit g_debitRoue;

std::uint8_t* D_Roue(void* aListe)
{
    std::uint8_t* vanilla = o_Roue(aListe); // consomme le tirage comme CDPR (F-PNJ-205)
    ++g_c.roue.appels;
    std::uint64_t cle = 0;
    const Source src = resoudre_cle(t_ctx, g_table, cle);
    const bool controle = g_inter.controle == 'B';
    auto* base = Champ<std::uint8_t*>(aListe, 0);
    const auto n = Champ<std::uint32_t>(aListe, 0xc);
    if ((!controle && (!Actif() || src == Source::Aucune)) || base == nullptr || n == 0 || n > 256)
    {
        ++g_c.roue.hors_cle;
        return vanilla;
    }
    float poids[256];
    for (std::uint32_t i = 0; i < n; ++i)
    {
        std::memcpy(&poids[i], base + i * 12ull + 8, 4);
    }
    const int d = decider_archetype(poids, n, cle, g_inter.controle);
    std::uint8_t* derive = base + static_cast<std::size_t>(d) * 12;
    ++g_c.roue.derives;
    if (derive != vanilla)
    {
        ++g_c.roue.changes;
    }
    FOULE_LOG(g_debitRoue, "[foule] B vanilla=%d derive=%d n=%u cle=%llx source=%d",
              static_cast<int>((vanilla - base) / 12), d, n, static_cast<unsigned long long>(cle),
              static_cast<int>(src));
    return derive;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// C — `1405e7974` (sous-systeme, sortie, record deja choisi, drapeau) → sortie. F-PNJ-178.
// Tirage INLINE `sortie_pcg % N` (trois branches, trois listes) : on impose la sortie.
//   Cible        : la prochaine sortie du generateur du thread (voir `TirageImpose`).
//   Lecteurs     : 1 par appel — la branche prise (table par cle · liste filtree · liste brute).
//   Alimente     : l'indice dans la liste d'apparences, donc `*sortie`.
//   Domaine      : `bits % PPCM(1..22)` → `% N` vaut `index_uniforme(N, cle, Apparence)` pour
//                  tout N <= 22 ; au-dela, deterministe mais hors reference Rust (F-PNJ-254).
//   Hors domaine : liste vide → le moteur ne tire pas, l'etat est remis.
//   Qui d'AUTRE  : rien d'autre ne tire entre notre ecriture et le `% N` (relu : les trois
//                  branches appellent `14013bdb8` puis tirent, sans tirage intermediaire).
// ⚠️ L'ORDRE de la liste est celui du moteur, pas un tri par nom (spec §3 ligne 4) :
// non mesure — hypothese qu'il est le meme sur deux clients (meme fichier). Sonde : T13.
// ═══════════════════════════════════════════════════════════════════════════════════════════
using Apparence_t = void** (*)(void*, void**, std::uint64_t, char);
Apparence_t o_Apparence = nullptr;
Debit g_debitApparence;

void** D_Apparence(void* aSys, void** aSortie, std::uint64_t aRecord, char aDrapeau)
{
    ++g_c.apparence.appels;
    std::uint64_t cle = 0;
    const Source src = resoudre_cle(t_ctx, g_table, cle);
    const bool controle = g_inter.controle == 'C';
    if (!controle && (!Actif() || src == Source::Aucune))
    {
        ++g_c.apparence.hors_cle;
        return o_Apparence(aSys, aSortie, aRecord, aDrapeau);
    }
    const std::uint32_t voulu = sortie_apparence(cle, g_inter.controle);
    TirageImpose tirage(voulu);
    void** r = o_Apparence(aSys, aSortie, aRecord, aDrapeau);
    const bool tire = tirage.Fin();
    if (tire)
    {
        ++g_c.apparence.derives;
        if (tirage.SortieVanilla() != voulu)
        {
            ++g_c.apparence.changes;
        }
    }
    FOULE_LOG(g_debitApparence,
              "[foule] C vanilla=%u derive=%u (sorties, a prendre modulo N) tire=%d record=%llx "
              "cle=%llx",
              tirage.SortieVanilla(), voulu, tire ? 1 : 0, static_cast<unsigned long long>(aRecord),
              static_cast<unsigned long long>(cle));
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// D — `1405fc200` (bloc de vitesse, min, max) : `bloc[0] = min ; bloc[1] = max ;
// bloc[4] = lerp(min, max, pcg)`. F-PNJ-188, F-PNJ-221.
//   Cible        : `bloc[4]` (bloc = etat+0x4c, donc etat+0x5c) — la vitesse individuelle.
//   Lecteurs     : non recenses un par un — non mesure ; au moins `1405fc1d8` (lu dans le
//                  constructeur `1405fc0c8`, qui recopie dans membre+0x64/+0x68) et l'integration
//                  du deplacement (F-PNJ-188).
//   Alimente     : la vitesse de croisiere du membre ; c'est `v_k` de la reference (spec §1).
//   Domaine      : [min, max], les deux arguments (bornes du record `Crowds.DefaultMovement`
//                  ou `Crowds.DefaultDriving`, ou celles memorisees dans le membre).
//   Hors domaine : sans objet (`vitesse()` reste dans [min, max)).
//   Qui d'AUTRE  : `14088d44c` REECRIT LE MEME CHAMP (meme bloc, meme `[4]`) depuis
//                  `1410ea1c0` (re-tirage vehicule) et `14088be44` — F-PNJ-253 ; et le
//                  re-tirage `140889bd4` rappelle D a chaque changement de reglages
//                  (F-PNJ-221). Les trois sont derives avec le MEME selecteur : quel que soit
//                  le dernier ecrivain, la valeur est la meme.
// ═══════════════════════════════════════════════════════════════════════════════════════════
using Vitesse_t = void (*)(float*, float, float);
Vitesse_t o_Vitesse = nullptr;
Debit g_debitVitesse;

void D_Vitesse(float* aBloc, float aMin, float aMax)
{
    o_Vitesse(aBloc, aMin, aMax);
    ++g_c.vitesse.appels;
    // Les bornes que le moteur emploie, par famille — pour l'emetteur de A'.
    if (t_ctx.ctor_base && !t_ctx.vehicule)
    {
        g_vMin[1].store(aMin, std::memory_order_relaxed);
        g_vMax[1].store(aMax, std::memory_order_relaxed);
    }
    else if (t_ctx.vehicule && !t_ctx.ctor_base)
    {
        g_vMin[2].store(aMin, std::memory_order_relaxed);
        g_vMax[2].store(aMax, std::memory_order_relaxed);
    }
    std::uint64_t cle = 0;
    const Source src = resoudre_cle(t_ctx, g_table, cle);
    const bool controle = g_inter.controle == 'D';
    if (!controle && (!Actif() || src == Source::Aucune))
    {
        ++g_c.vitesse.hors_cle;
        return;
    }
    const float vanilla = aBloc[4];
    const float derive = decider_vitesse(aMin, aMax, cle, g_inter.controle);
    aBloc[4] = derive;
    ++g_c.vitesse.derives;
    if (derive != vanilla)
    {
        ++g_c.vitesse.changes;
    }
    FOULE_LOG(g_debitVitesse, "[foule] D vanilla=%.4f derive=%.4f min=%.3f max=%.3f cle=%llx source=%d",
              vanilla, derive, aMin, aMax, static_cast<unsigned long long>(cle),
              static_cast<int>(src));
}

// G (2/3) — `14088d44c` (bloc) : `bloc[4] = lerp(bloc[0], bloc[1], pcg)`. Meme champ que D.
using Conduite_t = void (*)(float*);
Conduite_t o_Conduite = nullptr;
Debit g_debitConduite;

void D_Conduite(float* aBloc)
{
    o_Conduite(aBloc);
    ++g_c.conduite.appels;
    std::uint64_t cle = 0;
    const Source src = resoudre_cle(t_ctx, g_table, cle);
    const bool controle = g_inter.controle == 'D';
    if (!controle && (!Actif() || src == Source::Aucune))
    {
        ++g_c.conduite.hors_cle;
        return;
    }
    const float vanilla = aBloc[4];
    const float derive = decider_vitesse(aBloc[0], aBloc[1], cle, g_inter.controle);
    aBloc[4] = derive;
    ++g_c.conduite.derives;
    if (derive != vanilla)
    {
        ++g_c.conduite.changes;
    }
    FOULE_LOG(g_debitConduite, "[foule] G/conduite vanilla=%.4f derive=%.4f cle=%llx", vanilla,
              derive, static_cast<unsigned long long>(cle));
}

// Constructeur d'etat commun `1405fc0c8` (etat, lien faible vers le membre, …) : il appelle D.
// Portee membre seulement — rien n'est ecrit ici.
using Ctor_t = void* (*)(void*, void*, void*, void*, void*);
Ctor_t o_CtorBase = nullptr;
Ctor_t o_CtorVehicule = nullptr;

void* D_CtorBase(void* aEtat, void* aLien, void* aP3, void* aP4, void* aP5)
{
    PorteeMembre portee(aLien != nullptr ? Champ<std::uint64_t>(aLien, 0) : 0, false, true, false);
    return o_CtorBase(aEtat, aLien, aP3, aP4, aP5);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// G — les parametres de conduite d'un vehicule, tires a la NAISSANCE (`140889c70`, constructeur
// de l'etat vehicule, vtable `142b2ace8`) et RE-TIRES a chaque changement de reglages
// (`140889bd4`, appelee a chaque tick). F-PNJ-221. Les deux sites ouvrent une portee
// « vehicule » ; les trois fonctions de tirage (D, `14088a908`, `14088d44c`) y derivent.
//   Cible        : etat+0x5c (vitesse, par D et `14088d44c`), etat+0x13c et etat+0x138 (les
//                  deux delais, valeurs de retour de `14088a908`).
//   Lecteurs     : non recenses — non mesure. Sonde : S-L6b (journal), puis xrefs Ghidra des
//                  trois decalages dans le tick vehicule `1402d377c`, ~30 min.
//   Alimente     : l'allure et les temps de reaction du vehicule de foule.
//   Domaine      : ceux que le moteur calcule lui-meme — on ne fournit que la fraction [0,1).
//   Hors domaine : sans objet.
//   Qui d'AUTRE  : le moteur, a chaque changement du compteur de reglages (membre+0x23c contre
//                  etat+0x60) — c'est precisement pourquoi les DEUX sites sont couverts : un
//                  re-tirage rend alors la meme valeur. Non couvert : `140411dcc` (table de
//                  paires, tirage inline, « ce qu'elle alimente n'est pas tranche », F-PNJ-221).
// ═══════════════════════════════════════════════════════════════════════════════════════════
void* D_CtorVehicule(void* aEtat, void* aLien, void* aP3, void* aP4, void* aP5)
{
    PorteeMembre portee(aLien != nullptr ? Champ<std::uint64_t>(aLien, 0) : 0, true, false, true);
    return o_CtorVehicule(aEtat, aLien, aP3, aP4, aP5);
}

using Etat_t = void (*)(void*);
Etat_t o_Reroll = nullptr;

void D_Reroll(void* aEtat)
{
    PorteeMembre portee(MembreDeLEtat(aEtat), true, false, true);
    o_Reroll(aEtat);
}

// G (3/3) — `14088a908` (bloc, x) → delai : un tirage inline, appelee DEUX fois de suite.
using Delai_t = float (*)(float*, float);
Delai_t o_Delai = nullptr;
Debit g_debitDelai;

float D_Delai(float* aBloc, float aX)
{
    ++g_c.delai.appels;
    std::uint64_t cle = 0;
    const Source src = resoudre_cle(t_ctx, g_table, cle);
    const bool controle = g_inter.controle == 'G';
    if (!t_ctx.vehicule || (!controle && (!Actif() || src == Source::Aucune)))
    {
        ++g_c.delai.hors_cle;
        return o_Delai(aBloc, aX);
    }
    const int rang = t_ctx.delai++;
    const float f = fraction_delai(cle, rang & 1, g_inter.controle);
    TirageImpose tirage(mantisse_de(f));
    const float r = o_Delai(aBloc, aX);
    if (tirage.Fin())
    {
        ++g_c.delai.derives;
        if (tirage.FractionVanilla() != f)
        {
            ++g_c.delai.changes;
        }
    }
    FOULE_LOG(g_debitDelai, "[foule] G/delai%d vanilla=%.6f derive=%.6f (fractions) rendu=%.4f cle=%llx",
              rang, tirage.FractionVanilla(), f, r, static_cast<unsigned long long>(cle));
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// E — `1405fdbe4` (liste d'entrees de 12 o., foncteur de score) → entree retenue, ou 0.
// F-PNJ-211, 220, 229, 231 ; contenu des foncteurs : F-PNJ-260 a 262.
//   Cible        : la valeur de RETOUR (pointeur sur le candidat retenu).
//   Lecteurs     : 2 — `140408398` (pieton : la borne de fin du dernier segment, puis
//                  l'extension recursive) et `1405fd0d8` (vehicule : la voie suivante).
//   Alimente     : le trajet (la rue) de l'individu, tous les 70 m / jusqu'au budget.
//   Domaine      : `base + i × 12`, i dans [0, nombre) — la roue NORMALISEE `u <= cumul`
//                  de F-PNJ-229, copiee par `tirage_pondere_normalise`, une cle par TENTATIVE.
//   Hors domaine : liste vide → 0 (vanilla) ; > 64 candidats → vanilla.
//   Qui d'AUTRE  : personne (retour). MAIS LA LISTE ELLE-MEME DEPEND D'UN ETAT LOCAL : le
//                  filtre d'occupation `141ca8ef0` retire les voies pleines AVANT nous, et le
//                  score pieton depend de la position courante du passant (F-PNJ-262, 263). E rend
//                  « l'indice k » dans une liste qui peut differer entre deux clients — il
//                  JOURNALISE donc candidats, scores et verdicts du filtre : c'est S-L4h.
// Le foncteur est appele une fois de plus par candidat pour lire les scores (lecture seule,
// relu : `1405fddd8` ne fait que calculer).
// ═══════════════════════════════════════════════════════════════════════════════════════════
using RoueNormalisee_t = std::uint8_t* (*)(void*, std::uint8_t*);
RoueNormalisee_t o_RoueNormalisee = nullptr;
Debit g_debitTrajet;

struct DernierTrajet
{
    std::uint64_t cle = 0, membre = 0, t_ms = 0;
    std::uint32_t n = 0, tentative = 0, occAppels = 0, occRend1 = 0;
    int vanilla = -1, derive = -1, famille = 0, source = 0;
    bool scorePur = false;
    float scores[16] = {};
    std::uint16_t voies[16] = {};
};
std::mutex g_dernierVerrou;
DernierTrajet g_dernierTrajet[3]; // [1] pieton, [2] vehicule, [0] foncteur inconnu

std::uint8_t* D_RoueNormalisee(void* aListe, std::uint8_t* aFoncteur)
{
    ++g_c.trajet.appels;
    auto* base = Champ<std::uint8_t*>(aListe, 0);
    const auto n = Champ<std::uint32_t>(aListe, 0xc);
    const auto vtable = Champ<std::uintptr_t>(aFoncteur, 0x40);
    const int famille = vtable == Adresse(kVaVtableScorePieton)     ? 1
                        : vtable == Adresse(kVaVtableScoreVehicule) ? 2
                                                                    : 0;
    const std::uint32_t occAppels = t_occAppels, occRend1 = t_occRend1;
    t_occAppels = 0;
    t_occRend1 = 0;

    // TESSERA_FOULE_SCORE_PUR : +INF dans posA du foncteur pieton → sa branche pure
    // (`_fdclass >= 1`, relu dans `1405fddd8`), au prix de la preference « pas de demi-tour ».
    float sauve[3] = {};
    const bool pur = g_inter.score_pur && famille == 1;
    if (pur)
    {
        std::memcpy(sauve, aFoncteur, 12);
        const float inf = std::numeric_limits<float>::infinity();
        const float trois[3] = {inf, inf, inf};
        std::memcpy(aFoncteur, trois, 12);
    }

    float scores[64];
    const bool lisible = base != nullptr && n != 0 && n <= 64 && vtable != 0;
    if (lisible)
    {
        using Score_t = float (*)(std::uint8_t*, std::uint64_t*, std::uint8_t*);
        const auto invoquer = *reinterpret_cast<Score_t*>(vtable);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            std::uint64_t tampon = 0;
            scores[i] = invoquer(aFoncteur, &tampon, base + i * 12ull);
        }
    }

    std::uint8_t* vanilla = o_RoueNormalisee(aListe, aFoncteur); // consomme le tirage (F-PNJ-205)
    if (pur)
    {
        std::memcpy(aFoncteur, sauve, 12);
    }

    std::uint64_t cle = 0;
    std::uint32_t tentative = 0;
    const bool controle = g_inter.controle == 'E';
    const bool aCle = lisible && vanilla != nullptr && Actif() &&
                      tentative_trajet(t_ctx, g_table, cle, tentative);
    std::uint8_t* rendu = vanilla;
    int d = -1;
    if (lisible && vanilla != nullptr && (aCle || controle))
    {
        d = decider_trajet(scores, n, cle, tentative, g_inter.controle);
        rendu = base + static_cast<std::size_t>(d) * 12;
        ++g_c.trajet.derives;
        if (rendu != vanilla)
        {
            ++g_c.trajet.changes;
        }
    }
    else
    {
        ++g_c.trajet.hors_cle;
    }
    const int v = (lisible && vanilla != nullptr) ? static_cast<int>((vanilla - base) / 12) : -1;
    {
        std::lock_guard<std::mutex> g(g_dernierVerrou);
        DernierTrajet& t = g_dernierTrajet[famille];
        t.cle = cle;
        t.membre = t_ctx.membre;
        t.t_ms = HeureServeurMs();
        t.n = n;
        t.tentative = tentative;
        t.occAppels = occAppels;
        t.occRend1 = occRend1;
        t.vanilla = v;
        t.derive = d;
        t.famille = famille;
        t.source = t_ctx.naissance ? 1 : (t_ctx.membre != 0 ? 2 : 0);
        t.scorePur = pur;
        for (std::uint32_t i = 0; i < 16; ++i)
        {
            t.scores[i] = (lisible && i < n) ? scores[i] : 0.0f;
            t.voies[i] = (lisible && i < n) ? Champ<std::uint16_t>(base + i * 12ull, 0) : 0;
        }
    }
    FOULE_LOG(g_debitTrajet,
              "[foule] E famille=%d vanilla=%d derive=%d n=%u tentative=%u cle=%llx "
              "filtreOcc=%u/%u(rend1) pur=%d s0=%.4f s1=%.4f s2=%.4f",
              famille, v, d, n, tentative, static_cast<unsigned long long>(cle), occAppels,
              occRend1, pur ? 1 : 0, lisible ? scores[0] : 0.0f, (lisible && n > 1) ? scores[1] : 0.0f,
              (lisible && n > 2) ? scores[2] : 0.0f);
    return rendu;
}

// Le filtre d'occupation `141ca8ef0` (captures, candidat) → octet. COMPTEUR SEUL, rien n'est
// change : il rend 0 quand l'occupation de la voie est sous sa capacite, 1 sinon ou quand le
// candidat porte le drapeau `+3 & 1` (relu au decompile). Sens exact du 1 (« a retirer ») :
// F-PNJ-263. Le compte est remis a zero a chaque passage dans E, sur le meme thread.
using Filtre_t = std::uint64_t (*)(void*, void*);
Filtre_t o_FiltreOccupation = nullptr;

std::uint64_t D_FiltreOccupation(void* aCaptures, void* aCandidat)
{
    const std::uint64_t r = o_FiltreOccupation(aCaptures, aCandidat);
    ++t_occAppels;
    if ((r & 0xff) != 0)
    {
        ++t_occRend1;
    }
    return r;
}

// Portees membre du trajet EN COURS DE VIE : sans elles E ne sait pas qui prolonge.
//   `1408f18dc` (etat, membre)  — prolongement depuis le tick pieton (F-PNJ-211) ;
//   `1408f215c` (etat)          — replan sur obstacle (F-PNJ-188) ;
//   `14012bc94` (etat vehicule) — pas de tick vehicule, seul appelant de `1405fd0d8` en vie.
using TickPieton_t = bool (*)(void*, void*);
TickPieton_t o_TickTrajetPieton = nullptr;
bool D_TickTrajetPieton(void* aEtat, void* aMembre)
{
    PorteeMembre portee(reinterpret_cast<std::uint64_t>(aMembre), false, false, false);
    return o_TickTrajetPieton(aEtat, aMembre);
}

using Replan_t = std::uint8_t (*)(void*);
Replan_t o_ReplanPieton = nullptr;
std::uint8_t D_ReplanPieton(void* aEtat)
{
    PorteeMembre portee(MembreDeLEtat(aEtat), false, false, false);
    return o_ReplanPieton(aEtat);
}

using TickVehicule_t = std::uint64_t (*)(void*);
TickVehicule_t o_TickTrajetVehicule = nullptr;
std::uint64_t D_TickTrajetVehicule(void* aEtat)
{
    PorteeMembre portee(MembreDeLEtat(aEtat), false, false, false);
    return o_TickTrajetVehicule(aEtat);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// A''' — `14083b614` (sous-systeme des garees = CrowdSystem+0x670, id de place, entree de la
// file {position 16 o., …, +0x20 indice de profil, +0x24 id, +0x28 active}, indice de profil).
// Porte de Bernoulli INLINE : `f <= densite × BaseParkedCarProbability × rampe`. F-PNJ-210
// (corrige), F-PNJ-221, F-VEH-060.
//   Cible        : la prochaine sortie du generateur du thread (voir `TirageImpose`).
//   Lecteurs     : 1 — la comparaison de la porte, premier tirage de la fonction.
//   Alimente     : « une voiture apparait-elle sur cette place » ; puis B pour le modele
//                  (`14264e748` → `1404062b8`), que la portee `garee` derive de la meme cle.
//   Domaine      : la mantisse de `fraction(cle_place_garee(graine, id, cycle 30 min))`. Le
//                  SEUIL reste celui du moteur : on ne recopie ni la densite ni la rampe
//                  (donc pas besoin de `presence_garee`/`rampe_garee` cote C++).
//   Hors domaine : gardes en amont (deja presente, plafond de 50, zone) → le moteur ne tire
//                  pas, l'etat est remis.
//   Qui d'AUTRE  : rien entre notre ecriture et la comparaison, a un niveau d'appel : aucune
//                  des quatre fonctions appelees avant le tirage (`14083b7d4`, `14204a3d8`,
//                  `142641c04`, `1406024f4`) ne figure parmi les 293 appelants de `14013bdb8`.
//                  Leurs propres appelees ne sont pas relues — non mesure, hypothese ; si l'une
//                  tirait, le journal montrerait une presence sans rapport avec `derive`.
// ⚠️ La RAMPE depend de l'effectif local (spec §2) : meme fraction ≠ meme verdict hors regime
// etabli. Et « id (entree+0x24) = parkingSpaceId » est non mesure — hypothese ; sonde S-L3d.
// ═══════════════════════════════════════════════════════════════════════════════════════════
using PorteGaree_t = void (*)(void*, std::uint32_t, float*, std::uint32_t);
PorteGaree_t o_PorteGaree = nullptr;
Debit g_debitGaree;

void D_PorteGaree(void* aSys, std::uint32_t aPlace, float* aEntree, std::uint32_t aProfil)
{
    ++g_c.garee.appels;
    const bool controle = g_inter.controle == 'P';
    const std::uint64_t t = HeureServeurMs();
    if (!controle && (!Actif() || t == 0))
    {
        ++g_c.garee.hors_cle;
        o_PorteGaree(aSys, aPlace, aEntree, aProfil);
        return;
    }
    const std::uint64_t graine = g_graine.load(std::memory_order_relaxed);
    const float f = fraction_garee(graine, aPlace, t, g_inter.controle);
    const bool gareeAvant = t_ctx.garee;
    const std::uint64_t cleAvant = t_ctx.cle_garee;
    t_ctx.garee = true;
    t_ctx.cle_garee = pop::cle_place_garee(graine, aPlace, pop::cycle_garee(t));
    TirageImpose tirage(mantisse_de(f));
    o_PorteGaree(aSys, aPlace, aEntree, aProfil);
    const bool tire = tirage.Fin();
    t_ctx.garee = gareeAvant;
    t_ctx.cle_garee = cleAvant;
    if (tire)
    {
        ++g_c.garee.derives;
        if (tirage.FractionVanilla() != f)
        {
            ++g_c.garee.changes;
        }
    }
    FOULE_LOG(g_debitGaree,
              "[foule] A''' place=%u profil=%u pos=(%.1f,%.1f,%.1f) vanilla=%.6f derive=%.6f "
              "(fractions) tire=%d cycle=%llu",
              aPlace, aProfil, aEntree[0], aEntree[1], aEntree[2], tirage.FractionVanilla(), f,
              tire ? 1 : 0, static_cast<unsigned long long>(pop::cycle_garee(t)));
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// H — `141cc9e00` (pointeur sur l'etat vehicule) → octet. Seul appelant : le tick vehicule
// `1402d377c`. Tirage INLINE `f × 0.9 < 0.3` : « ce vehicule reevalue-t-il son creneau a ce
// tick ». F-PNJ-208 (corrige), F-PNJ-221.
//   Cible        : la prochaine sortie du generateur du thread (voir `TirageImpose`).
//   Lecteurs     : 1 — la comparaison a 0.3, seul tirage de la fonction.
//   Alimente     : le verdict de reevaluation (puis un argmin deterministe, `1401991f0`).
//   Domaine      : la mantisse de `fraction(cle_prolongement(cle, ⌊t_serveur / 100 ms⌋), Trajet)`
//                  — la meme pendant tout le seau, donc les deux clients reevaluent aux memes
//                  instants serveur. Le moteur garde SA comparaison.
//   Hors domaine : gardes en amont (une dizaine de conditions sur l'etat et le membre) → pas
//                  de tirage, l'etat est remis.
//   Qui d'AUTRE  : `141cca120`, appelee avant le tirage, ne figure pas parmi les 293
//                  appelants de `14013bdb8` ; ses appelees ne sont pas relues — non mesure,
//                  hypothese (meme sonde que A''' : `derive` contre verdict rendu).
// ═══════════════════════════════════════════════════════════════════════════════════════════
using PorteReeval_t = std::uint64_t (*)(void**);
PorteReeval_t o_PorteReeval = nullptr;
Debit g_debitReeval;

std::uint64_t D_PorteReeval(void** aPEtat)
{
    ++g_c.reeval.appels;
    void* etat = aPEtat != nullptr ? *aPEtat : nullptr;
    const std::uint64_t membre = etat != nullptr ? MembreDeLEtat(etat) : 0;
    const std::uint64_t t = HeureServeurMs();
    const bool controle = g_inter.controle == 'H';
    Fiche fiche;
    const bool aCle = Actif() && t != 0 && membre != 0 && g_table.trouver(membre, fiche);
    if (!controle && !aCle)
    {
        ++g_c.reeval.hors_cle;
        return o_PorteReeval(aPEtat);
    }
    const float f = fraction_reevaluation(fiche.cle, t, g_inter.controle);
    TirageImpose tirage(mantisse_de(f));
    const std::uint64_t r = o_PorteReeval(aPEtat);
    if (tirage.Fin())
    {
        ++g_c.reeval.derives;
        if ((tirage.FractionVanilla() * 0.9f < 0.3f) != (f * 0.9f < 0.3f))
        {
            ++g_c.reeval.changes;
        }
        FOULE_LOG(g_debitReeval, "[foule] H vanilla=%.6f derive=%.6f (fractions, seuil 1/3) rendu=%u cle=%llx seau=%llu",
                  tirage.FractionVanilla(), f, static_cast<unsigned>(r & 0xff),
                  static_cast<unsigned long long>(fiche.cle), static_cast<unsigned long long>(t / 100));
    }
    return r;
}

// DESPAWN — `140887bb8` (mgr, entree de 0x18 o. dont [0] = membre simule, supprimerStub, raison).
// Neuf appelants, treize raisons (F-PNJ-225). Lecture seule : on retire la fiche AVANT que
// l'adresse ne soit rendue au moteur, qui la recycle.
using Despawn_t = void (*)(void*, std::uint64_t*, char, void*);
Despawn_t o_Despawn = nullptr;

void D_Despawn(void* aMgr, std::uint64_t* aEntree, char aSupprimerStub, void* aRaison)
{
    ++g_c.despawns;
    if (aEntree != nullptr && aEntree[0] != 0 && g_table.retirer(aEntree[0]))
    {
        ++g_c.despawnsConnus;
    }
    o_Despawn(aMgr, aEntree, aSupprimerStub, aRaison);
}

// ── Attache ────────────────────────────────────────────────────────────────────────────────
template <typename T> void Attacher1(const char* aNom, std::uint64_t aVa, T aDetour, T* aOriginal)
{
    ++g_hooksTotal;
    const bool ok = g_sdk->hooking->Attach(g_handle, reinterpret_cast<void*>(Adresse(aVa)),
                                           reinterpret_cast<void*>(aDetour),
                                           reinterpret_cast<void**>(aOriginal));
    if (ok)
    {
        ++g_hooksOk;
    }
    g_sdk->logger->InfoF(g_handle, "[foule] hook %-22s %llx : %s", aNom,
                         static_cast<unsigned long long>(aVa), ok ? "OK" : "ECHEC");
}

std::string Env(const char* aNom)
{
    char tampon[64] = {};
    const DWORD n = GetEnvironmentVariableA(aNom, tampon, sizeof(tampon));
    return (n > 0 && n < sizeof(tampon)) ? std::string(tampon) : std::string();
}

// ── T14 : un appel de la porte de teleportation ────────────────────────────────────────────
struct ResultatT14
{
    int code = 0; // 0 gestionnaire jamais vu, 1 entite absente de la table, 2 fait, -1 exception
    float avant[3] = {}, apres[3] = {};
    int etatEntite = -1;
    std::uint32_t membres = 0;
};

// Comme `1403bf538` (le moteur, a chaque tick) : verrou `mgr+0xf8`, table `mgr+0xd8` de
// `mgr+0xe4` entrees de 0x18 o., `[2]` = enveloppe (`+0x30` stub, `+0x40` id), puis la porte.
ResultatT14 TeleporterStub(std::uintptr_t aMgr, std::uint64_t aEntite, float aDx, float aDy) noexcept
{
    ResultatT14 r;
    if (aMgr == 0)
    {
        return r;
    }
    using Verrouiller_t = void (*)(std::uint8_t*);
    using Teleport_t = void (*)(std::uintptr_t, void*, const char*);
    struct Transformee
    {
        std::int32_t x, y, z, _;
        std::uint8_t rotation[16];
    };
    auto* verrou = reinterpret_cast<std::uint8_t*>(aMgr + 0xf8);
    __try
    {
        reinterpret_cast<Verrouiller_t>(Adresse(kVaVerrouiller))(verrou);
        r.code = 1;
        const auto* table = *reinterpret_cast<std::uint64_t**>(aMgr + 0xd8);
        r.membres = *reinterpret_cast<std::uint32_t*>(aMgr + 0xe4);
        for (std::uint32_t i = 0; table != nullptr && i < r.membres; ++i)
        {
            const std::uintptr_t enveloppe = table[i * 3ull + 2];
            if (enveloppe == 0 || *reinterpret_cast<std::uint64_t*>(enveloppe + 0x40) != aEntite)
            {
                continue;
            }
            const std::uintptr_t stub = *reinterpret_cast<std::uintptr_t*>(enveloppe + 0x30);
            if (stub == 0)
            {
                break;
            }
            const auto* pos = reinterpret_cast<float*>(stub + 0x80);
            r.avant[0] = pos[0];
            r.avant[1] = pos[1];
            r.avant[2] = pos[2];
            const std::uintptr_t ent = *reinterpret_cast<std::uintptr_t*>(stub + 0xa8);
            r.etatEntite = ent != 0 ? *reinterpret_cast<std::uint8_t*>(ent + 0x156) : -1;
            Transformee t{};
            t.x = pop::metres_vers_point_fixe(pos[0] + aDx);
            t.y = pop::metres_vers_point_fixe(pos[1] + aDy);
            t.z = pop::metres_vers_point_fixe(pos[2]);
            std::memcpy(t.rotation, reinterpret_cast<void*>(stub + 0x70), 16);
            reinterpret_cast<Teleport_t>(Adresse(kVaStubTeleport))(stub, &t, "Tessera_T14");
            r.apres[0] = pos[0];
            r.apres[1] = pos[1];
            r.apres[2] = pos[2];
            r.code = 2;
            break;
        }
        InterlockedExchange8(reinterpret_cast<char*>(verrou), 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedExchange8(reinterpret_cast<char*>(verrou), 0);
        r.code = -1;
    }
    return r;
}

void Ligne(std::string& aSortie, const char* aNom, const Compteur& aC)
{
    char t[128];
    std::snprintf(t, sizeof(t), " %s:%llu/%llu/%llu/%llu", aNom,
                  static_cast<unsigned long long>(aC.appels.load()),
                  static_cast<unsigned long long>(aC.derives.load()),
                  static_cast<unsigned long long>(aC.changes.load()),
                  static_cast<unsigned long long>(aC.hors_cle.load()));
    aSortie += t;
}
}  // namespace

namespace Tessera::Foule
{
void Attacher(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle)
{
    g_sdk = aSdk;
    g_handle = aHandle;
    g_inter = lire_interrupteurs(Env("TESSERA_FOULE_DERIVEE").c_str(), Env("TESSERA_FOULE_CONTROLE").c_str(),
                                 Env("TESSERA_FOULE_SCORE_PUR").c_str(), Env("TESSERA_FOULE_EMETTEUR").c_str(),
                                 Env("TESSERA_FOULE_JOURNAL").c_str());
    const std::string graine = Env("TESSERA_FOULE_GRAINE");
    if (!graine.empty())
    {
        g_graine.store(std::strtoull(graine.c_str(), nullptr, 0));
    }
    aSdk->logger->InfoF(aHandle,
                        "[foule] interrupteurs : derivee=%d controle=%c scorePur=%d emetteur=%s journal=%d "
                        "graine(env)=%llu — EFFET EN JEU NON MESURE",
                        g_inter.derivee ? 1 : 0, g_inter.controle != 0 ? g_inter.controle : '-',
                        g_inter.score_pur ? 1 : 0, g_inter.emetteur_creneau ? "creneau" : "fragment",
                        g_inter.journal ? 1 : 0, static_cast<unsigned long long>(g_graine.load()));

    Attacher1("entree", kVaEntree, &D_Entree, &o_Entree);
    Attacher1("A' naissance pieton", kVaNaissancePieton, &D_NaissancePieton, &o_NaissancePieton);
    Attacher1("A'' naissance vehic.", kVaNaissanceVehicule, &D_NaissanceVehicule, &o_NaissanceVehicule);
    Attacher1("B roue", kVaRoue, &D_Roue, &o_Roue);
    Attacher1("C apparence", kVaApparence, &D_Apparence, &o_Apparence);
    Attacher1("D vitesse", kVaVitesse, &D_Vitesse, &o_Vitesse);
    Attacher1("ctor etat commun", kVaCtorBase, &D_CtorBase, &o_CtorBase);
    Attacher1("E roue normalisee", kVaRoueNormalisee, &D_RoueNormalisee, &o_RoueNormalisee);
    Attacher1("filtre occupation", kVaFiltreOccupation, &D_FiltreOccupation, &o_FiltreOccupation);
    Attacher1("tick trajet pieton", kVaTickTrajetPieton, &D_TickTrajetPieton, &o_TickTrajetPieton);
    Attacher1("replan pieton", kVaReplanPieton, &D_ReplanPieton, &o_ReplanPieton);
    Attacher1("tick trajet vehicule", kVaTickTrajetVehicule, &D_TickTrajetVehicule, &o_TickTrajetVehicule);
    Attacher1("A''' porte garee", kVaPorteGaree, &D_PorteGaree, &o_PorteGaree);
    Attacher1("H porte reevaluation", kVaPorteReeval, &D_PorteReeval, &o_PorteReeval);
    Attacher1("G ctor vehicule", kVaCtorVehicule, &D_CtorVehicule, &o_CtorVehicule);
    Attacher1("G re-tirage", kVaReroll, &D_Reroll, &o_Reroll);
    Attacher1("G delai", kVaDelai, &D_Delai, &o_Delai);
    Attacher1("G conduite", kVaConduite, &D_Conduite, &o_Conduite);
    Attacher1("despawn", kVaDespawn, &D_Despawn, &o_Despawn);
    aSdk->logger->InfoF(aHandle, "[foule] %d hooks attaches sur %d", g_hooksOk, g_hooksTotal);
}

void PoserHeureServeur(std::uint64_t aServeurMs)
{
    if (aServeurMs == 0)
    {
        return;
    }
    g_decalageServeurMs.store(static_cast<std::int64_t>(aServeurMs) - LocalMs(), std::memory_order_relaxed);
    g_horlogePosee.store(true, std::memory_order_relaxed);
}

void RecevoirMondePartage(std::uint64_t aGraine, std::uint8_t aMode, std::uint64_t aEmpreinteServeur,
                          const std::uint64_t* aSuspendus, std::size_t aNombre)
{
    g_graine.store(aGraine);
    g_mode.store(aMode);
    g_empreinteServeur.store(aEmpreinteServeur);
    g_mondeRecu.store(true);
    {
        std::lock_guard<std::mutex> g(g_suspendusVerrou);
        g_suspendus.clear();
        for (std::size_t i = 0; aSuspendus != nullptr && i < aNombre; ++i)
        {
            g_suspendus.insert(aSuspendus[i]);
        }
    }
    if (g_sdk != nullptr)
    {
        g_sdk->logger->InfoF(g_handle, "[foule] MondePartage recu : graine=%llu mode=%u empreinte=%llx suspendus=%llu → %s",
                             static_cast<unsigned long long>(aGraine), aMode,
                             static_cast<unsigned long long>(aEmpreinteServeur),
                             static_cast<unsigned long long>(aNombre), Actif() ? "DERIVE" : "foule locale");
    }
}

void RecevoirRendu(const std::uint64_t* aCles, std::size_t aNombre)
{
    std::lock_guard<std::mutex> g(g_suspendusVerrou);
    for (std::size_t i = 0; aCles != nullptr && i < aNombre; ++i)
    {
        g_suspendus.erase(aCles[i]);
    }
}

std::string Etat()
{
    char t[512];
    std::size_t suspendus = 0;
    {
        std::lock_guard<std::mutex> g(g_suspendusVerrou);
        suspendus = g_suspendus.size();
    }
    std::snprintf(t, sizeof(t),
                  "mode=%u recu=%d graine=%llu actif=%d controle=%c scorePur=%d emetteur=%s hooks=%d/%d "
                  "heure=%llu table=%llu evictions=%llu suspendus=%llu naissances=%llu horsCle=%llu "
                  "(sansFragment=%llu sansBornes=%llu sansHorloge=%llu sansCandidat=%llu) despawns=%llu/%llu "
                  "v=[%.2f,%.2f]/[%.2f,%.2f] | decisions(appels/derives/changes/horsCle)",
                  g_mode.load(), g_mondeRecu.load() ? 1 : 0, static_cast<unsigned long long>(g_graine.load()),
                  Actif() ? 1 : 0, g_inter.controle != 0 ? g_inter.controle : '-', g_inter.score_pur ? 1 : 0,
                  g_inter.emetteur_creneau ? "creneau" : "fragment", g_hooksOk, g_hooksTotal,
                  static_cast<unsigned long long>(HeureServeurMs()),
                  static_cast<unsigned long long>(g_table.taille()),
                  static_cast<unsigned long long>(g_table.evictions()), static_cast<unsigned long long>(suspendus),
                  static_cast<unsigned long long>(g_c.naissancesCle.load()),
                  static_cast<unsigned long long>(g_c.naissancesHorsCle.load()),
                  static_cast<unsigned long long>(g_c.sansFragment.load()),
                  static_cast<unsigned long long>(g_c.sansBornes.load()),
                  static_cast<unsigned long long>(g_c.sansHorloge.load()),
                  static_cast<unsigned long long>(g_c.sansCandidat.load()),
                  static_cast<unsigned long long>(g_c.despawnsConnus.load()),
                  static_cast<unsigned long long>(g_c.despawns.load()), g_vMin[1].load(), g_vMax[1].load(),
                  g_vMin[2].load(), g_vMax[2].load());
    std::string s(t);
    Ligne(s, "entree", g_c.entree);
    Ligne(s, "A", g_c.abscisse);
    Ligne(s, "B", g_c.roue);
    Ligne(s, "C", g_c.apparence);
    Ligne(s, "D", g_c.vitesse);
    Ligne(s, "E", g_c.trajet);
    Ligne(s, "P", g_c.garee);
    Ligne(s, "H", g_c.reeval);
    Ligne(s, "Gdelai", g_c.delai);
    Ligne(s, "Gconduite", g_c.conduite);
    return s;
}

std::string Scores()
{
    std::string s;
    std::lock_guard<std::mutex> g(g_dernierVerrou);
    for (int famille = 1; famille <= 2; ++famille)
    {
        const DernierTrajet& d = g_dernierTrajet[famille];
        char t[256];
        std::snprintf(t, sizeof(t),
                      "%s famille=%d t=%llu cle=%llx membre=%llx source=%d tentative=%u candidats=%u "
                      "filtreOcc=%u/%u vanilla=%d derive=%d pur=%d scores=",
                      famille == 1 ? "" : " ||", famille, static_cast<unsigned long long>(d.t_ms),
                      static_cast<unsigned long long>(d.cle), static_cast<unsigned long long>(d.membre), d.source,
                      d.tentative, d.n, d.occAppels, d.occRend1, d.vanilla, d.derive, d.scorePur ? 1 : 0);
        s += t;
        for (std::uint32_t i = 0; i < d.n && i < 16; ++i)
        {
            std::snprintf(t, sizeof(t), "%u:%.6f,", d.voies[i], d.scores[i]);
            s += t;
        }
    }
    return s;
}

std::string StubTeleportSonde(std::uint64_t aEntite, float aDx, float aDy)
{
    const ResultatT14 r = TeleporterStub(g_mgrPietons.load(), aEntite, aDx, aDy);
    char t[256];
    std::snprintf(t, sizeof(t),
                  "code=%d (0 gestionnaire pieton jamais vu, 1 entite absente, 2 teleporte, -1 exception) "
                  "membres=%u ent156=%d avant=(%.3f,%.3f,%.3f) apres=(%.3f,%.3f,%.3f)",
                  r.code, r.membres, r.etatEntite, r.avant[0], r.avant[1], r.avant[2], r.apres[0], r.apres[1],
                  r.apres[2]);
    return std::string(t);
}
}  // namespace Tessera::Foule

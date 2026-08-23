#include "TesseraSpawnEnrichi.h"

#include "TesseraEsthetiqueV.h"
// ⚠️ `Main.h` AVANT `Utils.h` : ce dernier journalise par `SDK`/`PLUGIN`, que seul `Main.h`
// definit. L'inverse produit « SDK : identificateur non declare » DANS Utils.h — une erreur
// qui designe le mauvais fichier, et qu'on cherche donc au mauvais endroit.
#include "Main.h"
#include "Utils.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace Tessera::SpawnEnrichi
{
bool g_actif = false;

namespace
{
// ── LES CONSTANTES ADRESSEES AU BINAIRE — Cyberpunk 2077 2.31, et elle seule ─────────────────────
//
// Toutes viennent de mesures datees de la sonde `tools/re-probe`. Aucune n'est re-devinee ici.

/// La voie ENRICHIE du spawner du mode photo — celle qui accepte une charge de customisation.
/// `FUN_140662e08`, mesuree F-PLY-194.
constexpr std::uint64_t kRvaSpawnEnrichi = 0x662E08ull;

/// `DynamicBuffer::Reserve(tableau, capacite, tailleElement, alignement, rappel)` — F-PLY-267.
/// Identifiee par l'assertion de debug de CDPR elle-meme
/// (`redContainers/src/dynamicBuffer.cpp`, message nommant `capacity` et `elementSize`).
constexpr std::uint64_t kRvaReserve = 0x14B9D0ull;

/// Le spawner vit dans le systeme photomode, a cet offset (F-PLY-194).
constexpr std::size_t kOffsetSpawner = 0x390;

/// Taille de la requete de spawn, assemblee a la main.
constexpr std::size_t kTailleRequete = 0xF0;

/// Une paire de customisation : deux `u64`.
constexpr std::uint32_t kTaillePaire = 0x10;
constexpr std::uint32_t kAlignementPaire = 8;

using SpawnEnrichi_t = void* (*)(void* aSpawner, void* aSortie, void* aRequete, std::uint64_t aRecord,
                                 void* aCharge);
using Reserve_t = void (*)(void* aTableau, std::uint32_t aCapacite, std::uint32_t aTailleElement,
                           std::uint32_t aAlignement, void* aRappel);

/// Rayon d'enumeration pour retrouver le corps qu'on vient de fabriquer, en metres.
///
/// ⚠️ **Pose explicitement, jamais laisse a un defaut.** Le 2026-08-22, un rayon code en dur a 30 m
/// a rendu « aucune entite » sur une entite a exactement 30 m — et la conclusion tiree etait que le
/// spawn avait echoue. Un rayon d'enumeration est un instrument : un instrument trop court ne rend
/// pas « rien », il rend un FAUX NEGATIF.
constexpr float kRayonRecherche = 12.0f;

/// Resout un systeme du jeu par son nom RTTI.
///
/// ⚠️ **Pas de `CallStatic`, et c'est une lecon payee** (2026-08-15, systeme workspot) :
/// `CallStatic("ScriptGameInstance", "Get...")` a echoue avec CHAQUE type de sortie essaye, en
/// journalisant parfaitement ses ordres pendant que la couche visee n'etait jamais atteinte.
/// Resoudre par le TYPE dans le registre du moteur ne depend d'aucune signature de script.
RED4ext::IScriptable* SystemeParNom(const char* aNom)
{
    auto* rtti = RED4ext::CRTTISystem::Get();
    if (rtti == nullptr) { return nullptr; }
    auto* cls = rtti->GetClass(aNom);
    if (cls == nullptr) { return nullptr; }
    auto* engine = RED4ext::CGameEngine::Get();
    if (engine == nullptr || engine->framework == nullptr || engine->framework->gameInstance == nullptr)
    {
        return nullptr;
    }
    return engine->framework->gameInstance->GetSystem(cls);
}

/// Vrai si `aObjet` est bien de la classe `aClasse`.
///
/// ⚠️ On va calculer des adresses a offset fixe dans ces objets et appeler des fonctions natives.
/// Un objet inattendu ici, ce n'est pas une erreur : c'est un crash (F-PLY-225).
bool EstDeClasse(RED4ext::IScriptable* aObjet, const char* aClasse)
{
    if (aObjet == nullptr || aObjet->GetType() == nullptr) { return false; }
    const char* nom = aObjet->GetType()->name.ToString();
    return nom != nullptr && std::strcmp(nom, aClasse) == 0;
}

/// Les `EntityID` des corps portant `aRecord` autour du joueur.
///
/// ⭐ **C'est le seul chemin vers l'`EntityID` d'un corps enrichi**, et il n'est pas de confort : le
/// retour de la voie enrichie N'EST PAS un objet de script — le dereferencer a fait planter le jeu
/// le 2026-08-21 (F-PLY-225). On ne le touche donc jamais, et on retrouve le corps par le monde.
///
/// C'est la voie qu'emploie la sonde depuis toujours (`GetEntitiesAroundObject`, filtre par record),
/// et elle a deja enumere 200 corps enrichis a la fois (F-PLY-170) — donc elle les VOIT.
void EnumererCorps(std::uint64_t aRecord, std::vector<std::uint64_t>& aOut)
{
    aOut.clear();
    const auto joueur = Cyberverse::Utils::GetPlayer();
    if (joueur == nullptr) { return; }

    RED4ext::DynArray<RED4ext::Handle<RED4ext::GameObject>> autour;
    if (!Red::CallVirtual(joueur, "GetEntitiesAroundObject", autour, kRayonRecherche))
    {
        return;
    }
    // ⚠️ DEUX TYPES DE TABLEAU DANS CE MEME FICHIER, et ils n'ont pas la meme convention.
    // Celui du SDK RED4ext expose `size()` — une METHODE. Celui du MOTEUR, qu'on manipule par
    // offsets pour la charge de customisation, expose un CHAMP `size`. Les confondre donne une
    // erreur de conversion vers un pointeur de membre, qui ne ressemble pas du tout a sa cause.
    for (std::uint32_t i = 0; i < autour.size(); ++i)
    {
        auto& obj = autour[i];
        if (obj == nullptr) { continue; }
        RED4ext::TweakDBID rec{};
        if (!Red::CallVirtual(obj, "GetRecordID", rec)) { continue; }
        if (rec.value != aRecord) { continue; }
        aOut.push_back(obj->entityID.hash);
    }
}
}  // namespace

Resultat Tenter(std::uint64_t aRecord, const std::vector<std::uint8_t>& aBlob,
                const RED4ext::Vector4& aPosition)
{
    Resultat r;

    // ── GARDE 1 : L'INTERRUPTEUR ────────────────────────────────────────────────────────────────
    // Rien de ce qui suit ne s'execute tant qu'il est eteint. C'est le premier garde-fou decide
    // avec Lucas, et il rend l'A/B possible : un seul parametre change entre les deux moities.
    if (!g_actif)
    {
        return r;
    }
    // Sans fiche d'apparence, cette voie n'apporte rien que la voie sure ne fasse mieux.
    if (aBlob.empty())
    {
        return r;
    }
    r.tente = true;

    // ── GARDE 2 : LA CHARGE, VALIDEE AVANT TOUT ─────────────────────────────────────────────────
    // Frontiere de confiance : ces octets viennent du reseau, et ils vont servir a calculer des
    // adresses d'ecriture dans le tas du jeu.
    std::uint32_t nHead = 0, nBody = 0, nArms = 0;
    std::vector<std::uint64_t> paires;
    std::string pourquoi;
    if (!EsthetiqueV::Decoder(aBlob, nHead, nBody, nArms, paires, pourquoi))
    {
        r.diag = "charge REFUSEE : " + pourquoi;
        return r;
    }
    const std::uint32_t nInj = nHead + nBody + nArms;

    // ── LES DEUX OBJETS DU JEU ──────────────────────────────────────────────────────────────────
    auto* pms = SystemeParNom("gamePhotoModeSystem");
    auto* ccs = SystemeParNom("gameuiCharacterCustomizationSystem");
    if (pms == nullptr || ccs == nullptr)
    {
        char b[200];
        std::snprintf(b, sizeof(b), "systeme(s) INJOIGNABLE(S) — photomode=%s customisation=%s",
                      pms != nullptr ? "ok" : "nul", ccs != nullptr ? "ok" : "nul");
        r.diag = b;
        return r;
    }
    RED4ext::Handle<RED4ext::IScriptable> etatHandle;
    if (!Red::CallVirtual(ccs, "GetState", etatHandle) || etatHandle == nullptr)
    {
        r.diag = "l'etat de customisation est INJOIGNABLE (GetState)";
        return r;
    }
    auto* etat = etatHandle.GetPtr();
    if (!EstDeClasse(pms, "gamePhotoModeSystem") || !EstDeClasse(etat, "gameuiCharacterCustomizationState"))
    {
        r.diag = "types inattendus — appel NON tente";
        return r;
    }

    // ── GARDE 3 : LES GARDES DU SPAWNER, RELUES A L'INSTANT ─────────────────────────────────────
    // Jamais supposees depuis la mesure : un spawner desinitialise rendrait 0 en silence, et on
    // chercherait l'erreur dans la requete.
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto spawner = reinterpret_cast<std::uintptr_t>(pms) + kOffsetSpawner;
    if (!EsthetiqueV::Lisible(spawner + 0x50, 0x10))
    {
        r.diag = "spawner ILLISIBLE — appel non tente";
        return r;
    }
    if (*reinterpret_cast<std::uint64_t*>(spawner + 0x50) == 0
        || *reinterpret_cast<std::uint64_t*>(spawner + 0x58) == 0)
    {
        r.diag = "garde du spawner NULLE — appel non tente";
        return r;
    }

    // ── LA RECOLTE, PUIS LA RESERVE ─────────────────────────────────────────────────────────────
    //
    // On recolte d'abord sur le V LOCAL — non pas pour son contenu, qu'on va ecraser, mais pour
    // obtenir un tableau VIVANT, alloue par le moteur, avec sa duree de vie. Fournir notre propre
    // tampon serait parier que le moteur ne le libere pas, et ce pari se paie en crash DIFFERE.
    EsthetiqueV::DynArray charge{reinterpret_cast<void*>(base + EsthetiqueV::RvaSentinelle()), 0, 0};
    std::uint32_t parSection[3] = {0, 0, 0};
    EsthetiqueV::Recolter(etat, base, charge, parSection);
    const std::uint32_t recolte = charge.size;

    // ⭐ LA PARADE AU « MUR DE LA CAPACITE » (F-PLY-267).
    //
    // Sans elle, on ne pourrait injecter que ce qui tient dans la capacite recoltee sur le joueur
    // LOCAL — et un joueur masculin ne pourrait pas afficher une voisine (un V feminin porte des
    // paires `breast` qu'un V masculin n'a pas). Les producteurs reservent la valeur EXACTE, sans
    // marge : la capacite finit collee au nombre de paires recoltees. Compter sur du mou serait un
    // pari perdant.
    //
    // ⚠️ TROIS PRECAUTIONS, toutes lues dans le binaire :
    //   · `Reserve` agit des que la capacite demandee DIFFERE de la courante — donc il sait aussi
    //     RETRECIR. On ne l'appelle que pour agrandir.
    //   · Il REALLOUE : `entries` change, il faut le relire. Ecrire dans l'ancien pointeur, c'est
    //     ecrire dans un tampon libere.
    //   · Le 5e parametre est un rappel de deplacement/destruction ; les producteurs passent 0 pour
    //     ces paires POD, on passe 0 aussi.
    if (nInj > charge.capacity)
    {
        const auto reserver = reinterpret_cast<Reserve_t>(base + kRvaReserve);
        reserver(&charge, nInj, kTaillePaire, kAlignementPaire, nullptr);
        if (charge.capacity < nInj)
        {
            char b[220];
            std::snprintf(b, sizeof(b),
                          "Reserve N'A PAS mordu : %u demandees, capacite %u — appel NON tente",
                          nInj, charge.capacity);
            r.diag = b;
            return r;
        }
    }

    // ── L'INJECTION ─────────────────────────────────────────────────────────────────────────────
    // `entries` est relu MAINTENANT, apres la reserve : c'est tout l'objet de la precaution
    // ci-dessus, et l'oublier serait une ecriture dans de la memoire liberee.
    const auto ou = reinterpret_cast<std::uintptr_t>(charge.entries);
    if (!EsthetiqueV::Lisible(ou, static_cast<std::size_t>(nInj) * kTaillePaire))
    {
        r.diag = "le tableau n'est pas lisible sur toute la taille a injecter — appel NON tente";
        return r;
    }
    for (std::uint32_t i = 0; i < nInj; ++i)
    {
        *reinterpret_cast<std::uint64_t*>(ou + static_cast<std::size_t>(i) * kTaillePaire) = paires[i * 2];
        *reinterpret_cast<std::uint64_t*>(ou + static_cast<std::size_t>(i) * kTaillePaire + 8) =
            paires[i * 2 + 1];
    }
    charge.size = nInj;

    // ── LA REQUETE ──────────────────────────────────────────────────────────────────────────────
    // Zeroee d'abord : les champs qu'on ne renseigne pas doivent etre nuls et non remplis de pile
    // sale — c'est ce qui rend un echec interpretable.
    alignas(16) std::uint8_t requete[kTailleRequete] = {};
    auto* f32 = reinterpret_cast<float*>(&requete[0]);
    f32[0x10 / 4] = aPosition.X;
    f32[0x14 / 4] = aPosition.Y;
    f32[0x18 / 4] = aPosition.Z;
    // ⚠️ Rotation NEUTRE : (0,0,0,1). Un quaternion (0,0,0,0) serait DEGENERE, pas neutre — c'est
    // l'erreur que F-PLY-195 a corrigee.
    f32[0x20 / 4] = 0.f;
    f32[0x24 / 4] = 0.f;
    f32[0x28 / 4] = 0.f;
    f32[0x2C / 4] = 1.f;
    requete[0xE9] = 1;
    requete[0xEA] = 1;

    // ── L'ETAT DU MONDE AVANT L'APPEL ───────────────────────────────────────────────────────────
    // On photographie les corps de ce record AVANT, pour retrouver le neuf par difference. Sans
    // cette photo, un corps deja present serait pris pour celui qu'on vient de faire naitre.
    std::vector<std::uint64_t> avant;
    EnumererCorps(aRecord, avant);

    // ── L'APPEL ─────────────────────────────────────────────────────────────────────────────────
    const auto fn = reinterpret_cast<SpawnEnrichi_t>(base + kRvaSpawnEnrichi);
    void* sortie[2] = {nullptr, nullptr};
    fn(reinterpret_cast<void*>(spawner), &sortie[0], &requete[0], aRecord, &charge);
    r.appelFait = true;
    // ⚠️ `sortie[0]` N'EST PAS DEREFERENCE, ET CE N'EST PAS UNE PRECAUTION DE STYLE. Une version de
    // la sonde l'a traite comme un objet de script le 2026-08-21 : le type s'est lu `None` (un
    // `CName` de hachage nul) et le jeu est mort dans la foulee (F-PLY-225). « Lisible » n'est pas
    // « c'est un objet de la classe que je crois ».

    // ── RETROUVER LE CORPS ──────────────────────────────────────────────────────────────────────
    std::vector<std::uint64_t> apres;
    EnumererCorps(aRecord, apres);
    std::vector<std::uint64_t> neufs;
    for (const auto id : apres)
    {
        bool connu = false;
        for (const auto a : avant)
        {
            if (a == id) { connu = true; break; }
        }
        if (!connu) { neufs.push_back(id); }
    }

    char b[320];
    if (neufs.size() == 1)
    {
        r.entite = RED4ext::ent::EntityID{neufs[0]};
        std::snprintf(b, sizeof(b),
                      "corps enrichi NE — entite %llu · %u paire(s) (%u+%u+%u) injectee(s), "
                      "recolte %u, capacite %u · retour 0x%llX (non dereference)",
                      static_cast<unsigned long long>(neufs[0]), nInj, nHead, nBody, nArms, recolte,
                      charge.capacity, reinterpret_cast<unsigned long long>(sortie[0]));
    }
    else
    {
        // ⚠️ On ne DEVINE pas lequel. Rendre une entite au hasard ferait piloter le mauvais corps —
        // un defaut qui se chercherait tres loin d'ici. On rend la main a la voie sure.
        std::snprintf(b, sizeof(b),
                      "corps enrichi INTROUVABLE apres l'appel — %zu entite(s) neuve(s) de ce "
                      "record (attendu 1), %u avant / %u apres · retour 0x%llX. "
                      "Reprise par la voie sure.",
                      neufs.size(), static_cast<unsigned>(avant.size()),
                      static_cast<unsigned>(apres.size()),
                      reinterpret_cast<unsigned long long>(sortie[0]));
    }
    r.diag = b;
    return r;
}
}  // namespace Tessera::SpawnEnrichi

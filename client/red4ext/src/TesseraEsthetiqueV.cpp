#include "TesseraEsthetiqueV.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace Tessera::EsthetiqueV
{
namespace
{
// ── LES CONSTANTES ADRESSEES AU BINAIRE ──────────────────────────────────────────────────────────
//
// Toutes viennent de mesures datees de la sonde `tools/re-probe`, et sont recopiees ici sans etre
// re-devinees. Elles valent pour Cyberpunk 2077 **2.31** et pour elle seule.

/// (this, nomDeGroupe, isFPP, tableauDeSortie) -> 1 si le groupe est ABSENT
using Charge_t = std::uint8_t (*)(void*, std::uint64_t, std::uint8_t, void*);

/// Tampon « tableau vide » partage du jeu : le point de depart d'une recolte.
constexpr std::uint64_t kRvaSentinelle = 0x2AD9D50ull;

constexpr std::size_t kEnTete = 8;
constexpr std::uint32_t kMaxSection = 255;

/// Offset du drapeau de FINALISATION dans `gameuiCharacterCustomizationState` (F-PLY-160).
constexpr std::size_t kOffsetFinalise = 0x41;

/// LA selection de groupes, en un seul endroit.
///
/// ⚠️ Les groupes d'une section sont des ALTERNATIVES, pas des couches (F-PLY-197) : `TPP_Body` et
/// `FPP_Body` sont la meme chair vue de deux points de vue, les `holstered_*` sont des etats
/// mutuellement exclusifs du meme bras. Enumerer tout empile les variantes sur le squelette.
struct Groupe
{
    std::size_t conteneur;
    std::uint64_t rva;
    const char* nom;
    int section;
};

/// ⚠️⚠️ CETTE TABLE PORTE DEUX DEFAUTS CONNUS, audites le 2026-08-21 (F-PLY-226). Ils sont ici
/// parce que ce port est une COPIE FIDELE de la sonde, et les corriger d'un cote sans l'autre les
/// ferait diverger — c'est justement ce que la table centralisee evite. A traiter aux DEUX endroits.
///
/// DEFAUT 1 — trois paires partent EN DOUBLE. En section Arms, `character_customization` et
/// `holstered_default` partagent trois options sur quatre. 4+3 = 7 paires Arms, exactement le
/// compte mesure en F-PLY-172 : les doublons voyagent. Meme valeur deux fois, donc probablement
/// inoffensif — mais non mesure, et la sonde est gratuite (les retirer, comparer le rendu).
///
/// DEFAUT 2 — LES CYBERBRAS NE TRAVERSENT PAS, et rien ne le signale. `holstered_default` est code
/// en dur : un joueur aux bras mantis a son etat range dans `holstered_mantis`, que cette table ne
/// lit jamais. Sur l'ecran des autres, il a des bras ordinaires — ni erreur, ni journal. Chaque
/// variante (mantis, gorille, monofil, lance-projectiles) porte ses propres couleurs. Et
/// l'information « laquelle » n'est PAS dans cette table : elle est dans l'EQUIPEMENT du joueur
/// (F-PLY-226 et la mesure qui la suit), donc hors de portee de cette recolte telle qu'elle est.
constexpr Groupe kGroupes[] = {
    {0x70, 0x38503Cull, "character_customization", 0},   // Head
    {0x80, 0xB85240ull, "TPP_Body", 1},                  // Body — ce que voient les AUTRES
    {0x80, 0xB85240ull, "genitals", 1},
    {0x80, 0xB85240ull, "breast", 1},                    // rend 0 sur un V masculin : inoffensif
    {0x90, 0x1192F2Cull, "character_customization", 2},  // Arms
    {0x90, 0x1192F2Cull, "holstered_default", 2},        // bras au repos, sans cyberware degaine
};

/// Une adresse est-elle MAPPEE et lisible sur `aSize` octets ?
///
/// ⚠️⚠️ CE QUE CETTE GARDE NE DIT PAS, et ca a coute un crash du jeu le 2026-08-21 (F-PLY-225) :
/// elle ne dit RIEN de ce que les octets SIGNIFIENT. « Lisible » n'est pas « c'est un objet de la
/// classe que je crois ». Elle protege d'une lecture hors memoire, pas d'une interpretation fausse.
///
/// ⚠️ L'alignement sur 8 octets est une contrainte REELLE de cette garde, pas une precaution : elle
/// refuse toute adresse impaire. Interroger directement un drapeau a offset impair rend donc
/// TOUJOURS faux, et le refus est parfaitement credible (F-PLY-171). On interroge le mot ALIGNE qui
/// contient le drapeau, jamais le drapeau lui-meme.
}  // namespace  (fin de l'espace anonyme — ce qui suit est PARTAGE, voir l'en-tete)

bool Lisible(std::uintptr_t aPtr, std::size_t aSize)
{
    if (aPtr == 0 || (aPtr & 0x7) != 0)
    {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(aPtr), &mbi, sizeof(mbi)) == 0)
    {
        return false;
    }
    if (mbi.State != MEM_COMMIT)
    {
        return false;
    }
    constexpr DWORD kOk = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    if ((mbi.Protect & kOk) == 0)
    {
        return false;
    }
    const auto fin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return aPtr + aSize <= fin;
}

/// Recolte les six groupes dans `aCharge`, et rend le compte PAR SECTION.
void Recolter(void* aEtat, std::uintptr_t aBase, DynArray& aCharge, std::uint32_t aParSection[3])
{
    aParSection[0] = aParSection[1] = aParSection[2] = 0;
    for (const auto& g : kGroupes)
    {
        const auto avant = aCharge.size;
        reinterpret_cast<Charge_t>(aBase + g.rva)(aEtat, RED4ext::CName(g.nom).hash, 0u, &aCharge);
        aParSection[g.section] += aCharge.size - avant;
    }
}

char Chiffre(unsigned aV)
{
    return static_cast<char>(aV < 10 ? '0' + aV : 'a' + (aV - 10));
}

void EnHex(const std::uint8_t* aOctets, std::size_t aN, std::string& aOut)
{
    aOut.clear();
    aOut.reserve(aN * 2);
    for (std::size_t i = 0; i < aN; ++i)
    {
        aOut.push_back(Chiffre(static_cast<unsigned>(aOctets[i] >> 4)));
        aOut.push_back(Chiffre(static_cast<unsigned>(aOctets[i] & 0x0F)));
    }
}
std::uint64_t RvaSentinelle()
{
    return kRvaSentinelle;
}

bool Decoder(const std::vector<std::uint8_t>& aBlob, std::uint32_t& aHead, std::uint32_t& aBody,
             std::uint32_t& aArms, std::vector<std::uint64_t>& aPaires, std::string& aPourquoi)
{
    aPaires.clear();
    aHead = aBody = aArms = 0;

    if (aBlob.size() < kEnTete)
    {
        aPourquoi = "trop court pour porter l'en-tete TSV1";
        return false;
    }
    if (aBlob[0] != 'T' || aBlob[1] != 'S' || aBlob[2] != 'V' || aBlob[3] != '1')
    {
        aPourquoi = "magic absent (pas un descripteur TSV1)";
        return false;
    }
    aHead = aBlob[4];
    aBody = aBlob[5];
    aArms = aBlob[6];
    // L'octet reserve DOIT valoir 0. Un jour il portera un drapeau ; ce jour-la, un client qui
    // l'ignorait aurait applique une esthetique en croyant la comprendre.
    if (aBlob[7] != 0)
    {
        aPourquoi = "octet reserve non nul — descripteur d'une version qu'on ne sait pas lire";
        return false;
    }

    const std::uint32_t total = aHead + aBody + aArms;
    if (total == 0)
    {
        aPourquoi = "descripteur VIDE — aucune paire";
        return false;
    }
    if (total > kMaxSection * 3)
    {
        aPourquoi = "trop de paires (au-dela du plafond annonce)";
        return false;
    }
    // ⚠️ LA VERIFICATION QUI COMPTE : la taille annoncee par les compteurs doit correspondre EXACTEMENT
    // a la taille recue. Sans elle, un blob tronque ferait lire au-dela du vecteur — et ces octets
    // servent ensuite a calculer des adresses d'ecriture dans le tas du jeu.
    if (aBlob.size() != kEnTete + static_cast<std::size_t>(total) * 16)
    {
        aPourquoi = "taille incoherente avec les compteurs de l'en-tete";
        return false;
    }

    aPaires.reserve(static_cast<std::size_t>(total) * 2);
    for (std::uint32_t i = 0; i < total; ++i)
    {
        const std::uint8_t* ou = &aBlob[kEnTete + static_cast<std::size_t>(i) * 16];
        std::uint64_t a = 0, c = 0;
        for (int k = 7; k >= 0; --k) { a = (a << 8) | ou[k]; }
        for (int k = 7; k >= 0; --k) { c = (c << 8) | ou[8 + k]; }
        aPaires.push_back(a);
        aPaires.push_back(c);
    }
    return true;
}

bool Lire(RED4ext::IScriptable* aEtat, std::string& aHexOut, std::string& aErreurOut)
{
    aHexOut.clear();
    aErreurOut.clear();

    if (aEtat == nullptr)
    {
        aErreurOut = "etat nul";
        return false;
    }

    // ── GARDE DE TYPE, ET ELLE N'EST PAS DECORATIVE ──────────────────────────────────────────
    //
    // On va calculer une adresse a offset fixe dans cet objet et lui passer des fonctions natives.
    // Un objet inattendu ici n'est pas une erreur : c'est un crash.
    const char* classe = (aEtat->GetType() != nullptr) ? aEtat->GetType()->name.ToString() : nullptr;
    if (classe == nullptr || std::strcmp(classe, "gameuiCharacterCustomizationState") != 0)
    {
        aErreurOut = std::string("classe '") + ((classe != nullptr) ? classe : "<sans type>") +
                     "' — attendu gameuiCharacterCustomizationState";
        return false;
    }

    const auto adr = reinterpret_cast<std::uintptr_t>(aEtat);

    // ── LA FINALISATION, AVANT TOUTE LECTURE (F-PLY-160) ─────────────────────────────────────
    //
    // L'etat de customisation est PARFOIS non finalise — mesure a 0 puis a 1 aux memes arguments de
    // lancement. Refuser ici est le BON endroit : emettre un descripteur vide donnerait un avatar
    // sans visage chez les autres joueurs, avec un symptome tres loin de sa cause.
    //
    // ⚠️ On interroge `+0x40` (aligne) et non `+0x41` (impair) — voir le bandeau de `Lisible`.
    if (!Lisible(adr + 0x40, 8))
    {
        aErreurOut = "etat illisible autour du drapeau de finalisation (+0x40)";
        return false;
    }
    if (*reinterpret_cast<std::uint8_t*>(adr + kOffsetFinalise) == 0)
    {
        aErreurOut = "etat NON FINALISE — il n'y a rien a lire, et emettre un descripteur vide "
                     "donnerait un avatar sans visage chez les autres joueurs";
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    DynArray charge{reinterpret_cast<void*>(base + kRvaSentinelle), 0, 0};
    std::uint32_t parSection[3] = {0, 0, 0};
    Recolter(aEtat, base, charge, parSection);

    if (charge.size == 0)
    {
        aErreurOut = "charge VIDE malgre un etat finalise — anomalie, rien n'est emis";
        return false;
    }
    for (int i = 0; i < 3; ++i)
    {
        if (parSection[i] > kMaxSection)
        {
            aErreurOut = "une section depasse 255 paires — le format ne peut pas l'exprimer";
            return false;
        }
    }

    // ── ASSEMBLAGE DU BLOB, DANS L'ORDRE EXACT DU SERVEUR ────────────────────────────────────
    const std::size_t n = kEnTete + static_cast<std::size_t>(charge.size) * 16;
    std::vector<std::uint8_t> blob(n, 0);
    blob[0] = 'T';
    blob[1] = 'S';
    blob[2] = 'V';
    blob[3] = '1';
    blob[4] = static_cast<std::uint8_t>(parSection[0]);
    blob[5] = static_cast<std::uint8_t>(parSection[1]);
    blob[6] = static_cast<std::uint8_t>(parSection[2]);
    blob[7] = 0;  // reserve — trois compteurs aujourd'hui contre une migration de population demain

    const auto ou = reinterpret_cast<std::uintptr_t>(charge.entries);
    if (!Lisible(ou, static_cast<std::size_t>(charge.size) * 16))
    {
        aErreurOut = "tableau recolte illisible";
        return false;
    }
    for (std::uint32_t i = 0; i < charge.size; ++i)
    {
        const auto a = *reinterpret_cast<std::uint64_t*>(ou + static_cast<std::size_t>(i) * 16);
        const auto b = *reinterpret_cast<std::uint64_t*>(ou + static_cast<std::size_t>(i) * 16 + 8);
        std::uint8_t* d = &blob[kEnTete + static_cast<std::size_t>(i) * 16];
        for (int k = 0; k < 8; ++k)
        {
            d[k] = static_cast<std::uint8_t>((a >> (k * 8)) & 0xFF);
        }
        for (int k = 0; k < 8; ++k)
        {
            d[8 + k] = static_cast<std::uint8_t>((b >> (k * 8)) & 0xFF);
        }
    }

    EnHex(blob.data(), blob.size(), aHexOut);
    return true;
}
}  // namespace Tessera::EsthetiqueV

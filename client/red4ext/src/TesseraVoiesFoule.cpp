// Lecteur du SYSTEME DE VOIES de la foule — T0b / T0c du monde partage (F-PNJ-239, F-PNJ-244).
//
// `GameInstance.GetTrafficSystem()` rend l'interface de script `worldTrafficScriptInterface`, pas
// le systeme de voies : `+0x88`/`+0x90` y valent n'importe quoi (F-PNJ-239). La chaine lue dans le
// binaire 2.31 (mini-ghidra, 2026-09-30) part d'un systeme de jeu ordinaire :
//
//   gameCommunitySystem (GetSystem)                  1401b5f40 : rcx = [X+0xd0] -> 140417348
//   +0xd0  -> CrowdSystem                            140417348 : tableau en ligne +0x640, compte +0x650
//   +0x640 -> gestionnaire PIETONS (+0x648 vehicules) 140be2e6c "CrowdSystem_UpdateStubCtrls_Pedestrian"
//   +0x140 -> laneSys                                140413794 -> 140413fa8 / 1402f5a2c ; 1404066c8
//   laneSys+0x00 : HashMap cle-de-voie -> voie        1404067e4 (index +0, taille +8, capacite +0xc,
//                                                     noeuds +0x10, pas +0x1c ; noeud +0 suivant,
//                                                     +4 hash, +8 cle 16 o., +0x18 -> voie)
//   voie+0x60    : DynArray de fragments de 0x40 o.   14040689c (+0x38/+0x3c abscisse, +0x34 indice)
//   laneSys+0x50 -> +0x30 : table des profils, 0x128 o. par profil (F-PNJ-186)
//   laneSys+0x88 : indice de phase ; laneSys+0x90 : compteur de rotation des zones (F-PNJ-192)
//
// ⚠️ LECTURE SEULE, et chaque pointeur est verifie (`Lisible`) avant d'etre suivi : chaine VIDE si
// un maillon manque — jamais des zeros plausibles. Les decalages sont ceux du binaire 2.31 ; une
// montee de version les invalide (ADR 0001).
#include "NetworkGameSystem.h"
#include "TesseraEsthetiqueV.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
using Tessera::EsthetiqueV::Lisible;

bool LirePtr(std::uintptr_t aOu, std::uintptr_t& aSortie)
{
    if (!Lisible(aOu, 8))
    {
        return false;
    }
    aSortie = *reinterpret_cast<const std::uintptr_t*>(aOu);
    return aSortie != 0;
}

template <typename T> bool Lire(std::uintptr_t aOu, T& aSortie)
{
    if (!Lisible(aOu, sizeof(T)))
    {
        return false;
    }
    std::memcpy(&aSortie, reinterpret_cast<const void*>(aOu), sizeof(T));
    return true;
}

// Octet d'un global du binaire, adresse donnee dans la base de Ghidra (0x140000000).
int OctetGlobal(std::uint64_t aVaGhidra)
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto ou = base + static_cast<std::uintptr_t>(aVaGhidra - 0x140000000ull);
    std::uint8_t v = 0;
    // `Lisible` exige l'alignement de la taille lue : un octet n'en demande aucun.
    return Lire(ou, v) ? v : -1;
}
}  // namespace

Red::CString NetworkGameSystem::Tessera_LireVoiesFoule(uint64_t aCleVoie, int32_t aMax) const
{
    auto* rtti = RED4ext::CRTTISystem::Get();
    auto* engine = RED4ext::CGameEngine::Get();
    if (rtti == nullptr || engine == nullptr || engine->framework == nullptr
        || engine->framework->gameInstance == nullptr)
    {
        return Red::CString("");
    }
    auto* cls = rtti->GetClass("gameCommunitySystem");
    auto* communaute = cls ? engine->framework->gameInstance->GetSystem(cls) : nullptr;
    if (communaute == nullptr || communaute->GetType() != cls)
    {
        return Red::CString("");
    }

    std::uintptr_t crowd = 0, pietons = 0, vehicules = 0, lanes = 0, lanesVeh = 0;
    std::uint32_t nbGestionnaires = 0;
    if (!LirePtr(reinterpret_cast<std::uintptr_t>(communaute) + 0xd0, crowd)
        || !Lire(crowd + 0x650, nbGestionnaires) || nbGestionnaires == 0 || nbGestionnaires > 8
        || !LirePtr(crowd + 0x640, pietons) || !LirePtr(pietons + 0x140, lanes))
    {
        return Red::CString("");
    }
    if (nbGestionnaires >= 2 && LirePtr(crowd + 0x648, vehicules))
    {
        LirePtr(vehicules + 0x140, lanesVeh);
    }

    std::uint32_t phase = 0, compteur = 0, taille = 0, capacite = 0, pas = 0;
    std::uintptr_t index = 0, noeuds = 0, holder = 0, profils = 0;
    std::uint32_t apresProfils[4] = {};
    if (!Lire(lanes + 0x88, phase) || !Lire(lanes + 0x90, compteur) || !Lire(lanes + 0x8, taille)
        || !Lire(lanes + 0xc, capacite) || !Lire(lanes + 0x1c, pas) || !LirePtr(lanes, index)
        || !LirePtr(lanes + 0x10, noeuds))
    {
        return Red::CString("");
    }
    if (LirePtr(lanes + 0x50, holder) && LirePtr(holder + 0x30, profils))
    {
        Lire(holder + 0x38, apresProfils);
    }

    char tampon[256];
    std::snprintf(tampon, sizeof(tampon),
                  "phase=%u compteur=%u voies=%u cap=%u pas=%u gest=%u memeLaneSys=%d "
                  "profils+38=%u/%u/%u/%u portes=%d/%d/%d |",
                  phase, compteur, taille, capacite, pas, nbGestionnaires,
                  lanesVeh == 0 ? -1 : (lanesVeh == lanes ? 1 : 0), apresProfils[0],
                  apresProfils[1], apresProfils[2], apresProfils[3], OctetGlobal(0x1432fe598),
                  OctetGlobal(0x1432fe560), OctetGlobal(0x1432fe608));
    std::string sortie(tampon);
    if (aMax <= 0 || pas < 0x20 || pas > 0x100 || capacite == 0 || capacite > (1u << 22))
    {
        return Red::CString(sortie.c_str());
    }

    // Parcours par les SEAUX, pas par le tableau de noeuds : un noeud libre n'y est pas chaine.
    int32_t ecrits = 0;
    std::uint32_t vus = 0;
    for (std::uint32_t s = 0; s < capacite && ecrits < aMax; ++s)
    {
        std::uint32_t i = 0;
        if (!Lire(index + s * 4ull, i))
        {
            return Red::CString("");
        }
        for (std::uint32_t garde = 0; i != 0xFFFFFFFFu && garde < 4096 && ecrits < aMax; ++garde)
        {
            const auto noeud = noeuds + static_cast<std::uintptr_t>(i) * pas;
            std::uint32_t suivant = 0;
            std::uint64_t cle = 0;
            std::uint16_t a = 0, b = 0;
            std::uint8_t c = 0;
            std::uintptr_t voie = 0;
            if (!Lire(noeud, suivant) || !Lire(noeud + 8, cle) || !Lire(noeud + 0x10, a)
                || !Lire(noeud + 0x12, b) || !Lire(noeud + 0x14, c) || !LirePtr(noeud + 0x18, voie))
            {
                return Red::CString("");
            }
            ++vus;
            if (aCleVoie == 0 || cle == aCleVoie)
            {
                std::uintptr_t frags = 0;
                std::uint32_t nb = 0;
                std::snprintf(tampon, sizeof(tampon), " %llx/%u/%u/%u:", static_cast<unsigned long long>(cle),
                              a, b, c);
                sortie += tampon;
                if (Lire(voie + 0x60 + 0xc, nb) && nb > 0 && nb < 256 && LirePtr(voie + 0x60, frags))
                {
                    for (std::uint32_t f = 0; f < nb; ++f)
                    {
                        float d = 0, e = 0;
                        std::uint32_t idx = 0;
                        const auto fr = frags + f * 0x40ull;
                        if (!Lire(fr + 0x34, idx) || !Lire(fr + 0x38, d) || !Lire(fr + 0x3c, e))
                        {
                            break;
                        }
                        std::snprintf(tampon, sizeof(tampon), "%.1f-%.1f=%u,", d, e, idx);
                        sortie += tampon;
                    }
                }
                ++ecrits;
            }
            i = suivant;
        }
    }
    std::snprintf(tampon, sizeof(tampon), " | parcourus=%u", vus);
    sortie += tampon;
    return Red::CString(sortie.c_str());
}

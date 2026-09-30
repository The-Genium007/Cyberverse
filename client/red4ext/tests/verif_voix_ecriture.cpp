// =====================================================================================
// Auto-verification de l'ECRIVAIN de la memoire partagee voix (V4.2). Meme patron que
// `verif_voix_partage.cpp` et `verif_tampon_interpolation.cpp` : aucun framework, aucun
// type moteur.
//
// Ce que ce fichier PROUVE : `EcrivainVoixPartagee` cree reellement une section nommee
// Windows (`Local\TesseraVoix-<pid>`), y ecrit avec le protocole seqlock, et un LECTEUR
// INDEPENDANT (ecrit ici a la main, au meme protocole que `LecteurMemoirePartagee` cote
// Rust — PAS le meme code, pour que ce test ne puisse pas se mentir a lui-meme) retrouve
// exactement ce qui a ete ecrit, y compris apres plusieurs ecritures successives (un
// "fichu-a-jour", pas un instantane).
//
// Ce que ce fichier NE PROUVE PAS : qu'un launcher REEL (le code Rust) lit correctement
// cette section — c'est `le_lecteur_retrouve_ce_que_l_ecrivain_a_ecrit` (Rust, deja vert
// le 2026-09-29) qui couvre ce cote-la, contre un ecrivain de TEST. La preuve complete
// (ecrivain C++ REEL <-> lecteur Rust REEL) exige de lancer les deux processus ensemble —
// c'est le sondage en jeu du bloc VOIX.
//
// AVANT ce fichier (2026-09-30) : aucune fonction du depot ne creait la section nommee —
// seule la structure existait (`verif_voix_partage.cpp`). Ce test etait donc ROUGE par
// construction (rien a inclure, `EcrivainVoixPartagee` n'existait pas) avant l'ecriture de
// `VoixEcriture.h`.
//
// Compilation et execution (BuildTools 18) :
//   cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\
//            Build\vcvars64.bat" && cl /std:c++20 /EHsc /W4 /nologo
//            client\red4ext\tests\verif_voix_ecriture.cpp /Fe:verif_voix_ecriture.exe && verif_voix_ecriture.exe'
// =====================================================================================

#include "../src/PlayerSync/VoixEcriture.h"

#include <cstdio>
#include <cstring>
#include <Windows.h>

using namespace Tessera::Voix;

static int g_echecs = 0;
static int g_verifs = 0;

static void Verifier(bool condition, const char* quoi)
{
    ++g_verifs;
    if (!condition)
    {
        ++g_echecs;
        std::printf("  ECHEC : %s\n", quoi);
    }
}

// Lecteur INDEPENDANT, minimal, ecrit a la main : ouvre une section EXISTANTE et applique
// le protocole seqlock documente dans VoixPartage.h. Volontairement une implementation
// SEPAREE de EcrivainVoixPartagee::Ecrire, pour qu'un bug symetrique des deux cotes ne se
// masque pas lui-meme.
static bool LireSection(const char* nom, EtatVoixPartage& dehors)
{
    HANDLE handle = ::OpenFileMappingA(FILE_MAP_READ, FALSE, nom);
    if (handle == nullptr)
    {
        return false;
    }
    const auto* vue = static_cast<const EtatVoixPartage*>(
        ::MapViewOfFile(handle, FILE_MAP_READ, 0, 0, sizeof(EtatVoixPartage)));
    if (vue == nullptr)
    {
        ::CloseHandle(handle);
        return false;
    }
    bool ok = false;
    for (int tentative = 0; tentative < 8; ++tentative)
    {
        const std::uint32_t avant = vue->compteurEcriture;
        if (avant % 2 != 0)
        {
            continue; // ecriture en cours
        }
        EtatVoixPartage copie = *vue;
        const std::uint32_t apres = vue->compteurEcriture;
        if (avant != apres)
        {
            continue; // dechiree
        }
        dehors = copie;
        ok = true;
        break;
    }
    ::UnmapViewOfFile(vue);
    ::CloseHandle(handle);
    return ok;
}

int main()
{
    const DWORD pid = ::GetCurrentProcessId();
    char nom[64];
    std::snprintf(nom, sizeof(nom), "Local\\TesseraVoix-%lu", static_cast<unsigned long>(pid));

    // --- Round-trip simple ---
    EcrivainVoixPartagee ecrivain;
    Verifier(ecrivain.AssurerOuverte(), "AssurerOuverte() cree la section");

    EtatVoixPartage envoye{};
    envoye.monCid = 424242;
    envoye.vAppuyee = 1;
    envoye.auditeurPositionX = 12.5f;
    envoye.auditeurPositionY = -3.0f;
    envoye.auditeurPositionZ = 40.0f;
    envoye.nombreAvatars = 2;
    envoye.avatars[0] = AvatarPartage{7, 1.0f, 2.0f, 3.0f};
    envoye.avatars[1] = AvatarPartage{(std::uint64_t{1} << 48) + 9, 4.0f, 5.0f, 6.0f};
    ecrivain.Ecrire(envoye);

    EtatVoixPartage recu{};
    const bool lu = LireSection(nom, recu);
    Verifier(lu, "la section existe et se lit depuis un lecteur INDEPENDANT");
    if (lu)
    {
        Verifier(recu.version == kVersionStructure, "version publiee correcte");
        Verifier(recu.compteurEcriture % 2 == 0, "compteur publie PAIR (stable)");
        Verifier(recu.monCid == 424242, "monCid retrouve");
        Verifier(recu.vAppuyee == 1, "vAppuyee retrouve");
        Verifier(recu.auditeurPositionX == 12.5f, "position X retrouvee");
        Verifier(recu.auditeurPositionZ == 40.0f, "position Z retrouvee");
        Verifier(recu.nombreAvatars == 2, "nombreAvatars retrouve");
        Verifier(recu.avatars[1].cid == (std::uint64_t{1} << 48) + 9,
                  "cid PNJ (>= 1<<48) d'un avatar ne se tronque pas");
    }

    // --- Une seconde ecriture change ce que le lecteur voit : fichu-a-jour, pas figee ---
    EtatVoixPartage second{};
    second.monCid = 999;
    second.vAppuyee = 0;
    second.nombreAvatars = 0;
    ecrivain.Ecrire(second);

    EtatVoixPartage recuDeuxieme{};
    const bool luDeuxieme = LireSection(nom, recuDeuxieme);
    Verifier(luDeuxieme, "seconde lecture reussie");
    if (luDeuxieme)
    {
        Verifier(recuDeuxieme.monCid == 999, "seconde ecriture visible (pas un instantane fige)");
        Verifier(recuDeuxieme.vAppuyee == 0, "vAppuyee relache visible");
    }

    // --- Le compteur ne cesse jamais d'avancer entre deux ecritures ---
    Verifier(recuDeuxieme.compteurEcriture > recu.compteurEcriture,
              "le compteur d'ecriture avance a chaque Ecrire()");

    std::printf("%d verifications, %d echecs\n", g_verifs, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

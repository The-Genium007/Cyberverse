// =====================================================================================
// Auto-vérification de la structure partagée voix (V4.1). AUCUN framework, AUCUN type
// moteur — même patron que `verif_tampon_interpolation.cpp`.
//
// Ce que ce fichier prouve, et ce qu'il NE prouve PAS :
//   - PROUVE : la structure compile, sa disposition est EXACTEMENT celle documentée dans
//     `VoixPartage.h` (les `static_assert` du header font déjà l'essentiel — ce fichier
//     est une seconde preuve, par la valeur réellement observée à l'exécution, pas
//     seulement à la compilation), le seqlock (compteurEcriture) se comporte comme prévu
//     sur un cas simulé, et un cid PNJ (>= 1 << 48) survit sans troncature.
//   - NE PROUVE PAS : que la mémoire partagée nommée fonctionne entre deux VRAIS
//     processus, que le jeu écrit quoi que ce soit dedans, ni qu'un launcher la lit —
//     tout ça exige une session (VM3/VM4), hors de cette passe.
//
// N'est PAS dans `src/CMakeLists.txt` : ce fichier porte un `main()`, comme son voisin.
//
// Compilation et exécution (BuildTools 18) :
//   cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\
//            Build\vcvars64.bat" && cl /std:c++20 /EHsc /W4 /nologo
//            client\red4ext\tests\verif_voix_partage.cpp /Fe:verif_voix.exe && verif_voix.exe'
// =====================================================================================

#include "../src/PlayerSync/VoixPartage.h"

#include <cstdio>
#include <cstring>

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

// --- Le seqlock, simulé sans thread (un seul processus, mais le protocole est le même :
// écrire encadré par deux incréments, lire encadré par deux lectures du compteur). ---
static void VerifierSeqlock()
{
    EtatVoixPartage etat;
    Verifier(etat.compteurEcriture == 0, "compteur initial pair (0)");

    // Une "écriture" : impair pendant, pair après.
    etat.compteurEcriture++;
    Verifier(etat.compteurEcriture % 2 == 1, "compteur impair PENDANT l'écriture");
    etat.monCid = 777;
    etat.compteurEcriture++;
    Verifier(etat.compteurEcriture % 2 == 0, "compteur pair APRÈS l'écriture");
    Verifier(etat.compteurEcriture == 2, "deux écritures complètes -> compteur à 2");

    // Une "lecture déchirée" détectée : si on avait lu le compteur pendant l'écriture
    // (valeur 1, impaire), le protocole dit RELIRE. Ici on vérifie juste la règle de
    // décision elle-même, séparément de tout fil d'exécution réel.
    const std::uint32_t compteurVuPendant = 1;
    Verifier((compteurVuPendant % 2) != 0, "un compteur impair signale une lecture à rejeter");
}

// --- Le champ V (vAppuyee) : 0 -> 1 -> 0, exactement la séquence attendue par le plan
// pour la preuve VM3 (« V 0 -> 1 -> 0 »), simulée ici sans jeu. ---
static void VerifierSequenceV()
{
    EtatVoixPartage etat;
    Verifier(etat.vAppuyee == 0, "V au repos par défaut");
    etat.vAppuyee = 1;
    Verifier(etat.vAppuyee == 1, "V appuyée");
    etat.vAppuyee = 0;
    Verifier(etat.vAppuyee == 0, "V relâchée");
}

// --- Un cid PNJ (>= 1 << 48, cf. `world.rs:NPC_ID_RANGE_START` côté serveur Tessera) ne
// doit RIEN perdre : c'est exactement la classe de valeur qu'un `uint32_t` aurait
// tronquée — la même correction que `tessera-core/voix/src/paquet.rs` (dépôt Tessera,
// commit du 2026-09-29). ---
static void VerifierCidPnj()
{
    EtatVoixPartage etat;
    const std::uint64_t cidPnj = (std::uint64_t{1} << 48) + 42;
    etat.monCid = cidPnj;
    Verifier(etat.monCid == cidPnj, "monCid porte un cid PNJ (>= 1<<48) sans troncature");

    etat.avatars[0].cid = cidPnj;
    etat.nombreAvatars = 1;
    Verifier(etat.avatars[0].cid == cidPnj, "un avatar PNJ dans le tableau ne perd rien non plus");
}

// --- La disposition observée à l'exécution, pas seulement à la compilation. ---
static void VerifierDisposition()
{
    Verifier(sizeof(AvatarPartage) == 20, "sizeof(AvatarPartage) == 20 (à l'exécution)");
    Verifier(sizeof(EtatVoixPartage) == 1332, "sizeof(EtatVoixPartage) == 1332 (à l'exécution)");
    Verifier(kAvatarsMax == 64, "kAvatarsMax == 64");
    Verifier(kVersionStructure == 1, "kVersionStructure == 1");

    // Offset du premier avatar : doit être exactement APRÈS l'en-tête (52 octets), sans le
    // moindre octet de bourrage — c'est `pack(1)` qui le garantit, vérifié ici plutôt que
    // supposé.
    EtatVoixPartage etat;
    const auto* base = reinterpret_cast<const unsigned char*>(&etat);
    const auto* premierAvatar = reinterpret_cast<const unsigned char*>(&etat.avatars[0]);
    const std::ptrdiff_t offsetAvatars = premierAvatar - base;
    Verifier(offsetAvatars == 52, "l'en-tête fait exactement 52 octets avant le tableau d'avatars");
}

int main()
{
    VerifierSeqlock();
    VerifierSequenceV();
    VerifierCidPnj();
    VerifierDisposition();

    std::printf("%d verifications, %d echecs\n", g_verifs, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

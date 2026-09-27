// =====================================================================================
// Auto-verification de `JugerIdentiteStatique` (retours du playtest 2, R9/R10).
// Meme forme que `verif_tampon_interpolation.cpp` : aucun framework, aucun type moteur.
//
//   cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\
//            Build\vcvars64.bat" && cl /std:c++20 /EHsc /W4 /nologo
//            client\red4ext\tests\verif_identite_statique.cpp /Fe:verif_id.exe && verif_id.exe'
// =====================================================================================

#include "../src/PlayerSync/IdentiteStatique.h"

#include <cmath>
#include <cstdio>

using namespace Tessera::Sync;

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

int main()
{
    constexpr std::uint64_t kCorporatMa = 0x1111;
    constexpr std::uint64_t kQueenWa = 0x2222;

    // Le cas du journal du 2026-09-25 : meme identifiant, AUTRE personnage.
    Verifier(JugerIdentiteStatique(true, kQueenWa, kCorporatMa, 0.0f, 0.0f, 0.0f)
                 == IdentiteStatique::AutreRecord,
             "un autre record ne se rhabille pas, meme a la meme place");

    Verifier(JugerIdentiteStatique(true, kCorporatMa, kCorporatMa, 0.5f, 0.2f, 0.0f)
                 == IdentiteStatique::Meme,
             "meme record, a 50 cm : c'est lui");

    Verifier(JugerIdentiteStatique(true, kCorporatMa, kCorporatMa, 12.0f, 0.0f, 0.0f)
                 == IdentiteStatique::AutreEndroit,
             "meme record a 12 m : un autre exemplaire");

    Verifier(JugerIdentiteStatique(true, 0, kCorporatMa, 1.0f, 0.0f, 0.0f)
                 == IdentiteStatique::Meme,
             "record non rapporte : la position seule tranche");

    Verifier(JugerIdentiteStatique(true, kCorporatMa, kCorporatMa, NAN, 0.0f, 0.0f)
                 == IdentiteStatique::AutreEndroit,
             "une position illisible refuse");

    Verifier(JugerIdentiteStatique(false, kCorporatMa, kCorporatMa, 0.0f, 0.0f, 0.0f)
                 == IdentiteStatique::SansReference,
             "sans reference, on le dit");

    std::printf("%d verifications, %d echec(s)\n", g_verifs, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

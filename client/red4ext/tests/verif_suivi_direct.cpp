// =====================================================================================
// Auto-verification du suivi direct et de la garde de marche plantee. AUCUN framework,
// AUCUN type moteur (voir src/PlayerSync/SuiviDirect.h).
//
// Compilation et execution (BuildTools 18) :
//   cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\
//            Build\vcvars64.bat" && cl /std:c++20 /EHsc /W4 /nologo
//            client\red4ext\tests\verif_suivi_direct.cpp /Fe:verif_suivi.exe && verif_suivi.exe'
// =====================================================================================

#include "../src/PlayerSync/SuiviDirect.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

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

static float Horiz(const PositionSuivi& a, const PositionSuivi& b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

static void Pente()
{
    // 3,9 m de montee sur 12,9 m a 1,5 m/s, 90 i/s, corps 3 m sous la cible.
    const float dt = 1.0f / 90.0f;
    const float v = 1.5f;
    const float longueur = 12.9f, montee = 3.9f;
    PositionSuivi corps{0.0f, 0.0f, -3.0f};
    float dzMax = 0.0f, dzPremiere = 99.0f;
    int n = 0;
    for (float d = 0.0f; d < longueur; d += v * dt, ++n)
    {
        const PositionSuivi cible{d, 0.0f, montee * d / longueur};
        corps = PasSuiviDirect(corps, cible, 0.15f);
        const float dz = std::fabs(corps.z - cible.z);
        if (n == 0) dzPremiere = dz;
        if (dz > dzMax) dzMax = dz;
    }
    Verifier(dzPremiere == 0.0f, "pente : |dz| = 0 apres 1 image");
    Verifier(dzMax < 0.01f, "pente : |dz| < 0,01 m sur tout le trajet");
}

static void SprintDemiTours(float images)
{
    const float dt = 1.0f / images;
    const float v = 5.2f;
    PositionSuivi corps{0.0f, 0.0f, 0.0f};
    float ecartMax = 0.0f;
    float sens = 1.0f, x = 0.0f;
    for (int trajet = 0; trajet < 12; ++trajet) // 12 trajets = 6 allers-retours
    {
        while (sens > 0 ? x < 10.0f : x > 0.0f)
        {
            x += sens * v * dt;
            const PositionSuivi cible{x, 0.0f, 0.0f};
            corps = PasSuiviDirect(corps, cible, 0.15f);
            const float e = Horiz(corps, cible);
            if (e > ecartMax) ecartMax = e;
        }
        sens = -sens; // demi-tour instantane
    }
    std::printf("  sprint %.0f i/s : ecart max %.3f m\n", images, ecartMax);
    Verifier(ecartMax < 1.5f, "sprint demi-tours secs : ecart max < 1,5 m");
    Verifier(ecartMax <= 3.0f, "sprint demi-tours secs : jamais > 3 m (aucun palier)");
}

static void RegimeEtabli()
{
    const float dt = 1.0f / 90.0f;
    PositionSuivi corps{0.0f, 0.0f, 0.0f};
    float x = 0.0f, ecartMax = 0.0f;
    for (int i = 0; i < 900; ++i)
    {
        x += 5.2f * dt;
        const PositionSuivi cible{x, 0.0f, 0.0f};
        corps = PasSuiviDirect(corps, cible, 0.15f);
        if (i > 90) ecartMax = std::fmax(ecartMax, Horiz(corps, cible));
    }
    std::printf("  regime etabli 90 i/s : ecart %.3f m\n", ecartMax);
    Verifier(ecartMax < 0.5f, "regime etabli en ligne droite : ecart < 0,5 m a 90 i/s");
}

static void CorpsPoseLoin()
{
    const PositionSuivi cible{5.88f, 0.0f, 0.0f};
    const PositionSuivi corps = PasSuiviDirect({0.0f, 0.0f, 0.0f}, cible, 0.15f);
    const float e = Horiz(corps, cible);
    Verifier(e <= 1.5f + 1e-4f, "corps a 5,88 m : ramene a <= 1,5 m en une image");
    Verifier(corps.x <= cible.x, "corps a 5,88 m : ne depasse pas la cible");
    // Plafond inactif : fraction simple.
    const PositionSuivi proche = PasSuiviDirect({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0.15f);
    Verifier(std::fabs(proche.x - 0.15f) < 1e-5f, "ecart < plafond : fraction simple");
    // NaN / inf : position lue inchangee.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const PositionSuivi lue{1.0f, 2.0f, 3.0f};
    const PositionSuivi a = PasSuiviDirect(lue, {nan, 0.0f, 0.0f}, 0.15f);
    const PositionSuivi b = PasSuiviDirect(lue, {0.0f, inf, 0.0f}, 0.15f);
    Verifier(a.x == 1.0f && a.y == 2.0f && a.z == 3.0f, "NaN : position lue inchangee");
    Verifier(b.x == 1.0f && b.y == 2.0f && b.z == 3.0f, "inf : position lue inchangee");
}

static void Garde()
{
    // Releve reel du 2026-10-01 19:02:10,3 -> 11,25 (X du corps au pont, ~7 Hz), sprint commande.
    const double t[] = {10.368, 10.536, 10.674, 10.815, 10.955, 11.092, 11.233};
    const double x[] = {-1345.953, -1345.117, -1344.750, -1344.553, -1344.437, -1344.424, -1344.423};
    GardeMarchePlantee g;
    double tDeclenche = -1.0;
    for (int i = 1; i < 7; ++i)
    {
        const float dt = static_cast<float>(t[i] - t[i - 1]);
        const float v = static_cast<float>(std::fabs(x[i] - x[i - 1]) / dt);
        if (g.Avancer(dt, true, v) && tDeclenche < 0.0) tDeclenche = t[i];
    }
    std::printf("  garde : declenchee a t=%.3f\n", tDeclenche);
    // Au pas de 7 Hz du releve, le corps n'est sous 0,5 m/s qu'a partir de 11,092 : 0,25 s
    // d'immobilite ne sont donc atteintes qu'a 11,233 (pas avant 11,10 comme l'enonce l'attendait :
    // la regle 0,5 m/s / 0,25 s ne le permet pas sur ces echantillons).
    Verifier(tDeclenche > 0.0 && tDeclenche <= 11.25, "garde : se declenche au plus tard a 11,25 sur le releve reel");
    Verifier(tDeclenche > 11.092, "garde : pas avant que le corps soit reellement sous 0,5 m/s");

    // Controle negatif : corps a 5,3 m/s.
    GardeMarchePlantee n;
    bool jamais = true;
    for (int i = 0; i < 200; ++i) jamais = jamais && !n.Avancer(0.011f, true, 5.3f);
    Verifier(jamais, "controle negatif : corps a 5,3 m/s, jamais");

    // Pas de deplacement commande.
    GardeMarchePlantee p;
    bool jamais2 = true;
    for (int i = 0; i < 200; ++i) jamais2 = jamais2 && !p.Avancer(0.011f, false, 0.0f);
    Verifier(jamais2, "pas de deplacement commande : jamais");

    // Pas de seconde alerte avant 0,25 s, puis re-declenchement.
    GardeMarchePlantee r;
    int alertes = 0;
    float t2 = 0.0f;
    float dernier = -1.0f, ecartMin = 99.0f;
    for (int i = 0; i < 100; ++i)
    {
        t2 += 0.011f;
        if (r.Avancer(0.011f, true, 0.0f))
        {
            ++alertes;
            if (dernier >= 0.0f) ecartMin = std::fmin(ecartMin, t2 - dernier);
            dernier = t2;
        }
    }
    Verifier(alertes >= 2, "reamorcage : une seconde alerte arrive apres");
    Verifier(ecartMin >= 0.25f - 1e-3f, "pas de seconde alerte avant 0,25 s");

    // Remise a zero quand le corps avance.
    GardeMarchePlantee z;
    z.Avancer(0.2f, true, 0.0f);
    z.Avancer(0.01f, true, 3.0f);
    Verifier(!z.Avancer(0.1f, true, 0.0f), "corps qui avance : remise a zero");
}

int main()
{
    std::printf("verif_suivi_direct\n");
    Pente();
    SprintDemiTours(90.0f);
    SprintDemiTours(30.0f);
    RegimeEtabli();
    CorpsPoseLoin();
    Garde();
    std::printf("%d verifs, %d echecs\n", g_verifs, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

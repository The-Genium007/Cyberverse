// =====================================================================================
// Auto-verification de l'ecrivain du vol (src/PlayerSync/EcrivainVol.h). AUCUN framework,
// AUCUN type moteur. Les nombres viennent du bloc SAUT-2 (2026-10-05, F-PLY-716/717).
//
// Compilation et execution (BuildTools 18) :
//   cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\
//            Build\vcvars64.bat" && cl /std:c++20 /EHsc /W4 /nologo
//            client\red4ext\tests\verif_ecrivain_vol.cpp /Fe:verif_vol.exe && verif_vol.exe'
// =====================================================================================

#include "../src/PlayerSync/EcrivainVol.h"

#include <cmath>
#include <cstdio>
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

// L'arc de V (F-PLY-705) : 0,971 m, 0,716 s, sommet a 0,335 s. Deux demi-paraboles.
static float ArcV(float t)
{
    const float h = 0.971f, tm = 0.335f, T = 0.716f;
    if (t <= 0.0f || t >= T) return 0.0f;
    const float u = t < tm ? (tm - t) / tm : (t - tm) / (T - tm);
    return h * (1.0f - u * u);
}

// Rejoue la boucle du client : vitesse lissee (moyenne exponentielle 0,25) puis avance.
// Rend le sommet et le creux de la cible ECRITE sur un saut de V, a 30 i/s, tampon 0,1 s.
static void CibleEcrite(bool porte, float& sommet, float& creux)
{
    const float dt = 0.031f, delai = 0.1f;
    float vz = 0.0f, prec = 0.0f;
    sommet = -1e9f; creux = 1e9f;
    for (float t = dt; t < 0.716f; t += dt)
    {
        const float z = ArcV(t);
        vz += 0.25f * ((z - prec) / dt - vz);
        prec = z;
        const float ecrit = z + AvanceVerticale(vz, delai, dt, porte);
        sommet = std::max(sommet, ecrit);
        creux = std::min(creux, ecrit);
    }
}

static void Avance()
{
    // Le defaut mesure : un sauteur traite comme un passager de cabine. Le temoin (porte = vrai)
    // doit REPRODUIRE les nombres du bloc SAUT-2 (sommet 1,43 m, fin a -0,48 m), sinon ce test
    // ne mesure pas le defaut qu'il pretend fermer.
    float sommet = 0.0f, creux = 0.0f;
    CibleEcrite(true, sommet, creux);
    Verifier(sommet > 1.25f && sommet < 1.60f, "temoin : extrapole, le sommet ecrit depasse 1,25 m (mesure : 1,43)");
    Verifier(creux < -0.30f, "temoin : extrapole, la cible ecrite passe sous le sol (mesure : -0,48)");

    CibleEcrite(false, sommet, creux);
    Verifier(std::fabs(sommet - 0.971f) < 0.02f, "sauteur : le sommet ecrit est celui du fil (0,971 m)");
    Verifier(creux >= 0.0f, "sauteur : la cible ecrite ne passe jamais sous le sol");

    // Passager de cabine : la regle d'origine est intacte (3 m/s, tampon 0,1 s, image 0,02 s).
    Verifier(std::fabs(AvanceVerticale(3.0f, 0.1f, 0.02f, true) - 0.39f) < 1e-4f, "passager : vz x (delai + 1,5 image)");
    Verifier(std::fabs(AvanceVerticale(3.0f, 0.9f, 0.02f, true) - 0.99f) < 1e-4f, "passager : delai borne a 0,3 s");
    Verifier(AvanceVerticale(-3.0f, -1.0f, 0.02f, true) < 0.0f && AvanceVerticale(-3.0f, -1.0f, 0.02f, true) > -0.1f,
             "passager : delai negatif ramene a zero");
    Verifier(AvanceVerticale(30.0f, 0.1f, 0.02f, true) == 0.0f, "filet : au-dela de 2 m, aucune avance");
    Verifier(AvanceVerticale(std::numeric_limits<float>::quiet_NaN(), 0.1f, 0.02f, true) == 0.0f, "filet : NaN");
}

static void Queue()
{
    // Au contact, l'ecrivain du vol garde la main 0,15 s : sinon le corps reste a la hauteur de sa
    // derniere image de vol (+0,1/+0,2 m mesures) jusqu'au placement immobile suivant.
    QueueDeVol q;
    Verifier(!q.Avancer(0.03f, false), "au sol sans vol : l'ecrivain du vol ne prend pas la main");
    Verifier(q.Avancer(0.03f, true), "en l'air : il a la main");
    Verifier(q.Avancer(0.03f, false), "contact, +0,03 s : il la garde");
    Verifier(q.Avancer(0.06f, false), "contact, +0,09 s : il la garde");
    Verifier(!q.Avancer(0.07f, false), "contact, +0,16 s : il la rend");
    Verifier(!q.Avancer(0.03f, false), "et ne la reprend pas tout seul");
    Verifier(q.Avancer(0.03f, true) && q.Avancer(0.03f, false), "saut suivant : meme regle");
}

static void Temoin()
{
    // « Qui a ecrit ? » : le corps lu a l'image n+1 est-il ce que nous avons ecrit a l'image n ?
    BilanVol b;
    Verifier(b.Noter(30.0f, 0.0f) == Ecrivain::Inconnu, "premiere image : rien a comparer");
    Verifier(b.Noter(30.405f, 30.0005f) == Ecrivain::Nous, "lu = ecrit a 1 mm : nous");
    Verifier(b.Noter(30.9f, 29.5f) == Ecrivain::Autre, "lu au sol alors qu'on a ecrit 30,405 : un autre");
    Verifier(b.images == 3 && b.nous == 1 && b.autre == 1, "le bilan compte");
    Verifier(std::fabs(b.pireEcartM - 0.905f) < 1e-3f, "et garde le pire ecart");
    b = BilanVol{};
    Verifier(b.images == 0 && b.Noter(1.0f, 1.0f) == Ecrivain::Inconnu, "remis a zero au decollage");
    Verifier(std::string_view(NomEcrivain(Ecrivain::Autre)) == "AUTRE", "le nom est celui du journal");
}

static void Decollage()
{
    // Chute T6 : premiere image « en l'air » deja 0,21 m sous le rebord (29,29 pour 29,50).
    Verifier(ZDecollage(29.29f, 29.50f, true) == 29.50f, "chute : la hauteur de depart est celle du sol quitte");
    Verifier(ZDecollage(29.60f, 29.50f, true) == 29.60f, "saut : la premiere image de vol est deja plus haute");
    Verifier(ZDecollage(29.29f, 0.0f, false) == 29.29f, "sans image precedente : la premiere image de vol");
}

int main()
{
    Avance();
    Queue();
    Temoin();
    Decollage();
    std::printf("%d verification(s), %d echec(s)\n", g_verifs, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

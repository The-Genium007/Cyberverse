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

static void VolRendu()
{
    // Le saut se LIT sur la pose rendue (en retard d'un tampon), pas sur le dernier echantillon recu : sinon
    // l'animation part 0,05 a 0,1 s avant l'arc, et la hauteur de chute se lit 1 m au-dessus du sol (F-PLY-723).
    Verifier(!EnVolRendu(0, 0), "sol -> sol : pas en vol");
    Verifier(EnVolRendu(0, 6), "sol -> air : le segment rendu quitte le sol, en vol");
    Verifier(EnVolRendu(6, 6), "air -> air : en vol");
    Verifier(EnVolRendu(6, 0), "air -> sol : encore en vol tant que le segment descend");
    Verifier(!EnVolRendu(2, 3), "course -> sprint : pas en vol");
}

static void Marche()
{
    using A = GardeMarcheVol;
    // Decollage : on coupe (annuler + tenir). Avec tenir = faux, on annule seulement.
    A g;
    Verifier(g.Avancer(0.02f, false, true, false, true) == A::Rien, "au sol : la garde ne touche a rien");
    Verifier(g.Avancer(0.02f, true, true, false, true) == (A::Annuler | A::Tenir), "decollage : annuler ET tenir");
    // Image suivante : la marche a disparu, la tenue est la. Rien a faire.
    Verifier(g.Avancer(0.02f, true, false, true, true) == A::Rien, "en vol, marche absente, tenue vue : rien");
    // La marche REVIENT (annulation acceptee sans etre executee, ou reemise par ailleurs) : on la recoupe.
    Verifier(g.Avancer(0.02f, true, true, true, true) == A::Annuler, "en vol, marche revenue : annuler, a CHAQUE image");
    Verifier(g.Avancer(0.02f, true, true, true, true) == A::Annuler, "encore la : encore annuler");
    // La tenue a expire (chute longue) : on la reemet, mais pas a chaque image (une commande par image a deja
    // fait tomber le jeu) — au plus une toutes les 0,25 s.
    Verifier(g.Avancer(0.02f, true, false, false, true) == A::Rien, "tenue absente depuis peu : on attend");
    unsigned vu = A::Rien;
    for (int i = 0; i < 12 && vu == A::Rien; ++i) vu = g.Avancer(0.02f, true, false, false, true);
    Verifier(vu == A::Tenir, "tenue absente 0,25 s : on la reemet");
    Verifier(g.Avancer(0.02f, true, false, false, true) == A::Rien, "et pas deux fois de suite");
    // Contact (fin de la queue) : la marche est reemise, une fois.
    Verifier(g.Avancer(0.02f, false, false, true, true) == A::Reemettre, "fin du vol : reemettre la marche");
    Verifier(g.Avancer(0.02f, false, false, false, true) == A::Rien, "une seule fois");
    Verifier(g.images == 15 && g.marcheVue == 3 && g.annulations == 3 && g.tenues == 2, "le bilan compte images, retours, annulations, tenues");
    // Sans tenue (A/B) : jamais de Tenir, et le bilan repart de zero au decollage suivant.
    Verifier(g.Avancer(0.02f, true, false, false, false) == A::Annuler, "decollage sans tenue : annuler seul");
    Verifier(g.images == 1 && g.marcheVue == 0 && g.annulations == 1 && g.tenues == 0, "bilan remis a zero au decollage");
    for (int i = 0; i < 30; ++i) Verifier(g.Avancer(0.02f, true, false, false, false) == A::Rien, "sans tenue : jamais de Tenir");
}

int main()
{
    Avance();
    Queue();
    Temoin();
    Decollage();
    VolRendu();
    Marche();
    std::printf("%d verification(s), %d echec(s)\n", g_verifs, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

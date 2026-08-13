// =====================================================================================
// Auto-vérification du tampon d'interpolation. AUCUN framework, AUCUN type moteur.
//
// C'est possible — et c'est tout l'intérêt — parce que les couches 1 et 2 sont de
// l'arithmétique pure. Elles admettent donc un vrai test rouge→vert (doctrine D1) sans
// session Windows, ce qui n'existe nulle part ailleurs de ce côté du fil.
//
// N'est PAS dans `src/CMakeLists.txt` : ce fichier porte un `main()` et n'a rien à faire
// dans la DLL. Les sources y sont listées à la main (pas de glob), donc rien à exclure.
//
// Compilation et exécution (BuildTools 18) :
//   cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\
//            Build\vcvars64.bat" && cl /std:c++20 /EHsc /W4 /nologo
//            client\red4ext\tests\verif_tampon_interpolation.cpp /Fe:verif.exe && verif.exe'
// =====================================================================================

#include "../src/PlayerSync/TamponInterpolation.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

static void Proche(float obtenu, float attendu, const char* quoi, float tolerance = 1e-3f)
{
    ++g_verifs;
    if (std::fabs(obtenu - attendu) > tolerance)
    {
        ++g_echecs;
        std::printf("  ECHEC : %s (obtenu %.4f, attendu %.4f)\n", quoi, obtenu, attendu);
    }
}

static Pose PoseXY(float x, float y, float yaw = 0.0f, std::uint8_t loco = 1)
{
    Pose p;
    p.x = x;
    p.y = y;
    p.yaw = yaw;
    p.locomotion = loco;
    return p;
}

// -------------------------------------------------------------------------------------

static void InterpoleExactementEntreDeuxEchantillons()
{
    std::printf("interpolation exacte au milieu de deux echantillons\n");
    TamponPose t;
    t.Pousser(100, PoseXY(0.0f, 0.0f));
    t.Pousser(101, PoseXY(1.0f, 2.0f));

    PoseRendue r;
    // Tick 100 = 5,000 s ; tick 101 = 5,050 s. Milieu = 5,025 s.
    Verifier(t.Echantillonner(5.025, r), "echantillonnage possible");
    Proche(r.x, 0.5f, "x au milieu");
    Proche(r.y, 1.0f, "y au milieu");
    Verifier(!r.extrapolee, "milieu de deux echantillons = interpole, jamais extrapole");
    // Vitesse : 1 m et 2 m parcourus en 50 ms.
    Proche(r.vx, 20.0f, "vitesse x deduite", 0.01f);
    Proche(r.vy, 40.0f, "vitesse y deduite", 0.01f);
}

static void LeYawPrendLePlusCourtChemin()
{
    std::printf("le yaw passe par le plus court chemin (le TODO jamais fait du fork)\n");
    // 350 -> 10 : +20 en passant par 0, JAMAIS -340.
    Proche(LerpAngle(350.0f, 10.0f, 0.5f), 0.0f, "350 -> 10 a mi-chemin = 0");
    Proche(LerpAngle(10.0f, 350.0f, 0.5f), 0.0f, "10 -> 350 a mi-chemin = 0");
    Proche(LerpAngle(0.0f, 90.0f, 0.5f), 45.0f, "cas simple sans franchissement");
    // Le demi-tour exact (180) est ambigu par nature : on exige seulement qu'il ne
    // parcoure pas plus de 180 degres.
    Verifier(std::fabs(EcartAngulaire(0.0f, 180.0f)) <= 180.0f, "jamais plus de 180 degres");

    TamponPose t;
    t.Pousser(10, PoseXY(0.0f, 0.0f, 350.0f));
    t.Pousser(11, PoseXY(0.0f, 0.0f, 10.0f));
    PoseRendue r;
    Verifier(t.Echantillonner(0.525, r), "echantillonnage possible");
    Proche(r.yaw, 0.0f, "yaw interpole par le tampon");
}

static void UnEchantillonPerimeEstIgnore()
{
    std::printf("un echantillon perime ou duplique est ignore\n");
    TamponPose t;
    t.Pousser(100, PoseXY(0.0f, 0.0f));
    t.Pousser(101, PoseXY(1.0f, 0.0f));
    t.Pousser(100, PoseXY(999.0f, 999.0f)); // en retard — le canal est non fiable
    t.Pousser(101, PoseXY(888.0f, 888.0f)); // duplique

    Verifier(t.Nombre() == 2, "les deux intrus n'entrent pas");
    Verifier(t.DernierTick() == 101, "le dernier tick reste 101");
    PoseRendue r;
    Verifier(t.Echantillonner(5.050, r), "echantillonnage possible");
    Proche(r.x, 1.0f, "la pose perimee n'a rien ecrase");
}

static void ExtrapolationBorneePuisGel()
{
    std::printf("l'extrapolation est bornee, puis l'avatar fige\n");
    TamponPose t;
    t.Pousser(100, PoseXY(0.0f, 0.0f)); // 5,000 s
    t.Pousser(101, PoseXY(1.0f, 0.0f)); // 5,050 s, soit 20 m/s

    PoseRendue juste, borne, bienApres;
    Verifier(t.Echantillonner(5.100, juste), "50 ms apres le dernier");
    Verifier(t.Echantillonner(5.300, borne), "250 ms apres = la borne exacte");
    Verifier(t.Echantillonner(9.999, bienApres), "tres au-dela de la borne");

    Verifier(juste.extrapolee, "au-dela du dernier echantillon = extrapole");
    Proche(juste.x, 2.0f, "extrapolation lineaire sur 50 ms");
    Proche(borne.x, 1.0f + 20.0f * 0.25f, "extrapolation a la borne (250 ms)");
    Proche(bienApres.x, borne.x, "au-dela de la borne, la position ne bouge plus");
}

static void TropEnRetardOnFigeSurLePlusAncien()
{
    std::printf("trop en retard : on fige sur le plus ancien, on n'invente pas de passe\n");
    TamponPose t;
    t.Pousser(100, PoseXY(7.0f, 0.0f));
    t.Pousser(101, PoseXY(8.0f, 0.0f));

    PoseRendue r;
    Verifier(t.Echantillonner(0.0, r), "echantillonnage possible malgre le retard");
    Proche(r.x, 7.0f, "fige sur le plus ancien echantillon");
}

static void LAnneauTourneEtGardeLesPlusRecents()
{
    std::printf("l'anneau tourne et garde les plus recents, dans l'ordre\n");
    TamponPose t;
    for (std::uint64_t i = 0; i < kProfondeurTampon + 5; ++i)
    {
        t.Pousser(i, PoseXY(static_cast<float>(i), 0.0f));
    }
    Verifier(t.Nombre() == kProfondeurTampon, "le tampon ne depasse pas sa profondeur");
    Verifier(t.DernierTick() == kProfondeurTampon + 4, "le dernier tick est le plus recent");

    // Les deux derniers ticks sont 11 et 12 (profondeur 8 : ticks 5..12).
    const double tDernier = static_cast<double>(kProfondeurTampon + 4) * kPeriodeTickS;
    PoseRendue r;
    Verifier(t.Echantillonner(tDernier - kPeriodeTickS * 0.5, r), "echantillonnage possible");
    Proche(r.x, static_cast<float>(kProfondeurTampon + 3) + 0.5f, "interpolation correcte apres rotation");
}

static void LEtatDAnimationNAntipipeJamais()
{
    std::printf("l'etat d'animation est celui de l'echantillon courant, pas du suivant\n");
    TamponPose t;
    t.Pousser(100, PoseXY(0.0f, 0.0f, 0.0f, /*loco=*/1)); // marche
    t.Pousser(101, PoseXY(1.0f, 0.0f, 0.0f, /*loco=*/3)); // sprint

    PoseRendue r;
    Verifier(t.Echantillonner(5.049, r), "juste avant le second echantillon");
    Verifier(r.locomotion == 1, "encore en marche : une posture est ce qu'elle etait");
}

static void LHorlogeSAmorceRattrapeEtSaute()
{
    std::printf("l'horloge s'amorce, rattrape doucement, et saute sur discontinuite\n");
    HorlogeRendu h;
    Verifier(!h.Amorcee(), "non amorcee avant tout snapshot");

    h.ObserverSnapshot(100); // 5,000 s - 0,100 s de delai
    Verifier(h.Amorcee(), "amorcee au premier snapshot");
    Proche(static_cast<float>(h.TempsRendu()), 4.9f, "amorcage direct sur la cible");

    h.Avancer(0.05);
    Proche(static_cast<float>(h.TempsRendu()), 4.95f, "avance avec le temps local");

    // Petit ecart : rattrapage DOUX, jamais un saut.
    const double avant = h.TempsRendu();
    h.ObserverSnapshot(101); // cible 4,950 s — deja atteinte, ecart nul ou minime
    Verifier(std::fabs(h.TempsRendu() - avant) < 0.02, "pas de saut sur un ecart minime");

    // Grosse discontinuite : saut franc.
    h.ObserverSnapshot(2000); // cible = 100,000 - 0,100
    Proche(static_cast<float>(h.TempsRendu()), 99.9f, "saut franc au-dela du seuil", 0.01f);
}

static void LePointDeViseeEstDevantEtStableALArret()
{
    std::printf("le point de visee est devant, et ne tourne pas a l'arret\n");
    PoseRendue enMouvement;
    enMouvement.x = 10.0f;
    enMouvement.y = 5.0f;
    enMouvement.vx = 3.0f; // plein est, 3 m/s
    float x = 0.0f, y = 0.0f, z = 0.0f;
    PointDeVisee(enMouvement, 2.0f, x, y, z);
    Proche(x, 12.0f, "vise 2 m devant, le long de la vitesse");
    Proche(y, 5.0f, "pas de derive laterale");

    PoseRendue immobile;
    immobile.x = 10.0f;
    immobile.y = 5.0f;
    immobile.vx = 0.001f; // bruit de mesure
    PointDeVisee(immobile, 2.0f, x, y, z);
    Proche(x, 10.0f, "a l'arret, on vise sa propre position");
    Proche(y, 5.0f, "a l'arret, on vise sa propre position");
}

static void LeDelaiSAdapteALaGigueEtRedescendLentement()
{
    std::printf("le delai grandit avec la gigue, et redescend lentement\n");
    HorlogeRendu h;
    h.ObserverSnapshot(100);
    Proche(static_cast<float>(h.DelaiCourant()), 0.100f, "au repos : le plancher");

    // Regime NOMINAL : un snapshot toutes les 50 ms, aucune gigue.
    for (std::uint64_t t = 101; t < 110; ++t)
    {
        h.Avancer(kPeriodeTickS);
        h.ObserverSnapshot(t);
    }
    Proche(static_cast<float>(h.DelaiCourant()), 0.100f, "flux regulier : le delai ne bouge pas");

    // RAFALE : un snapshot arrive 200 ms en retard (chargement d'une autre instance).
    h.Avancer(0.25);
    h.ObserverSnapshot(110);
    Verifier(h.DelaiCourant() > 0.100, "apres une rafale, le delai a GRANDI");
    Verifier(h.Gigue() > 0.15, "la gigue retenue reflete le retard observe");
    const double apresRafale = h.DelaiCourant();

    // Le calme revient : la descente doit etre LENTE, pas immediate.
    h.Avancer(kPeriodeTickS);
    h.ObserverSnapshot(111);
    Verifier(h.DelaiCourant() > apresRafale * 0.9,
             "un seul snapshot calme ne doit PAS effacer la gigue — sinon on retombe dans le trou");

    // Mais elle finit par revenir au plancher.
    for (std::uint64_t t = 112; t < 900; ++t)
    {
        h.Avancer(kPeriodeTickS);
        h.ObserverSnapshot(t);
    }
    Proche(static_cast<float>(h.DelaiCourant()), 0.100f, "apres un long calme : retour au plancher", 0.01f);
}

static void LeDelaiEstPlafonne()
{
    std::printf("le delai adaptatif est plafonne — un reseau casse ne se repare pas au tampon\n");
    HorlogeRendu h;
    h.ObserverSnapshot(100);
    h.Avancer(5.0); // cinq secondes sans rien : une vraie coupure
    h.ObserverSnapshot(101);
    Verifier(h.DelaiCourant() <= kDelaiInterpolationMaxS + 1e-9,
             "le delai ne depasse jamais son plafond");
    Proche(static_cast<float>(h.DelaiCourant()), static_cast<float>(kDelaiInterpolationMaxS),
           "et il s'y colle");
}

static void LAgeDuDernierEchantillonDitQuandLeFilSeTait()
{
    std::printf("l'age du dernier echantillon distingue une perte d'un fil muet\n");
    TamponPose t;
    t.Pousser(100, PoseXY(0.0f, 0.0f)); // tick 100 = 5,000 s
    // On rend a 5,000 s : l'echantillon vient d'arriver.
    Proche(static_cast<float>(t.AgeDuDernierEchantillon(5.0)), 0.0f, "frais");
    // On rend a 5,500 s alors que rien n'est arrive depuis : le fil est muet depuis 500 ms.
    Proche(static_cast<float>(t.AgeDuDernierEchantillon(5.5)), 0.5f, "500 ms de silence");
    TamponPose vide;
    Proche(static_cast<float>(vide.AgeDuDernierEchantillon(9.0)), 0.0f,
           "un tampon vide ne pretend pas avoir un age");
}

static void UnTamponVideNeRendRien()
{
    std::printf("un tampon vide ne rend rien — l'appelant ne doit rien afficher\n");
    TamponPose t;
    PoseRendue r;
    Verifier(!t.Echantillonner(1.0, r), "faux sur un tampon vide");
}

int main()
{
    std::printf("=== Verification du tampon d'interpolation ===\n\n");
    InterpoleExactementEntreDeuxEchantillons();
    LeYawPrendLePlusCourtChemin();
    UnEchantillonPerimeEstIgnore();
    ExtrapolationBorneePuisGel();
    TropEnRetardOnFigeSurLePlusAncien();
    LAnneauTourneEtGardeLesPlusRecents();
    LEtatDAnimationNAntipipeJamais();
    LHorlogeSAmorceRattrapeEtSaute();
    LePointDeViseeEstDevantEtStableALArret();
    LeDelaiSAdapteALaGigueEtRedescendLentement();
    LeDelaiEstPlafonne();
    LAgeDuDernierEchantillonDitQuandLeFilSeTait();
    UnTamponVideNeRendRien();

    std::printf("\n%d verifications, %d echec(s)\n", g_verifs, g_echecs);
    return g_echecs == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

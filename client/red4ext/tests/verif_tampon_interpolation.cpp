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

#include "../src/PlayerSync/HorlogeServeur.h"
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

// Instant d'un tick, EN SECONDES, derive de la cadence.
//
// ⚠️ Les instants etaient ecrits en dur (5,025 s pour le tick 100). Ca supposait 20 Hz sans le
// dire : au passage a 50 Hz, quatorze verifications sont tombees d'un coup alors que le code
// etait juste. Un test qui code en dur ce que le code calcule teste la constante, pas le
// comportement.
static double T(double tick)
{
    return tick * kPeriodeTickS;
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
    // ⚠️ Des ecarts REALISTES. Ce test affirmait 1 m et 2 m en 50 ms, soit 44,7 m/s — une vitesse
    // qu'aucun joueur a pied n'atteint (le sprint plafonne vers 9 m/s). Il passait quand meme,
    // parce que rien ne bornait la vitesse derivee ; le bridage l'a fait tomber, et c'est le test
    // qui avait tort. On mesure ici la DERIVATION, pas la capacite a croire l'impossible :
    // 0,10 m et 0,20 m en 50 ms = 2 et 4 m/s, l'allure d'une vraie marche.
    t.Pousser(100, PoseXY(0.0f, 0.0f));
    t.Pousser(101, PoseXY(0.10f, 0.20f));

    PoseRendue r;
    // Tick 100 = 5,000 s ; tick 101 = 5,050 s. Milieu = 5,025 s.
    Verifier(t.Echantillonner(T(100.5), r), "echantillonnage possible");
    Proche(r.x, 0.05f, "x au milieu");
    Proche(r.y, 0.10f, "y au milieu");
    Verifier(!r.extrapolee, "milieu de deux echantillons = interpole, jamais extrapole");
    Proche(r.vx, static_cast<float>(0.10 / kPeriodeTickS), "vitesse x deduite", 0.01f);
    Proche(r.vy, static_cast<float>(0.20 / kPeriodeTickS), "vitesse y deduite", 0.01f);
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
    Verifier(t.Echantillonner(T(10.5), r), "echantillonnage possible");
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
    Verifier(t.Echantillonner(T(101), r), "echantillonnage possible");
    Proche(r.x, 1.0f, "la pose perimee n'a rien ecrase");
}

static void ExtrapolationBorneePuisGel()
{
    std::printf("l'extrapolation est bornee, puis l'avatar fige\n");
    // ⚠️ Une allure REALISTE, et derivee de la cadence. Ce test poussait 1 m par tick — soit 20 m/s
    // a 20 Hz, mais 50 m/s a 50 Hz, donc AU-DESSUS de la bride, et il tombait. Ce qu'on mesure ici
    // est l'extrapolation, pas la bride (elle a son propre test) : on prend donc une vitesse qui
    // reste sous le plafond quelle que soit la cadence.
    constexpr float kVitesse = 2.0f; // m/s, une marche
    const float pas = kVitesse * static_cast<float>(kPeriodeTickS);
    TamponPose t;
    t.Pousser(100, PoseXY(0.0f, 0.0f));
    t.Pousser(101, PoseXY(pas, 0.0f));

    PoseRendue juste, borne, bienApres;
    Verifier(t.Echantillonner(T(101) + 0.050, juste), "50 ms apres le dernier");
    Verifier(t.Echantillonner(T(101) + kExtrapolationMaxS, borne), "a la borne exacte");
    Verifier(t.Echantillonner(T(101) + 5.0, bienApres), "tres au-dela de la borne");

    Verifier(juste.extrapolee, "au-dela du dernier echantillon = extrapole");
    Proche(juste.x, pas + kVitesse * 0.050f, "extrapolation lineaire sur 50 ms");
    Proche(borne.x, pas + kVitesse * static_cast<float>(kExtrapolationMaxS), "extrapolation a la borne");
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
    Verifier(t.Echantillonner(T(101) - 0.001, r), "juste avant le second echantillon");
    Verifier(r.locomotion == 1, "encore en marche : une posture est ce qu'elle etait");
}

static void LHorlogeSAmorceRattrapeEtSaute()
{
    std::printf("l'horloge s'amorce, rattrape doucement, et saute sur discontinuite\n");
    HorlogeRendu h;
    Verifier(!h.Amorcee(), "non amorcee avant tout snapshot");

    h.ObserverSnapshot(100); // 5,000 s - 0,100 s de delai
    Verifier(h.Amorcee(), "amorcee au premier snapshot");
    Proche(static_cast<float>(h.TempsRendu()), static_cast<float>(T(100) - kDelaiInterpolationS), "amorcage direct sur la cible");

    h.Avancer(0.05);
    Proche(static_cast<float>(h.TempsRendu()), static_cast<float>(T(100) - kDelaiInterpolationS + 0.05), "avance avec le temps local");

    // Petit ecart : rattrapage DOUX, jamais un saut.
    const double avant = h.TempsRendu();
    h.ObserverSnapshot(101); // cible 4,950 s — deja atteinte, ecart nul ou minime
    Verifier(std::fabs(h.TempsRendu() - avant) < 0.02, "pas de saut sur un ecart minime");

    // Grosse discontinuite : saut franc.
    h.ObserverSnapshot(2000); // cible = 100,000 - 0,100
    Proche(static_cast<float>(h.TempsRendu()), static_cast<float>(T(2000) - h.DelaiCourant()), "saut franc au-dela du seuil", 0.01f);
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
    Proche(static_cast<float>(t.AgeDuDernierEchantillon(T(100))), 0.0f, "frais");
    // On rend a 5,500 s alors que rien n'est arrive depuis : le fil est muet depuis 500 ms.
    Proche(static_cast<float>(t.AgeDuDernierEchantillon(T(100) + 0.5)), 0.5f, "500 ms de silence");
    TamponPose vide;
    Proche(static_cast<float>(vide.AgeDuDernierEchantillon(T(180))), 0.0f,
           "un tampon vide ne pretend pas avoir un age");
}

static void UneVitesseAberranteEstBridee()
{
    std::printf("une vitesse aberrante est bridee - pas de teleportation par extrapolation\n");
    TamponPose t;
    // Deux echantillons separes de 5 m en 50 ms : 100 m/s. C'est ce que produit un paquet
    // desordonne depuis le passage en canal non fiable.
    t.Pousser(100, PoseXY(0.0f, 0.0f));
    t.Pousser(101, PoseXY(5.0f, 0.0f));

    PoseRendue r;
    Verifier(t.Echantillonner(T(100.5), r), "echantillonnage possible");
    const float norme = std::sqrt(r.vx * r.vx + r.vy * r.vy + r.vz * r.vz);
    Verifier(norme <= kVitesseMaxMS + 1e-3f, "la vitesse est bridee au plafond");
    Verifier(r.vx > 0.0f, "mais la DIRECTION est conservee");

    // Et surtout : l'extrapolation a la borne ne doit plus faire de saut de dizaines de metres.
    PoseRendue loin;
    Verifier(t.Echantillonner(T(101) + 5.0, loin), "tres au-dela de la borne");
    const float saut = std::sqrt((loin.x - 5.0f) * (loin.x - 5.0f) + loin.y * loin.y);
    Verifier(saut <= kVitesseMaxMS * static_cast<float>(kExtrapolationMaxS) + 0.01f,
             "l'extrapolation reste bornee par la vitesse bridee");
    Verifier(saut < 6.0f, "et donc tres loin des dizaines de metres observees en jeu");
}

static void UneDiscontinuiteCoupeAuLieuDeGlisser()
{
    std::printf("une discontinuite coupe l'historique au lieu de faire glisser l'avatar\n");
    TamponPose t;
    t.Pousser(100, PoseXY(0.0f, 0.0f));
    t.Pousser(101, PoseXY(0.2f, 0.0f));
    Verifier(t.Nombre() == 2, "deux echantillons normaux");

    // Saut franc : teleportation, ascenseur, ou paquet desordonne.
    t.Pousser(102, PoseXY(500.0f, 0.0f));
    Verifier(t.Nombre() == 1, "l'historique est jete : on ne glisse pas a travers la ville");

    PoseRendue r;
    Verifier(t.Echantillonner(T(102), r), "echantillonnage possible");
    Proche(r.x, 500.0f, "on est NET a la nouvelle position, pas quelque part entre les deux");
    Proche(r.vx, 0.0f, "et sans vitesse heritee du saut");
}

static void UnTamponVideNeRendRien()
{
    std::printf("un tampon vide ne rend rien — l'appelant ne doit rien afficher\n");
    TamponPose t;
    PoseRendue r;
    Verifier(!t.Echantillonner(1.0, r), "faux sur un tampon vide");
}

// ─────────────────────────────────────────────────────────────────────────────────────
// LA PANNE QUI GELAIT TOUT — régression franche du numéro de tick
// ─────────────────────────────────────────────────────────────────────────────────────
//
// Ce test échoue sur le code d'avant le 2026-08-15 : `Pousser` rejetait tout tick
// inférieur ou égal au dernier vu, sans distinguer le paquet en retard (quelques ticks,
// régime NORMAL sur un canal non fiable) de la timeline qui repart de zéro (un shard
// redémarré, cf. `shard.rs` : « le Server est reconstruit par connexion »).
//
// Conséquence de l'ancien code, et c'est le pire mode de panne du fichier : plus AUCUN
// échantillon accepté, pour toute la session, avec un tampon qui se dit plein et zéro
// extrapolation signalée. L'avatar gèle, et l'instrument affiche un système sain.
static void UneRegressionFrancheDeTickRepartDeZero()
{
    std::printf("une regression franche de tick redemarre le tampon au lieu de tout rejeter\n");
    TamponPose t;
    // Une timeline avancée, comme après quelques minutes de jeu.
    t.Pousser(1'000'000, Pose{0.0f, 0.0f, 0.0f, 0.0f, 1, 0});
    t.Pousser(1'000'001, Pose{1.0f, 0.0f, 0.0f, 0.0f, 1, 0});
    Verifier(t.Nombre() == 2, "deux echantillons rentres");
    Verifier(t.Regressions() == 0, "aucune regression pour l'instant");

    // Le shard redémarre : les ticks repartent de 1.
    t.Pousser(1, Pose{50.0f, 0.0f, 0.0f, 0.0f, 1, 0});
    Verifier(t.Regressions() == 1, "la regression est COMPTEE, donc journalisable");
    Verifier(t.Nombre() == 1, "l'historique perime est jete, le nouvel echantillon est garde");
    Verifier(t.DernierTick() == 1, "la nouvelle timeline fait autorite");

    // Et surtout : la suite est acceptée normalement. C'est ça qui manquait.
    t.Pousser(2, Pose{51.0f, 0.0f, 0.0f, 0.0f, 1, 0});
    Verifier(t.Nombre() == 2, "la nouvelle timeline continue d'alimenter le tampon");
}

// Le pendant du test ci-dessus : un retard ORDINAIRE ne doit surtout pas vider le tampon.
// Confondre les deux transformerait le régime normal d'un canal non fiable en purge
// permanente, et l'avatar sauterait à chaque paquet désordonné.
static void UnRetardOrdinaireNeViderPasLeTampon()
{
    std::printf("un paquet en retard ordinaire est ignore, PAS traite comme une discontinuite\n");
    TamponPose t;
    t.Pousser(1000, Pose{0.0f, 0.0f, 0.0f, 0.0f, 1, 0});
    t.Pousser(1001, Pose{1.0f, 0.0f, 0.0f, 0.0f, 1, 0});
    t.Pousser(1002, Pose{2.0f, 0.0f, 0.0f, 0.0f, 1, 0});
    // Un paquet du tick 999 arrive après coup : c'est du désordre, pas une nouvelle timeline.
    t.Pousser(999, Pose{99.0f, 0.0f, 0.0f, 0.0f, 1, 0});
    Verifier(t.Nombre() == 3, "le tampon garde ses trois echantillons");
    Verifier(t.Regressions() == 0, "un retard ordinaire n'est PAS compte comme une regression");
    Verifier(t.DernierTick() == 1002, "le plus recent reste le plus recent");
}

// ─────────────────────────────────────────────────────────────────────────────────────
// L'HORLOGE SERVEUR
// ─────────────────────────────────────────────────────────────────────────────────────

static void LHorlogeServeurRetrouveUnDecalageMalgreLaGigue()
{
    std::printf("l'horloge serveur retrouve le decalage par le minimum, malgre la gigue\n");
    HorlogeServeur h;
    Verifier(!h.EstAmorcee(), "non amorcee tant qu'aucun snapshot n'est observe");
    Verifier(h.TempsServeurMs(5000) == 5000, "sans observation, on rend l'heure locale telle quelle");

    // Vérité fabriquée : l'horloge locale AVANCE de 3 000 ms sur celle du serveur, et le délai
    // aller vaut 20 ms au mieux, avec des pointes jusqu'à 120 ms.
    constexpr std::int64_t kDecalageVrai = 3000;
    const int delais[] = {60, 120, 45, 20, 80, 95, 30, 110};
    std::uint64_t tsServeur = 1'000'000;
    for (int d : delais)
    {
        const std::uint64_t reception =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(tsServeur) + kDecalageVrai + d);
        h.Observer(tsServeur, reception);
        tsServeur += 40; // 25 Hz de diffusion
    }

    Verifier(h.EstAmorcee(), "amorcee apres observation");
    // Le minimum vaut `decalage + plus petit delai aller` = 3000 + 20.
    Verifier(h.DecalageMs() == kDecalageVrai + 20,
             "le decalage estime vaut le vrai decalage plus le plus petit delai aller");
    // L'étalement dit de combien la datation est floue : 120 - 20 = 100 ms.
    Verifier(h.EtalementMs() == 100, "l'etalement expose la barre d'erreur");

    // Ce qu'on en fait : dater un événement local sur la timeline du serveur.
    const std::uint64_t maintenantLocal = 1'000'320 + kDecalageVrai;
    Verifier(h.TempsServeurMs(maintenantLocal) == 1'000'320 - 20,
             "un instant local se traduit en instant serveur a 20 ms pres");
}

static void LHorlogeServeurIgnoreUnHorodatageAbsent()
{
    std::printf("un ts_ms absent (serveur ancien) est ignore, jamais pris pour l'annee 1970\n");
    HorlogeServeur h;
    h.Observer(0, 1'700'000'000'000ull);
    Verifier(!h.EstAmorcee(), "un ts_ms a zero n'amorce rien");
    Verifier(h.TempsServeurMs(42) == 42, "et ne fabrique aucune correction");
}

static void LHorlogeServeurSupporteUneHorlogeLocaleEnRetard()
{
    std::printf("une horloge locale EN RETARD sur le serveur donne un decalage negatif\n");
    HorlogeServeur h;
    // Le PC du joueur retarde de 5 s. Sans arithmetique signee, ce cas fait un tour complet
    // d'`uint64` et l'estimation devient une aberration silencieuse.
    constexpr std::int64_t kRetardLocal = -5000;
    std::uint64_t tsServeur = 2'000'000;
    for (int i = 0; i < 10; ++i)
    {
        const std::uint64_t reception = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(tsServeur) + kRetardLocal + 30);
        h.Observer(tsServeur, reception);
        tsServeur += 40;
    }
    Verifier(h.DecalageMs() == kRetardLocal + 30, "le decalage negatif est retrouve tel quel");
    Verifier(h.TempsServeurMs(1'000'000) == 1'004'970,
             "et la traduction locale vers serveur reste juste");
}

static void LHorlogeServeurOublieUnVieuxMinimum()
{
    std::printf("la fenetre glissante OUBLIE, sinon elle ne suivrait jamais la derive\n");
    HorlogeServeur h;
    // Un premier paquet exceptionnellement rapide, puis une longue serie plus lente. Une fois le
    // chanceux sorti de la fenetre, l'estimation doit avoir suivi — c'est toute la raison d'avoir
    // une fenetre plutot qu'un minimum absolu.
    h.Observer(1000, 1000 + 1); // delai aller de 1 ms, le chanceux
    Verifier(h.DecalageMs() == 1, "le premier echantillon fixe l'estimation initiale");
    std::uint64_t ts = 1040;
    for (std::size_t i = 0; i < kFenetreHorloge; ++i)
    {
        h.Observer(ts, ts + 50);
        ts += 40;
    }
    Verifier(h.DecalageMs() == 50, "le chanceux est sorti de la fenetre, l'estimation a suivi");
}

// ─────────────────────────────────────────────────────────────────────────────────────
// LA GIGUE FANTÔME — quand le serveur ne diffuse pas à chaque tick
// ─────────────────────────────────────────────────────────────────────────────────────
//
// Ce test échoue sur le code d'avant le 2026-08-15 : l'estimateur comparait l'arrivée
// réelle à `kPeriodeTickS`, c'est-à-dire à la période de SIMULATION. Depuis le découplage
// (`snapshot_divider()` côté serveur : simulation 50 Hz, diffusion 25 Hz), un fil
// parfaitement sain arrive toutes les 40 ms — et était donc lu comme 20 ms de retard
// PERMANENT.
//
// Rien ne se voyait à l'écran (le délai adaptatif reste au plancher de 100 ms), mais le
// chiffre `gigue` du journal devenait un mensonge — et c'est exactement celui qu'on lira
// pour décider si le réseau d'un joueur va bien. Un instrument qui accuse à tort coûte
// plus cher qu'un instrument absent.
static void UneDiffusionUnTickSurDeuxNeProduitAucuneGigue()
{
    std::printf("une diffusion un tick sur deux ne fabrique PAS de gigue fantome\n");
    HorlogeRendu h;
    h.ObserverSnapshot(100);

    // Regime NOMINAL a 25 Hz de diffusion : les ticks avancent de 2, les arrivees sont
    // espacees de 40 ms, et tout est parfaitement regulier.
    for (std::uint64_t t = 102; t < 140; t += 2)
    {
        h.Avancer(kPeriodeTickS * 2.0);
        h.ObserverSnapshot(t);
    }
    Proche(static_cast<float>(h.Gigue()), 0.0f,
           "un fil regulier a 25 Hz ne doit produire AUCUNE gigue", 1e-4f);
    Proche(static_cast<float>(h.DelaiCourant()), 0.100f,
           "et le delai doit rester au plancher");

    // Et la detection d'un VRAI retard doit continuer de fonctionner : une arrivee a 100 ms sur
    // un intervalle attendu de 40 ms (le tick avance de 2), c'est 60 ms de gigue.
    //
    // ⚠️ Le tick DOIT etre 140 : la boucle ci-dessus s'arrete a 138 (condition `t < 140`). Viser
    // 142 ferait un ecart de 4 ticks, donc un intervalle attendu de 80 ms et une gigue de 20 —
    // premiere version de ce test, et elle accusait le code d'une erreur qui etait la mienne.
    h.Avancer(0.100);
    h.ObserverSnapshot(140);
    Proche(static_cast<float>(h.Gigue()), 0.060f,
           "un vrai retard se mesure contre l'intervalle ATTENDU, pas contre la periode de "
           "simulation",
           1e-3f);
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
    UneVitesseAberranteEstBridee();
    UneDiscontinuiteCoupeAuLieuDeGlisser();
    LeDelaiSAdapteALaGigueEtRedescendLentement();
    LeDelaiEstPlafonne();
    LAgeDuDernierEchantillonDitQuandLeFilSeTait();
    UnTamponVideNeRendRien();
    UneRegressionFrancheDeTickRepartDeZero();
    UnRetardOrdinaireNeViderPasLeTampon();
    LHorlogeServeurRetrouveUnDecalageMalgreLaGigue();
    LHorlogeServeurIgnoreUnHorodatageAbsent();
    LHorlogeServeurSupporteUneHorlogeLocaleEnRetard();
    LHorlogeServeurOublieUnVieuxMinimum();
    UneDiffusionUnTickSurDeuxNeProduitAucuneGigue();

    std::printf("\n%d verifications, %d echec(s)\n", g_verifs, g_echecs);
    return g_echecs == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

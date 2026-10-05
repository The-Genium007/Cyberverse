#pragma once
// =====================================================================================
// L'ecrivain du vol d'un avatar distant. ZERO DEPENDANCE AU MOTEUR
// (testable hors du jeu : tests/verif_ecrivain_vol.cpp).
//
// Pourquoi (bloc SAUT-2 du 2026-10-05, F-PLY-716/717) : en vol, UN seul appel ecrit la
// position (`TesseraEcrirePositionRepresentation`, a chaque image), et le corps lu a l'image
// suivante EST ce qu'on a ecrit (107 lignes sur 118, saut sur place). Ce qui etait faux,
// c'etait la CIBLE : l'avance de retard ecrite pour un passager de cabine (vitesse constante)
// s'appliquait aussi au sauteur, parce que les deux rapportent `locomotion = 6`. Sur un arc,
// vitesse lissee x 0,145 s = +0,46 m au sommet et -0,48 m au contact.
// =====================================================================================

#include <cmath>
#include <string_view>

namespace Tessera::Sync {

/// Avance (m) a ajouter a la hauteur voulue pour annuler le retard du tampon d'interpolation.
/// `porte` = le serveur dit que ce corps est porte par une plateforme. Un corps NON porte en
/// l'air suit un arc : extrapoler sa vitesse le projette au-dela du sommet puis sous le sol.
/// Bornes d'origine conservees : delai dans [0 ; 0,3] s, une image et demie, filet a 2 m (et NaN).
inline float AvanceVerticale(float vzLissee, float delaiS, float dtS, bool porte)
{
    if (!porte)
    {
        return 0.0f;
    }
    const float delai = delaiS > 0.3f ? 0.3f : (delaiS < 0.0f ? 0.0f : delaiS);
    const float avance = vzLissee * (delai + 1.5f * dtS);
    return (avance > -2.0f && avance < 2.0f) ? avance : 0.0f;
}

/// L'ecrivain du vol garde la main un court instant APRES le contact : la derniere image de vol
/// laisse le corps au-dessus du sol (+0,1/+0,2 m mesures), et sous 0,5 m d'ecart vertical aucune
/// autre branche ne le replace avant le placement immobile suivant.
struct QueueDeVol
{
    static constexpr float kQueueS = 0.15f;
    float depuisContactS = kQueueS;   // « pas de vol recent »

    /// Vrai si l'ecriture directe doit porter la position cette image.
    bool Avancer(float dtS, bool enLair)
    {
        depuisContactS = enLair ? 0.0f : depuisContactS + dtS;
        return enLair || depuisContactS < kQueueS;
    }
};

enum class Ecrivain { Inconnu, Nous, Autre };

inline const char* NomEcrivain(Ecrivain e)
{
    return e == Ecrivain::Nous ? "nous" : (e == Ecrivain::Autre ? "AUTRE" : "?");
}

/// Le temoin « qui a ecrit ? » : la hauteur lue a l'image n+1 est-elle celle ecrite a l'image n ?
/// (La relecture dans la MEME image ne vaut rien : elle rend la valeur d'avant, 310 fois sur 310.)
/// A remettre a zero (`= BilanVol{}`) au decollage.
struct BilanVol
{
    static constexpr float kToleranceM = 0.01f;
    int images = 0, nous = 0, autre = 0;
    float pireEcartM = 0.0f;
    float ecritPrecedentZ = 0.0f;

    /// `ecritZ` = ce qu'on ecrit cette image ; `luZ` = le corps lu AVANT cette ecriture.
    Ecrivain Noter(float ecritZ, float luZ)
    {
        Ecrivain qui = Ecrivain::Inconnu;
        if (images > 0)
        {
            const float ecart = std::fabs(luZ - ecritPrecedentZ);
            qui = ecart <= kToleranceM ? Ecrivain::Nous : Ecrivain::Autre;
            (qui == Ecrivain::Nous ? nous : autre) += 1;
            pireEcartM = ecart > pireEcartM ? ecart : pireEcartM;
        }
        ++images;
        ecritPrecedentZ = ecritZ;
        return qui;
    }
};

/// Hauteur de depart d'un vol. La premiere image « en l'air » d'une CHUTE est deja sous le rebord
/// (29,29 m pour 29,50 : les 6,2 m se lisaient 5,3 a 5,9, sous le seuil de reception lourde).
inline float ZDecollage(float zPremiereImageVol, float zImagePrecedente, bool precedenteConnue)
{
    return (precedenteConnue && zImagePrecedente > zPremiereImageVol) ? zImagePrecedente : zPremiereImageVol;
}

/// « En l'air » lu sur la pose RENDUE (en retard d'un tampon, comme la position), pas sur le dernier
/// echantillon recu : `rendue` = allure de l'echantillon d'avant l'instant rendu, `suivante` = celle d'apres.
/// Le vol couvre tout segment qui touche un echantillon en l'air : il commence quand le corps quitte le sol
/// et finit quand il le touche. (Lu sur le dernier echantillon, le saut partait 0,05 a 0,1 s avant l'arc et
/// la hauteur de chute se lisait 0,3 a 1,1 m trop court, F-PLY-723.)
inline bool EnVolRendu(unsigned char rendue, unsigned char suivante)
{
    return rendue == 6 || suivante == 6;
}

/// Coupe la marche du moteur pendant le vol, et le VERIFIE a chaque image (F-PLY-721 : en course, le moteur
/// reecrit hauteur et avance, 307 images sur 380 ; l'annulation du decollage est acceptee sans effet visible).
/// L'appelant lit l'etat des commandes d'IA (`IsCommandExecuting` / `IsCommandWaiting`) et applique les actions.
struct GardeMarcheVol
{
    enum Action : unsigned { Rien = 0, Annuler = 1, Tenir = 2, Reemettre = 4 };
    /// Une tenue expiree se reemet au plus a cette cadence : une commande d'IA par image a deja fait tomber le jeu.
    static constexpr float kPeriodeTenueS = 0.25f;

    bool enVol = false;
    float depuisTenueS = 0.0f;
    int images = 0, marcheVue = 0, annulations = 0, tenues = 0;

    /// `vol` = l'ecrivain du vol a la main (vol + queue). `marcheActive` / `tenueActive` = une commande de
    /// marche / de tenue est en cours ou en attente, lue AVANT d'agir. `tenir` = poser une tenue (defaut) ou
    /// seulement annuler (A/B).
    unsigned Avancer(float dtS, bool vol, bool marcheActive, bool tenueActive, bool tenir)
    {
        unsigned action = Rien;
        if (vol && !enVol)
        {
            images = marcheVue = annulations = tenues = 0;
            depuisTenueS = 0.0f;
            action = Annuler | (tenir ? Tenir : Rien);
            marcheVue += marcheActive ? 1 : 0;
        }
        else if (vol)
        {
            depuisTenueS += dtS;
            if (marcheActive)
            {
                ++marcheVue;
                action |= Annuler;
            }
            if (tenir && !tenueActive && depuisTenueS >= kPeriodeTenueS)
            {
                depuisTenueS = 0.0f;
                action |= Tenir;
            }
        }
        else if (enVol)
        {
            action = Reemettre;
        }
        images += vol ? 1 : 0;
        annulations += (action & Annuler) ? 1 : 0;
        tenues += (action & Tenir) ? 1 : 0;
        enVol = vol;
        return action;
    }
};

} // namespace Tessera::Sync

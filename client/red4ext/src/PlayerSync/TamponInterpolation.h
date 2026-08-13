#pragma once
// =====================================================================================
// Tampon d'interpolation — couches 1 (tampon) et 2 (échantillonnage).
//
// Ce que ça règle : un avatar distant avance par à-coups parce que chaque snapshot est
// appliqué à son arrivée, comme s'il décrivait le présent. Il n'y a aujourd'hui AUCUN
// tampon (`InterpolationData` hérité du fork n'a jamais été alimenté : la map
// `m_interpolationData` n'est écrite nulle part). On rend donc les autres joueurs en
// retard d'un délai FIXE — on dispose alors toujours d'un échantillon avant et d'un après
// l'instant à rendre, donc on INTERPOLE (exact) au lieu d'EXTRAPOLER (deviné).
//
// ⚠️ ZÉRO DÉPENDANCE AU MOTEUR, et ce n'est pas un goût de style — c'est la condition de
// deux propriétés qu'on veut :
//   · testable HORS du jeu, donc admettant un vrai test rouge→vert (doctrine D1) sans
//     session Windows — ce qui est rare de ce côté du fil ;
//   · inchangé le jour où le pantin de foule (F-PLY-007) cédera la place au vrai corps
//     joueur : seules les couches PLACEMENT et GESTE dépendent du moteur, pas celle-ci.
// Aucun `#include` de RED4ext ici. Jamais.
//
// Ce fichier ne DÉCIDE de rien côté jeu : il rend une pose. Comment on l'applique
// (téléport à la fréquence de rendu ? commande de marche visant un point devant ?) dépend
// de la sonde S-FLU-1, non encore jouée — voir
// docs/superpowers/specs/2026-08-10-fluidite-avatars-distants-design.md §6.
// =====================================================================================

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Tessera::Sync {

/// Période d'un tick serveur, en secondes (`default_tick_rate_hz()` = 20 Hz côté Rust).
inline constexpr double kPeriodeTickS = 0.05;

/// Délai de rendu des entités distantes. Deux intervalles de snapshot : il faut un
/// échantillon de part et d'autre de l'instant rendu pour interpoler, et un seul
/// intervalle ne laisserait aucune marge à la gigue réseau.
///
/// ⚠️ Ce délai s'AJOUTE à ce qui existe déjà (émission client, tick serveur, RTT/2). Ce
/// n'est pas un défaut : c'est le prix payé une fois pour ne plus jamais deviner. Le
/// joueur local, lui, ne subit aucun retard — il est déplacé par le moteur.
inline constexpr double kDelaiInterpolationS = 0.100;

/// Plafond du délai adaptatif. Au-delà, on cesse d'allonger : un réseau qui exige plus d'un
/// tiers de seconde de tampon a un problème que le tampon ne réglera pas, et le retard visuel
/// deviendrait pire que le symptôme qu'on corrige.
inline constexpr double kDelaiInterpolationMaxS = 0.35;

/// Vitesse de DESCENTE de la gigue retenue (fraction résorbée par snapshot). Volontairement lente :
/// ~0,02 par snapshot, soit quelques secondes pour revenir au plancher après une rafale. La montée,
/// elle, est immédiate — voir `HorlogeRendu::DelaiCourant`.
inline constexpr double kDecrueGigue = 0.02;

/// Au-delà, on cesse d'extrapoler et on FIGE. Extrapoler sans borne fabrique un avatar
/// qui traverse les murs en ligne droite pendant une coupure réseau — un mensonge plus
/// coûteux qu'un arrêt visible.
inline constexpr double kExtrapolationMaxS = 0.25;

/// Plafond de la vitesse DÉRIVÉE, en m/s. Voir `PoserVitesse` — c'est le garde-fou qui empêche
/// une erreur d'échantillon de devenir une téléportation de plusieurs dizaines de mètres.
/// 20 m/s : bien au-dessus du sprint (~8-9 m/s), donc jamais atteint par un joueur à pied.
inline constexpr float kVitesseMaxMS = 20.0f;

/// Écart entre deux échantillons consécutifs au-delà duquel on parle de DISCONTINUITÉ, pas de
/// déplacement. Voir `TamponPose::Pousser`. 15 m à 20 Hz vaudrait 300 m/s.
inline constexpr float kSautFrancM = 15.0f;

/// Écart au-delà duquel l'horloge de rendu SAUTE au lieu de rattraper doucement
/// (chargement de zone, pause, reprise après coupure).
inline constexpr double kEcartRecalageFrancS = 0.5;

/// Fraction de l'écart résorbée par observation. Assez lent pour être invisible, assez
/// vif pour ne pas accumuler une dérive permanente.
inline constexpr double kRattrapageHorloge = 0.1;

/// Profondeur du tampon, en échantillons. À 20 Hz, 8 échantillons = 400 ms d'historique —
/// quatre fois le délai d'interpolation, donc de la marge pour une rafale en retard, sans
/// garder un passé dont personne ne se sert.
inline constexpr std::size_t kProfondeurTampon = 8;

/// Pose telle qu'elle arrive du serveur, déjà déquantifiée. Pas de `frame`/`slot` ici :
/// le serveur résout tout en espace MONDE avant d'émettre (ADR 0013).
struct Pose
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f; // degrés
    std::uint8_t locomotion = 0;
    std::uint8_t moveDir = 0;
};

/// Ce que le tampon rend pour un instant donné.
struct PoseRendue
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    /// Vitesse monde en m/s, dérivée de deux échantillons consécutifs. C'est elle qui
    /// donne gratuitement l'extrapolation bornée ET le point de visée devant l'avatar —
    /// l'équivalent joueur de `NpcState.move_target`, sans nouveau champ de protocole
    /// (le serveur ne connaît pas la destination d'un joueur ; le client la déduit).
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
    std::uint8_t locomotion = 0;
    std::uint8_t moveDir = 0;
    /// Vrai quand la pose est DEVINÉE (tampon à sec) plutôt qu'interpolée entre deux
    /// échantillons réels. L'appelant a le droit de traiter les deux différemment ; il
    /// n'a pas le droit de l'ignorer sans le savoir.
    bool extrapolee = false;
};

/// Ramène un angle en degrés dans [0, 360).
inline float NormaliserDegres(float degres) noexcept
{
    float d = std::fmod(degres, 360.0f);
    return d < 0.0f ? d + 360.0f : d;
}

/// Écart signé le plus COURT de `de` vers `vers`, dans [-180, 180).
///
/// ⚠️ C'est le `TODO` jamais fait du fork (« if we go from 5 -> 355°, we should only do
/// 10°, not 350 »). Sans lui, un avatar qui passe par le nord fait un demi-tour complet
/// sur lui-même à chaque franchissement de zéro.
inline float EcartAngulaire(float de, float vers) noexcept
{
    // `vers - de` ∈ (-360, 360) pour deux angles normalisés, donc +540 est toujours
    // positif et `fmod` ne peut pas rendre de résultat négatif ici.
    return std::fmod(NormaliserDegres(vers) - NormaliserDegres(de) + 540.0f, 360.0f) - 180.0f;
}

/// Interpolation angulaire par le plus court chemin.
inline float LerpAngle(float de, float vers, float t) noexcept
{
    return NormaliserDegres(de + EcartAngulaire(de, vers) * t);
}

/// Horloge de rendu, commune à toutes les entités : à quel instant de la timeline SERVEUR
/// on rend, maintenant.
///
/// Elle n'essaie pas de synchroniser les horloges (pas de NTP, pas d'aller-retour) : elle
/// avance toute seule avec le temps local et se recale doucement sur le dernier tick reçu
/// moins le délai. C'est suffisant parce qu'on ne cherche pas l'heure du serveur — on
/// cherche à consommer les snapshots au rythme où ils arrivent, avec un retard constant.
class HorlogeRendu
{
public:
    /// À appeler à la réception de chaque `Snapshot`, avec son champ `tick`.
    ///
    /// ⚠️ `Snapshot.tick` existe sur le fil depuis le gel du palier 2 et n'est lu NULLE
    /// PART côté client aujourd'hui. C'est la donnée dont tout ce fichier dépend.
    void ObserverSnapshot(std::uint64_t tick) noexcept
    {
        if (m_amorcee)
        {
            ObserverIntervalle(m_depuisDernierSnapshot);
        }
        m_depuisDernierSnapshot = 0.0;
        const double cible = static_cast<double>(tick) * kPeriodeTickS - DelaiCourant();
        if (!m_amorcee)
        {
            m_tempsRendu = cible;
            m_amorcee = true;
            return;
        }
        const double ecart = cible - m_tempsRendu;
        if (std::fabs(ecart) > kEcartRecalageFrancS)
        {
            // Trop loin pour être du bruit : c'est une discontinuité (chargement, reprise).
            // Rattraper doucement mettrait des secondes et donnerait un avatar au ralenti.
            m_tempsRendu = cible;
        }
        else
        {
            m_tempsRendu += ecart * kRattrapageHorloge;
        }
    }

    /// À appeler une fois par frame, avec le delta local.
    void Avancer(double dt) noexcept
    {
        if (m_amorcee)
        {
            m_tempsRendu += dt;
            m_depuisDernierSnapshot += dt;
        }
    }

    [[nodiscard]] double TempsRendu() const noexcept { return m_tempsRendu; }
    [[nodiscard]] bool Amorcee() const noexcept { return m_amorcee; }

    /// Délai d'interpolation EFFECTIF, en secondes. Grandit avec la gigue observée.
    ///
    /// ── POURQUOI IL NE PEUT PAS ÊTRE FIXE ──────────────────────────────────────────────
    ///
    /// Un délai fixe est un pari sur la régularité du réseau. Il tient tant que les snapshots
    /// arrivent à intervalle régulier, et il lâche exactement quand ça compte : Lucas l'a observé
    /// le 2026-08-13 — « quand plusieurs instances chargent, il y a des micro-coupures ». Un
    /// chargement fait hoqueter l'émission ; le tampon s'assèche ; on extrapole ; ça flotte.
    ///
    /// La gigue mesurée ici, c'est l'écart entre l'intervalle THÉORIQUE de deux snapshots (50 ms)
    /// et leur intervalle RÉEL d'arrivée. On en garde le pire récent, amorti, et on s'assure que
    /// le délai le couvre — plus une marge d'un intervalle.
    ///
    /// ⚠️ Il MONTE vite et DESCEND lentement, et l'asymétrie est le cœur du réglage. Monter vite,
    /// c'est absorber la rafale dès le premier symptôme. Descendre lentement, c'est ne pas
    /// re-tomber dans le trou au premier calme trompeur — et surtout ne pas faire varier le retard
    /// visuel en permanence, ce qui se verrait comme un ralenti/accéléré.
    [[nodiscard]] double DelaiCourant() const noexcept
    {
        const double adaptatif = m_gigue + kPeriodeTickS;
        const double d = adaptatif > kDelaiInterpolationS ? adaptatif : kDelaiInterpolationS;
        return d > kDelaiInterpolationMaxS ? kDelaiInterpolationMaxS : d;
    }

    /// Gigue courante retenue, en secondes — exposée pour le diagnostic.
    [[nodiscard]] double Gigue() const noexcept { return m_gigue; }

private:
    /// Met à jour la gigue depuis l'intervalle réel d'arrivée de deux snapshots.
    void ObserverIntervalle(double intervalleReel) noexcept
    {
        const double ecart = intervalleReel - kPeriodeTickS;
        const double retard = ecart > 0.0 ? ecart : 0.0;
        // Montée immédiate, descente amortie : voir `DelaiCourant`.
        m_gigue = retard > m_gigue ? retard : m_gigue + (retard - m_gigue) * kDecrueGigue;
    }

    double m_tempsRendu = 0.0;
    bool m_amorcee = false;
    double m_gigue = 0.0;
    double m_depuisDernierSnapshot = 0.0;
};

/// Historique récent d'UNE entité réseau, et l'échantillonnage qui en tire une pose.
class TamponPose
{
public:
    /// Range un échantillon. Les ticks périmés ou déjà vus sont ignorés en silence : un
    /// snapshot en retard n'a rien à dire qu'on ne sache déjà, et le canal est
    /// délibérément non fiable côté serveur (`SendFlags::UNRELIABLE`), donc le désordre
    /// est le régime NORMAL, pas une anomalie.
    void Pousser(std::uint64_t tick, const Pose& pose) noexcept
    {
        if (m_nombre > 0 && tick <= DernierTick())
        {
            return;
        }
        // ── UNE DISCONTINUITÉ SE COUPE, ELLE NE S'INTERPOLE PAS ────────────────────────────
        //
        // Un saut franc — téléportation, ascenseur, sortie puis retour dans l'AoI, ou simplement
        // un paquet désordonné depuis le passage en non-fiable — n'est PAS un déplacement. Le
        // traiter comme tel fait GLISSER l'avatar d'un point à l'autre en ligne droite, à travers
        // les murs, sur toute la distance. C'est ce que Lucas décrit comme des « effets fantômes ».
        //
        // On jette donc l'historique et on repart de la nouvelle pose : l'avatar est replacé net.
        // Une coupure franche est honnête ; un glissement de trente mètres à travers un immeuble
        // ne l'est pas.
        //
        // 15 m entre deux échantillons consécutifs : à 20 Hz, aucun déplacement humain n'y arrive
        // (ce serait 300 m/s), et c'est bien en dessous du seuil de téléport franc de l'anti-triche
        // serveur (200 m) — on coupe donc AVANT que le serveur ne juge, jamais après.
        if (m_nombre > 0)
        {
            const Entree& dernier = At(m_nombre - 1);
            const float dx = pose.x - dernier.pose.x;
            const float dy = pose.y - dernier.pose.y;
            const float dz = pose.z - dernier.pose.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > kSautFrancM)
            {
                m_nombre = 0;
                m_tete = 0;
            }
        }
        m_entrees[m_tete] = Entree{tick, pose};
        m_tete = (m_tete + 1) % kProfondeurTampon;
        if (m_nombre < kProfondeurTampon)
        {
            ++m_nombre;
        }
    }

    /// Rend la pose à l'instant `tempsRendu` (timeline serveur, secondes).
    /// Faux si le tampon est vide — l'appelant ne doit alors RIEN afficher de nouveau.
    [[nodiscard]] bool Echantillonner(double tempsRendu, PoseRendue& sortie) const noexcept
    {
        if (m_nombre == 0)
        {
            return false;
        }
        if (m_nombre == 1)
        {
            const Entree& seule = At(0);
            sortie = Figee(seule.pose);
            sortie.extrapolee = tempsRendu > Temps(seule.tick);
            return true;
        }

        // Trop en retard : on est avant le plus ancien échantillon retenu. On FIGE sur lui
        // plutôt que d'inventer un passé qu'on n'a pas — le tampon a débordé, l'avatar
        // rattrapera au prochain échantillon.
        if (tempsRendu <= Temps(At(0).tick))
        {
            sortie = Figee(At(0).pose);
            return true;
        }

        // Cas nominal : deux échantillons encadrent l'instant demandé.
        for (std::size_t i = 0; i + 1 < m_nombre; ++i)
        {
            const Entree& avant = At(i);
            const Entree& apres = At(i + 1);
            const double tAvant = Temps(avant.tick);
            const double tApres = Temps(apres.tick);
            if (tempsRendu <= tApres)
            {
                const double duree = tApres - tAvant;
                const float t = duree > 0.0 ? static_cast<float>((tempsRendu - tAvant) / duree) : 0.0f;
                sortie = Entre(avant, apres, t, duree);
                sortie.extrapolee = false;
                return true;
            }
        }

        // Tampon à sec : on continue le mouvement à partir des deux derniers, borné.
        const Entree& avantDernier = At(m_nombre - 2);
        const Entree& dernier = At(m_nombre - 1);
        const double duree = Temps(dernier.tick) - Temps(avantDernier.tick);
        const double depuis = tempsRendu - Temps(dernier.tick);
        const double borne = depuis > kExtrapolationMaxS ? kExtrapolationMaxS : depuis;
        sortie = Prolongee(avantDernier, dernier, borne, duree);
        sortie.extrapolee = true;
        return true;
    }

    [[nodiscard]] std::size_t Nombre() const noexcept { return m_nombre; }

    /// Âge du dernier échantillon reçu, en secondes, à l'instant `tempsRendu`.
    ///
    /// C'est la mesure de SANTÉ DU FIL pour cette entité, et elle est distincte de
    /// `PoseRendue::extrapolee` : extrapoler 50 ms est normal (un paquet perdu sur un canal
    /// délibérément non fiable), un fil muet depuis une demi-seconde ne l'est pas. Confondre les
    /// deux fait soit figer un avatar qui va bien, soit laisser courir un avatar dont plus
    /// personne ne donne de nouvelles.
    ///
    /// Négatif si l'instant demandé précède le dernier échantillon — c'est le cas NOMINAL, puisque
    /// l'on rend volontairement en retard de `kDelaiInterpolationS`.
    [[nodiscard]] double AgeDuDernierEchantillon(double tempsRendu) const noexcept
    {
        return m_nombre == 0 ? 0.0 : tempsRendu - Temps(DernierTick());
    }

    [[nodiscard]] std::uint64_t DernierTick() const noexcept
    {
        return m_nombre == 0 ? 0 : At(m_nombre - 1).tick;
    }

private:
    struct Entree
    {
        std::uint64_t tick = 0;
        Pose pose{};
    };

    static double Temps(std::uint64_t tick) noexcept
    {
        return static_cast<double>(tick) * kPeriodeTickS;
    }

    /// `i` = 0 pour le plus ANCIEN échantillon retenu, `m_nombre - 1` pour le plus récent.
    [[nodiscard]] const Entree& At(std::size_t i) const noexcept
    {
        const std::size_t base = (m_tete + kProfondeurTampon - m_nombre) % kProfondeurTampon;
        return m_entrees[(base + i) % kProfondeurTampon];
    }

    static PoseRendue Figee(const Pose& p) noexcept
    {
        PoseRendue r;
        r.x = p.x;
        r.y = p.y;
        r.z = p.z;
        r.yaw = NormaliserDegres(p.yaw);
        r.locomotion = p.locomotion;
        r.moveDir = p.moveDir;
        return r;
    }

    /// L'état d'animation (`locomotion`, `moveDir`) est pris sur l'échantillon COURANT à
    /// cet instant, c'est-à-dire le PRÉCÉDENT : une posture est ce qu'elle était, pas ce
    /// qu'elle sera. L'interpoler n'aurait aucun sens — ce sont des états discrets.
    static PoseRendue Entre(const Entree& a, const Entree& b, float t, double duree) noexcept
    {
        PoseRendue r;
        r.x = a.pose.x + (b.pose.x - a.pose.x) * t;
        r.y = a.pose.y + (b.pose.y - a.pose.y) * t;
        r.z = a.pose.z + (b.pose.z - a.pose.z) * t;
        r.yaw = LerpAngle(a.pose.yaw, b.pose.yaw, t);
        r.locomotion = a.pose.locomotion;
        r.moveDir = a.pose.moveDir;
        PoserVitesse(r, a, b, duree);
        return r;
    }

    static PoseRendue Prolongee(const Entree& a, const Entree& b, double depuis, double duree) noexcept
    {
        PoseRendue r = Figee(b.pose);
        PoserVitesse(r, a, b, duree);
        r.x += static_cast<float>(r.vx * depuis);
        r.y += static_cast<float>(r.vy * depuis);
        r.z += static_cast<float>(r.vz * depuis);
        return r;
    }

    /// ⚠️ LA VITESSE EST BRIDÉE, ET C'EST LE GARDE-FOU LE PLUS IMPORTANT DU FICHIER.
    ///
    /// Elle est DÉRIVÉE de deux échantillons : une erreur sur l'un des deux devient une vitesse
    /// aberrante, et l'extrapolation la multiplie ensuite par un temps. Sans bride, une vitesse de
    /// 100 m/s (5 m d'écart lus sur un intervalle de 50 ms) produit **25 m de saut** à la borne
    /// d'extrapolation. C'est ce que Lucas a observé le 2026-08-13 : « de grosses téléportations de
    /// plusieurs dizaines de mètres », avec du rollback et des effets fantômes.
    ///
    /// D'où vient l'erreur : depuis que le flux de position part en NON-FIABLE, un paquet peut
    /// arriver dans le désordre. Le serveur applique alors une position plus ancienne, le snapshot
    /// la relaie, et deux échantillons consécutifs se retrouvent séparés par un grand écart. La
    /// dérivée explose. Le non-fiable est le bon choix (voir `SendPositionUpdate`), mais il EXIGE
    /// ce garde-fou — je l'ai livré sans, et c'est la régression.
    ///
    /// 20 m/s : bien au-dessus du sprint (~8-9 m/s), donc jamais atteint par un joueur à pied — ce
    /// chemin ne traite QUE des avatars bipèdes, les véhicules ont leur propre table. Assez bas
    /// pour borner l'extrapolation à 5 m à la borne de 250 ms.
    static void PoserVitesse(PoseRendue& r, const Entree& a, const Entree& b, double duree) noexcept
    {
        if (duree <= 0.0)
        {
            return;
        }
        const float inv = static_cast<float>(1.0 / duree);
        float vx = (b.pose.x - a.pose.x) * inv;
        float vy = (b.pose.y - a.pose.y) * inv;
        float vz = (b.pose.z - a.pose.z) * inv;
        const float norme = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (norme > kVitesseMaxMS)
        {
            // On garde la DIRECTION et on ramène la norme : une vitesse aberrante reste un
            // mouvement dans un sens plausible, et l'écraser à zéro figerait un avatar qui court
            // vraiment. Ce qu'on refuse, c'est de la CROIRE sur sa magnitude.
            const float k = kVitesseMaxMS / norme;
            vx *= k;
            vy *= k;
            vz *= k;
        }
        r.vx = vx;
        r.vy = vy;
        r.vz = vz;
    }

    Entree m_entrees[kProfondeurTampon]{};
    std::size_t m_tete = 0;   // prochaine case à écrire
    std::size_t m_nombre = 0; // échantillons valides
};

/// Point à viser DEVANT l'avatar, le long de sa vitesse — l'équivalent joueur de
/// `NpcState.move_target`.
///
/// Pourquoi c'est nécessaire : une commande de marche vers la position serveur vise un
/// point à 15 cm (à 3 m/s et 20 Hz). Le pantin l'atteint instantanément et s'arrête. La
/// mesure du 2026-08-06 l'a établi pour les PNJ (« ils frémissent sur place ») et le
/// correctif — `move_target` — n'a jamais été porté aux joueurs, faute de champ dans
/// `PlayerState`. On n'en ajoute pas : le client a la vitesse, donc il a la direction.
///
/// Rend la position telle quelle si l'entité est trop lente pour qu'une direction ait un
/// sens — viser un point tiré d'un bruit de mesure ferait tourner l'avatar sur place.
inline void PointDeVisee(const PoseRendue& pose, float metres, float& outX, float& outY, float& outZ) noexcept
{
    const float vitesse = std::sqrt(pose.vx * pose.vx + pose.vy * pose.vy + pose.vz * pose.vz);
    constexpr float kVitesseMiniMS = 0.1f;
    if (vitesse < kVitesseMiniMS)
    {
        outX = pose.x;
        outY = pose.y;
        outZ = pose.z;
        return;
    }
    const float k = metres / vitesse;
    outX = pose.x + pose.vx * k;
    outY = pose.y + pose.vy * k;
    outZ = pose.z + pose.vz * k;
}

} // namespace Tessera::Sync

#pragma once
// =====================================================================================
// HORLOGE SERVEUR — ce qui rend comparables deux journaux écrits sur deux machines
// =====================================================================================
//
// POURQUOI CE FICHIER EXISTE. Demandé par Lucas le 2026-08-15, pour le playtest à une
// cinquantaine de joueurs : « il faudra peut-être la synchroniser avec un timestamp,
// trouver une solution avec un ping pour qu'on puisse calibrer le moment où le ping
// arrive […] ça commence à telle heure sur le serveur et à telle heure sur le client
// local, et du coup on peut synchroniser avec un calcul mathématique le timestamp ».
//
// ── CE QUE ÇA REMPLACE, ET POURQUOI C'ÉTAIT INSUFFISANT ─────────────────────────────
//
// `Telemetrie.h` dit, noir sur blanc : « Les deux instances de test tournent sur LA MÊME
// MACHINE, donc sur LA MÊME horloge murale. Pas besoin de synchroniser quoi que ce soit ».
// C'était exact pour deux fenêtres de jeu sur le PC de Lucas — et ça cesse de l'être à la
// seconde où le playtest met cinquante joueurs sur cinquante PC. Deux horloges Windows non
// synchronisées dérivent couramment de plusieurs secondes ; l'instrument phare du chantier
// (la latence bout-en-bout p50, mesurée comme une soustraction d'horodatages) ne
// transférerait pas, et **il mentirait sans le dire** : les deux fichiers auraient l'air
// parfaits, et l'écart mesuré serait la dérive des horloges, pas la latence du réseau.
//
// ── LA MÉTHODE, ET POURQUOI ELLE N'A PAS BESOIN D'ALLER-RETOUR ──────────────────────
//
// Le serveur estampille chaque `Snapshot` de son horloge murale (`Snapshot.ts_ms`, ajouté
// au protocole le 2026-08-15). À la réception, le client mesure :
//
//     delta = reception_locale_ms - ts_ms_serveur
//           = décalage_des_horloges + délai_aller
//
// `délai_aller` est toujours POSITIF et varie ; `décalage` est ~constant. Donc le MINIMUM
// des `delta` observés sur une fenêtre vaut `décalage + le plus petit délai aller vu`.
// C'est le meilleur estimateur possible sans horodatage d'émission côté client, et c'est
// exactement le principe de NTP réduit à sa moitié utile.
//
// Ce que ça donne : `TempsServeurMs()` = horloge locale − décalage estimé. L'erreur
// résiduelle est le plus petit délai aller de la fenêtre — quelques millisecondes sur un
// bon lien, quelques dizaines sinon. À 50 Hz de simulation, c'est un à deux ticks : assez
// pour dater un événement, pas pour arbitrer un tir. On ne prétend rien de plus.
//
// ⚠️ **On n'utilise PAS le ping de GNS pour ça, et c'est délibéré.** Le RTT donne la
// LARGEUR de l'incertitude, jamais le SENS du décalage : deux horloges désynchronisées
// d'une heure donnent le même ping que deux horloges parfaites. Le ping est journalisé à
// côté comme mesure de qualité du lien, jamais comme terme de la correction.
//
// ── POURQUOI UNE FENÊTRE GLISSANTE, ET PAS UN MINIMUM DEPUIS TOUJOURS ───────────────
//
// Un minimum absolu serait piégé par le premier paquet chanceux et ne suivrait jamais la
// dérive réelle des quartz (quelques dizaines de ppm, soit plusieurs centaines de ms sur
// une session de trois heures — exactement la durée d'un playtest). Une fenêtre glissante
// oublie, donc elle suit. Elle encaisse aussi les sauts francs (NTP qui recale la machine
// du joueur en cours de partie, sortie de veille) en quelques secondes au lieu de rester
// fausse jusqu'à la déconnexion.
//
// ZÉRO DÉPENDANCE AU MOTEUR — même règle que `TamponInterpolation.h`, et pour la même
// raison : ce fichier est de l'arithmétique, donc il se teste hors du jeu.
// =====================================================================================

#include <cstddef>
#include <cstdint>

namespace Tessera::Sync
{

/// Profondeur de la fenêtre d'observation, en échantillons.
///
/// 128 snapshots ≈ 5 secondes à 25 Hz de diffusion. Assez long pour qu'un creux de délai
/// aller (le paquet qui a eu de la chance) tombe dans la fenêtre — c'est lui qui porte
/// l'estimation. Assez court pour suivre la dérive et encaisser un recalage NTP en
/// quelques secondes.
inline constexpr std::size_t kFenetreHorloge = 128;

/// Estimateur du décalage entre l'horloge locale et celle du serveur.
///
/// Usage : `Observer(ts_ms_du_snapshot, maintenant_local_ms)` à chaque snapshot reçu, puis
/// `TempsServeurMs(maintenant_local_ms)` pour dater n'importe quel événement local sur la
/// timeline du serveur.
class HorlogeServeur
{
public:
    /// À appeler à la réception de chaque snapshot portant un `ts_ms` non nul.
    ///
    /// ⚠️ Un `ts_ms` à 0 signifie « serveur plus ancien que ce champ » (FlatBuffers rend le
    /// défaut quand le champ est absent). On l'IGNORE au lieu de l'interpréter : le prendre
    /// pour une date placerait le serveur au 1er janvier 1970 et l'estimation deviendrait
    /// une aberration de cinquante ans, propagée dans chaque ligne de journal.
    void Observer(std::uint64_t tsServeurMs, std::uint64_t receptionLocaleMs) noexcept
    {
        if (tsServeurMs == 0)
        {
            return;
        }
        // Signé : l'horloge locale peut être EN RETARD sur celle du serveur, et un `uint64`
        // ferait alors un tour complet. C'est le genre de dépassement qui ne se voit pas en
        // test (les deux horloges d'une même machine sont égales) et qui explose en playtest.
        const std::int64_t delta =
            static_cast<std::int64_t>(receptionLocaleMs) - static_cast<std::int64_t>(tsServeurMs);

        m_fenetre[m_tete] = delta;
        m_tete = (m_tete + 1) % kFenetreHorloge;
        if (m_nombre < kFenetreHorloge)
        {
            ++m_nombre;
        }
        ++m_observations;

        // Recalcul du minimum sur toute la fenêtre. O(128) par snapshot, à 25 Hz : négligeable,
        // et surtout CORRECT — un minimum incrémental ne sait pas oublier la valeur qui sort de
        // la fenêtre, donc il resterait collé au meilleur échantillon de toute la session et la
        // fenêtre glissante ne servirait plus à rien. C'est la simplicité qui garde la propriété.
        std::int64_t mini = m_fenetre[0];
        std::int64_t maxi = m_fenetre[0];
        for (std::size_t i = 1; i < m_nombre; ++i)
        {
            if (m_fenetre[i] < mini)
            {
                mini = m_fenetre[i];
            }
            if (m_fenetre[i] > maxi)
            {
                maxi = m_fenetre[i];
            }
        }
        m_decalageMs = mini;
        // L'étalement de la fenêtre est la BARRE D'ERREUR de l'estimation, et il doit voyager
        // avec elle. Sans lui, un journal donne une date sans jamais dire à quel point elle est
        // sûre — et on finirait par lire des écarts de 200 ms comme des faits alors qu'ils
        // tiennent dans l'incertitude.
        m_etalementMs = maxi - mini;
    }

    /// Instant `maintenantLocalMs` exprimé sur l'horloge du SERVEUR.
    ///
    /// Rend l'heure locale telle quelle tant qu'aucun snapshot n'a été observé : sans
    /// observation on n'a pas d'estimation, et inventer une correction serait pire que ne pas
    /// corriger. `EstAmorcee()` dit lequel des deux régimes on lit.
    [[nodiscard]] std::uint64_t TempsServeurMs(std::uint64_t maintenantLocalMs) const noexcept
    {
        if (m_nombre == 0)
        {
            return maintenantLocalMs;
        }
        const std::int64_t corrige = static_cast<std::int64_t>(maintenantLocalMs) - m_decalageMs;
        return corrige < 0 ? 0 : static_cast<std::uint64_t>(corrige);
    }

    /// Décalage estimé, en ms (`horloge_locale - horloge_serveur`, délai aller minimal inclus).
    [[nodiscard]] std::int64_t DecalageMs() const noexcept { return m_decalageMs; }

    /// Étalement des deltas dans la fenêtre — la barre d'erreur. Un chiffre élevé veut dire que
    /// le délai aller varie beaucoup, donc que la datation est floue d'autant.
    [[nodiscard]] std::int64_t EtalementMs() const noexcept { return m_etalementMs; }

    [[nodiscard]] bool EstAmorcee() const noexcept { return m_nombre > 0; }

    /// Nombre total de snapshots datés observés — pour distinguer « estimation jeune » (deux ou
    /// trois échantillons, peu fiable) de « estimation mûre » (fenêtre pleine).
    [[nodiscard]] std::uint64_t Observations() const noexcept { return m_observations; }

    [[nodiscard]] std::size_t Echantillons() const noexcept { return m_nombre; }

private:
    std::int64_t m_fenetre[kFenetreHorloge]{};
    std::size_t m_tete = 0;
    std::size_t m_nombre = 0;
    std::uint64_t m_observations = 0;
    std::int64_t m_decalageMs = 0;
    std::int64_t m_etalementMs = 0;
};

} // namespace Tessera::Sync

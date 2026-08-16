#pragma once

// ─────────────────────────────────────────────────────────────────────────────────────────────
// TÉLÉMÉTRIE DE RENDU — le journal qui remplace « ça a l'air mieux » par un chiffre
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// POURQUOI CE FICHIER EXISTE. Demandé par Lucas le 2026-08-14 : « est-ce qu'il y a un moyen de
// mettre des outils de métriques côté serveur et côté client, pour que tu ne sois pas assujetti
// qu'à mon appréciation visuelle et que tu puisses te débugger en autonomie ».
//
// Le besoin est réel et il est démontré par la panne du matin même : `EXTRAPOLE` affiché avec un
// tampon PLEIN (`ech=48`) est une contradiction — et c'est elle, pas l'œil, qui a nommé la cause
// (une cadence serveur restée à 20 Hz). L'œil avait dit « une à deux secondes de délai », ce qui
// désignait la bonne zone sans jamais pouvoir désigner le mécanisme.
//
// ── CE QUI A CHANGÉ LE 2026-08-15 : LE PASSAGE À L'ÉCHELLE DU PLAYTEST ────────────────────────
//
// Ce module a été écrit pour DEUX instances sur LA MÊME machine. Il partait de là :
//
//     « Les deux instances de test tournent sur LA MÊME MACHINE, donc sur LA MÊME horloge
//       murale. Pas besoin de synchroniser quoi que ce soit. »
//
// C'était exact, et ça cesse de l'être pour un playtest à une cinquantaine de joueurs sur
// cinquante PC. Trois choses en découlent, et elles sont toutes les trois ici :
//
//   1. **Un temps SERVEUR à côté du temps local.** Deux horloges Windows dérivent couramment de
//      plusieurs secondes. Sans référence commune, une soustraction d'horodatages ne mesure plus
//      la latence, elle mesure la dérive des horloges — et elle le fait SANS LE DIRE : les deux
//      fichiers ont l'air parfaits. Voir `HorlogeServeur.h` pour la méthode d'estimation.
//   2. **Un dossier à nous.** Les journaux vivaient dans `red4ext/logs`, mélangés aux journaux du
//      chargeur de mods. Pour que le launcher puisse les ramasser (et les purger) sans risquer
//      d'emporter autre chose, ils ont désormais un dossier qui n'appartient qu'à nous.
//   3. **Un plafond d'écriture.** À 200 voisins et 60 fps, la ligne « rendu » par avatar et par
//      frame ferait **12 000 lignes par seconde**. L'instrument deviendrait sa propre charge, et
//      fausserait ce qu'il prétend mesurer. Voir `kPeriodeRenduParEntiteS`.
//
// ── FORMAT ────────────────────────────────────────────────────────────────────────────────────
//
// JSONL : une ligne = un objet JSON = un événement. Pas de tableau englobant, donc un fichier
// tronqué (le jeu qu'on tue) reste exploitable jusqu'à sa dernière ligne complète — ce qui arrive
// à CHAQUE session, puisqu'on ferme le jeu par `Stop-Process`.
//
// Un fichier PAR PROCESSUS (le PID est dans le nom) : deux instances sur une même machine
// partagent le dossier du jeu, et deux écrivains sur un même fichier produiraient un mélange
// illisible. C'est le même piège que le marqueur `tessera-dev.flag` partagé, qui a coûté une
// demi-heure le 2026-08-09.
//
// Chaque ligne porte DEUX horodatages, et il faut savoir lequel lire :
//   · `t`  — horloge locale de la machine, ms Unix. Toujours présent, jamais corrigé.
//   · `ts` — le même instant sur l'horloge du SERVEUR. C'est celui qui se compare d'une machine
//            à l'autre. Égal à `t` tant que l'estimateur n'est pas amorcé.
// Les deux sont écrits parce que garder le brut permet de re-corriger après coup si l'estimation
// se révèle mauvaise ; n'écrire que le corrigé rendrait l'erreur irrattrapable.
//
// ⚠️ Ce module n'écrit RIEN tant que `Demarrer` n'a pas été appelé — donc rien en production, où
// personne ne le démarre. Une télémétrie qui s'active toute seule est un coût imposé au joueur.

#include "HorlogeServeur.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>

namespace Tessera::Sync
{

/// Dossier des journaux de playtest, relatif à la racine du jeu.
///
/// ⚠️ **C'est un contrat avec le launcher**, qui ramasse et purge ce dossier après la session.
/// Il doit donc n'appartenir qu'à nous : y mélanger les journaux d'un autre outil ferait
/// disparaître les siens à la première collecte. C'est la raison de ne PAS réutiliser
/// `red4ext/logs`, où ils vivaient jusqu'au 2026-08-15.
inline constexpr const char* kDossierJournaux = "TesseraLogs";

/// Période minimale entre deux lignes `rx` POUR UNE MÊME ENTITÉ, en secondes.
///
/// ── POURQUOI UN PLAFOND, ET POURQUOI PAR ENTITÉ ──────────────────────────────────────────────
///
/// Sans lui : 200 voisins (plafond depuis le 2026-08-15) × 60 fps = **12 000 lignes/s**, soit
/// ~1,5 Mo/s et autant d'appels à `fflush`. L'instrument coûterait alors plus cher que ce qu'il
/// mesure, et la première chose qu'il fausserait serait justement le temps de frame.
///
/// 100 ms = 10 lignes/s par entité. À 200 voisins ça fait 2 000 lignes/s en pointe, et surtout
/// **une trace régulière de chaque avatar** — ce qui est ce dont on a besoin : la dérive, les
/// recalages et les téléports se lisent sur une tendance, pas sur chaque frame.
///
/// ⚠️ Le plafond est PAR ENTITÉ, jamais global. Un plafond global laisserait les avatars les plus
/// nombreux évincer les autres du journal, et un avatar absent du fichier est indiscernable d'un
/// avatar qui va bien — l'erreur d'instrument que ce module existe précisément pour éviter.
inline constexpr double kPeriodeRenduParEntiteS = 0.100;

/// Écrivain JSONL. Une instance globale, protégée par un mutex : `PiloterAvatar` tourne sur le
/// thread de rendu et `HandleSnapshot` sur celui du réseau — mesuré le 2026-08-14, les lignes du
/// journal client portent des identifiants de thread différents.
class Telemetrie
{
public:
    /// Ouvre le fichier. Sans cet appel, tout le reste est un no-op silencieux.
    ///
    /// ⚠️ LE RÉPERTOIRE COURANT N'EST PAS CELUI QU'ON CROIT. Le jeu se lance avec
    /// `-WorkingDirectory <racine du jeu>/bin/x64` (obligatoire, sinon `Cyberverse.Red4Ext.dll` ne
    /// charge pas) — mais rien ne garantit qu'il l'y laisse. Un chemin relatif unique aurait donc
    /// marché ou non selon une condition qu'on ne contrôle pas, et l'échec aurait été SILENCIEUX :
    /// un `fopen` qui rend `nullptr`, un journal vide, et un verdict qui dit « aucun journal » sans
    /// jamais dire pourquoi.
    ///
    /// On essaie donc les deux emplacements plausibles et on garde le premier qui s'ouvre.
    ///
    /// `CheminUtilise()` dit lequel a gagné — parce qu'un instrument doit pouvoir répondre « où
    /// écris-tu ? » sans qu'on ait à le deviner.
    void Demarrer(const std::string& dossier, std::uint32_t pid) noexcept
    {
        std::lock_guard<std::mutex> verrou(m_mutex);
        if (m_fichier != nullptr)
        {
            return;
        }
        const std::string suffixe = "/tessera-telemetrie-" + std::to_string(pid) + ".jsonl";
        // ── LE REPLI NE MARCHE QUE SI LA PREMIÈRE BRANCHE PEUT ÉCHOUER ────────────────────
        //
        // ⚠️ DÉFAUT MESURÉ LE 2026-08-15, première session réelle : les journaux atterrissaient
        // dans `bin/x64/TesseraLogs` au lieu de la racine du jeu.
        //
        // Le mécanisme d'origine essayait deux emplacements et gardait le premier qui s'ouvrait.
        // C'était correct tant que le premier POUVAIT échouer : avec `red4ext/logs`, le dossier
        // n'existait pas sous `bin/x64`, donc `fopen` rendait `nullptr` et le repli `../../`
        // s'exécutait. En ajoutant la création du dossier — nécessaire, puisque celui-ci est neuf
        // et que personne d'autre ne le crée — le premier candidat réussit **toujours**, et le
        // repli est devenu du code mort.
        //
        // *Un correctif qui rend infaillible la première branche d'un test supprime la seconde.*
        //
        // Le launcher, lui, ramasse à la RACINE du jeu (`journaux_playtest.rs`). Un journal écrit
        // deux dossiers plus bas n'aurait jamais été collecté, et rien ne l'aurait signalé : la
        // collecte aurait simplement rendu « aucun journal », ce qui est exactement ce que dit une
        // machine où personne n'a joué.
        //
        // On ne DEVINE donc plus l'emplacement : on essaie d'abord la racine du jeu explicitement
        // (`../../` depuis `bin/x64`, le répertoire de travail imposé au lancement), et on ne
        // retombe sur le chemin nu que si celui-là échoue — c'est-à-dire si le jeu a été lancé
        // depuis sa racine.
        for (const char* prefixe : {"../../", ""})
        {
            const std::string racine = std::string(prefixe) + dossier;
            // ⚠️ CRÉER LE DOSSIER, ET NE PAS SUPPOSER QU'IL EXISTE. C'est la différence avec
            // `red4ext/logs`, qui existait forcément puisque le chargeur de mods y écrivait déjà.
            // Un dossier À NOUS n'a personne pour le créer : sans cet appel, `fopen` rendrait
            // `nullptr` et la télémétrie serait muette sans qu'une seule ligne ne le dise.
            CreerDossier(racine);
            const std::string chemin = racine + suffixe;
            // "wb" et non "ab" : chaque lancement repart d'un fichier neuf. Un journal qui accumule
            // plusieurs sessions oblige à deviner où l'une finit — et on compare alors des chiffres
            // qui ne parlent pas de la même exécution.
            m_fichier = std::fopen(chemin.c_str(), "wb");
            if (m_fichier != nullptr)
            {
                m_chemin = chemin;
                return;
            }
        }
    }

    /// Chemin réellement ouvert, vide si aucun. À journaliser au démarrage.
    [[nodiscard]] const std::string& CheminUtilise() const noexcept { return m_chemin; }

    void Arreter() noexcept
    {
        std::lock_guard<std::mutex> verrou(m_mutex);
        if (m_fichier != nullptr)
        {
            std::fclose(m_fichier);
            m_fichier = nullptr;
        }
    }

    [[nodiscard]] bool Active() const noexcept { return m_fichier != nullptr; }

    /// Millisecondes depuis l'époque Unix, horloge LOCALE de cette machine.
    ///
    /// `system_clock` et surtout PAS `steady_clock` : l'origine de `steady_clock` est arbitraire
    /// par processus, donc deux instances n'auraient aucun point commun — et l'erreur serait
    /// invisible, les deux fichiers auraient l'air parfaits.
    [[nodiscard]] static std::int64_t Maintenant() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /// Branche l'estimateur d'horloge serveur. Sans lui, `ts` vaut `t` : le journal reste
    /// exploitable localement, mais il ne se compare plus à celui d'une autre machine.
    void BrancherHorloge(const HorlogeServeur* horloge) noexcept
    {
        std::lock_guard<std::mutex> verrou(m_mutex);
        m_horloge = horloge;
    }

    /// EN-TÊTE DE SESSION — la première ligne du fichier, et celle sans laquelle le reste ne se
    /// rattache à rien.
    ///
    /// ⚠️ **C'est ce qui rend cinquante journaux exploitables au lieu de cinquante fichiers
    /// anonymes.** Sans elle, un fichier ramassé par le launcher ne dit ni de qui il vient, ni
    /// contre quel serveur, ni avec quelle version — donc on ne peut ni l'apparier avec le journal
    /// serveur, ni écarter les sessions d'un build périmé. Sur une soirée à cinquante joueurs,
    /// c'est la différence entre un corpus et un tas.
    void Session(const char* compte, const char* serveur, const char* version,
                 std::uint32_t protocole) noexcept
    {
        Ecrire("{\"t\":%lld,\"ts\":%lld,\"k\":\"session\",\"compte\":\"%s\",\"serveur\":\"%s\","
               "\"version\":\"%s\",\"protocole\":%u}\n",
               static_cast<long long>(Maintenant()), static_cast<long long>(TempsServeur()),
               compte != nullptr ? compte : "", serveur != nullptr ? serveur : "",
               version != nullptr ? version : "", protocole);
    }

    /// L'ÉTAT DE L'HORLOGE — écrit périodiquement, et c'est la ligne qui permet de RE-CORRIGER
    /// tout le fichier après coup si l'estimation s'avère mauvaise.
    ///
    /// `decalage` = horloge locale − horloge serveur (délai aller minimal inclus).
    /// `etalement` = la barre d'erreur : de combien la datation est floue.
    /// `ping` = RTT rapporté par le transport, en ms. Il ne CORRIGE rien (un RTT ne donne pas le
    /// sens du décalage) — il dit la qualité du lien, et il permet de séparer « ce joueur a une
    /// mauvaise connexion » de « ce joueur a une horloge fausse ». Deux causes, même symptôme.
    void Horloge(std::int64_t decalage, std::int64_t etalement, std::uint64_t observations,
                 int ping) noexcept
    {
        Ecrire("{\"t\":%lld,\"ts\":%lld,\"k\":\"horloge\",\"decal\":%lld,\"etal\":%lld,"
               "\"obs\":%llu,\"ping\":%d}\n",
               static_cast<long long>(Maintenant()), static_cast<long long>(TempsServeur()),
               static_cast<long long>(decalage), static_cast<long long>(etalement),
               static_cast<unsigned long long>(observations), ping);
    }

    /// CE QUE J'ÉMETS — la pose du joueur local, au moment où elle part sur le fil.
    ///
    /// `lyaw`/`lpitch` : le REGARD (spec 2026-08-15 §5.1). ⚠️ Ils sont là pour TRANCHER une
    /// hypothèse, pas seulement pour tracer. `ReadLookYaw` lit
    /// `gameCameraSystem.GetActiveCameraForward()`, dont la convention d'axes n'est **pas mesurée**
    /// (voir NetworkGameSystem.reds). Journaliser `yaw` et `lyaw` CÔTE À CÔTE rend la question
    /// décidable depuis le journal seul, sans que personne n'ait à regarder l'écran :
    ///   · joueur immobile regardant droit devant → `lyaw ≈ yaw` : la convention est bonne ;
    ///   · écart CONSTANT (±90°, ±180°) → convention d'axes différente, corrigeable par un offset ;
    ///   · écart ERRATIQUE → vecteur non normalisé, ou ce n'est pas la caméra attendue ;
    ///   · `lyaw` toujours 0 → l'appel échoue et le repli s'applique (dégradation sûre, pas panne).
    /// `lpitch` se lit pareil : lever la tête doit le faire monter, la baisser le faire descendre.
    void Emission(float x, float y, float z, float yaw, std::uint8_t locomotion, float lookYaw,
                  float lookPitch, std::uint8_t moveDir = 0) noexcept
    {
        Ecrire("{\"t\":%lld,\"ts\":%lld,\"k\":\"tx\",\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,"
               "\"yaw\":%.1f,\"loco\":%u,\"lyaw\":%.1f,\"lpitch\":%.1f,\"mdir\":%u,\"cmds\":%u,\"ret\":%d}\n",
               static_cast<long long>(Maintenant()), static_cast<long long>(TempsServeur()), x, y,
               z, yaw, static_cast<unsigned>(locomotion), lookYaw, lookPitch,
               static_cast<unsigned>(moveDir));
    }

    /// CE QUE JE RENDS — la pose à laquelle un avatar distant est effectivement placé, plus l'état
    /// interne qui explique cette pose. Les derniers champs sont ce qui distingue « il est au bon
    /// endroit » de « il y est pour de bonnes raisons ».
    ///
    /// ⚠️ **Échantillonné par entité** (`kPeriodeRenduParEntiteS`) : à 200 voisins et 60 fps, tout
    /// écrire noierait le disque et fausserait le temps de frame. `RenduAutorise` décide.
    /// `libre` : de combien l'avatar s'est deplace TOUT SEUL depuis la position ou on l'a place a
    /// la frame precedente — du mouvement que NOUS n'avons pas ordonne. C'est la mesure qui tranche
    /// la question ouverte de F-PLY-064 : un correcteur qui ferme 15 % de l'ecart par frame ne peut
    /// pas laisser 9 m de derive soutenue, a moins que quelque chose ne rattrape l'avatar entre
    /// deux corrections. `-1` = pas de position de reference (premiere frame de cet avatar).
    void Rendu(std::uint64_t id, float x, float y, float z, std::uint8_t locomotion, float derive,
               std::size_t echantillons, bool extrapole, double delai, double gigue,
               bool recalage, float libre = -1.0f, float depuisPlace = -1.0f, float ecartPose = -1.0f,
               std::uint8_t moveDir = 0, std::uint32_t cmds = 0, int retCmd = -1) noexcept
    {
        Ecrire("{\"t\":%lld,\"ts\":%lld,\"k\":\"rx\",\"id\":%llu,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,"
               "\"loco\":%u,\"derive\":%.3f,\"ech\":%zu,\"ext\":%d,\"delai\":%.4f,\"gigue\":%.4f,"
               "\"recal\":%d,\"libre\":%.3f,\"dtcorr\":%.4f,\"pose\":%.4f,\"mdir\":%u,\"cmds\":%u,\"ret\":%d}\n",
               static_cast<long long>(Maintenant()), static_cast<long long>(TempsServeur()),
               static_cast<unsigned long long>(id), x, y, z,
               static_cast<unsigned>(locomotion), derive, echantillons, extrapole ? 1 : 0, delai,
               gigue, recalage ? 1 : 0, libre, depuisPlace, ecartPose, static_cast<unsigned>(moveDir), cmds, retCmd);
    }

    /// Faut-il écrire une ligne `rx` pour cette entité maintenant ?
    ///
    /// ⚠️ **Un événement RARE ne doit jamais être échantillonné.** Un recalage ou un téléport est
    /// précisément ce qu'on cherche : le manquer parce qu'il tombe dans le mauvais dixième de
    /// seconde rendrait le journal muet sur le seul symptôme qui compte. D'où `force`.
    [[nodiscard]] bool RenduAutorise(std::uint64_t id, bool force) noexcept
    {
        if (m_fichier == nullptr)
        {
            return false;
        }
        if (force)
        {
            return true;
        }
        std::lock_guard<std::mutex> verrou(m_mutex);
        const double maintenant = static_cast<double>(Maintenant()) / 1000.0;
        auto& dernier = m_dernierRendu[id];
        if (maintenant - dernier < kPeriodeRenduParEntiteS)
        {
            return false;
        }
        dernier = maintenant;
        return true;
    }

    /// Une entité disparue n'a plus à occuper la table d'échantillonnage. Sans ça, une session
    /// longue accumule une entrée par id réseau jamais revu — petit, mais c'est exactement le
    /// genre de table « jamais purgée » que le dépôt a déjà payé ailleurs.
    void OublierEntite(std::uint64_t id) noexcept
    {
        std::lock_guard<std::mutex> verrou(m_mutex);
        m_dernierRendu.erase(id);
    }

    /// Un événement nommé, pour ce qui n'est pas une pose : gel, entité irrésolue, naissance,
    /// destruction, régression de tick. `detail` est libre, il finit tel quel dans le champ `d`.
    void Evenement(const char* nom, std::uint64_t id, const char* detail) noexcept
    {
        Ecrire("{\"t\":%lld,\"ts\":%lld,\"k\":\"ev\",\"n\":\"%s\",\"id\":%llu,\"d\":\"%s\"}\n",
               static_cast<long long>(Maintenant()), static_cast<long long>(TempsServeur()), nom,
               static_cast<unsigned long long>(id), detail != nullptr ? detail : "");
    }

private:
    /// Instant courant sur l'horloge du SERVEUR. Égal à l'heure locale si l'estimateur n'est pas
    /// branché ou pas encore amorcé — jamais une valeur inventée.
    [[nodiscard]] std::int64_t TempsServeur() const noexcept
    {
        const std::int64_t local = Maintenant();
        if (m_horloge == nullptr || !m_horloge->EstAmorcee())
        {
            return local;
        }
        return static_cast<std::int64_t>(
            m_horloge->TempsServeurMs(static_cast<std::uint64_t>(local)));
    }

    /// Crée le dossier s'il manque. `create_directories` est déjà idempotent et ne lève pas dans
    /// sa forme à `error_code` — on ne teste donc pas l'existence au préalable, et on n'agit pas
    /// non plus sur l'échec : c'est `fopen` qui tranchera, et `CheminUtilise()` qui le dira.
    static void CreerDossier(const std::string& chemin) noexcept
    {
        std::error_code ec;
        std::filesystem::create_directories(chemin, ec);
    }

    template <typename... Args>
    void Ecrire(const char* format, Args... args) noexcept
    {
        std::lock_guard<std::mutex> verrou(m_mutex);
        if (m_fichier == nullptr)
        {
            return;
        }
        std::fprintf(m_fichier, format, args...);
        // ⚠️ On vide à chaque ligne. C'est délibérément coûteux : le jeu se termine par un
        // `Stop-Process`, jamais proprement, et un tampon non vidé emporte les dernières secondes
        // — précisément celles qui décrivent ce qu'on venait d'observer.
        std::fflush(m_fichier);
    }

    std::FILE* m_fichier = nullptr;
    std::string m_chemin;
    mutable std::mutex m_mutex;
    const HorlogeServeur* m_horloge = nullptr;
    /// Dernière écriture `rx` par id réseau, en secondes — support de `RenduAutorise`.
    std::unordered_map<std::uint64_t, double> m_dernierRendu;
};

} // namespace Tessera::Sync

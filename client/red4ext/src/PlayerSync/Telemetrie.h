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
// ── CE QUI REND LA MESURE EXACTE, ET C'EST GRATUIT ────────────────────────────────────────────
//
// Les deux instances de test tournent sur LA MÊME MACHINE, donc sur LA MÊME horloge murale. Pas
// besoin de synchroniser quoi que ce soit : l'instance A écrit « à T j'étais en P », l'instance B
// écrit « à T′ j'ai rendu l'avatar A en P ». `T′ − T` est la latence bout-en-bout VRAIE, celle que
// Lucas ressent, décomposable ensuite en ses postes (tampon, serveur, moteur).
//
// C'est pour ça que l'horodatage est `system_clock` (époque Unix, partagée entre processus) et
// SURTOUT PAS `steady_clock` (origine arbitraire par processus — deux instances n'auraient aucun
// point commun, et l'erreur serait invisible : les deux fichiers auraient l'air parfaits).
//
// ── FORMAT ────────────────────────────────────────────────────────────────────────────────────
//
// JSONL : une ligne = un objet JSON = un événement. Pas de tableau englobant, donc un fichier
// tronqué (le jeu qu'on tue) reste exploitable jusqu'à sa dernière ligne complète — ce qui arrive
// à CHAQUE session, puisqu'on ferme le jeu par `Stop-Process`.
//
// Un fichier PAR PROCESSUS (le PID est dans le nom) : les deux instances partagent le dossier du
// jeu, et deux écrivains sur un même fichier produiraient un mélange illisible. C'est le même
// piège que le marqueur `tessera-dev.flag` partagé, qui a coûté une demi-heure le 2026-08-09.
//
// ⚠️ Ce module n'écrit RIEN tant que `Demarrer` n'a pas été appelé — donc rien en production, où
// personne ne le démarre. Une télémétrie qui s'active toute seule est un coût imposé au joueur.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace Tessera::Sync
{

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
    /// pas) — mais rien ne garantit qu'il l'y laisse. Un chemin relatif unique aurait donc marché
    /// ou non selon une condition qu'on ne contrôle pas, et l'échec aurait été SILENCIEUX : un
    /// `fopen` qui rend `nullptr`, un journal vide, et un verdict qui dit « aucun journal » sans
    /// jamais dire pourquoi.
    ///
    /// On essaie donc les deux emplacements plausibles et on garde le premier qui s'ouvre. Deux
    /// lignes, contre une session perdue à comprendre un fichier absent.
    ///
    /// `chemin_utilise()` dit lequel a gagné — parce qu'un instrument doit pouvoir répondre « où
    /// écris-tu ? » sans qu'on ait à le deviner.
    void Demarrer(const std::string& dossier, std::uint32_t pid) noexcept
    {
        std::lock_guard<std::mutex> verrou(m_mutex);
        if (m_fichier != nullptr)
        {
            return;
        }
        const std::string suffixe = "/tessera-telemetrie-" + std::to_string(pid) + ".jsonl";
        // 1. répertoire courant = racine du jeu ; 2. répertoire courant = bin/x64.
        for (const char* prefixe : {"", "../../"})
        {
            const std::string chemin = std::string(prefixe) + dossier + suffixe;
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

    /// Millisecondes depuis l'époque Unix — la seule base commune aux deux processus.
    [[nodiscard]] static std::int64_t Maintenant() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /// CE QUE J'ÉMETS — la pose du joueur local, au moment où elle part sur le fil.
    void Emission(float x, float y, float z, float yaw, std::uint8_t locomotion) noexcept
    {
        Ecrire(
            "{\"t\":%lld,\"k\":\"tx\",\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"yaw\":%.1f,\"loco\":%u}\n",
            static_cast<long long>(Maintenant()), x, y, z, yaw, static_cast<unsigned>(locomotion));
    }

    /// CE QUE JE RENDS — la pose à laquelle un avatar distant est effectivement placé, plus l'état
    /// interne qui explique cette pose. Les six derniers champs sont ce qui distingue « il est au
    /// bon endroit » de « il y est pour de bonnes raisons ».
    void Rendu(std::uint64_t id, float x, float y, float z, std::uint8_t locomotion, float derive,
               std::size_t echantillons, bool extrapole, double delai, double gigue,
               bool recalage) noexcept
    {
        Ecrire("{\"t\":%lld,\"k\":\"rx\",\"id\":%llu,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"loco\":%u,"
               "\"derive\":%.3f,\"ech\":%zu,\"ext\":%d,\"delai\":%.4f,\"gigue\":%.4f,\"recal\":%d}\n",
               static_cast<long long>(Maintenant()), static_cast<unsigned long long>(id), x, y, z,
               static_cast<unsigned>(locomotion), derive, echantillons, extrapole ? 1 : 0, delai,
               gigue, recalage ? 1 : 0);
    }

    /// Un événement nommé, pour ce qui n'est pas une pose : gel, entité irrésolue, naissance,
    /// destruction. `detail` est libre, il finit tel quel dans le champ `d`.
    void Evenement(const char* nom, std::uint64_t id, const char* detail) noexcept
    {
        Ecrire("{\"t\":%lld,\"k\":\"ev\",\"n\":\"%s\",\"id\":%llu,\"d\":\"%s\"}\n",
               static_cast<long long>(Maintenant()), nom, static_cast<unsigned long long>(id),
               detail != nullptr ? detail : "");
    }

private:
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
};

} // namespace Tessera::Sync

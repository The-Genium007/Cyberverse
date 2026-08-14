#ifndef NETWORKMANAGERCONTROLLER_H
#define NETWORKMANAGERCONTROLLER_H
#include <RED4ext/RED4ext.hpp>
#include "RED4ext/SystemUpdate.hpp"

#include "RED4ext/Scripting/IScriptable.hpp"
#include "RED4ext/Scripting/Natives/Generated/ink/ISystemRequestsHandler.hpp"
#include "RED4ext/Scripting/Stack.hpp"

#include "CommandLine.h"
// `SDK` et `PLUGIN` sont utilisés par les fonctions INLINE de cet en-tête (journalisation).
// ⚠️ Inclus EXPLICITEMENT depuis le 2026-08-10. Ils arrivaient jusque-là par un chemin transitif —
// `PlayerSync/InterpolationData.h` commençait par `#include "Main.h"`. En remplaçant ce fichier
// (jamais alimenté, cf. TamponInterpolation.h) par un en-tête délibérément SANS dépendance, le
// chemin a disparu et la compilation est tombée sur `'SDK': identificateur non déclaré` — dans une
// autre unité de compilation, à des lignes qui n'avaient pas bougé. Un en-tête doit inclure ce
// qu'il utilise ; celui-ci ne le faisait pas, et personne ne pouvait le voir.
#include "Main.h"
#include "PlayerActionTracker.h"
#include "PlayerSync/TamponInterpolation.h"
#include "RED4ext/Scripting/Natives/Generated/AI/Command.hpp"
#include "RED4ext/Scripting/Natives/Generated/Vector4.hpp"
#include "RED4ext/Scripting/Natives/entEntityID.hpp"

#include <RedLib.hpp>
#include <clientbound/WorldPacketsClientBound.h>
#include <map>
#include <set>   // suivi des apparences statiques deja appliquees (hydratation discrete)
#include <cmath> // std::floor pour le decoupage en cellules de halo
#include <string>
#include <vector>
#include <steam/isteamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

#include <serverbound/WorldPacketsServerBound.h>

// Protocole TesseraSynth (FlatBuffers) — forward decl pour ne pas tirer l'en-tête généré ici.
namespace cyberpunk_rp::protocol {
    struct Snapshot; struct PositionCorrection; struct ShardAssignment; struct StaticAppearance;
    struct WorldState; struct Kicked; struct AppearanceSync; struct ConfigSync; struct CellAppearances;
    struct PlayerEvent;
    // ⚠️ Oublier une declaration avancee ici ne donne PAS « type inconnu » : le compilateur lit
    // `const CharacterList*` comme `const int` et l'erreur sort a l'APPEL, sous la forme
    // « impossible de convertir 'const CharacterList *' en 'const int' » — un message qui pointe
    // vers l'appelant alors que le defaut est ici. Piege deja paye une fois (2026-08-07).
    struct CharacterList; struct CharacterResult;
    // Piege paye une SECONDE fois le 2026-08-08 : `HandleHealthSync` a ete declaree plus bas sans
    // passer par ici, et le build entier tombait sur « 'HealthSync' n'est pas membre de
    // cyberpunk_rp::protocol ». Consequence en chaine : plus de DLL, donc un `.reds` deja deploye
    // (jonction vers le jeu) qui declare `Tessera_RapporterDegats` sans backing natif, donc TOUT
    // r6/scripts par terre au prochain lancement. Tout nouveau `Handle<X>` se declare ICI en meme
    // temps qu'il se declare plus bas.
    struct HealthSync;
    // Meme regle : toute nouvelle table manipulee ici se declare AUSSI dans ce bloc.
    struct RespawnRequest;
    struct HealthReport;
    // Interactions joueur<->joueur (spec 2026-08-09). Meme regle, troisieme rappel.
    struct ActionCatalog;
    struct IdentitesConnues;
}

// Apparence faisant autorité pour chaque PNJ STATIQUE, par EntityID — définie dans le .cpp.
// Hors de la classe : `NetworkGameSystem` est alloué par le moteur (`RTTI_IMPL_ALLOCATOR`), lui
// ajouter un membre corrompt la mémoire voisine (mesuré le 2026-08-06).
extern std::map<uint64_t, uint64_t> g_apparencesStatiques;
// Ce qu'il faut pour REFABRIQUER un statique absent chez ce client : record, apparence, pose.
// Distinct de `g_apparencesStatiques`, qui ne sert qu'a corriger un PNJ deja present. Voir
// `docs/superpowers/specs/2026-08-09-presence-et-apparence-a-cent-pour-cent-design.md`.
struct InscriptionRoster
{
    uint64_t record = 0;
    uint64_t apparence = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    int16_t yaw = 0;
};
extern std::map<uint64_t, InscriptionRoster> g_rosterStatiques;
// Nos remplacants : id du natif absent -> id de l'entite LOCALE creee a sa place. C'est la seule
// entite qu'on ait le droit de detruire ; un PNJ de communaute ne se retire pas (F-PNJ-091).
extern std::map<uint64_t, RED4ext::ent::EntityID> g_remplacants;
// Statiques du roster qu'on a VUS présents au moins une fois dans cette session. On ne les
// supplée plus jamais : leur absence ultérieure est un déchargement du moteur (le PNJ passé dans
// le dos du joueur), pas la divergence durable que le roster existe pour combler.
//
// ponytail: jamais purgé — un set qui grossit avec le nombre de statiques rencontrés dans la
// session (ordre de grandeur : quelques milliers d'uint64, donc quelques dizaines de Ko). À borner
// seulement si une session longue le montre ; le purger serait pire que le garder, puisqu'oublier
// qu'on a vu quelqu'un le rend à nouveau duplicable.
extern std::set<uint64_t> g_dejaVus;

/// Compteurs de santé du roster — pour qu'une régression SE VOIE sans être reproduite.
///
/// Un ratio « créés / refusés » qui bascule dit qu'une garde a cessé de mordre, et lequel des
/// compteurs bouge dit LAQUELLE. Journalisés périodiquement (voir `NettoyerRemplacants`).
struct StatsRoster
{
    std::uint64_t crees = 0;
    /// Retirés parce que quelqu'un d'autre occupe la place (garde spatiale).
    std::uint64_t retiresPlaceOccupee = 0;
    /// Purgés parce que trop loin du joueur — le nettoyage de fond.
    std::uint64_t purgesDistance = 0;
    /// Oubliés parce que l'entité locale n'existait plus (le moteur l'avait déjà détruite).
    std::uint64_t oubliesDisparus = 0;
    /// Refusés à la création : on avait déjà vu ce PNJ présent (déchargement, pas divergence).
    std::uint64_t refusesDejaVu = 0;
    /// Refusés à la création : un remplaçant tient déjà cet endroit.
    std::uint64_t refusesEndroitPris = 0;
    /// Avatars figés parce que leur fil s'est tu (micro-coupure) — voir `PiloterAvatar`.
    /// Un compteur qui monte sans arrêt dit que le réseau souffre, pas que le code est faux.
    std::uint64_t avatarsFiges = 0;
    /// Recalages francs d'avatar (dérive au-delà du seuil). C'est le compteur de la MALADIE :
    /// il doit rester proche de zéro. S'il monte, la boucle de suivi ne tient pas la cible.
    std::uint64_t recalagesAvatar = 0;
    /// Passages où le moteur n'a pas su rendre l'entité de l'avatar. Piste du « délai au retour
    /// dans le champ de vision » — hypothèse non mesurée, voir `PiloterAvatar`.
    std::uint64_t avatarsIrresolus = 0;
};
extern StatsRoster g_statsRoster;
// Cellules de halo deja recues du serveur. Meme decoupage que `halo.rs` cote serveur — 64 m.
extern std::set<std::pair<int32_t, int32_t>> g_cellulesRecues;
constexpr float kCoteCelluleM = 64.0f;
inline std::pair<int32_t, int32_t> CelluleDe(float x, float y)
{
    return {static_cast<int32_t>(std::floor(x / kCoteCelluleM)),
            static_cast<int32_t>(std::floor(y / kCoteCelluleM))};
}
// Statiques dont l'apparence autoritaire a REELLEMENT ete appliquee. La difference avec
// `g_apparencesStatiques` est la file de travail de l'hydratation discrete.
extern std::set<uint64_t> g_apparencesAppliquees;
// Sonde d'apparence : premiere apparence vue par record, et garde one-shot.
extern std::map<uint64_t, uint64_t> g_premiereApparence;
extern bool g_sondeApparenceFaite;

// --- Rendu des avatars JOUEURS : l'état, hors de la classe (même règle que ci-dessus) ---
// À quel instant de la timeline SERVEUR on rend, maintenant.
extern Tessera::Sync::HorlogeRendu g_horlogeRendu;
// Historique récent par id réseau — ce qui permet d'interpoler au lieu de deviner.
extern std::map<uint64_t, Tessera::Sync::TamponPose> g_tamponsJoueurs;
/// Dernier point de visée réellement commandé, et depuis quand — pour ne pas réémettre un ordre
/// identique vingt fois par seconde (le moteur s'est effondré pour cette raison le 2026-08-06).
struct SuiviAvatar
{
    float cibleX = 0.0f;
    float cibleY = 0.0f;
    float cibleZ = 0.0f;
    float depuisS = 0.0f;
    /// Temps écoulé depuis la dernière ligne de diagnostic — voir `PiloterAvatar`.
    float depuisLogS = 0.0f;
    bool commande = false;
    /// Dernière allure commandée. Un changement d'allure est un ÉVÉNEMENT : il déclenche une
    /// réémission immédiate au lieu d'attendre le créneau — c'est ce qui supprime le « petit délai
    /// avant que ça se déclenche » signalé le 2026-08-13.
    std::uint8_t derniereLocomotion = 0;
};
extern std::map<uint64_t, SuiviAvatar> g_suiviAvatars;

// Identité visuelle d'une entité réseau, telle que le SERVEUR la décide (`AppearanceSync`).
// Deux hashes suffisent (modèle PRESET, ADR/design apparence §6.2) : le record TweakDB à faire
// apparaître et le nom d'apparence `.ent` à appliquer. C'est ce qui remplace le `Character.Panam`
// codé en dur — l'apparence n'est plus une constante du client, c'est une donnée serveur.
struct NetworkAppearance
{
    uint64_t baseRecord = 0;
    uint64_t appearance = 0;
    // Arme actuellement EN MAIN (hash TweakDBID), 0 = mains vides. Transportee par `garments` dans
    // `AppearanceSync`. ⚠️ `0` est un ETAT, pas une absence d'information : le serveur le pousse
    // quand le joueur range son arme, et le confondre avec « pas d'info » laisserait l'arme dans
    // les mains de l'avatar pour toujours.
    uint64_t arme = 0;
};

class NetworkGameSystem : public Red::IGameSystem
{
private:
    HSteamNetConnection m_hConnection;
    ISteamNetworkingSockets *m_pInterface;
    bool m_hasTriedToConnect = false;
    bool m_hasEnqueuedLoadLastCheckpoint = false;
    Red::Handle<Red::ink::ISystemRequestsHandler> m_systemRequestsHandler;
    bool m_gameRestored = false;
    std::map<uint64_t, RED4ext::ent::EntityID> m_networkedEntitiesLookup;

    // --- Rendu des avatars JOUEURS (couches 1-2, cf. PlayerSync/TamponInterpolation.h) ---
    //
    // ⚠️ JOUEURS SEULEMENT. Les PNJ gardent leur chemin (`SetEntityPose`), réglé en jeu le
    // 2026-08-06 : le serveur planifie leur trajet et leur envoie une VRAIE destination
    // (`NpcState.move_target`), et on veut que le moteur navigue — trottoirs, passages, feux
    // (F-PNJ-095). Un joueur, lui, n'a pas de destination connue du serveur, et sa position FAIT
    // AUTORITÉ : on ne veut surtout pas que le moteur lui recalcule un chemin autour d'un obstacle.
    // Deux populations, deux boucles. Les mélanger casserait l'une ou l'autre.
    //
    // Remplace `m_interpolationData`, hérité du fork : cette map n'était écrite NULLE PART, donc
    // `InterpolatePuppets` parcourait une map vide à chaque frame depuis toujours.
    //
    // ⚠️ L'ÉTAT VIT HORS DE LA CLASSE — `g_horlogeRendu`, `g_tamponsJoueurs`, `g_suiviAvatars`,
    // déclarés plus haut avec `g_apparencesStatiques` et ses voisines. Ce fichier prescrit
    // explicitement cette forme : « `NetworkGameSystem` est alloué par le moteur
    // (`RTTI_IMPL_ALLOCATOR`), lui ajouter un membre corrompt la mémoire voisine (mesuré le
    // 2026-08-06) ». J'avais d'abord posé trois membres ici, par réflexe.
    //
    // ⚠️⚠️ CETTE CONSIGNE EST À INSTRUIRE, PAS À CROIRE SUR PAROLE. La classe porte déjà 40 membres
    // non statiques, dont certains ajoutés par des commits POSTÉRIEURS à celui qui a écrit
    // l'avertissement (`7ecadb4`). Les deux ne peuvent pas être vrais au même sens : soit le danger
    // est plus étroit que sa formulation, soit un vrai plantage a été attribué à la mauvaise cause.
    // Le fait n'existe NULLE PART dans `docs/connaissances/` — il ne vit que dans ce commentaire,
    // donc D3 ne le voit pas et personne ne peut le contredire par une mesure. En attendant qu'une
    // mesure tranche, on suit la consigne écrite : elle ne coûte rien ici, et l'ignorer coûterait
    // une corruption silencieuse — le pire mode de panne du lot.

    std::map<RED4ext::ent::EntityID, RED4ext::Handle<RED4ext::AICommand>> m_LastTeleportCommand;
    float m_TimeSinceLastPlayerPositionSync;
    /// Horloges de la passe de nettoyage du roster et de son bilan périodique.
    float m_tempsDepuisNettoyage = 0.0f;
    float m_tempsDepuisBilanRoster = 0.0f;
    /// Tourniquet : dernier remplaçant examiné, pour reprendre où l'on s'était arrêté et garder un
    /// coût CONSTANT quelle que soit la taille de la table.
    std::uint64_t m_dernierRemplacantExamine = 0;

    // --- Autorité serveur (TesseraSynth) ---
    // Dernier ShardAssignment reçu : placement autoritaire décidé par le serveur (topology.locate),
    // poussé au HUD moniteur de cohérence via les getters natifs ci-dessous. Vides tant qu'aucun
    // ShardAssignment n'est arrivé (le HUD affiche alors seulement son calcul local).
    std::string m_serverShard;        // shard autoritaire, ex. "group-1"
    std::string m_serverOverlapsCsv;  // shards tampon en CSV, ex. "group-0,group-2"
    // Anti-boucle rubber-band : après une PositionCorrection, on saute le PROCHAIN envoi de
    // PositionUpdate pour laisser le moteur appliquer la téléportation et éviter de ré-émettre
    // immédiatement l'ancienne position (qui redéclencherait une correction côté serveur, cf. spec
    // mouvement §4.3). Un seul tick suffit : le sync suivant lit la position déjà corrigée.
    bool m_skipNextPositionUpdate = false;

    // --- Flux d'arrivee : personnages du compte (lobby Tessera, 2026-08-08) ---
    // Le serveur envoie un `CharacterList` juste apres le Join, puis un `CharacterResult` apres
    // chaque creation/selection refusee. Le lobby redscript LIT cet etat, il ne le calcule jamais :
    // l'autorite sur « quels personnages ce compte possede » est entierement serveur, y compris le
    // cap de slots (`character.slots.N`, defaut 1, illimite pour un joker).
    //
    // Pas de mutex, et c'est DELIBERE : `PollIncomingMessages` est appele depuis `OnNetworkUpdate`,
    // enregistre en `UpdateTickGroup::FrameBegin`, donc sur le FIL DE JEU — le meme qui execute le
    // redscript qui lit ces champs. Meme raisonnement que `m_serverShard` ci-dessus. Si la
    // reception passait un jour sur un fil dedie, il faudrait un verrou ICI.
    struct PersonnageDistant
    {
        uint64_t id = 0;
        std::string pseudonyme;
        // L'avatar du personnage, tel que le SERVEUR le connait. Sans lui, le lobby ne peut
        // afficher qu'une silhouette : il sait QUI est le personnage, pas a quoi il ressemble.
        uint64_t record = 0;
        uint64_t apparence = 0;
    };
    std::vector<PersonnageDistant> m_personnages;
    // Dernier verdict de creation. `m_aUnResultat` distingue « rien recu » de « recu un refus » —
    // sans lui, une UI ne saurait pas si le serveur a repondu. Le motif n'est PAS traduit ici :
    // l'ecran est mieux place pour le formuler, et un motif inconnu doit pouvoir traverser sans
    // etre avale (le schema est append-only, un serveur plus recent peut en inventer).
    bool m_aUnResultat = false;
    bool m_dernierResultatOk = false;
    std::string m_dernierResultatMotif;
    // Distingue « liste vide » (compte neuf, il faut creer) de « rien recu » (trop tot, il faut
    // attendre). Deux etats que l'UI doit traiter differemment, et qu'une taille de vecteur seule
    // ne separe pas.
    bool m_listeRecue = false;

    // --- Coma / mort (chantier autorite totale, 2026-08-09) ---
    // Pousses par `HealthSync` quand `mine` est vrai. `-1` = vivant, pour que « pas de decompte »
    // et « decompte a zero » ne se confondent pas — la seconde autorise l'hopital, la premiere non.
    int32_t m_secondesSecours = -1;
    bool m_hopitalOuvert = false;

    // --- Faim / soif (chantier besoins, 2026-08-09) ---
    // Pour mille, 1000 = rassasie. Pousses par `HealthSync` quand `mine` est vrai, et par LUI SEUL :
    // les copies destinees aux voisins portent 0/0 a dessein (la faim d'un tiers ne regarde
    // personne, et c'est autant d'octets en moins dans un message diffuse a l'AoI).
    //
    // ⚠️ Valeur de depart 1000, pas 0. Le serveur n'emet un `HealthSync` que sur CHANGEMENT de
    // jauge, soit une dizaine de secondes apres l'entree en session : demarrer a 0 afficherait deux
    // jauges vides pendant tout ce temps, ce qui se lirait comme « le serveur me laisse mourir de
    // faim » alors que rien n'est encore arrive. Le defaut doit ressembler a la verite, pas a zero.
    int32_t m_faim = 1000;
    int32_t m_soif = 1000;

    // Derniere sante locale CONNUE, en pourcentage (0-100). `-1` = jamais lue : le premier passage
    // ne rapporte donc rien, il ne fait qu'etablir la reference. Sans ce -1, l'entree en session
    // produirait un faux « gain de 100 % ».
    float m_santeLocaleConnue = -1.0f;

    // --- Détection « modset non compilé » (incident playtest 2026-07-20) ---
    // `SpawnTransientEntity` est déclarée en REDSCRIPT (r6/scripts/Cyberverse/NetworkGameSystem.reds).
    // Or redscript est tout-ou-rien : un seul .reds en erreur — y compris un mod TIERS sans rapport
    // avec Tessera — fait tomber TOUT r6/scripts. Le plugin C++, lui, est indépendant : il se
    // connecte, envoie les positions, reçoit les snapshots. Résultat vécu en playtest : le joueur
    // est un fantôme (les autres le voient, lui ne voit personne), sans le moindre signe à l'écran,
    // et `Red::CallVirtual` échoue en boucle — des centaines de lignes de log par seconde.
    //
    // Deux compteurs, deux rôles distincts :
    // - `m_spawnFailureCount` / `m_timeSinceSpawnFailureLog` : agrègent les logs (une ligne
    //   périodique avec un total, au lieu d'une ligne par snapshot et par joueur) ;
    // - `m_spawnFailureNotified` : garde one-shot de l'alerte utilisateur.
    uint64_t m_spawnFailureCount = 0;
    float m_timeSinceSpawnFailureLog = 0.0f;
    bool m_spawnFailureNotified = false;

    // --- Autorité serveur sur l'apparence (`AppearanceSync`) ---
    // Identité visuelle décidée par le serveur, par id d'entité réseau (joueur OU PNJ : le fil est
    // générique en `id` et les plages d'ids sont disjointes par construction, cf. protocol.fbs
    // NpcState). Alimentée AVANT le premier Snapshot qui porte l'entité (le serveur pousse
    // l'apparence à l'entrée en AoI), donc consultable au moment du spawn.
    std::map<uint64_t, NetworkAppearance> m_appearances;
    // Apparences reçues pour des ids pas encore spawnés, et inversement : un id déjà spawné dont
    // l'apparence change ARRIVE APRÈS le spawn. On mémorise ce qui a été réellement appliqué pour
    // ne re-appliquer que sur changement réel (`ScheduleAppearanceChange` a un effet différé et
    // relire immédiatement renvoie l'ancienne valeur — F-PNJ-050).
    std::map<uint64_t, uint64_t> m_appliedAppearance;

    // --- Horloge monde serveur (`WorldState`) ---
    // Aucun état ici : le seuil de resynchronisation vit côté redscript, seul endroit d'où l'on
    // peut lire l'horloge du MOTEUR — le seul référent qui ait du sens. Une version antérieure
    // gardait ici la dernière heure SERVEUR appliquée et comparait à elle : elle ne gardait donc
    // rien (le serveur avance de 2 min entre deux diffusions, le seuil valait 2 min).
    /// Dernier preset météo réellement demandé au moteur. Vide = aucun. Sert à ne re-demander que
    /// sur changement : le serveur diffuse `WorldState` périodiquement, et redemander le même
    /// preset relancerait une transition de 3 s en boucle — un ciel qui ne se stabilise jamais.
    std::string m_lastAppliedWeather;
    /// Gardes one-shot des avertissements « méthode redscript introuvable ». Sans elles, un modset
    /// non compilé produit une ligne toutes les 2 secondes (cadence de `WorldState`) et une toutes
    /// les 5 secondes (rapport d'heure) : des milliers de lignes identiques par session, exactement
    /// le bruit qui avait déjà noyé le diagnostic des échecs de spawn (~5 Mo de lignes, cf.
    /// `m_spawnFailureCount`). La première ligne date le problème, les suivantes n'apprennent rien.
    bool m_avertiHeureIntrouvable = false;
    bool m_avertiMeteoIntrouvable = false;
    bool m_avertiLectureHeureIntrouvable = false;
    /// Secondes écoulées depuis le dernier `ClientTimeReport` (diagnostic de dérive d'horloge).
    float m_tempsDepuisRapportHeure = 0.0f;

    // --- Interactions joueur<->joueur (spec 2026-08-09) ---
    /// Une recette du catalogue, telle qu'elle arrive du serveur. `portee_m` sert UNIQUEMENT a
    /// decider d'afficher ou non une ligne : le serveur revérifie la portée à l'exécution, et c'est
    /// lui qui tranche. Le client affiche ; il n'autorise jamais.
    struct ActionRecue
    {
        uint32_t id = 0;
        std::string libelle;
        float portee_m = 0.0f;
    };
    /// Ce que CE joueur a le droit de proposer. Deja filtre par le serveur — il n'apprend jamais
    /// l'existence des actions qu'il n'a pas, donc pas de liste grisee qui expose celles du staff.
    std::vector<ActionRecue> m_actions;
    /// Les noms qu'on CONNAIT, par id reseau. C'est la seule source du nametag.
    ///
    /// ⚠️ Une entree n'apparait ici que parce que le serveur l'a envoyee, et il ne l'envoie qu'a
    /// qui s'est fait presenter. Un nom inconnu n'est pas masque a l'affichage : il n'a JAMAIS
    /// traverse le fil. La discretion est une propriete du serveur — un binaire modifie ne peut pas
    /// reveler ce qu'il n'a jamais recu.
    std::map<uint64_t, std::string> m_nomsConnus;

private:
    // Appelé à chaque échec de `SpawnTransientEntity`. Agrège les logs et déclenche UNE fois
    // l'alerte native quand le seuil est franchi.
    void OnSpawnFailure();
    // Alerte NATIVE (Win32), volontairement pas une UI de jeu : dans ce scénario tout redscript
    // est mort, donc l'UI kit Tessera l'est aussi. Affichée depuis un thread détaché pour ne
    // jamais bloquer la boucle de jeu.
    void NotifyModsetNotCompiled();

    void OnRegisterUpdates(RED4ext::UpdateRegistrar* aRegistrar) override;
    bool OnGameRestored() override;

private:
    bool ConnectToServer(const std::string& host, uint16_t port);
    static void ConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);
    void OnNetworkUpdate(RED4ext::FrameInfo& frame_info, RED4ext::JobQueue& job_queue);
    /// Rend TOUS les avatars de joueurs pour l'instant courant, une fois par frame.
    ///
    /// Remplace `InterpolatePuppets`, qui itérait sur une map jamais alimentée. La boucle suivie
    /// ici est celle qui est MESURÉE viable (backlog Q6/Q6b, 2026-07-23, sondes `loco_active`/
    /// `loco_lag`/`loco_hybrid`) : commande de marche CONTINUE pour l'animation, `Teleport` de
    /// recalage pour la position. Le Teleport ne casse pas l'animation tant que la commande tourne.
    void RendreAvatarsDistants(float deltaTime);
    /// Applique une pose échantillonnée à UN avatar : ordre de marche vers le point de visée si
    /// besoin, plus recalage si la dérive est trop grande.
    void PiloterAvatar(uint64_t networkId, RED4ext::ent::EntityID entityId,
                       const Tessera::Sync::PoseRendue& pose, float deltaTime);
    /// Place l'entité SANS toucher à sa file de commandes d'IA — voir le corps. À utiliser pour
    /// toute correction continue ; `SetEntityPosition`, lui, envoie un ordre de téléport qui
    /// ANNULE la commande de marche en cours.
    void PlacerSansCommande(RED4ext::ent::EntityID entityId, RED4ext::Vector4 worldPosition, float yaw);
    void SetEntityPosition(RED4ext::ent::EntityID entityId, RED4ext::Vector4 worldPosition, float yaw);
    // Placement HYBRIDE : marche animée quand l'entité bouge et que la dérive est faible,
    // téléportation de correction sinon. Le mécanisme vient de F-PNJ-082/F-PLY-007, qui posent
    // aussi la règle : la commande de marche est de l'animation, l'autorité reste au Snapshot.
    /// `moveTarget` : destination du PNJ (plusieurs metres), pas sa position du tick suivant.
    /// Nul = inconnue, on retombe sur `worldPosition`. Voir le commentaire dans le .cpp.
    void SetEntityPose(uint64_t networkId, RED4ext::ent::EntityID entityId,
                       RED4ext::Vector4 worldPosition, float yaw, uint8_t locomotion,
                       const RED4ext::Vector4* moveTarget = nullptr);

protected:
    void PollIncomingMessages();
    void TrackPlayerPosition(float deltaTime);

    // --- Couture protocole TesseraSynth (FlatBuffers) ---
    // Envoient un ClientEnvelope (Join / PositionUpdate) au serveur Rust autoritaire.
    void SendJoin(const std::string& displayName);
    // `locomotionForcee` >= 0 remplace la lecture du blackboard — voir le mode robot
    // (`PlayerSync/Robot.h`). -1 = comportement normal.
    void SendPositionUpdate(float x, float y, float z, float yaw,
                            int locomotionForcee = -1);
    // Remonte l'heure que CE client observe localement, pour que le serveur mesure l'ecart avec
    // son horloge autoritaire (`ClientTimeReport` -> `world_clock.rs`). Diagnostic pur : le
    // serveur journalise, il ne corrige rien avec — la correction descend, elle, par `WorldState`.
    // Sans cet envoi, la moitie « mesure » du dispositif n'existait que sur le papier : le serveur
    // savait lire un rapport que personne n'emettait.
    void SendClientTimeReport();
    // Remonte un stimulus OBSERVE chez le joueur local. Le client observe, il ne decide jamais de
    // la reaction : c'est le serveur qui rediffuse aux voisins (ADR 0022). `nature` est l'ordinal
    // de `gamedataStimType`, catalogue des 67 valeurs dans docs/connaissances/catalogue-stimulus.md.
    // `target` = pantin explicitement vise, 0 sinon (entree de la decision de promotion, serveur).
    void SendStimReport(uint8_t nature, float radiusMetres, uint64_t target);
    // Rapporte des degats infliges a une entite reseau. Passe par `EntityInteraction { target,
    // kind=5, param }` — un canal DEJA en place, dont le schema declare depuis le gel que la cible
    // est agnostique (« joueur↔joueur = joueur↔PNJ, meme plomberie »). Rien de neuf sur le fil
    // montant : seule la branche serveur manquait.
    void SendAttackReport(uint64_t target, uint32_t degats);
    // Demande de prise d'autorite sur un figurant local. Porte de quoi le REFABRIQUER, pas un
    // identifiant : voir `PromotionRequest` dans protocol.fbs.
    /// Renvoie true si la requete est REELLEMENT partie. Un false signifie deduplication, absence
    /// de connexion ou record nul — et l'appelant ne doit alors surtout pas masquer son pantin
    /// local : il effacerait un corps sans qu'aucun ne le remplace.
    bool SendPromotionRequest(uint64_t record, uint64_t apparence, float x, float y, float z,
                              float yaw, bool mort);
    // Rapporte un PNJ STATIQUE et l'apparence qu'on lui voit. Le serveur arbitre laquelle fait foi.
    void SendStaticNpcReport(uint64_t entityId, uint64_t record, uint64_t apparence, float x,
                             float y, float z, float yaw);
    // Vide la file des rapports de statiques, UN PAR TICK au plus et pas plus vite que la cadence
    // fixee. Appelee depuis `OnNetworkUpdate`.
    void DrainerRapportsStatiques();
    // Applique les apparences autoritaires encore en attente, UNE par tick au plus, et seulement
    // quand le PNJ est hors du champ de vision du joueur. Voir le commentaire dans le .cpp.
    void HydraterApparencesDiscretement();
    // Complete le roster : cree un remplacant LOCAL pour un statique absent, le retire quand le
    // natif arrive. Spec 2026-08-09 (complétion asymétrique).
    void ReparerRoster();
    /// Filet de fond : détruit les remplaçants qui n'ont plus lieu d'être (entité disparue,
    /// joueur parti trop loin). Bornée en travail ET en fréquence — voir le corps.
    void NettoyerRemplacants(float deltaTime);
    /// Teardown : détruit TOUS nos remplaçants. Appelé à la déconnexion — sans ça ils survivent
    /// à la session qui les a créés.
    void DetruireTousLesRemplacants(const char* raison);
    // Réconcilie un Snapshot serveur : spawn (id inconnu) / interpole (id connu) / despawn (id disparu).
    void HandleSnapshot(const cyberpunk_rp::protocol::Snapshot* snapshot);
    // Rubber-band / spawn autoritaire : téléporte le joueur local à la position corrigée par le
    // serveur et arme l'anti-boucle (m_skipNextPositionUpdate).
    void HandlePositionCorrection(const cyberpunk_rp::protocol::PositionCorrection* correction);
    // Placement autoritaire : mémorise shard + overlaps pour les getters natifs exposés au HUD.
    void HandleShardAssignment(const cyberpunk_rp::protocol::ShardAssignment* assignment);
    // Horloge monde partagée : le SERVEUR décide l'heure qu'il est, le client l'applique
    // (`TimeSystem.SetGameTimeByHMS` — signature vérifiée dans les scripts décompilés CDPR,
    // scripts/core/systems/timeSystem.script:15). La météo du même message n'est PAS appliquée :
    // aucun setter météo n'existe dans le dump RTTI (seuls des getters sur
    // worldWeatherScriptInterface) — voir la sonde S-W1 avant d'affirmer que c'est faisable.
    void HandleWorldState(const cyberpunk_rp::protocol::WorldState* state);
    // Météo décidée par le serveur, portée par le même message que l'heure. Séparée parce qu'elle
    // n'a pas la même cadence utile : l'heure se resynchronise sur seuil, la météo sur changement.
    void ApplyServerWeather(const cyberpunk_rp::protocol::WorldState* state);
    // Valeurs de jeu imposées par le serveur (prix, dégâts, portées…). Écrites dans TweakDB en
    // cours de partie — mesuré (F-PLF-018), pas supposé.
    void HandleConfigSync(const cyberpunk_rp::protocol::ConfigSync* sync);
    // Refus/expulsion serveur. Sans ce câblage, un client rejeté (serveur plein, token invalide,
    // ban, version de protocole) reste coupé SANS AUCUNE explication — le motif était envoyé
    // depuis le début et jeté par le `default:` de PollIncomingMessages.
    void HandleKicked(const cyberpunk_rp::protocol::Kicked* kicked);
    // Identité visuelle décidée par le serveur. Mémorise, et applique tout de suite si l'entité
    // est déjà là (l'apparence peut changer en cours de session : tenue, dégainage).
    void HandleAppearanceSync(const cyberpunk_rp::protocol::AppearanceSync* sync);
    // Evenement one-shot relaye par le serveur depuis un joueur voisin. kind=1 (Stim) rejoue le
    // stimulus sur la foule LOCALE : c'est ce qui fait fuir la meme rue au meme instant chez tout
    // le monde sans repliquer un seul fuyant (ADR 0022).
    void HandlePlayerEvent(const cyberpunk_rp::protocol::PlayerEvent* event);
    // Apparence FAISANT AUTORITE pour un PNJ statique. On ne cree rien : l'entite existe deja des
    // deux cotes, seule sa variante visuelle change.
    void HandleStaticAppearance(const cyberpunk_rp::protocol::StaticAppearance* msg);
    // Table d'apparences d'une CELLULE entiere, recue quand le joueur entre dans son halo — donc
    // AVANT qu'il ne voie les PNJ. Remplace l'envoi PNJ par PNJ.
    void HandleCellAppearances(const cyberpunk_rp::protocol::CellAppearances* msg);
    // Sante ARBITREE par le serveur. Deux lectures selon `mine`, et le drapeau n'est pas
    // redondant : ce client ne connait PAS son propre identifiant reseau (rien ne le lui envoie),
    // donc lui seul distingue « voici TA sante » de « voici celle du voisin ».
    void HandleHealthSync(const cyberpunk_rp::protocol::HealthSync* msg);
    // Liste des personnages du compte, poussee par le serveur apres le Join et apres chaque
    // creation. REMPLACE l'etat local : le serveur envoie toujours la liste complete, jamais un
    // delta — donc pas de fusion a faire, et un personnage supprime disparait de lui-meme.
    void HandleCharacterList(const cyberpunk_rp::protocol::CharacterList* list);
    // Verdict d'une creation de personnage (succes, ou motif de refus).
    void HandleCharacterResult(const cyberpunk_rp::protocol::CharacterResult* result);

    // ── Interactions joueur<->joueur (spec 2026-08-09) ────────────────────────────────────────
    // Catalogue DEJA FILTRE pour ce joueur : le serveur n'envoie que ce qu'il a le droit de faire.
    // REMPLACE l'etat local a chaque reception (join, puis chaque changement de permissions) —
    // pas un delta, donc rien a fusionner, et une action retiree disparait d'elle-meme.
    void HandleActionCatalog(const cyberpunk_rp::protocol::ActionCatalog* msg);
    // Noms que ce joueur CONNAIT. En lot au join, a une entree a chaque presentation recue. On
    // ACCUMULE ici (contrairement au catalogue) : le message a une entree est un ajout, pas un
    // remplacement, et le traiter comme tel effacerait toutes les connaissances a chaque poignee
    // de main.
    void HandleIdentitesConnues(const cyberpunk_rp::protocol::IdentitesConnues* msg);
    // Declenche une recette du catalogue sur une cible. `kind = 2` (Interagit), `param` = l'id de
    // la recette : le canal montant existe depuis le gel, zero octet ajoute au fil.
    void SendActionJoueur(uint64_t target, uint32_t recette);

    // Fait apparaître une entité réseau à l'apparence décidée par le serveur, ou au repli si
    // aucune n'est connue pour cet id. Renvoie false si le spawn a échoué (modset non compilé).
    bool SpawnNetworkEntity(uint64_t networkId, const RED4ext::Vector4& worldPosition);
    // Applique une apparence serveur sur une entité déjà spawnée (changement en cours de session).
    void ApplyAppearance(uint64_t networkId, RED4ext::ent::EntityID entityId);

public:
    bool FullyConnected = false;
    Red::Handle<PlayerActionTracker> playerActionTracker = Red::Handle(new PlayerActionTracker());

    /// This is called by Redscript when the connection wasn't established as the UI had loaded the available savegames.
    /// Thus we will _enqueue_ the "LoadLastCheckpoint" call, that Redscript had otherwise done, had we connected fast enough.
    void EnqueueLoadLastCheckpoint(const Red::Handle<RED4ext::ink::ISystemRequestsHandler>& handler)
    {
        m_systemRequestsHandler = handler;
        m_hasEnqueuedLoadLastCheckpoint = true;
    }

    template<typename T>
    bool EnqueueMessage(uint8_t channel_id, T frame);

    // --- Getters exposés au HUD Lua (via des wrappers redscript @addMethod(PlayerPuppet) côté
    // modset Tessera, qui délèguent à GameInstance.GetNetworkGameSystem()). Reflètent le dernier
    // ShardAssignment reçu + le nombre de puppets distants suivis. Chaînes vides / 0 tant que rien
    // n'est arrivé — le HUD retombe alors sur son calcul local seul.
    Red::CString Tessera_GetServerShard() const { return Red::CString(m_serverShard.c_str()); }
    Red::CString Tessera_GetServerOverlaps() const { return Red::CString(m_serverOverlapsCsv.c_str()); }
    int32_t Tessera_GetVisiblePlayerCount() const
    {
        return static_cast<int32_t>(m_networkedEntitiesLookup.size());
    }

    // --- Flux d'arrivee : ce que le lobby redscript appelle (2026-08-08) ---
    // Volontairement plat (des entiers et des chaines, indexes par position) plutot qu'un tableau
    // de structures : redscript ne consomme pas un `std::vector<PersonnageDistant>`, et exposer un
    // type par RTTI pour trois champs coute plus cher que trois getters.
    //
    // ⚠️ « 0 personnage » n'est PAS « pas encore recu ». Un compte neuf a legitimement une liste
    // vide, et l'ecran doit alors proposer la creation ; tant que le serveur n'a rien envoye,
    // `Tessera_ListePersonnagesRecue()` est faux et l'ecran doit attendre au lieu d'affirmer
    // « aucun personnage ». Sans cette distinction, un lobby ouvert trop tot pousse le joueur a
    // creer un doublon qui sera refuse par le cap de slots.
    int32_t Tessera_NombrePersonnages() const { return static_cast<int32_t>(m_personnages.size()); }
    bool Tessera_ListePersonnagesRecue() const { return m_listeRecue; }
    // `--tessera-dev` : sauter le lobby, entrer avec un personnage assigné d'office. Lu à CHAQUE
    // appel plutôt que mémorisé — la ligne de commande ne change pas en cours de session, et un
    // cache serait un état de plus à tenir pour rien.
    bool Tessera_ModeDeveloppement() const { return ModeDeveloppementDemande(GetCommandLineA()); }
    Red::CString Tessera_NomPersonnage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_personnages[static_cast<size_t>(index)].pseudonyme.c_str());
    }
    // (record, apparence) du personnage a cet index — 0 si l'index est hors bornes OU si le serveur
    // n'a pas d'avatar valide pour lui. Le client traite les deux cas pareil : repli silhouette.
    uint64_t Tessera_RecordPersonnage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size()) { return 0; }
        return m_personnages[static_cast<size_t>(index)].record;
    }
    uint64_t Tessera_ApparencePersonnage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size()) { return 0; }
        return m_personnages[static_cast<size_t>(index)].apparence;
    }
    uint64_t Tessera_IdPersonnage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size())
        {
            return 0;
        }
        return m_personnages[static_cast<size_t>(index)].id;
    }
    // Verdict de la derniere creation. "" = rien recu depuis le dernier appel ; "ok" = succes ;
    // toute autre valeur = motif de refus brut du serveur. La lecture CONSOMME le resultat, pour
    // qu'un ecran ne reaffiche pas indefiniment un refus deja traite.
    Red::CString Tessera_DernierResultat()
    {
        if (!m_aUnResultat)
        {
            return Red::CString("");
        }
        m_aUnResultat = false;
        return Red::CString(m_dernierResultatOk ? "ok" : m_dernierResultatMotif.c_str());
    }
    // Demande la creation d'un personnage. Le serveur arbitre : cap de slots, pseudonyme deja pris,
    // apparence hors catalogue. Renvoie false seulement si l'envoi lui-meme n'a pas pu partir
    // (pas de connexion) — un `true` ne dit RIEN du verdict, qui arrive en `CharacterResult`.
    bool Tessera_CreerPersonnage(const Red::CString& pseudonyme, uint64_t record, uint64_t apparence);
    // Entre dans le monde avec ce personnage. Meme remarque : `true` = « parti », pas « accepte ».
    bool Tessera_ChoisirPersonnage(uint64_t id);
    // Supprime un personnage du compte. Le SERVEUR arbitre (`not_owner`, `not_found`) et renvoie la
    // liste a jour — le client ne retire rien de son cote, sinon il afficherait une suppression qui
    // pourrait etre refusee. `true` = la demande est PARTIE, jamais qu'elle a ete acceptee.
    bool Tessera_SupprimerPersonnage(uint64_t id);

    // Remonte un stimulus au serveur depuis redscript. Point d'entree UNIQUE de l'observation :
    // l'entonnoir d'action est `StimBroadcasterComponent.TriggerSingleBroadcast` (F-PLY-029), ou
    // aboutissent 83 sites d'appel du jeu.
    //
    // `nature` en Uint32 et non Uint8 : redscript n'a pas de type 8 bits, la conversion se fait au
    // franchissement du fil.
    //
    // `cible` est l'entite VISEE au moment de l'action, ou une EntityID nulle si le joueur ne vise
    // rien.
    //
    // ⚠️ ELLE EST TRADUITE ICI, ET C'EST LE POINT ESSENTIEL. Un `EntityID` est attribue LOCALEMENT
    // au spawn : le meme passant porte une valeur differente chez chaque joueur. L'envoyer tel quel
    // ferait croire au serveur qu'il designe quelqu'un, alors qu'il ne designerait rien de partage —
    // un mensonge silencieux sur le fil. On renvoie donc l'ID RESEAU, seule identite que le serveur
    // et tous les clients partagent, et **0 quand la cible n'est pas une entite serveur**.
    //
    // Ce 0 n'est pas un echec a masquer : il dit exactement ou en est le modele de promotion. Un
    // passant de la foule NATIVE n'a aucune identite partagee, et lui en donner une est la question
    // ouverte de l'ADR 0022 (le hachage par place). Tant qu'elle n'est pas tranchee, seuls les
    // avatars des joueurs et les PNJ spawnes par le serveur sont designables.
    //
    // Balayage lineaire de la table : elle compte les entites reseau EN VUE (quelques dizaines au
    // plus), et l'appel est etrangle a 250 ms par (type, rayon).
    void Tessera_ReportStim(uint32_t nature, float radiusMetres, RED4ext::ent::EntityID cible)
    {
        uint64_t idReseau = 0;
        if (cible.IsDefined())
        {
            for (const auto& paire : m_networkedEntitiesLookup)
            {
                if (paire.second == cible)
                {
                    idReseau = paire.first;
                    break;
                }
            }
            // SONDE (F-PLY-030), a retirer une fois la cause tranchee. Une cible EXISTE mais ne
            // correspond a aucune entite reseau : le serveur recevra 0, et sans cette ligne on ne
            // saurait pas distinguer « le joueur ne visait rien » de « le joueur visait quelque
            // chose que nous ne savons pas nommer ». Deux causes opposees, meme valeur sur le fil.
            if (idReseau == 0)
            {
                SDK->logger->InfoF(PLUGIN,
                    "Stim : cible %llu visee mais INCONNUE du reseau (%zu entites suivies)",
                    cible.hash, m_networkedEntitiesLookup.size());
            }
        }
        SendStimReport(static_cast<uint8_t>(nature & 0xFF), radiusMetres, idReseau);
    }

    // Rapporte au serveur des degats infliges a une entite reseau, depuis redscript.
    //
    // MEME TRADUCTION que `Tessera_ReportStim`, et pour la meme raison : un `EntityID` est local au
    // client, seul l'id RESEAU designe quelque chose de partage. Une cible qui n'est PAS une entite
    // reseau (un passant de la foule native) ne produit AUCUN message — un figurant local n'a pas
    // d'identite chez le serveur, et lui en inventer une serait un mensonge sur le fil.
    //
    // `degats` en points de vie « jeu », tels que le moteur du tireur les a calcules — c'est lui qui
    // sait le faire (arme, mods, armure, critiques, zone touchee). Le serveur, lui, decide de leur
    // EFFET : il ecrete, il cadence, et il tient la seule barre de vie qui compte (`sante.rs`).
    //
    // Renvoie true si un message est parti. `false` = cible non reseau, degats nuls, ou pas de
    // connexion — l'appelant s'en sert pour savoir s'il doit se taire.
    bool Tessera_RapporterDegats(RED4ext::ent::EntityID cible, uint32_t degats)
    {
        if (!cible.IsDefined() || degats == 0)
        {
            return false;
        }
        uint64_t idReseau = 0;
        for (const auto& paire : m_networkedEntitiesLookup)
        {
            if (paire.second == cible)
            {
                idReseau = paire.first;
                break;
            }
        }
        if (idReseau == 0)
        {
            return false;
        }
        SendAttackReport(idReseau, degats);
        return true;
    }

    // ── L'ARME EN MAIN : ce que le joueur LOCAL tient, annonce au serveur ─────────────────────
    //
    // `item` = hash TweakDBID de l'arme, `degainee` = elle est en main. Une arme rangee s'annonce
    // avec `degainee = false` ; le serveur ecrit alors 0 et l'avatar se retrouve les mains vides
    // chez tout le monde.
    //
    // ⚠️ N'EMETTRE QUE SUR CHANGEMENT. Le detecteur cote redscript sonde deux fois par seconde
    // (`ArmeAvatar.reds`) ; reemettre a chaque sondage inonderait le fil pour rien ET ferait
    // rejouer l'animation de degainage en boucle chez tous les observateurs. Le filtre vit du cote
    // qui SAIT ce qui a change, pas ici.
    //
    // Renvoie true si un message est parti — jamais qu'il a ete accepte (D1).
    bool Tessera_RapporterArme(uint64_t item, bool degainee);

    // L'arme que le SERVEUR annonce pour une entite reseau donnee. Renvoie un `TweakDBID` INVALIDE
    // (hash 0) si l'entite est inconnue ou si le joueur a les mains vides.
    //
    // ⚠️ RENVOIE UN `TweakDBID`, PAS UN `uint64_t`, ET C'EST OBLIGATOIRE. redscript expose bien
    // `TDBID.ToNumber` mais **aucune conversion inverse** : un hash 64 bits y est un cul-de-sac,
    // impossible a retransformer en identifiant utilisable. La conversion doit donc se faire ICI,
    // ou le hash EST deja un TweakDBID. Verifie dans `core/data/tweakDBID.script` avant d'ecrire
    // la premiere ligne du cote script — sans quoi tout le chemin de reception aurait ete a jeter.
    RED4ext::TweakDBID Tessera_ArmeDeLEntite(RED4ext::ent::EntityID cible) const
    {
        for (const auto& paire : m_networkedEntitiesLookup)
        {
            if (paire.second == cible)
            {
                const auto it = m_appearances.find(paire.first);
                return RED4ext::TweakDBID(it == m_appearances.end() ? 0 : it->second.arme);
            }
        }
        return RED4ext::TweakDBID(static_cast<uint64_t>(0));
    }

    // Demande au serveur de faire reapparaitre le joueur local apres son coma.
    //
    // C'est une DEMANDE : le serveur refuse si le joueur est vivant ou si le delai de secours n'est
    // pas ecoule (`sante.rs::reapparaitre`), et ne repond alors rien. Le bouton de l'ecran de mort
    // n'a donc aucune autorite — il ne fait que demander, ce qui est exactement ce qu'on veut d'un
    // client qu'on ne controle pas.
    //
    // Renvoie true si le message est PARTI, jamais s'il a ete accepte.
    bool Tessera_DemanderReapparition();

    // Secondes de coma restantes, telles que le SERVEUR les pousse. -1 = le joueur n'est pas mort.
    // Lue par l'ecran de mort ; jamais decomptee par le client (deux horloges divergeraient).
    int32_t Tessera_SecondesSecours() const { return m_secondesSecours; }
    // Le serveur autorise-t-il la reapparition ? Le client ne le DEDUIT pas du decompte : c'est le
    // serveur qui tranche, et lui seul refusera une demande prematuree.
    bool Tessera_HopitalOuvert() const { return m_hopitalOuvert; }

    // Faim / soif en POUR MILLE (0-1000), telles que le serveur les pousse. Lues par les jauges du
    // HUD (`TesseraHudVitals`), jamais decomptees par le client : la seule horloge qui compte est
    // celle du serveur, exactement comme pour le coma ci-dessus.
    //
    // `int32_t` et non `uint16_t` : redscript n'a pas de type 16 bits — la conversion se fait au
    // franchissement du fil, meme regle que `nature` dans `Tessera_ReportStim`.
    int32_t Tessera_Faim() const { return m_faim; }
    int32_t Tessera_Soif() const { return m_soif; }

    // Rapporte au serveur une variation de vie que LUI SEUL ne peut pas connaitre : regeneration,
    // soin, chute, feu, PNJ, vehicule. Appelee periodiquement par redscript avec le pourcentage de
    // vie COURANT du joueur local ; toute la logique est ici, pour que le script reste bete.
    //
    // ⚠️ LE PIEGE QUE CETTE FONCTION EXISTE POUR EVITER : le serveur ECRIT lui aussi cette barre
    // (`AppliquerSanteJoueur`, sur `HealthSync`). Sans garde, le client observerait l'ecriture du
    // serveur, la lui renverrait comme une « variation locale », le serveur la reappliquerait — une
    // boucle qui diverge. `m_santeLocaleConnue` est donc remise a jour AUSSI par `HandleHealthSync`,
    // ce qui rend l'ecriture serveur invisible a la detection. C'est le point de conception de tout
    // ce canal.
    //
    // Renvoie le delta REELLEMENT envoye en pour mille (0 = rien, sous le seuil).
    int32_t Tessera_RapporterVariation(float pourcentCourant, uint32_t cause);

    // Cette entite est-elle repliquee par le serveur ? Redscript ne peut pas repondre : la table
    // `networkId → EntityID` vit ici. C'est ce qui distingue un FIGURANT (foule native, purement
    // local) d'une entite deja sous autorite — donc ce qui decide s'il y a lieu de promouvoir.
    bool Tessera_EstEntiteReseau(RED4ext::ent::EntityID cible) const
    {
        for (const auto& paire : m_networkedEntitiesLookup)
        {
            if (paire.second == cible)
            {
                return true;
            }
        }
        return false;
    }

    // ══ INTERACTIONS JOUEUR<->JOUEUR (spec 2026-08-09) ═══════════════════════════════════════
    //
    // Pourquoi une liste indexee et non un tableau rendu d'un coup : redscript ne sait pas recevoir
    // un `array<StructMaison>` d'un natif sans declarer la struct des DEUX cotes, et une struct de
    // plus est une occasion de plus de desynchroniser les deux declarations — panne qui fait tomber
    // TOUT r6/scripts (F-PLF-020). Trois accesseurs scalaires ne peuvent pas diverger.

    /// Combien d'actions ce joueur a le droit de proposer. 0 = catalogue vide (cas legitime : un
    /// serveur sans `actions.toml`), pas une erreur.
    int32_t Tessera_NombreActions() const { return static_cast<int32_t>(m_actions.size()); }

    /// Id de recette a la position `index` — c'est LUI qui repart au serveur dans
    /// `Tessera_EnvoyerAction`. Jamais l'index : le catalogue peut changer entre l'affichage et le
    /// clic (un `/groupgrant` le repousse a chaud), et un index designerait alors autre chose.
    int32_t Tessera_ActionId(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_actions.size())
        {
            return 0;
        }
        return static_cast<int32_t>(m_actions[static_cast<size_t>(index)].id);
    }

    /// Le libelle a afficher, tel que l'operateur l'a ecrit dans son TOML.
    Red::CString Tessera_ActionLibelle(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_actions.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_actions[static_cast<size_t>(index)].libelle.c_str());
    }

    /// Portee en METRES (le fil la porte en decimetres, la conversion se fait a la reception).
    /// 0 = sans limite. Sert a griser/masquer une ligne, jamais a autoriser.
    float Tessera_ActionPorteeM(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_actions.size())
        {
            return 0.0f;
        }
        return m_actions[static_cast<size_t>(index)].portee_m;
    }

    /// Le nom de cette entite, SI on nous l'a donne. Chaine VIDE sinon — et c'est la reponse
    /// normale pour un inconnu, pas une erreur a journaliser.
    ///
    /// ⚠️ Prend une `EntityID` locale et fait la traduction ici, comme `Tessera_RapporterDegats` :
    /// la table `networkId -> EntityID` vit de ce cote, et le script n'a aucun moyen de la refaire.
    Red::CString Tessera_NomConnu(RED4ext::ent::EntityID cible) const
    {
        for (const auto& paire : m_networkedEntitiesLookup)
        {
            if (paire.second == cible)
            {
                const auto it = m_nomsConnus.find(paire.first);
                return Red::CString(it == m_nomsConnus.end() ? "" : it->second.c_str());
            }
        }
        return Red::CString("");
    }

    // L'EntityID de la N-ieme entite reseau. Sert a PARCOURIR les avatars depuis redscript, pour y
    // poser un nametag sans attendre que le joueur en vise un.
    //
    // Le COMPTE se lit avec `Tessera_GetVisiblePlayerCount`, qui rend deja la taille de cette meme
    // table — on n'ajoute donc qu'un seul natif, pas deux.
    //
    // ⚠️ L'index n'est PAS un identifiant : la table est un `std::map` dont l'ordre change quand une
    // entite apparait ou disparait. Un appelant doit s'en servir pour ENUMERER dans la foulee, et
    // memoriser l'`EntityID`, jamais l'index.
    //
    // ⚠️ Elle contient les avatars de joueurs ET les PNJ promus (meme table que
    // `Tessera_EstEntiteReseau`). Ce n'est pas un defaut ici : `Tessera_NomConnu` rend une chaine
    // vide pour tout ce que le serveur n'a pas nomme, donc un PNJ promu n'obtient jamais de nametag.
    RED4ext::ent::EntityID Tessera_AvatarParIndex(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_networkedEntitiesLookup.size())
        {
            return RED4ext::ent::EntityID{};
        }
        auto it = m_networkedEntitiesLookup.begin();
        std::advance(it, index);
        return it->second;
    }

    /// Declenche une recette sur une cible. `recette` est l'id rendu par `Tessera_ActionId`.
    ///
    /// Renvoie true si le message est PARTI — jamais qu'il a ete accepte (D1). Le serveur reverifie
    /// droit, portee et etat, et refuse par un `InteractionResult` porteur d'un motif in-fiction.
    /// `false` ici = cible non reseau, recette nulle, ou pas de connexion.
    bool Tessera_EnvoyerAction(RED4ext::ent::EntityID cible, uint32_t recette)
    {
        if (!cible.IsDefined() || recette == 0)
        {
            return false;
        }
        uint64_t idReseau = 0;
        for (const auto& paire : m_networkedEntitiesLookup)
        {
            if (paire.second == cible)
            {
                idReseau = paire.first;
                break;
            }
        }
        if (idReseau == 0)
        {
            // Un figurant de la foule native ne designe personne chez le serveur. Lui inventer une
            // identite serait un mensonge sur le fil (meme regle que `Tessera_RapporterDegats`).
            return false;
        }
        SendActionJoueur(idReseau, recette);
        return true;
    }

    // Demande au serveur de prendre un figurant sous son autorite (ADR 0022).
    //
    // On envoie de quoi le REFABRIQUER, pas un identifiant : le pantin n'existe que sur cette
    // machine, les autres joueurs ont d'autres passants au meme endroit. Voir `PromotionRequest`
    // dans protocol.fbs.
    bool Tessera_DemanderPromotion(uint64_t record, RED4ext::CName apparence, float x, float y,
                                   float z, float yaw, bool mort)
    {
        return SendPromotionRequest(record, apparence.hash, x, y, z, yaw, mort);
    }

    // Journal de SONDE, ecrit dans le log du plugin — donc UN FICHIER PAR INSTANCE DE JEU.
    //
    // Pourquoi ne pas utiliser `FTLog` : il ecrit dans `cyber_engine_tweaks/gamelog.log`, PARTAGE
    // par toutes les instances. Deux clients lances sur la meme install y melangent leurs lignes,
    // ce qui rend toute comparaison entre eux impossible — or comparer deux clients est justement
    // ce que nos sondes font depuis qu'on sait en lancer deux (F-PLF-022).
    void Tessera_Journal(const Red::CString& texte)
    {
        SDK->logger->InfoF(PLUGIN, "%s", texte.c_str());
    }

    // Rapporte un statique au serveur depuis redscript.
    //
    // ⚠️ `entityId` est passe TEL QUEL, sans traduction — contrairement a `Tessera_ReportStim` qui
    // traduit en id reseau. C'est LA difference entre les deux populations : l'identifiant d'un
    // statique derive des donnees de secteur et vaut la meme chose sur toutes les machines
    // (F-PNJ-128), donc il DESIGNE quelque chose pour le serveur. Celui d'un passant ne designe
    // rien hors de sa machine.
    void Tessera_RapporterStatique(RED4ext::ent::EntityID cible, uint64_t record,
                                   RED4ext::CName apparence, float x, float y, float z, float yaw)
    {
        // ⚠️ La position est celle du PNJ, pas du joueur : c'est elle qui range le rapport dans la
        // bonne cellule du halo. Un joueur voit a 80 m, donc souvent dans une autre cellule que la
        // sienne — ranger sur la position du rapporteur eparpillerait la table.
        SendStaticNpcReport(cible.hash, record, apparence.hash, x, y, z, yaw);
    }

    // Cette cellule a-t-elle deja ete servie par le serveur ? Si oui, inutile d'y rapporter quoi que
    // ce soit : c'est ce qui fait tomber le trafic de 5 rapports/s a un par cellule vierge.
    bool Tessera_CelluleConnue(float x, float y) const
    {
        return g_cellulesRecues.contains(CelluleDe(x, y));
    }

    // Apparence faisant autorite deja connue pour ce statique, ou CName nulle si le serveur n'a
    // rien dit. Sert au REJEU : une apparence peut arriver avant que le PNJ ne soit streame, auquel
    // cas l'application echoue et doit se refaire a son attachement.
    RED4ext::CName Tessera_ApparenceStatiqueConnue(RED4ext::ent::EntityID cible) const
    {
        const auto it = g_apparencesStatiques.find(cible.hash);
        // ⚠️ `CName()` et non `CName(0)` : le litteral 0 est un `int`, ambigu avec le
        // constructeur `const char*`. Le defaut vaut deja hash = 0.
        return it == g_apparencesStatiques.end() ? RED4ext::CName() : RED4ext::CName(it->second);
    }

    // SONDE one-shot : `ScheduleAppearanceChange` a-t-il un effet sur un PNJ de communaute ?
    //
    // Renvoie une apparence DIFFERENTE deja vue pour ce meme record — donc forcement valide, le
    // moteur rejetant en silence une apparence etrangere a l'entite (F-PNJ-051) — ou une CName nulle
    // s'il n'y a pas encore de quoi comparer. Memorise au passage.
    //
    // ⚠️ L'etat vit ICI et non en redscript : une classe `ScriptableSystem` maison n'a PAS ete
    // resolue par `GetScriptableSystemsContainer().Get()` (essai du 2026-08-08, 171 pantins classes
    // et zero appel). Le C++ garde l'etat, c'est eprouve.
    RED4ext::CName Tessera_CobayeApparence(uint64_t record, RED4ext::CName apparence)
    {
        if (g_sondeApparenceFaite || apparence.hash == 0)
        {
            return RED4ext::CName();
        }
        const auto it = g_premiereApparence.find(record);
        if (it == g_premiereApparence.end())
        {
            g_premiereApparence[record] = apparence.hash;
            return RED4ext::CName();
        }
        if (it->second == apparence.hash)
        {
            return RED4ext::CName(); // meme apparence : pas un cobaye utile
        }
        g_sondeApparenceFaite = true;
        return RED4ext::CName(it->second);
    }

    /// Called from the plugin load and unload events
    static bool Load();
    /// Called from the plugin load and unload events
    static void Unload();

    // Accesseur natif exposé au redscript comme `GameInstance.GetNetworkGameSystem()` (déclaré dans
    // RedscriptModule/src/Network/NetworkGameSystem.reds). Ce backing MANQUAIT : le `native func`
    // était déclaré côté redscript mais aucune fonction C++ ne l'enregistrait → redscript ne
    // résolvait pas l'appel → tout r6/scripts échouait à compiler (modset entier mort au lancement,
    // diagnostiqué 2026-07-18). Pattern calé sur Codeware (App::ResourceDepot::Get +
    // RTTI_EXPAND_CLASS(Red::ScriptGameInstance)). Red::ToHandle partage le refcount existant du
    // système via .Lock() (aucun double-free), cf. RedLib include/Red/Utils/Handles.hpp.
    //
    // ⚠️ NE JAMAIS nommer cette méthode `Get()`. RedLib détecte `static T::Get() -> Handle<T>` via
    // le concept `HasSystemGetter` (red-lib Definition.hpp:32) et l'appelle depuis
    // `SystemBuilder::BuildSystem()` pour CONSTRUIRE le game system au chargement. Or notre
    // accesseur RÉCUPÈRE l'existant (`GetGameSystem` = null tant que le système n'est pas créé) :
    // nommée `Get`, `BuildSystem` renvoie null → `RegisterSystem` bail (`if (!systemInstance)
    // return;`) → le système n'est JAMAIS créé ni tické → l'auto-connexion réseau du 1er tick ne
    // part jamais → le client ne se connecte plus DU TOUT. Régression exacte de netcode-v0.1.5
    // (l'ajout de `Get()` pour l'accesseur a rendu le multi injouable, 2026-07-18/19). ResourceDepot
    // (Codeware) PEUT s'appeler `Get()` : c'est un singleton `Core::Feature`, PAS un IGameSystem —
    // il ne passe pas par `SystemBuilder`. Nous sommes un IGameSystem → nom neutre OBLIGATOIRE.
    static Red::Handle<NetworkGameSystem> Resolve()
    {
        return Red::ToHandle(Red::GetGameSystem<NetworkGameSystem>());
    }
private:
    RTTI_IMPL_TYPEINFO(NetworkGameSystem);
    RTTI_IMPL_ALLOCATOR();
};

RTTI_DEFINE_CLASS(NetworkGameSystem, {
    RTTI_METHOD(EnqueueLoadLastCheckpoint);
    RTTI_METHOD(Tessera_GetServerShard);
    RTTI_METHOD(Tessera_GetServerOverlaps);
    RTTI_METHOD(Tessera_GetVisiblePlayerCount);
    RTTI_METHOD(Tessera_ReportStim);
    RTTI_METHOD(Tessera_EstEntiteReseau);
    RTTI_METHOD(Tessera_RapporterDegats);
    RTTI_METHOD(Tessera_RapporterArme);
    RTTI_METHOD(Tessera_ArmeDeLEntite);
    RTTI_METHOD(Tessera_DemanderReapparition);
    RTTI_METHOD(Tessera_RapporterVariation);
    RTTI_METHOD(Tessera_SecondesSecours);
    RTTI_METHOD(Tessera_HopitalOuvert);
    RTTI_METHOD(Tessera_Faim);
    RTTI_METHOD(Tessera_Soif);
    RTTI_METHOD(Tessera_Journal);
    RTTI_METHOD(Tessera_RapporterStatique);
    RTTI_METHOD(Tessera_ApparenceStatiqueConnue);
    RTTI_METHOD(Tessera_CelluleConnue);
    RTTI_METHOD(Tessera_CobayeApparence);
    RTTI_METHOD(Tessera_DemanderPromotion);
    RTTI_METHOD(Tessera_NombrePersonnages);
    RTTI_METHOD(Tessera_ListePersonnagesRecue);
    RTTI_METHOD(Tessera_ModeDeveloppement);
    RTTI_METHOD(Tessera_NomPersonnage);
    RTTI_METHOD(Tessera_IdPersonnage);
    RTTI_METHOD(Tessera_RecordPersonnage);
    RTTI_METHOD(Tessera_ApparencePersonnage);
    RTTI_METHOD(Tessera_DernierResultat);
    RTTI_METHOD(Tessera_CreerPersonnage);
    RTTI_METHOD(Tessera_ChoisirPersonnage);
    RTTI_METHOD(Tessera_SupprimerPersonnage);
    // Interactions joueur<->joueur. ⚠️ Chacune de ces cinq lignes a son `public native func` dans
    // `RedscriptModule/src/Network/NetworkGameSystem.reds` : les deux cotes se posent ET se
    // deploient ENSEMBLE. Un `native func` sans backing dans la DLL deployee fait tomber TOUT
    // r6/scripts et le jeu se ferme sans un mot (F-PLF-020, F-PLF-023).
    RTTI_METHOD(Tessera_NombreActions);
    RTTI_METHOD(Tessera_ActionId);
    RTTI_METHOD(Tessera_ActionLibelle);
    RTTI_METHOD(Tessera_ActionPorteeM);
    RTTI_METHOD(Tessera_NomConnu);
    RTTI_METHOD(Tessera_EnvoyerAction);
    RTTI_METHOD(Tessera_AvatarParIndex);
    RTTI_PROPERTY(FullyConnected);
    RTTI_PROPERTY(playerActionTracker);
    RTTI_ALIAS("Cyberverse.Network.Managers.NetworkGameSystem");
});

// Enregistre l'accesseur `GameInstance.GetNetworkGameSystem()` attendu par le RedscriptModule.
// Sans ce bloc, `@addMethod(GameInstance) static native func GetNetworkGameSystem()` n'a aucun
// backing natif → [UNRESOLVED_TYPE] à la compilation redscript → modset entier refusé. Même
// pattern que Codeware ResourceDepot (RTTI_EXPAND_CLASS(Red::ScriptGameInstance) + RTTI_METHOD_FQN).
RTTI_EXPAND_CLASS(Red::ScriptGameInstance, {
    RTTI_METHOD_FQN(NetworkGameSystem::Resolve, "GetNetworkGameSystem");
});

// TODO: Thing about the concept of having EnqueueMessage public, it causes _this_, at least with templates: We need to
//  expliticly state template invocations for the code to be generated, as otherwise NetworkGameSystem.cpp doesn't know
//  about PlayerActionTracked. Maybe we could also inline the whole EnqueueMessage code, then it will be emited into the
//  relevant caller CU (but that may be more than one for the same packet, making template hell even worse).
template bool NetworkGameSystem::EnqueueMessage(uint8_t channel_id, PlayerActionTracked msg);
template bool NetworkGameSystem::EnqueueMessage(uint8_t channel_id, PlayerSpawnCar msg);
template bool NetworkGameSystem::EnqueueMessage(uint8_t channel_id, PlayerUnmountCar msg);
template bool NetworkGameSystem::EnqueueMessage(uint8_t channel_id, PlayerEquipItem msg);
template bool NetworkGameSystem::EnqueueMessage(uint8_t channel_id, PlayerShoot msg);

#endif //NETWORKMANAGERCONTROLLER_H

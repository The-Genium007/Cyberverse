#ifndef NETWORKMANAGERCONTROLLER_H
#define NETWORKMANAGERCONTROLLER_H
#include <RED4ext/RED4ext.hpp>
#include "RED4ext/SystemUpdate.hpp"

#include "RED4ext/Scripting/IScriptable.hpp"
#include "RED4ext/Scripting/Natives/Generated/ink/ISystemRequestsHandler.hpp"
#include "RED4ext/Scripting/Stack.hpp"

#include "PlayerActionTracker.h"
#include "PlayerSync/InterpolationData.h"
#include "RED4ext/Scripting/Natives/Generated/AI/Command.hpp"
#include "RED4ext/Scripting/Natives/Generated/Vector4.hpp"
#include "RED4ext/Scripting/Natives/entEntityID.hpp"

#include <RedLib.hpp>
#include <clientbound/WorldPacketsClientBound.h>
#include <map>
#include <string>
#include <steam/isteamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

#include <serverbound/WorldPacketsServerBound.h>

// Protocole TesseraSynth (FlatBuffers) — forward decl pour ne pas tirer l'en-tête généré ici.
namespace cyberpunk_rp::protocol {
    struct Snapshot; struct PositionCorrection; struct ShardAssignment;
    struct WorldState; struct Kicked; struct AppearanceSync; struct ConfigSync;
}

// Identité visuelle d'une entité réseau, telle que le SERVEUR la décide (`AppearanceSync`).
// Deux hashes suffisent (modèle PRESET, ADR/design apparence §6.2) : le record TweakDB à faire
// apparaître et le nom d'apparence `.ent` à appliquer. C'est ce qui remplace le `Character.Panam`
// codé en dur — l'apparence n'est plus une constante du client, c'est une donnée serveur.
struct NetworkAppearance
{
    uint64_t baseRecord = 0;
    uint64_t appearance = 0;
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
    std::map<RED4ext::ent::EntityID, InterpolationData> m_interpolationData;
    std::map<RED4ext::ent::EntityID, RED4ext::Handle<RED4ext::AICommand>> m_LastTeleportCommand;
    float m_TimeSinceLastPlayerPositionSync;

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
    void InterpolatePuppets(float deltaTime);
    void SetEntityPosition(RED4ext::ent::EntityID entityId, RED4ext::Vector4 worldPosition, float yaw);
    // Placement HYBRIDE : marche animée quand l'entité bouge et que la dérive est faible,
    // téléportation de correction sinon. Le mécanisme vient de F-PNJ-082/F-PLY-007, qui posent
    // aussi la règle : la commande de marche est de l'animation, l'autorité reste au Snapshot.
    /// `moveTarget` : destination du PNJ (plusieurs metres), pas sa position du tick suivant.
    /// Nul = inconnue, on retombe sur `worldPosition`. Voir le commentaire dans le .cpp.
    void SetEntityPose(RED4ext::ent::EntityID entityId, RED4ext::Vector4 worldPosition, float yaw,
                       uint8_t locomotion, const RED4ext::Vector4* moveTarget = nullptr);

protected:
    void PollIncomingMessages();
    void TrackPlayerPosition(float deltaTime);

    // --- Couture protocole TesseraSynth (FlatBuffers) ---
    // Envoient un ClientEnvelope (Join / PositionUpdate) au serveur Rust autoritaire.
    void SendJoin(const std::string& displayName);
    void SendPositionUpdate(float x, float y, float z, float yaw);
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

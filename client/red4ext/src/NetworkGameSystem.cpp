#include "NetworkGameSystem.h"

#include "CommandLine.h"
#include "Main.h"
#include "Utils.h"

#include "RED4ext/RTTISystem.hpp"
#include "RED4ext/Scripting/Natives/Generated/EulerAngles.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/TeleportationFacility.hpp"
#include "RED4ext/Scripting/Utils.hpp"
#include "RED4ext/SystemUpdate.hpp"

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h> // Required, see https://github.com/ValveSoftware/GameNetworkingSockets/issues/171^
#include <steam/steamnetworkingsockets.h>

#include "serverbound/AuthPacketsServerBound.h"
#include "serverbound/WorldPacketsServerBound.h"
#include "clientbound/AuthPacketsClientBound.h"
#include "clientbound/WorldPacketsClientBound.h"

#include <zpp_bits.h>

#include <chrono>  // etranglement des stimulus remontes au serveur (budget 40 msg/s du Gateway)
#include <tuple>   // cle de deduplication des promotions (record + position arrondie)
#include <cmath>   // std::lround/std::fmod (quantization du fil, gel palier 2 — cf. QuantPos/QuantYaw)
#include <cstdlib> // std::getenv (token ZITADEL transmis par le launcher, cf. SendJoin)
#include <set>     // set des ids presents dans un Snapshot + garde anti-spam de LogUnhandledServerMsg
#include <thread>  // alerte « modset non compile » affichee hors boucle de jeu (NotifyModsetNotCompiled)

// Protocole TesseraSynth (FlatBuffers) + en-tetes generes.
#include <flatbuffers/flatbuffers.h>
#include "generated/protocol_generated.h"

// Version du protocole FlatBuffers parlee par ce client. DOIT rester egale a
// CURRENT_PROTOCOL_VERSION cote serveur (tessera-core/server/src/gateway_routing.rs) : le Gateway
// compare les deux au Join et kicke sur mismatch ("kick : version protocole incompatible").
//
// Piege (vecu le 2026-07-15, jeu injouable) : protocol_version a ete ajoute au schema le
// 2026-07-13 alors que le netcode publie datait du 2026-06-29. L'en-tete genere ne connaissait pas
// le champ, CreateJoin ne le posait pas, FlatBuffers renvoyait le defaut 0 cote serveur -> kick a
// chaque connexion. Rien ne cassait a la compilation : le parametre a un defaut (= 0), donc un
// appel qui l'omet compile silencieusement et ment sur le fil.
// => Toujours passer cette constante EXPLICITEMENT a CreateJoin, jamais s'appuyer sur le defaut.
// => A regenerer avec l'en-tete des que protocol.fbs bouge :
//    flatc --cpp -o client/red4ext/src/generated <chemin>/protocol.fbs   (flatc 25.12.19)
// v2 : gel palier 2 (2026-07-23) — positions Vec3->QVec3 fixed-point, yaw float->ushort.
static constexpr uint32_t kTesseraProtocolVersion = 2;

// Record de repli quand le serveur n'a poussé AUCUNE apparence pour une entité réseau. Ce n'est
// PAS la valeur normale : c'est le filet qui rend visible un trou d'autorité au lieu de laisser un
// joueur invisible. Toute entité rendue avec ce record signale que son `AppearanceSync` n'est pas
// arrivé — et le log qui l'accompagne le dit.
//
// Historiquement, `Character.Panam` était codé en dur comme apparence NORMALE de tout joueur
// distant : tous les joueurs se ressemblaient, et le serveur — qui tient pourtant l'apparence
// choisie de chacun (`appearance_relay.rs`) — n'avait aucun moyen de le dire.
static constexpr const char* kFallbackAvatarRecord = "Character.Panam";

// Journalise UNE SEULE FOIS par type de message serveur non câblé. Sans ce garde, un message
// diffusé à 20 Hz remplirait le log à lui seul (précédent vécu : des centaines de lignes par
// seconde sur l'échec de spawn), et le log cesserait d'être lisible — donc de servir.
static void LogUnhandledServerMsg(int msgType)
{
    static std::set<int> seen;
    if (seen.insert(msgType).second)
    {
        SDK->logger->InfoF(PLUGIN,
            "ServerMsg type=%d recu mais pas encore cable cote client (premiere occurrence "
            "seulement). Le serveur l'emet deja.", msgType);
    }
}

// --- Quantization du fil (gel palier 2) — miroir EXACT de tessera-core/server/src/quant.rs ---
// Position : fixed-point WORLD-ABSOLU, metres = bits / 131072 (2^17) — la representation native
// du jeu. Grille absolue : une position s'encode bit-a-bit identiquement quel que soit le shard.
// Yaw : uint16, 0..65535 = 0..360 degres. Le yaw du jeu (degres, potentiellement negatif) est
// normalise dans [0, 360) avant quantization — meme formule que q_yaw cote Rust.
static constexpr float kQuantPosScale = 131072.0f;
static inline int32_t QuantPos(float meters)
{
    return static_cast<int32_t>(std::lround(meters * kQuantPosScale));
}
static inline float DequantPos(int32_t bits)
{
    return static_cast<float>(bits) / kQuantPosScale;
}
static inline uint16_t QuantYaw(float degrees)
{
    float normalized = std::fmod(degrees, 360.0f);
    if (normalized < 0.0f)
    {
        normalized += 360.0f; // rem_euclid : [0, 360)
    }
    const long q = std::lround(normalized * (65536.0f / 360.0f));
    return static_cast<uint16_t>(q % 65536); // 65536 (=360deg exactement) revient a 0
}
static inline float DequantYaw(uint16_t q)
{
    return static_cast<float>(q) * (360.0f / 65536.0f);
}

// --- Détection « modset non compilé » (incident playtest 2026-07-20, cf. NetworkGameSystem.h) ---
// Nombre d'échecs de spawn avant d'alerter le joueur. Le débit dépend du nombre de joueurs visibles
// et de la cadence des snapshots (20 Hz) : un seul joueur en vue produit déjà ~20 échecs/s. 40
// laisse donc passer un hoquet d'une seconde ou deux sans crier, tout en alertant bien avant que le
// joueur ait eu le temps de conclure « le serveur est vide ».
static constexpr uint64_t kSpawnFailureAlertThreshold = 40;
// Période d'agrégation des logs, en secondes. Sans elle : ~5 Mo de lignes identiques par partie
// (constaté sur le log d'un playtester le 2026-07-20).
static constexpr float kSpawnFailureLogPeriodSeconds = 5.0f;

#include <set>

bool NetworkGameSystem::Load()
{
    if (SteamDatagramErrMsg errMsg; !GameNetworkingSockets_Init(nullptr, errMsg))
    {
        SDK->logger->ErrorF(PLUGIN, "Could not initialize GameNetworkingSockets: %s", errMsg);
        return false;
    }

    return true;
}
void NetworkGameSystem::Unload()
{
    GameNetworkingSockets_Kill();
}

bool NetworkGameSystem::ConnectToServer(const std::string& host, uint16_t port)
{
    SDK->logger->InfoF(PLUGIN, "Trying to connect to server at %s:%d", host.c_str(), port);

    if (m_pInterface != nullptr)
    {
        SDK->logger->Warn(PLUGIN, "Trying to connect while already being connected. Aborting");
        return false;
    }

    m_pInterface = SteamNetworkingSockets();

    if (m_pInterface == nullptr)
    {
        SDK->logger->Error(PLUGIN, "Failed to initialize the networking library");
    }

    // Since I don't want to parse the ip manually and support both IP versions, I need to create a string first...
    const auto connection_string_size = host.length() + 2 + 5; // 2 (':' + \0) and 5 (65535)
    const auto connection_string = new char[connection_string_size];
    memset(connection_string, '\0', connection_string_size);
    sprintf_s(connection_string, connection_string_size, "%s:%d", host.c_str(), port);

    SteamNetworkingIPAddr address = {};
    if (!address.ParseString(connection_string))
    {
        SDK->logger->WarnF(PLUGIN, "Failed to parse connection string \"%s\"", connection_string);
        m_pInterface = nullptr; // prevent polling
        return false;
    }

    SteamNetworkingConfigValue_t opt = {};
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(ConnectionStatusChangedCallback));

    // m_hConnection = m_pInterface->ConnectToHostedDedicatedServer(identity, 0, 1, &opt);
    m_hConnection = m_pInterface->ConnectByIPAddress(address, 1, &opt);

    if (m_hConnection == k_HSteamNetConnection_Invalid)
    {
        SDK->logger->WarnF(PLUGIN, "Could not create connection for string \"%s\": invalid", connection_string);
        return false;
    }

    return true;
}

void NetworkGameSystem::OnNetworkUpdate(RED4ext::FrameInfo& frame_info, RED4ext::JobQueue& job_queue)
{
    // TODO: make this framerate indepedent, maybe also use multiple UpdateTickGroups.
    if (!m_hasTriedToConnect)
    {
        // We auto-connect on the first tick with the CLI address. We don't connect earlier because the message loop
        // isn't run there yet and we are prone to time out.
        const auto commandLine = GetCommandLineA();
        // DIAGNOSTIC TesseraSynth : tracer ce que voit réellement le tick réseau au 1er passage.
        SDK->logger->InfoF(PLUGIN, "[net] 1er tick — GetCommandLineA = %s", commandLine ? commandLine : "(null)");
        const auto host = ParseHostFromCommandLine(commandLine);
        const auto port = ParsePortFromCommandLine(commandLine);
        if (host.has_value() && port.has_value())
        {
            SDK->logger->InfoF(PLUGIN, "[net] args OK : host=%s port=%d", host->c_str(), port.value());
            ConnectToServer(host.value(), port.value());
        }
        else
        {
            SDK->logger->Warn(PLUGIN, "[net] pas d'adresse serveur sur la ligne de commande "
                                      "(--cyberverse-server-address= / --cyberverse-server-port=)");
        }
        m_hasTriedToConnect = true; // We lie here, to prevent parsing the cli every time.
    }

    if (m_pInterface == nullptr)
    {
        return;
    }

    PollIncomingMessages();
    TrackPlayerPosition(frame_info.deltaTime);
    InterpolatePuppets(frame_info.deltaTime);

    // Log agrégé des échecs de spawn : une ligne périodique avec le total, plutôt qu'une ligne par
    // snapshot et par joueur (~5 Mo de lignes identiques constatés sur un log de playtest).
    if (m_spawnFailureCount > 0)
    {
        m_timeSinceSpawnFailureLog += frame_info.deltaTime;
        if (m_timeSinceSpawnFailureLog >= kSpawnFailureLogPeriodSeconds)
        {
            m_timeSinceSpawnFailureLog = 0.0f;
            SDK->logger->WarnF(PLUGIN,
                "Echec spawn avatar reseau : %llu au total depuis le debut de la session "
                "(modset redscript non compile ? voir r6/logs/redscript_rCURRENT.log)",
                m_spawnFailureCount);
        }
    }

    m_pInterface->RunCallbacks(); // This shall be called in a loop.
}

void NetworkGameSystem::OnRegisterUpdates(RED4ext::UpdateRegistrar* aRegistrar)
{
    // TODO: If we have no connection information passed on the command line, we have no reason to even register.
    IGameSystem::OnRegisterUpdates(aRegistrar);
    aRegistrar->RegisterUpdate(RED4ext::UpdateTickGroup::FrameBegin, this, "NetworkUpdate",
        [this](RED4ext::FrameInfo &frame_info, RED4ext::JobQueue &job_queue) {
            this->OnNetworkUpdate(frame_info, job_queue);
    });
}

void NetworkGameSystem::ConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    SDK->logger->InfoF(PLUGIN, "Connection Status Changed (%d): %s", pInfo->m_info.m_eState,
                       pInfo->m_info.m_szEndDebug);

    if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected)
    {
        // Nom affiche : `TESSERA_DISPLAY_NAME` s'il est pose, sinon le nom de session Windows.
        //
        // Pourquoi cette surcharge existe : sur un serveur PRIVE (`identity.public = false`), la
        // cle de persistance du joueur EST son display_name (`resolve_join_key`, gateway.rs:494).
        // Deux instances du jeu sur la MEME machine renvoient donc le meme nom, partagent le meme
        // compte, et se disputent la meme position — ce qui rend tout test a deux clients
        // impossible en local. Mesure du 2026-08-07 : deux instances tournent bien en parallele
        // (aucun verrou d'instance unique), seul le nom bloquait.
        //
        // ⚠️ C'est un outil de TEST, pas une identite. Sur un serveur public la variable est sans
        // effet : le `display_name` du client y est deja ignore au profit du `sub` du JWT verifie.
        // Elle ne cree donc aucune voie d'usurpation nouvelle.
        char buf[255];
        DWORD buf_len = 255;
        GetUserNameA(buf, &buf_len);
        const char* nomForce = std::getenv("TESSERA_DISPLAY_NAME");
        const std::string nom = (nomForce != nullptr && nomForce[0] != '\0') ? nomForce : buf;

        SDK->logger->InfoF(PLUGIN, "Socket connected, sending Join (TesseraSynth) — nom « %s »%s",
            nom.c_str(), nomForce != nullptr && nomForce[0] != '\0' ? " (force par TESSERA_DISPLAY_NAME)" : "");
        auto* system = Red::GetGameSystem<NetworkGameSystem>();
        system->SendJoin(nom);
        // Notre serveur n'a pas d'ACK d'auth : connexion etablie = pret.
        system->FullyConnected = true;
    } else {
        Red::GetGameSystem<NetworkGameSystem>()->FullyConnected = false;
    }
}

template<typename T>
bool NetworkGameSystem::EnqueueMessage(uint8_t channel_id, T content)
{
    auto frame = MessageFrame{};
    frame.channel_id = channel_id;
    content.FillMessageFrame(frame);

    auto [data, out] = zpp::bits::data_out();

    auto zpp_result = out(frame);
    if (zpp::bits::failure(zpp_result))
    {
        // Failed to serialize the frame(!)
        return false;
    }

    zpp_result = out(content);
    if (zpp::bits::failure(zpp_result))
    {
        // Failed to serialize the content
        return false;
    }

    // TODO: derive the send flags from the channel id, i.e. lookup registered channels.
    assert(data.size() < std::numeric_limits<uint32_t>::max());

    const auto result = m_pInterface->SendMessageToConnection(
        m_hConnection, data.data(), static_cast<uint32_t>(data.size()), k_nSteamNetworkingSend_Reliable, nullptr);

    if (result == k_EResultOK)
    {
        return true;
    }

    SDK->logger->ErrorF(PLUGIN, "NetworkGameSystem::EnqueueMessage(%d) => Error %d\n", channel_id, result);
    return false;
}


namespace
{
/// Derniere destination REELLEMENT commandee a chaque entite reseau.
///
/// ⚠️ HORS DE LA CLASSE, et ce n'est pas un detail de style. `NetworkGameSystem` est alloue par le
/// moteur (`RTTI_IMPL_ALLOCATOR`) : lui AJOUTER UN MEMBRE corrompt la memoire voisine. Mesure du
/// 2026-08-06 — avec un membre de plus, `m_spawnFailureCount` (declare `= 0`, et dont le seul
/// chemin d'incrementation n'avait jamais ete execute) affichait 2 190 697 836 704, et le jeu
/// tombait juste apres la creation de la PREMIERE entite. L'ancienne DLL, elle, encaissait le
/// meme serveur sans broncher : 39 PNJ, session stable.
///
/// Trois flottants nus, pas un `RED4ext::Vector4` : ce dernier est `__declspec(align(0x10))` et
/// n'a rien a faire dans les noeuds d'une `std::map`.
struct CibleCommandee
{
    float x;
    float y;
    float z;
};
std::map<uint64_t, CibleCommandee> g_dernieresCibles;

/// Dernier envoi par type de stimulus — meme raison d'etre HORS DE LA CLASSE que ci-dessus.
///
/// Pourquoi un etranglement est NECESSAIRE et pas un confort : le Gateway plafonne chaque client a
/// 40 messages/s, TOUTES familles confondues (rate_limit.rs). Une rafale d'arme automatique emet un
/// `Gunshot` par balle, soit ~10/s a elle seule — sans garde, les stimulus mangeraient le budget des
/// mises a jour de position, et un joueur qui arrose se ferait deconnecter pour flood.
///
/// 250 ms : une rafale devient ~4 messages/s au lieu de ~10, et deux coups de feu DISTINCTS restent
/// distinguables. Valeur v1, jamais mesuree en charge reelle.
///
/// ⚠️ La cle est le couple (type, RAYON), pas le type seul — corrige apres mesure en jeu
/// (F-PNJ-113). Un tir emet DEUX `Gunshot` dans la meme frame : la portee audio (30 m dehors) puis
/// la portee visuelle (50 m). Keye sur le seul type, l'etranglement supprimait la seconde, et la
/// scene vue par les autres joueurs aurait ete plus etroite que celle vue par le tireur — une
/// perte d'information qu'aucune relecture n'avait revelee.
///
/// Une rafale d'arme automatique reste collapsee : ses tirs portent le meme rayon.
std::map<std::pair<uint8_t, uint16_t>, std::chrono::steady_clock::time_point> g_derniersStims;
constexpr auto kIntervalleStimMin = std::chrono::milliseconds(250);

/// Figurants deja soumis a promotion — (record, position arrondie au metre).
///
/// Sans ce garde, le MEME pantin serait promu a chaque stimulus qu'il declenche : un joueur qui
/// vide un chargeur en creerait dix, et le serveur paierait dix entites la ou une suffit. Le
/// serveur ne peut pas dedupliquer a notre place — il ne sait pas que ces requetes designent le
/// meme passant, puisqu'il n'a justement AUCUNE identite pour lui (c'est tout le probleme que la
/// promotion contourne).
///
/// La position entre dans la cle : deux passants du meme archetype a deux endroits restent deux
/// promotions distinctes. Arrondie au metre, parce qu'un pantin bouge entre deux tirs.
///
/// ponytail: jamais purge — un set qui grossit avec le nombre de promotions de la session.
/// A borner si une session longue le montre.
std::set<std::tuple<uint64_t, int32_t, int32_t, int32_t>> g_promotionsDemandees;

/// Entites reseau deja mises a l'etat MORT chez nous.
///
/// Le comportement voyage dans CHAQUE snapshot : sans memoire, on rejouerait `Kill` vingt fois par
/// seconde sur le meme cadavre. Le set retient ce qui est deja fait.
std::set<uint64_t> g_cadavresAppliques;
/// Nombre de tentatives par entite — sert uniquement au diagnostic (voir le log associe).
std::map<uint64_t, uint32_t> g_essaisCadavre;
constexpr uint8_t kComportementATerre = 5;
} // namespace

void NetworkGameSystem::SetEntityPose(uint64_t networkId, RED4ext::ent::EntityID entityId,
                                      RED4ext::Vector4 worldPosition, float yaw, uint8_t locomotion,
                                      const RED4ext::Vector4* moveTarget)
{
    // Modele HYBRIDE, prescrit par la mesure et non choisi au jugé.
    //
    // `AIMoveToCommand` fait reellement MARCHER et naviguer une entite commandee par le serveur
    // (F-PNJ-082, en jeu le 2026-07-22 ; F-PLY-007 pour l'usage avatar distant). Mais ce meme fait
    // precise que c'est de l'ANIMATION LOCALE, PAS de l'autorite : « la position qui fait foi
    // reste celle du Snapshot serveur, corrigee a chaque tick ».
    //
    // D'ou les deux regimes :
    //  · entite IMMOBILE (locomotion 0) -> placement direct. Rien a animer, et une commande de
    //    marche vers un point ou l'on est deja produirait un piétinement.
    //  · entite EN MOUVEMENT -> on demande au moteur de MARCHER vers la position serveur. Le
    //    trajet est joué, pas saute. C'est ce qui remplace le glissement.
    //  · derive TROP GRANDE -> teleportation de CORRECTION. Sans ce garde, une entite qui a rate
    //    des snapshots (perte de paquets, sortie/rentree d'AoI) marcherait indefiniment vers une
    //    cible qu'elle ne rattraperait jamais, en accumulant du retard.
    static constexpr float kCorrectionDistanceMeters = 8.0f;

    if (locomotion != 0)
    {
        const auto entity = Cyberverse::Utils::GetDynamicEntity(entityId);
        if (entity.has_value())
        {
            const auto current = Cyberverse::Utils::Entity_GetWorldPosition(entity.value());
            const float dx = worldPosition.X - current.X;
            const float dy = worldPosition.Y - current.Y;
            const float dz = worldPosition.Z - current.Z;
            const float drift = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (drift < kCorrectionDistanceMeters)
            {
                // ── VISER LA DESTINATION, PAS LA POSITION ───────────────────────────────
                //
                // `AIMoveToCommand` porte `finishWhenDestinationReached`. A 20 Hz, la position
                // serveur n'est en avant que de la distance parcourue en UN tick : 15 cm a 3 m/s,
                // 5 cm a 1 m/s. Le pantin l'atteint instantanement, la commande se termine, et la
                // suivante arrive avant qu'une marche ait pu s'amorcer — il fremit sur place.
                //
                // Mesure du 2026-08-06 : « ils trottinent » a 3 m/s, « ils marchent bien » a 1,4,
                // « ils sont statiques » a 1,0. Ralentir RAPPROCHAIT la cible ; la vitesse n'etait
                // pas la cause, elle etait le seul parametre qui la masquait.
                //
                // `move_target` (protocol.fbs) porte le point de cheminement courant, a plusieurs
                // metres. Le pantin marche alors en continu a l'allure du MOTEUR, et
                // `worldPosition` reprend son role unique : l'autorite et la correction de derive
                // (le calcul de `drift` ci-dessus, inchange).
                //
                // Absent = serveur plus ancien, ou PNJ sans chemin : on retombe sur la position,
                // c'est-a-dire le comportement d'avant ce champ.
                const RED4ext::Vector4 destination = moveTarget ? *moveTarget : worldPosition;

                // ── NE PAS REJOUER UN ORDRE INCHANGE ────────────────────────────────────
                //
                // Viser une vraie destination transforme `AIMoveToCommand` en cheminement REEL.
                // La reemettre a chaque tick revient donc a relancer 20 cheminements par seconde
                // et par PNJ. A 156 PNJ, le moteur s'est effondre — le jeu est tombe quelques
                // minutes apres le deploiement du 2026-08-06.
                //
                // Une commande de marche vers un point fixe n'a aucune raison d'etre reemise tant
                // que ce point n'a pas bouge : le pantin est deja en route. On ne la rejoue donc
                // que si la destination a change de plus d'un metre — soit une fois par point de
                // cheminement au lieu de vingt fois par seconde.
                //
                // Sortir ici sans rien faire est SUR : `drift` a deja ete verifie juste au-dessus,
                // donc le pantin est a moins de 8 m de la verite serveur. La correction de derive
                // reste assuree par la branche du dessous des que cet ecart se creuse.
                static constexpr float kRetargetThresholdMeters = 1.0f;
                const auto known = g_dernieresCibles.find(networkId);
                if (known != g_dernieresCibles.end())
                {
                    const float tx = destination.X - known->second.x;
                    const float ty = destination.Y - known->second.y;
                    const float tz = destination.Z - known->second.z;
                    if (std::sqrt(tx * tx + ty * ty + tz * tz) < kRetargetThresholdMeters)
                    {
                        return; // meme destination : le pantin y va deja, on le laisse marcher
                    }
                }

                bool moving = false;
                if (Red::CallVirtual(this, "MoveNetworkEntityTo", moving, entityId, destination,
                                     static_cast<int32_t>(locomotion))
                    && moving)
                {
                    g_dernieresCibles[networkId] =
                        CibleCommandee{ destination.X, destination.Y, destination.Z };
                    return; // le moteur joue le trajet
                }
                // Echec de la commande (entite pas encore prete, pas un ScriptedPuppet…) : on
                // retombe sur le placement direct plutot que de laisser l'entite sur place.
            }
        }
    }

    SetEntityPosition(entityId, worldPosition, yaw);
}

void NetworkGameSystem::SetEntityPosition(const RED4ext::ent::EntityID entityId, RED4ext::Vector4 worldPosition, float yaw)
{
    const auto entity = Cyberverse::Utils::GetDynamicEntity(entityId);
    // TODO: For most, this is actually a NPCPuppet, for those that aren't, this will crash.

    if (entity.has_value())
    {
        // TODO: Find out if this is an NPCPuppet...
        // AI
        RED4ext::Handle<RED4ext::AICommand> commandRef;
        Red::CallVirtual(this, "TeleportPuppet", commandRef, entity.value(), worldPosition, yaw);
        m_LastTeleportCommand[entityId] = commandRef;

        // random game objects
        RED4ext::EulerAngles angles = { 0.0f, 0.0f, yaw };
        const auto teleportFacility = Red::GetGameSystem<RED4ext::TeleportationFacility>();
        if (!Red::CallVirtual(teleportFacility, "Teleport", entity.value() /*"game object"*/, worldPosition, angles))
        {
            SDK->logger->Warn(PLUGIN, "Failed to teleport");
        }

        // Try: scriptInterface.PushAnimationEvent(n"Jump");
        // if (!Red::CallVirtual(entity.value(), "PushAnimationEvent", "Jump"))
        // {
        //     SDK->logger->Info(PLUGIN, "Could not push jump event.");
        // }
    } else
    {
        SDK->logger->Warn(PLUGIN, "Cannot SetEntityPosition, because the entity hasn't been found");
    }
}


void NetworkGameSystem::PollIncomingMessages()
{
    while (true)
    {
        ISteamNetworkingMessage* pIncomingMsg = nullptr;
        const auto numMsgs = m_pInterface->ReceiveMessagesOnConnection(m_hConnection, &pIncomingMsg, 1);
        if (numMsgs == 0)
        {
            break;
        }
        if (numMsgs < 0)
        {
            SDK->logger->ErrorF(PLUGIN, "Error polling messages: %d", numMsgs);
            return;
        }

        // Un ServerEnvelope FlatBuffers par message. On verifie le buffer avant lecture.
        const auto* bytes = static_cast<const uint8_t*>(pIncomingMsg->GetData());
        const auto size = static_cast<size_t>(pIncomingMsg->GetSize());
        flatbuffers::Verifier verifier(bytes, size);
        if (verifier.VerifyBuffer<cyberpunk_rp::protocol::ServerEnvelope>(nullptr))
        {
            const auto* env = flatbuffers::GetRoot<cyberpunk_rp::protocol::ServerEnvelope>(bytes);
            if (env != nullptr)
            {
                switch (env->msg_type())
                {
                case cyberpunk_rp::protocol::ServerMsg_Snapshot:
                    HandleSnapshot(env->msg_as_Snapshot());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_PositionCorrection:
                    HandlePositionCorrection(env->msg_as_PositionCorrection());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_ShardAssignment:
                    HandleShardAssignment(env->msg_as_ShardAssignment());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_WorldState:
                    HandleWorldState(env->msg_as_WorldState());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_Kicked:
                    HandleKicked(env->msg_as_Kicked());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_AppearanceSync:
                    HandleAppearanceSync(env->msg_as_AppearanceSync());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_ConfigSync:
                    HandleConfigSync(env->msg_as_ConfigSync());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_PlayerEvent:
                    HandlePlayerEvent(env->msg_as_PlayerEvent());
                    break;
                default:
                    // Reste non câblé : CommandResult, PermissionSync, CharacterList,
                    // CharacterResult, QueueStatus, InteractionOpen, InteractionResult,
                    // ElevatorStateMsg. Le serveur les émet déjà — les brancher est le chantier
                    // « autorité totale », étapes 2 et 6. Journalisé au lieu d'être jeté en
                    // silence : un message serveur ignoré sans trace est exactement ce qui a fait
                    // croire pendant des semaines que le protocole n'était pas implémenté.
                    LogUnhandledServerMsg(static_cast<int>(env->msg_type()));
                    break;
                }
            }
        }
        else
        {
            SDK->logger->Warn(PLUGIN, "Paquet serveur invalide (FlatBuffers)");
        }

        pIncomingMsg->Release();
    }
}

void NetworkGameSystem::SendJoin(const std::string& displayName)
{
    if (m_pInterface == nullptr)
    {
        return;
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto name = builder.CreateString(displayName);
    // token : JWT ZITADEL transmis par le launcher via l'environnement du process
    // (TESSERA_JOIN_TOKEN), cf. design launcher-server-auth 2026-07-09 §2.4 et le contrat
    // tessera-core/client-mod/INTEGRATION-server-contract.md. Le launcher pose cette variable au
    // lancement (jamais en ligne de commande : un JWT y serait visible dans la liste des process).
    // Absente (serveur prive identity.public=false, ou lancement hors launcher) => chaine vide,
    // ignoree par un serveur prive ; un serveur public exige un token valide et kicke sinon
    // ("compte requis sur ce serveur"). Ne jamais logger cette valeur (secret).
    const char* tokenEnv = std::getenv("TESSERA_JOIN_TOKEN");
    const auto token = builder.CreateString(tokenEnv != nullptr ? tokenEnv : "");
    // protocol_version : EXPLICITE, jamais laisse au defaut (= 0 => kick). Voir
    // kTesseraProtocolVersion en tete de fichier.
    const auto join = cyberpunk_rp::protocol::CreateJoin(
        builder, name, token, kTesseraProtocolVersion);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_Join, join.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(
        m_hConnection, builder.GetBufferPointer(), builder.GetSize(),
        k_nSteamNetworkingSend_Reliable, nullptr);
}

void NetworkGameSystem::SendPositionUpdate(float x, float y, float z, float yaw)
{
    if (m_pInterface == nullptr)
    {
        return;
    }
    flatbuffers::FlatBufferBuilder builder;
    // Gel palier 2 : position quantifiee QVec3 fixed-point + yaw ushort (cf. QuantPos/QuantYaw en
    // tete de fichier — miroir de quant.rs). frame/slot poses EXPLICITEMENT a 0 (= monde, ADR
    // 0013) : le repere d'un joueur est serveur-autoritaire (pose par EntityInteraction kind=6/7),
    // ce client emet toujours une position monde tant que la conversion offset-local au montage
    // (C4.2) n'est pas cablee.
    // Locomotion et direction de déplacement : LUES sur le joueur, plus posées à 0 en dur.
    // Tant qu'elles valaient 0, tout avatar distant glissait en posture « Idle » quoi que fasse
    // le joueur d'en face — alors que le protocole porte ces champs depuis le gel du palier 2 et
    // que le serveur les relaie déjà tels quels dans PlayerState.
    //
    // Le calcul vit en redscript (`ReadLocomotionPacked`) parce qu'il lit le blackboard
    // PlayerStateMachine, et que la recette exacte y a été MESURÉE en jeu (sonde `loco_read`,
    // 2026-07-23) — la transcrire en C++ serait la ré-inventer.
    //
    // Empaquetage : bits 0-7 = locomotion, bits 8-15 = move_dir (voir la fonction redscript).
    // Un appel qui échoue laisse 0/0 — même valeur qu'avant ce changement, jamais pire.
    int32_t packedLocomotion = 0;
    if (!Red::CallVirtual(this, "ReadLocomotionPacked", packedLocomotion))
    {
        packedLocomotion = 0;
    }
    const auto locomotion = static_cast<uint8_t>(packedLocomotion & 0xFF);
    const auto moveDir = static_cast<uint8_t>((packedLocomotion >> 8) & 0xFF);

    const cyberpunk_rp::protocol::QVec3 pos(QuantPos(x), QuantPos(y), QuantPos(z));
    const auto pu = cyberpunk_rp::protocol::CreatePositionUpdate(
        builder, &pos, QuantYaw(yaw), locomotion, moveDir, /*flags=*/0,
        /*frame=*/0, /*slot=*/0);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_PositionUpdate, pu.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(
        m_hConnection, builder.GetBufferPointer(), builder.GetSize(),
        k_nSteamNetworkingSend_Reliable, nullptr);
}

void NetworkGameSystem::OnSpawnFailure()
{
    ++m_spawnFailureCount;

    // Première occurrence toujours loguée : c'est elle qui date le début du problème.
    if (m_spawnFailureCount == 1)
    {
        SDK->logger->Warn(PLUGIN,
            "Echec spawn avatar reseau — SpawnTransientEntity introuvable. Le modset redscript "
            "n'est probablement PAS compile : voir r6/logs/redscript_rCURRENT.log");
    }

    if (!m_spawnFailureNotified && m_spawnFailureCount >= kSpawnFailureAlertThreshold)
    {
        m_spawnFailureNotified = true;
        NotifyModsetNotCompiled();
    }
}

void NetworkGameSystem::NotifyModsetNotCompiled()
{
    SDK->logger->WarnF(PLUGIN,
        "%llu echecs de spawn consecutifs — alerte affichee au joueur.", m_spawnFailureCount);

    // Thread détaché : MessageBox est bloquant, et on est appelé depuis la boucle de jeu.
    // Volontairement natif Win32 et non une notification in-game : dans ce scénario, redscript
    // n'a pas compilé, donc l'UI kit Tessera n'existe pas non plus.
    std::thread([]() {
        MessageBoxW(nullptr,
            L"Tessera : les autres joueurs ne peuvent pas s'afficher.\n\n"
            L"Le modset redscript n'a pas compile — c'est presque toujours un mod tiers "
            L"installe a la main qui bloque tout le dossier r6\\scripts.\n\n"
            L"Ouvre ce fichier, la fin indique le mod fautif :\n"
            L"    r6\\logs\\redscript_rCURRENT.log\n\n"
            L"La connexion au serveur, elle, fonctionne : les autres TE voient.",
            L"Tessera — modset non compile",
            MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }).detach();
}

void NetworkGameSystem::HandleSnapshot(const cyberpunk_rp::protocol::Snapshot* snapshot)
{
    if (snapshot == nullptr)
    {
        return;
    }

    // Le serveur remplit TROIS tableaux dans un Snapshot (server_loop.rs::encode_snapshot_for) :
    // `players`, `npcs` et `vehicles`. Ce client ne lisait que le premier — les PNJ et vehicules
    // pilotes par le serveur etaient donc encodes, transmis, puis jetes a l'arrivee. Les ids des
    // trois familles sont DISJOINTS par construction (protocol.fbs, NpcState : « id dans une plage
    // reservee disjointe des connexions reelles »), donc une seule table de suivi suffit et le
    // despawn par absence reste correct.
    std::set<uint64_t> present;

    // Applique une entree de snapshot, quelle que soit sa famille : spawn si l'id est inconnu,
    // repositionnement sinon. Factorise pour que les trois tableaux ne divergent pas dans leur
    // traitement — c'est exactement ce genre de duplication qui laisse un tableau en arriere.
    const auto applyPose = [this, &present](uint64_t id, const cyberpunk_rp::protocol::QVec3* pos,
                                            uint16_t quantizedYaw, uint8_t locomotion,
                                            const cyberpunk_rp::protocol::QVec3* moveTarget = nullptr)
    {
        if (pos == nullptr)
        {
            return;
        }
        present.insert(id);

        // Gel palier 2 : dequantization du QVec3 fixed-point + yaw ushort -> degres.
        const RED4ext::Vector4 worldPosition = {
            DequantPos(pos->x()), DequantPos(pos->y()), DequantPos(pos->z()), 1.0f
        };
        const float yaw = DequantYaw(quantizedYaw);

        const auto existing = m_networkedEntitiesLookup.find(id);
        if (existing == m_networkedEntitiesLookup.end())
        {
            SpawnNetworkEntity(id, worldPosition);
        }
        else
        {
            // Destination reelle du PNJ (a plusieurs metres), quand le serveur la connait. Voir
            // SetEntityPose : sans elle le pantin recoit l'ordre d'avancer de 5 cm cinquante fois
            // par seconde et fremit sur place au lieu de marcher.
            RED4ext::Vector4 target{};
            const bool hasTarget = moveTarget != nullptr;
            if (hasTarget)
            {
                target = { DequantPos(moveTarget->x()), DequantPos(moveTarget->y()),
                           DequantPos(moveTarget->z()), 1.0f };
            }
            SetEntityPose(id, existing->second, worldPosition, yaw, locomotion,
                          hasTarget ? &target : nullptr);
        }
    };

    // Le serveur exclut deja le joueur local : `players` = uniquement les autres.
    const auto* players = snapshot->players();
    if (players != nullptr)
    {
        for (const auto* ps : *players)
        {
            if (ps != nullptr)
            {
                applyPose(ps->id(), ps->position(), ps->yaw(), ps->locomotion());
            }
        }
    }

    // PNJ orchestres par le serveur (foule, vendeurs, hostiles — crowd_producer.rs / stub.rs).
    // `archetype` est un id de COMPORTEMENT (npc-catalog.toml), pas une identite visuelle : c'est
    // `AppearanceSync` qui dit quoi faire apparaitre, exactement comme pour un joueur. Tant que le
    // serveur n'en emet pas pour les PNJ, ils sortent au record de repli et le log le signale.
    const auto* npcs = snapshot->npcs();
    if (npcs != nullptr)
    {
        for (const auto* ns : *npcs)
        {
            if (ns != nullptr)
            {
                applyPose(ns->id(), ns->position(), ns->yaw(), ns->locomotion(), ns->move_target());
                // Etat de MORT, une seule fois par entite. Le serveur l'annonce dans `behavior`
                // (`ATerre`), champ qui existait deja sur le fil et que le client ignorait.
                if (ns->behavior() == kComportementATerre && !g_cadavresAppliques.contains(ns->id()))
                {
                    const auto entite = m_networkedEntitiesLookup.find(ns->id());
                    if (entite != m_networkedEntitiesLookup.end())
                    {
                        bool ok = false;
                        const bool appele = Red::CallVirtual(this, "TesseraRendreMort", ok,
                                                             entite->second);
                        if (appele && ok)
                        {
                            g_cadavresAppliques.insert(ns->id());
                            SDK->logger->InfoF(PLUGIN, "Cadavre applique sur %llu", ns->id());
                        }
                        else
                        {
                            // On RÉESSAIE au snapshot suivant (rien n'est inséré dans le set). Une
                            // entite qui vient de naitre n'est pas forcement prete a mourir — c'est
                            // l'hypothese de Lucas, et ce log dit en combien de tentatives on y
                            // arrive, ou si on n'y arrive jamais. Sans lui, « il est debout » ne
                            // distingue pas « jamais tente » de « tente et refuse ».
                            g_essaisCadavre[ns->id()]++;
                            if (g_essaisCadavre[ns->id()] % 20 == 1)
                            {
                                SDK->logger->WarnF(PLUGIN,
                                    "Cadavre REFUSE sur %llu (essai %u, appel=%s)", ns->id(),
                                    g_essaisCadavre[ns->id()], appele ? "ok" : "echec");
                            }
                        }
                    }
                }
            }
        }
    }

    // Vehicules AUTONOMES (trafic). Distincts des vehicules PILOTES (`vehicles_player`), qui
    // relevent du niveau 2 et d'un filet de correction, pas d'un placement direct — les traiter
    // ici les ferait « glisser » (fantome observe en jeu, cf. protocol.fbs VehiclePlayerState).
    const auto* vehicles = snapshot->vehicles();
    if (vehicles != nullptr)
    {
        for (const auto* vs : *vehicles)
        {
            if (vs != nullptr)
            {
                // Les vehicules n'ont PAS le triplet biped (ils portent une `speed`
                // scalaire) : locomotion 0, donc placement direct sans commande de marche.
                applyPose(vs->id(), vs->position(), vs->yaw(), 0);
            }
        }
    }

    // Ids disparus du snapshot -> despawn.
    for (auto it = m_networkedEntitiesLookup.begin(); it != m_networkedEntitiesLookup.end();)
    {
        if (!present.contains(it->first))
        {
            if (!Red::CallVirtual(this, "DestroyTransientEntity", it->second))
            {
                SDK->logger->Warn(PLUGIN, "Echec despawn avatar reseau");
            }
            // Oublier ce qui a ete APPLIQUE, pas ce que le serveur a DIT. `m_appearances` garde
            // l'apparence connue : une entite qui sort puis rerentre dans l'AoI doit pouvoir
            // respawner correctement meme si le serveur ne renvoie pas d'AppearanceSync (il ne le
            // fait que sur changement reel, cf. appearance_relay.rs). En revanche
            // `m_appliedAppearance` decrit une entite de jeu qui vient d'etre detruite : le
            // conserver ferait sauter l'application sur la NOUVELLE entite au respawn.
            m_appliedAppearance.erase(it->first);
            // Meme raison que ci-dessus : la destination commandee decrivait une entite de jeu qui
            // vient d'etre detruite. La garder ferait sauter le premier ordre de marche au respawn.
            g_dernieresCibles.erase(it->first);
            it = m_networkedEntitiesLookup.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void NetworkGameSystem::HandlePositionCorrection(const cyberpunk_rp::protocol::PositionCorrection* correction)
{
    if (correction == nullptr || correction->position() == nullptr)
    {
        return;
    }

    // Gel palier 2 : dequantization du QVec3 fixed-point + yaw ushort -> degres.
    const auto* pos = correction->position();
    const RED4ext::Vector4 worldPosition = {
        DequantPos(pos->x()), DequantPos(pos->y()), DequantPos(pos->z()), 1.0f
    };
    const float yaw = DequantYaw(correction->yaw());
    const uint8_t reason = correction->reason(); // 0=Spawn, 1=AntiCheat, 2=Resync (diagnostic uniquement)

    // Téléporte le JOUEUR LOCAL à la position autoritaire du serveur. Même facilité que pour les
    // avatars distants (confirmée fonctionnelle en jeu, cf. SetEntityPosition) mais appliquée au
    // joueur contrôlé. reason ne change PAS le comportement (spec : téléportation dans tous les cas),
    // il n'est que journalisé pour le diagnostic.
    const auto player = Cyberverse::Utils::GetPlayer();
    if (player == nullptr)
    {
        SDK->logger->Warn(PLUGIN, "PositionCorrection recue mais joueur local introuvable");
        return;
    }

    const RED4ext::EulerAngles angles = { 0.0f, 0.0f, yaw };
    const auto teleportFacility = Red::GetGameSystem<RED4ext::TeleportationFacility>();
    if (!Red::CallVirtual(teleportFacility, "Teleport", player, worldPosition, angles))
    {
        SDK->logger->Warn(PLUGIN, "PositionCorrection : echec de la teleportation du joueur local");
        return;
    }

    // Anti-boucle rubber-band (spec mouvement §4.3) : on saute le PROCHAIN PositionUpdate. Sans ça,
    // le sync suivant (toutes les 0.1 s) enverrait l'ancienne position d'avant téléportation — le
    // serveur la verrait « sauter » et renverrait une nouvelle correction, en boucle. Un seul tick
    // suffit : au sync d'après, GetWorldPosition lit déjà la position corrigée.
    m_skipNextPositionUpdate = true;

    SDK->logger->InfoF(PLUGIN, "PositionCorrection appliquee: reason=%u pos=(%.2f, %.2f, %.2f) yaw=%.1f",
        reason, worldPosition.X, worldPosition.Y, worldPosition.Z, yaw);
}

void NetworkGameSystem::HandleShardAssignment(const cyberpunk_rp::protocol::ShardAssignment* assignment)
{
    if (assignment == nullptr)
    {
        return;
    }

    const auto* authoritative = assignment->authoritative();
    m_serverShard = authoritative != nullptr ? authoritative->str() : std::string();

    // overlaps : vector<Offset<String>> -> CSV pour franchir simplement la couture Lua (le HUD
    // attend une chaîne "group-0,group-2", cf. Tessera_GetServerOverlaps).
    std::string csv;
    const auto* overlaps = assignment->overlaps();
    if (overlaps != nullptr)
    {
        for (const auto* s : *overlaps)
        {
            if (s == nullptr)
            {
                continue;
            }
            if (!csv.empty())
            {
                csv += ",";
            }
            csv += s->str();
        }
    }
    m_serverOverlapsCsv = csv;

    SDK->logger->InfoF(PLUGIN, "ShardAssignment recu: autoritatif=%s overlaps=[%s]",
        m_serverShard.c_str(), m_serverOverlapsCsv.c_str());
}

void NetworkGameSystem::HandleWorldState(const cyberpunk_rp::protocol::WorldState* state)
{
    if (state == nullptr)
    {
        return;
    }

    // Note : `ApplyServerConfig` (redscript) est prêt et MESURÉ — TweakXL réécrit TweakDB à
    // chaud, en cours de partie, et le contrôle positif tient (F-PLF-018, 2026-08-04). Il n'est
    // appelé par personne pour l'instant : il attend son canal `ConfigSync` dans le protocole.
    // La sonde qui a produit cette mesure a été retirée d'ici — elle écrivait une valeur en dur.

    // Meteo AVANT le seuil de resynchronisation de l'heure ci-dessous : ce seuil provoque un
    // `return` anticipe quand l'heure n'a pas assez derive, et la meteo serait alors ignoree
    // pendant des minutes alors qu'elle vient de changer. Les deux vivent dans le meme message
    // mais n'ont pas la meme cadence utile.
    ApplyServerWeather(state);

    // L'heure du monde est une ressource GLOBALE, pas par-shard (world_clock.rs) : tous les
    // joueurs voient la meme heure ou qu'ils soient. C'est le serveur qui la decide.
    //
    // Le SEUIL de resynchronisation vit cote redscript, parce que c'est la qu'on peut lire
    // l'horloge du moteur — le seul referent qui ait du sens. Une version anterieure comparait au
    // dernier ordre SERVEUR appliqué et ne gardait donc rien : le serveur avance de 2 minutes
    // entre deux diffusions et le seuil valait 2 minutes, la condition n'etait jamais vraie.
    //
    // 3 minutes de jeu : assez large pour que l'horloge du moteur suive sans etre corrigee en
    // permanence, assez serre pour qu'aucun joueur ne voie un decalage credible avec les autres.
    const int32_t h = static_cast<int32_t>(state->hour());
    const int32_t m = static_cast<int32_t>(state->minute());
    const int32_t tolerance = 3;

    bool applied = false;
    if (!Red::CallVirtual(this, "ApplyServerTime", applied, h, m, tolerance))
    {
        SDK->logger->Warn(PLUGIN, "WorldState : ApplyServerTime introuvable (module redscript ?)");
        return;
    }
    // `false` = l'heure locale etait deja assez proche, rien a corriger. Ce n'est pas une erreur,
    // et le journaliser a chaque message noierait le log.
    if (applied)
    {
        SDK->logger->InfoF(PLUGIN, "Heure serveur appliquee : %02d:%02d", h, m);
    }
}

void NetworkGameSystem::ApplyServerWeather(const cyberpunk_rp::protocol::WorldState* state)
{
    const auto* weather = state != nullptr ? state->weather() : nullptr;
    if (weather == nullptr || weather->size() == 0)
    {
        return;
    }
    const std::string preset = weather->str();

    // Ne re-demander que sur CHANGEMENT reel. Le serveur diffuse WorldState periodiquement et la
    // meteo bouge rarement : reappeler a chaque message declencherait une transition de 3 s en
    // boucle, donc un ciel qui ne se stabilise jamais.
    if (preset == m_lastAppliedWeather)
    {
        return;
    }

    // ✅ MESURE le 2026-08-04 (F-MND-043, sonde `weather_probe`) : `SetWeather` existe et agit
    // dans les deux sens (intensite de pluie 0 -> 1, puis 1 -> 0). Ce code n'existait pas avant
    // cette mesure : le setter est absent du dump RTTI ET de la classe WeatherSystem des scripts
    // decompiles, donc rien d'autre qu'un test en jeu ne pouvait dire qu'il existait.
    Red::CString redPreset(preset.c_str());
    bool accepted = false;
    if (!Red::CallVirtual(this, "ApplyServerWeather", accepted, redPreset))
    {
        SDK->logger->Warn(PLUGIN, "ApplyServerWeather introuvable — module redscript non compile ?");
        return;
    }

    // `false` = preset DEJA applique (comportement observe, pas suppose). Ce n'est pas une erreur :
    // on memorise quand meme, sinon on redemanderait indefiniment la meteo deja en place.
    m_lastAppliedWeather = preset;
    SDK->logger->InfoF(PLUGIN, "Meteo serveur appliquee : %s (accepte=%s)",
        preset.c_str(), accepted ? "true" : "false");
}

void NetworkGameSystem::HandleKicked(const cyberpunk_rp::protocol::Kicked* kicked)
{
    const auto* reason = kicked != nullptr ? kicked->reason() : nullptr;
    const std::string text = reason != nullptr ? reason->str() : std::string("(aucun motif fourni)");

    // Ce message existait cote serveur depuis le debut et etait jete par le `default:` du
    // dispatch : un joueur refuse (serveur plein, token invalide, ban, version de protocole
    // incompatible) restait coupe SANS AUCUNE explication, ni a l'ecran ni au log.
    SDK->logger->WarnF(PLUGIN, "Refuse par le serveur : %s", text.c_str());

    // Alerte native (Win32) et pas une UI de jeu, pour la meme raison que NotifyModsetNotCompiled :
    // au moment d'un kick, on ne peut rien supposer de l'etat du redscript ni de l'UI kit.
    std::thread([text]()
    {
        const std::string body = "Le serveur a refuse la connexion.\n\nMotif : " + text;
        MessageBoxA(nullptr, body.c_str(), "Tessera - connexion refusee",
            MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }).detach();
}


void NetworkGameSystem::HandleConfigSync(const cyberpunk_rp::protocol::ConfigSync* sync)
{
    if (sync == nullptr || sync->entries() == nullptr)
    {
        return;
    }

    // L'autorite de configuration : le serveur decide ce que valent les choses (prix, degats,
    // portees), et le jeu l'applique EN COURS DE PARTIE — sans redemarrage ni republication de
    // modset. Mesure a l'appui : F-PLF-018, `Price.GoodQualityDrink.value` 4 -> 1337 sur une
    // session deja chargee, avec controle positif.
    uint32_t applied = 0;
    uint32_t refused = 0;
    for (const auto* e : *sync->entries())
    {
        if (e == nullptr || e->flat() == nullptr || e->flat()->size() == 0)
        {
            continue;
        }
        Red::CString flat(e->flat()->c_str());
        bool ok = false;
        if (Red::CallVirtual(this, "ApplyServerConfig", ok, flat, e->value()) && ok)
        {
            ++applied;
        }
        else
        {
            // Un chemin refuse se VOIT. Le serveur ne peut pas verifier qu'un chemin designe un
            // champ et non un record (les deux portent des points, la profondeur TweakDB n'est
            // pas fixe) : c'est ici, avec la base sous la main, que ca se tranche. Sans ce log,
            // une faute de frappe de l'operateur serait parfaitement muette.
            ++refused;
            SDK->logger->WarnF(PLUGIN, "ConfigSync : « %s » refuse par TweakDB",
                e->flat()->c_str());
        }
    }
    SDK->logger->InfoF(PLUGIN, "ConfigSync : %u valeur(s) appliquee(s), %u refusee(s)",
        applied, refused);
}

void NetworkGameSystem::SendPromotionRequest(uint64_t record, uint64_t apparence, float x, float y,
                                             float z, float yaw, bool mort)
{
    if (m_pInterface == nullptr || record == 0)
    {
        return;
    }

    // Etranglement : une seule demande par pantin. Sans ca, le meme figurant serait promu a chaque
    // stimulus qu'il declenche — un joueur qui vide un chargeur en creerait dix, et le serveur
    // paierait dix entites la ou une suffit. La cle est le record ET la position arrondie au metre :
    // deux passants du meme archetype a deux endroits restent deux promotions distinctes.
    const auto cle = std::make_tuple(record, static_cast<int32_t>(x), static_cast<int32_t>(y),
                                     static_cast<int32_t>(z));
    if (!g_promotionsDemandees.insert(cle).second)
    {
        return;
    }

    SDK->logger->InfoF(PLUGIN, "Promotion demandee : record %llu apparence %llu a (%.1f, %.1f, %.1f)%s",
        record, apparence, x, y, z, mort ? " [MORT]" : "");

    flatbuffers::FlatBufferBuilder builder;
    const cyberpunk_rp::protocol::QVec3 position(QuantPos(x), QuantPos(y), QuantPos(z));
    const auto req = cyberpunk_rp::protocol::CreatePromotionRequest(
        builder, record, apparence, &position, QuantYaw(yaw), mort);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_PromotionRequest, req.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
}

void NetworkGameSystem::HandlePlayerEvent(const cyberpunk_rp::protocol::PlayerEvent* event)
{
    if (event == nullptr)
    {
        return;
    }

    // kind : 0=Action, 1=Stim. Un kind INCONNU se journalise et s'ignore — jamais d'interpretation
    // par defaut. Le schema est append-only : un client plus ancien que le serveur DOIT pouvoir
    // recevoir un kind qu'il ne connait pas sans mal se comporter.
    constexpr uint8_t kKindStim = 1;
    if (event->kind() != kKindStim)
    {
        SDK->logger->InfoF(PLUGIN, "PlayerEvent kind=%u ignore (acteur %llu)",
            static_cast<unsigned>(event->kind()), event->actor());
        return;
    }

    // Le coeur de l'ADR 0022 : on rejoue l'EVENEMENT sur la foule locale, on ne replique pas ses
    // consequences. Un message au lieu de mille positions de fuyants — et chaque joueur voit SA
    // foule fuir le meme point au meme instant.
    //
    // `BroadcastStim` a besoin d'un EMETTEUR (un GameObject), pas d'une position : c'est lui qui
    // donne a la foule la direction de fuite. L'avatar du joueur distant est deja spawne chez nous
    // — le serveur a filtre par AoI avant de relayer.
    //
    // S'il ne l'est pas, on n'a PAS de repli acceptable : prendre le joueur local comme emetteur
    // ferait fuir la rue dans la mauvaise direction, ce qui est pire que ne rien faire.
    const auto actorEntity = m_networkedEntitiesLookup.find(event->actor());
    if (actorEntity == m_networkedEntitiesLookup.end())
    {
        SDK->logger->WarnF(PLUGIN, "Stim %u ignore : acteur %llu pas spawne localement",
            static_cast<unsigned>(event->action()), event->actor());
        return;
    }

    // Le rayon voyage en DECIMETRES (entier) et redevient un flottant ici : `BroadcastStim` prend
    // un float, et les portees natives du jeu ne sont pas entieres.
    const float radiusMetres = static_cast<float>(event->param()) / 10.0f;
    bool ok = false;
    if (!Red::CallVirtual(this, "ApplyServerStim", ok, actorEntity->second,
            static_cast<uint32_t>(event->action()), radiusMetres)
        || !ok)
    {
        // Un stimulus refuse se VOIT. Cause attendue : ordinal hors des 67 du catalogue, ou entite
        // sans composant emetteur.
        SDK->logger->WarnF(PLUGIN, "Stim %u refuse (acteur %llu, rayon %.1f m)",
            static_cast<unsigned>(event->action()), event->actor(), radiusMetres);
    }
}

void NetworkGameSystem::SendStimReport(uint8_t nature, float radiusMetres, uint64_t target)
{
    if (m_pInterface == nullptr)
    {
        return;
    }

    // Decimetres : voir le commentaire de `StimReport` dans protocol.fbs. Borne basse a 0 pour ne
    // pas replier un rayon negatif en un ushort enorme (un rayon negatif n'a pas de sens, mais un
    // appelant redscript peut en produire un et le fil ne doit pas mentir).
    //
    // Calcule AVANT l'etranglement : c'est la valeur quantifiee, pas le flottant d'origine, qui
    // sert de cle — sinon deux rayons qui se confondent sur le fil compteraient pour deux.
    const float clamped = radiusMetres > 0.0f ? radiusMetres : 0.0f;
    const auto decimetres = static_cast<uint16_t>(
        clamped * 10.0f > 65535.0f ? 65535.0f : clamped * 10.0f);

    // Etranglement par (TYPE, RAYON), pas global : un coup de feu ne doit faire taire ni un cri
    // simultane, ni sa propre portee visuelle (F-PNJ-113).
    const auto cle = std::make_pair(nature, decimetres);
    const auto maintenant = std::chrono::steady_clock::now();
    const auto precedent = g_derniersStims.find(cle);
    if (precedent != g_derniersStims.end() && maintenant - precedent->second < kIntervalleStimMin)
    {
        return;
    }
    g_derniersStims[cle] = maintenant;

    // Journalise CE QUI PART, pour que le silence du serveur soit interpretable : sans cette ligne,
    // « rien dans les logs serveur » ne distingue pas « le hook n'a pas tire » de « le message s'est
    // perdu ». Deux causes tres differentes, meme symptome.
    SDK->logger->InfoF(PLUGIN, "Stim %u envoye (rayon %.1f m, cible %llu)",
        static_cast<unsigned>(nature), clamped, target);

    flatbuffers::FlatBufferBuilder builder;
    const auto stim = cyberpunk_rp::protocol::CreateStimReport(builder, nature, decimetres, target);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_StimReport, stim.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
}

void NetworkGameSystem::HandleAppearanceSync(const cyberpunk_rp::protocol::AppearanceSync* sync)
{
    if (sync == nullptr || sync->spec() == nullptr)
    {
        return;
    }

    const uint64_t id = sync->id();
    NetworkAppearance appearance;
    appearance.baseRecord = sync->spec()->base_record();
    appearance.appearance = sync->spec()->appearance();
    m_appearances[id] = appearance;

    SDK->logger->InfoF(PLUGIN, "AppearanceSync %llu : record=%llu apparence=%llu",
        id, appearance.baseRecord, appearance.appearance);

    // L'apparence arrive AVANT le premier Snapshot qui porte l'entite (le serveur la pousse a
    // l'entree en AoI) — dans ce cas il n'y a rien a appliquer, le spawn s'en servira. Mais elle
    // peut aussi CHANGER en cours de session (tenue, degainage) : l'entite est alors deja la.
    const auto existing = m_networkedEntitiesLookup.find(id);
    if (existing != m_networkedEntitiesLookup.end())
    {
        ApplyAppearance(id, existing->second);
    }
}

void NetworkGameSystem::ApplyAppearance(uint64_t networkId, RED4ext::ent::EntityID entityId)
{
    const auto it = m_appearances.find(networkId);
    if (it == m_appearances.end() || it->second.appearance == 0)
    {
        return;
    }

    // Ne re-appliquer que sur changement REEL. `ScheduleAppearanceChange` a un effet DIFFERE et
    // relire l'apparence immediatement apres renvoie encore l'ancienne (F-PNJ-050) : sans ce
    // garde, on la reprogrammerait a chaque message sans jamais pouvoir constater qu'elle a pris.
    const auto applied = m_appliedAppearance.find(networkId);
    if (applied != m_appliedAppearance.end() && applied->second == it->second.appearance)
    {
        return;
    }

    const auto entity = Cyberverse::Utils::GetDynamicEntity(entityId);
    if (!entity.has_value())
    {
        return;
    }

    const RED4ext::CName appearanceName(it->second.appearance);
    if (!Red::CallVirtual(entity.value(), "ScheduleAppearanceChange", appearanceName))
    {
        SDK->logger->WarnF(PLUGIN, "ScheduleAppearanceChange refuse pour %llu", networkId);
        return;
    }
    m_appliedAppearance[networkId] = it->second.appearance;
}

bool NetworkGameSystem::SpawnNetworkEntity(uint64_t networkId, const RED4ext::Vector4& worldPosition)
{
    // Le record vient du SERVEUR (AppearanceSync). Le repli n'est utilise que si aucune apparence
    // n'est encore connue pour cet id — et il se signale, parce qu'un avatar de repli silencieux
    // est indistinguable d'un avatar correct.
    RED4ext::TweakDBID record;
    RED4ext::CName appearanceName(static_cast<uint64_t>(0));
    const auto it = m_appearances.find(networkId);
    if (it != m_appearances.end() && it->second.baseRecord != 0)
    {
        record = RED4ext::TweakDBID(it->second.baseRecord);
        appearanceName = RED4ext::CName(it->second.appearance);
    }
    else
    {
        record = RED4ext::TweakDBID(kFallbackAvatarRecord);
        SDK->logger->WarnF(PLUGIN,
            "Spawn %llu SANS apparence serveur — repli %s. Le serveur n'a pas (encore) envoye "
            "d'AppearanceSync pour cette entite.", networkId, kFallbackAvatarRecord);
    }

    const RED4ext::Quaternion worldOrientation = { 0.0f, 0.0f, 0.0f, 1.0f };
    RED4ext::ent::EntityID entityId;
    if (!Red::CallVirtual(this, "SpawnNetworkAvatar", entityId, record, appearanceName,
                          worldPosition, worldOrientation))
    {
        OnSpawnFailure();
        return false;
    }

    m_networkedEntitiesLookup.insert(std::make_pair(networkId, entityId));
    if (appearanceName.hash != 0)
    {
        // Posee au spawn par le spec : on l'enregistre comme appliquee pour ne pas la
        // reprogrammer inutilement au premier AppearanceSync suivant.
        m_appliedAppearance[networkId] = appearanceName.hash;
    }
    SDK->logger->InfoF(PLUGIN, "Spawn entite reseau %llu -> entity %llu", networkId, entityId.hash);
    return true;
}

bool NetworkGameSystem::OnGameRestored()
{
    const auto res = IGameSystem::OnGameRestored();
    SDK->logger->Info(PLUGIN, "Game restored: We're in the world");
    const auto player = Cyberverse::Utils::GetPlayer();

    // Broken attempts:
    // const auto transform = Cyberverse::Utils::Entity_GetWorldTransform(player);
    // const auto position = Cyberverse::Utils::WorldPosition_ToVector4(transform.Position);
    // SDK->logger->InfoF(PLUGIN, "Player at (%f, %f, %f)", transform.Position.x.Bits, transform.Position.y.Bits,
    // transform.Position.z.Bits);

    const auto position = Cyberverse::Utils::Entity_GetWorldPosition(player);
    SDK->logger->InfoF(PLUGIN, "Player at (%f, %f, %f, %f)", position.X, position.Y, position.Z, position.W);

    // Couture TesseraSynth : le Join part a la connexion GNS (ConnectionStatusChangedCallback),
    // pas ici. L'ancien PlayerJoinWorld (zpp) n'est pas compris par notre serveur FlatBuffers.

    m_gameRestored = true;
    return res;
}

void NetworkGameSystem::TrackPlayerPosition(float deltaTime)
{
    m_TimeSinceLastPlayerPositionSync += deltaTime;
    if (m_TimeSinceLastPlayerPositionSync < 0.1f /* update rate*/)
    {
        return;
    }

    m_TimeSinceLastPlayerPositionSync = 0.0f;

    // Anti-boucle rubber-band : une PositionCorrection vient d'être appliquée ; on saute CE seul
    // envoi pour laisser le moteur poser la téléportation avant de re-mesurer/ré-émettre la
    // position (sinon on renverrait l'ancienne, redéclenchant une correction — cf.
    // HandlePositionCorrection).
    if (m_skipNextPositionUpdate)
    {
        m_skipNextPositionUpdate = false;
        return;
    }

    const auto player = Cyberverse::Utils::GetPlayer();
    const auto [X, Y, Z, W] = Cyberverse::Utils::Entity_GetWorldPosition(player);
    const auto orientation = Cyberverse::Utils::Entity_GetWorldOrientation(player);
    const auto [Roll, Pitch, Yaw] = Cyberverse::Utils::Quaternion_ToEulerAngles(orientation);

    this->SendPositionUpdate(X, Y, Z, Yaw);
}

void NetworkGameSystem::InterpolatePuppets(const float deltaTime)
{
    for (auto it = m_interpolationData.begin(); it != m_interpolationData.end();)
    {
        auto& entityId = it->first;
        auto& interpolator = it->second;

        const auto interpolationProgress = std::min(1.0f, interpolator.CalcInterpolationProgress(deltaTime));
        const auto targetDestination = Cyberverse::Utils::LerpLocal(interpolator.positionSource, interpolator.positionTarget, interpolationProgress);

        auto angleDirection1 = interpolator.rotationTarget - interpolator.rotationSource;
        auto angleDirection2 = interpolator.rotationSource - interpolator.rotationTarget;

        if (angleDirection1 < 0.0f)
        {
            angleDirection1 += 360.0f;
        }

        if (angleDirection2 < 0.0f)
        {
            angleDirection2 += 360.0f;
        }

        auto actualAngleDistance = 0.0f;
        if (angleDirection1 < angleDirection2)
        {
            actualAngleDistance = angleDirection1;
        } else
        {
            actualAngleDistance = -angleDirection2;
        }

        // TODO: Better interpolation here, e.g. if we go from 5 -> 355°, we should only do 10°, not 350.
        const float targetYaw = interpolator.rotationSource + interpolationProgress * actualAngleDistance;
        const auto targetDestinationVec4 = Cyberverse::Utils::Vector3To4(targetDestination); // TODO: Get rid of this function call
        SDK->logger->TraceF(PLUGIN, "Interpolation Progress: %f, yaw: %f. dest (%f, %f, %f)", interpolationProgress, targetYaw, targetDestinationVec4.X, targetDestinationVec4.Y, targetDestinationVec4.Z);

        SetEntityPosition(entityId, targetDestinationVec4, targetYaw);

        if (interpolationProgress >= 1.0f)
        {
            it = m_interpolationData.erase(it);
        } else {
            ++it;
        }
    }
}

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
        char buf[255];
        DWORD buf_len = 255;
        GetUserNameA(buf, &buf_len);

        SDK->logger->Info(PLUGIN, "Socket connected, sending Join (TesseraSynth)");
        auto* system = Red::GetGameSystem<NetworkGameSystem>();
        system->SendJoin(std::string(buf));
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
                default:
                    // Reste non câblé : CommandResult, PermissionSync, PlayerEvent, CharacterList,
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
                                            uint16_t quantizedYaw)
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
            // Id connu -> teleporte a la nouvelle pose (interpolation a ajouter plus tard).
            SetEntityPosition(existing->second, worldPosition, yaw);
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
                applyPose(ps->id(), ps->position(), ps->yaw());
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
                applyPose(ns->id(), ns->position(), ns->yaw());
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
                applyPose(vs->id(), vs->position(), vs->yaw());
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

    // Meteo AVANT le seuil de resynchronisation de l'heure ci-dessous : ce seuil provoque un
    // `return` anticipe quand l'heure n'a pas assez derive, et la meteo serait alors ignoree
    // pendant des minutes alors qu'elle vient de changer. Les deux vivent dans le meme message
    // mais n'ont pas la meme cadence utile.
    ApplyServerWeather(state);

    // L'heure du monde est une ressource GLOBALE, pas par-shard (world_clock.rs) : tous les
    // joueurs voient la meme heure ou qu'ils soient. C'est le serveur qui la decide.
    const int32_t serverMinutes =
        static_cast<int32_t>(state->hour()) * 60 + static_cast<int32_t>(state->minute());

    // Le moteur fait avancer le temps localement entre deux WorldState. Reecrire l'heure a chaque
    // message la ferait donc sauter en arriere en permanence (visible : le cycle jour/nuit
    // saccade). On ne corrige que sur ecart reel — le serveur reste la reference, le moteur
    // interpole entre deux corrections. Seuil en minutes de JEU.
    static constexpr int32_t kResyncThresholdMinutes = 2;
    if (m_lastAppliedWorldMinutes >= 0)
    {
        int32_t delta = serverMinutes - m_lastAppliedWorldMinutes;
        // Distance circulaire sur 24 h : 23h59 -> 00h01 vaut 2 minutes, pas 1438.
        if (delta > 720) { delta -= 1440; }
        if (delta < -720) { delta += 1440; }
        if (delta < 0) { delta = -delta; }
        if (delta < kResyncThresholdMinutes)
        {
            return;
        }
    }

    const int32_t h = static_cast<int32_t>(state->hour());
    const int32_t m = static_cast<int32_t>(state->minute());

    // Passe par REDSCRIPT, pas par `Red::CallStatic("ScriptGameInstance", "GetTimeSystem", …)`.
    // Cette dernière forme ne résolvait JAMAIS le natif — « TimeSystem introuvable » à chaque
    // message, mesuré en jeu le 2026-08-04, pendant que la météo (elle, passée par redscript)
    // marchait du premier coup. Même besoin, deux voies, une seule qui résout : on garde celle
    // dont l'effet est prouvé.
    bool applied = false;
    if (!Red::CallVirtual(this, "ApplyServerTime", applied, h, m) || !applied)
    {
        SDK->logger->Warn(PLUGIN, "WorldState : heure serveur non appliquee (TimeSystem absent ?)");
        return;
    }

    m_lastAppliedWorldMinutes = serverMinutes;
    SDK->logger->InfoF(PLUGIN, "Heure serveur appliquee : %02d:%02d", h, m);

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

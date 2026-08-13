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
#include <deque>   // file des rapports de statiques, drainee a cadence limitee
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
// Record de repli quand le serveur n'a pas (encore) dit a quoi ressemble une entite reseau.
//
// ⚠️ C'ETAIT `Character.Panam`, ET C'EST CE QUI RENDAIT LE PVP IMPOSSIBLE. Panam est une compagne
// de QUETE : son record porte le tag TweakDB `Invulnerable`, et `NPCManager::SetNPCImmortalityMode`
// (npcManager.script:123-135) lit ces tags a l'attachement pour poser un god mode. Tout avatar ne
// sous ce repli etait donc litteralement indestructible — mesure le 2026-08-09 par la sonde
// `sonde_cible` : `recordID Character.Panam`, `GodMode Invulnerable true`, attitude `AIA_Friendly`,
// groupe d'attitude `panam`. Aucune balle ne pouvait l'atteindre, quelle que soit la visee.
//
// Un repli doit etre le personnage le plus BANAL possible : un citoyen d'ambiance, sans tag de
// quete, sans invulnerabilite, sans attitude amicale imposee. `Character.CitizenBikerMale` est dans
// le catalogue d'avatars joueur du serveur (`puppet_catalog.rs`), donc valide et deja servi.
//
// ⚠️ Ceci ne corrige que la CONSEQUENCE. La cause racine est l'ORDRE : l'`AppearanceSync` arrive
// apres le spawn (~0,9 s mesure), donc l'avatar nait toujours sous le repli, et
// `ScheduleAppearanceChange` ne change ensuite que la VARIANTE visuelle — jamais le record, donc
// jamais les tags. Tant que l'ordre n'est pas corrige, ce repli n'est pas un cas rare : c'est le
// cas NORMAL.
static constexpr const char* kFallbackAvatarRecord = "Character.CitizenBikerMale";

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
// Cadence du rapport d'heure locale au serveur (diagnostic de derive). 5 s : assez rare pour
// peser zero sur le fil (3 octets utiles), assez frequent pour dater une derive a la minute de
// jeu pres — l'horloge du jeu tourne ~60x le temps reel (F-PNJ-094), 5 s reelles valent donc
// ~5 minutes de jeu.
static constexpr float kPeriodeRapportHeureSecondes = 5.0f;

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
    DrainerRapportsStatiques();
    HydraterApparencesDiscretement();
    ReparerRoster();
    NettoyerRemplacants(frame_info.deltaTime);

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
    RendreAvatarsDistants(frame_info.deltaTime);

    // Rapport d'heure locale — l'autre moitie de l'horloge partagee. Le serveur DECIDE l'heure
    // (`WorldState`, descendant) ; ce rapport lui dit ce que le client affiche VRAIMENT, pour
    // qu'une desynchronisation se voie dans le log au lieu de se deviner sur un ecran.
    m_tempsDepuisRapportHeure += frame_info.deltaTime;
    if (m_tempsDepuisRapportHeure >= kPeriodeRapportHeureSecondes)
    {
        m_tempsDepuisRapportHeure = 0.0f;
        SendClientTimeReport();
    }

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
        auto* systeme = Red::GetGameSystem<NetworkGameSystem>();
        systeme->FullyConnected = false;
        // ⚠️ Nos remplaçants ne survivent PAS à la session qui les a créés. Sans ça, une
        // déconnexion (serveur coupé, réseau perdu, retour au menu) laisse des PNJ fabriqués par
        // nous debout dans un monde qui n'a plus d'autorité pour en parler — et la reconnexion en
        // recrée par-dessus.
        systeme->DetruireTousLesRemplacants("deconnexion");
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

/// Statiques deja rapportes au serveur — un par entite et par session.
std::set<uint64_t> g_statiquesRapportes;

/// File d'attente des rapports de statiques, drainee A CADENCE LIMITEE.
///
/// ⚠️ NECESSAIRE, PAS UN CONFORT. Les statiques s'attachent par RAFALES au chargement d'un secteur :
/// mesure du 2026-08-08, ~180 pantins classes en quelques secondes, soit des pics de 32-33 rapports
/// par seconde. Le Gateway plafonne chaque client a 40 messages/s TOUTES familles confondues
/// (rate_limit.rs) — les rapports passaient donc au-dessus du plafond et etaient JETES :
/// « message ignore (rate-limit) », 80 statiques arbitres sur 204 observes.
///
/// Etaler coute quelques secondes de convergence et ne perd rien. Relever le plafond serveur
/// aurait desarme une protection contre le vrai flood pour un besoin qui n'est pas urgent.
struct RapportStatique
{
    uint64_t entityId;
    uint64_t record;
    uint64_t apparence;
    float x, y, z;
    // Oriente le remplacant qu'un client a qui ce PNJ MANQUE fabriquera a sa place (spec
    // 2026-08-09). Sans lui, le remplacant regarderait ailleurs.
    int16_t yaw;
};
std::deque<RapportStatique> g_fileStatiques;
std::chrono::steady_clock::time_point g_dernierEnvoiStatique{};
/// 5 par seconde : trois ordres de grandeur sous le plafond, et 200 statiques convergent en 40 s —
/// bien avant qu'un joueur ait traverse le quartier.
constexpr auto kIntervalleStatique = std::chrono::milliseconds(200);
// `g_apparencesStatiques` est definie APRES cet espace anonyme : l'en-tete la declare `extern`
// parce que l'accesseur RTTI `Tessera_ApparenceStatiqueConnue` y est inline.

/// Entites reseau deja mises a l'etat MORT chez nous.
///
/// Le comportement voyage dans CHAQUE snapshot : sans memoire, on rejouerait `Kill` vingt fois par
/// seconde sur le meme cadavre. Le set retient ce qui est deja fait.
std::set<uint64_t> g_cadavresAppliques;
/// Nombre de tentatives par entite — sert uniquement au diagnostic (voir le log associe).
std::map<uint64_t, uint32_t> g_essaisCadavre;
constexpr uint8_t kComportementATerre = 5;
} // namespace

/// Apparence faisant autorite, telle que le serveur l'a dite, par EntityID de statique.
/// Conservee meme quand l'application echoue : l'entite peut n'etre pas encore streamee, et c'est
/// cette table qui permet de rejouer l'apparence a son attachement.
std::map<uint64_t, uint64_t> g_apparencesStatiques;
/// Le ROSTER : de quoi RECREER un statique chez un client a qui il manque (spec 2026-08-09).
/// Distinct de `g_apparencesStatiques`, qui ne sert qu'a corriger un PNJ deja present.
std::map<uint64_t, InscriptionRoster> g_rosterStatiques;
/// Nos remplacants : id du PNJ natif absent -> id de l'entite LOCALE qu'on a creee a sa place.
/// C'est la seule chose qu'on ait le droit de detruire — un natif ne se retire pas (F-PNJ-091).
std::map<uint64_t, RED4ext::ent::EntityID> g_remplacants;
std::set<uint64_t> g_dejaVus;
/// Depuis quand un id reseau est absent des snapshots — l'echeance du delai de grace avant
/// despawn. Vide en regime nominal : une entree n'y vit que le temps d'une absence.
std::map<uint64_t, std::chrono::steady_clock::time_point> g_absentsDepuis;

// ── COMPTEURS DE SANTÉ DU ROSTER ────────────────────────────────────────────────────────────
//
// Ils existent pour qu'une régression SE VOIE sans avoir à la reproduire. Trois fois en deux jours,
// un chemin sans instrument n'a pas su distinguer « vide » de « ne s'exécute pas » (F-PNJ-149) ;
// et la duplication signalée le 2026-08-13 a demandé une session entière d'archéologie de journaux
// parce que rien ne comptait les refus.
//
// Un ratio « créés / refusés » qui bascule est le signal qu'une des gardes a cessé de mordre.
StatsRoster g_statsRoster;
// --- Rendu des avatars JOUEURS : l'etat, hors de la classe (cf. NetworkGameSystem.h) ---
Tessera::Sync::HorlogeRendu g_horlogeRendu;
std::map<uint64_t, Tessera::Sync::TamponPose> g_tamponsJoueurs;
std::map<uint64_t, SuiviAvatar> g_suiviAvatars;
std::set<std::pair<int32_t, int32_t>> g_cellulesRecues;
/// Sonde d'apparence — premiere apparence vue par record, et garde one-shot. Une sonde qui
/// rhabillerait toute la rue changerait la scene observee et rendrait le resultat inexploitable.
std::set<uint64_t> g_apparencesAppliquees;
/// Tentatives refusees par le cone de vision (« pas maintenant »). Voir HydraterApparencesDiscretement.
std::size_t g_hydratationsDifferees = 0;
/// Depuis quand chaque apparence est connue — sert l'echeance de convergence (kDelaiForcage).
std::map<uint64_t, std::chrono::steady_clock::time_point> g_apparenceConnueDepuis;
std::map<uint64_t, uint64_t> g_premiereApparence;
bool g_sondeApparenceFaite = false;

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

void NetworkGameSystem::PlacerSansCommande(const RED4ext::ent::EntityID entityId,
                                           RED4ext::Vector4 worldPosition, float yaw)
{
    // ── PLACEMENT PUR : AUCUNE COMMANDE D'IA ───────────────────────────────────────────────
    //
    // `SetEntityPosition` fait DEUX choses : il envoie un `AITeleportCommand` au contrôleur d'IA,
    // PUIS il place l'entité. La première est ce qui rendait la résorption douce non seulement
    // inutile mais NUISIBLE : en l'appelant à chaque frame, on empilait une commande de téléport
    // par frame dans la file du contrôleur, et cette file est la MÊME que celle de la commande de
    // marche. On annulait donc, soixante fois par seconde, l'ordre qui faisait marcher l'avatar.
    //
    // Ce que ça produit, et c'est exactement ce que Lucas décrit le 2026-08-13 : « le personnage
    // reste statique sans animation avant de se déplacer », « ça crée du flou », « les animations
    // ont du mal à se lancer ». Et la mesure le confirmait sans que je la lise : la dérive
    // journalisée MONTAIT sans jamais redescendre — 6,61 puis 7,14 puis 7,86 puis 8,95 m — alors
    // qu'une correction de 15 % par frame aurait dû la résorber en moins de 100 ms. Elle ne
    // corrigeait rien parce qu'elle cassait le mécanisme qu'elle était censée aider.
    //
    // Ici : uniquement `TeleportationFacility`. L'entité bouge, la file de commandes n'est pas
    // touchée, la marche continue. C'est ce que Q6b avait mesuré comme viable — un Teleport
    // PENDANT une commande de marche active, pas un Teleport QUI REMPLACE la commande.
    const auto entity = Cyberverse::Utils::GetDynamicEntity(entityId);
    if (!entity.has_value())
    {
        return;
    }
    const RED4ext::EulerAngles angles = { 0.0f, 0.0f, yaw };
    const auto teleportFacility = Red::GetGameSystem<RED4ext::TeleportationFacility>();
    Red::CallVirtual(teleportFacility, "Teleport", entity.value(), worldPosition, angles);
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
                case cyberpunk_rp::protocol::ServerMsg_StaticAppearance:
                    HandleStaticAppearance(env->msg_as_StaticAppearance());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_CellAppearances:
                    HandleCellAppearances(env->msg_as_CellAppearances());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_HealthSync:
                    HandleHealthSync(env->msg_as_HealthSync());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_CharacterList:
                    HandleCharacterList(env->msg_as_CharacterList());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_CharacterResult:
                    HandleCharacterResult(env->msg_as_CharacterResult());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_ActionCatalog:
                    HandleActionCatalog(env->msg_as_ActionCatalog());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_IdentitesConnues:
                    HandleIdentitesConnues(env->msg_as_IdentitesConnues());
                    break;
                default:
                    // Reste non câblé : CommandResult, PermissionSync,
                    // QueueStatus, InteractionOpen, InteractionResult,
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
    // ── LE SEUL FLUX QUI PART EN NON-FIABLE, ET C'EST DÉLIBÉRÉ ─────────────────────────────
    //
    // Dette relevée à l'audit du **2026-07-02** et jamais payée : « envois client toujours en
    // Reliable alors que le serveur est déjà en unreliable pour les snapshots — il manque le
    // pendant client ».
    //
    // Pourquoi ça coûte cher. Un canal fiable garantit l'ORDRE : un paquet perdu RETIENT tous les
    // suivants jusqu'à sa retransmission. Le serveur voit donc le joueur FIGÉ pendant un RTT, puis
    // reçoit d'un coup une rafale de positions périmées dont il ne garde que la dernière. Vu d'en
    // face : un gel, puis un saut. C'est le pire des deux mondes pour une donnée dont seule la
    // PLUS RÉCENTE compte.
    //
    // Le protocole énonce déjà la règle pour `VehicleInput` : « canal NON-FIABLE — un paquet d'état
    // périmé se jette, ne s'attend pas ». Une position est exactement de cette famille.
    //
    // `NoNagle` en plus : Nagle regroupe les petits paquets pour économiser des en-têtes, au prix
    // de quelques millisecondes d'attente. Sur un flux de position à cadence fixe, ces
    // millisecondes sont de la latence pure et l'économie est nulle — les paquets partent déjà
    // espacés.
    //
    // ⚠️ CE QUE ÇA INTRODUIT, ET POURQUOI ON L'ACCEPTE. Le non-fiable autorise le DÉSORDRE : un
    // paquet en retard peut appliquer côté serveur une position plus ancienne que la courante. Le
    // dégât est borné à UNE période d'envoi et corrigé par le paquet suivant. Surtout il est
    // INVISIBLE en aval : les autres clients rendent avec un tampon d'interpolation qui lisse
    // précisément ce transitoire, et qui rejette déjà les ticks périmés (`TamponPose::Pousser`).
    // Un gel d'un RTT, lui, ne se lisse pas.
    //
    // Un numéro de séquence sur `PositionUpdate` rendrait le rejet explicite côté serveur — c'est
    // ce que le schéma a fait pour `VehicleInput` (« `tick` est OBLIGATOIRE et c'est le champ le
    // plus important »). Il n'est pas ajouté ici : ce serait toucher un protocole gelé pour un
    // défaut dont on n'a pas mesuré qu'il se voit. À faire si une mesure le montre.
    m_pInterface->SendMessageToConnection(
        m_hConnection, builder.GetBufferPointer(), builder.GetSize(),
        k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
}

void NetworkGameSystem::SendClientTimeReport()
{
    if (m_pInterface == nullptr || !FullyConnected)
    {
        return;
    }

    // La LECTURE de l'heure vit en redscript, exactement pour la meme raison que son ECRITURE
    // (`ApplyServerTime`) : `GetTimeSystem` appele depuis le plugin n'etait JAMAIS resolu —
    // « TimeSystem introuvable » a chaque message, mesure en jeu le 2026-08-04. On garde la voie
    // prouvee des deux cotes plutot que d'en entretenir deux.
    int32_t secondesLocales = -1;
    if (!Red::CallVirtual(this, "ReadLocalGameSeconds", secondesLocales))
    {
        if (!m_avertiLectureHeureIntrouvable)
        {
            m_avertiLectureHeureIntrouvable = true;
            SDK->logger->Warn(PLUGIN,
                "ReadLocalGameSeconds introuvable (module redscript non compile ?) — aucune "
                "mesure de derive d'horloge cette session. Message affiche une seule fois.");
        }
        return;
    }

    // Hors bornes = le redscript n'a pas pu lire l'horloge (pas encore en partie, menu principal).
    // On se TAIT plutot que de rapporter une heure inventee : un rapport faux ferait crier le
    // diagnostic serveur, et un diagnostic qui crie a tort est pire que pas de diagnostic.
    if (secondesLocales < 0 || secondesLocales >= 86400)
    {
        return;
    }

    const auto total = static_cast<uint32_t>(secondesLocales);
    flatbuffers::FlatBufferBuilder builder;
    const auto report = cyberpunk_rp::protocol::CreateClientTimeReport(
        builder,
        static_cast<uint8_t>(total / 3600),
        static_cast<uint8_t>((total / 60) % 60),
        static_cast<uint8_t>(total % 60));
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_ClientTimeReport, report.Union());
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
    //
    // ── LES JOUEURS NE SONT PLUS PLACES ICI ────────────────────────────────────────────────
    //
    // Ils sont RANGES dans un tampon, et rendus a chaque frame avec 100 ms de retard
    // (`RendreAvatarsDistants`). Les appliquer a l'arrivee revenait a traiter chaque snapshot
    // comme s'il decrivait le present : un snapshot en retard de 20 ms produisait un a-coup, un
    // snapshot perdu un trou. C'est le mode de panne le plus banal du multijoueur, et il etait ici
    // a l'etat pur — le tampon herite du fork n'ayant jamais ete alimente.
    //
    // `snapshot->tick()` existe sur le fil depuis le gel du palier 2 et n'etait lu NULLE PART.
    // C'est la donnee dont depend tout le rendu lisse.
    g_horlogeRendu.ObserverSnapshot(snapshot->tick());
    const auto* players = snapshot->players();
    if (players != nullptr)
    {
        for (const auto* ps : *players)
        {
            if (ps == nullptr || ps->position() == nullptr)
            {
                continue;
            }
            present.insert(ps->id());

            const RED4ext::Vector4 positionMonde = {
                DequantPos(ps->position()->x()), DequantPos(ps->position()->y()),
                DequantPos(ps->position()->z()), 1.0f
            };
            // Premiere vue : il faut bien un corps avant d'avoir quoi que ce soit a animer. On le
            // fait naitre a la position brute — le tampon n'a pas encore deux echantillons, donc
            // rien a interpoler.
            if (m_networkedEntitiesLookup.find(ps->id()) == m_networkedEntitiesLookup.end())
            {
                SpawnNetworkEntity(ps->id(), positionMonde);
            }

            Tessera::Sync::Pose pose;
            pose.x = positionMonde.X;
            pose.y = positionMonde.Y;
            pose.z = positionMonde.Z;
            pose.yaw = DequantYaw(ps->yaw());
            pose.locomotion = ps->locomotion();
            // `move_dir` voyage depuis le gel du palier 2 et n'etait lu nulle part : un joueur qui
            // marche en crabe ou a reculons etait rendu de face. On le RANGE des maintenant ; ce
            // qu'on saura en faire depend du backlog Q7.
            pose.moveDir = ps->move_dir();
            g_tamponsJoueurs[ps->id()].Pousser(snapshot->tick(), pose);
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

    // Ids disparus du snapshot -> despawn, APRÈS UN DÉLAI DE GRÂCE.
    //
    // ── POURQUOI ON N'EFFACE PLUS TOUT DE SUITE ────────────────────────────────────────────
    //
    // Signalé par Lucas le 2026-08-13 : quand plusieurs instances chargent, il y a des
    // micro-coupures. Côté serveur, une déconnexion retire le joueur du monde IMMÉDIATEMENT
    // (`world.remove_player`) : le snapshot suivant ne le contient plus, et on détruisait son
    // avatar dans la foulée. Une coupure d'une seconde produisait donc une disparition franche
    // suivie d'une réapparition — le pire des deux, parce qu'un avatar qui clignote est plus
    // troublant qu'un avatar figé.
    //
    // On garde donc le corps quelques secondes. Il ne bouge pas — `PiloterAvatar` le fige dès que
    // son fil se tait depuis 400 ms — et il repart tout seul si le joueur revient sous le même id.
    //
    // ⚠️ POURQUOI CE DÉLAI VIT ICI ET PAS SUR LE SERVEUR. Le faire côté serveur serait plus propre
    // en apparence, et c'est un piège : à la reconnexion, GNS attribue un NOUVEL identifiant de
    // connexion. Retenir l'ancien pendant la grâce ferait coexister deux entrées pour la même
    // personne — un jumeau fantôme, exactement la classe de défaut qu'on vient de corriger sur les
    // statiques. Un délai de grâce serveur exige une continuité d'IDENTITÉ entre deux connexions,
    // qui n'est pas mesurée ici. Côté client, la question ne se pose pas : si l'id change, l'ancien
    // avatar s'éteint à l'échéance et le nouveau naît — le comportement d'avant, sans le
    // clignotement pour les coupures qui ne changent pas l'id.
    //
    // 3 s : au-dessus d'une micro-coupure, bien en dessous d'une absence réelle. Un joueur qui sort
    // de l'AoI en marchant met plus longtemps que ça à revenir.
    static constexpr auto kGraceDisparitionS = std::chrono::seconds(3);
    const auto maintenant = std::chrono::steady_clock::now();
    for (const auto& [id, _] : m_networkedEntitiesLookup)
    {
        if (present.contains(id))
        {
            g_absentsDepuis.erase(id); // revenu (ou jamais parti) : l'échéance est annulée.
        }
        else
        {
            g_absentsDepuis.try_emplace(id, maintenant);
        }
    }

    for (auto it = m_networkedEntitiesLookup.begin(); it != m_networkedEntitiesLookup.end();)
    {
        const auto absent = g_absentsDepuis.find(it->first);
        const bool echu = absent != g_absentsDepuis.end()
            && maintenant - absent->second >= kGraceDisparitionS;
        if (echu)
        {
            g_absentsDepuis.erase(it->first);
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
            // Idem cote joueurs. Le tampon PARTICULIEREMENT : garder des echantillons d'avant la
            // sortie d'AoI ferait interpoler le respawn depuis une position vieille de plusieurs
            // secondes — l'avatar traverserait la rue pour rejoindre son propre passe.
            g_tamponsJoueurs.erase(it->first);
            g_absentsDepuis.erase(it->first);
            g_suiviAvatars.erase(it->first);
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
    // ⚠️ Ce nombre est APPARIE au seuil d'alerte de derive du serveur
    // (`TIME_DRIFT_WARN_THRESHOLD_SECS`, gateway.rs) : le serveur ne s'inquiete qu'au-dela de
    // cette tolerance plus une marge. Bouger l'un sans l'autre remet le diagnostic a crier en
    // permanence — ou l'aveugle.
    const int32_t h = static_cast<int32_t>(state->hour());
    const int32_t m = static_cast<int32_t>(state->minute());
    const int32_t tolerance = 3;

    // TAILLE du saut en minutes de jeu, 0 si rien n'a bouge — pas un bool. C'est la seule mesure
    // qui reponde a « le joueur a-t-il vu quelque chose ? » : 4 minutes et le soleil n'a pas bouge
    // d'un degre, une heure et le ciel bascule. La mesurer cote serveur est impossible sans
    // repliement (rapport toutes les 5 s contre correction toutes les ~4 s : les deux periodes
    // battent l'une contre l'autre et rendent une enveloppe fausse — constate le 2026-08-08).
    int32_t sautMinutes = 0;
    if (!Red::CallVirtual(this, "ApplyServerTime", sautMinutes, h, m, tolerance))
    {
        // Une seule fois : ce message se repeterait toutes les 2 secondes pendant toute la session.
        if (!m_avertiHeureIntrouvable)
        {
            m_avertiHeureIntrouvable = true;
            SDK->logger->Warn(PLUGIN,
                "WorldState : ApplyServerTime introuvable (module redscript non compile ?) — "
                "l'heure du monde ne sera PAS synchronisee de toute la session. "
                "Voir r6/logs/redscript_rCURRENT.log. Message affiche une seule fois.");
        }
        return;
    }
    // 0 = l'heure locale etait deja assez proche, rien a corriger. Ce n'est pas une erreur, et le
    // journaliser a chaque message noierait le log.
    if (sautMinutes > 0)
    {
        SDK->logger->InfoF(PLUGIN, "Heure serveur appliquee : %02d:%02d (saut de %d min de jeu)",
            h, m, sautMinutes);
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
        // Une seule fois, meme raison que pour l'heure ci-dessus. Et surtout : on ne memorise
        // PAS `m_lastAppliedWeather` sur cet echec — memoriser une meteo qu'on n'a pas appliquee
        // ferait renoncer a la re-demander si le module redscript revenait (rechargement a chaud).
        if (!m_avertiMeteoIntrouvable)
        {
            m_avertiMeteoIntrouvable = true;
            SDK->logger->Warn(PLUGIN,
                "ApplyServerWeather introuvable (module redscript non compile ?) — la meteo "
                "restera celle du jeu. Message affiche une seule fois.");
        }
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

bool NetworkGameSystem::SendPromotionRequest(uint64_t record, uint64_t apparence, float x, float y,
                                             float z, float yaw, bool mort)
{
    if (m_pInterface == nullptr || record == 0)
    {
        return false;
    }

    // Etranglement : une seule demande par pantin. Sans ca, le meme figurant serait promu a chaque
    // stimulus qu'il declenche — un joueur qui vide un chargeur en creerait dix, et le serveur
    // paierait dix entites la ou une suffit. La cle est le record ET la position arrondie au metre :
    // deux passants du meme archetype a deux endroits restent deux promotions distinctes.
    const auto cle = std::make_tuple(record, static_cast<int32_t>(x), static_cast<int32_t>(y),
                                     static_cast<int32_t>(z));
    if (!g_promotionsDemandees.insert(cle).second)
    {
        return false;
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
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// FLUX D'ARRIVEE — personnages du compte (2026-08-08)
//
// Le serveur decide de tout : quels personnages existent, combien un compte peut en avoir
// (`character.slots.N`, defaut 1, illimite pour un joker), et si un pseudonyme est libre. Le client
// ne fait qu'AFFICHER et DEMANDER. Aucune de ces regles n'est dupliquee ici — c'est ce qui garantit
// qu'un client modifie ne peut pas s'octroyer un second personnage.
// ─────────────────────────────────────────────────────────────────────────────────────────────

void NetworkGameSystem::HandleCharacterList(const cyberpunk_rp::protocol::CharacterList* list)
{
    if (list == nullptr)
    {
        return;
    }
    // REMPLACEMENT, pas fusion : le serveur envoie toujours la liste complete. Un personnage
    // supprime disparait donc de lui-meme, sans message de suppression a traiter.
    m_personnages.clear();
    const auto* characters = list->characters();
    if (characters != nullptr)
    {
        for (uint32_t i = 0; i < characters->size(); ++i)
        {
            const auto* c = characters->Get(i);
            if (c == nullptr)
            {
                continue;
            }
            PersonnageDistant p;
            p.id = c->id();
            p.pseudonyme = c->pseudonym() != nullptr ? c->pseudonym()->str() : std::string();
            p.record = c->base_record();
            p.apparence = c->appearance();
            m_personnages.push_back(std::move(p));
        }
    }
    m_listeRecue = true;
    SDK->logger->InfoF(PLUGIN, "CharacterList : %zu personnage(s) sur ce compte", m_personnages.size());

    // TESSERA_AUTO_CHARACTER=1 : incarne le premier personnage sans passer par l'écran de choix.
    // Réservé aux runs automatisés à deux instances, même usage que TESSERA_DISPLAY_NAME ci-dessus.
    // POURQUOI (2026-08-08) : sans sélection, la Gateway retient TOUT ce que le client envoie (le
    // joueur n'incarne personne), le Shard reste à 0 joueur et aucun rapport n'arrive — un silence
    // que j'ai d'abord pris pour un bug de câblage du halo.
    if (!m_personnages.empty() && std::getenv("TESSERA_AUTO_CHARACTER") != nullptr)
    {
        SDK->logger->InfoF(PLUGIN, "TESSERA_AUTO_CHARACTER : incarnation automatique de %s",
            m_personnages.front().pseudonyme.c_str());
        Tessera_ChoisirPersonnage(m_personnages.front().id);
    }
}

void NetworkGameSystem::HandleCharacterResult(const cyberpunk_rp::protocol::CharacterResult* result)
{
    if (result == nullptr)
    {
        return;
    }
    m_dernierResultatOk = result->success();
    m_dernierResultatMotif = result->reason() != nullptr ? result->reason()->str() : std::string();
    m_aUnResultat = true;
    SDK->logger->InfoF(PLUGIN, "CharacterResult : %s%s%s",
        m_dernierResultatOk ? "succes" : "refus",
        m_dernierResultatMotif.empty() ? "" : " — ",
        m_dernierResultatMotif.c_str());
}

// ══ INTERACTIONS JOUEUR<->JOUEUR (spec 2026-08-09) ═══════════════════════════════════════════

void NetworkGameSystem::HandleActionCatalog(const cyberpunk_rp::protocol::ActionCatalog* msg)
{
    if (msg == nullptr)
    {
        return;
    }
    // REMPLACEMENT, pas fusion : le serveur envoie toujours la liste complete de ce que ce joueur a
    // le droit de faire. Une action retiree (`/groupremove`) disparait donc d'elle-meme — la
    // fusionner la laisserait affichee pour toujours.
    m_actions.clear();
    if (msg->actions() != nullptr)
    {
        for (const auto* a : *msg->actions())
        {
            if (a == nullptr)
            {
                continue;
            }
            ActionRecue r;
            r.id = a->id();
            r.libelle = a->libelle() != nullptr ? a->libelle()->str() : std::string();
            // Le fil porte des DECIMETRES (`portee_dm`, ushort) : un ushort en metres perdrait les
            // demi-metres. La conversion se fait ICI, une fois, pour que le script raisonne en
            // metres comme le reste du jeu.
            r.portee_m = static_cast<float>(a->portee_dm()) / 10.0f;
            m_actions.push_back(std::move(r));
        }
    }
    SDK->logger->InfoF(PLUGIN, "ActionCatalog : %zu action(s) disponibles", m_actions.size());
}

void NetworkGameSystem::HandleIdentitesConnues(
    const cyberpunk_rp::protocol::IdentitesConnues* msg)
{
    if (msg == nullptr || msg->entrees() == nullptr)
    {
        return;
    }
    // ⚠️ ON ACCUMULE, contrairement au catalogue ci-dessus, et la difference est structurelle : ce
    // message arrive EN LOT au join, puis A UNE ENTREE a chaque presentation recue. Le traiter
    // comme un remplacement effacerait toutes les connaissances a chaque poignee de main — et la
    // panne serait discrete, puisque le nom qui vient d'arriver, lui, s'afficherait.
    int ajoutes = 0;
    for (const auto* e : *msg->entrees())
    {
        if (e == nullptr || e->nom() == nullptr)
        {
            continue;
        }
        m_nomsConnus[e->id()] = e->nom()->str();
        ++ajoutes;
    }
    SDK->logger->InfoF(PLUGIN, "IdentitesConnues : +%d, %zu nom(s) connus au total", ajoutes,
        m_nomsConnus.size());
}

void NetworkGameSystem::SendActionJoueur(uint64_t target, uint32_t recette)
{
    if (m_pInterface == nullptr || target == 0 || recette == 0)
    {
        return;
    }

    // kind=2=Interagit, `param` = l'id de la recette. Constantes du gel du schema (protocol.fbs,
    // EntityInteraction) — surtout pas une numerotation locale. Zero octet ajoute au fil montant :
    // le canal existait deja pour les degats (kind=5) et le mount (kind=3/4).
    constexpr uint8_t kKindInteragit = 2;

    SDK->logger->InfoF(PLUGIN, "Action %u demandee sur %llu", recette, target);

    flatbuffers::FlatBufferBuilder builder;
    const auto ei = cyberpunk_rp::protocol::CreateEntityInteraction(
        builder, target, kKindInteragit, recette);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_EntityInteraction, ei.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
}

bool NetworkGameSystem::Tessera_CreerPersonnage(const Red::CString& pseudonyme, uint64_t record,
                                                uint64_t apparence)
{
    if (m_pInterface == nullptr)
    {
        SDK->logger->Warn(PLUGIN, "CreateCharacter ignore : pas de connexion serveur");
        return false;
    }
    // Le pseudonyme n'est PAS valide ici (longueur, caracteres, unicite) : c'est au serveur de le
    // faire, puisque lui seul voit tous les comptes et qu'un client modifie contournerait n'importe
    // quel controle local. Le client se contente de refuser l'evidence — une chaine vide, qui ne
    // merite pas un aller-retour reseau.
    const std::string nom = pseudonyme.c_str() != nullptr ? std::string(pseudonyme.c_str()) : std::string();
    if (nom.empty())
    {
        SDK->logger->Warn(PLUGIN, "CreateCharacter ignore : pseudonyme vide");
        return false;
    }

    SDK->logger->InfoF(PLUGIN, "CreateCharacter : « %s » record %llu apparence %llu",
        nom.c_str(), record, apparence);

    flatbuffers::FlatBufferBuilder builder;
    const auto pseudo = builder.CreateString(nom);
    const auto req = cyberpunk_rp::protocol::CreateCreateCharacter(builder, pseudo, record, apparence);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_CreateCharacter, req.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    return true;
}

bool NetworkGameSystem::Tessera_ChoisirPersonnage(uint64_t id)
{
    if (m_pInterface == nullptr || id == 0)
    {
        SDK->logger->Warn(PLUGIN, "SelectCharacter ignore : pas de connexion, ou id nul");
        return false;
    }
    SDK->logger->InfoF(PLUGIN, "SelectCharacter : id %llu", id);

    flatbuffers::FlatBufferBuilder builder;
    const auto req = cyberpunk_rp::protocol::CreateSelectCharacter(builder, id);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_SelectCharacter, req.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    return true;
}

void NetworkGameSystem::SendStaticNpcReport(uint64_t entityId, uint64_t record, uint64_t apparence,
                                            float x, float y, float z, float yaw)
{
    if (m_pInterface == nullptr || entityId == 0)
    {
        return;
    }
    // Un seul rapport par entite et par session : le serveur applique « premier arrive fait foi ».
    if (!g_statiquesRapportes.insert(entityId).second)
    {
        return;
    }
    // On MET EN FILE, on n'envoie pas : voir `g_fileStatiques`. Le drainage se fait au tick.
    g_fileStatiques.push_back({entityId, record, apparence, x, y, z,
        static_cast<int16_t>(std::lround(yaw))});
    // Instrument : sans lui, « le serveur ne recoit rien » ne distingue pas « le client n'envoie
    // pas » de « le message se perd ». Cadence pour ne pas noyer le journal.
    if (g_fileStatiques.size() % 25 == 1)
    {
        SDK->logger->InfoF(PLUGIN, "Statique mis en file (%zu en attente, %zu deja rapportes)",
            g_fileStatiques.size(), g_statiquesRapportes.size());
    }
}

void NetworkGameSystem::DrainerRapportsStatiques()
{
    if (m_pInterface == nullptr || g_fileStatiques.empty())
    {
        return;
    }
    const auto maintenant = std::chrono::steady_clock::now();
    if (maintenant - g_dernierEnvoiStatique < kIntervalleStatique)
    {
        return;
    }
    g_dernierEnvoiStatique = maintenant;

    const auto rapport = g_fileStatiques.front();
    g_fileStatiques.pop_front();

    flatbuffers::FlatBufferBuilder builder;
    const cyberpunk_rp::protocol::QVec3 pos(QuantPos(rapport.x), QuantPos(rapport.y),
                                            QuantPos(rapport.z));
    const auto rep = cyberpunk_rp::protocol::CreateStaticNpcReport(
        builder, rapport.entityId, rapport.record, rapport.apparence, &pos, rapport.yaw);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_StaticNpcReport, rep.Union());
    builder.Finish(env);
    const auto res = m_pInterface->SendMessageToConnection(m_hConnection,
        builder.GetBufferPointer(), builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    if (res != k_EResultOK)
    {
        SDK->logger->WarnF(PLUGIN, "Rapport statique REFUSE par le transport (res=%d)",
            static_cast<int>(res));
    }
}

void NetworkGameSystem::HandleCellAppearances(
    const cyberpunk_rp::protocol::CellAppearances* msg)
{
    if (msg == nullptr || msg->entries() == nullptr)
    {
        return;
    }
    // La cellule est notee RECUE avant meme d'appliquer : c'est ce qui fait cesser les rapports
    // dans cette zone, que l'application reussisse ou non. Un PNJ pas encore streame sera repris
    // par la passe d'hydratation.
    g_cellulesRecues.insert({msg->cell_x(), msg->cell_y()});

    uint32_t n = 0;
    for (const auto* e : *msg->entries())
    {
        if (e == nullptr || e->entity_id() == 0 || e->appearance() == 0)
        {
            continue;
        }
        g_apparencesStatiques[e->entity_id()] = e->appearance();
        g_apparenceConnueDepuis.emplace(e->entity_id(), std::chrono::steady_clock::now());
        g_apparencesAppliquees.erase(e->entity_id());
        // Le ROSTER, distinct de la table d'apparences : il porte de quoi RECREER le PNJ chez un
        // client a qui il manque (spec 2026-08-09). Un client qui possede deja le natif n'en fait
        // rien — c'est justement ce qui rend la reparation asymetrique.
        if (const auto* p = e->position())
        {
            g_rosterStatiques[e->entity_id()] = InscriptionRoster{e->record(), e->appearance(),
                DequantPos(p->x()), DequantPos(p->y()), DequantPos(p->z()), e->yaw()};
        }
        ++n;
    }
    // Le compte du ROSTER est journalise a part, et ce n'est pas un doublon : une entree sans
    // position n'entre pas au roster, donc « N apparences » peut valoir 40 pendant que le roster
    // reste vide — ce qui s'est produit, et que rien ne disait.
    SDK->logger->InfoF(PLUGIN, "Halo : cellule (%d,%d) recue — %u apparences, roster %zu entrees",
        msg->cell_x(), msg->cell_y(), n, g_rosterStatiques.size());
}

void NetworkGameSystem::HandleStaticAppearance(const cyberpunk_rp::protocol::StaticAppearance* msg)
{
    if (msg == nullptr || msg->entity_id() == 0 || msg->appearance() == 0)
    {
        return;
    }
    // On note ce qu'on a RECU avant meme de l'appliquer : si l'entite n'est pas encore streamee,
    // redscript echouera, et c'est cette table qui permettra de reessayer plus tard.
    g_apparencesStatiques[msg->entity_id()] = msg->appearance();
    // Retiree de la liste des appliquees : une nouvelle autorite doit etre (re)posee.
    g_apparencesAppliquees.erase(msg->entity_id());

    const RED4ext::ent::EntityID cible(msg->entity_id());
    const RED4ext::CName apparence(msg->appearance());
    bool ok = false;
    if (Red::CallVirtual(this, "AppliquerApparenceStatique", ok, cible, apparence) && ok)
    {
        g_apparencesAppliquees.insert(msg->entity_id());
    }
    else
    {
        // Cause attendue et NORMALE : le PNJ n'est pas encore charge chez nous. Le serveur diffuse
        // sans filtre de distance, exprès — un joueur doit connaitre l'apparence AVANT d'arriver,
        // sinon il la verrait changer sous ses yeux. L'application se refera a l'attachement.
        SDK->logger->InfoF(PLUGIN, "Apparence statique %llu memorisee (entite pas encore la)",
            msg->entity_id());
    }
}

void NetworkGameSystem::ReparerRoster()
{
    // ── POURQUOI CETTE PASSE EXISTE ──────────────────────────────────────────────────────────
    //
    // Mesure du 2026-08-09, deux clients a la position IDENTIQUE, immobiles : seulement **83 %**
    // des PNJ statiques sont vus des deux cotes, et le chiffre est **PLAT sur neuf minutes**. Ce
    // n'est donc pas un retard de streaming qu'il suffirait d'attendre — les deux moteurs peuplent
    // durablement deux mondes differents.
    //
    // On ne peut ni piloter la foule native (F-PNJ-069 : natif C++, aucun point d'entree scripte),
    // ni en retirer un membre (F-PNJ-091 et F-PNJ-093 sont des impasses). Il ne reste qu'une voie :
    // COMPLETER ce qui manque, chez le client a qui ca manque.
    //
    // ⚠️ Le remplacant est LOCAL, et c'est le coeur de la conception. Si le serveur le spawnait
    // comme entite reseau, le client qui possede deja le natif le recevrait aussi et verrait un
    // DOUBLON — qu'on ne saurait pas supprimer.
    // ⚠️ INTERRUPTEUR DE MESURE (2026-08-09) — `TESSERA_ROSTER_OFF=1`.
    //
    // Ce mecanisme et la promotion creent tous deux des PNJ, sans se consulter : un meme personnage
    // peut donc exister en TROIS exemplaires (figurant local, promu serveur, remplacant de roster).
    // Observe en jeu le 2026-08-09, avec 704 remplacants crees en une session.
    //
    // Cet interrupteur n'existe PAS pour desactiver la fonctionnalite — il existe pour qu'on puisse
    // mesurer l'autre moitie du systeme sans son bruit. Meme famille que `TESSERA_DISPLAY_NAME` et
    // `TESSERA_AUTO_CHARACTER` : un reglage de RUN AUTOMATISE, absent par defaut, sans effet sur un
    // joueur. Le comportement par defaut est strictement inchange.
    static const bool rosterCoupe = []() {
        char* v = nullptr;
        size_t n = 0;
        const bool ok = _dupenv_s(&v, &n, "TESSERA_ROSTER_OFF") == 0 && v != nullptr;
        const bool coupe = ok && v[0] == '1';
        if (v) free(v);
        return coupe;
    }();
    if (rosterCoupe)
    {
        return;
    }
    if (g_rosterStatiques.empty())
    {
        return;
    }
    // Delai de grace : on laisse au streaming natif le temps de faire son travail avant de le
    // suppleer. Substituer trop tot fabriquerait un doublon chaque fois que le natif est
    // simplement en retard de quelques secondes.
    constexpr auto kDelaiDeGrace = std::chrono::seconds(10);
    // Budget par tick, comme l'hydratation : un lot de pantins complets cree d'un coup se verrait.
    constexpr int kMaxParTick = 4;
    const auto maintenant = std::chrono::steady_clock::now();
    int faits = 0;

    // ── UN REMPLAÇANT PAR ENDROIT, PAS PAR IDENTIFIANT ─────────────────────────────────────
    //
    // La garde `g_remplacants` ci-dessous est correcte, et elle ne suffit pas : elle interdit deux
    // remplaçants pour un même IDENTIFIANT, alors que le roster contient plusieurs identifiants
    // pour un même PNJ RÉEL.
    //
    // Mesuré le 2026-08-10 (F-PNJ-150), session de 7 min à deux clients : **2 199 identifiants
    // distincts** ont reçu un remplaçant, pour **238 positions distinctes** — ~9,2 identifiants par
    // emplacement physique, et jusqu'à **110 remplaçants empilés en un seul point**. C'est ce que
    // Lucas voyait en jeu : « les PNJ statiques sont dupliqués ».
    //
    // ⚠️ Pourquoi une clé de POSITION plutôt qu'une meilleure clé d'identité : parce qu'on ne sait
    // pas encore POURQUOI un emplacement produit neuf identifiants — re-streaming qui réattribue,
    // ou deux clients qui rapportent chacun le leur (F-PNJ-150 pose la sonde qui trancherait). Une
    // clé de position est juste dans les DEUX cas. On ne fait pas dépendre un correctif d'une cause
    // qui n'est pas isolée.
    //
    // Décimètre, et la même formule que le redscript (`Cast<Int32>(x * 10.0)`) : c'est à cette
    // précision que la mesure a vu les 2 199 s'effondrer sur 238, donc elle suffit — et partager la
    // formule évite que les deux côtés découpent l'espace différemment.
    const auto cleDePosition = [](const InscriptionRoster& i) {
        return std::make_tuple(static_cast<int32_t>(i.x * 10.0f), static_cast<int32_t>(i.y * 10.0f),
                               static_cast<int32_t>(i.z * 10.0f));
    };
    std::set<std::tuple<int32_t, int32_t, int32_t>> occupees;
    for (const auto& [idPris, _] : g_remplacants)
    {
        const auto inscrit = g_rosterStatiques.find(idPris);
        if (inscrit != g_rosterStatiques.end())
        {
            occupees.insert(cleDePosition(inscrit->second));
        }
    }

    for (const auto& [id, inscription] : g_rosterStatiques)
    {
        if (faits >= kMaxParTick)
        {
            break;
        }
        const RED4ext::ent::EntityID cible(id);
        const auto remplacant = g_remplacants.find(id);
        if (remplacant != g_remplacants.end())
        {
            // On a deja un remplacant : la seule question est de savoir si quelqu'un d'AUTRE se
            // tient desormais a cet endroit. Si oui, le notre est le doublon et doit partir.
            //
            // ── L'ARBITRAGE, ET POURQUOI IL N'EST PAS UN CHOIX ────────────────────────────
            //
            // C'est TOUJOURS le notre qui part, jamais l'autre — non par preference, mais par
            // capacite : un PNJ de communaute natif NE SE RETIRE PAS (F-PNJ-091, F-PNJ-093, deux
            // impasses mesurees), et `DeleteEntity` rend meme un succes sans rien supprimer quand
            // l'entite est de-streamee (F-PNJ-090). Notre remplaçant est la seule entite qu'on ait
            // le droit — et le pouvoir — de detruire. La regle « garder le natif » n'est donc pas
            // discutable, elle est la seule executable.
            //
            // ⚠️ La question posee change : on demandait « l'identifiant `cible` existe-t-il ? »
            // (`TesseraEntiteExisteLocalement`), on demande maintenant « cet ENDROIT est-il occupe
            // par un autre ? ». Le premier reposait sur `FindEntityByID`, qui rend nil sur un
            // pantin bien vivant (F-PNJ-088) : quand il se trompait, notre doublon SURVIVAIT au
            // natif revenu. Le second ne depend d'aucune cle.
            const RED4ext::Vector4 ici{inscription.x, inscription.y, inscription.z, 1.0f};
            bool occupe = false;
            if (Red::CallVirtual(this, "TesseraQuelquUnIci", occupe, ici, 0.6f, remplacant->second)
                && occupe)
            {
                Red::CallVirtual(this, "DestroyTransientEntity", remplacant->second);
                SDK->logger->InfoF(PLUGIN,
                    "Roster : place de %llu occupee par un autre — notre remplacant retire", id);
                g_remplacants.erase(id);
                ++g_statsRoster.retiresPlaceOccupee;
                ++faits;
            }
            continue;
        }
        const auto connu = g_apparenceConnueDepuis.find(id);
        if (connu == g_apparenceConnueDepuis.end() || maintenant - connu->second < kDelaiDeGrace)
        {
            continue;
        }
        // ── ON NE REMPLACE JAMAIS QUELQU'UN QU'ON A DÉJÀ VU DE SES YEUX ────────────────────
        //
        // Recette de Lucas, 2026-08-10 : « je m'approche d'un PNJ, il reste normal ; je tourne à
        // 180° sur moi, il est dans mon dos ; je me retourne, je le vois normal et là il se
        // duplique une fois. »
        //
        // Le journal donne le mécanisme, et ce n'est ni le champ de vision ni la garde spatiale :
        //   [Etat] …;decharge;decharge     <- le moteur DÉCHARGE le natif passé dans le dos
        //   [Etat] …;remplacant-cree       <- on le supplée aussitôt
        // Au retour du regard, le natif re-streame : deux personnes, deux tenues.
        //
        // Le défaut est dans le délai de grâce, qui mesure la mauvaise chose : il court depuis que
        // l'APPARENCE EST CONNUE, pas depuis la dernière fois où l'entité a été VUE. Passé dix
        // secondes, la moindre absence — y compris un déchargement d'une seconde — déclenche un
        // remplacement.
        //
        // Le bon critère est celui de la raison d'être du roster : compléter ce qui manque
        // DURABLEMENT chez ce client (mesure du 2026-08-09 : 83 % de présence commune, PLAT sur
        // neuf minutes — donc pas un retard de streaming). Un PNJ qu'on a vu présent au moins une
        // fois n'est pas de cette famille : son absence est du déchargement, et il reviendra.
        //
        // ⚠️ Conséquence assumée : un PNJ vu une fois puis disparu pour de bon ne sera plus
        // suppléé. C'est le bon compromis — un manque se voit moins qu'un doublon, et le doublon,
        // lui, est certain.
        // ⚠️ LE TEST MÉMOIRE D'ABORD, L'APPEL SCRIPT ENSUITE — l'ordre inverse était un chemin
        // chaud, et le compteur l'a révélé : **2 208 063 refus** sur une session. Le garde était
        // évalué APRÈS l'appel de présence, donc on payait un `CallVirtual` → `FindEntityByID` pour
        // CHAQUE entrée du roster, à CHAQUE frame — ~173 entrées × 60 fps ≈ 10 000 appels script
        // par seconde, pour reconfirmer 173 fois par frame ce qu'on savait déjà.
        //
        // « Déjà vu » est définitif par construction : une fois l'entité aperçue, aucune mesure ne
        // peut l'infirmer. Le relire coûte, n'apprend rien, et le coût tombe pile dans la boucle de
        // rendu — donc en micro-saccades.
        if (g_dejaVus.contains(id))
        {
            ++g_statsRoster.refusesDejaVu;
            continue;
        }
        bool presentMaintenant = false;
        if (Red::CallVirtual(this, "TesseraEntiteExisteLocalement", presentMaintenant, cible)
            && presentMaintenant)
        {
            g_dejaVus.insert(id);
            ++g_statsRoster.refusesDejaVu;
            continue;
        }
        // Quelqu'un tient déjà cet endroit : ce serait le 2e, le 10e, le 110e du même individu.
        if (occupees.contains(cleDePosition(inscription)))
        {
            ++g_statsRoster.refusesEndroitPris;
            continue;
        }
        // Le redscript tranche : lui seul voit le joueur, l'entite et les distances. Une EntityID
        // vide signifie « pas maintenant » (natif present, trop pres, trop loin), jamais
        // « impossible » — on retentera au tick suivant.
        const RED4ext::Vector4 position{inscription.x, inscription.y, inscription.z, 1.0f};
        const float yawRad = static_cast<float>(inscription.yaw) * 3.14159265f / 180.0f;
        const RED4ext::Quaternion orientation{
            0.0f, 0.0f, std::sin(yawRad * 0.5f), std::cos(yawRad * 0.5f)};
        RED4ext::ent::EntityID cree;
        if (Red::CallVirtual(this, "ReparerStatique", cree, cible,
                RED4ext::TweakDBID(inscription.record), RED4ext::CName(inscription.apparence),
                position, orientation)
            && cree.IsDefined())
        {
            g_remplacants[id] = cree;
            ++g_statsRoster.crees;
            // Marquer l'endroit AVANT de sortir de la boucle : sans ça, les huit autres
            // identifiants du même individu, tous encore à parcourir dans CE passage, en
            // fabriqueraient chacun un de plus. `occupees` se reconstruit au passage suivant.
            occupees.insert(cleDePosition(inscription));
            SDK->logger->InfoF(PLUGIN, "Roster : %llu absent — remplacant cree (%zu au total)", id,
                g_remplacants.size());
            ++faits;
        }
    }
}

void NetworkGameSystem::DetruireTousLesRemplacants(const char* raison)
{
    if (g_remplacants.empty())
    {
        return;
    }
    const std::size_t combien = g_remplacants.size();
    for (const auto& [id, entite] : g_remplacants)
    {
        (void)id;
        Red::CallVirtual(this, "DestroyTransientEntity", entite);
    }
    g_remplacants.clear();
    // ⚠️ `g_dejaVus` N'EST PAS vidé : ce qu'on a vu de ses yeux reste vrai après une déconnexion,
    // et l'oublier rendrait chaque PNJ à nouveau duplicable à la reconnexion. Le roster, lui, sera
    // renvoyé par le serveur.
    SDK->logger->InfoF(PLUGIN, "Roster : %zu remplacants detruits (%s)", combien, raison);
}

void NetworkGameSystem::NettoyerRemplacants(float deltaTime)
{
    // ── LA CONTREPARTIE DE TOUTE CRÉATION ──────────────────────────────────────────────────
    //
    // Trois portes créent des remplaçants ; une seule les détruisait, et seulement quand quelqu'un
    // d'autre occupait la place. Il manquait le cas le plus banal d'un jeu ouvert : le joueur
    // s'éloigne, la cellule est oubliée côté serveur, et notre remplaçant reste debout pour le
    // reste de la session. Rien ne l'aurait jamais retiré.
    //
    // Cette passe est le filet de fond. Elle ne remplace aucune garde — elle rattrape ce qu'elles
    // laissent passer, y compris des cas qu'on n'a pas encore imaginés.
    //
    // ⚠️ BORNÉE EN TRAVAIL ET EN FRÉQUENCE. Un balayage complet à chaque frame sur une table qui
    // compte des centaines d'entrées est exactement le défaut qui a coûté 428 ms/tick côté serveur
    // (index de halo). Ici : une passe toutes les 2 s, 16 entrées au plus, reprise en tourniquet
    // là où on s'était arrêté — donc coût constant quelle que soit la taille de la table.
    static constexpr float kPeriodeS = 2.0f;
    static constexpr std::size_t kParPasse = 16;
    // 300 m et non 250 (le plafond de création) : SANS cette hystérésis, un remplaçant créé à la
    // limite serait purgé au pas suivant, recréé, repurgé — un cycle qui clignote sous les yeux du
    // joueur. Les 50 m d'écart sont la marge qui rend le cycle impossible.
    static constexpr float kPurgeM = 300.0f;

    m_tempsDepuisNettoyage += deltaTime;
    if (m_tempsDepuisNettoyage < kPeriodeS)
    {
        return;
    }
    m_tempsDepuisNettoyage = 0.0f;
    if (g_remplacants.empty())
    {
        return;
    }

    const auto joueur = Cyberverse::Utils::GetPlayer();
    if (joueur == nullptr)
    {
        return; // pas de repère : on ne purge rien plutôt que de purger au hasard.
    }
    const auto pos = Cyberverse::Utils::Entity_GetWorldPosition(joueur);

    // Tourniquet : on reprend après le dernier id traité, et on repart du début en fin de table.
    auto it = g_remplacants.upper_bound(m_dernierRemplacantExamine);
    if (it == g_remplacants.end())
    {
        it = g_remplacants.begin();
    }
    for (std::size_t faits = 0; faits < kParPasse && !g_remplacants.empty(); ++faits)
    {
        if (it == g_remplacants.end())
        {
            it = g_remplacants.begin();
        }
        const uint64_t id = it->first;
        const RED4ext::ent::EntityID entite = it->second;
        m_dernierRemplacantExamine = id;

        // 1. L'entité a-t-elle disparu sans nous ? Le moteur détruit ce qu'il veut : garder une
        //    fiche pour une entité morte fait grossir la table sans fin et fausse les compteurs.
        if (!Cyberverse::Utils::GetDynamicEntity(entite).has_value())
        {
            ++g_statsRoster.oubliesDisparus;
            it = g_remplacants.erase(it);
            continue;
        }

        // 2. Trop loin : le joueur est parti, la cellule est oubliée côté serveur, ce remplaçant
        //    n'a plus personne pour le regarder ni rien pour le justifier.
        const auto inscrit = g_rosterStatiques.find(id);
        if (inscrit != g_rosterStatiques.end())
        {
            const float dx = inscrit->second.x - pos.X;
            const float dy = inscrit->second.y - pos.Y;
            const float dz = inscrit->second.z - pos.Z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > kPurgeM)
            {
                Red::CallVirtual(this, "DestroyTransientEntity", entite);
                ++g_statsRoster.purgesDistance;
                it = g_remplacants.erase(it);
                continue;
            }
        }
        ++it;
    }

    // ── LE VÉRIFICATEUR ────────────────────────────────────────────────────────────────────
    //
    // Une ligne toutes les 30 s, qui dit d'un coup d'œil si le système se tient : combien vivent,
    // combien ont été créés, et par quelle garde les autres ont été refusés. Un ratio qui bascule
    // se voit ici sans avoir à reproduire quoi que ce soit.
    m_tempsDepuisBilanRoster += kPeriodeS;
    if (m_tempsDepuisBilanRoster >= 30.0f)
    {
        m_tempsDepuisBilanRoster = 0.0f;
        SDK->logger->InfoF(PLUGIN,
            "Roster : %zu vivants | crees %llu | retires(place) %llu purges(loin) %llu "
            "oublies(disparus) %llu | refus dejavu %llu endroit %llu | roster %zu, dejavus %zu",
            g_remplacants.size(), g_statsRoster.crees, g_statsRoster.retiresPlaceOccupee,
            g_statsRoster.purgesDistance, g_statsRoster.oubliesDisparus,
            g_statsRoster.refusesDejaVu, g_statsRoster.refusesEndroitPris,
            g_rosterStatiques.size(), g_dejaVus.size());
    }
}

void NetworkGameSystem::HydraterApparencesDiscretement()
{
    // ── POURQUOI CETTE PASSE EXISTE ──────────────────────────────────────────────────────────
    //
    // Deux defauts qu'elle corrige d'un coup.
    //
    // 1. UN ORDRE ARRIVE TROP TOT SE PERDAIT. Le serveur diffuse sans filtre de distance (exprès :
    //    un joueur doit connaitre l'apparence AVANT d'arriver). Si le PNJ n'est pas encore streame,
    //    l'application echoue, on memorise… et on ne reessayait qu'a son prochain attachement, qui
    //    peut ne jamais venir. C'est la cause probable des divergences residuelles observees par
    //    Lucas le 2026-08-08 (« la majorite a la meme esthetique, mais certains non »).
    //
    // 2. UN CHANGEMENT D'APPARENCE SOUS LES YEUX DU JOUEUR SE VOIT. D'ou l'idee de Lucas : ne
    //    l'appliquer que HORS du champ de vision. Le rendu reste fidele pour tout le monde, et
    //    personne ne voit un passant se rhabiller.
    //
    // Une seule par tick : un lot d'apparences appliquees d'un coup ferait un a-coup visible meme
    // hors champ (cout de rendu), et rien ne presse.
    // ── RE-VERIFICATION PERIODIQUE ───────────────────────────────────────────────────────────
    //
    // « Hydrate » n'est PAS un etat definitif. Un PNJ decharge puis recharge (le joueur s'eloigne
    // et revient) se voit retirer une apparence AU HASARD par le jeu, et rien ne la recorrigeait :
    // la coherence se degradait a chaque deplacement, en silence. C'est le defaut que le protocole
    // de Lucas — une instance qui marche, une qui reste — a ete concu pour reveler.
    //
    // On repasse donc toute la table a intervalle regulier. Ce n'est pas couteux : une entite deja
    // correcte repond DEJA-BON cote redscript et ressort sans qu'aucun ordre ne soit emis, donc
    // sans le moindre effet visible. Remettre l'horloge de forcage a zero est indispensable —
    // sinon toutes les entites redeviendraient « echues » d'un coup et se rhabilleraient en pleine
    // vue, ce que les 90 s ci-dessus cherchent justement a eviter.
    // ⚠️ DOIT rester nettement PLUS LONGUE que `kDelaiForcage` ci-dessous. Sinon la remise a zero
    // de l'horloge repousse l'echeance a chaque passe et le forcage n'arrive JAMAIS : mesure du
    // 2026-08-09 avec 1 min contre 90 s — 6 apparences appliquees pour 39 770 refus, et la
    // coherence retombee de 97 % a 89 %. Les deux reglages se lisent ensemble, jamais separement.
    constexpr auto kPeriodeVerification = std::chrono::minutes(5);
    static auto derniereVerification = std::chrono::steady_clock::now();
    const auto maintenant = std::chrono::steady_clock::now();
    if (maintenant - derniereVerification > kPeriodeVerification)
    {
        derniereVerification = maintenant;
        g_apparencesAppliquees.clear();
        for (auto& [id, _] : g_apparenceConnueDepuis)
        {
            g_apparenceConnueDepuis[id] = maintenant;
        }
        SDK->logger->InfoF(PLUGIN, "Hydratation : re-verification des %zu apparences connues",
            g_apparencesStatiques.size());
    }

    if (g_apparencesStatiques.size() == g_apparencesAppliquees.size())
    {
        return;
    }
    // Bornee : un PNJ hors champ se trouve en quelques essais, et on ne balaie pas des milliers
    // d'entrees a chaque tick quand tout est dans le champ.
    constexpr int kMaxTentativesParTick = 16;
    // 90 s et non 15 : le cone doit avoir le TEMPS de trouver son occasion. Un PNJ sort du champ
    // des que le joueur tourne la tete, entre dans un menu, ou passe une porte — il suffit d'une
    // fraction de seconde, et l'echange est alors invisible. Forcer a 15 s rhabillait les gens en
    // pleine vue avant meme que l'occasion ne se presente. L'echeance reste, parce que sans elle
    // un PNJ fixe en permanence ne convergeait JAMAIS (mesure : 2 appliquees sur 112).
    // ponytail: constante en dur, a passer en config serveur si un operateur veut arbitrer
    // « fidelite immediate » contre « aucun rhabillage visible ».
    constexpr auto kDelaiForcage = std::chrono::seconds(10);
    int tentatives = 0;
    for (const auto& [id, apparence] : g_apparencesStatiques)
    {
        if (g_apparencesAppliquees.contains(id))
        {
            continue;
        }
        bool ok = false;
        const RED4ext::ent::EntityID cible(id);
        const RED4ext::CName nom(apparence);
        // ECHEANCE DE CONVERGENCE. La discretion est une preference, pas une condition : un PNJ
        // qu'on regarde en continu ne quitte jamais le cone, et son apparence ne serait JAMAIS
        // appliquee. Mesure du 2026-08-08, joueur immobile : 16 appliquees sur 112 connues, 70 985
        // refus. Passe ce delai on applique quand meme — mieux vaut un changement visible qu'une
        // divergence permanente entre deux joueurs.
        const auto connueDepuis = g_apparenceConnueDepuis.find(id);
        const bool echu = connueDepuis != g_apparenceConnueDepuis.end() &&
            (std::chrono::steady_clock::now() - connueDepuis->second) > kDelaiForcage;
        const char* methode = echu ? "AppliquerApparenceStatique" : "AppliquerApparenceDiscrete";
        // Le redscript decide s'il est DISCRET d'appliquer maintenant — c'est lui qui voit le
        // joueur et le PNJ. Un `false` signifie « pas maintenant », pas « impossible ».
        if (Red::CallVirtual(this, methode, ok, cible, nom) && ok)
        {
            g_apparencesAppliquees.insert(id);
        }
        else
        {
            ++g_hydratationsDifferees;
            // ⚠️ NE PAS `return` ici. La version precedente sortait des le premier refus : elle
            // re-tentait donc la MEME entite a chaque tick, indefiniment, sans jamais atteindre les
            // suivantes. Mesure du 2026-08-08 : 2 apparences appliquees pour 11 824 refus, sur 112
            // connues — la cause racine des divergences residuelles vues par Lucas.
            if (++tentatives >= kMaxTentativesParTick)
            {
                return;
            }
            continue;
        }
        // Deux compteurs, pas un : « appliquees » seul ne dirait pas si le cone de vision BLOQUE
        // (le PNJ reste dans le champ) ou s'il n'y a simplement rien a faire. Le rapport entre les
        // deux est ce qui tranchera l'etape 5 de la spec (retirer le cone, ou non).
        if ((g_apparencesAppliquees.size() + g_hydratationsDifferees) % 25 == 1)
        {
            SDK->logger->InfoF(PLUGIN, "Hydratation : %zu appliquees, %zu differees (%zu connues)",
                g_apparencesAppliquees.size(), g_hydratationsDifferees,
                g_apparencesStatiques.size());
        }
        return; // une seule tentative par tick, reussie ou non
    }
}

bool NetworkGameSystem::Tessera_SupprimerPersonnage(uint64_t id)
{
    if (m_pInterface == nullptr || id == 0)
    {
        SDK->logger->Warn(PLUGIN, "DeleteCharacter ignore : pas de connexion, ou id nul");
        return false;
    }
    SDK->logger->InfoF(PLUGIN, "DeleteCharacter : id %llu", id);

    flatbuffers::FlatBufferBuilder builder;
    const auto req = cyberpunk_rp::protocol::CreateDeleteCharacter(builder, id);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_DeleteCharacter, req.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    return true;
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

// Seuil d'emission, en pour mille de la barre. Rien ne part en dessous.
//
// ⚠️ CHIFFRE INVENTE, et il doit le rester jusqu'a ce qu'une session le cale. 20 pour mille = 2 %
// de la barre : une regeneration lente n'emet presque rien, un soin d'inhalateur part tout de
// suite. C'est la regle « un SEUIL, pas une cadence » de la spec 2026-08-09 — la meme qui a fait
// ses preuves sur l'etranglement des stimulus.
static constexpr float kSeuilVariationPermille = 20.0f;

int32_t NetworkGameSystem::Tessera_RapporterVariation(float pourcentCourant, uint32_t cause)
{
    if (m_pInterface == nullptr || pourcentCourant < 0.0f)
    {
        return 0;
    }
    // Premier passage : on n'a rien a comparer. On etablit la reference et on se tait — sinon
    // l'entree en session serait rapportee comme un gain de toute la barre.
    if (m_santeLocaleConnue < 0.0f)
    {
        m_santeLocaleConnue = pourcentCourant;
        return 0;
    }

    // Pourcentage -> pour mille : la barre serveur est une FRACTION, donc une variation relative se
    // transporte telle quelle, quel que soit le maximum de vie du joueur (chrome compris).
    const float deltaPermille = (pourcentCourant - m_santeLocaleConnue) * 10.0f;
    if (deltaPermille > -kSeuilVariationPermille && deltaPermille < kSeuilVariationPermille)
    {
        return 0; // sous le seuil : on laisse s'accumuler plutot que d'emettre du bruit
    }

    // ⚠️ ON NE RAPPORTE QUE LES PERTES. JAMAIS LES GAINS. Mesure du 2026-08-09 :
    //
    //     17:23:55.650  degats 171 sur 9 -> sante 307
    //     17:23:56.050  variation +92 de 9 -> sante 399     (400 ms plus tard)
    //
    // La victime encaisse la balle DEUX FOIS : le moteur la lui applique localement, et le serveur
    // la lui applique aussi. Les deux barres divergent, ce sondeur voit la barre locale AU-DESSUS
    // de celle du serveur, et rapporte l'ecart comme un gain. Le serveur l'accorde — et le mort
    // remonte. C'est la boucle qui produisait « le personnage se releve alors qu'il est mort ».
    //
    // Le sens est asymetrique parce que la REALITE l'est :
    //   · une PERTE que le serveur ignore est une vraie information (chute, feu, PNJ) ;
    //   · un GAIN, depuis que la regeneration est coupee, n'est plus jamais une vraie information —
    //     c'est un ecart de reconciliation. Le seul gain legitime restant serait un soin explicite,
    //     et il devra passer par sa propre cause, pas par un ecart observe.
    //
    // On perd donc la remontee des soins, et c'est assume : mieux vaut un soin non replique qu'un
    // mort qui se releve.
    if (deltaPermille > 0.0f)
    {
        // La reference avance quand meme : sinon l'ecart se represenerait a chaque passe et on
        // journaliserait en boucle un gain qu'on ne rapporte pas.
        m_santeLocaleConnue = pourcentCourant;
        return 0;
    }

    // La reference avance AVANT l'envoi : si le message se perd, on ne rejouera pas la meme
    // variation au passage suivant. Un delta perdu est un delta perdu — le serveur reste la verite,
    // et il corrigera au prochain HealthSync.
    m_santeLocaleConnue = pourcentCourant;

    const auto delta = static_cast<int16_t>(deltaPermille);
    SDK->logger->InfoF(PLUGIN, "Variation de vie rapportee : %+d pour mille (cause %u)",
        static_cast<int>(delta), cause);

    flatbuffers::FlatBufferBuilder builder;
    const auto hr = cyberpunk_rp::protocol::CreateHealthReport(
        builder, delta, static_cast<uint8_t>(cause));
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_HealthReport, hr.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    return delta;
}

bool NetworkGameSystem::Tessera_DemanderReapparition()
{
    if (m_pInterface == nullptr)
    {
        return false;
    }
    // ⚠️ DEFINIE DANS LE .CPP, pas dans l'en-tete : celui-ci ne connait le protocole que par
    // declarations avancees (pour ne pas tirer l'en-tete genere partout), donc aucun
    // `Create*` n'y est visible. Meme raison que `SendStimReport`.
    flatbuffers::FlatBufferBuilder builder;
    const auto rr = cyberpunk_rp::protocol::CreateRespawnRequest(builder);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_RespawnRequest, rr.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    SDK->logger->Info(PLUGIN, "Reapparition demandee au serveur");
    return true;
}

void NetworkGameSystem::HandleHealthSync(const cyberpunk_rp::protocol::HealthSync* msg)
{
    if (msg == nullptr)
    {
        return;
    }

    // Journalise systematiquement : c'est le SEUL point d'observation du retour serveur. Sans lui,
    // « la barre ne bouge pas » ne distingue pas « rien n'arrive » de « ca arrive et l'application
    // echoue » — deux causes opposees, meme symptome a l'ecran.
    SDK->logger->InfoF(PLUGIN, "HealthSync %llu -> %u/1000 (%s)",
        msg->id(), static_cast<unsigned>(msg->health()), msg->mine() ? "moi" : "voisin");

    if (msg->mine())
    {
        // Etat de coma pousse par le SERVEUR. `-1` quand on est vivant : « pas de decompte » et
        // « decompte a zero » ne doivent pas se confondre — la seconde autorise l'hopital.
        if (msg->health() == 0)
        {
            m_secondesSecours = static_cast<int32_t>(msg->secondes_secours());
            m_hopitalOuvert = msg->hopital_ouvert();
        }
        else
        {
            m_secondesSecours = -1;
            m_hopitalOuvert = false;
        }

        // MES jauges de faim/soif (chantier besoins, 2026-08-09). Simple memorisation : ce sont les
        // widgets du HUD qui viendront les LIRE (`Tessera_Faim`/`Tessera_Soif`), comme l'ecran de
        // mort lit deja `Tessera_SecondesSecours`. Aucun appel vers redscript ici — un HUD pas
        // encore monte n'a alors rien a rater.
        m_faim = static_cast<int32_t>(msg->faim());
        m_soif = static_cast<int32_t>(msg->soif());

        // MA sante. Redscript ecrit la barre de vie du joueur local ; a 0, la mort NATIVE
        // s'enclenche toute seule et l'ecran de mort garni (C20) s'affiche derriere elle. On ne
        // reimplemente ni la mort, ni son ecran — on ne fait que poser le nombre.
        // `uint32_t` et non `uint16_t` : redscript n'a pas de type 16 bits, la conversion se fait
        // au franchissement du fil — meme regle que `nature` dans `Tessera_ReportStim`.
        // ⚠️ ANTI-ECHO, ET C'EST LE POINT DE CONCEPTION DU CANAL MONTANT. Ce que le serveur nous
        // impose devient immediatement notre reference : la detection locale ne verra donc PAS
        // cette ecriture comme une variation, et ne la renverra pas. Sans cette ligne, chaque
        // HealthSync produirait un HealthReport, qui produirait un HealthSync — une boucle.
        m_santeLocaleConnue = static_cast<float>(msg->health()) / 10.0f;

        // ⚠️ ON TRANSMET TOUJOURS, ET C'EST REDSCRIPT QUI DECIDE D'ECRIRE OU NON.
        //
        // Deux pannes opposees ont ete traversees ici en une heure, et la lecon est la meme :
        //   · reecrire la barre a CHAQUE battement -> entre deux battements le moteur rend sa vie
        //     au joueur, il se releve, le battement suivant le retue. Un va-et-vient visible.
        //   · ne l'ecrire QU'UNE FOIS -> plus rien ne le maintient mort, et il ressuscite pour de
        //     bon. Pire que le va-et-vient.
        // La bonne regle n'est ni « toujours » ni « une fois » : c'est **reconcilier quand l'etat
        // local contredit le serveur**. Or seul redscript peut lire cet etat local. Le C++ se
        // contente donc de transmettre, et `AppliquerSanteJoueur` compare avant d'ecrire.
        bool ok = false;
        if (!Red::CallVirtual(this, "AppliquerSanteJoueur", ok,
                static_cast<uint32_t>(msg->health()))
            || !ok)
        {
            SDK->logger->WarnF(PLUGIN, "AppliquerSanteJoueur refuse (%u/1000)",
                static_cast<unsigned>(msg->health()));
        }
        return;
    }

    // Sante d'un VOISIN. On ne s'en sert que pour coucher son avatar quand il tombe a zero : sa
    // barre de vie a lui n'est affichee nulle part chez nous, et l'avatar est rendu immortel
    // localement (AvatarNeutre.reds) precisement pour que ce soit le serveur qui tranche.
    if (msg->health() != 0)
    {
        return;
    }
    const auto entite = m_networkedEntitiesLookup.find(msg->id());
    if (entite == m_networkedEntitiesLookup.end())
    {
        // Pas spawne chez nous (hors de portee au moment du coup). Rien a coucher — et rien a
        // rattraper : s'il revient en vue, il reviendra vivant, ce qui est un ecart connu et
        // borne tant que le serveur ne porte pas l'etat de mort dans le Snapshot.
        SDK->logger->InfoF(PLUGIN, "Mort de %llu ignoree : avatar pas spawne localement", msg->id());
        return;
    }
    bool ok = false;
    if (!Red::CallVirtual(this, "TesseraRendreMort", ok, entite->second) || !ok)
    {
        // `false` veut souvent dire « pas encore » : `Kill` peut etre differe d'une frame, et le
        // pantin peut n'etre pas encore attache. Le prochain HealthSync ne viendra pas (le serveur
        // n'emet que sur changement), donc on journalise pour que le cas se voie.
        SDK->logger->WarnF(PLUGIN, "TesseraRendreMort refuse pour %llu", msg->id());
    }
}

void NetworkGameSystem::SendAttackReport(uint64_t target, uint32_t degats)
{
    if (m_pInterface == nullptr || target == 0 || degats == 0)
    {
        return;
    }

    // kind=5=Attaque, `param` = les degats. Constantes du gel du schema (protocol.fbs,
    // EntityInteraction) — surtout pas une numerotation locale.
    constexpr uint8_t kKindAttaque = 5;

    SDK->logger->InfoF(PLUGIN, "Degats %u rapportes sur %llu", degats, target);

    flatbuffers::FlatBufferBuilder builder;
    const auto ei = cyberpunk_rp::protocol::CreateEntityInteraction(
        builder, target, kKindAttaque, degats);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_EntityInteraction, ei.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
}

bool NetworkGameSystem::Tessera_RapporterArme(uint64_t item, bool degainee)
{
    if (m_pInterface == nullptr)
    {
        return false;
    }

    // ⚠️ `slot` = 0 : le SERVEUR ne connait pas les slots du jeu et n'a pas a les connaitre. C'est
    // le client qui resout « le slot d'arme principal » a la reception. Coder un hash de slot en
    // dur ici ferait dependre le protocole d'un detail de version du jeu — exactement ce que la
    // separation des depots evite.
    //
    // ⚠️ `equipped = true` toujours : ce canal ne parle QUE de l'arme en main. Le desequipement
    // d'inventaire (poser une arme dans un coffre) est un autre sujet, et melanger les deux dans un
    // meme champ ferait qu'un jour l'un annulerait l'autre.
    SDK->logger->InfoF(PLUGIN, "Arme rapportee : item=%llu degainee=%s", item,
        degainee ? "oui" : "non");

    flatbuffers::FlatBufferBuilder builder;
    const auto er = cyberpunk_rp::protocol::CreateEquipmentReport(builder, item, 0, degainee, true);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_EquipmentReport, er.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    return true;
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

    // ── L'ARME EN MAIN, TRANSPORTEE PAR `garments` ────────────────────────────────────────────
    //
    // Le champ existait dans le schema depuis l'origine et n'avait jamais eu d'emetteur. Il porte
    // desormais l'arme tenue par le joueur que cet avatar represente : un seul `EquippedItem`.
    //
    // ⚠️ VECTEUR ABSENT = MAINS VIDES, et c'est un etat A PART ENTIERE, pas un « pas d'info ». Le
    // serveur retire le garment quand le joueur range son arme (`appearance_relay.rs`) : traiter
    // l'absence comme « on ne sait pas » laisserait l'arme dans les mains de l'avatar pour
    // toujours. On ecrit donc explicitement 0.
    appearance.arme = 0;
    if (sync->spec()->garments() != nullptr && sync->spec()->garments()->size() > 0)
    {
        const auto* premier = sync->spec()->garments()->Get(0);
        if (premier != nullptr && premier->drawn())
        {
            appearance.arme = premier->item();
        }
    }
    m_appearances[id] = appearance;

    SDK->logger->InfoF(PLUGIN, "AppearanceSync %llu : record=%llu apparence=%llu arme=%llu",
        id, appearance.baseRecord, appearance.appearance, appearance.arme);

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
    // ── 50 Hz, ALIGNÉ SUR LE TICK SERVEUR ──────────────────────────────────────────────────
    //
    // C'était 10 Hz. Le serveur tourne à 20 Hz et diffuse à 20 Hz : la moitié de ses snapshots
    // relayait donc une position qu'il connaissait déjà. Tout ce qui se passait entre deux envois
    // — un pas de côté, une rotation, un demi-tour — était perdu AVANT même d'atteindre le fil, et
    // aucun tampon d'interpolation ne peut restituer ce qui n'a jamais été échantillonné.
    //
    // C'est la cause de fond de ce que Lucas décrit le 2026-08-13 : « les micro-déplacements, les
    // rotations, ça crée du flou et ce n'est pas franc ». Ce sont précisément les mouvements les
    // plus courts, donc les premiers à disparaître d'un échantillonnage trop lâche.
    //
    // ⚠️ CET ORDRE-LÀ N'EST PAS NÉGOCIABLE : il fallait d'abord passer le canal en NON-FIABLE.
    // À 20 Hz sur un canal fiable, une perte aurait retenu deux fois plus de paquets, et le
    // plafond de 40 messages/s du Gateway (`rate_limit.rs`) aurait été mangé à moitié par la
    // position seule — alors que les rapports de statiques l'ont déjà saturé une fois
    // (F-PNJ-131). Non fiable d'abord, cadence ensuite.
    //
    // On ne monte PAS au-delà : émettre plus vite que le serveur ne diffuse n'ajouterait que du
    // trafic que personne ne lira. Cette période SUIT donc `default_tick_rate_hz()` côté serveur —
    // 0,02 s = 50 Hz depuis le 2026-08-13.
    static constexpr float kPeriodeEnvoiS = 0.02f;
    m_TimeSinceLastPlayerPositionSync += deltaTime;
    if (m_TimeSinceLastPlayerPositionSync < kPeriodeEnvoiS)
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

void NetworkGameSystem::RendreAvatarsDistants(const float deltaTime)
{
    if (!g_horlogeRendu.Amorcee())
    {
        return; // aucun snapshot recu : rien a rendre, et surtout rien a deviner.
    }
    g_horlogeRendu.Avancer(deltaTime);
    const double instant = g_horlogeRendu.TempsRendu();

    for (auto& [networkId, tampon] : g_tamponsJoueurs)
    {
        const auto entite = m_networkedEntitiesLookup.find(networkId);
        if (entite == m_networkedEntitiesLookup.end())
        {
            continue; // corps pas encore ne (ou deja detruit) : le tampon attend.
        }
        Tessera::Sync::PoseRendue pose;
        if (!tampon.Echantillonner(instant, pose))
        {
            continue;
        }
        PiloterAvatar(networkId, entite->second, pose, deltaTime);
    }
}

void NetworkGameSystem::PiloterAvatar(uint64_t networkId, RED4ext::ent::EntityID entityId,
                                      const Tessera::Sync::PoseRendue& pose, float deltaTime)
{
    // ── LA BOUCLE PROUVEE, ET SEULEMENT ELLE ───────────────────────────────────────────────
    //
    // Mesuree en jeu le 2026-07-23 (backlog Q6/Q6b, sondes `loco_active`/`loco_lag`/`loco_hybrid`,
    // enrichie dans F-PLY-008) : commande de marche CONTINUE pour l'ANIMATION + `Teleport` de
    // recalage pour la POSITION. « Le Teleport ne casse PAS l'anim tant que la commande de marche
    // tourne. » Le pantin est a la position autoritaire ET s'anime.
    //
    // ⚠️ Ce n'est PAS la boucle que suivaient les joueurs jusqu'ici. Ils passaient par
    // `SetEntityPose`, concu pour les PNJ : commande TERMINANTE
    // (`finishWhenDestinationReached = true`), `ignoreNavigation = false`, reemission au metre. Ce
    // reglage-la est date et justifie POUR LES PNJ — le serveur planifie leur trajet et on veut que
    // le moteur navigue, trottoirs et feux compris (F-PNJ-095). Il ne vaut pas pour un joueur : sa
    // position FAIT AUTORITE, on ne veut pas que le moteur lui recalcule un chemin autour d'un
    // obstacle, on le veut la ou le serveur le dit.
    const RED4ext::Vector4 positionVoulue = { pose.x, pose.y, pose.z, 1.0f };

    const auto entite = Cyberverse::Utils::GetDynamicEntity(entityId);
    if (!entite.has_value())
    {
        // ⚠️ CE SILENCE-LÀ EST PEUT-ÊTRE LE « délai beaucoup plus important » AU RETOUR DANS LE
        // CHAMP DE VISION, signalé par Lucas le 2026-08-13. On ne le sait pas : ce chemin sortait
        // sans rien dire, comme le recalage avant lui.
        //
        // L'hypothèse la mieux étayée — NON MESURÉE ICI, donc à traiter comme telle : le moteur
        // dé-instancie l'avatar hors du champ, et `GetDynamicEntity` échoue tant qu'il n'est pas
        // revenu. C'est la même famille que F-PNJ-088 (`FindEntityByID` rend nil sur un pantin
        // vivant) et que la sonde `aoi_ladder`, qui n'a vu une entité se résoudre qu'après 3 s
        // d'attente. Si c'est ça, le délai appartient au moteur et aucun réglage réseau ne le
        // réduira — il faudra une sonde dédiée.
        //
        // On compte, on journalise à cadence basse, et la prochaine session tranchera par le
        // chiffre au lieu de l'impression.
        auto& suivi = g_suiviAvatars[networkId];
        suivi.depuisLogS += deltaTime;
        ++g_statsRoster.avatarsIrresolus;
        if (suivi.depuisLogS >= 2.0f)
        {
            suivi.depuisLogS = 0.0f;
            SDK->logger->InfoF(PLUGIN,
                "[avatar %llu] entite IRRESOLUE (total %llu) — le moteur ne la rend pas",
                networkId, g_statsRoster.avatarsIrresolus);
        }
        return;
    }

    // ── FIL MUET : ON FIGE, ON NE LAISSE PAS COURIR ────────────────────────────────────────
    //
    // Observé par Lucas le 2026-08-13, plusieurs instances chargeant en meme temps : « il y a des
    // micro-coupures et le PNJ reprend la main sur le joueur ». L'avatar se met a marcher seul.
    //
    // Ce n'est pas le moteur qui reprend la main, c'est NOUS qui ne la lachons pas. Notre commande
    // de marche est CONTINUE et NON TERMINANTE — c'est ce qui produit une locomotion fluide. Quand
    // le fil se tait, rien ne la remplace : le moteur continue d'executer le dernier ordre recu,
    // c'est-a-dire de marcher vers un point de visee perime.
    //
    // ⚠️ Le seuil ne se confond PAS avec `pose.extrapolee`. Extrapoler 50 ms est NORMAL — le canal
    // est delibrement non fiable, un paquet se perd. Un fil muet depuis 400 ms ne l'est pas.
    // Figer sur `extrapolee` ferait clignoter tous les avatars a la moindre perte ; ne jamais figer
    // les envoie se promener.
    //
    // 400 ms = huit intervalles de snapshot a 20 Hz. Assez long pour qu'une perte ordinaire, meme
    // en rafale, ne declenche rien ; assez court pour qu'une micro-coupure ne devienne jamais une
    // promenade.
    //
    // Un avatar fige est un defaut VISIBLE et honnete : le joueur d'en face comprend que quelqu'un
    // a lague. Un avatar qui part en promenade est un defaut MENSONGER — il raconte une action que
    // personne n'a faite, et en RP c'est bien pire.
    static constexpr double kFilMuetS = 0.4;
    const auto tampon = g_tamponsJoueurs.find(networkId);
    if (tampon != g_tamponsJoueurs.end()
        && tampon->second.AgeDuDernierEchantillon(g_horlogeRendu.TempsRendu()) > kFilMuetS)
    {
        auto& suivi = g_suiviAvatars[networkId];
        if (suivi.commande)
        {
            bool fige = false;
            Red::CallVirtual(this, "TesseraFigerAvatar", fige, entityId);
            suivi.commande = false;
            ++g_statsRoster.avatarsFiges;
            SDK->logger->InfoF(PLUGIN, "[avatar %llu] fil muet — fige sur place", networkId);
        }
        return; // on ne corrige pas non plus la position : sans nouvelles, on n'invente rien.
    }

    // ── IMMOBILE : rien a animer ───────────────────────────────────────────────────────────
    //
    // Une commande de marche vers un point ou l'on est deja produit un pietinement. On place, et
    // c'est tout — c'est aussi ce que fait le chemin PNJ pour `locomotion == 0`.
    if (pose.locomotion == 0)
    {
        // ⚠️ IL FAUT ANNULER L'ORDRE, PAS SEULEMENT CESSER D'EN DONNER.
        //
        // Signalé par Lucas le 2026-08-13, et c'est le MÊME défaut que le fil muet sous une autre
        // forme : « quand on arrête de marcher, après quelques mètres le PNJ reprend ses droits et
        // se met à marcher tout seul ».
        //
        // Ici le fil va très bien : le joueur s'est simplement arrêté, `locomotion` passe à 0. On
        // posait alors `commande = false` — ce qui ne dit RIEN au moteur, c'est juste notre
        // comptabilité — et on replaçait l'avatar. Mais la commande de marche, CONTINUE et NON
        // TERMINANTE, tournait toujours dans le contrôleur d'IA : le pantin continuait vers son
        // dernier point de visée, à trois mètres devant. D'où un avatar qui marche encore alors
        // que le joueur est à l'arrêt.
        //
        // `commande = false` empêche de RÉÉMETTRE ; seul `TesseraFigerAvatar` ANNULE.
        auto& suivi = g_suiviAvatars[networkId];
        if (suivi.commande)
        {
            bool fige = false;
            Red::CallVirtual(this, "TesseraFigerAvatar", fige, entityId);
            suivi.commande = false;
        }
        suivi.derniereLocomotion = 0;
        SetEntityPosition(entityId, positionVoulue, pose.yaw);
        return;
    }

    // ── RECALAGE : la position qui fait foi reste celle du serveur ─────────────────────────
    //
    // Sans ce garde, un avatar qui a rate des snapshots marcherait indefiniment vers une cible
    // qu'il ne rattraperait jamais. La derive du suivi seul a ete MESUREE : elle croit d'environ
    // 2 m/s quand l'allure du pantin est inferieure a la vitesse de la cible (`loco_lag`). Le
    // recalage n'est donc pas un confort.
    //
    // Le seuil descend de 8 m a 2,5 m : a 8 m, un avatar pouvait etre a une demi-rue de sa vraie
    // position sans que rien ne le corrige. Ce seuil etait dimensionne pour une boucle qui prenait
    // du retard par construction ; celle-ci ne devrait pas en prendre.
    //
    // ponytail: seuil et cadence de reemission non calibres en jeu — ce sont les deux boutons a
    // tourner si l'avatar sautille (baisser la reemission) ou s'il traine (baisser le seuil).
    // ── L'ERREUR SE RÉSORBE, ELLE NE SE PURGE PAS D'UN COUP ────────────────────────────────
    //
    // Signalé par Lucas le 2026-08-13 : « on se téléporte encore de deux à trois mètres à chaque
    // fois ». Ces sauts-là ne sont pas un défaut du réseau : ils SONT le seuil de recalage, qui se
    // déclenchait en boucle.
    //
    // Le mécanisme, et il était inévitable par conception : le moteur marche à l'allure d'un
    // PALIER fixe (Walk/Run/Sprint) pendant que la cible avance à une vitesse CONTINUE. Les deux
    // ne peuvent pas coïncider. L'écart grandit donc jusqu'au seuil, on téléportait, il
    // regrandissait — un saut de 2,5 m toutes les quelques secondes, indéfiniment. C'est aussi ce
    // que disait la mesure : une dérive qui OSCILLE entre 0,29 et 2,02 m, jamais stable.
    //
    // On cesse d'attendre le seuil. À chaque passage, on résorbe une FRACTION de l'écart : la
    // correction devient continue et invisible au lieu d'être rare et brutale. C'est le
    // « position smoothing » classique, et c'est possible ici parce que `Teleport` préserve
    // l'animation (F-PLY-008) — l'avatar continue de marcher pendant qu'on le recale.
    //
    // Le saut FRANC ne disparaît pas pour autant : il reste pour les vraies discontinuités
    // (téléportation, ascenseur, retour d'AoI), au même seuil que le tampon utilise pour couper
    // son historique — les deux doivent parler de la même chose, sinon l'un lisse ce que l'autre
    // vient de déclarer discontinu.
    static constexpr float kSautFrancM = 15.0f;
    static constexpr float kCorrectionMiniM = 0.25f;
    // 0,15 par passage : à 60 fps l'écart est divisé par deux en ~70 ms. Assez vif pour que la
    // dérive ne s'installe pas, assez doux pour qu'aucun pas ne se voie.
    static constexpr float kFractionCorrection = 0.15f;

    const auto position = Cyberverse::Utils::Entity_GetWorldPosition(entite.value());
    const float dx = positionVoulue.X - position.X;
    const float dy = positionVoulue.Y - position.Y;
    const float dz = positionVoulue.Z - position.Z;
    const float derive = std::sqrt(dx * dx + dy * dy + dz * dz);

    // ⚠️ LA VERTICALE APPARTIENT AU MOTEUR, PAS À NOUS.
    //
    // Signalé par Lucas le 2026-08-13, juste après l'arrivée de la résorption douce : « on voit le
    // personnage qui monte et qui descend ». C'est un artefact que J'AI créé, et le mécanisme est
    // une bagarre à deux : le moteur pose le pantin AU SOL à chaque frame (il est contraint par la
    // gravité et la géométrie), nous le poussons vers un Z interpolé qui n'est pas exactement le
    // sol, le moteur le re-pose, nous re-poussons. À 60 fps, ça se voit comme une oscillation
    // verticale — un personnage qui flotte et retombe sans arrêt.
    //
    // Sur l'horizontale il n'y a pas de bagarre : rien dans le moteur ne conteste un X/Y.
    //
    // On rend donc la verticale au moteur pour les petits écarts, et on ne la corrige que lorsque
    // l'écart est FRANC — un étage, un escalier, une passerelle — c'est-à-dire quand il n'est plus
    // explicable par le sol. 0,5 m : au-dessus d'une marche d'escalier et du bruit de terrain,
    // en dessous d'un demi-étage.
    static constexpr float kSeuilVerticalM = 0.5f;
    const float deriveHorizontale = std::sqrt(dx * dx + dy * dy);
    if (deriveHorizontale > kCorrectionMiniM && derive <= kSautFrancM)
    {
        // Résorption douce. On ne touche NI à la commande de marche (elle continue d'animer), ni
        // au yaw (l'orientation vient du moteur pendant qu'il marche ; l'imposer ici ferait
        // saccader le regard à chaque frame).
        const float corrigeZ = std::fabs(dz) > kSeuilVerticalM ? dz * kFractionCorrection : 0.0f;
        const RED4ext::Vector4 pas{
            position.X + dx * kFractionCorrection,
            position.Y + dy * kFractionCorrection,
            position.Z + corrigeZ,
            1.0f};
        // ⚠️ `PlacerSansCommande` et NON `SetEntityPosition` : ce dernier empile un ordre de
        // téléport qui annule la marche en cours. Voir le corps de `PlacerSansCommande`.
        PlacerSansCommande(entityId, pas, pose.yaw);
    }

    if (derive > kSautFrancM)
    {
        // ⚠️ CE CHEMIN EST LE PLUS IMPORTANT À JOURNALISER, et il ne l'était pas.
        //
        // Le 2026-08-13, Lucas a signalé « de grosses téléportations de plusieurs dizaines de
        // mètres ». Les compteurs ne montraient RIEN : la ligne de diagnostic vivait dans la
        // branche NOMINALE (celle qui commande la marche), jamais dans celle-ci. On journalisait
        // donc la santé et jamais la maladie — l'erreur d'instrument la plus banale, et la plus
        // coûteuse : les dérives lues plafonnaient à 1,68 m précisément parce que tout ce qui
        // dépassait 2,5 m sortait ici, en silence.
        SDK->logger->InfoF(PLUGIN, "[avatar %llu] RECALAGE derive=%.2fm allure=%u ech=%zu%s",
            networkId, derive, static_cast<unsigned>(pose.locomotion),
            g_tamponsJoueurs[networkId].Nombre(), pose.extrapolee ? " EXTRAPOLE" : "");
        ++g_statsRoster.recalagesAvatar;
        SetEntityPosition(entityId, positionVoulue, pose.yaw);
        // La commande survit au Teleport (c'est tout le resultat de Q6b) — mais la cible commandee
        // date d'avant le saut. On force une reemission au prochain passage.
        g_suiviAvatars[networkId].commande = false;
        return;
    }

    // ── VISER DEVANT, JAMAIS LA POSITION ───────────────────────────────────────────────────
    //
    // A 20 Hz, la position serveur n'est en avant que de la distance parcourue en un tick : 15 cm
    // a 3 m/s. Un pantin envoye a 15 cm y arrive instantanement et s'arrete. C'est la mesure du
    // 2026-08-06 (« ils fremissent sur place »), corrigee pour les PNJ par `NpcState.move_target`
    // et jamais portee aux joueurs, faute de champ equivalent dans `PlayerState`.
    //
    // On n'en ajoute pas : le tampon donne la VITESSE (derivee de deux echantillons), donc la
    // direction, donc un point de visee. Le serveur ne connait pas la destination d'un joueur ; le
    // client, lui, sait ou il va.
    static constexpr float kViseeM = 3.0f;
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    Tessera::Sync::PointDeVisee(pose, kViseeM, cx, cy, cz);

    // ── NE PAS REEMETTRE VINGT FOIS PAR SECONDE ────────────────────────────────────────────
    //
    // Le 2026-08-06, relancer un cheminement a chaque tick a fait TOMBER le jeu quelques minutes
    // apres le deploiement (156 PNJ x 20 ordres/s). La commande etant ici CONTINUE et NON
    // TERMINANTE, elle n'a pas besoin d'etre rejouee : on la rafraichit seulement quand le point de
    // visee a franchement bouge, ou apres un delai plafond. 4 Hz au pire, et pour une poignee de
    // joueurs — pas pour 156 pantins.
    static constexpr float kReemissionMaxS = 0.25f;
    static constexpr float kEcartVisee = 1.5f;
    auto& suivi = g_suiviAvatars[networkId];
    suivi.depuisS += deltaTime;
    suivi.depuisLogS += deltaTime;
    const float vx = cx - suivi.cibleX;
    const float vy = cy - suivi.cibleY;
    const float vz = cz - suivi.cibleZ;
    const bool cibleABouge = std::sqrt(vx * vx + vy * vy + vz * vz) > kEcartVisee;

    // ── L'ALLURE CHANGE : ON N'ATTEND PAS LE PROCHAIN CRÉNEAU ──────────────────────────────
    //
    // Signalé par Lucas le 2026-08-13 : « il y a un petit délai avant que ça se déclenche ».
    //
    // La cause était une cadence, pas une animation. La commande de marche n'était réémise qu'au
    // plus tous les 250 ms ou quand le point de visée avait bougé d'un mètre et demi. Un joueur qui
    // s'élance depuis l'arrêt pouvait donc attendre un quart de seconde AVANT que le premier ordre
    // ne parte — et l'animation ne commence qu'à cet ordre. S'ajoutaient les 100 ms du tampon : le
    // départ paraissait mou.
    //
    // Un changement d'allure est un ÉVÉNEMENT, pas une valeur qu'on échantillonne : il déclenche
    // immédiatement. On économise jusqu'à 250 ms sur le démarrage, l'arrêt et chaque changement de
    // rythme — sans toucher à la cadence de croisière, donc sans réintroduire le déluge d'ordres
    // qui a fait tomber le jeu le 2026-08-06.
    //
    // ⚠️ CE QUE ÇA NE FAIT PAS. Le reste du délai appartient au moteur : le temps que son graphe
    // d'animation fonde l'arrêt vers la marche. Le supprimer demanderait de piloter le graphe
    // directement (`AnimationControllerComponent::ApplyFeature`), ce qui exige de connaître le NOM
    // d'entrée du graphe pour la locomotion. Ce nom n'apparaît dans AUCUN script décompilé — c'est
    // de la donnée, dans le graphe. Le deviner serait le piège du symbole inventé (F-SCR-019).
    // Non mesuré, donc non fait : voir la sonde proposée au backlog.
    const bool allureAChange = suivi.commande && suivi.derniereLocomotion != pose.locomotion;
    suivi.derniereLocomotion = pose.locomotion;

    if (suivi.commande && !cibleABouge && !allureAChange && suivi.depuisS < kReemissionMaxS)
    {
        return; // il est deja en route, dans la bonne direction : on le laisse marcher.
    }

    const RED4ext::Vector4 visee = { cx, cy, cz, 1.0f };
    bool enRoute = false;
    if (Red::CallVirtual(this, "TesseraSuivreAvatar", enRoute, entityId, visee,
                         static_cast<int32_t>(pose.locomotion))
        && enRoute)
    {
        // ── L'INSTRUMENT ───────────────────────────────────────────────────────────────────
        //
        // Sans lui, la seule chose qu'une session puisse dire est « ça a l'air fluide » — et un
        // avatar qui traîne de deux mètres a exactement l'air d'un avatar qui va bien, vu de face.
        // Ces trois nombres tranchent ce que l'oeil ne tranche pas :
        //   derive     : l'écart à la position autoritaire. C'est LA mesure. S'il croît, l'allure
        //                commandée est trop lente (mesuré : ~2 m/s de creusement, sonde `loco_lag`).
        //   extrapole  : le tampon était à sec — un réseau qui hoquette, pas un défaut de rendu.
        //   ech        : profondeur du tampon. 0 ou 1 = on ne peut pas interpoler, on devine.
        //
        // Une ligne toutes les 2 s et par avatar : assez pour voir une tendance, trop peu pour
        // peser (le log de la session du 2026-08-10 fait déjà 3,7 Mo).
        //
        // ⚠️ Ne JAMAIS brider un instrument avant d'avoir mesuré : un plafond posé « au cas où » a
        // déjà fait passer une manipulation réelle pour un non-événement (protocole de sondage,
        // règle 10). 2 s est un choix d'échantillonnage, pas un plafond de sécurité.
        if (suivi.depuisLogS >= 2.0f)
        {
            suivi.depuisLogS = 0.0f;
            SDK->logger->InfoF(PLUGIN,
                "[avatar %llu] derive=%.2fm allure=%u ech=%zu%s", networkId, derive,
                static_cast<unsigned>(pose.locomotion), g_tamponsJoueurs[networkId].Nombre(),
                pose.extrapolee ? " EXTRAPOLE" : "");
        }

        suivi.cibleX = cx;
        suivi.cibleY = cy;
        suivi.cibleZ = cz;
        suivi.depuisS = 0.0f;
        suivi.commande = true;
        return;
    }

    // Commande refusee (entite pas encore prete, pas un `ScriptedPuppet`…) : on place, plutot que
    // de laisser l'avatar planté. Le prochain passage retentera la commande.
    suivi.commande = false;
    SetEntityPosition(entityId, positionVoulue, pose.yaw);
}

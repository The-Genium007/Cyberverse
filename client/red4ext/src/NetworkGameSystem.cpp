#include "NetworkGameSystem.h"

#include "TesseraEsthetiqueV.h"
#include "TesseraSpawnEnrichi.h"

// Pour la sonde `Tessera_LireTableAlias` (F-PLY-101) : releve ecrit dans un fichier, et tampon de
// lignes. Ajoutes explicitement plutot que supposes transitifs — une inclusion implicite qui
// disparait a une montee de dependance casse un build sans que le motif soit lisible.
#include <fstream>
#include <string>
#include <vector>

#include "PlayerSync/HorlogeServeur.h"
#include "PlayerSync/Robot.h"
#include "PlayerSync/Telemetrie.h"

/// Estimateur du décalage entre l'horloge de CETTE machine et celle du serveur.
///
/// ⚠️ Déclaré AVANT `g_telemetrie` : celle-ci en garde un pointeur, et l'ordre d'initialisation
/// des globales d'une même unité de traduction suit l'ordre de déclaration. L'inverse
/// laisserait `BrancherHorloge` pointer sur un objet pas encore construit.
///
/// Alimenté par `HandleSnapshot` (champ `Snapshot.ts_ms`), lu par la télémétrie pour dater chaque
/// ligne sur une base commune à tous les joueurs. Sans lui, cinquante journaux de playtest ne se
/// comparent pas — voir `HorlogeServeur.h`.
Tessera::Sync::HorlogeServeur g_horlogeServeur;

/// Journal de mesure — inerte tant que `--tessera-telemetrie` n'est pas passe (voir Telemetrie.h).
/// Déclaré ICI, tout en haut : il est utilisé dès le premier tick (démarrage), bien avant le bloc
/// d'état de rendu où vivent ses voisins logiques.
Tessera::Sync::Telemetrie g_telemetrie;

// --- Mode robot : l'etat du scenario, hors de la classe (cf. NetworkGameSystem.h) ---
bool g_robotActif = false;
/// Sonde T7 — voir `SondeAccroupiDemandee`. Inerte sans le drapeau.
bool g_sondeAccroupi = false;
bool g_robotOrigineConnue = false;
float g_robotOrigineX = 0.0f, g_robotOrigineY = 0.0f, g_robotOrigineZ = 0.0f;
std::chrono::steady_clock::time_point g_robotDebut;
int g_robotPhase = -1;

#include "CommandLine.h"
#include "Main.h"
#include "Utils.h"

#include "RED4ext/RTTISystem.hpp"
#include "RED4ext/Scripting/Natives/Generated/EulerAngles.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/TeleportationFacility.hpp"
// Asseoir les avatars distants sur les sieges (F-VEH-031, mesure en jeu le 2026-08-14).
#include "RED4ext/Scripting/Natives/Generated/game/MountEventData.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/WorkspotGameSystem.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/MountAIEvent.hpp"
// Sonde F-PLY-101 : les types DECLARES par le script pour la customisation. Un handle de type de
// base ne se lie pas et echoue en SILENCE (lecon de `SystemeWorkspot`, mesuree le 2026-08-14).
#include "RED4ext/Scripting/Natives/Generated/game/ui/ICharacterCustomizationState.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/ui/ICharacterCustomizationSystem.hpp"
// La classe CONCRETE, telle que le releve en jeu la nomme (F-PLY-103) :
// `gameuiCharacterCustomizationSystem`. Le handle se calque sur elle.
#include "RED4ext/Scripting/Natives/Generated/game/ui/CharacterCustomizationSystem.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/mounting/IMountingFacility.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/mounting/MountingRequest.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/mounting/UnmountingRequest.hpp"
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

// Pitch du REGARD : meme echelle que le yaw (65536 crans pour 360 deg) mais SIGNE, donc +-180 deg
// exprimables la ou la camera du jeu ne depasse pas +-80 deg. On SATURE au lieu de replier : un
// pitch qui wrappe retourne la tete a l'envers, un pitch sature la laisse au maximum plausible.
//
// Mesure cote serveur (quant.rs, 2026-08-15) : `QuantYaw(x) as int16_t` donne EXACTEMENT le meme
// motif binaire que ceci pour |x| <= 180, par le complement a deux. La fonction separee existe donc
// pour la SATURATION (hors plage) et pour la lisibilite du site d'appel, pas pour l'arithmetique.
static inline int16_t QuantPitch(float degrees)
{
    if (std::isnan(degrees))
    {
        return 0;
    }
    const float clamped = degrees < -180.0f ? -180.0f : (degrees > 180.0f ? 180.0f : degrees);
    const long q = std::lround(clamped * (65536.0f / 360.0f));
    return static_cast<int16_t>(q < -32768 ? -32768 : (q > 32767 ? 32767 : q));
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
    // Mémorisé pour l'en-tête de session du journal de playtest (voir `SendJoin`).
    m_serverAddress = host + ":" + std::to_string(port);
    // Mémorisés pour POUVOIR RECOMMENCER : sans ça, l'adresse n'existe que dans la ligne de
    // commande, relue une seule fois au premier tick.
    m_host = host;
    m_port = port;

    // ⚠️ LE GARDE PORTE SUR LA CONNEXION, PLUS SUR L'INTERFACE.
    //
    // Il testait `m_pInterface != nullptr`, ce qui rendait toute reconnexion impossible :
    // `SteamNetworkingSockets()` renvoie un singleton de processus, donc une fois la première
    // connexion faite, ce pointeur ne redevient JAMAIS nul — pas même quand la socket tombe. Le
    // second appel était refusé par un garde censé empêcher les connexions concurrentes, et qui
    // interdisait en fait les connexions successives.
    //
    // Ce qu'on veut vraiment interdire, c'est d'ouvrir une seconde connexion pendant qu'une
    // première est vivante. C'est exactement ce que dit `m_hConnection`.
    if (m_hConnection != k_HSteamNetConnection_Invalid)
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
        // Un essai qui échoue ICI (adresse injoignable, socket refusée) ne produira jamais de
        // callback de changement d'état — c'est donc le seul endroit d'où réarmer, sans quoi la
        // reconnexion s'arrêterait au premier échec dur.
        ArmerReconnexion();
        return false;
    }

    return true;
}

void NetworkGameSystem::ArmerReconnexion()
{
    if (m_host.empty() || m_port == 0)
    {
        // Jamais connecté (jeu lancé sans adresse serveur) : il n'y a rien à retrouver.
        return;
    }
    m_tentativesReconnexion += 1;
    // 1, 2, 4, 8, 16, plafonné à 30 s. Le plafond compte autant que la croissance : un joueur parti
    // déjeuner pendant une panne longue doit retrouver sa partie sans avoir à relancer le jeu.
    double recul = 1.0;
    for (int32_t i = 1; i < m_tentativesReconnexion && recul < 30.0; ++i)
    {
        recul *= 2.0;
    }
    m_reconnexionDansS = recul > 30.0 ? 30.0 : recul;
    SDK->logger->InfoF(PLUGIN, "[reconnexion] essai %d arme dans %.0f s (%s)",
                       m_tentativesReconnexion, m_reconnexionDansS, m_serverAddress.c_str());
}

void NetworkGameSystem::TenterReconnexion()
{
    if (m_host.empty() || m_port == 0)
    {
        return;
    }
    SDK->logger->InfoF(PLUGIN, "[reconnexion] essai %d — %s", m_tentativesReconnexion,
                       m_serverAddress.c_str());
    ConnectToServer(m_host, m_port);
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
        g_sondeAccroupi = SondeAccroupiDemandee(commandLine);
        if (g_sondeAccroupi)
        {
            SDK->logger->Info(PLUGIN, "[sonde] ACCROUPI — stanceState.state=Crouch poussé sur "
                                      "chaque avatar distant. Observation attendue : l'avatar "
                                      "d'en face est accroupi, ou il ne l'est pas.");
        }
        g_robotActif = RobotDemande(commandLine);
        if (g_robotActif)
        {
            SDK->logger->Info(PLUGIN, "[robot] MODE ROBOT — la position emise est un SCENARIO, "
                                      "pas celle du joueur");
        }
        if (TelemetrieDemandee(commandLine))
        {
            // ⚠️ DOSSIER À NOUS depuis le 2026-08-15, plus `red4ext/logs`.
            //
            // Le launcher doit pouvoir RAMASSER puis PURGER ce dossier après une session de
            // playtest. Le faire sur `red4ext/logs` emporterait les journaux du chargeur de mods —
            // c'est-à-dire précisément ce qu'on relit quand un joueur signale un plantage.
            // Un dossier qui n'appartient qu'à nous est la condition pour que la collecte
            // automatique soit sûre.
            g_telemetrie.Demarrer(Tessera::Sync::kDossierJournaux,
                                  static_cast<std::uint32_t>(GetCurrentProcessId()));
            // L'estimateur d'horloge serveur alimente le champ `ts` de chaque ligne. Branché ici,
            // une fois : sans lui le journal reste lisible localement mais ne se compare plus à
            // celui d'une autre machine — voir `HorlogeServeur.h`.
            g_telemetrie.BrancherHorloge(&g_horlogeServeur);
            SDK->logger->InfoF(PLUGIN, "[mesure] telemetrie %s (pid %u) -> %s",
                               g_telemetrie.Active() ? "ACTIVE" : "REFUSEE (fichier non ouvert)",
                               GetCurrentProcessId(),
                               g_telemetrie.CheminUtilise().empty()
                                   ? "(aucun chemin ouvert)"
                                   : g_telemetrie.CheminUtilise().c_str());
        }
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

    // ── LE RETOUR ─────────────────────────────────────────────────────────────────────────────
    // Décompte du recul armé par `ArmerReconnexion`. Placé APRÈS le garde d'interface (il n'y a
    // rien à reconnecter tant qu'on n'a jamais connecté) et AVANT `RunCallbacks`, pour que l'essai
    // parte dans la frame où il est décidé plutôt qu'à la suivante.
    if (m_reconnexionDansS > 0.0)
    {
        m_reconnexionDansS -= frame_info.deltaTime;
        if (m_reconnexionDansS <= 0.0)
        {
            m_reconnexionDansS = 0.0;
            TenterReconnexion();
        }
    }

    PollIncomingMessages();
    TrackPlayerPosition(frame_info.deltaTime);
    // `RendreAvatarsDistants` est parti en `PostBuckets` (voir `OnRegisterUpdates`) : l'appeler ici
    // le ferait tourner AVANT que les plateformes ne bougent, et rendrait le retard d'une frame
    // qu'on vient precisement de supprimer.

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

    // ── ⭐ LE RENDU DES AVATARS PASSE APRES LE MOUVEMENT DES PLATEFORMES ────────────────────
    //
    // L'ordre du moteur (`SystemUpdate.hpp`) est :
    //     FrameBegin → EntityUpdateState → Buckets (physique, ANIMATION — LA CABINE BOUGE ICI)
    //                → PostBuckets → CameraUpdate → … → PreRenderUpdate
    //
    // Tout notre travail reseau tournait en `FrameBegin`, donc AVANT que la cabine ne bouge. On
    // placait donc chaque passager d'apres la hauteur de plancher de la frame PRECEDENTE : un
    // retard d'une image, constant, qui vaut `vitesse × dt` — 4 cm a 2,5 m/s et 60 fps, et
    // proportionnellement plus quand la cabine va vite.
    //
    // ⚠️ ET CE RETARD EST EXACTEMENT CE QU'ON VOIT. Sur une capture prise en pleine descente, le
    // DECOR est net comme une lame (il est immobile par rapport a la camera, qui descend avec la
    // cabine) et l'AVATAR est mou. Il n'y a qu'une facon d'etre mou a cote d'un decor net : bouger
    // par rapport a la camera. Le « flou » n'est donc pas un effet de rendu — c'est du MOUVEMENT
    // RELATIF RESIDUEL, et le retard d'une frame en est la source permanente.
    //
    // On enregistre donc une SECONDE passe, en `PostBuckets` : la cabine y a deja bouge, on lit sa
    // hauteur presente, et le passager est place au bon endroit dans la frame ou il est rendu.
    // Le reseau, lui, reste en `FrameBegin` — il n'a aucune raison d'attendre la physique.
    aRegistrar->RegisterUpdate(RED4ext::UpdateTickGroup::PostBuckets, this, "TesseraPassagers",
        [this](RED4ext::FrameInfo &frame_info, RED4ext::JobQueue &) {
            this->RendreAvatarsDistants(frame_info.deltaTime);
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
        // La serie d'essais s'arrete ici. Le compteur repart de zero, donc le prochain incident
        // recommencera son recul a 1 s au lieu d'heriter des 30 s de la panne precedente.
        if (system->m_tentativesReconnexion > 0)
        {
            SDK->logger->InfoF(PLUGIN, "[reconnexion] retablie apres %d essai(s)",
                               system->m_tentativesReconnexion);
        }
        system->m_tentativesReconnexion = 0;
        system->m_reconnexionDansS = 0.0;
        // ⚠️ `SelectCharacter` n'est PAS rejoue ici. Le serveur vient de recevoir notre `Join` et
        // n'a pas encore repondu : il repondra par une `CharacterList`, et c'est LA que l'on se
        // rincarne (`HandleCharacterList`). Envoyer les deux d'affilee marcherait peut-etre, mais
        // ferait dependre la reprise d'un ordre de traitement qu'on ne controle pas.
    } else {
        auto* systeme = Red::GetGameSystem<NetworkGameSystem>();
        systeme->FullyConnected = false;

        // ── LA CONNEXION MORTE SE FERME, PUIS SE REARME ────────────────────────────────────────
        //
        // ⚠️ SEULS LES ETATS TERMINAUX SONT DES PANNES. `ClosedByPeer` (4) et
        // `ProblemDetectedLocally` (5), rien d'autre.
        //
        // ⚠️⚠️ CE `if` EST LA CORRECTION D'UN BUG QUI A EMPECHE TOUTE CONNEXION — mesure du
        // 2026-08-16, journal `cyberverse.red4ext-2026-08-16-13-45-45.log`. La premiere version
        // fermait la connexion pour TOUT etat different de `Connected`. Or l'etablissement passe
        // par `Connecting` (1), qui est un etat NORMAL et transitoire : on fermait donc la
        // connexion pendant qu'elle s'ouvrait, et la boucle etait parfaite —
        //
        //     Trying to connect -> Status Changed (1) -> [reconnexion] essai N arme
        //     -> Status Changed (0) "Application closed connection"   <- nous
        //     -> essai N+1 -> ... a l'infini, jamais connecte.
        //
        // Le symptome n'avait rien d'un bug de connexion : en jeu, RIEN ne s'affichait — ni monde
        // partage, ni ecran de panne. Il a fallu le journal du plugin pour voir que le client se
        // sabordait lui-meme, une fois par seconde.
        //
        // `None` (0) est aussi exclu : c'est l'etat que produit NOTRE PROPRE `CloseConnection`, et
        // le traiter comme une panne rearmerait une reconnexion apres chaque nettoyage.
        const bool estUnePanne =
            pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
            pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally;

        if (estUnePanne)
        {
            // GNS impose de fermer explicitement une connexion passee dans un de ces deux etats :
            // sans ce `CloseConnection`, le handle fuit et le socket reste reserve. C'est aussi ce
            // qui remet `m_hConnection` a `Invalid`, donc ce qui REOUVRE le garde de
            // `ConnectToServer` — les deux gestes sont le meme.
            if (systeme->m_pInterface != nullptr && pInfo->m_hConn != k_HSteamNetConnection_Invalid)
            {
                systeme->m_pInterface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
            }
            if (pInfo->m_hConn == systeme->m_hConnection)
            {
                systeme->m_hConnection = k_HSteamNetConnection_Invalid;
                systeme->ArmerReconnexion();
            }
        }
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
std::deque<EtatAscenseurRecu> g_ascenseursRecus;
int32_t g_ascenseursTotalRecus = 0;
/// Range une ligne pour le script, en BORNANT la file.
///
/// ⚠️ Quand la borne est atteinte on jette la PLUS ANCIENNE **et on le dit** : une file qui perd
/// des lignes en silence transforme un probleme visible (« il en manque ») en probleme invisible
/// (« le serveur n a rien envoye »). C est la meme discipline que partout ailleurs ici.
static void PousserLigneConsole(uint8_t niveau, std::string texte)
{
    ++g_lignesConsoleTotalRecues;
    if (g_lignesConsole.size() >= kMaxLignesConsole)
    {
        g_lignesConsole.pop_front();
        texte = "[...lignes perdues, console non lue...] " + texte;
    }
    g_lignesConsole.push_back(LigneConsole{ niveau, std::move(texte) });
}

std::deque<EtatAppareilRecu> g_appareilsRecus;
std::deque<LigneConsole> g_lignesConsole;
int32_t g_lignesConsoleTotalRecues = 0;
int32_t g_appareilsTotalRecus = 0;
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

// Habillage : au plus dix tentatives par tenue, cadencees a l'HORLOGE.
//
// ⚠️ `kDelaiAvantHabillage` n'est pas une precaution, c'est une mesure : un pantin qui vient de
// naitre n'a fini ni son inventaire ni ses slots d'attache, et les ordres y sont ACCEPTES SANS
// EFFET — le succes trompeur que D1 interdit de compter. `ArmeAvatar.reds` attend deux secondes
// pour cette raison depuis le 2026-08-10.
//
// La premiere version comptait des appels de `PiloterAvatar` en supposant 20 Hz. Il tourne
// beaucoup plus vite : les dix passes tenaient en 2,6 s, toutes DANS la fenetre morte. D'ou
// l'horloge — une cadence ne se deduit pas d'un compteur dont on ne connait pas la frequence.
constexpr std::uint32_t kPassesHabillage = 10;
constexpr auto kDelaiAvantHabillage = std::chrono::milliseconds(2000);
constexpr auto kIntervallePasseHabillage = std::chrono::milliseconds(1000);
bool g_localEtaitEnLair = false;

/// Dernier masque d'etats de locomotion releve sur la population NATIVE autour du
/// joueur. -2 = jamais releve (-1 est une valeur legitime : joueur injoignable).
static std::int32_t g_dernierMasqueLocoNatif = -2;
bool g_suspendreCommandes = false;
bool g_suspendreCorrections = false;
// ── ALLUME PAR DEFAUT DEPUIS LE 2026-08-21 ────────────────────────────────────────────────────
//
// Il valait `false`, et son SEUL appelant dans les deux depots etait un mod de SONDE. Autrement
// dit : le correctif de marche des avatars distants existait, il etait ecrit, il etait correct — et
// personne ne l'allumait en partie normale. Les avatars des autres joueurs ont donc glisse pendant
// des mois avec le remede a portee de main.
//
// TRANCHE EN JEU PAR A/B, meme session, meme avatar, meme trace rejouee, seul ce drapeau changeant :
//   eteint  -> « elle glisse »       allume -> « elle marche »   (verdicts de Lucas)
// Voir F-PLY-222 (le drapeau) et F-PLY-173 (le symptome, clos).
//
// ⚠️ UNE RESERVE, ET ELLE EST HONNETE. Le tir portait AUSSI `corrections off`, que le harnais
// impose pour la mesure (sinon la correction ramene l'avatar sur la position autoritaire et masque
// l'effet). La part de chacun des deux n'est donc PAS departagee, et la configuration de
// PRODUCTION — pilotage par entrees allume AVEC les corrections actives — n'a jamais tourne telle
// quelle. C'est le premier point a regarder si les avatars se remettent a glisser ou se mettent a
// tressauter : le suspect est l'interaction des deux, pas ce drapeau seul.
bool g_pilotageParEntrees = true;

bool NetworkGameSystem::Tessera_PilotageParEntrees(bool actif)
{
    g_pilotageParEntrees = actif;
    // ── LE VERDICT DOIT POUVOIR DIRE SI LE MODE ETAIT ACTIF ────────────────────────────────
    //
    // La sonde de determinisme du 2026-08-16 a rendu « 0,000 m » et conclu au succes, sur un
    // dispositif qui ne mesurait rien. Sa garde verifiait la mauvaise grandeur. La lecon generale :
    // un depouillement qui ne peut pas voir l'etat de l'interrupteur qu'il teste ne peut pas
    // distinguer « le correctif marche » de « le mode n'etait pas allume ».
    g_telemetrie.Evenement("pilotage_entrees", actif ? 1u : 0u, "");
    return g_pilotageParEntrees;
}

/// Applique `--tessera-spawn-enrichi` une seule fois, au premier passage.
///
/// ⚠️ Posé ICI et pas dans un constructeur : `g_actif` vit dans une autre unité de compilation et
/// l'ordre d'initialisation des globales entre unités n'est pas garanti par le C++. Un réglage posé
/// trop tôt serait écrasé par l'initialisation de la variable qu'il règle — panne classique,
/// silencieuse, et qui ne se reproduit pas d'un build à l'autre.
void NetworkGameSystem::TesseraAppliquerDrapeauxUneFois()
{
    if (m_drapeauxAppliques) { return; }
    m_drapeauxAppliques = true;
    if (SpawnEnrichiDemande(GetCommandLineA()))
    {
        Tessera::SpawnEnrichi::g_actif = true;
        SDK->logger->InfoF(PLUGIN, "spawn enrichi ALLUME par --tessera-spawn-enrichi");
    }
}

bool NetworkGameSystem::Tessera_SpawnEnrichi(bool actif)
{
    Tessera::SpawnEnrichi::g_actif = actif;
    g_telemetrie.Evenement("spawn_enrichi", actif ? 1u : 0u, "");
    SDK->logger->InfoF(PLUGIN, "spawn enrichi %s", actif ? "ALLUME" : "eteint");
    return Tessera::SpawnEnrichi::g_actif;
}

// ── SONDE F-PLY-101 · LA TABLE D'ALIAS FPP/TPP, EN LECTURE SEULE ────────────────────────────────
//
// Voir la declaration dans le .h pour le POURQUOI et le desassemblage. Resume : `isFPP` ne fait que
// choisir une colonne dans une table de remappage portee par l'etat de customisation.
//
//     table  = *(etat + 0xa0)                  entrees de 3 pointeurs
//     nombre = *(uint32_t*)(etat + 0xac)
//     colonne 1 = variante FPP   ·   colonne 2 = variante TPP
//
// ⚠️ TROIS PRECAUTIONS, chacune payee ailleurs dans ce depot.
//
// 1. AUCUNE ECRITURE. La sonde doit pouvoir tourner sans consequence : si l'interpretation des
//    colonnes est fausse, on veut le savoir par un releve, pas par un corps de joueur casse.
// 2. LES OFFSETS SONT VERSIONNES. Ils viennent d'un desassemblage de la v2.31 (ADR 0001 epingle
//    cette version). A une montee de version, ils sont FAUX et cette sonde lira n'importe quoi —
//    d'ou le controle de vraisemblance sur `nombre` avant toute dereference.
// 3. LE NOM DE L'ACCESSEUR N'EST PAS VERIFIABLE HORS DU JEU. On essaie plusieurs voies et on
//    JOURNALISE laquelle passe. Un `return` muet sur une voie ratee est le defaut qui a coute deux
//    jours sur ce chantier.
Red::CString NetworkGameSystem::Tessera_LireTableAlias()
{
    std::vector<std::string> lignes;
    auto dire = [&lignes](const std::string& s) { lignes.push_back(s); };

    auto* rtti = Red::CRTTISystem::Get();
    dire("=== TABLE D'ALIAS FPP/TPP (lecture seule, F-PLY-101) ===");
    if (rtti == nullptr)
    {
        dire("CRTTISystem::Get() rend nullptr — rien de lisible.");
    }

    // ── Obtenir l'etat de customisation SANS AUCUNE OFFSET DEVINEE ──────────────────────────────
    //
    // Le chemin est celui du script : `GameInstance.GetCharacterCustomizationSystem(gi)` puis
    // `GetState()` (declares dans `characterCreationMenu.script:96-105`). On passe par le patron
    // deja PROUVE dans ce fichier pour les systemes de jeu — `Red::CallStatic("ScriptGameInstance",
    // "GetWorkspotSystem", ...)` — plutot que d'inventer une resolution d'instance.
    //
    // ⚠️ Chaque etape journalise son echec. Un `return` muet ici rendrait « ETAT NON OBTENU » sans
    // dire LAQUELLE des deux etapes a lache, et c'est exactement le defaut de diagnostic qui a
    // coute deux jours sur ce chantier.
    void* etat = nullptr;
    {
        // ⚠️⚠️ LE TYPE DU PARAMETRE DE SORTIE SE CALQUE SUR LE TYPE **DECLARE**, jamais sur ce qu'on
        // croit compatible par heritage. La lecon est deja ecrite dans ce fichier, a `SystemeWorkspot`
        // (mesuree le 2026-08-14) : un `Handle<IGameSystem>` ne se lie PAS a un systeme declare deux
        // crans plus bas, et l'echec est SILENCIEUX — le handle rend nul, ce qui se lit comme
        // « systeme injoignable ».
        //
        // Ma premiere version declarait `Handle<IScriptable>` et a produit exactement ce faux
        // diagnostic. Deux causes ont ete eliminees par mesure avant que je relise le fichier que je
        // modifiais : le NOM (enumeration des statiques de `ScriptGameInstance` : 1 trouvee sur 119)
        // et l'INSTANCIATION (releve refait avec le miroir de l'appartement OUVERT, meme echec).
        // Il ne restait que le type. D3 du CLAUDE.md — on consulte avant d'agir, y compris son propre
        // code.
        // ── TROIS VOIES, ET ON JOURNALISE LAQUELLE PASSE ────────────────────────────────────────
        //
        // Mesure du 2026-08-17 (F-PLY-103) : le Lua CET atteint ce systeme sans peine par
        // `Game.GetCharacterCustomizationSystem()`, et `GetState()` y rend le VRAI V en session
        // NORMALE, sans aucun menu ouvert. Ma route C++ etait donc seule en cause — et mon
        // hypothese d'instanciation etait fausse (elle est refutee, pas confirmee).
        //
        // Le releve donne aussi la classe REELLE : `gameuiCharacterCustomizationSystem`, la
        // **concrete**, alors que j'avais type l'interface.
        //
        // Deux inconnues restaient, et on ne les tranche pas en choisissant : `Game.X()` de CET mappe
        // vers un GLOBAL, alors que le script declare une statique sur `GameInstance`
        // (`core/systems/gameInstance.script:87`) que le RTTI expose sous `ScriptGameInstance`. On
        // essaie donc les trois, et le releve NOMME la gagnante — au lieu d'un cycle de build de plus
        // par supposition.
        Red::Handle<RED4ext::game::ui::CharacterCustomizationSystem> systeme;
        {
            using Poignee = Red::Handle<RED4ext::game::ui::CharacterCustomizationSystem>;
            struct Voie { const char* etiquette; bool (*tenter)(Poignee&); };
            static const Voie voies[] = {
                {"CallGlobal(GetCharacterCustomizationSystem)",
                 [](Poignee& s) { return Red::CallGlobal("GetCharacterCustomizationSystem", s); }},
                {"CallStatic(ScriptGameInstance)",
                 [](Poignee& s) {
                     return Red::CallStatic("ScriptGameInstance", "GetCharacterCustomizationSystem", s);
                 }},
                {"CallStatic(GameInstance)",
                 [](Poignee& s) {
                     return Red::CallStatic("GameInstance", "GetCharacterCustomizationSystem", s);
                 }},
                // -- LES DEUX FORMES QUI PASSENT L'INSTANCE DE JEU ------------------------------
                //
                // Le releve precedent a rendu `appel=false` sur les trois formes ci-dessus : ce n'est
                // donc pas la liaison du parametre de SORTIE qui rate, c'est l'invocation. Or la
                // statique declare `self : GameInstance` (core/systems/gameInstance.script:87) et
                // aucune des trois ne le passait. `ScriptGameInstance` se construit avec un pointeur
                // nul par defaut et le moteur le resout — c'est la forme que le RTTI attend pour une
                // statique de GameInstance.
                {"CallStatic(ScriptGameInstance) + instance",
                 [](Poignee& s) {
                     RED4ext::ScriptGameInstance gi;
                     return Red::CallStatic("ScriptGameInstance", "GetCharacterCustomizationSystem",
                                            s, gi);
                 }},
                {"CallGlobal + instance",
                 [](Poignee& s) {
                     RED4ext::ScriptGameInstance gi;
                     return Red::CallGlobal("GetCharacterCustomizationSystem", s, gi);
                 }},
            };
            for (const auto& v : voies)
            {
                const bool ok = v.tenter(systeme);
                dire(std::string("  voie ") + v.etiquette + " : appel="
                     + (ok ? "true" : "false") + "  systeme=" + (systeme ? "NON NUL" : "nul"));
                if (ok && systeme) break;
            }
        }
        if (!systeme)
        {
            dire("GetCharacterCustomizationSystem : ECHEC sur les TROIS voies.");
            dire("L'instanciation n'est PAS en cause (F-PLY-103 : le Lua y accede en session");
            dire("normale) — c'est donc la liaison C++ du parametre de sortie qu'il faut revoir.");
            dire("");
            // ── ON ENUMERE AU LIEU DE DEVINER ───────────────────────────────────────────────────
            //
            // Deux causes produisent le meme echec, et il faut les separer : soit le NOM de la
            // statique est faux, soit le systeme n'est pas instancie. Un second nom devine ne
            // trancherait rien — donc on liste ce que le RTTI porte reellement.
            //
            // Le script declare `GameInstance.GetCharacterCustomizationSystem(self : GameInstance)`
            // (`core/systems/gameInstance.script:87`), et le patron qui marche dans ce fichier passe
            // par la classe `ScriptGameInstance`. Si le nom court ne resout pas, le vrai est dans
            // cette liste — sinon c'est bien l'instanciation qui manque, et le releve le dira par
            // l'absence d'echec de nom.
            dire("--- statiques de ScriptGameInstance contenant 'Customization' ---");
            if (rtti != nullptr)
            {
                auto* cls = rtti->GetClass("ScriptGameInstance");
                if (cls == nullptr)
                {
                    dire("  ScriptGameInstance ABSENTE du RTTI — le patron lui-meme est a revoir.");
                }
                else
                {
                    uint32_t vues = 0;
                    for (auto* fn : cls->staticFuncs)
                    {
                        if (fn == nullptr) continue;
                        const char* nom = fn->fullName.ToString();
                        if (nom == nullptr) continue;
                        if (std::string(nom).find("Customization") != std::string::npos)
                        {
                            dire(std::string("  ") + nom);
                            ++vues;
                        }
                    }
                    dire("  total trouve : " + std::to_string(vues)
                         // ⚠️ `DynArray::size` est une METHODE ici, pas un champ — sans les
                         // parentheses, MSVC rend C3867 (« utilisez '&' »), qui ressemble a un
                         // probleme de pointeur alors que c'est un appel oublie.
                         + "  (sur " + std::to_string(cls->staticFuncs.size()) + " statiques)");
                    if (vues == 0)
                    {
                        dire("  AUCUNE : la statique n'existe pas sous ce nom sur cette classe.");
                        dire("  Alors le systeme s'obtient autrement — et pas par un nom devine.");
                    }
                    else
                    {
                        dire("  Le nom existe donc. L'echec vient de l'INSTANCIATION, pas du nom :");
                        dire("  relancer cette sonde pendant qu'un menu de customisation est ouvert.");
                    }
                }
            }
        }
        else
        {
            dire("GetCharacterCustomizationSystem : OK");
            Red::Handle<RED4ext::game::ui::ICharacterCustomizationState> poigneeEtat;
            if (!Red::CallVirtual(systeme, "GetState", poigneeEtat) || !poigneeEtat)
            {
                dire("GetState : ECHEC (le systeme repond, mais aucun etat).");
                dire("Un etat nul signifie qu'aucune customisation n'est chargee pour cette session.");
            }
            else
            {
                etat = poigneeEtat.instance;
                dire("GetState : OK — etat obtenu, aucune offset devinee pour y arriver.");
            }
        }
    }

    // ── Lire la table, si et seulement si on tient un etat plausible ────────────────────────────
    if (etat != nullptr)
    {
        auto base = reinterpret_cast<uintptr_t>(etat);
        auto** table = *reinterpret_cast<void***>(base + 0xa0);
        uint32_t nombre = *reinterpret_cast<uint32_t*>(base + 0xac);
        dire("");
        dire("table = " + std::to_string(reinterpret_cast<uintptr_t>(table))
             + "  nombre = " + std::to_string(nombre));
        // CONTROLE DE VRAISEMBLANCE. Un `nombre` absurde signe des offsets perimes (montee de
        // version) : on refuse de dereferencer plutot que de lire de la memoire au hasard.
        if (table == nullptr || nombre == 0 || nombre > 4096)
        {
            dire(">>> INVRAISEMBLABLE — offsets probablement perimes pour cette version du jeu.");
            dire(">>> Ne rien conclure de la table. Reverifier 0xa0/0xac au desassemblage.");
        }
        else
        {
            dire("");
            dire("i   | nom demande            | colonne 1 (FPP)        | colonne 2 (TPP)");
            auto nomDe = [](void* v) -> std::string {
                Red::CName cn(reinterpret_cast<uint64_t>(v));
                const char* s = cn.ToString();
                return (s != nullptr && *s != '\0') ? std::string(s) : std::string("<non resolu>");
            };
            for (uint32_t i = 0; i < nombre; ++i)
            {
                void** e = table + (static_cast<size_t>(i) * 3);
                char tampon[256];
                std::snprintf(tampon, sizeof(tampon), "%-3u | %-22s | %-22s | %s", i,
                              nomDe(e[0]).c_str(), nomDe(e[1]).c_str(), nomDe(e[2]).c_str());
                dire(tampon);
            }
            dire("");
            dire(">>> LIRE AINSI : chercher les lignes de la section Head (groupe `FPP` / `TPP`) et");
            dire(">>> des bras (`holstered_*`). La colonne 2 nomme la variante qui nous manque.");
        }
    }
    else
    {
        dire("");
        dire("ETAT NON OBTENU — la table n'a pas ete lue. Ce n'est pas un echec de la piste :");
        dire("c'est l'accesseur d'instance qui manque, et le releve ci-dessus dit lequel chercher.");
    }

    // Le releve part dans un fichier : un resume de quelques lignes ne porterait pas une table.
    std::string chemin = "TesseraLogs\\alias-apparence.txt";
    if (std::ofstream f(chemin, std::ios::trunc); f.is_open())
    {
        for (const auto& l : lignes) f << l << '\n';
    }

    std::string resume = std::to_string(lignes.size()) + " lignes -> " + chemin;
    return Red::CString(resume.c_str());
}

/// Avatars distants ATTACHES a une plateforme par `BindToComponent` — donc portes par le MOTEUR,
/// et qu'il ne faut plus placer du tout.
///
/// ⭐ MESURE DU 2026-08-28 (F-ASC-057) : l'attache porte le corps parfaitement — X et Y verrouilles
/// au centimetre sur la cabine pendant 74 m de montee, sans un ecart, et le corps reste VISIBLE.
/// C'est structurellement superieur au collage par ecriture (F-ASC-055), dont le retard residuel
/// est proportionnel a 1/fps : le verdict de Lucas etait « moins bon » a 25 images/s qu'a 39, a
/// code identique.
///
/// ⚠️⚠️ ET CE N'EST PAS UNE OPTIMISATION, C'EST UNE OBLIGATION MECANIQUE. Une fois le corps
/// attache, sa transformee devient LOCALE au plancher. Continuer a lui ecrire des positions MONDE
/// les fait interpreter comme des offsets locaux, et la position se compose a chaque ecriture :
/// mesure le 2026-08-28, le corps part a des centaines de metres en quelques secondes, avec un
/// increment par ecriture egal a la hauteur de la cabine.
///
/// ⚠️ Le registre est en HASH d'EntityID, pas en identifiant reseau : c'est ce que le redscript
/// tient au moment ou il attache (`EntityID.GetHash`), et `m_networkedEntitiesLookup` donne
/// l'EntityID a l'autre bout. Un seul point de conversion, cote client.
static std::set<uint32_t> g_avatarsAttachesPlateforme;

/// La derniere pose MONDE voulue pour chaque avatar attache. Le redscript la relit pour en deduire
/// l'ecart local — c'est le petit terme du modele en repere relatif, celui qui porte le mouvement
/// du passager DANS la cabine.
static std::map<uint32_t, RED4ext::Vector4> g_poseVoulueAttachee;

/// L'allure annoncee pour chaque avatar attache. Sert a deux choses, et il faut les distinguer :
/// figer un passager IMMOBILE (sinon son pantin joue une animation de marche sur place), et
/// laisser libre celui qui MARCHE dans la cabine.
///
/// ⚠️ Un passager est classe « EN L'AIR » (6) ou « EN COURSE » (2), jamais 0 pendant un trajet
/// (F-ASC-047) : « le sol bouge sous lui, le moteur le declare en chute ». La valeur telle quelle
/// ne dit donc PAS s'il marche. C'est le deplacement REEL qui tranche, pas l'etiquette.
static std::map<uint32_t, uint8_t> g_allureAttachee;

/// Le joueur LOCAL est-il porte par une cabine ? Pose par le module ascenseurs, lu par
/// `ReadLocomotionPacked` pour cesser d'annoncer « en l'air ».
///
/// ⚠️ LE SENS DE LA DEPENDANCE EST DELIBERE : le mod ascenseurs PREVIENT le netcode, le netcode ne
/// connait pas le mod. L'inverse ferait dependre le coeur reseau d'un module optionnel.
static bool g_joueurLocalPorte = false;

uint8_t AllureAttachee(uint32_t cle)
{
    const auto it = g_allureAttachee.find(cle);
    return it == g_allureAttachee.end() ? 0 : it->second;
}

RED4ext::Vector4 PoseVoulueAttachee(uint32_t cle)
{
    const auto it = g_poseVoulueAttachee.find(cle);
    return it == g_poseVoulueAttachee.end() ? RED4ext::Vector4{} : it->second;
}

/// Marque un avatar comme PORTE par une plateforme (ou leve la marque). Appele par le redscript
/// au moment ou il attache/detache — c'est lui qui sait, nous ne pouvons pas le deviner.
///
/// Rend l'etat effectif, pas l'argument : un appelant qui verifie ne peut pas se faire mentir.
/// Ecrire l'ecart LOCAL d'un corps attache, dans sa representation de mouvement.
///
/// ⭐ POURQUOI CE NATIF EXISTE. `BindToComponent` reparente la transformee, mais la representation
/// de mouvement du corps contient encore une position MONDE (F-PLY-336) — desormais relue comme un
/// ecart LOCAL. Le corps part donc a des centaines de metres et disparait de l'ecran. Mesure du
/// 2026-08-28 : verdict de Lucas, « quand la cabine se met en route, les pantins disparaissent ».
///
/// L'ecriture d'un petit ecart local remet le corps a sa place. C'est la seule ecriture qui
/// atteigne un avatar distant (F-PLY-337), et le chemin est mesure : composant -> `+0x160`
/// (`activeEntry`) -> `+0x20` (position).
///
/// ⚠️ CE QUI REND CE NATIF ACCEPTABLE DANS UNE DLL LIVREE, et c'est delibere :
///   · le composant est fourni PAR LE REDSCRIPT — aucun parcours de composants a coups d'offsets
///     bruts ici, donc aucun risque de lire a cote sur une montee de version ;
///   · garde d'ECRITURE stricte : page engagee ET inscriptible, pas seulement lisible ;
///   · bornes de plausibilite : un ecart local depasse rarement la taille d'une cabine, jamais
///     mille metres. Une valeur aberrante est refusee au lieu d'etre ecrite ;
///   · rend `false` sans rien faire au moindre doute. « Accepte » n'est pas « execute », et un
///     appelant qui verifie ne doit pas pouvoir se faire mentir.
bool NetworkGameSystem::Tessera_EcrireOffsetLocal(const Red::Handle<RED4ext::IScriptable>& moveComponent,
                                                  float x, float y, float z)
{
    auto* comp = moveComponent.instance;
    if (comp == nullptr)
    {
        return false;
    }
    constexpr float kBorne = 1000.0f;
    if (!(x > -kBorne && x < kBorne && y > -kBorne && y < kBorne && z > -kBorne && z < kBorne))
    {
        return false;   // hors de toute cabine plausible : c'est une faute de calcul, pas une cible.
    }

    const auto champ = reinterpret_cast<std::uintptr_t>(comp) + 0x160;
    MEMORY_BASIC_INFORMATION mbi{};
    auto inscriptible = [&](std::uintptr_t ou, std::size_t taille)
    {
        if (VirtualQuery(reinterpret_cast<void*>(ou), &mbi, sizeof(mbi)) == 0
            || mbi.State != MEM_COMMIT)
        {
            return false;
        }
        constexpr DWORD kOk = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
        if ((mbi.Protect & kOk) == 0)
        {
            return false;
        }
        const auto fin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return ou + taille <= fin;
    };
    if (!inscriptible(champ, 8))
    {
        return false;
    }
    const auto entree = *reinterpret_cast<std::uint64_t*>(champ);
    if (entree == 0 || !inscriptible(static_cast<std::uintptr_t>(entree) + 0x20, 12))
    {
        return false;
    }

    auto* champs = reinterpret_cast<float*>(static_cast<std::uintptr_t>(entree) + 0x20);
    champs[0] = x;
    champs[1] = y;
    champs[2] = z;
    // On RELIT : une ecriture qui ne prend pas doit se voir ici, pas trois heures plus tard.
    return champs[0] == x && champs[1] == y && champs[2] == z;
}

bool NetworkGameSystem::Tessera_PoserJoueurLocalPorte(bool actif)
{
    g_joueurLocalPorte = actif;
    return g_joueurLocalPorte;
}

bool NetworkGameSystem::Tessera_JoueurLocalPorte()
{
    return g_joueurLocalPorte;
}

bool NetworkGameSystem::Tessera_AvatarPorteParPlateforme(uint32_t entiteHash, bool actif)
{
    if (entiteHash == 0)
    {
        return false;   // « pas d'entite » n'est pas « attache » — on ne marque rien.
    }
    if (actif)
    {
        g_avatarsAttachesPlateforme.insert(entiteHash);
    }
    else
    {
        g_avatarsAttachesPlateforme.erase(entiteHash);
    }
    return g_avatarsAttachesPlateforme.count(entiteHash) != 0;
}

bool NetworkGameSystem::Tessera_SuspendreCorrections(bool actif)
{
    g_suspendreCorrections = actif;
    return g_suspendreCorrections;
}

bool NetworkGameSystem::Tessera_SuspendreCommandes(bool actif)
{
    g_suspendreCommandes = actif;
    return g_suspendreCommandes;
}
/// Ecart entre la position DEMANDEE au dernier placement et celle relue dans la MEME
/// frame. Non nul = notre placement n'a pas pris (F-PLY-066, candidat 2).
static float g_ecartApresPose = -1.0f;
std::map<uint64_t, PoseEnAttente> g_posesEnAttente;
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

/// Guette le moment ou l'entite d'un avatar devient RESOLVABLE, et le dit une seule fois.
///
/// ⭐⭐ POURQUOI CE GUET EXISTE. La fiche posee au spawn ne testait la resolution qu'UNE FOIS, a
/// l'instant meme du spawn — et concluait « IRRESOLVABLE » pour toujours.
///
/// Or la decompilation montre que l'objet rendu par l'appel enrichi est un **ticket de spawn** (deux
/// constructeurs, `FUN_140661684` et `FUN_14065f018`, partagent sa vtable et ne remplissent que des
/// champs de requete), dont `+0x110` recoit l'EntityID **resultat**. Un identifiant peut donc etre
/// alloue AVANT que l'entite n'existe : le tester tout de suite ne dit rien de ce qu'il vaudra une
/// seconde plus tard.
///
/// ⚠️ C'est la meme faute que celle qui a coute six tirs ce soir, sous une forme de plus : mesurer
/// une fois et conclure pour toujours. Un « non » instantane sur un mecanisme ASYNCHRONE n'est pas
/// un non — c'est un « pas encore », et rien ne les distinguait.
void NetworkGameSystem::GuetterResolution(uint64_t networkId, RED4ext::ent::EntityID entityId,
                                          const Tessera::Sync::PoseRendue& pose)
{
    // ── ⚠️ REECRIT LE 2026-08-23 APRES UNE INONDATION DE JOURNAL ────────────────────────────────
    //
    // La version precedente deduisait son etat d'un booleen unique (`dit`). Resultat : la branche
    // « resolvable » se rejournalisait a CHAQUE tick — 19 000 lignes identiques — et la mesure de
    // derive, placee apres, n'etait jamais atteinte.
    //
    // C'est le douzieme instrument fautif de la journee, et le premier qui NUIT au lieu de
    // seulement mentir : un journal noye empeche de lire tout le reste.
    //
    // D'ou une machine a TROIS ETATS EXPLICITES, sans aucune deduction :
    //   Attente   -> on retente la resolution a chaque tick
    //   Mesure    -> resolue ; on echantillonne la derive, quelques fois seulement
    //   Fini      -> plus rien, jamais
    //
    // La regle de la journee, appliquee a l'instrument lui-meme : **un etat qui commande une
    // decision se DECLARE.** Un booleen qui doit signifier trois choses en signifie zero.
    enum class Etape { Attente, Mesure, Fini };
    struct Guet
    {
        Etape etape = Etape::Attente;
        std::chrono::steady_clock::time_point debut;
        std::chrono::steady_clock::time_point dernierEchantillon;
        std::uint32_t essais = 0;
        std::uint32_t derives = 0;
    };
    static std::map<uint64_t, Guet> s_guets;
    auto& g = s_guets[networkId];
    if (g.etape == Etape::Fini) { return; }

    const auto maintenant = std::chrono::steady_clock::now();
    if (g.debut == std::chrono::steady_clock::time_point{}) { g.debut = maintenant; }

    if (g.etape == Etape::Attente)
    {
        ++g.essais;
        const auto ecoule = std::chrono::duration_cast<std::chrono::milliseconds>(
                                maintenant - g.debut).count();
        const auto e = Cyberverse::Utils::GetDynamicEntity(entityId);
        if (e.has_value() && e->instance != nullptr)
        {
            const char* classe = (e->instance->GetType() != nullptr)
                                     ? e->instance->GetType()->name.ToString() : "<sans type>";
            Red::Handle<RED4ext::IScriptable> ia;
            const bool aIa = Red::CallVirtual(e.value(), "GetAIControllerComponent", ia)
                             && ia != nullptr;
            SDK->logger->InfoF(PLUGIN,
                               "[guet %llu] ⭐ RESOLVABLE apres %lld ms (%u essais) · classe=%s · IA=%s",
                               networkId, (long long)ecoule, g.essais, classe, aIa ? "OUI" : "NON");
            g.etape = Etape::Mesure;
            return;
        }
        // ⚠️ On abandonne EN LE DISANT : un guet muet ne se distingue pas d'un guet qui n'a jamais
        // tourne — c'est precisement le defaut que tout ce bloc existe pour ne plus commettre.
        if (ecoule > 15000)
        {
            SDK->logger->WarnF(PLUGIN,
                               "[guet %llu] entite %llu TOUJOURS irresolvable apres %lld ms et %u "
                               "essais — ce n'est pas un retard, c'est une absence",
                               networkId, entityId.hash, (long long)ecoule, g.essais);
            g.etape = Etape::Fini;
        }
        return;
    }

    // ── ETAPE « MESURE » : LES ORDRES PRENNENT-ILS EFFET ? ──────────────────────────────────────
    //
    // Le corps est resolvable, de la bonne espece, et il a une IA. Reste LA question suivante, qui
    // est differente : ce qu'on lui demande a-t-il un effet ? On compare donc, quatre fois sur huit
    // secondes, la position ou il EST a celle ou on le VEUT.
    //
    // Un ecart qui ne descend jamais dit que les ordres partent dans le vide — exactement le defaut
    // mesure sur `PlacerSansCommande` (F-PLY-073) : accepte, sans effet.
    const auto depuis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            maintenant - g.dernierEchantillon).count();
    if (g.dernierEchantillon != std::chrono::steady_clock::time_point{} && depuis < 2000) { return; }
    g.dernierEchantillon = maintenant;
    ++g.derives;

    const auto e = Cyberverse::Utils::GetDynamicEntity(entityId);
    if (e.has_value())
    {
        const auto p = Cyberverse::Utils::Entity_GetWorldPosition(e.value());
        const float dx = p.X - pose.x, dy = p.Y - pose.y, dz = p.Z - pose.z;
        SDK->logger->InfoF(PLUGIN,
                           "[derive %llu #%u] corps=(%.1f %.1f %.1f) voulu=(%.1f %.1f %.1f) "
                           "ecart=%.2f m",
                           networkId, g.derives, p.X, p.Y, p.Z, pose.x, pose.y, pose.z,
                           std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    if (g.derives >= 4) { g.etape = Etape::Fini; }
}

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

    // ⚠️ LE CAST N'EST PAS COSMETIQUE — SANS LUI, CET APPEL N'A JAMAIS RIEN FAIT (F-PLY-070).
    //
    // `GetDynamicEntity` rend un `Handle<Entity>` : un handle type sur la classe de BASE. Or la
    // signature RTTI est `Teleport(handle:gameObject, Vector4, EulerAngles)` — un handle type sur
    // `gameObject`, classe DERIVEE. Le marshalling refuse un handle de base la ou il attend un
    // derive, et il le refuse EN SILENCE : pas d'erreur, pas de valeur de retour, l'entite ne bouge
    // simplement pas.
    //
    // Prouve sur 3048 echantillons : la mediane de `libre / derive` valait 0,1489 pour une fraction
    // de correction de 0,1500 — c'est-a-dire que l'ecart au point vise egalait EXACTEMENT la
    // correction demandee, la signature d'une entite qui n'a pas bouge du tout.
    //
    // Le recalage franc (`SetEntityPosition`, au-dela de 15 m) passait par un `AITeleportCommand`,
    // un mecanisme different, et fonctionnait — ce qui a masque le defaut : le seul chemin de
    // placement qui marchait etait celui reserve aux cas extremes.
    const auto cible = Red::Cast<RED4ext::game::Object>(entity.value());
    if (!cible)
    {
        return;
    }
    Red::CallVirtual(teleportFacility, "Teleport", cible, worldPosition, angles);
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
        // CREUX B0 — on RETIENT au lieu de jeter.
        //
        // `CreateEntity` rend un id immediatement mais instancie en differe : l'entite existe
        // pour le serveur et pas encore pour le moteur. Jeter la position ici (ce que faisait
        // l'ancien code, avec un simple Warn) la laissait figee la ou elle etait nee, POUR
        // TOUJOURS — il n'y a aucune autre voie de rattrapage.
        //
        // La derniere pose voulue ecrase la precedente : c'est la bonne semantique, on veut la
        // position ACTUELLE quand l'entite arrivera, pas l'historique de celles qu'on a ratees.
        g_posesEnAttente[entityId.hash] = PoseEnAttente{ worldPosition, yaw };
    }
}

/// Rejoue les poses retenues pour les entites qui n'etaient pas encore resolvables.
///
/// Appelee une fois par snapshot : c'est la cadence a laquelle de nouvelles poses arrivent, donc
/// celle a laquelle une entite a une chance d'etre devenue resolvable. Une entite qui repond enfin
/// est positionnee puis retiree du tampon ; les autres restent pour le prochain tour.
///
/// Cout borne : le tampon ne contient que des entites en attente d'instanciation, jamais le roster
/// complet. En regime etabli il est vide, et la boucle ne coute rien.
void NetworkGameSystem::RejouerPosesEnAttente()
{
    if (g_posesEnAttente.empty())
    {
        return;
    }
    for (auto it = g_posesEnAttente.begin(); it != g_posesEnAttente.end();)
    {
        const RED4ext::ent::EntityID id{ it->first };
        if (Cyberverse::Utils::GetDynamicEntity(id).has_value())
        {
            const auto pose = it->second;
            it = g_posesEnAttente.erase(it);
            // Apres l'erase : SetEntityPosition peut re-remplir le tampon si la resolution
            // echoue de nouveau entre-temps, et invalider l'iterateur qu'on tient.
            SetEntityPosition(id, pose.position, pose.yaw);
        }
        else
        {
            ++it;
        }
    }
}


void NetworkGameSystem::PollIncomingMessages()
{
    // ⚠️ RIEN A LIRE SUR UNE CONNEXION FERMEE — et surtout, ne pas INTERROGER un handle invalide.
    //
    // Depuis la reconnexion automatique (2026-08-16), `m_hConnection` retombe a `Invalid` des que
    // la connexion meurt. `ReceiveMessagesOnConnection` sur un handle invalide renvoie -1, et le
    // journal se remplissait alors de « Error polling messages: -1 » A CHAQUE FRAME pendant toute
    // la coupure — mesure du 2026-08-16, journal `cyberverse.red4ext-2026-08-16-15-01-05.log`.
    //
    // Le bruit n'est pas le seul probleme : ce message ressemble a une panne reseau alors qu'il ne
    // dit que « la connexion est fermee », ce que nous savons deja. Un journal qui crie a l'erreur
    // pendant un etat normal rend le diagnostic SUIVANT plus difficile, pas plus facile.
    if (m_hConnection == k_HSteamNetConnection_Invalid)
    {
        return;
    }
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
                case cyberpunk_rp::protocol::ServerMsg_CommandCatalog:
                    HandleCommandCatalog(env->msg_as_CommandCatalog());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_StaffMode:
                    HandleStaffMode(env->msg_as_StaffMode());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_ActionCatalog:
                    HandleActionCatalog(env->msg_as_ActionCatalog());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_IdentitesConnues:
                    HandleIdentitesConnues(env->msg_as_IdentitesConnues());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_InventaireAutoritaire:
                    HandleInventaireAutoritaire(env->msg_as_InventaireAutoritaire());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_DeviceStateMsg:
                    HandleDeviceState(env->msg_as_DeviceStateMsg());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_ElevatorStateMsg:
                    HandleElevatorState(env->msg_as_ElevatorStateMsg());
                    break;
                case cyberpunk_rp::protocol::ServerMsg_InteractionOpen:
                    HandleInteractionOpen(env->msg_as_InteractionOpen());
                    break;
                // ⭐ LA CONSOLE — cable le 2026-08-30. Avant, ces deux messages tombaient dans
                // le `default` juste en dessous : le serveur repondait, le client JETAIT.
                case cyberpunk_rp::protocol::ServerMsg_CommandResult:
                {
                    const auto* r = env->msg_as_CommandResult();
                    if (r)
                    {
                        // 1 = succes, 3 = erreur. Le niveau part avec le texte : sans lui, la
                        // console ne pourrait pas distinguer un refus d une confirmation.
                        PousserLigneConsole(r->success() ? 1 : 3,
                            r->message() ? r->message()->str() : std::string());
                    }
                    break;
                }
                case cyberpunk_rp::protocol::ServerMsg_ConsoleLine:
                {
                    const auto* l = env->msg_as_ConsoleLine();
                    if (l)
                    {
                        PousserLigneConsole(l->level(), l->text() ? l->text()->str() : std::string());
                    }
                    break;
                }
                default:
                    // Reste non câblé : CommandResult, PermissionSync,
                    // QueueStatus, InteractionOpen, InteractionResult. Le serveur les émet déjà —
                    // les brancher est le chantier « autorité totale », étapes 2 et 6. Journalisé
                    // au lieu d'être jeté en silence : un message serveur ignoré sans trace est
                    // exactement ce qui a fait croire pendant des semaines que le protocole
                    // n'était pas implémenté.
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

    // ── L'EN-TÊTE DE SESSION, ÉCRIT UNE FOIS, ET SANS LEQUEL LE RESTE EST ANONYME ──────────
    //
    // C'est ici, au Join, que toutes les informations d'identité sont réunies au même endroit :
    // qui, contre quel serveur, avec quelle version de protocole. Plus tard elles sont dispersées.
    //
    // Ce que ça achète, et c'est ce qui sépare un CORPUS d'un TAS : le launcher va ramasser une
    // cinquantaine de fichiers après une soirée de playtest. Sans cette ligne, aucun ne dit de qui
    // il vient — donc on ne peut ni l'apparier au journal serveur, ni écarter les sessions d'un
    // build périmé, ni même savoir si deux fichiers viennent du même joueur relancé deux fois.
    //
    // La « version » est l'horodatage de COMPILATION du plugin. C'est gratuit (le compilateur le
    // fournit), ça ne peut pas se désynchroniser d'un fichier de version qu'on oublierait de
    // bumper, et ça répond à la seule question qu'on se posera vraiment en dépouillant :
    // « est-ce que cette session vient du build que je crois ? »
    g_telemetrie.Session(displayName.c_str(), m_serverAddress.c_str(), __DATE__ " " __TIME__,
                         kTesseraProtocolVersion);

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

void NetworkGameSystem::SendPositionUpdate(float x, float y, float z, float yaw,
                                          int locomotionForcee)
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
    const auto locomotion = locomotionForcee >= 0
                                ? static_cast<uint8_t>(locomotionForcee)
                                : static_cast<uint8_t>(packedLocomotion & 0xFF);
    const auto moveDir = static_cast<uint8_t>((packedLocomotion >> 8) & 0xFF);

    // Le REGARD (spec 2026-08-15 §5.1) — `lookState.lookDir` de gameMuppetState. Lu en redscript
    // pour la meme raison que la locomotion : l'API camera y est accessible et la conversion
    // vecteur->angles y vit deja (cf. `ReadLookYaw`/`ReadLookPitch`). La QUANTIZATION reste ici,
    // parce qu'un seul module porte les constantes du fil (miroir de quant.rs).
    //
    // Un appel qui echoue laisse 0/0 — soit exactement « aucun regard rapporte », que le
    // consommateur traduit par « regarde droit devant, tete a l'horizontale ». Degrader vers le
    // comportement d'avant ce changement, jamais vers un regard faux.
    float lookYawDeg = 0.0f;
    float lookPitchDeg = 0.0f;
    if (!Red::CallVirtual(this, "ReadLookYaw", lookYawDeg))
    {
        lookYawDeg = 0.0f;
    }
    if (!Red::CallVirtual(this, "ReadLookPitch", lookPitchDeg))
    {
        lookPitchDeg = 0.0f;
    }

    // La pose EXACTE qui part sur le fil, horodatée sur l'horloge murale — c'est la moitié
    // « émission » de la mesure de latence bout-en-bout (voir Telemetrie.h). Journalisée APRÈS la
    // lecture du regard, pour que `yaw` et `lyaw` de la même ligne décrivent le même instant :
    // c'est leur COMPARAISON qui tranche la convention d'axes non mesurée.
    g_telemetrie.Emission(x, y, z, yaw, locomotion, lookYawDeg, lookPitchDeg, moveDir);

    // -- LE DECOLLAGE PART COMME UN EVENEMENT, PAS SEULEMENT COMME UN ETAT ------------------
    //
    // L'allure voyage dans `PositionUpdate`, ECHANTILLONNEE : le serveur diffuse a 25 Hz, donc un
    // observateur apprend le saut jusqu'a 40 ms apres son debut, plus le delai du tampon
    // d'interpolation. Pour un geste qui dure 800 ms, c'est un sixieme du geste perdu avant meme
    // qu'il ne commence a s'afficher.
    //
    // L'etat continu et l'evenement discret ont des exigences OPPOSEES : l'un tolere la perte et
    // veut la fraîcheur, l'autre exige l'ordre et la livraison. Les faire voyager ensemble, c'est
    // choisir les exigences du premier pour les deux.
    //
    // ⚠️ LE CANAL EXISTAIT DEJA AUX DEUX TIERS, ET PERSONNE NE S'EN SERVAIT. `PlayerActionReport`
    // est dans `protocol.fbs`, le serveur le relaie aux voisins d'AoI en `PlayerEvent kind=0` —
    // avec trois tests dedies (`player_action_report_relays_player_event_to_aoi_neighbor` et
    // suivants) — et le client, lui, n'en emettait AUCUN et ignorait tous les `kind` autres que le
    // stimulus. Le tiers manquant etait entierement de ce cote.
    //
    // Le franchissement de `!onGround` suffit a dater le decollage : `locomotion == 6` est
    // exactement ce que `ReadLocomotionPacked` met a 6 quand `IsOnGround` est faux, et cette
    // lecture est MESUREE (F-PLY-008, table complete des huit etats).
    static constexpr std::uint8_t kLocoEnLair = 6;
    static constexpr std::uint8_t kActionSaut = 0;   // eACTION_JUMP (WorldPacketsServerBound.h)
    const bool enLairMaintenant = locomotion == kLocoEnLair;
    if (enLairMaintenant && !g_localEtaitEnLair)
    {
        flatbuffers::FlatBufferBuilder bAction(128);
        const auto rapport = cyberpunk_rp::protocol::CreatePlayerActionReport(
            bAction, kActionSaut, /*param=*/0u);
        const auto envAction = cyberpunk_rp::protocol::CreateClientEnvelope(
            bAction, cyberpunk_rp::protocol::ClientMsg_PlayerActionReport, rapport.Union());
        bAction.Finish(envAction);
        // FIABLE, et c'est tout l'interet : un evenement perdu n'existe pas, contrairement a une
        // pose perdue que la suivante remplace. C'est la ligne de partage entre les deux canaux —
        // et la raison pour laquelle la pose juste en dessous part, elle, en `Unreliable`.
        //
        // ⚠️ `SendMessageToConnection`, jamais `SendMessage` : ce dernier est une MACRO Win32
        // (`SendMessageA`) et le compilateur ne dit pas « fonction inconnue », il dit « ne prend
        // pas 3 arguments » — une erreur qui envoie chercher une signature au lieu d'un nom.
        m_pInterface->SendMessageToConnection(
            m_hConnection, bAction.GetBufferPointer(), bAction.GetSize(),
            k_nSteamNetworkingSend_Reliable, nullptr);
        g_telemetrie.Evenement("action_emise", 0, "saut");
    }
    g_localEtaitEnLair = enLairMaintenant;

    const cyberpunk_rp::protocol::QVec3 pos(QuantPos(x), QuantPos(y), QuantPos(z));
    const auto pu = cyberpunk_rp::protocol::CreatePositionUpdate(
        builder, &pos, QuantYaw(yaw), locomotion, moveDir, /*flags=*/0,
        /*frame=*/0, /*slot=*/0, QuantYaw(lookYawDeg), QuantPitch(lookPitchDeg));
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

// ── ASSEOIR LES AVATARS DISTANTS SUR LES SIEGES ────────────────────────────────────────────────
//
// MESURE le 2026-08-14 (F-VEH-031, sonde `seat_mount`, Lucas au volant) : `MountingFacility.Mount`
// assied reellement un pantin, ET le moteur le PORTE ensuite — « un PNJ est assis et il est synchro
// quand je roule ». C'est ce second point qui dicte la forme de ce code.
//
// ⚠️ CONSEQUENCE : ON NE REPLIQUE PAS LA POSITION D'UN OCCUPANT. Le mounting natif attache le corps
// au repere du vehicule ; continuer a le teleporter sur la pose serveur le ferait lutter contre le
// moteur a chaque frame — exactement le « fantome qui glisse » deja observe sur les vehicules
// pilotes. On paie donc UN evenement de transition par occupant, jamais un flux continu.
//
// Etat des montages DEJA APPLIQUES, par avatar distant. Global, jamais membre : `NetworkGameSystem`
// est alloue par le moteur (cf. en-tete), un membre de plus corrompt la memoire voisine — mesure le
// 2026-08-06.
/// Ce que le SERVEUR veut pour un occupant. Recu en entier a chaque snapshot — ce n'est donc pas un
/// miroir du jeu, seulement le dernier ordre connu.
struct MontageVoulu
{
    uint64_t vehicule;
    uint8_t siege;
    bool operator==(const MontageVoulu& a) const
    {
        return vehicule == a.vehicule && siege == a.siege;
    }
};

/// Ce que le JEU observe, lu quand on en a besoin et JAMAIS memorise.
struct EtatObserve
{
    bool attache;
    RED4ext::ent::EntityID parent;
    RED4ext::CName slot;
};

/// Identifiant reseau du vehicule ou le joueur LOCAL est assis (0 = aucun). Pose par
/// `RapporterMontage`, verifie periodiquement contre le jeu, lu par `HandleSnapshot` pour ne PAS
/// placer cette voiture-la : celui qui conduit la possede (F-VEH-033). Global parce que l'en-tete
/// interdit d'ajouter un membre a `NetworkGameSystem`, alloue par le moteur.
static uint64_t g_vehiculeLocalMonte = 0;

/// Avatars distants OBSERVES assis quelque part. Alimente par la reconciliation, jamais par un
/// evenement — et ce n'est PAS une autorite : c'est un cache de RENDU, consulte a chaque frame
/// pour ne pas piloter un corps que le moteur porte deja. S'il se trompe, la reconciliation le
/// corrige au passage suivant ; la seule consequence est une frame de pilotage en trop.
static std::set<uint64_t> g_avatarsAssis;


/// Avatars distants PORTES par une cabine d'ascenseur. Meme nature que `g_avatarsAssis` : un cache
/// de RENDU, pas une autorite. Alimente par `PlayerState.frame` (ADR 0039), le seul champ qui dise
/// « ce joueur-la est dans la cabine X ».
///
/// ⚠️ Ce champ voyageait depuis le 2026-08-24 et n'etait lu NULLE PART cote client — meme trou que
/// `sustained` (F-PLY-303).
///
/// ⚠️⚠️ ET L'HYPOTHESE QU'IL A D'ABORD SERVIE ETAIT FAUSSE. On a cru que le moteur PORTAIT le corps
/// distant (« le personnage est bien attache »), et qu'il suffisait de lui laisser la verticale.
/// Mesure du 2026-08-25, en cessant de corriger : l'avatar N'EST PAS porte. Il reste sur place
/// pendant que la cabine part, se retrouve « de l'autre cote, dans le vide », puis disparait. Ce
/// qui le raccrochait, c'etait la correction reseau elle-meme — mal, par a-coups, mais c'etait
/// elle. « Attache » decrivait un corps rattrape vingt fois par seconde, pas un corps porte.
///
/// D'ou l'ANCRAGE : la position voulue devient « la cabine, MAINTENANT, plus l'ecart releve a
/// l'embarquement ». Le retard du fil disparait par construction — la cabine est locale, on lit sa
/// position presente — au lieu d'etre corrige apres coup.
namespace Tessera
{
/// ⛔ L'ANCRAGE VERTICAL D'UN PASSAGER DE PLATEFORME — DESACTIVE (Lucas, 2026-08-27).
///
/// Une seule constante garde les DEUX moities du mecanisme : le calcul de la hauteur
/// (`positionVoulue.Z = plancher + dz`) et le CAS PARTICULIER de placement d'un passager. Les
/// separer serait un piege : desactiver l'un sans l'autre laisse un demi-mecanisme actif, et un
/// demi-mecanisme est plus difficile a diagnostiquer qu'un mecanisme entier.
///
/// Pourquoi c'est a `false` : voir le pave a l'endroit du calcul. En un mot — la branche passager
/// appelait `PlacerSansCommande`, dont ce meme fichier avait DEJA prouve qu'elle n'applique rien
/// (F-PLY-070, 3048 echantillons). L'avatar n'etait donc jamais place pendant le trajet.
///
/// A `false`, un passager est un avatar distant comme un autre : sa position vient du fil, la
/// physique du jeu fait le reste. Moins juste dans une cabine en mouvement, et ASSUME.
inline constexpr bool kAncrageVerticalActif = true;
// ⭐⭐ RALLUME LE 2026-08-31, ET LA CONDITION QUI L'AVAIT ETEINT EST LEVEE — MESUREE.
//
// Il avait ete eteint parce que son ENTREE sautait d'un etage a l'autre : « aucune valeur
// intermediaire, l'avatar reste a une altitude fixe pendant que la cabine descend autour de lui ».
// Le mecanisme n'a jamais ete en cause, seulement ce qu'on lui donnait a manger.
//
// L'entree a ete refaite depuis (interpolation de trajet cote redscript, et surtout lecture de
// l'altitude du JOUEUR LOCAL quand il partage la cabine — que le moteur porte parfaitement,
// F-ASC-001). MESURE du 2026-08-31 sur un trajet reel de la gaine du megabatiment :
//
//     cabine=2707237757  publications=785  distinctes=512  amplitude=91,84 m
//
// 65 % des publications portent une valeur NEUVE. En marches, on aurait vu deux ou trois valeurs
// distinctes sur 785. Les 35 % restants sont les battements a l'arret, ou la valeur ne change pas.
// L'amplitude de 91,84 m atteste qu'on a bien mesure un TRAJET et pas un stationnement.
//
// ⚠️ ET C'EST LA SUITE DIRECTE DE F-ASC-074 : le parentage etait la cause du tremblement et des
// genoux (mesure par suppression). Le remplacer par une correction de la position VISEE laisse le
// netcode piloter l'animation comme pour tout autre avatar — c'est le seul chemin qui porte le
// passager SANS orpheliner son animation.
//
// ⚠️ Cette constante garde DEUX choses, et la seconde compte autant : le calcul de hauteur
// ci-dessous, et le retrait de l'AMORTISSEMENT pour les passagers (voir `passager`, plus bas).
// L'amortissement avait ete pose pour masquer le flou ; il le FABRIQUAIT. Les deux vont ensemble.
}   // namespace Tessera

struct AncrageCabine
{
    /// `EntityID` de la cabine porteuse (`PlayerState.frame`).
    uint64_t cabine = 0;
    /// L'avatar est-il MONTE sur la cabine ? Tant qu'il l'est, on ne le pilote plus du tout —
    /// c'est le moteur qui le porte, exactement comme un passager de vehicule (F-VEH-031).
    bool monte = false;
    /// Le montage a-t-il ete tente et REFUSE ? On ne le retente pas soixante fois par seconde ; on
    /// retombe sur le placement, qui marche mais tremble.
    bool montageRefuse = false;
    /// Le nom d'emplacement retenu, pour le demontage.
    RED4ext::CName emplacement;
    /// ⭐⭐ LE DERNIER PLANCHER CONNU — pour que l'echec d'une lecture ne fasse jamais SAUTER le corps.
    ///
    /// ⚠️ MESURE DU 2026-08-31. `HauteurCabineA` peut rendre `false` en pleine descente : deux
    /// publications tombees dans la meme image donnent `duree = 0`, et la fonction abandonne. Tout
    /// le bloc d'ancrage etait alors SAUTE, et la position retombait sur celle du fil — perimee de
    /// plus d'une seconde, donc plusieurs metres plus haut. Une image sur deux, le corps sautait.
    ///
    /// Releve apres le verrou anti-scintillement, sur une descente ou la vraie position ne fait
    /// que BAISSER : 101.557 -> 116.672 (+15 m), 56.236 -> 70.001 (+14 m), 39.212 -> 51.408 (+12 m).
    /// Le verrou avait supprime les longs gels ; ces trois sauts-la venaient d'ailleurs.
    ///
    /// ⭐ LA REGLE : un repli sur une valeur PERIMEE est pire que pas de repli du tout. On garde
    /// donc le dernier plancher connu et on continue d'ancrer dessus. Le corps derive alors au
    /// pire de quelques centimetres pendant l'image manquante, au lieu de sauter de quinze metres.
    float plancherConnu = 0.0f;
    bool plancherVu = false;
    /// ── L'ERREUR VISUELLE, ET SA DECROISSANCE ──────────────────────────────────────────────
    /// On n'affiche jamais la cible brute : on affiche `cible + erreur`, et on fait DECROITRE
    /// l'erreur. C'est le patron d'Unreal sur ses proxys simules et de Gaffer On Games
    /// (« position error offset ») — il gomme le micro-bruit sans faire trainer les vraies
    /// corrections, parce que le facteur depend de l'amplitude.
    float errX = 0.0f;
    float errY = 0.0f;
    float errZ = 0.0f;
    bool errValide = false;
    /// Ecart VERTICAL entre l'avatar et le plancher de la cabine. Releve tant que la cabine est a
    /// l'arret, fige pendant le trajet — c'est la grandeur « quasi constante » du patron relatif.
    float dz = 0.0f;
    bool ecartConnu = false;
};

/// Avatars distants portes par une cabine, et leur ancrage. Repond aussi a « cette cabine est-elle
/// occupee ? » — la notion PARTAGEE d'occupation qui manque au moteur (`Tessera_CabineOccupee`).
static std::map<uint64_t, AncrageCabine> g_porteurParAvatar;

/// Voir la declaration dans l'en-tete : le natif qui l'expose est une methode inline, et la table
/// ci-dessus est un statique de fichier.
uint64_t CabineDeAvatarReseau(uint64_t networkId)
{
    const auto porte = g_porteurParAvatar.find(networkId);
    return porte == g_porteurParAvatar.end() ? 0 : porte->second.cabine;
}

/// ── LA HAUTEUR VIVANTE DU PLANCHER, PUBLIEE PAR REDSCRIPT ──────────────────────────────────
///
/// Deux releves suffisent : la cabine suit une rampe (courbe `cosine`, quasi lineaire sur 50 ms),
/// donc extrapoler entre deux points laisse une erreur de l'ordre du centimetre. On garde le
/// PRECEDENT et le DERNIER, rien de plus.
struct HauteurCabineSuivie
{
    /// Poignee sur le composant porteur. CONSERVEE mais PLUS LUE EN C++ : `GetLocalToWorld` par
    /// `CallVirtual` rendait une transformee VIDE en se disant reussie (voir `HauteurCabineA`).
    Red::Handle<RED4ext::IScriptable> composant;
    /// La hauteur qui fait foi : publiee par redscript a 20 Hz, extrapolee lineairement ici.
    double tAvant = 0.0;
    double tDernier = 0.0;
    float zAvant = 0.0f;
    float zDernier = 0.0f;
    bool deuxPoints = false;
    /// ⭐⭐ INSTANT DU DERNIER MOUVEMENT AVERE — le verrou anti-scintillement.
    ///
    /// ⚠️ MESURE DU 2026-08-31, ET ELLE EXPLIQUE LE DEFAUT QUE LUCAS VOIT DEPUIS DEUX JOURS.
    /// `enMouvement` se decidait sur la pente entre les DEUX DERNIERS echantillons. Or deux
    /// echantillons identiques arrivent souvent : la hauteur publiee vient de l'altitude du
    /// joueur local, qui n'avance pas a chaque battement de 50 ms. La pente vaut alors zero, la
    /// cabine est declaree A L'ARRET, et l'ancrage RECAPTURE son ecart de reference — donc la
    /// position affichee redevient celle du fil, en retard. Au battement suivant la pente est
    /// non nulle, l'ecart est garde, et la position redevient ancree.
    ///
    /// Resultat : deux valeurs ALTERNENT, a plusieurs dizaines de metres d'ecart. Releve pendant
    /// une descente reelle, ou la vraie position ne fait que BAISSER :
    ///     119.105 (x5) -> 117.196 -> 115.848 -> 104.769 (x4) -> 63.687 -> 52.435 -> 42.883
    ///     -> 72.466 (+30 m !) -> 54.962 -> 48.947 -> 51.086 (+2 m) -> 49.450 -> ...
    /// Une serie NON MONOTONE sur un trajet monotone : ce n'est pas un retard, ce sont deux
    /// ecrivains qui se relaient. C'est le « il remonte » signale par Lucas.
    ///
    /// Le remede est un VERROU, pas un seuil : une cabine qui a bouge est declaree en mouvement
    /// pendant encore un demi-seconde. Un echantillon plat ne peut plus, a lui seul, faire croire
    /// a un arret et reinitialiser la reference.
    double tDernierMouvement = 0.0;
    /// Derniere hauteur rendue, pour savoir si la cabine bouge sans rien demander de plus.
    float zPrecedent = 0.0f;
    bool zPrecedentValide = false;
    /// ⚠️ L'INSTANT DE CE RELEVE. Sans lui, le test « bouge-t-elle ? » est FAUX des qu'il y a plus
    /// d'un passager — voir le pave dans `HauteurCabineA`.
    double instantZPrecedent = -1.0;
    /// Dernier etat de mouvement decide, rendu a tous les passagers de la meme frame.
    bool enMouvement = false;
};
static std::map<uint64_t, HauteurCabineSuivie> g_hauteurCabine;

/// Horloge LOCALE monotone, en secondes, avancee par `RendreAvatarsDistants`.
///
/// Volontairement distincte de `g_horlogeRendu` : celle-la vit sur la timeline SERVEUR et n'avance
/// qu'au rythme des snapshots. La hauteur d'une cabine, elle, est une grandeur purement locale —
/// la melanger a une horloge reseau reintroduirait le retard qu'on cherche precisement a supprimer.
static double g_tempsLocalS = 0.0;

void PoserHauteurCabine(uint64_t cabineHash, float z, double instant)
{
    if (cabineHash == 0)
    {
        return;
    }
    auto& h = g_hauteurCabine[cabineHash];

    // TRACE D'ENTREE. Redscript lit 119.111 sur le composant (verifie au harnais) et le C++ voyait
    // zero : la rupture est quelque part sur ce fil-la. On regarde ce qui ARRIVE, avant tout calcul.
    static double s_dernierLog = -1000.0;
    if (instant - s_dernierLog > 1.0)
    {
        s_dernierLog = instant;
        SDK->logger->InfoF(PLUGIN, "[hauteur cabine %llu] RECU z=%.3f a t=%.3f", cabineHash,
                           static_cast<double>(z), instant);
    }

    if (h.tDernier > 0.0 && instant <= h.tDernier)
    {
        return; // meme instant : rien de neuf, et ecraser confondrait les deux points.
    }
    h.tAvant = h.tDernier;
    h.zAvant = h.zDernier;
    h.tDernier = instant;
    h.zDernier = z;
    h.deuxPoints = h.tAvant > 0.0;
}

void PoserPlancherCabine(uint64_t cabineHash, const Red::Handle<RED4ext::IScriptable>& composant)
{
    if (cabineHash == 0)
    {
        return;
    }
    g_hauteurCabine[cabineHash].composant = composant;
}

bool HauteurCabineA(uint64_t cabineHash, double instant, float& sortie, bool& enMouvement)
{
    const auto it = g_hauteurCabine.find(cabineHash);
    if (it == g_hauteurCabine.end())
    {
        return false;
    }
    auto& h = it->second;

    // ⚠️⚠️⚠️ LA « VOIE DIRECTE » A ETE RETIREE : ELLE RENDAIT ZERO, EN SE DISANT REUSSIE.
    //
    // Elle lisait le composant `movingPlatform` en C++ :
    //     RED4ext::WorldTransform t = {};
    //     if (Red::CallVirtual(h.composant, "GetLocalToWorld", t)) { ... t.Position ... }
    //
    // MESURE DU 2026-08-26, trois nombres cote a cote pendant un trajet reel :
    //     plancher=-0.000 (lu=1 bouge=0) dz=119.097 cible=119.097 reel=119.097 ecart=0.000
    //
    // `lu=1` — donc l'appel s'est dit REUSSI. Et `plancher = 0`. La transformee revenait a sa
    // valeur d'initialisation, et personne ne le signalait. Consequence en cascade :
    //   · `dz` absorbait la hauteur ABSOLUE (119,097 au lieu d'un ecart au plancher) ;
    //   · `cible = 0 + 119.097` devenait une CONSTANTE ;
    //   · l'avatar etait donc epingle a une altitude fixe, et ne suivait la cabine EN RIEN.
    //
    // C'est exactement ce que Lucas decrivait : « on passe sous le plancher PROGRESSIVEMENT », et
    // le sens s'inverse entre montee et descente — parce que c'est la CABINE qui s'eloigne d'un
    // corps immobile, pas le corps qui derive.
    //
    // ⚠️ ET C'EST LA TROISIEME FOIS DE LA JOURNEE QUE LE MEME PIEGE MORD : `Red::CallVirtual` rend
    // `true` quand l'appel est DISPATCHE, pas quand il a produit un resultat. Lucas l'avait attrape
    // sur le montage (« t'es sur que le mount est bien applique ? ») ; je ne l'ai pas cherche ici.
    // Le tour de plus : ici la valeur de repli (zero) est PLAUSIBLE pour une coordonnee, donc rien
    // ne detonne — alors qu'un `false` aurait saute aux yeux.
    //
    // `ecart = 0.000` dit la seconde moitie, et elle est bonne : le PLACEMENT est parfait. On
    // posait le corps exactement ou on le demandait. On le demandait au mauvais endroit.
    //
    // On garde donc la seule voie qui produit de vrais nombres : la hauteur publiee par redscript,
    // ou `GetLocalToWorld().GetTranslation().Z` est evalue par le moteur lui-meme.
    // ── REPLI : la hauteur publiee a 20 Hz ─────────────────────────────────────────────────
    if (!h.deuxPoints)
    {
        return false;
    }
    const double duree = h.tDernier - h.tAvant;
    if (duree <= 0.0)
    {
        return false;
    }
    const double pente = (h.zDernier - h.zAvant) / duree;   // m/s

    // ⭐ VERROU ANTI-SCINTILLEMENT (voir `tDernierMouvement`). Un seul echantillon plat ne doit
    // pas pouvoir declarer la cabine a l'arret : la hauteur publiee vient de l'altitude du joueur
    // local, qui se repete d'un battement a l'autre. Sans ce verrou, l'ancrage relache sa
    // reference une image sur deux et la position ALTERNE entre deux valeurs eloignees.
    //
    // ⚠️ 0,5 s est choisi pour couvrir largement un trou d'echantillonnage a 20 Hz sans survivre
    // a un arret reel — une cabine qui s'arrete redevient « immobile » en une demi-seconde, ce qui
    // est bien plus court que le temps qu'un joueur met a sortir. A ajuster par la MESURE si le
    // recalage a l'arrivee se voyait, jamais par raisonnement.
    static constexpr double kMemoireMouvementS = 0.5;
    if (std::fabs(pente) > 0.05)
    {
        h.tDernierMouvement = h.tDernier;
    }
    enMouvement = (instant - h.tDernierMouvement) < kMemoireMouvementS;

    // ⚠️ EXTRAPOLATION BORNEE. Si redscript cesse de publier (cabine de-streamee, tick mort), on ne
    // prolonge pas la rampe indefiniment — ce serait envoyer l'avatar sous la carte. Au-dela de
    // trois periodes de publication, on rend la derniere hauteur connue telle quelle.
    static constexpr double kProlongementMaxS = 0.15;
    double depuis = instant - h.tDernier;
    if (depuis < 0.0) { depuis = 0.0; }
    if (depuis > kProlongementMaxS) { depuis = kProlongementMaxS; }
    sortie = h.zDernier + static_cast<float>(pente * depuis);
    return true;
}

bool NetworkGameSystem::Tessera_PoserHauteurCabine(RED4ext::ent::EntityID cabine, float z)
{
    if (!cabine.IsDefined())
    {
        return false;
    }
    PoserHauteurCabine(cabine.hash, z, g_tempsLocalS);
    return true;
}

bool NetworkGameSystem::Tessera_PoserPlancherCabine(RED4ext::ent::EntityID cabine,
                                                   const Red::Handle<RED4ext::IScriptable>& plancher)
{
    if (!cabine.IsDefined() || !plancher)
    {
        return false;
    }
    PoserPlancherCabine(cabine.hash, plancher);
    return true;
}

// Declarations avancees : ces deux aides sont definies plus bas (chemin vehicule), et le montage
// de cabine les reutilise telles quelles plutot que d'en dupliquer la logique.
static RED4ext::IScriptable* FacadeMontage();
static Red::Handle<RED4ext::game::MountEventData> ContexteMontage(RED4ext::ent::EntityID vehicule,
                                                                  RED4ext::CName siege,
                                                                  bool instantane);
static RED4ext::IScriptable* SystemeWorkspot();
static std::optional<Red::Handle<RED4ext::GameObject>> ObjetDe(RED4ext::ent::EntityID id);

static void DescendreDeCabine(RED4ext::ent::EntityID avatar, uint64_t cabineHash,
                              RED4ext::CName emplacement);

/// ── MONTER UN AVATAR SUR UNE CABINE ────────────────────────────────────────────────────────
///
/// ⭐ L'IDEE EST DE LUCAS (2026-08-26), et elle est juste : « si tu fais un mouvement de
/// deplacement plutot qu'une mise a jour x fois par seconde, il n'y a pas de flou ». Le moteur a
/// exactement ce mecanisme, et on l'utilise DEJA — c'est celui des passagers de vehicule
/// (F-VEH-031 : « il est synchro quand je roule », sans le moindre tremblement).
///
/// Un ordre UNIQUE attache le corps a son porteur ; ensuite c'est le moteur qui le deplace. Plus
/// aucune ecriture de position, donc plus rien a flouter, et un portage exact a n'importe quelle
/// vitesse — ce que le placement image par image ne pourra jamais donner.
///
/// `preservePositionAfterMounting = true` : on attache OU IL EST, on n'assoit personne.
///
/// ⚠️ NON MESURE AVANT CE JOUR : que la cabine ACCEPTE un montage. Elle porte bien un
/// `gameOccupantSlotComponent` (releve de composants du 2026-08-25), donc le mecanisme est prevu
/// cote donnees ; mais aucun nom d'emplacement n'est connu. On essaie donc, dans l'ordre,
/// l'emplacement VIDE (attache sans siege) puis quelques noms plausibles, et on JOURNALISE celui
/// qui passe. Un refus n'est pas une panne : on retombe sur le placement d'avant.
static bool MonterSurCabine(uint64_t avatarReseau, RED4ext::ent::EntityID avatar,
                            uint64_t cabineHash, RED4ext::CName& emplacementRetenu)
{
    auto* facility = FacadeMontage();
    if (facility == nullptr)
    {
        return false;
    }
    const RED4ext::ent::EntityID cabine{ cabineHash };

    // Aucun nom d'emplacement de cabine n'est connu. On essaie donc, et on JOURNALISE celui qui
    // passe les DEUX couches — c'est la sonde et le correctif dans le meme geste. `main_slot` et
    // `AppearanceSlot` viennent du relevé de composants réel de la cabine ; les noms de sièges
    // viennent du chemin véhicule, au cas où `OccupantSlots` partagerait sa convention.
    static const RED4ext::CName candidats[] = {
        RED4ext::CName(),                    // aucun siege : simple attache
        RED4ext::CName("main_slot"),
        RED4ext::CName("occupant_slot_0"),
        RED4ext::CName("seat_front_left"),
        RED4ext::CName("passenger"),
        RED4ext::CName("elevator_slot"),
    };

    for (const auto& nom : candidats)
    {
        auto requete = Red::MakeScriptedHandle<RED4ext::game::mounting::MountingRequest>();
        if (!requete)
        {
            return false;
        }
        requete->lowLevelMountingInfo.childId = avatar;
        requete->lowLevelMountingInfo.parentId = cabine;
        requete->lowLevelMountingInfo.slotId.id = nom;
        requete->preservePositionAfterMounting = true;
        requete->mountData = ContexteMontage(cabine, nom, /*instantane=*/true);

        if (!Red::CallVirtual(facility, "Mount", requete))
        {
            continue;
        }

        // ── COUCHE C — CELLE QUI REMET LE CORPS A L'ECRAN ──────────────────────────────────
        //
        // Le montage seul etablit l'occupation et REND LE CORPS INVISIBLE (mesure du 2026-08-26).
        // Le chemin vehicule le disait deja : « COUCHE C. Sans elle, l'occupation est correcte
        // partout SAUF a l'ecran. » Ce qui la fournit est `WorkspotSystem.MountToVehicle`, qui
        // vise un composant nomme `OccupantSlots` — et la cabine EN PORTE UN, exactement sous ce
        // nom (releve de composants, F-ASC-032). Le nom d'EMPLACEMENT dans ce composant, lui,
        // reste inconnu : on essaie, et on journalise celui qui prend.
        // ⚠️⚠️ TROIS ECHECS POSSIBLES, TROIS MESSAGES DIFFERENTS.
        //
        // La version precedente les confondait tous les trois en un seul « NON RENDU », et Lucas a
        // eu raison de ne pas y croire : `Red::CallVirtual` rend `true` si l'appel a ete DISPATCHE,
        // PAS si la methode a reussi — le chemin vehicule le dit dans son propre message (« Mount
        // REFUSE — la methode n'a pas ete trouvee »). Un `false` peut donc vouloir dire « le moteur
        // refuse » AUTANT que « je ne l'ai jamais appele ».
        //
        // Et il y avait pire : si la resolution de la cabine en `GameObject` echoue, la couche C
        // n'est meme pas tentee — et l'ancien code journalisait quand meme « NON RENDU ».
        // « Accepte ≠ execute » (D1), applique a mon propre instrument.
        bool corpsPlace = false;
        const char* pourquoiPas = "?";
        auto* workspot = SystemeWorkspot();
        if (workspot == nullptr)
        {
            pourquoiPas = "systeme workspot INJOIGNABLE";
        }
        else
        {
            const auto oCabine = ObjetDe(cabine);
            const auto oCorps = ObjetDe(avatar);
            if (!oCabine.has_value())
            {
                pourquoiPas = "la CABINE ne se resout pas en GameObject — couche C jamais appelee";
            }
            else if (!oCorps.has_value())
            {
                pourquoiPas = "le CORPS ne se resout pas en GameObject — couche C jamais appelee";
            }
            else
            {
                RED4ext::DynArray<RED4ext::ent::EntityID> aucunSync{};
                RED4ext::DynArray<RED4ext::CName> aucuneVar{};
                corpsPlace = Red::CallVirtual(workspot, "MountToVehicle", *oCabine, *oCorps,
                                              0.0f, 0.0f, RED4ext::CName("OccupantSlots"), nom,
                                              aucunSync, RED4ext::CName(), aucuneVar);
                pourquoiPas = corpsPlace
                    ? "appel DISPATCHE"
                    : "MountToVehicle non dispatche — signature ou methode absente, PAS un refus";
            }
        }

        if (corpsPlace)
        {
            emplacementRetenu = nom;
            SDK->logger->InfoF(PLUGIN,
                "[cabine %llu] avatar %llu : Mount ET couche C dispatches — emplacement '%s'. "
                "⚠️ 'dispatche' n'est pas 'a marche' : le verdict est a l'ecran.",
                cabineHash, avatarReseau, nom.ToString() ? nom.ToString() : "(vide)");
            return true;
        }

        // ⚠️ LE REPLI EST OBLIGATOIRE, ET IL EST LA PARTIE QUI COMPTE. Monte sans couche C, le
        // corps est INVISIBLE — bien pire que flou. On defait donc immediatement le montage plutot
        // que de laisser un essai rate a l'ecran. Un test qui echoue doit revenir a l'etat d'avant.
        DescendreDeCabine(avatar, cabineHash, nom);
        SDK->logger->InfoF(PLUGIN,
            "[cabine %llu] avatar %llu : emplacement '%s' — couche C KO : %s. Demonte, placement garde.",
            cabineHash, avatarReseau, nom.ToString() ? nom.ToString() : "(vide)", pourquoiPas);
    }

    SDK->logger->InfoF(PLUGIN,
        "[cabine %llu] avatar %llu : montage REFUSE sur tous les emplacements essayes — on garde le "
        "placement image par image (il porte, mais il tremble)", cabineHash, avatarReseau);
    return false;
}

/// Detache l'avatar de sa cabine. Appele quand le serveur cesse de le dire porte.
static void DescendreDeCabine(RED4ext::ent::EntityID avatar, uint64_t cabineHash,
                              RED4ext::CName emplacement)
{
    auto* facility = FacadeMontage();
    if (facility == nullptr)
    {
        return;
    }
    const RED4ext::ent::EntityID cabine{ cabineHash };
    auto requete = Red::MakeScriptedHandle<RED4ext::game::mounting::UnmountingRequest>();
    if (!requete)
    {
        return;
    }
    requete->lowLevelMountingInfo.childId = avatar;
    requete->lowLevelMountingInfo.parentId = cabine;
    requete->lowLevelMountingInfo.slotId.id = emplacement;
    requete->mountData = ContexteMontage(cabine, emplacement, true);
    Red::CallVirtual(facility, "Unmount", requete);
}

bool CabinePorteQuelquun(uint64_t cabineHash)
{
    if (cabineHash == 0)
    {
        return false;
    }
    for (const auto& [avatar, ancrage] : g_porteurParAvatar)
    {
        if (ancrage.cabine == cabineHash)
        {
            return true;
        }
    }
    return false;
}

/// Inverse exact de `IndexDeSiege` (PlayerActionTracker.cpp). Les deux tables DOIVENT rester
/// jumelles : le serveur ne transporte qu'un index, et un decalage assied les gens ailleurs.
static RED4ext::CName SiegeDeIndex(uint8_t index)
{
    switch (index)
    {
    case 0: return RED4ext::CName("seat_front_left");
    case 1: return RED4ext::CName("seat_front_right");
    case 2: return RED4ext::CName("seat_back_left");
    case 3: return RED4ext::CName("seat_back_right");
    default: return RED4ext::CName("seat_front_left");
    }
}

/// Journalise une fois toutes les 3 s par motif. Un `return` muet dans un chemin appele a 50 Hz est
/// un trou de diagnostic, pas une optimisation — la journee du 2026-08-14 l'a paye quatre fois.
static void JournalRalenti(const char* motif, const char* details)
{
    // Cle = l'ADRESSE du litteral : chaque site d'appel a la sienne, et on evite <string>.
    static std::map<const void*, std::chrono::steady_clock::time_point> s_dernier;
    const auto maintenant = std::chrono::steady_clock::now();
    auto& quand = s_dernier[static_cast<const void*>(motif)];
    if (maintenant - quand < std::chrono::seconds(3))
    {
        return;
    }
    quand = maintenant;
    SDK->logger->InfoF(PLUGIN, "[occupation] %s%s", motif, details);
}

/// COUCHE B — l'attache logique. Le type de sortie fait partie de l'appel : `IMountingFacility` est
/// a UN cran de `IGameSystem`, donc un `Handle<IGameSystem>` se lie (mesure le 2026-08-14 : un
/// `Handle<IScriptable>`, lui, ne se liait pas).
static RED4ext::IScriptable* FacadeMontage()
{
    Red::Handle<Red::IGameSystem> facility;
    if (Red::CallStatic("ScriptGameInstance", "GetMountingFacility", facility) && facility)
    {
        return facility.instance;
    }
    if (auto* direct = Red::GetGameSystem<RED4ext::game::mounting::IMountingFacility>())
    {
        return direct;
    }
    JournalRalenti("facade de montage INJOIGNABLE", "");
    return nullptr;
}

/// COUCHE C — le corps et l'animation. Le type DECLARE est `WorkspotGameSystem`, a DEUX crans de
/// `IGameSystem` (`WorkspotGameSystem` -> `IWorkspotGameSystem` -> `IGameSystem`), et un
/// `Handle<IGameSystem>` ne se lie PAS : `workspot INJOIGNABLE` quatre fois par minute, mesure le
/// 2026-08-14. Le parametre de sortie se calque sur le type DECLARE, jamais sur ce qu'on croit
/// compatible par heritage.
static RED4ext::IScriptable* SystemeWorkspot()
{
    Red::Handle<RED4ext::game::WorkspotGameSystem> systeme;
    if (Red::CallStatic("ScriptGameInstance", "GetWorkspotSystem", systeme) && systeme)
    {
        return systeme.instance;
    }
    // ⚠️ REPLI PAR RESOLUTION DIRECTE, et c'est lui qui marche (mesure du 2026-08-15).
    //
    // `CallStatic("ScriptGameInstance", "GetWorkspotSystem", ...)` a echoue avec CHAQUE type de
    // sortie essaye — `IGameSystem` puis `WorkspotGameSystem`, le type declare. La reconciliation
    // emettait donc parfaitement ses ordres (montage, changement de place, sortie : tous
    // journalises) et la couche C n'etait JAMAIS atteinte : `corps NON PLACE` a chaque fois.
    //
    // `GetGameSystem<T>` ne passe pas par la fonction statique du script : il resout le systeme par
    // son TYPE dans le registre du moteur. C'est le chemin qu'emploie `Utils::GetPlayer` pour
    // `PlayerSystem` depuis toujours, et il ne depend d'aucune signature de script.
    //
    // Lecon : quand un `CallStatic` refuse de se lier apres deux types de sortie corrects, ce n'est
    // pas le type qui est en cause — c'est la voie. Il faut en changer, pas la raffiner.
    if (auto* direct = Red::GetGameSystem<RED4ext::game::WorkspotGameSystem>())
    {
        return direct;
    }
    JournalRalenti("systeme workspot INJOIGNABLE", " — ni CallStatic ni GetGameSystem<T>");
    return nullptr;
}

static std::optional<Red::Handle<RED4ext::GameObject>> ObjetDe(RED4ext::ent::EntityID id)
{
    const auto entite = Cyberverse::Utils::GetDynamicEntity(id);
    if (!entite.has_value() || entite->instance == nullptr)
    {
        return {};
    }
    // Descente de type explicite : `Handle<Entity>` ne se convertit pas seul vers
    // `Handle<GameObject>` (assertion statique de RED4ext).
    return Red::ToHandle(static_cast<RED4ext::GameObject*>(entite->instance));
}

/// Lit la couche B. `false` = on n'a pas pu lire ; l'appelant ne doit alors RIEN faire — agir a
/// l'aveugle est ce qui produit les etats incoherents.
static bool LireEtatObserve(RED4ext::ent::EntityID avatar, EtatObserve& sortie)
{
    auto* facility = FacadeMontage();
    const auto objet = ObjetDe(avatar);
    if (facility == nullptr || !objet.has_value())
    {
        return false;
    }
    // Les TROIS parametres se passent, meme les optionnels : `optional` decrit ce que le SCRIPT
    // peut omettre, pas ce que l'appel RTTI peut omettre (mesure le 2026-08-14).
    RED4ext::game::mounting::MountingInfo reel{};
    Red::Handle<RED4ext::GameObject> aucunParent{};
    RED4ext::game::mounting::MountingSlotId aucunSlot{};
    if (!Red::CallVirtual(facility, "GetMountingInfoSingleWithObjects", reel, *objet, aucunParent,
                          aucunSlot))
    {
        JournalRalenti("lecture d'attache IMPOSSIBLE", "");
        return false;
    }
    sortie.attache = reel.parentId.hash != 0;
    sortie.parent = reel.parentId;
    sortie.slot = reel.slotId.id;
    return true;
}

/// Remplit le contexte que le chemin natif remplit toujours
/// (`gameVehicleMountableComponent.script:98-105`). Un `mountData` nul prive le moteur de ce qu'il
/// doit faire — c'est ce qui laissait un avatar assis dans une voiture qu'il avait quittee.
static Red::Handle<RED4ext::game::MountEventData> ContexteMontage(RED4ext::ent::EntityID vehicule,
                                                                  RED4ext::CName siege,
                                                                  bool instantane)
{
    auto donnees = Red::MakeScriptedHandle<RED4ext::game::MountEventData>();
    if (donnees)
    {
        donnees->slotName = siege;
        donnees->mountParentEntityId = vehicule;
        donnees->isInstant = instantane;
    }
    return donnees;
}

/// DEHORS -> ASSIS. Couche B puis couche C, dans cet ordre.
static void AsseoirAvatar(uint64_t avatarReseau, RED4ext::ent::EntityID vehicule,
                          RED4ext::ent::EntityID avatar, uint64_t vehiculeReseau, uint8_t siege,
                          bool instantane)
{
    auto* facility = FacadeMontage();
    if (facility == nullptr)
    {
        return;
    }

    // Le siege conducteur a son propre chemin dans le jeu : `PreHijackPrepareDriverSlot` est
    // appelee avant tout montage au volant (`gameVehicleMountableComponent.script`). L'ignorer se
    // payait en reessais — quatre tentatives mesurees le 2026-08-14.
    if (siege == 0)
    {
        const auto caisse = Cyberverse::Utils::GetDynamicEntity(vehicule);
        if (caisse.has_value() && caisse->instance != nullptr)
        {
            Red::CallVirtual(caisse->instance, "PreHijackPrepareDriverSlot");
        }
    }

    const RED4ext::CName nomSiege = SiegeDeIndex(siege);
    auto requete = Red::MakeScriptedHandle<RED4ext::game::mounting::MountingRequest>();
    if (!requete)
    {
        JournalRalenti("requete de montage NON CONSTRUITE", "");
        return;
    }
    requete->lowLevelMountingInfo.childId = avatar;
    requete->lowLevelMountingInfo.parentId = vehicule;
    requete->lowLevelMountingInfo.slotId.id = nomSiege;
    requete->preservePositionAfterMounting = true;
    requete->mountData = ContexteMontage(vehicule, nomSiege, instantane);
    if (!Red::CallVirtual(facility, "Mount", requete))
    {
        JournalRalenti("Mount REFUSE", " — la methode n'a pas ete trouvee");
        return;
    }

    // ── COUCHE C', LA RECETTE DU JEU POUR UN NON-JOUEUR (2026-08-26) ────────────────────────
    //
    // MESURE QUI L'A MOTIVEE. Avec la seule couche C, le journal du temoin repetait :
    //
    //     [occupation] avatar 1 -> vehicule 562949953421314 siege 0
    //                  (corps NON PLACE, instantane) — MountToVehicle introuvable OU refuse
    //
    // et la capture prise depuis le siege arriere montrait **le siege conducteur VIDE**, alors que
    // le serveur y assied bien le joueur 1 et que le temoin recoit l'occupation. Trois couches sur
    // quatre marchaient ; c'est le corps qui ne suivait pas.
    //
    // FICHE DU CONSOMMATEUR (ADR 0034), lue dans les scripts du JEU, pas devinee :
    //
    //   · cible          `MountAIEvent` (nom `'Mount'`) poste sur le PANTIN
    //   · lecteurs       `aiComponent.OnVehicleAssign` + l'arbre de comportement natif
    //   · alimente       l'assise ET l'animation d'entree d'un occupant NON-JOUEUR
    //   · domaine        `slotName` = un slot reel du vehicule ; `mountParentEntityId` = le vehicule
    //   · hors domaine   le jeu teste `IsSlotOccupied` AVANT d'emettre — il n'emet pas sinon
    //
    // ⭐ CE QUI REND CETTE VOIE DIFFERENTE, ET POURQUOI L'AUTRE REFUSAIT. `MountToVehicle` est
    // appelee a TROIS endroits dans tout le jeu, et les trois sont dans `vehicleTransition.script`
    // — la machine a etats du JOUEUR. Aucun PNJ ne passe par la. Le jeu assied ses PNJ autrement :
    // `MountAssigendVehicle` (`aiVehicle.script:877`) construit un `MountEventData` et pose un
    // `MountAIEvent` sur le pantin, et c'est tout. Nous appelions donc la fonction du joueur sur un
    // corps qui n'en est pas un — un refus parfaitement normal, qui ressemblait a une panne.
    //
    // ⚠️ `ignoreHLS = true` et `isInstant = false` sont copies du jeu tels quels. Le premier evite
    // que le systeme de « high level state » du pantin ne refuse la transition ; le second laisse
    // l'animation d'entree se jouer — sauf quand on RATTRAPE un etat deja vieux, ou l'on veut au
    // contraire le placement immediat (meme regle que la couche B).
    //
    // ⚠️ CE N'EST PAS PROUVE TANT QU'UN OEIL NE L'A PAS VU. `QueueEvent` ne rend rien
    // d'exploitable : l'evenement PART, il n'est pas « accepte ». Le verdict est la capture du
    // siege conducteur, et rien d'autre.
    bool evenementAiPoste = false;
    {
        const auto oCorpsAi = ObjetDe(avatar);
        if (oCorpsAi.has_value())
        {
            auto donneesAi = Red::MakeScriptedHandle<RED4ext::game::MountEventData>();
            auto evt = Red::MakeScriptedHandle<RED4ext::game::MountAIEvent>();
            if (donneesAi && evt)
            {
                donneesAi->slotName = nomSiege;
                donneesAi->mountParentEntityId = vehicule;
                donneesAi->isInstant = instantane;
                donneesAi->ignoreHLS = true;
                evt->name = RED4ext::CName("Mount");
                evt->data = donneesAi;
                evenementAiPoste = Red::CallVirtual(*oCorpsAi, "QueueEvent", evt);
            }
        }
    }

    // COUCHE C. Sans elle, l'occupation est correcte partout SAUF a l'ecran.
    //
    // ⚠️ CHAQUE CAUSE D'ECHEC EST NOMMEE, et ce n'est pas du zele. Premiere version : un seul
    // booleen `corpsPlace`, donc un seul message « NON PLACE » pour TROIS causes distinctes —
    // systeme injoignable, entites non resolues, appel refuse. Mesure du 2026-08-26 : le journal
    // repetait « corps NON PLACE » une fois par seconde pendant que Lucas testait, et il etait
    // impossible de savoir laquelle des trois regarder. Un diagnostic qui ne distingue pas ses
    // causes ne fait pas gagner de temps, il en fait perdre.
    bool corpsPlace = false;
    const char* causeEchec = "";
    auto* workspot = SystemeWorkspot();
    if (workspot == nullptr)
    {
        causeEchec = " — WorkspotGameSystem INJOIGNABLE";
    }
    else
    {
        const auto oCaisse = ObjetDe(vehicule);
        const auto oCorps = ObjetDe(avatar);
        if (!oCaisse.has_value())
        {
            causeEchec = " — la CAISSE ne se resout pas en objet";
        }
        else if (!oCorps.has_value())
        {
            causeEchec = " — le CORPS ne se resout pas en objet";
        }
        else
        {
            RED4ext::DynArray<RED4ext::ent::EntityID> aucunSync{};
            RED4ext::DynArray<RED4ext::CName> aucuneVar{};
            corpsPlace = Red::CallVirtual(workspot, "MountToVehicle", *oCaisse, *oCorps, 0.0f, 0.0f,
                                          RED4ext::CName("OccupantSlots"), nomSiege, aucunSync,
                                          RED4ext::CName(), aucuneVar);
            if (!corpsPlace)
            {
                // ⚠️ `CallVirtual` rend `false` pour DEUX raisons qu'il ne distingue pas : la
                // methode n'a pas ete trouvee, ou elle a ete appelee et a refuse. On le dit tel
                // quel plutot que de choisir — se tromper de moitie ici enverrait chercher un nom
                // de methode alors que c'est le contrat de l'appel qui est en cause, ou l'inverse.
                causeEchec = " — MountToVehicle introuvable OU refuse";
            }
        }
    }
    SDK->logger->InfoF(PLUGIN,
        "[occupation] avatar %llu -> vehicule %llu siege %u (corps %s, %s, evenement IA %s)%s",
        avatarReseau, vehiculeReseau, static_cast<uint32_t>(siege),
        corpsPlace ? "place" : "NON PLACE", instantane ? "instantane" : "anime",
        evenementAiPoste ? "poste" : "NON poste", causeEchec);
}

/// ASSIS(a) -> ASSIS(b). Une operation DEDIEE, pas une descente suivie d'une montee : decomposer
/// laisse le corps accroche a l'ancien siege entre les deux (mesure le 2026-08-14).
static void ChangerDeSiege(uint64_t avatarReseau, RED4ext::ent::EntityID vehicule,
                           RED4ext::ent::EntityID avatar, uint8_t siege)
{
    auto* workspot = SystemeWorkspot();
    const auto oCaisse = ObjetDe(vehicule);
    const auto oCorps = ObjetDe(avatar);
    if (workspot == nullptr || !oCaisse.has_value() || !oCorps.has_value())
    {
        return;
    }
    RED4ext::DynArray<RED4ext::CName> aucuneVar{};
    const bool ok = Red::CallVirtual(workspot, "SwitchSeatVehicle", *oCaisse, *oCorps,
                                     RED4ext::CName("OccupantSlots"), SiegeDeIndex(siege),
                                     RED4ext::CName("switch_seat"), aucuneVar, aucuneVar);
    SDK->logger->InfoF(PLUGIN, "[occupation] avatar %llu change pour le siege %u (%s)",
        avatarReseau, static_cast<uint32_t>(siege), ok ? "ok" : "REFUSE");
}

/// ASSIS -> DEHORS. Couche B sur l'attache REELLE (jamais reconstruite de memoire), puis couche C,
/// animee : la descente vient d'arriver sur le fil, la jouer maintenant est juste.
static void DescendreAvatar(uint64_t avatarReseau, RED4ext::ent::EntityID avatar,
                            const EtatObserve& observe)
{
    if (auto* facility = FacadeMontage())
    {
        auto requete = Red::MakeScriptedHandle<RED4ext::game::mounting::UnmountingRequest>();
        if (requete)
        {
            requete->lowLevelMountingInfo.childId = avatar;
            requete->lowLevelMountingInfo.parentId = observe.parent;
            requete->lowLevelMountingInfo.slotId.id = observe.slot;
            requete->mountData = ContexteMontage(observe.parent, observe.slot, true);
            Red::CallVirtual(facility, "Unmount", requete);
        }
    }

    if (auto* workspot = SystemeWorkspot())
    {
        const auto oCaisse = ObjetDe(observe.parent);
        const auto oCorps = ObjetDe(avatar);
        if (oCaisse.has_value() && oCorps.has_value())
        {
            RED4ext::Vector4 aucunDelta{};
            RED4ext::Quaternion aucuneRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
            Red::CallVirtual(workspot, "UnmountFromVehicle", *oCaisse, *oCorps, false, aucunDelta,
                             aucuneRotation, RED4ext::CName());
        }
    }

    // ON JETTE LE TAMPON D'INTERPOLATION (F-VEH-035). Il contient encore les poses du temps ou
    // l'avatar etait assis — c'est-a-dire la position du VEHICULE, puisque l'invariant convoi
    // ecrase la position de chaque occupant. Les rejouer teleporte le corps en pleine carrosserie,
    // et le moteur l'ejecte : un corps sur le toit, l'autre dessous, la voiture qui decolle.
    g_tamponsJoueurs.erase(avatarReseau);
    SDK->logger->InfoF(PLUGIN, "[occupation] avatar %llu sorti du vehicule", avatarReseau);
}


int NetworkGameSystem::PingCourantMs() const
{
    if (m_pInterface == nullptr || m_hConnection == k_HSteamNetConnection_Invalid)
    {
        return -1; // pas encore connecte : -1 dit « inconnu », jamais 0 qui dirait « parfait ».
    }
    SteamNetConnectionRealTimeStatus_t etat{};
    if (!m_pInterface->GetConnectionRealTimeStatus(m_hConnection, &etat, 0, nullptr))
    {
        return -1;
    }
    return etat.m_nPing;
}

void NetworkGameSystem::HandleSnapshot(const cyberpunk_rp::protocol::Snapshot* snapshot)
{
    // D'ABORD rattraper ce qui n'avait pas pu etre place (creux B0), AVANT d'appliquer le nouveau
    // snapshot : une entite nee au snapshot precedent est justement devenue resolvable entre-temps.
    RejouerPosesEnAttente();

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

    // ── L'ANCRE DE TEMPS COMMUNE ───────────────────────────────────────────────────────────
    //
    // `ts_ms` est l'horloge MURALE du serveur au moment de l'encodage. Elle n'a rien à voir avec
    // `tick`, qui est un compteur de simulation dont l'origine change à chaque redémarrage de
    // shard : `tick` sert à interpoler, `ts_ms` sert à DATER.
    //
    // C'est ce qui rend le journal de cette machine comparable à celui du serveur et à ceux des
    // quarante-neuf autres joueurs. Sans ça, une soustraction d'horodatages entre deux PC mesure
    // la dérive des horloges Windows (couramment plusieurs secondes) et l'appelle « latence ».
    //
    // Un serveur antérieur au 2026-08-15 laisse le champ à son défaut (0) : l'estimateur l'ignore
    // et la télémétrie retombe sur l'heure locale, sans rien inventer.
    g_horlogeServeur.Observer(snapshot->ts_ms(),
                              static_cast<std::uint64_t>(Tessera::Sync::Telemetrie::Maintenant()));

    // L'état de l'horloge, périodiquement — c'est la ligne qui permet de RE-CORRIGER tout le
    // fichier après coup si l'estimation s'avère mauvaise. À 25 Hz de diffusion, une fois par
    // seconde suffit : le décalage ne bouge pas d'un snapshot à l'autre.
    if (g_telemetrie.Active())
    {
        static std::uint64_t s_prochainRapportHorloge = 0;
        const auto maintenant = static_cast<std::uint64_t>(Tessera::Sync::Telemetrie::Maintenant());
        if (maintenant >= s_prochainRapportHorloge)
        {
            s_prochainRapportHorloge = maintenant + 1000;
            g_telemetrie.Horloge(g_horlogeServeur.DecalageMs(), g_horlogeServeur.EtalementMs(),
                                 g_horlogeServeur.Observations(), PingCourantMs());
        }
    }

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
            // Le REGARD (spec 2026-08-15 §5.1). Defaut (0,0) = non rapporte par ce client :
            // le consommateur retombe alors sur le yaw du corps.
            pose.lookYaw = DequantYaw(ps->look_yaw());
            pose.lookPitch = static_cast<float>(ps->look_pitch()) * (360.0f / 65536.0f);
            // `move_dir` voyage depuis le gel du palier 2 et n'etait lu nulle part : un joueur qui
            // marche en crabe ou a reculons etait rendu de face. On le RANGE des maintenant ; ce
            // qu'on saura en faire depend du backlog Q7.
            pose.moveDir = ps->move_dir();
            // LA POSE TENUE (assis, adosse). Le champ voyage depuis le palier 2 et n'etait lu
            // NULLE PART : le serveur l'ecrivait, le fil le transportait, le client le jetait
            // (F-PLY-303, mesure du 2026-08-24 — recensement exhaustif des accesseurs `ps->...()`).
            // Toute la moitie « les autres me voient assis » reposait sur un canal a zero lecteur.
            pose.sustained = ps->sustained();
            // QUI LE PORTE (ADR 0039). `frame` = 0 a pied, sinon l'EntityID de la cabine. On ne
            // s'en sert pas pour placer le corps — la position reste une position MONDE — mais
            // pour savoir qu'un AUTRE systeme le place deja, et lui laisser la verticale.
            if (ps->frame() != 0)
            {
                auto& ancrage = g_porteurParAvatar[ps->id()];
                if (ancrage.cabine != ps->frame())
                {
                    ancrage = AncrageCabine{};
                    ancrage.cabine = ps->frame();
                }
            }
            else
            {
                // ⚠️ DEMONTER AVANT D'OUBLIER. Effacer l'entree d'abord perdrait l'id de cabine et
                // l'emplacement, donc le corps resterait attache a une cabine dont plus personne ne
                // sait qu'il descend — un passager fantome qui suivrait la cabine a jamais.
                const auto porte = g_porteurParAvatar.find(ps->id());
                if (porte != g_porteurParAvatar.end())
                {
                    if (porte->second.monte)
                    {
                        const auto corps = m_networkedEntitiesLookup.find(ps->id());
                        if (corps != m_networkedEntitiesLookup.end())
                        {
                            DescendreDeCabine(corps->second, porte->second.cabine,
                                              porte->second.emplacement);
                        }
                    }
                    g_porteurParAvatar.erase(porte);
                }
            }
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
    // ── LA PROPRIETE LOCALE SE VERIFIE, ELLE NE SE CROIT PAS ──────────────────────────────
    //
    // `g_vehiculeLocalMonte` est pose par `RapporterMontage`, donc par un EVENEMENT. Si l'evenement
    // de descente manque une fois — sortie par un chemin qui ne le declenche pas, mort dans le
    // vehicule, changement de siege mal apparie — la voiture reste « a moi » pour toujours : je
    // cesse definitivement de lui appliquer la pose du serveur, elle se fige chez moi et continue
    // de bouger chez les autres.
    //
    // C'est exactement le symptome rapporte par Lucas le 2026-08-14 : « on voit pas les memes
    // voitures, elles sont pas au meme endroit », alors que les sieges, eux, restaient synchrones.
    //
    // On relit donc l'etat REEL du joueur local une fois par seconde. Meme discipline que le
    // montage des avatars : l'evenement propose, la relecture dispose.
    if (g_vehiculeLocalMonte != 0)
    {
        static std::chrono::steady_clock::time_point s_derniereVerifProprio{};
        const auto maintenant = std::chrono::steady_clock::now();
        if (maintenant - s_derniereVerifProprio >= std::chrono::seconds(1))
        {
            s_derniereVerifProprio = maintenant;
            if (auto* facility = FacadeMontage())
            {
                const auto joueur = Cyberverse::Utils::GetPlayer();
                if (joueur)
                {
                    bool monte = false;
                    // `entityID` est un CHAMP, pas un accesseur — `GetEntityID()` n'existe pas sur
                    // `game::Object` (erreur deja faite le 2026-08-14, en C++ comme ici).
                    if (Red::CallVirtual(facility, "IsMountedToAnything", monte, joueur->entityID)
                        && !monte)
                    {
                        SDK->logger->InfoF(PLUGIN,
                            "[montage] je ne suis plus dans le vehicule %llu — je lui rends sa "
                            "pose serveur", g_vehiculeLocalMonte);
                        g_vehiculeLocalMonte = 0;
                    }
                }
            }
        }
    }

    const auto* vehicles = snapshot->vehicles();
    // ── LA TRACE QUI SEPARE TROIS PANNES QUI SE RESSEMBLENT ────────────────────────────────
    //
    // « Aucune voiture a l'ecran » a trois causes possibles, et AUCUN journal ne les distinguait
    // le 2026-08-24 : (1) le serveur n'envoie rien, (2) il envoie et on saute le traitement,
    // (3) on traite et le spawn echoue. Sans cette ligne, un correctif sur (2) qui ne change rien
    // se lit comme « le correctif ne marche pas » alors que la panne etait en (1).
    //
    // Bornee a une ligne toutes les 5 s : le chemin est chaud (un passage par snapshot).
    {
        static std::chrono::steady_clock::time_point s_dernierBilanVehicules{};
        const auto maintenant = std::chrono::steady_clock::now();
        if (maintenant - s_dernierBilanVehicules > std::chrono::seconds(5))
        {
            s_dernierBilanVehicules = maintenant;
            size_t recus = (vehicles != nullptr) ? vehicles->size() : 0;
            size_t connus = 0;
            if (vehicles != nullptr)
            {
                for (const auto* vs : *vehicles)
                {
                    if (vs != nullptr
                        && m_networkedEntitiesLookup.find(vs->id())
                            != m_networkedEntitiesLookup.end())
                    {
                        ++connus;
                    }
                }
            }
            SDK->logger->InfoF(PLUGIN, "[vehicules] recus du serveur=%zu deja nes cote client=%zu", recus, connus);
        }
    }
    if (vehicles != nullptr)
    {
        for (const auto* vs : *vehicles)
        {
            if (vs != nullptr)
            {
                // La voiture ou JE suis assis m'appartient : la placer depuis le fil produirait la
                // boucle mesuree le 2026-08-14 (voiture enfoncee, volant inerte) — cf. F-VEH-033
                // et le commentaire de `RapporterMontage`. On la marque presente pour qu'elle
                // echappe au despawn, et on ne touche pas a sa pose.
                // ── LA CASSE : on ne pousse que les CHANGEMENTS ──────────────────────
                //
                // ⚠️ AVANT le court-circuit du vehicule monte et AVANT la bande morte : une
                // voiture GAREE ne bouge jamais, et c'est precisement celle qu'on emboutit sans
                // la conduire. Placer ce bloc plus bas ne l'aurait jamais vue changer — la meme
                // faute de placement que la bande morte qui masquait la creation (F-VEH-039).
                {
                    // ⚠️⚠️ ON N'ENFILE QUE CE QUI EST APPLICABLE — mesure du 2026-08-26.
                    //
                    // Premiere version : on enfilait des la premiere vue du vehicule dans le
                    // snapshot. Or a cet instant l'entite n'est PAS encore nee cote client, donc
                    // `Tessera_DegatsVehicule()` rendait un `EntityID` vide, et redscript defilait
                    // dans le vide. La trace le disait mot pour mot :
                    //
                    //     [Tessera/Casse] file : 1 en attente
                    //     [Tessera/Casse] defile : cible definie=false casse=85
                    //
                    // Et la casse etait alors PERDUE POUR TOUJOURS : on ne pousse que sur
                    // CHANGEMENT, et `m_degatsConnus` avait deja enregistre 85. Le vehicule
                    // naissait une seconde plus tard, intact, et plus rien ne le corrigeait.
                    //
                    // Le correctif tient a l'ordre : on ne memorise la valeur QUE si on a pu
                    // l'enfiler. Tant que l'entite n'existe pas, `m_degatsConnus` reste en arriere
                    // — donc le tick suivant reessaie tout seul, sans compteur ni minuterie.
                    //
                    // ⚠️ Et defiler d'abord cote redscript reste JUSTE : une file qui se bouche sur
                    // une entree intraitable est pire. Les deux gardes se complètent — ici on
                    // n'enfile que le traitable, la-bas on ne bloque jamais sur l'intraitable.
                    const uint8_t casse = vs->degats();
                    const bool entiteConnue =
                        m_networkedEntitiesLookup.find(vs->id()) != m_networkedEntitiesLookup.end();
                    auto connu = m_degatsConnus.find(vs->id());
                    const bool aChange = (connu == m_degatsConnus.end()) ? (casse > 0)
                                                                        : (connu->second != casse);
                    if (aChange && entiteConnue)
                    {
                        m_degatsAAppliquer.emplace_back(vs->id(), casse);
                        if (connu == m_degatsConnus.end())
                        {
                            m_degatsConnus.emplace(vs->id(), casse);
                        }
                        else
                        {
                            connu->second = casse;
                        }
                    }
                    else if (!aChange && connu == m_degatsConnus.end())
                    {
                        // Vehicule intact jamais vu : on retient qu'il est a zero, sans rien
                        // enfiler. Sans ca, chaque tick le traiterait comme une premiere vue.
                        m_degatsConnus.emplace(vs->id(), casse);
                    }
                }

                if (vs->id() == g_vehiculeLocalMonte)
                {
                    present.insert(vs->id());
                    continue;
                }
                // ── ON NE TELEPORTE PAS UNE VOITURE QUI N'A PAS BOUGE ─────────────────
                //
                // MESURE le 2026-08-15 (Lucas) : « des vehicules empiles, ca sature physiquement,
                // ca prend des degats, ca saute dans tous les sens ».
                //
                // Ces voitures sont de VRAIES entites physiques — pas les fantomes immateriels du
                // sondage de juillet (F-VEH-007), qui n'existaient que par `Teleport`. Le moteur
                // leur applique donc collisions et gravite, et notre placement autoritaire par
                // frame se BAT contre lui : le moteur pousse, on repose, le moteur repousse. Sur
                // une voiture a l'arret c'est un tremblement permanent et des degats gratuits.
                //
                // Une voiture dont la pose serveur n'a pas bouge n'a rien a recevoir : on la laisse
                // au moteur. Le seuil est large devant la quantization du fil (`q_pos`) et etroit
                // devant un deplacement reel — une voiture qui roule depasse 5 cm par snapshot des
                // la premiere vitesse.
                static std::map<uint64_t, RED4ext::Vector4> s_dernierePoseVehicule;
                const RED4ext::Vector4 posee = { DequantPos(vs->position()->x()),
                                                 DequantPos(vs->position()->y()),
                                                 DequantPos(vs->position()->z()), 1.0f };
                auto& precedente = s_dernierePoseVehicule[vs->id()];
                const float dx = posee.X - precedente.X;
                const float dy = posee.Y - precedente.Y;
                const float dz = posee.Z - precedente.Z;
                // ⚠️⚠️ LA BANDE MORTE NE VAUT QUE POUR UN VEHICULE QUI EXISTE DEJA.
                //
                // MESURE le 2026-08-24 : le serveur semait trois voitures GAREES a six metres du
                // joueur, et le client n'en avait AUCUNE — sans une ligne d'erreur nulle part.
                //
                // Cause : `applyPose` est le SEUL endroit qui appelle `SpawnNetworkEntity`. Une
                // voiture garee ne l'atteignait qu'au tout PREMIER snapshot ; ensuite la bande
                // morte `continue`ait avant. Or un spawn peut echouer ou etre DIFFERE — c'est meme
                // le cas nominal, le journal le dit pour les avatars (« spawn differe le temps que
                // l'AppearanceSync arrive, retente au prochain snapshot »). Le mecanisme de reprise
                // EST le rappel d'`applyPose` au snapshot suivant. La bande morte le supprimait.
                //
                // Et `s_dernierePoseVehicule` est `static` : il survit a la deconnexion. A la
                // reconnexion suivante, une voiture immobile depuis la session precedente etait
                // jugee immobile DES LE PREMIER snapshot — donc jamais creee du tout.
                //
                // Regle generale, et elle depasse ce fichier : une optimisation qui saute la MISE A
                // JOUR ne doit jamais sauter la CREATION. « Rien n'a change » et « ca n'existe pas
                // encore » sont deux etats differents, et un seul des deux se passe de travail.
                const bool dejaPresent =
                    m_networkedEntitiesLookup.find(vs->id()) != m_networkedEntitiesLookup.end();
                const bool immobile = dejaPresent && precedente.W != 0.0f
                    && (dx * dx + dy * dy + dz * dz) < (0.05f * 0.05f);
                if (immobile)
                {
                    present.insert(vs->id());
                    continue;
                }
                precedente = posee;

                // Les vehicules n'ont PAS le triplet biped (ils portent une `speed`
                // scalaire) : locomotion 0, donc placement direct sans commande de marche.
                applyPose(vs->id(), vs->position(), vs->yaw(), 0);
            }
        }
    }

    // ── OCCUPANTS : RÉCONCILIATION, PAS ÉVÉNEMENTS ─────────────────────────────────────────
    //
    // Refonte du 2026-08-14 (spec `2026-08-14-vehicules-refonte-occupation-design.md` §2.1), après
    // sept correctifs successifs qui n'avaient rien débloqué. Le modèle précédent traitait
    // l'occupation comme une suite d'ÉVÉNEMENTS et maintenait un miroir local de « qui est assis
    // où ». Un miroir diverge toujours : la voiture figée chez un client, l'avatar assis dans une
    // voiture vide, la place restée vacante — les trois étaient des divergences de miroir, pas des
    // défauts de réplication.
    //
    // Ici on ne mémorise plus l'état du JEU. À chaque passage on lit ce que le jeu observe, on le
    // compare à ce que le serveur veut, et on n'émet que la différence. Trois propriétés en
    // découlent, et ce sont exactement celles qui manquaient : auto-réparant (un ordre perdu ou
    // différé se rattrape au passage suivant, sans code de reprise), sans dérive (rien à
    // maintenir), idempotent (le même snapshot appliqué deux fois n'émet rien).
    //
    // ⚠️ TROIS COUCHES, ET L'ORDRE N'EST PAS NÉGOCIABLE (spec §2) :
    //     A · le SERVEUR décide      → `occupants[]`
    //     B · `MountingFacility`     → l'attache logique (occupation, portes, scanner)
    //     C · `WorkspotGameSystem`   → le CORPS et l'animation
    // Écrire B sans C donne un état cohérent partout SAUF à l'écran. C'est ce qui a coûté la
    // journée du 14 août : chaque instrument ajouté interrogeait B, qui disait vrai.
    {
        // État VOULU (couche A). Le joueur local s'en exclut par construction : il n'est jamais
        // dans `m_networkedEntitiesLookup`, qui ne contient que les corps nés pour les autres.
        std::map<uint64_t, MontageVoulu> voulu;
        if (vehicles != nullptr)
        {
            for (const auto* vs : *vehicles)
            {
                if (vs == nullptr || vs->occupants() == nullptr)
                {
                    continue;
                }
                for (const auto* occ : *vs->occupants())
                {
                    if (occ != nullptr)
                    {
                        voulu[occ->client()] = { vs->id(), occ->seat() };
                    }
                }
            }
        }

        // Qui faut-il examiner à ce passage ? Tout le monde serait correct mais coûteux : lire les
        // couches B et C est un aller RTTI par avatar. On examine donc (a) ceux dont l'état VOULU
        // vient de changer — c'est la réactivité, et comparer deux états SERVEUR reçus en entier
        // n'est pas un miroir du jeu — et (b) tout le monde, une fois par seconde, pour la
        // réparation. Entre les deux, on ne paie rien.
        static std::map<uint64_t, MontageVoulu> s_vouluPrecedent;
        static std::set<uint64_t> s_candidats; // avatars déjà assis : à surveiller pour la descente
        /// Corps qu'on avait deja resolus au passage precedent. Un avatar qu'on decouvre a
        /// l'instant rejoue forcement un etat vieux de plusieurs secondes, meme si le serveur
        /// vient de nous l'annoncer : il etait deja assis avant qu'on le voie.
        static std::set<uint64_t> s_corpsConnus;
        static std::chrono::steady_clock::time_point s_dernierBalayage{};
        const auto maintenant = std::chrono::steady_clock::now();
        const bool balayage = (maintenant - s_dernierBalayage) >= std::chrono::seconds(1);
        if (balayage)
        {
            s_dernierBalayage = maintenant;
        }

        std::set<uint64_t> aExaminer;
        for (const auto& [id, cible] : voulu)
        {
            const auto avant = s_vouluPrecedent.find(id);
            if (balayage || avant == s_vouluPrecedent.end() || !(avant->second == cible))
            {
                aExaminer.insert(id);
            }
        }
        for (uint64_t id : s_candidats)
        {
            if (balayage || voulu.find(id) == voulu.end())
            {
                aExaminer.insert(id);
            }
        }
        // ⚠️ ON GARDE UNE COPIE AVANT D'ECRASER. C'est le seul signal qui distingue « le serveur
        // vient d'annoncer qu'il s'assied » de « on decouvre qu'il etait deja assis » — et c'est
        // cette distinction, et elle seule, qui decide si l'animation se joue.
        const std::map<uint64_t, MontageVoulu> precedent = s_vouluPrecedent;
        s_vouluPrecedent = voulu;

        for (uint64_t avatarReseau : aExaminer)
        {
            const auto corps = m_networkedEntitiesLookup.find(avatarReseau);
            if (corps == m_networkedEntitiesLookup.end())
            {
                continue; // corps pas (encore) rendu : rien à réconcilier, on retentera.
            }
            // Note du passage COURANT, lu au passage suivant. Insere apres le `continue` : un corps
            // qu'on n'a pas pu resoudre n'est pas « connu ».
            const bool corpsVuAvant = s_corpsConnus.count(avatarReseau) != 0;
            s_corpsConnus.insert(avatarReseau);

            EtatObserve observe{};
            if (!LireEtatObserve(corps->second, observe))
            {
                continue; // lecture impossible : ne rien faire vaut mieux que faire à l'aveugle.
            }

            const auto cible = voulu.find(avatarReseau);
            if (cible == voulu.end())
            {
                // Le serveur ne le dit plus assis. S'il l'est encore dans le jeu, on l'en sort.
                if (observe.attache)
                {
                    DescendreAvatar(avatarReseau, corps->second, observe);
                }
                s_candidats.erase(avatarReseau);
                g_avatarsAssis.erase(avatarReseau);
                continue;
            }

            const auto caisse = m_networkedEntitiesLookup.find(cible->second.vehicule);
            if (caisse == m_networkedEntitiesLookup.end())
            {
                continue; // véhicule pas rendu : on ne peut pas encore l'y asseoir.
            }
            s_candidats.insert(avatarReseau);
            g_avatarsAssis.insert(avatarReseau);

            const RED4ext::CName siegeVoulu = SiegeDeIndex(cible->second.siege);
            const bool bonVehicule = observe.attache
                && observe.parent.hash == caisse->second.hash;
            if (bonVehicule && observe.slot == siegeVoulu)
            {
                continue; // déjà exactement là où il doit être : rien à faire.
            }

            // ⚠️ UN CHANGEMENT DE PLACE N'EST PAS UNE DESCENTE SUIVIE D'UNE MONTÉE. Le jeu a une
            // opération dédiée (`workspotSystem.script:34`, employée par
            // `vehicleTransition.script:1995`). La décomposer laisse le corps accroché à l'ancien
            // siège entre les deux — c'est le « la place reste vide » mesuré le 2026-08-14.
            if (bonVehicule)
            {
                ChangerDeSiege(avatarReseau, caisse->second, corps->second, cible->second.siege);
                continue;
            }

            // Attaché ailleurs (autre véhicule) : on le décroche d'abord, sinon le montage ne
            // déplace rien.
            if (observe.attache)
            {
                DescendreAvatar(avatarReseau, corps->second, observe);
            }

            // ── REGLE DU « DEJA VECU » (spec §2.3) ────────────────────────────────────────
            //
            // On n'anime QUE ce qu'on voit arriver en direct. Un avatar qu'on decouvre deja assis
            // rejoue un etat vieux de plusieurs secondes — l'animer le ferait entrer dans une
            // voiture ou il est deja, en retard et en double.
            //
            // ⚠️ LA PREMIERE VERSION DE CE TEST ETAIT INVERSEE, et l'animation d'entree ne se
            // jouait donc JAMAIS : `s_candidats.insert` avait lieu quelques lignes plus haut, si
            // bien que `count() != 0` etait toujours vrai et que la condition se reduisait a
            // `observe.attache` — faux par definition dans une transition DEHORS -> ASSIS. Un
            // drapeau calcule APRES avoir ete pose ne mesure plus rien.
            //
            // Le bon signal tient en deux termes, et aucun n'est un miroir de l'etat du jeu :
            //   · le corps etait DEJA connu au passage precedent (sinon c'est une decouverte) ;
            //   · le serveur ne le disait PAS assis au passage precedent (sinon c'est un
            //     rattrapage).
            // Les deux ensemble : le serveur vient de l'asseoir sous nos yeux.
            const bool corpsDejaConnu = corpsVuAvant;
            const bool assisAuPassagePrecedent = precedent.find(avatarReseau) != precedent.end();
            const bool vuArriver = corpsDejaConnu && !assisAuPassagePrecedent;
            AsseoirAvatar(avatarReseau, caisse->second, corps->second, cible->second.vehicule,
                          cible->second.siege, !vuArriver);
        }

        // ── PURGE ─────────────────────────────────────────────────────────────────────────
        //
        // Ces ensembles survivraient au despawn : fuite lente, et surtout une entree perimee ferait
        // rater le premier placement d'un identifiant REUTILISE — on le croirait deja connu, ou
        // deja assis, sur la foi d'une vie anterieure. Un cache qui ne se vide pas finit par
        // repondre sur des morts.
        for (auto it = s_corpsConnus.begin(); it != s_corpsConnus.end();)
        {
            it = (m_networkedEntitiesLookup.find(*it) == m_networkedEntitiesLookup.end())
                     ? s_corpsConnus.erase(it)
                     : std::next(it);
        }
        for (auto it = s_candidats.begin(); it != s_candidats.end();)
        {
            it = (m_networkedEntitiesLookup.find(*it) == m_networkedEntitiesLookup.end())
                     ? s_candidats.erase(it)
                     : std::next(it);
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
            // La table d'échantillonnage de la télémétrie suit la même règle que ses voisines
            // ci-dessus : une entrée par id réseau jamais revu s'accumulerait sur une session de
            // plusieurs heures. Petit, mais c'est exactement le patron de la « table jamais
            // purgée » que ce dépôt a déjà payé ailleurs (cf. le halo côté serveur).
            g_telemetrie.OublierEntite(it->first);
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
            p.origine = c->origine() != nullptr ? c->origine()->str() : std::string();
            p.corpsMasculin = c->corps_masculin();
            p.cerveauMasculin = c->cerveau_masculin();
            if (c->esthetique() != nullptr)
            {
                p.esthetique.assign(c->esthetique()->begin(), c->esthetique()->end());
            }
            // ⭐ La recette transparente, resserialisee ICI dans la forme que le client rejoue —
            // « nom:index;nom:index ». La convertir a la reception plutot qu'a chaque lecture
            // evite de refaire le meme travail a chaque appel de l'accesseur.
            if (c->options_apparence() != nullptr)
            {
                for (const auto* o : *c->options_apparence())
                {
                    if (o == nullptr || o->nom() == nullptr) continue;
                    if (!p.recette.empty()) p.recette.push_back(';');
                    p.recette += o->nom()->str();
                    p.recette.push_back(':');
                    p.recette += std::to_string(o->index());
                }
            }
            m_personnages.push_back(std::move(p));
        }
    }
    m_listeRecue = true;
    SDK->logger->InfoF(PLUGIN, "CharacterList : %zu personnage(s) sur ce compte", m_personnages.size());
    for (size_t i = 0; i < m_personnages.size(); ++i)
    {
        SDK->logger->InfoF(PLUGIN, "  [%zu] « %s » esthetique %zu o, recette %zu caracteres",
                           i, m_personnages[i].pseudonyme.c_str(),
                           m_personnages[i].esthetique.size(), m_personnages[i].recette.size());
    }

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
        return;
    }

    // ── REPRISE APRES RECONNEXION ─────────────────────────────────────────────────────────────
    //
    // On incarnait deja quelqu'un avant la coupure : on y retourne, sans repasser par le lobby.
    // C'est ICI et pas dans le callback de connexion, parce que cette liste EST la reponse du
    // serveur a notre `Join` : la recevoir prouve qu'il nous a acceptes et qu'il est pret a
    // entendre un `SelectCharacter`. Rejouer plus tot ferait dependre la reprise d'un ordre de
    // traitement qu'on ne controle pas.
    //
    // ⚠️ On verifie que le personnage EXISTE ENCORE dans la liste. Il peut avoir ete supprime
    // depuis une autre machine pendant la coupure ; demander un id disparu ferait repondre au
    // serveur un refus que personne n'affiche, et le joueur resterait spectateur sans savoir
    // pourquoi. Absent = on laisse le lobby faire son travail.
    if (m_personnageIncarne != 0)
    {
        const uint64_t vise = m_personnageIncarne;
        bool existe = false;
        for (const auto& p : m_personnages)
        {
            if (p.id == vise)
            {
                existe = true;
                break;
            }
        }
        if (existe)
        {
            SDK->logger->InfoF(PLUGIN, "[reconnexion] reprise du personnage %llu", vise);
            Tessera_ChoisirPersonnage(vise);
        }
        else
        {
            SDK->logger->WarnF(PLUGIN,
                "[reconnexion] le personnage %llu n'est plus sur ce compte — retour au lobby", vise);
            m_personnageIncarne = 0;
        }
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

void NetworkGameSystem::HandleStaffMode(const cyberpunk_rp::protocol::StaffMode* msg)
{
    if (msg == nullptr)
    {
        return;
    }
    // AFFECTATION, pas bascule. Le serveur envoie l ETAT, pas un evenement « ca a change » — donc
    // deux `/gm on` de suite laissent le temoin allume, et un message perdu se rattrape au message
    // suivant ou au prochain `Join`. Une bascule ferait exactement l inverse : elle DIVERGERAIT
    // au premier message manque, et plus rien ne la remettrait d aplomb.
    m_modeStaff = msg->on();
    SDK->logger->InfoF(PLUGIN, "StaffMode : %s", m_modeStaff ? "ON" : "OFF");
}

void NetworkGameSystem::HandleCommandCatalog(const cyberpunk_rp::protocol::CommandCatalog* msg)
{
    if (msg == nullptr)
    {
        return;
    }
    // REMPLACEMENT, pas fusion — meme raison que pour `ActionCatalog` : le serveur envoie toujours
    // la liste COMPLETE de ce que ce joueur a le droit de taper. Une commande retiree (kill switch,
    // ou droit revoque) disparait donc d'elle-meme. La fusionner la laisserait suggeree pour
    // toujours, et le joueur la taperait pour se faire refuser sans comprendre.
    m_commandes.clear();
    if (msg->commandes() != nullptr)
    {
        for (const auto* e : *msg->commandes())
        {
            if (e == nullptr)
            {
                continue;
            }
            CommandeRecue r;
            r.nom = e->nom() != nullptr ? e->nom()->str() : std::string();
            if (r.nom.empty())
            {
                continue;
            }

            // La ligne d'affichage, batie une fois a la reception plutot qu'a chaque frappe :
            //     /vehicle <action> <id> [record] — give a car to a character, or repair a wreck.
            // `<...>` pour un argument requis, `[...]` pour un optionnel — la convention de
            // Brigadier, que tout joueur venant de Minecraft lit sans explication.
            //
            // ⭐ SAUF si le serveur envoie un `usage` explicite. `/tp` prend SOIT un joueur, SOIT
            // deux nombres, SOIT trois : une ALTERNATIVE, que la liste plate rendait
            // `<player|x> [y] [z]` — exact et illisible.
            std::string ligne;
            if (e->usage() != nullptr && e->usage()->size() > 0)
            {
                ligne = e->usage()->str();
            }
            else
            {
                ligne = "/" + r.nom;
                if (e->args() != nullptr)
                {
                    for (const auto* a : *e->args())
                    {
                        if (a == nullptr || a->nom() == nullptr)
                        {
                            continue;
                        }
                        ligne += a->requis() ? " <" : " [";
                        ligne += a->nom()->str();
                        ligne += a->requis() ? ">" : "]";
                    }
                }
            }

            // Les valeurs proposables du PREMIER argument qui en porte. Pour `/tp`, ce sont les
            // joueurs connectes — le serveur les met a jour a chaque arrivee.
            if (e->args() != nullptr)
            {
                for (const auto* a : *e->args())
                {
                    if (a == nullptr || a->valeurs() == nullptr || a->valeurs()->size() == 0)
                    {
                        continue;
                    }
                    for (const auto* v : *a->valeurs())
                    {
                        if (v != nullptr)
                        {
                            r.valeurs.push_back(v->str());
                        }
                    }
                    break;
                }
            }
            if (e->aide() != nullptr && e->aide()->size() > 0)
            {
                // Tiret CADRATIN, comme partout dans l'interface Tessera.
                ligne += " \xE2\x80\x94 ";
                ligne += e->aide()->str();
            }
            r.affichage = std::move(ligne);
            m_commandes.push_back(std::move(r));
        }
    }
    SDK->logger->InfoF(PLUGIN, "CommandCatalog : %zu commande(s) disponibles", m_commandes.size());
}

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

void NetworkGameSystem::HandleInventaireAutoritaire(
    const cyberpunk_rp::protocol::InventaireAutoritaire* msg)
{
    if (msg == nullptr)
    {
        return;
    }
    // ⚠️ ON REMPLACE, contrairement aux identites qui s'ACCUMULENT — et la difference est
    // structurelle. Ce message est un ETAT (« voici ton sac »), pas un ajout. Le traiter comme un
    // ajout ferait grossir le sac a chaque envoi, et le rejeu cesserait d'etre inoffensif.
    m_sacAutoritaire.clear();
    m_aPreserver.clear();

    if (msg->items() != nullptr)
    {
        for (const auto* it : *msg->items())
        {
            if (it == nullptr || it->id() == nullptr)
            {
                continue;
            }
            ItemAutoritaire item;
            item.id = it->id()->str();
            item.quantite = it->quantite();
            m_sacAutoritaire.push_back(std::move(item));
        }
    }
    if (msg->preserver() != nullptr)
    {
        for (const auto* p : *msg->preserver())
        {
            if (p != nullptr)
            {
                m_aPreserver.push_back(p->str());
            }
        }
    }

    // Pose EN DERNIER : tant que ce drapeau est faux, redscript ne touche a rien. Le poser avant de
    // remplir laisserait une fenetre ou le sac autoritaire est vu VIDE — donc ou le joueur se
    // ferait vider.
    m_sacRecu = true;
    // ⭐ LE NUMERO DE SEQUENCE DU SAC, et il ferme une course reelle (2026-08-26).
    //
    // A la fermeture d'un coffre, le client envoie son rapport et REPREND aussitot le
    // realignement de l'inventaire. Mais le sac autoritaire qu'il a en cache est encore
    // l'ANCIEN — celui d'avant le depot. La veille compare donc l'inventaire local (l'objet
    // est parti) a un cache perime (l'objet y est) et **rend l'objet au joueur**, pendant
    // qu'il est deja dans le coffre. Duplication, le temps que la reponse du serveur arrive.
    //
    // Un booleen « recu » ne peut pas distinguer deux sacs successifs. Un compteur si.
    ++m_sacSeq;

    SDK->logger->InfoF(PLUGIN, "InventaireAutoritaire : %zu item(s), %zu a preserver",
        m_sacAutoritaire.size(), m_aPreserver.size());
}

// ── ASCENSEURS : les deux envois montants (ADR 0012) ─────────────────────────────────────────
void NetworkGameSystem::SendElevatorCall(uint64_t elevatorId, int32_t floor)
{
    if (m_pInterface == nullptr || elevatorId == 0)
    {
        return;
    }
    // `Reliable` et pas `Unreliable` : un appel d'ascenseur perdu ne se rattrape pas tout seul du
    // cote montant (le serveur n'a rien a re-demander), alors qu'un rappel d'ETAT descendant, lui,
    // repart chaque seconde. L'asymetrie est voulue.
    flatbuffers::FlatBufferBuilder builder;
    const auto call = cyberpunk_rp::protocol::CreateElevatorCall(builder, elevatorId, floor);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_ElevatorCall, call.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
}

// ── APPAREILS DU MONDE : le rapport montant (spec 2026-08-26) ───────────────────────────────
void NetworkGameSystem::SendDeviceCall(uint64_t device, uint8_t famille, uint8_t action,
    uint8_t etatObserve)
{
    if (m_pInterface == nullptr || device == 0)
    {
        return;
    }
    // `Reliable`, meme asymetrie que l'appel d'ascenseur : un rapport perdu ne se rattrape pas du
    // cote montant (le serveur n'a rien a re-demander, il ne sait meme pas que cet appareil
    // existe), alors qu'un etat descendant est reemis au prochain changement.
    flatbuffers::FlatBufferBuilder builder;
    const auto call = cyberpunk_rp::protocol::CreateDeviceCall(builder, device, famille, action,
        etatObserve);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_DeviceCall, call.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
}

// ── LE CANAL DE COMMANDE D'ADMINISTRATION ───────────────────────────────────────────────────
void NetworkGameSystem::SendAdminCommand(const char* texte)
{
    if (m_pInterface == nullptr || texte == nullptr || *texte == '\0')
    {
        return;
    }
    // `Reliable` : une commande d'administration perdue ne se rejoue pas toute seule, et son
    // auteur n'a aucun moyen de savoir qu'elle s'est perdue — il verrait juste « rien ne s'est
    // passe », ce qui est le pire retour possible sur un geste d'operateur.
    flatbuffers::FlatBufferBuilder builder;
    const auto texteOff = builder.CreateString(texte);
    const auto cmd = cyberpunk_rp::protocol::CreateAdminCommand(builder, texteOff);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_AdminCommand, cmd.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
}

// ── LE COFFRE : L'ETAT QUI DESCEND, LE RAPPORT QUI REMONTE ──────────────────────────────────
//
// `InteractionOpen` etait EMIS par le serveur et lu par PERSONNE — le `default:` du routeur le
// journalisait comme non cable. C'est le premier consommateur.
//
// ⚠️ `ui_kind` selectionne l'ecran. 7 = coffre de vehicule. Le catalogue n'est pas fige dans le
// schema (« contenu differe ») : ce numero est donc une convention, et il vit ici ET dans
// `gateway.rs`. Les deux doivent bouger ensemble.
/// Rapporte OU le jeu a pose la voiture. Douze octets — trois flottants, sans schema.
///
/// ⚠️ `choice = 15` est ce qui distingue ce rapport de tous les autres verbes d'interaction. Sans
/// lui, le serveur devrait DEVINER en essayant de decoder le payload, ce que fait deja le rapport
/// de coffre et qui est fragile : deux charges utiles de meme taille seraient confondues.
bool NetworkGameSystem::SendRapportInvocation(float x, float y, float z)
{
    constexpr uint32_t kChoixRapportInvocation = 15;
    if (m_invocationVehicule == 0)
    {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder(256);
    std::array<uint8_t, 12> octets{};
    std::memcpy(octets.data(), &x, 4);
    std::memcpy(octets.data() + 4, &y, 4);
    std::memcpy(octets.data() + 8, &z, 4);
    const auto payload = builder.CreateVector(octets.data(), octets.size());
    const auto choix = cyberpunk_rp::protocol::CreateInteractionChoice(
        builder, m_invocationVehicule, kChoixRapportInvocation, 0, payload);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_InteractionChoice, choix.Union());
    builder.Finish(env);
    // ⚠️ FIABLE, pas « au mieux » : un rapport perdu laisserait la voiture a sa vieille coordonnee
    // en base, et le joueur croirait l'avoir sortie. Meme voie que le rapport de coffre.
    if (m_pInterface == nullptr)
    {
        return false;
    }
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    SDK->logger->InfoF(PLUGIN, "[invocation] rapport envoye : vehicule=%llu pos=(%.2f %.2f %.2f)",
        m_invocationVehicule, x, y, z);
    // ⚠️ La session se FERME ici. Sans ca, un second rappel de la boucle de veille renverrait la
    // meme position, et le serveur reposerait la voiture une deuxieme fois — inoffensif
    // aujourd'hui, faux le jour ou le rapport portera autre chose.
    m_invocationVehicule = 0;
    m_invocationRecord.clear();
    return true;
}

void NetworkGameSystem::HandleInteractionOpen(const cyberpunk_rp::protocol::InteractionOpen* msg)
{
    constexpr uint8_t kUiKindCoffre = 7;
    constexpr uint8_t kUiKindContenant = 8;
    constexpr uint8_t kUiKindInvocation = 8;
    if (msg == nullptr)
    {
        return;
    }
    // ── L'ORDRE D'INVOCATION — « fais naitre ta voiture, et dis-moi OU » ────────────────────
    //
    // ⭐ Le serveur n'envoie PAS une position, il envoie un RECORD. C'est le renversement de
    // F-VEH-054 : le serveur decide *que* la voiture sort et *a qui* elle est ; le JEU decide *ou*,
    // parce qu'il est le seul a savoir ce qu'est une route degagee. Nos coordonnees ecrites a la
    // main ne le savaient pas, et c'est la qu'etaient les tonneaux — donc la casse DEFINITIVE
    // (F-VEH-053).
    //
    // ⚠️ Le payload est ici une CHAINE BRUTE, pas un tampon FlatBuffers imbrique : un nom de record
    // n'a pas besoin d'un schema, et lui en donner un aurait impose une regeneration d'en-tete C++
    // et une porte de merge pour transporter du texte.
    if (msg->ui_kind() == kUiKindInvocation)
    {
        const auto* brut = msg->payload();
        if (brut == nullptr || brut->size() == 0)
        {
            SDK->logger->Warn(PLUGIN, "[invocation] ordre recu SANS record — ignore");
            return;
        }
        m_invocationRecord.assign(reinterpret_cast<const char*>(brut->data()), brut->size());
        m_invocationVehicule = msg->session_id();
        // EN DERNIER, comme partout : tant que la sequence n'a pas bouge, redscript ne fait rien.
        // L'incrementer avant de remplir laisserait une fenetre ou l'ordre est vu avec un record
        // VIDE — et une invocation sans record est une invocation qui echoue en silence.
        ++m_invocationSeq;
        SDK->logger->InfoF(PLUGIN, "[invocation] ordre recu : vehicule=%llu record=%s seq=%d",
            m_invocationVehicule, m_invocationRecord.c_str(), m_invocationSeq);
        return;
    }
    if (msg->ui_kind() != kUiKindCoffre && msg->ui_kind() != kUiKindContenant)
    {
        // ⚠️ On le DIT. Premiere version : `return` muet. Le serveur repondait, le client ne
        // decodait pas, et RIEN nulle part ne disait pourquoi — ni erreur, ni message « non
        // cable » (le routeur, lui, avait bien appele ce gestionnaire). Un chemin de sortie
        // silencieux dans un decodeur est une session de diagnostic en attente.
        SDK->logger->InfoF(PLUGIN, "[coffre] InteractionOpen ignore : ui_kind=%u (attendus %u ou %u)",
            static_cast<unsigned>(msg->ui_kind()), static_cast<unsigned>(kUiKindCoffre),
            static_cast<unsigned>(kUiKindContenant));
        return;
    }
    // Le contenu voyage en tampon IMBRIQUE dans le payload opaque. Verifie avant lecture : un
    // payload tronque ou d'un autre type ferait lire de la memoire arbitraire.
    const auto* brut = msg->payload();
    if (brut == nullptr || brut->size() == 0)
    {
        return;
    }
    flatbuffers::Verifier v(brut->data(), brut->size());
    if (!v.VerifyBuffer<cyberpunk_rp::protocol::CoffreContenu>(nullptr))
    {
        SDK->logger->Warn(PLUGIN, "[coffre] payload illisible — ignore");
        return;
    }
    const auto* contenu = flatbuffers::GetRoot<cyberpunk_rp::protocol::CoffreContenu>(brut->data());

    m_coffreAutoritaire.clear();
    if (contenu->lignes() != nullptr)
    {
        for (const auto* l : *contenu->lignes())
        {
            if (l == nullptr || l->item() == nullptr)
            {
                continue;
            }
            ItemAutoritaire ligne;
            ligne.id = l->item()->str();
            ligne.quantite = l->quantite();
            m_coffreAutoritaire.push_back(std::move(ligne));
        }
    }
    // ⚠️ Le champ s'appelle `contenant` depuis le 2026-08-30 : il porte un id de vehicule OU
    // l'EntityID d'un appareil du monde (voir `protocol.fbs`). Ici on est sur le chemin VEHICULE
    // — d'ou le membre qui garde son nom, et la recherche dans `m_networkedEntitiesLookup` juste
    // apres, qui n'a de sens que pour un vehicule reseau.
    // Le genre de session, retenu ICI et nulle part ailleurs : c'est la seule information que le
    // message d'ouverture porte et que le rapport de fermeture devra rendre.
    m_coffreEstContenant = (msg->ui_kind() == kUiKindContenant);
    m_coffreVehicule = contenu->contenant();
    m_coffreCapacite = contenu->capacite();
    m_coffreSession = msg->session_id();
    // EN DERNIER, comme le drapeau du sac : tant que la sequence n'a pas bouge, redscript ne
    // touche a rien. L'incrementer avant de remplir laisserait une fenetre ou le coffre est vu
    // VIDE — donc ou le joueur se ferait vider son coffre.
    ++m_coffreSeq;

    SDK->logger->InfoF(PLUGIN, "[coffre] vehicule=%llu capacite=%u lignes=%zu seq=%d",
        m_coffreVehicule, static_cast<unsigned>(m_coffreCapacite), m_coffreAutoritaire.size(),
        m_coffreSeq);
}

// Le rapport de fermeture : l'ETAT COMPLET du coffre, pas un delta. Meme raison que pour les
// sieges et pour le sac — un modele par evenements suppose qu'aucun message ne se perd et
// qu'aucun n'arrive deux fois, et les deux arrivent.
void NetworkGameSystem::SendCoffreRapport()
{
    if (m_pInterface == nullptr)
    {
        return;
    }
    flatbuffers::FlatBufferBuilder interne;
    std::vector<flatbuffers::Offset<cyberpunk_rp::protocol::CoffreLigne>> lignes;
    lignes.reserve(m_coffreRapport.size());
    for (const auto& l : m_coffreRapport)
    {
        lignes.push_back(cyberpunk_rp::protocol::CreateCoffreLigne(
            interne, interne.CreateString(l.id), l.quantite));
    }
    // `capacite` reste a 0 dans ce sens : c'est une regle SERVEUR, et un client qui l'annoncerait
    // ne serait pas cru. La renseigner ici donnerait l'illusion qu'elle se negocie.
    const auto contenu = cyberpunk_rp::protocol::CreateCoffreContenu(
        interne, m_coffreVehicule, 0, interne.CreateVector(lignes));
    interne.Finish(contenu);

    flatbuffers::FlatBufferBuilder builder;
    const auto payload = builder.CreateVector(interne.GetBufferPointer(), interne.GetSize());
    // ⛔ LE VERBE EST CE QUI DISTINGUE LES DEUX RAPPORTS, ET IL N'Y A RIEN D'AUTRE.
    //
    // Cote serveur, `extract_rapport_coffre` ne regarde AUCUN verbe : il reconnait un coffre au
    // fait que `session_id` tombe dans la plage d'ids des vehicules. Or un `EntityID` d'appareil
    // n'a aucune plage reservee et peut y tomber par hasard — d'ou un verbe EXPLICITE pour le
    // contenant (16), et l'ordre d'essai cote Gateway qui tente le plus specifique en premier.
    // Se tromper ici ferait ecrire le contenu d'une caisse dans le coffre d'une voiture.
    const uint32_t verbeChoix = m_coffreEstContenant ? 16u : 0u;
    const auto choix = cyberpunk_rp::protocol::CreateInteractionChoice(
        builder, m_coffreSession, verbeChoix, 0, payload);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_InteractionChoice, choix.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);

    SDK->logger->InfoF(PLUGIN, "[coffre] rapport envoye : %zu ligne(s), verbe %u, cible %llu",
        m_coffreRapport.size(), static_cast<unsigned>(verbeChoix), m_coffreVehicule);
}

// ── LES VERBES VEHICULE : LE CANAL MONTANT QUI MANQUAIT ──────────────────────────────────────
//
// Verrou (9), revendication (10), radio (11), casse (12), coffre (14). Un seul emetteur pour les
// cinq : ils ne different que par deux entiers, et cinq fonctions jumelles auraient surtout
// multiplie les endroits ou se tromper de kind.
//
// ⚠️ `param` change de SENS selon le verbe, et c'est le schema qui fait foi (protocol.fbs) :
//    9  -> 0 ouvre, non-nul ferme        (proprietaire uniquement, refus SERVEUR)
//    10 -> ignore                        (sans effet si le vehicule a deja un proprietaire)
//    11 -> station de radio, 0 = eteinte (CONDUCTEUR uniquement)
//    12 -> casse 0..100, MONOTONE        (CONDUCTEUR uniquement)
//    14 -> ignore                        (le serveur repond par InteractionOpen + CoffreContenu)
void NetworkGameSystem::SendVehiculeVerbe(uint64_t target, uint8_t verbe, uint32_t param)
{
    if (m_pInterface == nullptr || target == 0)
    {
        return;
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto ei = cyberpunk_rp::protocol::CreateEntityInteraction(builder, target, verbe, param);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_EntityInteraction, ei.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
}

void NetworkGameSystem::SendElevatorMount(uint64_t elevatorId, bool mount)
{
    if (m_pInterface == nullptr || elevatorId == 0)
    {
        return;
    }
    // kind=6=MountElevator, kind=7=UnmountElevator (protocol.fbs, EntityInteraction). `target` est
    // un elevator_id, JAMAIS un id de la plage vehicule : c'est le kind seul qui desambiguise.
    constexpr uint8_t kMountElevator = 6;
    constexpr uint8_t kUnmountElevator = 7;
    flatbuffers::FlatBufferBuilder builder;
    const auto ei = cyberpunk_rp::protocol::CreateEntityInteraction(
        builder, elevatorId, mount ? kMountElevator : kUnmountElevator, 0);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_EntityInteraction, ei.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
}

// ── POSTURES : l'annonce montante ────────────────────────────────────────────────────────────
//
// Le client DEMANDE, le serveur DECIDE. On n'envoie pas « je suis assis » : on envoie « je
// voudrais l'emplacement N », et c'est `postures.rs` qui accorde ou refuse (occupe, trop loin,
// inconnu) puis pose `PlayerState.sustained` que TOUS les clients recoivent.
//
// ⚠️ POURQUOI CETTE FONCTION MANQUAIT, ET CE QUE CA COUTAIT. Le serveur ecoute `EntityInteraction`
// kind=13 depuis le 2026-08-19 (20 tests verts) et le client ne l'a jamais envoye — exactement
// comme il ne LISAIT jamais `sustained` en retour (F-PLY-303). Les deux moities du canal etaient
// absentes, chacune supposant que l'autre existait. Un canal se verifie AUX DEUX BOUTS.
//
// `emplacementId == 0` = liberation. C'est la meme convention que le serveur applique deja
// (`postures.rs::liberer`), et elle evite un second kind pour dire le contraire du premier.
bool NetworkGameSystem::EnvoyerPosture(uint64_t emplacementId, uint32_t code)
{
    if (m_pInterface == nullptr)
    {
        return false;
    }
    // kind=13 = posture (protocol.fbs, EntityInteraction ; plage CORE 0-63, cf. postures.rs).
    constexpr uint8_t kPosture = 13;
    flatbuffers::FlatBufferBuilder builder;
    const auto ei = cyberpunk_rp::protocol::CreateEntityInteraction(
        builder, emplacementId, kPosture, code);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_EntityInteraction, ei.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    char detail[64];
    std::snprintf(detail, sizeof(detail), "emplacement=%llu,code=%u",
                  static_cast<unsigned long long>(emplacementId), code);
    g_telemetrie.Evenement("posture_demande", 0, detail);
    return true;
}

// ── ASCENSEURS : l'etat autoritaire descendant ───────────────────────────────────────────────
//
// On ne decide RIEN ici : tout le raisonnement (rejouer ? recaler ? ignorer ?) vit en redscript,
// parce qu'il a besoin du `LiftControllerPS` et des actions quest, qui n'existent pas cote C++.
// Ce handler est un pur relais typé.
void NetworkGameSystem::HandleElevatorState(const cyberpunk_rp::protocol::ElevatorStateMsg* msg)
{
    if (msg == nullptr || msg->elevator_id() == 0)
    {
        return;
    }
    // CINQ arguments, pas neuf. `Red::CallVirtual` a une arite bornee, et surtout : le client n'a
    // PAS besoin des durees. Il ne calcule aucune trajectoire — c'est le moteur du jeu qui deplace
    // la cabine, avec ses propres constantes. `start_delay_ms`/`travel_time_ms`/`movement_state`
    // restent sur le fil pour le serveur et pour un futur affichage, pas pour ce pont.
    // int32 pour le tick et l'elapsed : redscript n'a d'operateur de comparaison ni pour `Uint64`
    // ni pour `Uint32`. `depart_tick` n'est pas une date pour le client, c'est un IDENTIFIANT DE
    // TRAJET (« ai-je deja rejoue celui-ci ? ») — le tronquer garde deux departs consecutifs
    // distincts. `elapsed_ms` se compare a un seuil de 500 ms.
    EtatAscenseurRecu etat;
    etat.elevatorId = msg->elevator_id();
    etat.etageActif = static_cast<int32_t>(msg->active_floor());
    etat.etageCible = static_cast<int32_t>(msg->target_floor());
    etat.departTick = static_cast<int32_t>(msg->depart_tick() & 0x7FFFFFFF);
    etat.elapsedMs = static_cast<int32_t>(msg->elapsed_ms() & 0x7FFFFFFF);

    // BORNE DE FILE. Sans mod ascenseur installe, personne ne draine : la file grossirait d'un
    // element par transition et par rappel periodique, indefiniment. On jette les PLUS ANCIENS —
    // un etat perime n'a aucune valeur, le dernier recu porte toute la verite.
    //
    // ⚠️ 256 ET PAS 64. A la connexion, le serveur envoie l'etat de TOUTES les cabines d'un coup :
    // 98 aujourd'hui. Un plafond de 64 en jetait 34 — silencieusement, et seulement pour les
    // cabines les plus anciennes de la liste. Mesure du 2026-08-24 : une cabine marchait, sa
    // voisine non, sans rien dans aucun journal pour distinguer les deux.
    constexpr std::size_t kFileMax = 256;
    while (g_ascenseursRecus.size() >= kFileMax)
    {
        g_ascenseursRecus.pop_front();
    }
    g_ascenseursRecus.push_back(etat);
    ++g_ascenseursTotalRecus;
}

void NetworkGameSystem::HandleDeviceState(const cyberpunk_rp::protocol::DeviceStateMsg* msg)
{
    if (msg == nullptr || msg->device() == 0)
    {
        return;
    }
    EtatAppareilRecu etat;
    etat.device = msg->device();
    etat.famille = static_cast<int32_t>(msg->famille());
    etat.etat = static_cast<int32_t>(msg->etat());
    etat.proprietaire = msg->proprietaire();
    etat.tenants = static_cast<int32_t>(msg->tenants());

    // BORNE DE FILE, meme raison que les ascenseurs : sans mod appareils installe, personne ne
    // draine et la file grossirait sans borne. On jette les PLUS ANCIENS — un etat perime n'a
    // aucune valeur.
    //
    // ⚠️ 512 ET PAS 256 : un appareil change bien plus souvent qu'une cabine (chaque franchissement
    // de porte automatique en est un), et un joueur qui traverse un lobby peut en accumuler
    // plusieurs dizaines avant le prochain drainage. La lecon du plafond a 64 des ascenseurs — 34
    // cabines jetees en silence — coute moins cher a appliquer qu'a redecouvrir.
    constexpr std::size_t kFileMax = 512;
    while (g_appareilsRecus.size() >= kFileMax)
    {
        g_appareilsRecus.pop_front();
    }
    g_appareilsRecus.push_back(etat);
    ++g_appareilsTotalRecus;
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

// Decode une chaine hexadecimale en octets. Rend un vecteur VIDE des que quoi que ce soit cloche
// (longueur impaire, caractere hors [0-9a-fA-F]) : un blob a moitie decode serait pire que pas de
// blob — il passerait la validation de taille du serveur en portant n'importe quoi.
static std::vector<uint8_t> DeHex(const std::string& hex)
{
    std::vector<uint8_t> out;
    if (hex.empty() || (hex.size() % 2) != 0)
    {
        return out;
    }
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        int haut = -1, bas = -1;
        for (int k = 0; k < 2; ++k)
        {
            const char c = hex[i + static_cast<std::size_t>(k)];
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
            (k == 0 ? haut : bas) = v;
        }
        if (haut < 0 || bas < 0)
        {
            return {};
        }
        out.push_back(static_cast<uint8_t>((haut << 4) | bas));
    }
    return out;
}

Red::CString NetworkGameSystem::Tessera_LireEsthetique(
    const Red::Handle<RED4ext::IScriptable>& aEtat)
{
    std::string hex;
    std::string erreur;
    if (!Tessera::EsthetiqueV::Lire(aEtat.instance, hex, erreur))
    {
        // ⚠️ LE REFUS EST JOURNALISE, TOUJOURS. Le retour est une chaine vide, et une chaine vide
        // est indiscernable d'une autre : sans cette ligne, « non finalise » (un moment) et
        // « classe inattendue » (un bug d'appelant) donneraient exactement le meme silence.
        SDK->logger->WarnF(PLUGIN, "LireEsthetique REFUS : %s", erreur.c_str());
        return Red::CString("");
    }
    SDK->logger->InfoF(PLUGIN, "LireEsthetique : %zu caracteres hex (%zu octets)", hex.size(),
                       hex.size() / 2);
    return Red::CString(hex.c_str());
}

// Decoupe « nom:index;nom:index;... » en paires. Format volontairement TRIVIAL : il traverse la
// frontiere redscript -> C++, ou seuls des scalaires et des chaines passent commodement, et il
// reste lisible a l'oeil dans un journal — ce qui est TOUT l'interet de l'esthetique transparente.
//
// ⚠️ Une entree malformee est SAUTEE, pas fatale. Perdre une option coute un detail d'apparence ;
// rejeter la creation coute le personnage. Le desequilibre est net, et le compte des sautees est
// journalise pour qu'un format casse ne passe pas inapercu.
static std::vector<std::pair<std::string, uint32_t>> DecouperOptions(const std::string& brut,
                                                                     size_t& sautees)
{
    std::vector<std::pair<std::string, uint32_t>> sortie;
    sautees = 0;
    size_t debut = 0;
    while (debut <= brut.size())
    {
        const size_t fin = brut.find(';', debut);
        const std::string entree = brut.substr(debut, fin == std::string::npos ? std::string::npos
                                                                               : fin - debut);
        if (!entree.empty())
        {
            const size_t sep = entree.find(':');
            if (sep == std::string::npos || sep == 0 || sep + 1 >= entree.size())
            {
                ++sautees;
            }
            else
            {
                const std::string nom = entree.substr(0, sep);
                const std::string val = entree.substr(sep + 1);
                try
                {
                    // `stoul` et non `stoi` : l'index « inactif » du moteur vaut 0xFFFFFFFF, qui
                    // deborde un `int` signe et leverait `out_of_range`.
                    sortie.emplace_back(nom, static_cast<uint32_t>(std::stoul(val)));
                }
                catch (...)
                {
                    ++sautees;
                }
            }
        }
        if (fin == std::string::npos) break;
        debut = fin + 1;
    }
    return sortie;
}

bool NetworkGameSystem::Tessera_CreerPersonnage(const Red::CString& pseudonyme, uint64_t record,
                                                uint64_t apparence, const Red::CString& origine,
                                                const Red::CString& esthetiqueHex,
                                                bool corpsMasculin, bool cerveauMasculin,
                                                const Red::CString& optionsApparence)
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

    // L'ORIGINE (corpo / nomade / gosse des rues) gouverne la dotation de depart cote serveur,
    // eurodollars compris. VIDE EST LEGITIME : l'ecran du lobby ne la posait pas encore quand ce
    // champ a ete ajoute, et un client plus ancien ne l'envoie pas — le serveur retombe alors sur
    // sa dotation de repli. On ne refuse donc PAS une creation sans origine ici.
    const std::string org = origine.c_str() != nullptr ? std::string(origine.c_str()) : std::string();

    // ── L'ESTHETIQUE, TRANSPORTEE TELLE QUELLE ────────────────────────────────────────────────
    //
    // VIDE EST LE CAS NORMAL aujourd'hui : le createur de personnage n'existe pas encore, donc
    // personne n'a de blob a proposer, et le serveur retombe sur son repli. On n'echoue donc PAS
    // sur une esthetique absente.
    //
    // ⚠️ On distingue quand meme « absente » de « MAL FORMEE » dans le journal. Une chaine non
    // vide qui se decode en zero octet est une ERREUR de l'appelant, et elle serait autrement
    // indiscernable du cas normal — le joueur verrait un passant generique sans que rien ne le
    // dise. Meme famille que les gardes muettes qui ont deja coute des sessions entieres.
    const std::string hex = esthetiqueHex.c_str() != nullptr ? std::string(esthetiqueHex.c_str())
                                                            : std::string();
    const std::vector<uint8_t> blob = DeHex(hex);
    if (!hex.empty() && blob.empty())
    {
        SDK->logger->WarnF(PLUGIN,
            "CreateCharacter : esthetique MAL FORMEE (%zu caracteres hex), envoyee VIDE",
            hex.size());
    }

    // ── L'ESTHETIQUE TRANSPARENTE ─────────────────────────────────────────────────────────────
    //
    // Le blob ci-dessus est opaque et sert a habiller les avatars DISTANTS. Celle-ci est la
    // RECETTE du V du joueur — « hairstyle:12;skin_color:3 » — la seule forme que le moteur de
    // customisation sait rejouer, et la seule qu'une edition future pourra modifier.
    const std::string optionsBrutes = optionsApparence.c_str() != nullptr
                                          ? std::string(optionsApparence.c_str())
                                          : std::string();
    size_t sautees = 0;
    const auto options = DecouperOptions(optionsBrutes, sautees);
    if (sautees > 0)
    {
        SDK->logger->WarnF(PLUGIN,
            "CreateCharacter : %zu option(s) d'apparence MAL FORMEE(S) ignoree(s) sur %zu",
            sautees, sautees + options.size());
    }

    SDK->logger->InfoF(PLUGIN,
        "CreateCharacter : « %s » record %llu apparence %llu origine « %s » esthetique %zu o, "
        "%zu option(s) transparente(s)",
        nom.c_str(), record, apparence, org.c_str(), blob.size(), options.size());

    flatbuffers::FlatBufferBuilder builder;
    const auto pseudo = builder.CreateString(nom);
    const auto org_off = builder.CreateString(org);
    std::vector<flatbuffers::Offset<cyberpunk_rp::protocol::OptionApparence>> options_off;
    options_off.reserve(options.size());
    for (const auto& o : options)
    {
        const auto n = builder.CreateString(o.first);
        options_off.push_back(cyberpunk_rp::protocol::CreateOptionApparence(builder, n, o.second));
    }
    const auto options_vec = builder.CreateVector(options_off);
    // Vecteur de taille zero plutot qu'offset nul : les deux se lisent pareil cote serveur
    // (`esthetique()` rend nullptr ou un vecteur vide), et l'uniformite evite un cas de plus.
    const auto esth_off = builder.CreateVector(blob);
    const auto req = cyberpunk_rp::protocol::CreateCreateCharacter(
        builder, pseudo, record, apparence, org_off, esth_off, corpsMasculin, cerveauMasculin,
        options_vec);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_CreateCharacter, req.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    return true;
}

// ⭐⭐ LE DEPART VOLONTAIRE — « quitter le jeu », par opposition a une coupure subie.
//
// Le serveur distingue les deux depuis toujours (`gateway.rs`) : une coupure RESERVE la place
// quelques minutes — on a pu perdre le reseau et vouloir revenir —, un depart volontaire la
// LIBERE IMMEDIATEMENT, « sa place retourne au pot commun tout de suite ». Mais rien, cote client,
// n'envoyait jamais ce message : tout depart passait donc pour une coupure, et un joueur qui
// quittait proprement bloquait une place pendant dix minutes.
//
// Demande de Lucas, 2026-08-22 : « on aura le bouton quitter le jeu qui nous deconnecte du serveur,
// libere le personnage, et qu'il y ait un slot ouvert pour les suivants. »
//
// ⚠️ ON VIDE LA FILE D'ENVOI. Le processus se ferme dans la foulee ; un message « fiable » pose
// dans une file qu'on ne pousse jamais n'arrive jamais, et le serveur retomberait sur le chemin
// « coupure ». `FlushMessagesOnConnection` est la difference entre un depart propre et un depart
// qui en a l'air.
bool NetworkGameSystem::Tessera_QuitterServeur()
{
    if (m_pInterface == nullptr)
    {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto req = cyberpunk_rp::protocol::CreateLeave(builder);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_Leave, req.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
    m_pInterface->FlushMessagesOnConnection(m_hConnection);
    m_personnageIncarne = 0;
    SDK->logger->Info(PLUGIN, "Leave envoye et pousse : depart volontaire, la place est liberee");
    return true;
}

bool NetworkGameSystem::Tessera_ChoisirPersonnage(uint64_t id)
{
    if (m_pInterface == nullptr)
    {
        SDK->logger->Warn(PLUGIN, "SelectCharacter ignore : pas de connexion");
        return false;
    }
    // ⭐⭐ ZERO EST UN ORDRE VALIDE : « SORTIR DU MONDE SANS QUITTER LE SERVEUR ».
    //
    // ⛔ L'ancien garde le REFUSAIT (`|| id == 0`), et le commentaire d'a cote le savait :
    // « ce chemin-la est de toute facon refuse par ce meme garde ». Le serveur, lui, le traite
    // depuis toujours — il repasse le client en `AwaitingSelection`, sa connexion et sa place
    // restent (`gateway.rs`, branche `SelectionDemandee::Sortir`). Le fil etait mort d'un seul
    // cote, et le seul appelant (`UiKitRetourLobby.reds`) n'avait donc jamais rien fait.
    //
    // C'est la demande de Lucas du 2026-08-22 : « que ca supprime le personnage comme si on etait
    // deconnecte, sauf qu'on reste connecte au serveur, pour qu'on puisse changer de personnage
    // sans perdre notre place. »
    if (id == 0)
    {
        SDK->logger->Info(PLUGIN, "SelectCharacter(0) : sortie du monde, la connexion RESTE ouverte");
        // ⚠️ ON OUBLIE LE PERSONNAGE INCARNE. Sans ca, la reprise apres reconnexion nous
        // remettrait d'office dans un personnage qu'on vient de quitter volontairement — et le
        // lobby serait sauté sans que personne ne comprenne pourquoi.
        m_personnageIncarne = 0;
    }
    else
    {
        SDK->logger->InfoF(PLUGIN, "SelectCharacter : id %llu", id);
    }
    // Retenu pour la REPRISE apres une reconnexion (voir `HandleCharacterList`). Sans lui, un
    // joueur qui revient est connecte mais n'incarne personne : la Gateway retient tout ce qu'il
    // envoie, le Shard reste a zero joueur, et l'ecran reste vide (F-PLF-024).
    //
    // Seuls les id non nuls arrivent ici (le garde ci-dessus refuse zero), donc « sortir du monde »
    // par `SelectCharacter(0)` ne peut pas effacer cette memoire. Sans consequence aujourd'hui :
    // ce chemin-la est de toute facon refuse par ce meme garde (cf. `UiKitRetourLobby.reds`, qui
    // l'appelle et n'a jamais ete mesure en jeu).
    if (id != 0) m_personnageIncarne = id;

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
    constexpr uint8_t kKindAction = 0;
    constexpr uint8_t kKindStim = 1;

    // -- kind=0 : L'EVENEMENT D'ACTION, QUI ETAIT JETE PAR TERRE ------------------------------
    //
    // Le serveur relaie ces evenements aux voisins d'AoI depuis longtemps, avec des tests. Ce
    // handler, lui, les ignorait tous : seul le stimulus (kind=1) etait traite. Un saut, un tir,
    // un rechargement arrivaient chez l'observateur et n'y produisaient rien.
    //
    // Ce qu'on y gagne, precisement : l'animation se declenche a l'ARRIVEE DE L'EVENEMENT au lieu
    // d'attendre que le prochain instantane porte `locomotion == 6`. A 25 Hz de diffusion, c'est
    // jusqu'a 40 ms sur un geste qui en dure 800 — plus le delai du tampon d'interpolation.
    //
    // ⚠️ La POSITION reste pilotee par le flux de poses, et c'est voulu. Un evenement ne deplace
    // rien : il declenche. Confondre les deux ferait sauter l'avatar deux fois, une fois par
    // l'evenement et une fois par la pose — le defaut exact que la separation des canaux evite.
    if (event->kind() == kKindAction)
    {
        static constexpr std::uint8_t kActionSaut = 0;   // eACTION_JUMP
        const auto acteur = m_networkedEntitiesLookup.find(event->actor());
        if (acteur == m_networkedEntitiesLookup.end())
        {
            // Meme regle que pour le stimulus : pas de repli acceptable. Jouer l'animation sur
            // quelqu'un d'autre serait pire que ne rien jouer.
            g_telemetrie.Evenement("action_recue", event->actor(), "acteur_absent");
            return;
        }
        if (event->action() == kActionSaut)
        {
            bool pousse = false;
            Red::CallVirtual(this, "TesseraPousserFranchissement", pousse, acteur->second, true);
            g_telemetrie.Evenement("action_recue", event->actor(),
                                   pousse ? "saut" : "saut_refuse");
        }
        else
        {
            // Un code inconnu se journalise et s'ignore. Le schema est append-only : un client
            // plus ancien que le serveur DOIT pouvoir recevoir une action qu'il ne connaît pas.
            g_telemetrie.Evenement("action_recue", event->actor(), "code_inconnu");
        }
        return;
    }

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

uint64_t NetworkGameSystem::IdReseauDe(const RED4ext::ent::EntityID entityId) const
{
    for (const auto& [idReseau, idJeu] : m_networkedEntitiesLookup)
    {
        if (idJeu == entityId)
        {
            return idReseau;
        }
    }
    return 0;
}

void NetworkGameSystem::RapporterMontage(uint64_t vehiculeReseau, uint32_t siege, bool monte)
{
    if (m_pInterface == nullptr || vehiculeReseau == 0)
    {
        return;
    }

    // Constantes du GEL du schema (protocol.fbs, EntityInteraction) — surtout pas une
    // numerotation locale : 3=Mount, 4=Unmount, et `param` = index de siege desire.
    constexpr uint8_t kKindMount = 3;
    constexpr uint8_t kKindUnmount = 4;

    SDK->logger->InfoF(PLUGIN, "%s vehicule %llu siege %u", monte ? "Montee" : "Descente",
        vehiculeReseau, siege);

    // ── LA VOITURE OU JE SUIS ASSIS N'EST PLUS PLACEE PAR LE SERVEUR ───────────────────────
    //
    // MESURE le 2026-08-14 (F-VEH-033), Lucas au volant : « le vehicule est a moitie dans le sol »,
    // « le moteur demarre et la radio marche, mais impossible de l'utiliser », et au moment de
    // s'asseoir « ca a teleporte la voiture ailleurs ».
    //
    // Un seul mecanisme derriere les trois. Le serveur ecrit la pose du vehicule depuis celle du
    // CONDUCTEUR, et ce client la reappliquait au vehicule a chaque frame. D'ou une BOUCLE : le
    // vehicule est pose sur les pieds du joueur (origine caisse != origine pantin, donc il
    // s'enfonce), le joueur s'enfonce avec, la position remontee s'enfonce encore. Et un placement
    // autoritaire a 50 Hz ecrase tout ce que le modele de conduite calcule — le volant ne peut pas
    // gagner contre un `Teleport` par frame.
    //
    // Le client qui conduit POSSEDE sa voiture : elle appartient au moteur local, pas au fil.
    // (La remontee de la pose du VEHICULE — `VehiclePlayerState`, deja gele au protocole et
    // toujours pas alimente — reste le chantier de niveau 2 ; sans elle, les AUTRES clients voient
    // encore la voiture suivre la position du conducteur, avec le decalage que ca implique.)
    //
    // ⚠️ SEUL LE CONDUCTEUR POSSEDE, ET LA NUANCE N'EST PAS COSMETIQUE (Lucas, 2026-08-14).
    //
    // La premiere version coupait le placement pour TOUT occupant. Consequence mesuree : le
    // PASSAGER cessait lui aussi de recevoir la pose du vehicule — donc, sur son ecran, la voiture
    // ne bougeait plus du tout, et lui avec, pendant que le conducteur s'eloignait. « J'ai commence
    // a me deplacer avec le vehicule, l'autre joueur n'a pas suivi. »
    //
    // Un passager n'a aucune information locale sur ou va la voiture : la sienne vient du fil, et
    // elle est la SEULE qu'il ait. Couper le placement pour lui, c'est le laisser a l'arret dans un
    // vehicule parti sans lui.
    constexpr uint32_t kSiegeConducteur = 0; // meme convention que `IndexDeSiege`/`SiegeDeIndex`
    if (siege == kSiegeConducteur)
    {
        g_vehiculeLocalMonte = monte ? vehiculeReseau : 0;
    }
    else if (!monte && g_vehiculeLocalMonte == vehiculeReseau)
    {
        // Descente d'un siege passager alors qu'on possedait la voiture (changement de place du
        // conducteur vers l'arriere) : on rend la propriete, sinon elle resterait acquise a vie.
        g_vehiculeLocalMonte = 0;
    }

    flatbuffers::FlatBufferBuilder builder;
    const auto ei = cyberpunk_rp::protocol::CreateEntityInteraction(
        builder, vehiculeReseau, monte ? kKindMount : kKindUnmount, siege);
    const auto env = cyberpunk_rp::protocol::CreateClientEnvelope(
        builder, cyberpunk_rp::protocol::ClientMsg_EntityInteraction, ei.Union());
    builder.Finish(env);
    m_pInterface->SendMessageToConnection(m_hConnection, builder.GetBufferPointer(),
        builder.GetSize(), k_nSteamNetworkingSend_Reliable, nullptr);
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
    // ⭐ Le sexe du corps, qui choisit la TENUE de l'avatar (voir `NetworkAppearance`). Defaut
    // `true` cote schema : un serveur qui ne l'emet pas se comporte comme avant ce champ.
    appearance.corpsMasculin = sync->spec()->corps_masculin();

    // ── L'ARME EN MAIN, TRANSPORTEE PAR `garments` ────────────────────────────────────────────
    //
    // Le champ existait dans le schema depuis l'origine et n'avait jamais eu d'emetteur. Il porte
    // desormais l'arme tenue par le joueur que cet avatar represente : un seul `EquippedItem`.
    //
    // ⚠️ VECTEUR ABSENT = MAINS VIDES, et c'est un etat A PART ENTIERE, pas un « pas d'info ». Le
    // serveur retire le garment quand le joueur range son arme (`appearance_relay.rs`) : traiter
    // l'absence comme « on ne sait pas » laisserait l'arme dans les mains de l'avatar pour
    // toujours. On ecrit donc explicitement 0.
    //
    // ⚠️ ON LIT DESORMAIS TOUT LE VECTEUR, plus seulement `Get(0)`. Il porte deux choses distinguees
    // par `drawn` : l'arme EN MAIN (vrai, au plus une) et les VETEMENTS portes (faux, dans l'ordre
    // de pose decide par le serveur). L'arme reste emise EN PREMIER cote serveur — cette position
    // etait la seule information disponible ici avant aujourd'hui, et la garder evite qu'un client
    // non mis a jour ne degaine un t-shirt.
    appearance.arme = 0;
    appearance.vetements.clear();
    if (const auto* garments = sync->spec()->garments(); garments != nullptr)
    {
        for (flatbuffers::uoffset_t i = 0; i < garments->size(); ++i)
        {
            const auto* g = garments->Get(i);
            if (g == nullptr)
            {
                continue;
            }
            if (g->drawn())
            {
                appearance.arme = g->item();
            }
            else if (g->item() != 0)
            {
                appearance.vetements.push_back(g->item());
            }
        }
    }
    // ── L'ESTHETIQUE, RECOPIEE DEPUIS LE FIL ──────────────────────────────────────────────────
    //
    // On copie au lieu de garder le pointeur : le tampon FlatBuffers appartient au message recu et
    // meurt avec lui, alors que `m_appearances` survit jusqu'au spawn — qui arrive PLUS TARD (le
    // serveur pousse l'apparence a l'entree en AoI, avant le Snapshot qui porte l'entite).
    appearance.esthetique.clear();
    if (const auto* blob = sync->spec()->esthetique(); blob != nullptr && blob->size() > 0)
    {
        appearance.esthetique.assign(blob->data(), blob->data() + blob->size());
    }
    m_appearances[id] = appearance;

    // ⚠️ LA TAILLE EST JOURNALISEE, et c'est le seul instrument qui prouve que le blob TRAVERSE.
    // Les tests du serveur encodaient et decodaient en Rust — verts des deux cotes, muets sur le
    // fil. Cette ligne est ce qui distingue « le serveur a relaye » de « le client a recu ».
    SDK->logger->InfoF(PLUGIN,
        "AppearanceSync %llu : record=%llu apparence=%llu arme=%llu esthetique=%zu o vetements=%zu",
        id, appearance.baseRecord, appearance.appearance, appearance.arme,
        appearance.esthetique.size(), appearance.vetements.size());

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

/// Vrai tant qu'on accepte d'attendre pour ce voisin. Faux passe le delai.
///
/// ⚠️ **L'attente est BORNEE, et c'est delibere.** Si le serveur n'envoie JAMAIS ce qu'on attend,
/// patienter indefiniment rendrait le voisin INVISIBLE pour toujours — une panne muette, pire que
/// le corps generique qu'on cherche a eviter. Passe le delai on prend ce qu'on a, et on le dit.
bool NetworkGameSystem::AttendreEncore(uint64_t networkId)
{
    static std::map<uint64_t, std::chrono::steady_clock::time_point> s_depuis;
    constexpr auto kDelai = std::chrono::seconds(3);
    const auto maintenant = std::chrono::steady_clock::now();
    auto& debut = s_depuis[networkId];
    if (debut == std::chrono::steady_clock::time_point{})
    {
        debut = maintenant;
    }
    return (maintenant - debut) < kDelai;
}

bool NetworkGameSystem::SpawnNetworkEntity(uint64_t networkId, const RED4ext::Vector4& worldPosition)
{
    TesseraAppliquerDrapeauxUneFois();
    // Le record vient du SERVEUR (AppearanceSync). Le repli n'est utilise que si aucune apparence
    // n'est encore connue pour cet id — et il se signale, parce qu'un avatar de repli silencieux
    // est indistinguable d'un avatar correct.
    RED4ext::TweakDBID record;
    RED4ext::CName appearanceName(static_cast<uint64_t>(0));
    const auto it = m_appearances.find(networkId);

    // ── ⭐⭐ ON ATTEND AUSSI LA FICHE D'APPARENCE, PAS SEULEMENT LE RECORD ────────────────────
    //
    // MESURE DU 2026-08-23, 17:29 — la chronologie est sans ambiguite :
    //
    //     17:29:08.125  AppearanceSync 12 : ... esthetique=0 o     <- 1re annonce, SANS fiche
    //     17:29:08.161  Spawn entite reseau 12                      <- on fabrique (voie sure)
    //     17:29:09.086  AppearanceSync 12 : ... esthetique=344 o   <- la fiche arrive UNE SECONDE
    //                                                                  PLUS TARD
    //
    // Le serveur emet DEUX `AppearanceSync` pour le meme voisin : la premiere porte le preset du
    // catalogue, la seconde la vraie fiche. Fabriquer sur la premiere donne un passant — et **ce
    // corps-la ne guerit jamais**, parce que le record et l'esthetique sont figes a la naissance.
    //
    // ⚠️ C'est exactement le piege que le dossier avatar avait predit sur l'ordre d'emission cote
    // Shard. On le borde ICI plutot que la-bas : le client ne doit pas dependre de l'ordre dans
    // lequel un serveur lui parle, et une garde locale protege aussi des serveurs tiers.
    //
    // La fiche entre donc dans la MEME attente bornee que le record — meme delai, meme repli
    // bruyant. Un personnage qui n'en a legitimement pas (ancien preset de 16 octets) attend le
    // delai puis part en voie sure, ce qui est le comportement d'avant.
    const bool ficheAttendue = (it == m_appearances.end() || it->second.esthetique.empty());

    if (it != m_appearances.end() && it->second.baseRecord != 0 && !ficheAttendue)
    {
        record = RED4ext::TweakDBID(it->second.baseRecord);
        appearanceName = RED4ext::CName(it->second.appearance);
    }
    else if (it != m_appearances.end() && it->second.baseRecord != 0 && ficheAttendue
             && AttendreEncore(networkId))
    {
        JournalRalenti("fiche d'apparence ATTENDUE",
                       " — le record est la, la fiche pas encore. Fabriquer maintenant donnerait un "
                       "passant DEFINITIF (le corps ne guerit pas). Retente au prochain snapshot.");
        return false;
    }
    else if (it != m_appearances.end() && it->second.baseRecord != 0)
    {
        // Delai ecoule : on prend le record connu, sans fiche. La voie enrichie ne se declenchera
        // pas, et c'est dit plus bas par son propre silence.
        record = RED4ext::TweakDBID(it->second.baseRecord);
        appearanceName = RED4ext::CName(it->second.appearance);
    }
    else
    {
        // ── ON ATTEND LE RECORD PLUTOT QUE DE NAITRE FAUX ──────────────────────────────────
        //
        // MESURE le 2026-08-14, deux clients : le second a fait naitre les TROIS voitures serveur
        // en `Character.CitizenBikerMale` — le repli — parce que l'`AppearanceSync` n'etait pas
        // encore arrivee. Il avait donc trois PIETONS la ou l'autre voyait trois voitures, et
        // « je ne vois pas la voiture, je ne peux pas monter » n'etait pas un bug de vehicule.
        //
        // ⚠️ ET CE CORPS-LA NE GUERIT JAMAIS. `ApplyAppearance` change l'APPARENCE (un `CName`),
        // pas le RECORD : un pieton ne devient pas une voiture quand l'apparence arrive enfin.
        // Le repli etait donc definitif, alors qu'il se presentait comme temporaire.
        //
        // Differer ne coute rien : l'appelant retente le spawn a CHAQUE snapshot tant que l'entite
        // manque (c'est deja le mecanisme du creux B0). Une frame d'attente vaut mieux qu'un corps
        // faux pour la session entiere.
        static std::map<uint64_t, std::chrono::steady_clock::time_point> s_attenteApparence;
        const auto maintenant = std::chrono::steady_clock::now();
        auto& depuis = s_attenteApparence[networkId];
        if (depuis == std::chrono::steady_clock::time_point{})
        {
            depuis = maintenant;
        }

        // ⚠️ L'ATTENTE EST BORNEE, et c'est delibere. Si le serveur n'envoie JAMAIS d'apparence
        // pour cette entite, attendre indefiniment la rendrait INVISIBLE pour toujours — une
        // panne muette, pire que le corps faux qu'on cherche a eviter. Passe le delai, on prend le
        // repli et on le dit fort : un repli visible se diagnostique, un vide ne se voit pas.
        constexpr auto kDelaiApparence = std::chrono::seconds(3);
        if (maintenant - depuis < kDelaiApparence)
        {
            JournalRalenti("apparence ATTENDUE",
                " — spawn differe le temps que l'AppearanceSync arrive (retente au prochain "
                "snapshot)");
            return false;
        }

        record = RED4ext::TweakDBID(kFallbackAvatarRecord);
        SDK->logger->WarnF(PLUGIN,
            "Spawn %llu SANS apparence serveur apres 3 s d'attente — repli %s. Ce corps ne "
            "guerira PAS (le record ne se change pas apres coup) : cherche pourquoi le serveur "
            "n'envoie pas d'AppearanceSync pour cette entite.", networkId, kFallbackAvatarRecord);
    }

    const RED4ext::Quaternion worldOrientation = { 0.0f, 0.0f, 0.0f, 1.0f };
    RED4ext::ent::EntityID entityId;

    // ── LA VOIE ENRICHIE D'ABORD, LA VOIE SURE ENSUITE ─────────────────────────────────────
    //
    // Codeware ne sait pas porter un V : `DynamicEntitySpec` n'a AUCUN champ de customisation.
    // Le corps sortait donc du catalogue des passants, alors que la fiche d'apparence de ce
    // joueur etait DEJA rangee dans `m_appearances[...].esthetique` — sans aucun lecteur.
    //
    // ⚠️ Toute la conception tient dans une phrase : **cette tentative peut echouer, jamais
    // laisser sans corps.** Drapeau eteint, charge invalide, systeme injoignable, entite non
    // retrouvee — chaque sortie retombe sur `SpawnNetworkAvatar` juste en dessous. Il n'existe
    // pas de chemin ou l'on perd un voisin parce qu'on a voulu lui donner son visage.
    if (it != m_appearances.end() && !it->second.esthetique.empty())
    {
        const auto essai = Tessera::SpawnEnrichi::Tenter(networkId, it->second.esthetique,
                                                         worldPosition, it->second.corpsMasculin);
        if (essai.tente && !essai.diag.empty())
        {
            // Journalise MEME en cas de succes : c'est ce qui distingue « la voie enrichie a
            // marche » de « elle n'a jamais ete tentee », deux causes opposees derriere le meme
            // symptome a l'ecran (un visage generique).
            SDK->logger->InfoF(PLUGIN, "[spawn enrichi %llu] %s", networkId, essai.diag.c_str());
        }
        if (essai.entite.IsDefined())
        {
            entityId = essai.entite;
            m_networkedEntitiesLookup.insert(std::make_pair(networkId, entityId));
            SDK->logger->InfoF(PLUGIN, "Spawn entite reseau %llu -> entity %llu (voie ENRICHIE, "
                                       "porte le V de ce joueur)", networkId, entityId.hash);

            // ── ⭐ LA FICHE DU CORPS, UNE SEULE FOIS ──────────────────────────────────────────
            //
            // Verdict de Lucas, 2026-08-23 : le corps porte le bon visage, il est SEUL — et il est
            // « statique, pas anime, pas relie au joueur ».
            //
            // ⚠️ ET JE N'AVAIS PLUS D'INSTRUMENT POUR LE DIRE. Le `Failed to get the entity` a ete
            // retire au correctif precedent (il ecrivait une ligne par frame et par avatar). Un
            // journal sans echec ne prouvait donc plus rien : zero echec, ou zero ligne. C'est la
            // CINQUIEME fois de la journee qu'un mecanisme auxiliaire fait echouer la mesure qu'il
            // devait servir.
            //
            // Quatre questions, une fois par corps, et chacune elimine une cause :
            //   · l'entite se RESOUT-elle ? (sinon, tout le pilotage est aveugle)
            //   · quelle CLASSE ? (un `NPCPuppet` se pilote, un `gameObject` nu non)
            //   · a-t-elle un CONTROLEUR D'IA ? (sans lui, pas de commande de marche — F-PLY-205)
            //   · ou est-elle ? (compare a la position demandee : le placement a-t-il pris ?)
            {
                // ── ⚠️ LES DEUX VOIES SEPAREMENT, ET « S'EST-ELLE LIEE » SEPAREMENT DE « A-T-ELLE
                //    TROUVE » ────────────────────────────────────────────────────────────────────
                //
                // `IRRESOLVABLE` ne distinguait pas trois pannes : l'appel ne se lie pas, il se lie
                // et rend nul, ou le systeme lui-meme est injoignable. C'est la SEPTIEME fois de la
                // journee qu'un message unique couvre plusieurs causes opposees — et a chaque fois
                // ca a coute un tir.
                //
                // ⚠️ Piste ouverte, a confirmer ici : le script declare
                // `GameInstance.FindEntityByID( gi, entityID )` — la GameInstance en PREMIER
                // parametre. `Red::CallStatic("ScriptGameInstance", ...)` l'injecte pour les
                // accesseurs de systeme, mais rien n'etablit qu'il le fasse ici.
                Red::Handle<Red::IGameSystem> sysDyn;
                const bool sysLie = Red::CallStatic("ScriptGameInstance", "GetDynamicEntitySystem", sysDyn);
                Red::Handle<Red::Entity> parDyn;
                const bool dynLie = sysLie && sysDyn != nullptr
                                    && Red::CallVirtual(sysDyn, "GetEntity", parDyn, entityId);
                Red::Handle<Red::Entity> parMonde;
                const bool mondeLie = Red::CallStatic("ScriptGameInstance", "FindEntityByID", parMonde, entityId);

                // ── ⭐ ON DEMANDE AU MOTEUR CE QU'IL EXPOSE, AU LIEU DE DEVINER UN NOM DE PLUS ──
                //
                // `FindEntityByID` ne se lie pas (`appel=NON LIE`, mesure du 2026-08-23 18:16). Le
                // script l'ecrit pourtant `GameInstance.FindEntityByID( gi, entityID )`.
                //
                // ⚠️ Essayer des noms au hasard serait la HUITIEME hypothese de la journee, apres
                // sept qui ont coute un tir chacune. Le RTTI sait, lui : on lui demande. C'est le
                // meme principe que lire le script decompile plutot que supposer une signature —
                // et que lire l'assertion de CDPR plutot que deviner ce que fait `Reserve`.
                //
                // ⚠️ UNE SEULE FOIS : `s_dejaDump` garde. Enumerer une classe RTTI a chaque spawn
                // ecrirait des centaines de lignes par minute.
                static bool s_dejaDump = false;
                if (!s_dejaDump)
                {
                    s_dejaDump = true;
                    auto* rtti = RED4ext::CRTTISystem::Get();
                    auto* cls = (rtti != nullptr) ? rtti->GetClass("ScriptGameInstance") : nullptr;
                    if (cls == nullptr)
                    {
                        SDK->logger->WarnF(PLUGIN, "[rtti] classe ScriptGameInstance INTROUVABLE — "
                                                   "ce n'est donc pas par la qu'il faut passer");
                    }
                    else
                    {
                        std::string trouvees;
                        std::uint32_t total = 0;
                        for (auto* fn : cls->staticFuncs)
                        {
                            if (fn == nullptr) { continue; }
                            ++total;
                            const char* nom = fn->fullName.ToString();
                            if (nom == nullptr) { continue; }
                            // On ne garde que ce qui parle d'entites : le reste noierait la ligne.
                            if (std::strstr(nom, "Entity") != nullptr
                                || std::strstr(nom, "entity") != nullptr)
                            {
                                trouvees += nom;
                                trouvees += " (";
                                trouvees += std::to_string(fn->params.size());
                                trouvees += "p) · ";
                            }
                        }
                        SDK->logger->InfoF(PLUGIN,
                                           "[rtti] ScriptGameInstance : %u statique(s), celles qui "
                                           "parlent d'entite -> %s",
                                           total, trouvees.empty() ? "AUCUNE" : trouvees.c_str());
                    }
                }
                SDK->logger->InfoF(PLUGIN,
                                   "[fiche %llu] voie DYN : systeme=%s appel=%s entite=%s · "
                                   "voie MONDE : appel=%s entite=%s",
                                   networkId,
                                   sysLie ? "ok" : "INJOIGNABLE",
                                   dynLie ? "lie" : "NON LIE",
                                   parDyn != nullptr ? "trouvee" : "nulle",
                                   mondeLie ? "lie" : "NON LIE",
                                   parMonde != nullptr ? "trouvee" : "nulle");

                const auto e = Cyberverse::Utils::GetDynamicEntity(entityId);
                if (!e.has_value() || e->instance == nullptr)
                {
                    SDK->logger->WarnF(PLUGIN, "[fiche %llu] entite %llu IRRESOLVABLE par les DEUX "
                                               "voies — le pilotage sera aveugle sur ce corps",
                                       networkId, entityId.hash);
                }
                else
                {
                    const char* classe = (e->instance->GetType() != nullptr)
                                             ? e->instance->GetType()->name.ToString() : "<sans type>";
                    Red::Handle<RED4ext::IScriptable> ia;
                    const bool aIa = Red::CallVirtual(e.value(), "GetAIControllerComponent", ia)
                                     && ia != nullptr;
                    const auto p = Cyberverse::Utils::Entity_GetWorldPosition(e.value());
                    SDK->logger->InfoF(PLUGIN,
                                       "[fiche %llu] classe=%s · IA=%s · pos=(%.1f %.1f %.1f) "
                                       "demandee=(%.1f %.1f %.1f)",
                                       networkId, classe, aIa ? "OUI" : "NON",
                                       p.X, p.Y, p.Z,
                                       worldPosition.X, worldPosition.Y, worldPosition.Z);
                }
            }

            // ── ⭐⭐⭐ ON VERROUILLE L'APPARENCE : LE PRESET NE DOIT PAS ECRASER LE VRAI V ─────
            //
            // ⛔ LE DEFAUT, mesure le 2026-09-01, chronologie du journal a la milliseconde :
            //
            //     17:40:18.225  Spawn ENRICHI 4 -> entity 10039790  (porte le V de ce joueur)
            //     17:40:21.432  AppearanceSync 4  ... arme=114125024658   <- un DEGAINAGE
            //
            // Trois secondes apres sa naissance, le corps enrichi se faisait REECRIRE son
            // apparence. Le declencheur n'a rien a voir avec l'apparence : c'est un changement
            // d'arme, qui fait re-emettre le spec entier.
            //
            // Et ce qui etait applique, hash resolu contre `appearance-presets.toml` :
            //     record     108153521249            = Character.CitizenRichMale
            //     apparence  17980800529530541511    = citizen__rich_ma_rich_10
            // ...soit l'apparence d'un PASSANT, posee sur le pantin qui porte le V du joueur.
            //
            // ⭐ CE N'EST UN BUG D'AUCUN DES DEUX COTES : ce sont DEUX MODELES D'APPARENCE qui
            // tournent en meme temps sur le meme pantin. Le modele PRESET (decision du
            // 2026-07-25, repli assume quand le corps au visage exact etait bloque) et le modele
            // ENRICHI, qui marche depuis. Le second a rendu le premier caduc sans le debrancher.
            //
            // ⭐⭐ Ca explique la coiffure DOUBLEE (la charge en pose une, le preset une autre),
            // le visage MELANGE, le corps DECAPITE (quand le preset resolu est une apparence de
            // `player_base_bodies`, sans tete par construction), et la NON-DETERMINATION mesuree
            // sur quatre lancements (F-PLY-357) : le resultat depend de l'avancement de la charge
            // au moment de la reecriture, et de l'instant du premier degainage.
            //
            // La voie SURE, elle, enregistrait deja son apparence juste apres `SpawnNetworkAvatar`
            // (plus bas). C'est cette symetrie qui manquait — pas un mecanisme neuf.
            //
            // ⚠️ ET CE N'EST PAS UN GEL. Le garde de `ApplyAppearance` ne bloque que la MEME
            // valeur : un vrai changement d'apparence (tenue, coiffure) porte un autre hash et
            // passera. On refuse la re-application du preset de naissance, pas les mises a jour.
            if (it->second.appearance != 0)
            {
                m_appliedAppearance[networkId] = it->second.appearance;
            }
            return true;
        }
        // ── L'APPEL EST PARTI, LE CORPS N'EST PAS ENCORE NE : ON PATIENTE ────────────────────
        //
        // ⚠️ **C'est ce `return` qui supprime le doublon.** Sans lui, on enchaînait sur la voie sûre
        // dans la même frame : un corps enrichi (invisible à nos tables, donc jamais piloté ni
        // effacé) PLUS un corps de passant, empilés — exactement ce que Lucas a vu deux fois.
        //
        // La création d'entité est asynchrone : chercher dans la frame de l'appel, c'est chercher un
        // corps qui n'existe pas encore. On rend donc `false` sans rien enregistrer, et l'appelant
        // — qui retente à chaque snapshot tant que l'entité manque — repassera ici. `Tenter` fera
        // alors la différence au lieu de rappeler le spawner.
        //
        // Un diagnostic VIDE signifie précisément « patiente » ; `Tenter` n'en pose un que lorsqu'il
        // a quelque chose à dire (succès, refus, ou abandon après 30 passages). C'est ce qui évite
        // trente lignes de journal par voisin.
        if (essai.attente)
        {
            return false;
        }
    }

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
    // ⚠️ CE COMMENTAIRE DISAIT « on ne monte pas au-delà : émettre plus vite que le serveur ne
    // DIFFUSE n'ajouterait que du trafic que personne ne lira ». C'est devenu faux le 2026-08-15,
    // et la correction vaut d'être écrite plutôt que la ligne effacée.
    //
    // Depuis le découplage (`snapshot_divider()`), le serveur SIMULE à 50 Hz mais ne DIFFUSE qu'à
    // 25 Hz. La moitié de ce qu'on émet n'atteint donc jamais un autre client sous forme de pose
    // rendue — et pourtant cette cadence reste la bonne, pour une raison différente de celle qui
    // l'avait justifiée :
    //
    //   · le SERVEUR, lui, consomme tout à 50 Hz. L'anti-triche, les interactions, la santé et la
    //     position autoritaire travaillent sur chaque paquet reçu. Émettre à 25 Hz rendrait la
    //     vérification de vitesse deux fois plus grossière et perdrait les gestes courts AVANT
    //     qu'ils n'atteignent l'autorité — c'est-à-dire là où plus rien ne peut les récupérer.
    //   · pour les autres CLIENTS, l'échantillon supplémentaire n'est pas perdu non plus : il est
    //     la matière du prochain segment d'interpolation. Un tampon interpole entre deux
    //     échantillons, il ne les invente pas.
    //
    // Cette période suit donc `default_tick_rate_hz()` (la SIMULATION), jamais `snapshot_rate_hz()`
    // (la diffusion). Confondre les deux ferait baisser la cadence d'émission avec celle de
    // diffusion, et dégraderait l'autorité serveur pour économiser une bande passante MONTANTE qui
    // n'a jamais été le problème (le goulot mesuré est descendant).
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

    // ── MODE ROBOT : on annonce le scénario, pas la position réelle ────────────────────────
    //
    // L'origine est la position du joueur au PREMIER passage, jamais une constante : le scénario
    // doit se dérouler là où l'instance a chargé, quel que soit le point d'apparition.
    if (g_robotActif)
    {
        if (!g_robotOrigineConnue)
        {
            g_robotOrigineX = X;
            g_robotOrigineY = Y;
            g_robotOrigineZ = Z;
            g_robotOrigineConnue = true;
            g_robotDebut = std::chrono::steady_clock::now();
            SDK->logger->InfoF(PLUGIN, "[robot] scenario demarre en (%.1f, %.1f, %.1f)", X, Y, Z);
        }
        const double t = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - g_robotDebut)
                             .count();
        const auto pose = Tessera::Sync::PoseDuRobot(t);
        if (pose.phase != g_robotPhase)
        {
            g_robotPhase = pose.phase;
            g_telemetrie.Evenement("robot_phase", static_cast<std::uint64_t>(pose.phase), "");
        }
        this->SendPositionUpdate(g_robotOrigineX + pose.dx, g_robotOrigineY + pose.dy,
                                 g_robotOrigineZ + pose.dz, pose.yaw, pose.locomotion);
        return;
    }

    this->SendPositionUpdate(X, Y, Z, Yaw);
}

void NetworkGameSystem::RendreAvatarsDistants(const float deltaTime)
{
    g_tempsLocalS += static_cast<double>(deltaTime);
    if (!g_horlogeRendu.Amorcee())
    {
        return; // aucun snapshot recu : rien a rendre, et surtout rien a deviner.
    }
    g_horlogeRendu.Avancer(deltaTime);
    const double instant = g_horlogeRendu.TempsRendu();

    // ── UN INSTRUMENT DOIT DISTINGUER L'ABSENCE DU SILENCE ─────────────────────────────────
    //
    // Le 2026-08-14, la première session instrumentée a produit **zéro** pose rendue — et rien
    // n'a permis de dire laquelle des trois causes c'était : aucun joueur voisin connu, un corps
    // jamais né, ou un rendu qui va bien mais n'a rien à faire. Les trois se ressemblaient
    // exactement : un fichier sans lignes `rx`.
    //
    // C'est le mode d'échec que la doctrine nomme (« pas d'entrée = pas fait ») appliqué à
    // l'outil lui-même. Deux nombres toutes les deux secondes le lèvent : combien de voisins on
    // CONNAÎT (un tampon existe), et pour combien on a un CORPS. `connus=0` accuse le serveur ou
    // l'appariement de compte ; `connus>0, corps=0` accuse la naissance côté moteur.
    static double s_depuisRecensement = 0.0;
    s_depuisRecensement += deltaTime;
    if (g_telemetrie.Active() && s_depuisRecensement >= 2.0)
    {
        s_depuisRecensement = 0.0;
        std::size_t avecCorps = 0;
        for (const auto& [networkId, tampon] : g_tamponsJoueurs)
        {
            if (m_networkedEntitiesLookup.find(networkId) != m_networkedEntitiesLookup.end())
            {
                ++avecCorps;
            }
        }
        // ── LE COMPTEUR DE RÉGRESSIONS DOIT SORTIR D'ICI, ET IL NE SORTAIT PAS ────────────
        //
        // `TamponPose::Regressions()` compte les fois où la timeline serveur a reculé franchement
        // pour une entité — l'unique symptôme d'un shard qui a redémarré sous les pieds des
        // joueurs. Son doc-comment disait « doit finir dans un journal » ; rien ne l'y mettait.
        //
        // C'est l'instrument qui aurait nommé la panne du 2026-08-15 tout seul, au lieu qu'il
        // faille comparer des profondeurs de tampon avant/après à la main. Le voir à 0 sur toute
        // une soirée est une information ; le voir grimper explique d'un coup une salve de
        // plaintes qui, sans lui, ressemblent à n'importe quoi d'autre.
        std::uint32_t regressions = 0;
        for (const auto& [networkId, tampon] : g_tamponsJoueurs)
        {
            regressions += tampon.Regressions();
        }
        char detail[96];
        std::snprintf(detail, sizeof(detail), "connus=%zu,corps=%zu,regressions=%u",
                      g_tamponsJoueurs.size(), avecCorps, regressions);
        g_telemetrie.Evenement("voisins", 0, detail);

        // -- L'ENUMERATION PAR RAYON N'A RIEN A FAIRE ICI (incident du 2026-08-19) -----------
        //
        // J'ai place `TesseraEtatsLocomotionAutour` dans cette passe, qui tourne toutes les deux
        // secondes. Elle balaie 40 m de PNJ natifs, resout un composant et lit un enum sur chacun.
        // Le jeu s'est fige et le WATCHDOG DU MOTEUR l'a tue :
        //
        //     engineWatchdog.cpp:198 — Watchdog timeout! (120 seconds)
        //
        // Le cout exact n'est pas etabli (une seule occurrence), mais la lecon ne depend pas de
        // lui : **une enumeration spatiale est une operation a la demande, pas une passe de fond.**
        // Le controle qu'elle sert est un tir unique — savoir si l'etat `Move` existe dans la
        // population — pas une surveillance. Elle vit donc dans la sonde du pont
        // (`politique|autour`), ou elle ne s'execute que quand on la demande.
        //
        // C'est la meme famille que le defaut du 2026-08-06 : ce qui coute cher par entite ne se
        // met pas dans une boucle qui voit toutes les entites.

        // -- CE QUE LE MOTEUR CROIT ETRE EN TRAIN DE FAIRE (F-PLY-123) ----------------------
        //
        // Tout ce qu'on savait d'un avatar distant se deduisait de l'EXTERIEUR : position relue,
        // derive, ordres emis. Rien ne disait ce que sa machine de deplacement pense faire --
        // et c'est pour ca que « l'accroupi ne marche pas » et « on ne pilote rien du tout » ont
        // mis trois semaines a se distinguer.
        //
        // `TesseraLireLocomotion` rend `action * 10 + exploration` :
        //   action      0 Undefined 1 Exploration 2 Idle 3 IdleTurn 4 Reposition 5 Start 6 Move 7 Stop
        //   exploration 0 None 1 Ladder 2 Jump 3 Climb 4 Vault 5 ChargedJump
        //   -1          composant injoignable -- un resultat, pas une absence de resultat
        //
        // SUR CHANGEMENT UNIQUEMENT. Un etat de machine qui ne bouge pas n'a rien a dire, et une
        // ligne par avatar toutes les deux secondes noierait le journal sans rien apprendre.
        // La lecture, elle, coute un appel par avatar toutes les deux secondes : au plafond de
        // 200 voisins, 100 appels/s -- deux ordres de grandeur sous le regime qui a fait tomber
        // le jeu le 2026-08-06.
        for (const auto& [networkId, entite] : m_networkedEntitiesLookup)
        {
            std::int32_t lu = -1;
            if (!Red::CallVirtual(this, "TesseraLireLocomotion", lu, entite))
            {
                continue;
            }
            auto& suivi = g_suiviAvatars[networkId];
            if (suivi.derniereLocoMoteur != lu)
            {
                suivi.derniereLocoMoteur = lu;
                char etat[64];
                std::snprintf(etat, sizeof(etat), "action=%d,exploration=%d",
                              lu < 0 ? -1 : lu / 10, lu < 0 ? -1 : lu % 10);
                g_telemetrie.Evenement("loco_moteur", networkId, etat);
            }

            // -- L'ACCROUPI, MESURE AU LIEU D'ETRE REGARDE --------------------------------
            //
            // « Le pantin est-il accroupi ? » passait pour inaccessible a un instrument. Le
            // 2026-08-19 a montre que l'oeil ne suffit pas non plus : les captures de l'avatar
            // distant montrent ses CHEVEUX en travers de l'objectif, alors que l'entite est a
            // 6,50 m et parfaitement cadree. Quarante secondes de stabilisation n'y changent
            // rien, et aucun chiffre existant ne le signale.
            //
            // La hauteur de la TETE, elle, est une grandeur. Debout ~1,70 m, accroupi ~1,20 m :
            // un ecart d'un demi-metre, binaire, autonome, insensible a tout defaut de rendu.
            //
            // Seuil de 5 cm : sous cette valeur c'est la respiration de l'animation, pas une
            // posture. Sans seuil, la hauteur bougerait a chaque frame et le journal ne dirait
            // plus rien -- le meme piege que la bande morte des placements.
            std::int32_t zTeteCm = -1;
            // ⚠️ L'ECHEC EST JOURNALISE, PAS AVALE. Sans cette ligne, « aucun evenement `hauteur` »
            // ne distingue PAS trois causes : l'appel qui ne resout pas, la fonction qui rend -1
            // (slot absent), et la valeur qui n'a pas bouge de 5 cm. Trois verdicts opposes, un
            // seul silence — c'est exactement la famille de defaut que ce depot paye le plus cher.
            // ⚠️ `Int32` et non `Float` : le retour flottant rendait 0.00 quelle que soit la
            // branche prise cote redscript, sentinelles comprises (F-PLY-141). On emprunte le type
            // dont l'effet est etabli sur les quatorze autres appels de ce fichier.
            const bool appelOk = Red::CallVirtual(this, "TesseraZTeteCm", zTeteCm, entite);
            if (!appelOk || zTeteCm <= 0)
            {
                if (suivi.derniereHauteurTeteCm != -1)
                {
                    suivi.derniereHauteurTeteCm = -1;
                    char pourquoi[64];
                    std::snprintf(pourquoi, sizeof(pourquoi), "appel=%d,cm=%d", appelOk ? 1 : 0, zTeteCm);
                    g_telemetrie.Evenement("hauteur_echec", networkId, pourquoi);
                }
            }
            else
            {
                // La soustraction se fait ICI : l'accesseur scripte `GetWorldPosition()` rend zero
                // sur un pantin distant (mesure du 2026-08-19 : la hauteur relevee valait 2 468 cm,
                // soit le Z monde entier). `Entity_GetWorldPosition` est le chemin dont l'effet est
                // etabli, et il est deja lu partout ailleurs dans ce fichier.
                // ⚠️ ON JOURNALISE LE Z ABSOLU, LA SOUSTRACTION SE FAIT A L'ANALYSE.
                //
                // Premiere tentative : resoudre le handle ici pour lire les pieds. Mais
                // `GetDynamicEntity` rend `nullopt` dans cette boucle — mesure du 2026-08-19 :
                // `loco_moteur` sortait onze fois et `hauteur` **zero** fois, sur les memes
                // entites et dans la meme iteration. On ne cherche pas pourquoi : le Z du sol est
                // deja dans le journal (chaque ligne `rx` le porte), donc la soustraction ne
                // coute rien a l'analyse et ne peut pas echouer.
                //
                // C'est la regle qui evite une classe entiere de bugs d'instrument : *ne calcule
                // pas dans la sonde ce que le journal permet de calculer apres coup.* Un calcul
                // fait a la mesure peut echouer en silence ; un calcul fait a l'analyse se
                // rejoue autant de fois qu'on veut, sur des donnees deja acquises.
                const int cm = zTeteCm;   // deja en centimetres — plus de conversion ici
                if (std::abs(cm - suivi.derniereHauteurTeteCm) >= 5)
                {
                    suivi.derniereHauteurTeteCm = cm;
                    char mesure[48];
                    std::snprintf(mesure, sizeof(mesure), "tete=%dcm", cm);
                    g_telemetrie.Evenement("hauteur", networkId, mesure);
                }
            }
        }

        // ── SONDE T7 ───────────────────────────────────────────────────────────────────────
        //
        // Rejouée toutes les deux secondes plutôt qu'une seule fois : une entrée de graphe peut
        // être écrasée par la machine d'état du pantin au premier changement d'état. Un unique
        // appel qui « ne marche pas » ne distinguerait donc pas « inatteignable » de « atteint
        // puis écrasé » — deux verdicts opposés.
        if (g_sondeAccroupi)
        {
            for (const auto& [networkId, tampon] : g_tamponsJoueurs)
            {
                const auto e = m_networkedEntitiesLookup.find(networkId);
                if (e == m_networkedEntitiesLookup.end())
                {
                    continue;
                }
                bool pousse = false;
                Red::CallVirtual(this, "TesseraPousserPosture", pousse, e->second, true);
                g_telemetrie.Evenement("sonde_accroupi", networkId, pousse ? "envoye" : "refuse");
            }
        }
    }

    for (auto& [networkId, tampon] : g_tamponsJoueurs)
    {
        const auto entite = m_networkedEntitiesLookup.find(networkId);
        if (entite == m_networkedEntitiesLookup.end())
        {
            continue; // corps pas encore ne (ou deja detruit) : le tampon attend (voir recensement).
        }
        // ── UN AVATAR ASSIS APPARTIENT AU MOTEUR, PLUS A NOUS ──────────────────────────────
        //
        // MESURE le 2026-08-14 (F-VEH-031) : le mounting natif PORTE le corps — « il est synchro
        // quand je roule ». Continuer a le teleporter sur la pose serveur le ferait lutter contre
        // le vehicule a chaque frame, et produirait le tremblement deja vu sur les fantomes
        // glissants. On laisse donc le tampon se remplir (il servira a la descente) et on ne
        // pilote pas.
        if (g_avatarsAssis.count(networkId) != 0)
        {
            continue;
        }


        // ── UN PASSAGER D'ASCENSEUR SE MONTE, IL NE SE PLACE PAS ───────────────────────────
        //
        // Meme raisonnement que pour un passager de vehicule, juste au-dessus : quand le moteur
        // porte le corps, le placer nous-memes le ferait lutter a chaque frame. La difference est
        // qu'ici le montage n'etait pas tente — on placait, et ca se voyait (flou).
        {
            // ⚠️⚠️⚠️ VOIE FERMEE — MESURE DU 2026-08-26, EN DEUX TEMPS.
            //
            // TEMPS 1 — le montage SEUL : accepte, et l'avatar devient INVISIBLE.
            // TEMPS 2 — avec la couche C (`WorkspotSystem.MountToVehicle`) : le `Mount` passe sur
            //           les SIX emplacements essayes, et `MountToVehicle` refuse sur les six :
            //
            //     avatar 2 : monte sur 'None'            mais NON RENDU — demonte
            //     avatar 2 : monte sur 'main_slot'       mais NON RENDU — demonte
            //     avatar 2 : monte sur 'seat_front_left' mais NON RENDU — demonte
            //     … (six fois) … montage REFUSE sur tous les emplacements essayes
            //
            // Ce que ca etablit : une cabine ACCEPTE un montage (le mecanisme repond, et le nom
            // d'emplacement n'y est pour rien), mais la couche qui remet le corps a l'ecran REFUSE.
            // Son nom le disait : `MountToVehicle`. Elle est vraisemblablement reservee aux
            // vehicules, et il n'existe pas d'equivalent connu pour une plateforme.
            //
            // ✅ LE REPLI A FONCTIONNE : demontage immediat, retour au placement, aucun avatar
            // invisible. C'est la partie du dispositif qui valait la peine d'etre ecrite.
            //
            // TEMPS 3 — INSTRUMENT CORRIGE (Lucas : « t'es sur que le mount est bien applique ? »).
            //
            // Il avait raison de douter, et le defaut etait dans MON instrument : `Red::CallVirtual`
            // rend `true` si l'appel a ete DISPATCHE, pas s'il a reussi. Mes lignes « accepte » et
            // « NON RENDU » ne prouvaient donc RIEN — et j'allais classer la voie « impasse
            // mesuree » sur cette base. Une impasse fausse, c'est une porte qu'on ne rouvre plus.
            //
            // Avec trois messages distincts au lieu d'un, le verdict est enfin lisible :
            //
            //     couche C KO : MountToVehicle non dispatche — signature ou methode absente,
            //                   PAS un refus
            //
            // Et ce qui NE s'affiche PAS compte autant : jamais « la CABINE ne se resout pas ».
            // Les deux objets se resolvent bien en `GameObject` — ce n'est donc pas notre code qui
            // echoue a preparer l'appel. C'est l'appel lui-meme qui ne part pas.
            //
            // CONCLUSION, ETABLIE CETTE FOIS : `MountToVehicle` exige un type VEHICULE. Un
            // `LiftDevice` n'en est pas un, donc la verification de type refuse le dispatch. Le
            // `WorkspotSystem` n'expose que cinq points d'entree — `MountToVehicle`,
            // `SwitchSeatVehicle`, `UnmountFromVehicle` (typees vehicule) et `PlayInDevice` /
            // `PlayInDeviceSimple` (cote device, mais elles jouent un workspot a une place DEFINIE,
            // et la cabine ne porte qu'un workspot de prise personnelle).
            //
            // Il n'existe donc pas d'API toute faite pour « rendre un corps monte sur une
            // plateforme ». COUPE. Rouvrir demandera une couche de rendu NOUVELLE, pas un autre nom
            // d'emplacement : reessayer ces six-la ne produira que les six memes lignes.
            //
            // Le journal est formel, sur les deux clients :
            //     [cabine …] avatar N MONTE — emplacement 'None' accepte, on cesse de le placer
            // et Lucas, dans la seconde : « quand on rentre dans la cabine on ne voit plus les
            // joueurs ». La cabine ACCEPTE donc un montage — le mecanisme existe et fonctionne —
            // mais un corps monte n'est PAS RENDU.
            //
            // Ce n'etait pas imprevisible : le chemin vehicule le dit deja, dans son propre
            // commentaire — « COUCHE C. Sans elle, l'occupation est correcte partout SAUF a
            // l'ecran. » Pour un vehicule, c'est le WORKSPOT (l'animation d'assise dans le siege)
            // qui remet le corps a l'ecran. Une cabine d'ascenseur n'a pas de workspot de siege :
            // il manque donc la couche qui rend le corps visible, et on ne sait pas encore laquelle.
            //
            // Invisible est BIEN PIRE que flou : on revient au placement. Le code de montage reste,
            // avec son releve — remettre `true` ci-dessous suffit a le reprendre le jour ou la
            // couche de rendu sera trouvee. Ne pas le retenter en aveugle.
            static constexpr bool kMontageCabineActif = false;   // voie fermee — voir ci-dessus

            const auto ancre = g_porteurParAvatar.find(networkId);
            if (ancre != g_porteurParAvatar.end())
            {
                if (kMontageCabineActif && !ancre->second.monte && !ancre->second.montageRefuse)
                {
                    ancre->second.monte = MonterSurCabine(networkId, entite->second,
                                                          ancre->second.cabine,
                                                          ancre->second.emplacement);
                    ancre->second.montageRefuse = !ancre->second.monte;
                }
                if (ancre->second.monte)
                {
                    continue;   // le moteur le porte : on ne touche plus a sa position.
                }
            }
        }
        // ── UN PASSAGER SE REND AU PRESENT, PAS AU PASSE ──────────────────────────────────
        //
        // On rend volontairement en retard de `kDelaiInterpolationS` (100 ms) : c'est ce qui
        // permet d'interpoler entre deux echantillons au lieu de deviner. Le prix est un retard
        // CONSTANT, invisible sur quelqu'un qui marche.
        //
        // Dans une cabine, il cesse d'etre invisible : le decor autour de l'avatar (la cabine) est
        // rendu MAINTENANT, lui a 100 ms. A 2,5 m/s ca le met un quart de metre au-dessus du
        // plancher, et bien plus sur une grande gaine.
        //
        // Un passager est le cas ou extrapoler est SUR : sa trajectoire verticale est celle de la
        // cabine, donc lineaire et deja decidee par le serveur. On demande donc la pose au present.
        // ⚠️ ON N'EXTRAPOLE PLUS SUR `DelaiCourant()`. Ce delai est ADAPTATIF (il suit la gigue
        // reseau, monte vite, descend lentement) : l'utiliser comme instant d'echantillonnage
        // faisait varier l'instant lu d'une frame a l'autre, et un bruit TEMPOREL devient un bruit
        // de POSITION multiplie par la vitesse. C'etait donc, sur une cabine rapide, une source de
        // sautillement de plus — ajoutee en croyant en retirer une.
        //
        // La verticale d'un passager ne vient plus du fil du tout (voir l'ancrage plus bas), donc
        // il n'y a plus rien a rattraper ici : on echantillonne a l'instant nominal, comme pour
        // tout le monde.
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
    // ⚠️ LE GUET EST ICI, ET PAS DANS SetEntityPose : celle-la est le chemin des PNJ et des
    // vehicules. Les avatars de JOUEURS passent par PiloterAvatar. Pose au mauvais endroit, le
    // guet n'a produit AUCUNE ligne — et une absence totale de trace est le seul cas ou un
    // instrument muet se denonce lui-meme.
    GuetterResolution(networkId, entityId, pose);

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
    RED4ext::Vector4 positionVoulue = { pose.x, pose.y, pose.z, 1.0f };

    // ── UN CORPS ATTACHE A UNE PLATEFORME NE SE PLACE PLUS DU TOUT ─────────────────────────
    //
    // ⚠️ Ecrire une position monde sur un corps attache ne fait pas que « lutter » : ca CASSE. Sa
    // transformee est devenue LOCALE au plancher, donc la position se compose avec celle de la
    // cabine et le corps part a des centaines de metres (mesure du 2026-08-28, F-ASC-057).
    //
    // ⭐ MAIS ON MEMORISE LA POSE VOULUE AVANT DE SORTIR. C'est elle qui porte le mouvement PROPRE
    // du passager dans la cabine — celui que l'attache seule fige, et dont l'absence se voit :
    // « le PNJ a ete fige, il se remet a jour quand la cabine s'arrete » (Lucas, 2026-08-28). Le
    // redscript la relit et ecrit l'ecart LOCAL correspondant, ce qui complete le modele en repere
    // relatif : la cabine par l'attache (sans retard), le passager par le fil (petit et lent).
    //
    // ⚠️ La garde est ICI et plus dans la boucle appelante : la-bas, la pose n'est pas encore
    // calculee, et sortir avant de la connaitre reviendrait a jeter la seule chose qu'on veut.
    {
        const uint32_t cle = static_cast<uint32_t>(entityId.hash);
        if (g_avatarsAttachesPlateforme.count(cle) != 0)
        {
            g_poseVoulueAttachee[cle] = positionVoulue;
            g_allureAttachee[cle] = pose.locomotion;
            return;
        }
        g_poseVoulueAttachee.erase(cle);
        g_allureAttachee.erase(cle);
    }

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
            g_telemetrie.Evenement("irresolue", networkId, "");
        }
        return;
    }

    // ── PORTE PAR UNE CABINE : LA VERTICALE APPARTIENT AU MOTEUR ───────────────────────────
    //
    // Le corps est POSE sur le plancher d'une plateforme mobile, et le moteur l'y tient (F-ASC-001,
    // confirme en jeu le 2026-08-25 : « le personnage est bien attache »). Notre recalage, lui,
    // vise un Z INTERPOLE, en retard de 50 a 150 ms — donc plus haut pendant une descente. Les
    // deux se disputent le corps a chaque frame et l'avatar SAUTILLE, alors qu'il est parfaitement
    // stable sur l'ecran de celui qui le pilote.
    //
    // On ne cesse pas de piloter (ce que fait le cas ASSIS) : dans une cabine, on marche. On rend
    // seulement la VERTICALE au moteur, en visant le Z ou le corps se trouve deja. L'horizontale
    // continue d'etre corrigee, donc un occupant qui se deplace dans la cabine reste rendu.
    //
    // ⚠️⚠️ IL Y AVAIT ICI UN GARDE-FOU « au-dela de 3 m d'ecart, on reprend la main ». IL PRODUISAIT
    // EXACTEMENT LE DEGAT QU'IL PRETENDAIT EVITER, et Lucas l'a decrit au mot pres le 2026-08-25 :
    // « ca sautille moins souvent, mais on voit les personnages VOLER — il remonte sur la hauteur
    // de la cabine ».
    //
    // La raison. Dans une descente, le Z du reseau est celui d'il y a un aller-retour : donc PLUS
    // HAUT. L'ecart que le garde mesurait n'etait pas une derive, c'etait le RETARD NORMAL — et il
    // grandit avec la vitesse de la cabine, donc une grande gaine le franchit forcement. Passe le
    // seuil, le garde « corrigeait » vers ce Z perime : il TIRAIT L'AVATAR VERS LE HAUT d'une
    // hauteur d'etage, avant que la plateforme ne le rattrape.
    //
    // Un garde-fou qui se declenche quand tout va bien finit par etre la seule chose qu'on voie.
    // C'etait deja ecrit pour le recalage des cabines (`Recaler`, `IsMoving`) — repaye ici le meme
    // jour, sur un autre systeme, pour la meme raison.
    //
    // ⚠️⚠️ ET LA VERSION SUIVANTE — « porte = on ne touche plus au Z, le moteur s'en charge » — a
    // ete PIRE : l'avatar reste sur place pendant que la cabine part, finit « de l'autre cote, dans
    // le vide », puis disparait. Le moteur ne porte PAS ce corps-la. Ce qui le raccrochait, c'etait
    // la correction elle-meme.
    //
    // ── L'ANCRAGE, ET POURQUOI IL SUPPRIME LE RETARD AU LIEU DE LE CORRIGER ────────────────
    //
    // La pose reseau est reconstituee a `TempsRendu()`, deliberement EN RETARD sur le present d'un
    // tampon d'interpolation. La cabine, elle, est rendue MAINTENANT. C'est ce decalage entre deux
    // horloges — pas une imprecision — qui met l'avatar au-dessus du plancher pendant une descente,
    // d'autant plus haut que la cabine va vite.
    //
    // On ne corrige donc rien apres coup : on vise « la cabine, MAINTENANT, plus l'ecart releve a
    // l'embarquement ». La cabine est une entite LOCALE dont on lit la position presente, et son
    // trajet est identique sur tous les clients (c'est tout l'objet de l'ordre serveur). Le retard
    // disparait par construction.
    //
    // L'ecart se releve — et se RAFRAICHIT — tant que la cabine est A L'ARRET : la pose reseau y est
    // exacte, donc on suit quelqu'un qui se deplace dans la cabine. Des qu'elle bouge, l'ecart se
    // fige : on porte, on ne suit plus. Personne ne traverse une cabine pendant un trajet, et c'est
    // le prix d'un portage rigide.
    //
    // ⚠️ REPLI OBLIGATOIRE : cabine irresolue -> on garde la correction reseau. Un avatar mal place
    // se voit et se rattrape au pas suivant ; un avatar qu'on cesse de corriger se PERD, et c'est
    // exactement ce qui vient d'etre mesure.
    // ⚠️⚠️⚠️ ET L'ANCRAGE SUR LA CABINE A ETE ESSAYE, PUIS RETIRE : L'ENTITE NE BOUGE PAS.
    //
    // MESURE (2026-08-25, `lift_state` echantillonne toutes les 700 ms pendant un trajet complet) :
    //
    //     floor=1 target=0 moving=true MovingDown  x=-1435.090 y=1311.961 z=27.269
    //     ... douze relevés, douze secondes de descente, la MEME position a la 3e decimale ...
    //
    // `Entity_GetWorldPosition(LiftDevice)` rend la position du NOEUD DE GAINE, pas celle du
    // plancher. Ce qui descend est un composant (`MovingPlatform` / le maillage), et la transforme
    // de l'entite ne le suit pas. Un ancrage la-dessus fige donc l'avatar sur un point immobile —
    // pire que tout ce qui precede.
    //
    // ── ✅ ET C'EST LE COMPOSANT `movingPlatform` QUI DESCEND — MESURE LE 2026-08-25 ───────
    //
    //     floor=1 moving=false | movingPlatform=125.472
    //     floor=1 moving=true  | movingPlatform=120.466
    //     floor=1 moving=true  | movingPlatform=106.146
    //     floor=0 moving=false | movingPlatform= 90.954     (34,5 m de gaine)
    //
    // Redscript le lit sans peine et le PUBLIE a chaque tick de 50 ms
    // (`Tessera_PoserHauteurCabine`). On tient donc enfin la hauteur du plancher LOCALEMENT, a
    // l'instant present — sans reseau, donc sans retard.
    //
    // ── L'ANCRAGE : REPLIQUER RELATIVEMENT AU PORTEUR ──────────────────────────────────────
    //
    // C'est le patron standard du metier, et la raison pour laquelle il marche est structurelle :
    // au lieu de transporter une position VERTICALE qui change vite (donc dont le moindre retard se
    // voit), on transporte un ECART au plancher qui, lui, ne change quasiment pas — quelqu'un
    // debout dans une cabine garde le meme ecart pendant tout le trajet. Le retard du fil porte
    // alors sur une grandeur constante, et devient invisible.
    //
    //   Unreal : `ReplicatedBasedMovement` — position RELATIVE a la base, recomposee au rendu avec
    //            la position locale de la base.
    //   Unity  : parentage reseau + `NetworkTransform` en espace local.
    //
    // Ici : `positionVoulue.Z = hauteurPlancher(maintenant) + ecart`, l'ecart etant releve tant que
    // la cabine est A L'ARRET (la pose reseau y est exacte, donc on suit quelqu'un qui marche
    // dedans) puis fige pendant le trajet.
    //
    // ⚠️ REPLI OBLIGATOIRE : pas de hauteur publiee (cabine de-streamee, tick mort, cabine hors
    // catalogue) -> on garde la correction reseau. Un avatar mal place se rattrape au pas suivant ;
    // un avatar qu'on cesse de corriger se PERD — mesure le meme jour, au prix d'un avatar invisible.
    // ══════════════════════════════════════════════════════════════════════════════════════════
    // ⛔ ANCRAGE VERTICAL : DESACTIVE — decision de Lucas, 2026-08-27, en session.
    //
    // « La gestion des ecarts est trop problematique et trop bordelique. Je veux simplement que la
    //   personne soit, avec la gravite, synchronisee avec les autres joueurs. Quelque chose de
    //   beaucoup plus simple, meme si ce n'est pas parfait pour l'instant. »
    //
    // On revient donc a la VERTICALE RESEAU : Z arrive du fil comme X et Y, et la physique du jeu
    // fait le reste. C'est moins juste dans une cabine en mouvement, et c'est ASSUME — le flou est
    // accepte lui aussi, pour l'instant.
    //
    // ── POURQUOI L'ANCRAGE N'A PAS TENU, ET CE QUI RESTE VRAI ─────────────────────────────────
    //
    // Il ne s'est pas revele faux : il s'est revele INAPPLICABLE avec la hauteur dont on dispose.
    // Mesure du 2026-08-27, gaine de 91,8 m du Megabuilding H10, journal de ce meme bloc :
    //
    //     14:17:38  plancher=119.111  (lu=1 bouge=0)
    //               <-- 32 secondes SANS AUCUNE LIGNE -->
    //     14:18:10  plancher=27.269   (lu=1 bouge=0)
    //
    // La hauteur publiee SAUTE d'un etage a l'autre : aucune valeur intermediaire, `bouge=0` tout
    // du long. L'avatar reste donc a une altitude fixe pendant que la cabine descend autour de lui
    // — « il monte au plafond puis il redescend », mot pour mot ce que Lucas a rapporte.
    //
    // ⚠️ ET LA LECON DE METHODE, qui vaut plus que le code : la session a servi a ajuster une
    // COURBE d'interpolation (trois iterations, mesurees, documentees) sur un mecanisme qui ne
    // tournait pas. Le diagnostic `ecart=` compare le modele a la verite — il ne dit RIEN de ce qui
    // est reellement applique a l'avatar. **Avant de regler un mecanisme, prouver qu'il s'execute.**
    //
    // Ce qui reste ACQUIS et reutilisable le jour ou on y revient :
    //   · la geometrie d'etage se lit en redscript (F-ASC-044, mesure) ;
    //   · la duree reelle d'un trajet est `distance / vitesse`, pas le forfait (F-ASC-042) ;
    //   · la vitesse recoltee est ambigue sans l'occupation (F-ASC-045) ;
    //   · le chemin de publication fonctionne A L'ARRET : `dz=0.013 ecart=0.000`, mesure.
    // Le trou est entre les deux etages, et il est nomme.
    //
    // Remettre `Tessera::kAncrageVerticalActif` a `true` pour reactiver — tout le mecanisme est
    // intact en dessous, et le CAS PASSAGER plus bas est garde par la meme constante.
    if constexpr (Tessera::kAncrageVerticalActif)
    {
        const auto ancre = g_porteurParAvatar.find(networkId);
        if (ancre != g_porteurParAvatar.end())
        {
            float plancher = 0.0f;
            bool cabineEnMouvement = false;
            bool planchierLu = HauteurCabineA(ancre->second.cabine, g_tempsLocalS, plancher,
                                              cabineEnMouvement);
            if (!planchierLu && ancre->second.plancherVu)
            {
                // ⭐ REPLI SUR LE DERNIER PLANCHER CONNU, jamais sur la position du fil (voir
                // `plancherConnu`). Une lecture ratee ne doit pas relacher l'ancrage : elle doit
                // le figer une image, ce qui coute des centimetres au lieu de metres.
                plancher = ancre->second.plancherConnu;
                cabineEnMouvement = true;   // on ne RECAPTURE pas l'ecart sur une lecture ratee
                planchierLu = true;
            }
            if (planchierLu)
            {
                ancre->second.plancherConnu = plancher;
                ancre->second.plancherVu = true;
                if (!ancre->second.ecartConnu || !cabineEnMouvement)
                {
                    ancre->second.dz = positionVoulue.Z - plancher;
                    ancre->second.ecartConnu = true;
                }
                positionVoulue.Z = plancher + ancre->second.dz;
            }
        }
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
            g_telemetrie.Evenement("fil_muet", networkId, "fige");
        }
        return; // on ne corrige pas non plus la position : sans nouvelles, on n'invente rien.
    }

    // ── L'ACCROUPISSEMENT N'ETAIT POUSSE PAR PERSONNE (cable le 2026-08-19) ──────────────
    //
    // `TesseraPousserPosture` existe depuis aout et n'avait qu'UN SEUL appelant : la sonde T7,
    // sous `g_sondeAccroupi`. En regime normal — celui d'un joueur, pas d'une mesure — personne
    // ne la rappelait. On a donc passe trois semaines a se demander pourquoi l'accroupissement ne
    // se voyait pas, en instrumentant une fonction que le jeu n'appelait jamais.
    //
    // C'est le meme defaut que le saut plus bas : un geste correct sur un canal qui ne part pas.
    // L'allure porte deja l'information (4 = CrouchIdle, 5 = CrouchMove, mesures du 2026-07-23),
    // elle voyage sur le fil depuis le gel du palier 2, et elle arrive ici intacte.
    //
    // ⚠️ SUR CHANGEMENT, jamais en continu. Une ecriture de graphe d'animation par avatar et par
    // frame, c'est le regime qui a fait tomber le jeu deux fois le 2026-08-06. Un accroupissement
    // est un EVENEMENT : il se pousse quand il arrive, et se tait ensuite. Le -1 initial garantit
    // qu'un avatar qui naît deja accroupi recoive quand meme sa premiere pousse.
    {
        auto& suiviPosture = g_suiviAvatars[networkId];
        const bool accroupi = pose.locomotion == 4 || pose.locomotion == 5;
        const std::int8_t voulue = accroupi ? 1 : 0;
        if (suiviPosture.dernierePostureAccroupie != voulue)
        {
            suiviPosture.dernierePostureAccroupie = voulue;
            bool pousse = false;
            Red::CallVirtual(this, "TesseraPousserPosture", pousse, entityId, accroupi);
            g_telemetrie.Evenement("posture", networkId,
                                   accroupi ? (pousse ? "accroupi" : "accroupi_refuse")
                                            : (pousse ? "debout" : "debout_refuse"));
        }

        // -- LA POSE TENUE, SUR CHANGEMENT (cablee le 2026-08-24) -----------------------------
        //
        // `PlayerState.sustained` voyage depuis le palier 2 et n'avait AUCUN lecteur cote client
        // (F-PLY-303) : le serveur ecrivait, le fil transportait, le client jetait. Toute la
        // moitie « quand un joueur s'assied, les autres le voient assis » — la demande d'origine
        // de Lucas — reposait donc sur un canal a zero consommateur, et vingt tests verts cote
        // serveur n'en disaient rien.
        //
        // Ce bloc-ci ne fait pas encore JOUER la posture sur l'avatar : poser une pose sur un
        // pantin distant reste ouvert. Il rend le canal OBSERVABLE, ce qui est la condition pour
        // que la suite soit mesurable au lieu d'etre supposee.
        //
        // Meme discipline que l'accroupissement juste au-dessus : SUR CHANGEMENT, jamais en
        // continu — une ecriture par avatar et par frame a fait tomber le jeu deux fois le
        // 2026-08-06.
        if (suiviPosture.derniereSustained != pose.sustained)
        {
            const std::uint32_t precedente = suiviPosture.derniereSustained;
            suiviPosture.derniereSustained = pose.sustained;
            char detail[64];
            std::snprintf(detail, sizeof(detail), "%u->%u",
                          precedente == 0xFFFFFFFFu ? 0u : precedente, pose.sustained);
            g_telemetrie.Evenement("sustained", networkId, detail);
        }

        // -- L'ARME EN MAIN, SUR CHANGEMENT (F-PLY-138 : une COUCHE, pas un etat) -----------
        //
        // `m_appearances` porte deja l'arme degainee de cet avatar — le fil la transporte et
        // `HandleAppearanceSync` la range. Elle n'avait aucun consommateur.
        //
        // Meme discipline que la posture : SUR CHANGEMENT, jamais en continu, et l'etat initial
        // est `false` — un pantin naît les mains vides, donc aucune ecriture dans la frame de sa
        // naissance (F-PLY-119, qui rendait les corps invisibles).
        {
            const auto apparence = m_appearances.find(networkId);
            const bool degainee = apparence != m_appearances.end() && apparence->second.arme != 0;
            if (suiviPosture.derniereArmeDegainee != degainee)
            {
                suiviPosture.derniereArmeDegainee = degainee;
                bool pousse = false;
                Red::CallVirtual(this, "TesseraPousserArme", pousse, entityId, degainee);
                g_telemetrie.Evenement("arme_posture", networkId,
                                       degainee ? (pousse ? "degainee" : "degainee_refuse")
                                                : (pousse ? "rangee" : "rangee_refuse"));
            }
        }

        // -- L'HABILLAGE, PAR CRENEAUX ET BORNE (2026-08-24) ---------------------------------
        //
        // ⚠️ POURQUOI ICI ET PAS DANS UN HOOK redscript. La premiere version accrochait
        // `@wrapMethod(ScriptedPuppet) OnGameAttached`, en copiant `ArmeAvatar.reds`. Mesure du
        // 2026-08-24 : sur un corps ne de la voie ENRICHIE — celui qui porte le V du joueur — ce
        // hook ne se declenche JAMAIS. Zero ligne de journal, sur deux instances. Il ne marchait
        // que sur les pantins de passant de la voie sure, c'est-a-dire exactement les corps qu'on
        // ne veut plus.
        //
        // `PiloterAvatar` est le seul chemin dont on SAIT qu'il atteint ces corps : c'est lui qui
        // les fait marcher, et Lucas l'a confirme a l'ecran.
        //
        // ⚠️ PAR CRENEAUX, JAMAIS A CHAQUE INSTANTANE. Une passe d'habillage touche le
        // `TransactionSystem` quatre fois ; le faire vingt fois par seconde et par avatar est le
        // regime qui a fait tomber le jeu deux fois le 2026-08-06.
        {
            const auto apparence = m_appearances.find(networkId);
            std::uint64_t signature = 0;
            if (apparence != m_appearances.end())
            {
                for (const auto v : apparence->second.vetements)
                {
                    signature = (signature * 1099511628211ull) ^ v;
                }
            }
            const auto maintenantHab = std::chrono::steady_clock::now();
            if (suiviPosture.signatureVetements != signature)
            {
                // Tenue neuve (ou premiere) : on repart a zero. ⚠️ ET ON LAISSE LE CORPS FINIR DE
                // SE MONTER avant la premiere passe — tenter tout de suite, c'est tenter dans la
                // fenetre ou tout est accepte et rien n'est execute.
                suiviPosture.signatureVetements = signature;
                suiviPosture.passesHabillage = 0;
                suiviPosture.prochainePasseHabillage = maintenantHab + kDelaiAvantHabillage;
            }
            if (signature != 0 && suiviPosture.passesHabillage < kPassesHabillage)
            {
                if (maintenantHab >= suiviPosture.prochainePasseHabillage)
                {
                    suiviPosture.prochainePasseHabillage =
                        maintenantHab + kIntervallePasseHabillage;
                    bool habille = false;
                    Red::CallVirtual(this, "TesseraHabillerAvatar", habille, entityId,
                                     suiviPosture.passesHabillage);
                    if (habille)
                    {
                        // Convergé : on arrete. Un changement de tenue relancera les passes par la
                        // signature ci-dessus — il n'y a donc rien a re-armer.
                        suiviPosture.passesHabillage = kPassesHabillage;
                        g_telemetrie.Evenement("habillage", networkId, "pose");
                    }
                    else
                    {
                        ++suiviPosture.passesHabillage;
                        if (suiviPosture.passesHabillage == kPassesHabillage)
                        {
                            g_telemetrie.Evenement("habillage", networkId, "abandon");
                        }
                    }
                }
            }
        }

        // -- L'ANIMATION DU SAUT, AU DECOLLAGE ET A L'ATTERRISSAGE (F-PNJ-164) ---------------
        //
        // La POSITION du saut est repliquee depuis ce matin (F-PLY-117, amplitude relue 1,500 m).
        // Ce qui manque est l'ANIMATION : le pantin monte et redescend en gardant sa pose debout.
        //
        // F-PNJ-163 disait le 2026-08-18 qu'aucune entree du graphe n'adresse le saut. C'etait
        // vrai sur le NOM. Le groupe `exploration` etait pourtant dans sa propre liste, et son
        // enumeration contient `Jump` -- le selecteur a donc un nom, un type et une echelle.
        //
        // MEME REGLE QUE LA POSTURE : sur CHANGEMENT, jamais en continu. Un saut dure moins d'une
        // seconde ; le pousser a chaque frame ferait 60 ecritures de graphe pour un geste qui en
        // demande deux -- et c'est le regime qui a fait tomber le jeu deux fois le 2026-08-06.
        const bool enVolMaintenant = pose.locomotion == 6;
        if (suiviPosture.dernierEnVol != enVolMaintenant)
        {
            suiviPosture.dernierEnVol = enVolMaintenant;
            bool franchi = false;
            Red::CallVirtual(this, "TesseraPousserFranchissement", franchi, entityId, enVolMaintenant);
            g_telemetrie.Evenement("franchissement", networkId,
                                   enVolMaintenant ? (franchi ? "decollage" : "decollage_refuse")
                                                   : (franchi ? "sol" : "sol_refuse"));
        }
    }
    // ══════════════════════════════════════════════════════════════════════════════════════════
    // ⭐ L'INSTRUMENT DE LA VERTICALE — POSE AVANT LA SEPARATION DES BRANCHES, ET C'EST LE POINT.
    //
    // Le 2026-08-27, quatre correctifs successifs sur le placement d'un passager d'ascenseur n'ont
    // produit AUCUNE difference a l'ecran. Quand rien ne change, la question n'est pas « le
    // correctif est-il bon ? » mais « s'execute-t-il ? » — et je ne me l'etais pas posee.
    //
    // `PiloterAvatar` a DEUX branches, separees par `pose.locomotion == 0`, et elles se corrigent
    // de facons opposees :
    //   · IMMOBILE : cadence lente, bande morte de 5 cm, historiquement `PlacerSansCommande` ;
    //   · MOBILE   : declenchee par l'ecart HORIZONTAL (`deriveHorizontale > 0.25`) — donc JAMAIS
    //                pour un passager d'ascenseur, dont l'ecart est purement vertical.
    //
    // Selon la branche empruntee, le correctif a poser n'est pas le meme. On mesure donc laquelle
    // est prise, avec les nombres qui la determinent, AVANT de toucher a l'une ou a l'autre.
    //
    // Ne se declenche QUE quand la cible defile verticalement : aucun bruit en regime normal.
    {
        auto& suiviVert = g_suiviAvatars[networkId];
        const float dzCible =
            suiviVert.cibleImmobileConnue ? positionVoulue.Z - suiviVert.cibleImmobilePrecedente.Z
                                          : 0.0f;
        const bool cibleVerticale =
            suiviVert.cibleImmobileConnue && std::fabs(dzCible) > 0.005f;

        // ── ⭐⭐ EXTRAPOLATION DU RETARD — ICI, ET PAS DANS UNE BRANCHE ─────────────────────
        //
        // ⚠️ CE BLOC VIVAIT DANS LA BRANCHE « IMMOBILE », ET C'ETAIT LE DEFAUT. Mesure du
        // 2026-08-27 : un passager d'ascenseur rapporte `locomotion = 6` (EN L'AIR — la plateforme
        // bouge sous lui), donc il ne passe JAMAIS par cette branche. Quatre correctifs successifs
        // y ont ete poses, et aucun n'a tourne une seule fois. « Rien ne change » ne voulait pas
        // dire « le correctif est mauvais », mais « il ne s'execute pas » — et je ne me suis pas
        // pose la question.
        //
        // Ce qu'on corrige : la pose reseau est reconstituee a `TempsRendu()`, en retard d'un
        // tampon d'interpolation ; le monde autour, lui, est rendu MAINTENANT. A 3 m/s, 100 ms de
        // tampon font 30 cm — et le releve montre l'ecart croitre AVEC la vitesse, jusqu'a 44 cm au
        // pic, toujours du meme signe. Signature d'un retard, pas d'un bruit.
        //
        // On avance donc la cible de la duree du tampon, a la vitesse mesuree de la cible. Pour un
        // corps porte a vitesse quasi constante c'est presque exact, et ca s'ANNULE de soi-meme a
        // l'arret. Aucune connaissance de la cabine n'est requise : ni geometrie, ni courbe, ni
        // duree. Vaut pour toute plateforme, benne, et un jour un train.
        // ⚠️⚠️ ON MEMORISE LA CIBLE **BRUTE**, AVANT EXTRAPOLATION — ET C'EST VITAL.
        //
        // La premiere version memorisait la cible APRES extrapolation. L'ecart de la frame suivante
        // contenait donc l'extrapolation precedente, qui etait re-extrapolee, et ainsi de suite :
        // une BOUCLE DE RETROACTION POSITIVE. Mesuree le 2026-08-27, en quelques images :
        //
        //     cible.z = -13 291 337
        //     cible.z = -5,6e18
        //     cible.z = 5,09e29
        //     cible.z = ±inf        -> reel.z = -16384 (le moteur refuse la position)
        //
        // L'avatar devenait impossible a placer, donc invisible. C'est l'instrument `[vert]` qui l'a
        // attrape en une lecture — un correctif divergent est indiscernable d'un correctif inerte
        // quand on ne regarde que l'ecran.
        const RED4ext::Vector4 cibleBrute = positionVoulue;

        if (cibleVerticale && deltaTime > 0.0001f)
        {
            // ── ⭐ LISSAGE DE LA VITESSE, et il repare deux pics MESURES ────────────────────
            //
            // L'extrapolation multiplie la vitesse par le delai du tampon : une vitesse estimee sur
            // UNE image est donc aussi bruitee que la duree de cette image. Releve du 2026-08-27,
            // deux instances sur la meme machine : l'ecart tenait a ±5 cm, avec DEUX pics isoles a
            // 0,69 m — tous deux sur une image ou l'ecart de cible par frame passait de 0,09 a
            // 0,26 m. L'image etait longue, pas la cabine plus rapide.
            //
            // Une plateforme va a vitesse quasi constante : lisser ne coute donc rien en justesse,
            // et supprime la totalite du bruit d'echantillonnage. Moyenne exponentielle a 0,25 —
            // environ quatre images de memoire, soit assez pour absorber une image longue et pas
            // assez pour trainer au demarrage.
            const float vzInstantanee = dzCible / deltaTime;
            static constexpr float kLissageVitesse = 0.25f;
            if (suiviVert.vitesseZConnue)
            {
                suiviVert.vitesseZLissee += kLissageVitesse * (vzInstantanee - suiviVert.vitesseZLissee);
            }
            else
            {
                suiviVert.vitesseZLissee = vzInstantanee;
                suiviVert.vitesseZConnue = true;
            }
            const float vzCible = suiviVert.vitesseZLissee;
            const float delai = static_cast<float>(g_horlogeRendu.DelaiCourant());
            // Borne : au-dela de 300 ms de tampon on n'extrapole pas plus loin. Un fil degrade ne
            // doit pas projeter un avatar a plusieurs metres de sa cible.
            const float delaiBorne = delai > 0.3f ? 0.3f : (delai < 0.0f ? 0.0f : delai);

            // ── ⭐ ET UNE IMAGE DE PLUS, parce qu'il y en a une entre le calcul et l'ecran ─────
            //
            // La pose est reconstituee ICI, appliquee ICI, et RENDUE a l'image suivante. Ce retard
            // d'une image est constant et vaut `vitesse x dt` — ce fichier le decrivait deja, a
            // propos du passager d'ascenseur, quelques centaines de lignes plus haut.
            //
            // Mesure du 2026-08-27 qui le confirme : apres extrapolation du tampon, l'erreur de
            // fond tombe a +0,07..+0,14 m, TOUJOURS POSITIVE — et l'ecart de cible par image vaut
            // justement 0,11 a 0,14 m. Le residu est donc, exactement, une image.
            //
            // ⚠️ Un residu SYSTEMATIQUE (toujours du meme signe) se corrige ; un residu qui change
            // de signe est du bruit et ne se corrige pas. C'est le signe constant qui autorise ce
            // terme, pas sa taille.
            // ── ⭐ UNE IMAGE ET DEMIE, ET LE DEMI VIENT D'UNE MESURE ────────────────────────
            //
            // Releve du 2026-08-27, gaine de 91,8 m, 116 echantillons, apres le terme `+1 x dt` :
            //
            //     cible LENTE  (|dz par image| < 0,05)  ecart median  +0,011 m
            //     cible RAPIDE (|dz par image| > 0,15)  ecart median  +0,205 m
            //
            // Lucas, au meme moment : « il ne touche pas le sol quand on descend, il y a toujours
            // une vingtaine de centimetres ». Les deux se rejoignent au centimetre.
            //
            // ⭐ L'ajustement `ecart = k x |dz par image|` donne **k = 0,48** : le residu n'est donc
            // PAS un decalage constant, c'est un retard PROPORTIONNEL A LA VITESSE, et il vaut une
            // demi-image. Une valeur negative fixe — la premiere idee, naturelle — aurait
            // sur-corrige a vitesse lente et sous-corrige au pic. C'est la mesure qui a departage
            // les deux, pas le raisonnement.
            //
            // Apres retrait du terme, le residu median tombe a +0,005 m.
            static constexpr float kImagesDAvance = 1.5f;
            float avance = vzCible * (delaiBorne + kImagesDAvance * deltaTime);
            // ⚠️ FILET DE SECURITE, et il est la parce qu'un bug l'a franchi. Une cabine de Night
            // City ne parcourt jamais 2 m en un tampon d'interpolation (ce serait 20 m/s). Au-dela,
            // ce n'est plus une extrapolation, c'est un emballement — on refuse, et le pire cas
            // devient « pas de correction » au lieu de « avatar a l'infini ».
            static constexpr float kAvanceMaxM = 2.0f;
            if (!(avance > -kAvanceMaxM && avance < kAvanceMaxM))
            {
                avance = 0.0f;   // couvre aussi NaN : toute comparaison avec NaN est fausse
            }
            positionVoulue.Z += avance;
        }

        // ⚠️ LE SUIVI SE MET A JOUR ICI, POUR TOUT LE MONDE. Il vivait dans la branche immobile :
        // la cible precedente restait donc figee a jamais pour un passager (qui n'y passe pas), et
        // l'ecart par frame valait n'importe quoi — `-25.4382` constant dans le premier releve.
        suiviVert.cibleImmobilePrecedente = cibleBrute;
        suiviVert.cibleImmobileConnue = true;

        if (cibleVerticale)
        {
            suiviVert.depuisLogS += deltaTime;
            if (suiviVert.depuisLogS >= 0.5f)
            {
                suiviVert.depuisLogS = 0.0f;
                const auto reel = Cyberverse::Utils::Entity_GetWorldPosition(entite.value());
                const float dh = std::sqrt((positionVoulue.X - reel.X) * (positionVoulue.X - reel.X)
                                           + (positionVoulue.Y - reel.Y) * (positionVoulue.Y - reel.Y));
                SDK->logger->InfoF(PLUGIN,
                    "[vert %llu] loco=%u BRANCHE=%s | cible.z=%.3f reel.z=%.3f ECART.z=%+.3f | "
                    "dh=%.3f | dz_cible_par_frame=%+.4f porteur=%d",
                    networkId, static_cast<unsigned>(pose.locomotion),
                    pose.locomotion == 0 ? "IMMOBILE" : "MOBILE",
                    positionVoulue.Z, reel.Z, reel.Z - positionVoulue.Z, dh, dzCible,
                    g_porteurParAvatar.count(networkId) != 0 ? 1 : 0);
            }
        }
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

        // ── UN AVATAR IMMOBILE NE DOIT RIEN COÛTER, ET IL COÛTAIT LE PLUS CHER ─────────────
        //
        // ⚠️ Ce chemin appelait `SetEntityPosition` À CHAQUE FRAME. Or `SetEntityPosition` fait
        // deux choses (voir le corps de `PlacerSansCommande`) : il empile un `AITeleportCommand`
        // dans la file du contrôleur d'IA, PUIS il place l'entité. Il faisait donc très exactement
        // ce que `PlacerSansCommande` a été écrit pour éviter — sauf qu'ici personne ne l'avait vu,
        // parce qu'une branche « immobile » a l'air inoffensive par construction.
        //
        // L'arithmétique est brutale, et c'est elle qui en fait un défaut de production plutôt
        // qu'une inélégance : 50 joueurs immobiles × 60 fps = **3 000 commandes d'IA par seconde**.
        // C'est la zone du régime qui a fait tomber le jeu deux fois le 2026-08-06 (~3 120 par
        // seconde). Au plafond de 200 voisins, ce serait 12 000 par seconde. Et le pire cas d'un
        // serveur RP, c'est justement celui-là : un attroupement où tout le monde est debout et
        // personne ne bouge.
        //
        // Deux corrections, et la première est la plus importante :
        //
        // 1. **Une bande morte.** Un avatar déjà à sa place n'a aucune raison d'être replacé. Le
        //    coût nominal d'un avatar immobile tombe alors à une soustraction par frame — zéro
        //    appel moteur, zéro commande. C'est le patron que le chemin VÉHICULE applique déjà
        //    (test d'immobilité à 5 cm) et que celui-ci n'avait jamais reçu.
        // 2. **`PlacerSansCommande` au lieu de `SetEntityPosition`.** Quand il faut vraiment
        //    replacer, on place — sans repasser par la file d'IA qu'on vient précisément
        //    d'annuler quelques lignes plus haut avec `TesseraFigerAvatar`. Empiler un ordre juste
        //    après en avoir annulé un est contradictoire.
        //
        // 5 cm : sous le seuil de perception à distance de conversation, et au-dessus du bruit de
        // quantification de la position sur le fil (`QuantPos`).
        static constexpr float kBandeMorteImmobileM = 0.05f;
        const auto placeActuelle = Cyberverse::Utils::Entity_GetWorldPosition(entite.value());
        const float ex = positionVoulue.X - placeActuelle.X;
        const float ey = positionVoulue.Y - placeActuelle.Y;
        const float ez = positionVoulue.Z - placeActuelle.Z;
        const float deriveImmobile = std::sqrt(ex * ex + ey * ey + ez * ez);

        // ── UN AVATAR IMMOBILE QUI PIVOTE NE TOURNAIT JAMAIS (corrigé le 2026-08-15) ───────
        //
        // La bande morte ci-dessus est juste, et elle reste. Le défaut était ailleurs : `pose.yaw`
        // n'était appliqué qu'en **effet de bord** d'une correction de POSITION. Un joueur qui
        // pivote sur place ne franchit donc jamais les 5 cm — et son avatar ne tourne **jamais**
        // chez les autres. Il reste planté dans la direction qu'il avait en s'arrêtant.
        //
        // C'est le défaut le plus visible qui soit en RP, parce que c'est le cas NOMINAL : deux
        // joueurs debout qui se parlent. L'un se tourne vers un troisième, et personne ne le voit.
        //
        // Trouvé le 2026-08-15 par une passe adversariale sur la spec du regard des avatars — qui
        // s'apprêtait à répliquer une direction de REGARD par-dessus un corps qui, lui, ne tournait
        // pas. Répliquer plus finement un signal qu'on n'applique pas est un travail perdu.
        //
        // LE CORRECTIF : la bande morte devient bidimensionnelle. On replace si la position OU
        // l'orientation a dérivé. L'orientation courante se lit sur l'entité, exactement comme la
        // position juste au-dessus — donc aucun état à mémoriser, et le test se corrige tout seul si
        // quelque chose d'autre tourne l'avatar.
        //
        // ⚠️ `EcartAngulaire` et pas une soustraction. Sans lui, un avatar qui passe par le nord
        // verrait un écart de 358° au lieu de 2° et se ferait replacer à chaque frame. Le helper
        // existe déjà (`PlayerSync/TamponInterpolation.h`) et documente précisément ce piège.
        //
        // 2° : choisi pour produire, à distance de conversation (~2 m), le MÊME déplacement
        // perceptible que la bande morte linéaire — 2 × tan(2°) ≈ 7 cm à l'épaule, contre 5 cm. Les
        // deux seuils disent donc la même chose dans deux unités, au lieu d'être réglés séparément.
        //
        // ⚠️ CE QUE ÇA COÛTE, ET POURQUOI CE N'EST PAS LE DÉFAUT DU 2026-08-06. Le pire cas reste
        // **un appel par avatar et par frame** — exactement le pire cas d'aujourd'hui, qu'un avatar
        // oscillant autour de 5 cm atteint déjà. On ajoute une RAISON d'y être, pas un plafond plus
        // haut. Et le chemin employé est `PlacerSansCommande`, c'est-à-dire un `Teleport` seul :
        // **aucune commande d'IA n'est empilée**. L'effondrement des ~3 120 commandes/s venait de
        // `SetEntityPosition`, qui n'est pas appelé ici.
        static constexpr float kBandeMorteYawDeg = 2.0f;
        const auto orientationActuelle =
            Cyberverse::Utils::Entity_GetWorldOrientation(entite.value());
        const float yawActuel =
            Cyberverse::Utils::Quaternion_ToEulerAngles(orientationActuelle).Yaw;
        const float deriveYaw = std::fabs(Tessera::Sync::EcartAngulaire(yawActuel, pose.yaw));

        // ── LE MECANISME CHOISI ICI N'APPLIQUE RIEN — ET C'EST LA BRANCHE LA PLUS FREQUENTE ───
        //
        // ⚠️ Le raisonnement du bloc ci-dessus est juste sur le COUT et faux sur l'EFFET.
        // `PlacerSansCommande` (TeleportationFacility::Teleport) ne deplace PAS nos avatars :
        // prouve sur 3048 echantillons (F-PLY-070), et le cast de handle cense le reparer n'y a
        // rien change (F-PLY-071, refute en regime etabli sur 956 echantillons). Le seul chemin
        // dont on ait la preuve qu'il applique est `SetEntityPosition`, via `AITeleportCommand`
        // (F-PLY-085, A/B franc : corrections coupees, la derive va jusqu'a 43,8 m ; actives, la
        // mediane est a 0,00 m).
        //
        // On avait donc remplace un mecanisme couteux mais QUI MARCHE par un mecanisme gratuit et
        // INERTE. Deux consequences, et Lucas a rapporte les deux sans qu'on fasse le lien :
        //
        //   1. un avatar qui s'arrete en etant decale y reste — DEFINITIVEMENT. Mesure du
        //      2026-08-17 : 12,22 m de derive, corrections reactivees, personne au clavier,
        //      derive INCHANGEE pendant 10 s. Cette branche sort avant les correcteurs qui, eux,
        //      auraient ferme l'ecart.
        //   2. le correctif du 2026-08-15 (« un avatar immobile qui pivote ne tournait jamais »)
        //      n'a jamais pu fonctionner non plus : il passait le yaw a ce meme appel inerte.
        //      D'ou « la tete ne tourne pas quand on tourne la souris ni le corps complet »
        //      (2026-08-16).
        //
        // ── COMMENT ON REPREND LE MECANISME QUI MARCHE SANS REPRENDRE SON COUT ─────────────
        //
        // Le danger de `SetEntityPosition` ici etait reel : appele A CHAQUE FRAME sur chaque avatar
        // immobile, c'est 50 joueurs x 60 fps = 3 000 commandes d'IA par seconde, la zone qui a fait
        // tomber le jeu le 2026-08-06.
        //
        // Mais un avatar IMMOBILE n'a pas besoin d'etre replace soixante fois par seconde : il a
        // besoin de l'etre UNE fois. Rien ne le deplace ensuite — c'est la definition d'immobile.
        // La bande morte de 5 cm garde donc le cas nominal a ZERO appel, et le plafond ci-dessous
        // borne le cas anormal a 4 appels/s et par avatar. Cinquante avatars TOUS decales en meme
        // temps couteraient 200 commandes/s, soit un ordre de grandeur sous le seuil d'effondrement
        // — et ils convergent en moins d'une seconde, apres quoi la bande morte les rend gratuits.
        // ── UN SEUL PLACEMENT EN VOL A LA FOIS — LA FILE REJOUE DES CIBLES PERIMEES ────────
        //
        // ⚠️ La premiere version plafonnait a 4/s, en supposant qu'un placement qui n'aboutit pas
        // est simplement perdu. Il ne l'est pas : `SetEntityPosition` empile un
        // `AITeleportCommand`, et une commande d'IA ne se remplace pas — **elle s'execute**, avec
        // la destination qu'elle portait AU MOMENT OU ELLE A ETE EMPILEE.
        //
        // Mesure du 2026-08-17 (fantome en maintien, personne au clavier), profil pose par pose :
        // apres l'arret, l'avatar derive lentement pendant ~5 s, puis fait un saut de **13 m vers
        // un point FAUX** (derive 17,9 -> 30,4 m), puis 3 m de plus, et seulement ensuite atterrit
        // sur la bonne cible. Ce sont nos propres placements qui se rejouent dans l'ordre, chacun
        // vers la position autoritaire d'il y a une seconde.
        //
        // Emettre plus vite AGGRAVE donc le probleme. On n'en garde qu'un en vol : le suivant
        // n'est emis que si le precedent n'a pas abouti apres un delai franc.
        static constexpr float kPeriodePlacementImmobileS = 2.0f;
        auto& suiviImmobile = g_suiviAvatars[networkId];
        suiviImmobile.depuisPlacementImmobileS += deltaTime;

        // ── ⭐ LE PASSAGER EST L'EXCEPTION, ET C'EST LUI QUI SAUTILLAIT ────────────────────
        //
        // Tout ce qui precede suppose une chose : un avatar « immobile » ne bouge pas VRAIMENT,
        // donc corriger rarement suffit. Un passager d'ascenseur casse cette hypothese — il ne
        // marche pas (`locomotion = 0`, il atterrit donc ici), et pourtant sa position vraie
        // descend a plusieurs metres par seconde.
        //
        // Ce qu'il se passe alors, et c'est EXACTEMENT le « sautillement » decrit par Lucas : le
        // pantin n'est pas porte par la plateforme, il derive donc pendant ces deux secondes
        // pleines, puis un unique placement le ramene d'un coup. Fondu, saut, fondu, saut. Ce
        // n'etait jamais un probleme de synchronisation : le fil disait la verite tout du long,
        // c'est le RYTHME de la correction qui etait cale sur la mauvaise hypothese.
        //
        // Pour lui : chaque frame, sans bande morte. La raison d'etre du garde — « nos propres
        // placements empiles se rejouent vers des cibles perimees » (F-PLY-085) — vaut pour un
        // corps qui ne devrait pas bouger, pas pour un corps qu'on doit suivre en continu.
        // ⛔ Garde par la MEME constante que le calcul de hauteur (voir `Tessera::kAncrageVerticalActif`).
        // A `false`, `passager` est toujours faux : un passager de cabine redevient un avatar
        // distant ordinaire, corrige par le chemin qui a fait ses preuves (F-PLY-085 : mediane a
        // 0,00 m, corrections actives). C'est la simplicite demandee par Lucas le 2026-08-27.
        const bool passager =
            Tessera::kAncrageVerticalActif && g_porteurParAvatar.count(networkId) != 0;

        // ── L'AMORTISSEMENT DE L'ERREUR — la couche de finition, et ses limites ────────────
        //
        // On garde l'ecart entre ce qu'on a AFFICHE et ce qu'on VOULAIT afficher, et on le fait
        // decroitre a chaque frame. Le corps suit donc la cible sans jamais y sauter.
        //
        // Facteurs de Gaffer On Games : 0,95 par frame pour une petite erreur (gomme le bruit),
        // 0,85 pour une grande (ne fait pas trainer une vraie correction), melanges selon
        // l'amplitude. La puissance en `dt * 60` rend le lissage independant de la frequence
        // d'image — sinon un joueur a 120 fps serait lisse deux fois plus fort qu'un joueur a 60.
        //
        // ⚠️ CE QUE CA NE FAIT PAS, ET IL FAUT LE DIRE : ca ne supprime pas la teleportation, donc
        // ca ne traite PAS la cause du flou. C'est une finition, choisie en connaissance de cause
        // apres que les trois voies de fond ont ete fermees (attache script, montage workspot,
        // rendu). Si le flou ne bouge pas d'un pouce, c'est une information, pas un echec.
        // ⚠️⚠️ L'AMORTISSEMENT A ETE RETIRE POUR LES PASSAGERS, ET C'EST UN RENVERSEMENT.
        //
        // Il a ete pose pour MASQUER le flou. En comprenant que le flou EST du mouvement relatif,
        // il devient evident qu'il le FABRIQUAIT : amortir, c'est afficher le corps en retard sur
        // sa cible, donc lui donner en permanence une vitesse par rapport a la camera. On ajoutait
        // la maladie en croyant poser un pansement.
        //
        // Un passager n'a rien a lisser : sa cible est EXACTE (la hauteur du plancher, lue
        // localement dans la frame ou l'on rend). Lisser une cible exacte ne peut que la degrader.
        // Le lissage garde son sens pour un avatar dont la position vient du reseau — pas ici.

        // ⚠️⚠️⚠️ ET SURTOUT : `PlacerSansCommande`, JAMAIS `SetEntityPosition`.
        //
        // Ce garde-la etait deja ecrit, quelques lignes plus haut, et je l'ai enfreint en ajoutant
        // le cas passager : `SetEntityPosition` empile un `AITeleportCommand` dans la file du
        // controleur d'IA AVANT de placer l'entite. L'appeler a chaque frame, c'est empiler
        // soixante commandes par seconde dans la MEME file que la commande de marche — donc
        // annuler soixante fois par seconde l'ordre qui anime l'avatar.
        //
        // Le symptome est decrit mot pour mot dans le corps de `PlacerSansCommande`, d'apres Lucas
        // le 2026-08-13 : « le personnage reste statique sans animation avant de se deplacer »,
        // « CA CREE DU FLOU », « les animations ont du mal a se lancer ». Le 2026-08-25, sur les
        // passagers d'ascenseur, il a redit exactement la meme chose : « il se rafraichit tellement
        // qu'il devient flou ». Meme cause, meme mots, douze jours d'ecart.
        //
        // La lecon generale : une fonction qui a DEUX effets dont un seul est voulu ne doit pas
        // etre appelee en boucle. La variante sans effet de bord existait deja — il fallait la
        // chercher, pas la redecouvrir par le symptome.
        //
        // Bande morte de 1 cm : pendant un trajet elle ne bloque jamais rien (la cabine parcourt
        // 4 cm par frame a 2,5 m/s), et a l'arret elle ramene le cout d'un passager a une
        // soustraction par frame.
        static constexpr float kBandeMortePassagerM = 0.01f;
        if (!g_suspendreCorrections && passager && deriveImmobile > kBandeMortePassagerM)
        {
            suiviImmobile.depuisPlacementImmobileS = 0.0f;
            PlacerSansCommande(entityId, positionVoulue, pose.yaw);
        }

        // ── ⭐ L'INSTRUMENT QUI TRANCHE : LA CIBLE EST-ELLE JUSTE, ET EST-ELLE ATTEINTE ? ────
        //
        // Lucas, 2026-08-26 : « on passe sous le plancher PROGRESSIVEMENT », et le sens s'inverse
        // entre montee et descente. Un retard constant donnerait un ecart FIXE ; un ecart qui
        // GRANDIT veut dire que l'avatar suit la cabine moins vite qu'elle ne bouge.
        //
        // Deux causes possibles, et elles demandent des correctifs opposes :
        //   · la CIBLE derive       -> `plancher + dz` est faux, c'est notre calcul ;
        //   · la cible est juste mais N'EST PAS ATTEINTE -> le placement ne prend pas, et c'est le
        //     moteur qui ramene le corps ailleurs entre deux frames.
        //
        // On ne peut pas les distinguer de l'exterieur : il faut les trois nombres cote a cote.
        // `reel` est relu AVANT le placement de cette frame, donc il porte le resultat du
        // placement PRECEDENT — c'est exactement ce qu'on veut savoir.
        if (passager)
        {
            auto& suiviTrace = g_suiviAvatars[networkId];
            suiviTrace.depuisLogS += deltaTime;
            if (suiviTrace.depuisLogS >= 0.5f)
            {
                suiviTrace.depuisLogS = 0.0f;
                float plancherTrace = 0.0f;
                bool bougeTrace = false;
                const bool lu = HauteurCabineA(g_porteurParAvatar[networkId].cabine, g_tempsLocalS,
                                               plancherTrace, bougeTrace);
                SDK->logger->InfoF(PLUGIN,
                    "[passager %llu] plancher=%.3f (lu=%d bouge=%d) dz=%.3f cible=%.3f reel=%.3f "
                    "ecart=%.3f",
                    networkId, plancherTrace, lu ? 1 : 0, bougeTrace ? 1 : 0,
                    g_porteurParAvatar[networkId].dz, positionVoulue.Z, placeActuelle.Z,
                    placeActuelle.Z - positionVoulue.Z);
            }
        }

        // ⚠️ La garde `g_suspendreCorrections` porte ici AUSSI, et elle manquait.
        //
        // Sans elle, `corrections off` eteignait les trois correcteurs de la branche mobile et
        // laissait celui-ci tourner : un test qui croit avoir tout coupe mesure encore un
        // correcteur actif. C'est exactement la confusion qui a produit un premier verdict faux le
        // 2026-08-17 (F-PLY-085) — un instrument qui n'eteint pas tout ce qu'il pretend eteindre.
        // ── ⭐ LA CIBLE DEFILE-T-ELLE ? (2026-08-27) ────────────────────────────────────────
        //
        // On compare la cible de CETTE frame a celle de la precedente. Un corps vraiment immobile
        // a une cible figee ; un corps PORTE (ascenseur, plateforme, un jour un train) a une cible
        // qui defile alors que sa locomotion dit « au repos ».
        //
        // ⚠️ C'est cette confusion qui a produit le defaut rapporte par Lucas le 2026-08-27 : un
        // passager d'ascenseur tombait dans la branche « immobile », donc corrige toutes les DEUX
        // SECONDES, au-dela de CINQ CENTIMETRES, et par `PlacerSansCommande` — dont ce meme fichier
        // avait deja prouve qu'elle N'APPLIQUE RIEN (F-PLY-070, 3048 echantillons). Trois freins
        // empiles, dont un total : l'avatar ne bougeait tout simplement pas pendant que la cabine
        // descendait autour de lui.
        const RED4ext::Vector4 cibleActuelle = positionVoulue;
        float defilementCible = 0.0f;
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        if (suiviImmobile.cibleImmobileConnue && deltaTime > 0.0001f)
        {
            const float dx = cibleActuelle.X - suiviImmobile.cibleImmobilePrecedente.X;
            const float dy = cibleActuelle.Y - suiviImmobile.cibleImmobilePrecedente.Y;
            const float dz = cibleActuelle.Z - suiviImmobile.cibleImmobilePrecedente.Z;
            defilementCible = std::sqrt(dx * dx + dy * dy + dz * dz);
            vx = dx / deltaTime;
            vy = dy / deltaTime;
            vz = dz / deltaTime;
        }
        suiviImmobile.cibleImmobilePrecedente = cibleActuelle;
        suiviImmobile.cibleImmobileConnue = true;

        // 1 cm entre deux frames : en dessous, c'est du bruit de pose ; au-dessus, la cible defile
        // vraiment. Une cabine a 4 m/s parcourt ~7 cm par frame a 60 fps — tres au-dessus du seuil.
        static constexpr float kDefilementCibleM = 0.01f;
        const bool ciblePortee = defilementCible > kDefilementCibleM;

        // ── ⭐⭐ EXTRAPOLATION : ON ANNULE LE RETARD AU LIEU DE LE SUBIR ─────────────────────
        //
        // Lucas, 2026-08-27 : « quand on monte, les genoux dans le sol ; quand on descend, les
        // avatars volent ». MEME SIGNATURE DANS LES DEUX SENS : l'avatar est EN RETARD sur la
        // cabine. Ce n'est donc pas une imprecision de placement — c'est un decalage entre DEUX
        // HORLOGES, et ce fichier le documentait deja quelques centaines de lignes plus haut :
        //
        //   · la pose reseau est reconstituee a `TempsRendu()`, deliberement en retard d'un tampon
        //     d'interpolation — c'est ce qui rend le mouvement des autres joueurs fluide ;
        //   · la CABINE, elle, est rendue MAINTENANT : c'est une entite locale.
        //
        // A 4 m/s, 100 ms de tampon font 40 cm. C'est exactement la hauteur d'un genou.
        //
        // On avance donc la cible de la duree du tampon, a la vitesse mesuree de la cible. Pour un
        // ascenseur — vitesse quasi constante, aucune entree joueur — l'extrapolation est presque
        // exacte, et elle s'ANNULE d'elle-meme a l'arret (vitesse nulle, donc terme nul).
        //
        // ⚠️ POURQUOI CECI PLUTOT QUE L'ANCRAGE SUR LA CABINE : ca ne demande de connaitre NI la
        // cabine, NI sa geometrie d'etages, NI la courbe de son trajet — les trois choses sur
        // lesquelles la session du 2026-08-27 a bute. Et ca vaut pour TOUT corps porte : plateforme,
        // benne, un jour un train.
        //
        // ⚠️ Le pire cas est BORNE et transitoire : au demarrage et au freinage la vitesse change,
        // donc l'extrapolation depasse d'environ `acceleration x delai²` — quelques centimetres, le
        // temps de la rampe. A comparer aux 40 cm permanents qu'elle supprime.
        if (ciblePortee)
        {
            // Borne de securite : au-dela de 300 ms de tampon on n'extrapole pas plus loin. Un fil
            // degrade ne doit pas projeter un avatar a plusieurs metres de sa cible.
            const float delai = static_cast<float>(g_horlogeRendu.DelaiCourant());
            const float delaiBorne = delai > 0.3f ? 0.3f : (delai < 0.0f ? 0.0f : delai);
            positionVoulue.X += vx * delaiBorne;
            positionVoulue.Y += vy * delaiBorne;
            positionVoulue.Z += vz * delaiBorne;

            // ── ⭐⭐⭐ LA CORRECTION COSMETIQUE : ON S'ALIGNE SUR LE JOUEUR LOCAL ────────────
            //
            // Demande de Lucas, 2026-08-27 : « la hauteur, ce n'est pas grave dans les ascenseurs,
            // on peut tolerer quelques dizaines de centimetres de correction pour avoir quelque
            // chose de propre — que chaque client corrige ca de maniere esthetique. »
            //
            // C'est la bonne idee, et c'est la pratique standard : la position AUTORITAIRE et la
            // position AFFICHEE n'ont aucune raison d'etre la meme. Le serveur garde la verite ;
            // le rendu montre ce qui est juste a l'oeil.
            //
            // ⭐ Le point cle : il existe une reference locale, rendue MAINTENANT, exempte de tout
            // retard reseau — LE JOUEUR LOCAL. S'il est dans la meme cabine, les deux corps sont
            // sur le MEME plancher, donc a la MEME altitude. On n'a besoin de connaitre ni la
            // cabine, ni sa geometrie, ni sa courbe : juste ou sont nos propres pieds.
            //
            // ⚠️ C'est aussi la seule voie qui ne depend d'AUCUN des accesseurs qui ont echoue
            // cette session (F-ASC-041, hauteur de composant ; la geometrie d'etage ; la courbe de
            // trajet). `Entity_GetWorldPosition` sur le joueur local est le chemin le plus eprouve
            // de tout le mod.
            //
            // ── LES DEUX GARDES, ET POURQUOI ELLES SONT ETROITES ──────────────────────────────
            //
            // 1. PROXIMITE HORIZONTALE. Une cabine fait quelques metres ; au-dela, les deux corps
            //    ne sont pas sur le meme plancher et aligner serait FAUX. 3 m couvre une cabine
            //    d'ascenseur et exclut a peu pres tout le reste.
            // 2. ECART VERTICAL BORNE. Si l'ecart depasse 2 m, ce n'est pas un retard de tampon :
            //    c'est un autre etage, une autre cabine, ou une erreur. On ne corrige alors PAS —
            //    mieux vaut un avatar visiblement mal place qu'un avatar teleporte par surprise.
            //
            // ⚠️ ET C'EST BIEN COSMETIQUE : on ne touche qu'a `positionVoulue`, c'est-a-dire a ce
            // qu'on AFFICHE. Rien ne remonte au serveur, aucune pose n'est reecrite, et la position
            // autoritaire de l'autre joueur reste la sienne. Si le serveur et l'ecran divergent de
            // 30 cm dans une cabine, personne ne peut le voir et rien n'en depend.
            static constexpr float kRayonMemePlancherM = 3.0f;
            static constexpr float kEcartVerticalMaxM = 2.0f;
            if (const auto joueurLocal = Cyberverse::Utils::GetPlayer())
            {
                const auto posLocale = Cyberverse::Utils::Entity_GetWorldPosition(joueurLocal);
                const float dxl = positionVoulue.X - posLocale.X;
                const float dyl = positionVoulue.Y - posLocale.Y;
                const float distanceHorizontale = std::sqrt(dxl * dxl + dyl * dyl);
                const float ecartVertical = std::fabs(positionVoulue.Z - posLocale.Z);
                if (distanceHorizontale < kRayonMemePlancherM && ecartVertical < kEcartVerticalMaxM)
                {
                    positionVoulue.Z = posLocale.Z;
                }
            }
        }

        // Une cible qui defile se suit a la CADENCE DES SNAPSHOTS, pas plus vite : corriger a
        // 60 Hz une donnee qui arrive a 20 Hz n'ajoute aucune information et triple le cout.
        static constexpr float kPeriodeCiblePorteeS = 0.05f;
        const float periode = ciblePortee ? kPeriodeCiblePorteeS : kPeriodePlacementImmobileS;
        const float bandeMorte = ciblePortee ? 0.01f : kBandeMorteImmobileM;

        if (!g_suspendreCorrections && !passager
            && (deriveImmobile > bandeMorte || deriveYaw > kBandeMorteYawDeg)
            && suiviImmobile.depuisPlacementImmobileS >= periode)
        {
            suiviImmobile.depuisPlacementImmobileS = 0.0f;
            if (ciblePortee)
            {
                // ⭐ `SetEntityPosition` — le SEUL placement dont l'effet soit prouve (F-PLY-085).
                //
                // Il empile un `AITeleportCommand`, et c'est ce qui interdisait de l'appeler a
                // chaque frame sur un avatar QUI MARCHE : la commande de teleport annule la
                // commande de marche, soixante fois par seconde. Ici le corps est au repos — il n'y
                // a aucune commande de marche a annuler — et la cadence est bornee a 20 Hz.
                //
                // ⚠️ L'autre objection historique etait « ca cree du flou ». Elle est LEVEE : le
                // flou etait le FLOU CINETIQUE du jeu, un reglage graphique, identifie par Lucas le
                // 2026-08-27. F-ASC-037 (« le flou vient du placement image par image ») est donc
                // refute — on avait attribue a notre code un effet qui ne lui appartenait pas.
                SetEntityPosition(entityId, positionVoulue, pose.yaw);
            }
            else
            {
                // Corps reellement immobile : le placement doux suffit, et il ne coute rien.
                PlacerSansCommande(entityId, positionVoulue, pose.yaw);
            }
        }

        // ── T10 bis : L'INSTRUMENT ÉTAIT AVEUGLE SUR UN AVATAR IMMOBILE ────────────────────
        //
        // Relevé au chantier `fiabiliser-le-rendu-des-avatars` (T10 bis) : cette branche sortait
        // AVANT l'appel de télémétrie, donc un avatar parfaitement immobile ne produisait aucune
        // ligne `rx`. Conséquence : `rx=0` ne distinguait pas « rien rendu » de « rendu mais pas
        // mesuré » — deux verdicts opposés, même trace. C'est très exactement la confusion que cet
        // instrument existe pour éliminer, et il la portait lui-même.
        //
        // Le correctif était noté « à faire au prochain build client ». C'est celui-ci.
        if (g_telemetrie.RenduAutorise(networkId, /*force=*/false))
        {
            const auto tamponImmobile = g_tamponsJoueurs.find(networkId);
            g_telemetrie.Rendu(networkId, placeActuelle.X, placeActuelle.Y, placeActuelle.Z,
                               pose.locomotion, deriveImmobile,
                               tamponImmobile != g_tamponsJoueurs.end()
                                   ? tamponImmobile->second.Nombre()
                                   : 0u,
                               pose.extrapolee, g_horlogeRendu.DelaiCourant(),
                               g_horlogeRendu.Gigue(), /*recalage=*/false);
        }
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

    // ── LA MOITIÉ « RÉCEPTION » DE LA MESURE ──────────────────────────────────────────────
    //
    // On journalise la position RÉELLE du pantin (`position`), pas la pose autoritaire visée :
    // c'est celle-là qui est à l'écran, donc celle que Lucas commente. L'écart entre les deux est
    // `derive`, journalisé à côté — ensemble, ils distinguent « le réseau est en retard » de
    // « le moteur traîne derrière une pose pourtant à jour », deux pannes qui se ressemblent
    // exactement vues de face.
    //
    // ⚠️ **ÉCHANTILLONNÉ, ET IL LE FALLAIT.** Cette ligne partait à CHAQUE frame et pour CHAQUE
    // avatar. Tant que « chaque avatar » voulait dire un ou deux, c'était gratuit ; depuis que le
    // plafond de voisins est à 200 (2026-08-15), ça ferait **12 000 lignes par seconde** à 60 fps,
    // chacune suivie d'un `fflush`. L'instrument deviendrait la charge dominante, et la première
    // chose qu'il fausserait serait le temps de frame — c'est-à-dire précisément ce qu'on mesure.
    //
    // `RenduAutorise` plafonne à dix lignes par seconde ET PAR ENTITÉ (jamais globalement : un
    // plafond global laisserait les avatars les plus proches évincer les autres du journal, et un
    // avatar absent du fichier est indiscernable d'un avatar qui va bien).
    //
    // ⚠️ `force` sur le RECALAGE : un événement rare est exactement ce qu'on cherche. Le manquer
    // parce qu'il tombe dans le mauvais dixième de seconde rendrait le journal muet sur le seul
    // symptôme qui compte.
    const bool recalageFranc = derive > kSautFrancM;

    // ── COMBIEN L'AVATAR A-T-IL BOUGE TOUT SEUL DEPUIS QU'ON L'A PLACE ? ────────────────────
    //
    // La question ouverte de F-PLY-064. Le correcteur ferme 15 % de l'ecart par frame, ce qui
    // referme 99,99 % d'un ecart STATIQUE en ~0,1 s a 60 fps — et pourtant 9 m de derive tiennent.
    // Les deux ne se concilient que si quelque chose eloigne l'avatar ENTRE deux corrections.
    //
    // On compare donc la position lue MAINTENANT a celle ou on l'avait laisse a la frame
    // precedente. Cet ecart n'est pas de notre fait : nous, on ne l'a pas touche depuis.
    //   proche de 0                 -> le correcteur n'est pas distance, chercher ailleurs
    //   du meme ordre que la derive -> le moteur deplace l'avatar sous nos pieds, coupable nomme
    const auto& suivi2 = g_suiviAvatars[networkId];
    float libre = -1.0f;
    float depuisPlace = -1.0f;
    const float ecartPose = g_ecartApresPose;
    {
        auto& s = g_suiviAvatars[networkId];
        s.depuisPlaceS += deltaTime;   // avance a chaque passage, remis a 0 au placement
        if (s.placeValide)
        {
            const float lx = position.X - s.placeX;
            const float ly = position.Y - s.placeY;
            const float lz = position.Z - s.placeZ;
            libre = std::sqrt(lx * lx + ly * ly + lz * lz);
            // Le temps ecoulé depuis ce placement. Tout mon raisonnement « 15 % par frame a
            // 60 fps referme un ecart en 0,1 s » supposait un passage PAR FRAME. Si ces passages
            // sont en realite espaces de centaines de millisecondes, le calcul s'effondre — et
            // 7 m de mouvement libre deviennent normaux. C'est la seule variable de l'equation
            // que je n'ai jamais lue.
            depuisPlace = s.depuisPlaceS;
        }
    }

    if (g_telemetrie.RenduAutorise(networkId, /*force=*/recalageFranc))
    {
        g_telemetrie.Rendu(networkId, position.X, position.Y, position.Z, pose.locomotion, derive,
                           tampon != g_tamponsJoueurs.end() ? tampon->second.Nombre() : 0u,
                           pose.extrapolee, g_horlogeRendu.DelaiCourant(), g_horlogeRendu.Gigue(),
                           recalageFranc, libre, depuisPlace, ecartPose, pose.moveDir,
                           suivi2.commandesEmises, suivi2.dernierRetourCommande);
    }

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
    // ⚠️ SAUF EN L'AIR — ET C'EST MOI QUI AI CASSÉ LE SAUT EN L'OUBLIANT.
    //
    // Signalé par Lucas le 2026-08-13, juste après que j'aie rendu la verticale au moteur : « le
    // saut en hauteur ne se fait plus du tout, il n'y en a même pas l'animation ».
    //
    // Le raisonnement « le sol appartient au moteur » est juste — TANT QUE l'avatar est au sol. Un
    // joueur en l'air n'a plus de sol : le moteur le repose par gravité, et notre correction
    // verticale, bridée à 15 % et seulement au-dessus d'un demi-mètre, ne pouvait pas gagner. Le
    // pantin restait collé au sol pendant que le joueur sautait.
    //
    // `locomotion == 6` (InAir/Jump) dit exactement cela, et le champ voyage sur le fil depuis le
    // gel du palier 2. En l'air, la verticale redevient NÔTRE et à pleine amplitude : c'est la
    // seule information qui décrive un saut, et rien dans le moteur ne la conteste puisqu'il n'y a
    // pas de sol sous les pieds.
    static constexpr std::uint8_t kLocomotionEnLair = 6;
    const bool enLair = pose.locomotion == kLocomotionEnLair;

    static constexpr float kSeuilVerticalM = 0.5f;
    const float deriveHorizontale = std::sqrt(dx * dx + dy * dy);

    // ── LE CORRECTEUR PERDAIT LA COURSE, ET C'ETAIT ARITHMETIQUE (F-PLY-065) ────────────────
    //
    // Mesure du 2026-08-16, joueur en marche : derive moyenne 3,004 m, et l'avatar s'eloigne de
    // 1,156 m TOUT SEUL entre deux corrections (champ `libre`). Une correction de 15 % du gap ne
    // regagnait que 0,451 m. Bilan : +0,705 m par cycle, donc une derive qui CROIT jusqu'au point
    // fixe 0,15 x d = 1,156, soit d ~ 7,7 m — exactement les 9 m observes la veille. Le recalage
    // franc n'intervenant qu'a 15 m, l'equilibre s'installait sous son seuil et il ne se
    // declenchait jamais : l'avatar trainait a sept metres et rien ne considerait ca comme anormal.
    //
    // ⚠️ POURQUOI MONTER `kFractionCorrection` N'AURAIT PAS SUFFI. Le mouvement libre est a peu
    // pres CONSTANT (c'est notre propre commande de marche, que le moteur execute a SA vitesse vers
    // un point a 5 m devant — `ignoreNavigation = true`, donc la navigation est hors de cause).
    // Une correction PROPORTIONNELLE au gap est donc toujours perdante quand le gap est petit :
    // il faudrait f > 2,3 pour tenir a 50 cm de derive, ce qui n'a pas de sens.
    //
    // Le correctif ajoute un terme ABSOLU : on rattrape ce que l'avatar vient de perdre tout seul,
    // PLUS 15 % du reste. Le bilan par cycle devient -f x d — une decroissance geometrique propre,
    // a n'importe quelle amplitude de derive.
    //
    // On garde `kFractionCorrection` comme plancher (premier passage, ou `libre` indisponible) et
    // on plafonne a 1.0 : corriger plus que l'ecart ferait depasser la cible.
    // ⚠️ CORRECTIF RETIRE (2026-08-16, le jour meme). Ajouter un terme absolu au correcteur
    // faisait saturer la fraction a 1,0 — on teleportait l'avatar pile sur sa cible a chaque
    // passage — et la derive a EMPIRE (3,0 -> 6,7 m), le mouvement libre avec (1,16 -> 6,99 m).
    // Un correcteur maximal qui ne rattrape toujours pas veut dire que le probleme n'est PAS sa
    // force : ce sont les passages qui sont trop espaces. On mesure donc ca, au lieu de le deviner.

    // ── LE SAUT ÉTAIT ÉCRIT CONTRE UNE FONCTION INERTE (corrigé le 2026-08-19) ───────────
    //
    // Le raisonnement ci-dessus est juste — en l'air, la verticale nous revient — mais il
    // s'appliquait par `PlacerSansCommande`, dont on a MESURÉ depuis qu'elle n'applique RIEN sur un
    // avatar distant (F-PLY-073 : 10,000 m d'écart sur une cible arbitraire ; F-PLY-067 : la
    // position relue dans la MÊME frame est déjà fausse). Le correctif du 2026-08-13 rendait donc la
    // verticale à une fonction qui ne la posait pas : le pantin restait collé au sol, exactement
    // comme avant le correctif, et la plainte de Lucas (« le saut ne se fait plus du tout ») n'a
    // jamais cessé d'être vraie.
    //
    // C'est la famille de défaut du jour : un geste correct envoyé par un canal inerte. Même cause
    // que la branche « immobile » quelques lignes plus haut, même correctif.
    //
    // `SetEntityPosition` est le seul placement dont l'effet soit mesuré (F-PLY-085). Il empile un
    // `AITeleportCommand`, d'où la CADENCE, qui n'est pas une précaution de style : à 60 fps sans
    // borne, 50 sauteurs simultanés referaient les ~3 000 commandes/s qui ont fait tomber le jeu
    // deux fois le 2026-08-06. 10 Hz donne ~8 placements sur un saut de 0,8 s — assez pour dessiner
    // l'arc — et plafonne ce même pire cas à 500/s.
    //
    // `depuisPlaceS` est réutilisé tel quel : il est incrémenté à chaque passage juste au-dessus et
    // remis à zéro par tout placement, donc il dit déjà exactement « depuis combien de temps cet
    // avatar n'a pas été posé ». La branche immobile, elle, a besoin de son propre compteur parce
    // qu'elle sort AVANT l'incrément (voir `depuisPlacementImmobileS`).
    // ⚠️⚠️ 100 ms ETAIT LA MOITIE DU DEFAUT DE L'ASCENSEUR, et le nombre le dit.
    //
    // Cette cadence a ete choisie pour un SAUT : un corps en chute libre, une seconde, ou 100 ms de
    // retard ne se voient pas. Mais `locomotion == 6` (« en l'air ») couvre AUSSI le passager d'une
    // plateforme en mouvement — le jeu le classe ainsi parce que le sol bouge sous lui.
    //
    // A 3 m/s, 100 ms font 30 cm de derive entre deux corrections. Le releve du 2026-08-27 mesure
    // un ecart qui croit avec la vitesse jusqu'a 44 cm : ~30 cm de cadence + ~14 cm de tampon
    // d'interpolation (traite par l'extrapolation, plus haut). Les deux termes comptent, et aucun
    // des deux n'est negligeable devant l'autre.
    //
    // 30 ms (~33 Hz) ramene le terme de cadence sous 10 cm. C'est plus rapide que l'arrivee des
    // snapshots (25 Hz), donc on ne cree pas d'information — on cesse seulement d'en perdre.
    //
    // ⚠️ Le cout est borne et connu : `SetEntityPosition` empile un `AITeleportCommand`, et c'est
    // ce qui avait fait tomber le jeu a 60 Hz par avatar le 2026-08-06. Ici on reste sous 33 Hz,
    // et SEULEMENT pour un avatar en l'air — un etat rare et bref hors ascenseur.
    static constexpr float kPeriodePlacementVolS = 0.03f;
    if (!g_suspendreCorrections && enLair && derive <= kSautFrancM)
    {
        auto& s = g_suiviAvatars[networkId];
        // ── ⭐ A LA VITESSE DE POINTE, ON CORRIGE A CHAQUE IMAGE ────────────────────────────
        //
        // Lucas, 2026-08-27 : « c'est pendant la vitesse de pointe que vraiment ça n'arrive pas a
        // se recaler ». Le nombre lui donne raison : entre deux corrections espacees de 30 ms, une
        // cabine a 6 m/s parcourt 18 cm. La cadence est donc, a elle seule, une source d'erreur
        // PROPORTIONNELLE A LA VITESSE — celle que l'on voit au milieu du trajet et nulle part
        // ailleurs.
        //
        // On supprime l'espacement quand la cible defile vite, et on le garde sinon. Le cout est
        // borne a ce cas precis : un corps porte par une plateforme rapide, ce qui est rare et
        // bref. Un saut ordinaire (l'autre usage de `locomotion == 6`) reste a 33 Hz.
        //
        // ⚠️ Pourquoi ce n'est pas le defaut de 2026-08-06 : cet effondrement venait de
        // `SetEntityPosition` appele a 60 Hz sur CHAQUE avatar, en permanence. Ici c'est un avatar
        // dont la cible DEFILE de plus de 8 cm par image — autant dire, en pratique, quelqu'un dans
        // un ascenseur rapide.
        // Le declencheur se lit sur place : « la cible a-t-elle quitte de plus de 8 cm l'endroit ou
        // l'on a POSE le corps la derniere fois ? ». Il croit tout seul entre deux corrections, donc
        // il se declenche d'autant plus tot que la cabine va vite — exactement le comportement
        // voulu, et sans avoir a estimer une vitesse.
        static constexpr float kEcartDepuisPlacementM = 0.08f;
        const bool cibleTropLoin =
            s.placeValide && std::fabs(positionVoulue.Z - s.placeZ) > kEcartDepuisPlacementM;
        if (cibleTropLoin || s.depuisPlaceS >= kPeriodePlacementVolS)
        {
            // Un saut dure moins d'une seconde : l'amortir reviendrait à ne jamais le montrer. On
            // suit donc la verticale SANS lissage, et on garde l'amortissement sur l'horizontale.
            const RED4ext::Vector4 vol{
                position.X + dx * kFractionCorrection,
                position.Y + dy * kFractionCorrection,
                positionVoulue.Z,
                1.0f};
            SetEntityPosition(entityId, vol, pose.yaw);
            s.placeX = vol.X; s.placeY = vol.Y; s.depuisPlaceS = 0.0f; s.placeZ = vol.Z; s.placeValide = true;
            const auto reluvol = Cyberverse::Utils::Entity_GetWorldPosition(entite.value());
            const float rvolx = reluvol.X - vol.X, rvoly = reluvol.Y - vol.Y, rvolz = reluvol.Z - vol.Z;
            g_ecartApresPose = std::sqrt(rvolx * rvolx + rvoly * rvoly + rvolz * rvolz);
            g_telemetrie.Evenement("saut_place", networkId, "");

            // ── ⭐ L'INSTRUMENT QUI SEPARE LES DEUX SEULES CAUSES POSSIBLES ─────────────────
            //
            // Releve du 2026-08-27 : a la vitesse de pointe, l'ecart reste a ~20 cm MALGRE une
            // cible deja extrapolee. Deux causes, et elles demandent des correctifs OPPOSES :
            //
            //   · le placement n'est pas APPELE assez souvent  -> `rvolz` sera petit (on atteint la
            //     cible quand on la vise) et l'ecart se creuse ENTRE deux appels ;
            //   · le placement est appele et ECRASE juste apres -> `rvolz` sera deja grand DANS LA
            //     MEME FRAME : le moteur ramene le corps ailleurs immediatement.
            //
            // `rvolz` est relu APRES le placement, dans la meme frame. C'est la seule facon de les
            // distinguer de l'exterieur — et c'est exactement le genre de question que j'ai passe
            // la journee a ne pas poser.
            {
                auto& tracePose = g_suiviAvatars[networkId];
                tracePose.depuisLogS += deltaTime;
                if (std::fabs(vol.Z - position.Z) > 0.05f && tracePose.depuisLogS >= 0.4f)
                {
                    tracePose.depuisLogS = 0.0f;
                    SDK->logger->InfoF(PLUGIN,
                        "[pose %llu] visee.z=%.3f avant.z=%.3f APRES.z=%.3f | colle=%+.3f "
                        "| demande=%+.3f | dt=%.4f",
                        networkId, vol.Z, position.Z, reluvol.Z, reluvol.Z - vol.Z,
                        vol.Z - position.Z, deltaTime);
                }
            }
        }
    }
    else if (!g_suspendreCorrections && deriveHorizontale > kCorrectionMiniM
             && derive <= kSautFrancM)
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
        // ⚠️ « Figer, placer, recommander » a ete essaye ici le 2026-08-16 et RETIRE le meme
        // jour : annuler la commande avant de teleporter n'a pas fait prendre le placement
        // (`pose` 1,34 -> 1,64 m, inchange). L'hypothese « le systeme de mouvement ecrase
        // notre Teleport tant qu'une commande tourne » est donc REFUTEE — et figer a chaque
        // correction hacherait l'animation pour un gain nul. Voir F-PLY-068.
        // ── LE SEUL CHEMIN DE PLACEMENT DONT ON AIT LA PREUVE QU'IL APPLIQUE ──────────────
        //
        // `PlacerSansCommande` (TeleportationFacility::Teleport) n'applique RIEN sur nos avatars :
        // prouve sur 3048 echantillons (F-PLY-070), et le cast de handle n'y a rien change
        // (F-PLY-071, refute en regime etabli sur 956 echantillons). `SetEntityPosition`, lui,
        // passe par un `AITeleportCommand` — mecanisme different, employe par le recalage franc
        // au-dela de 15 m, et qui fonctionne.
        //
        // ⚠️ C'EST PRECISEMENT CE QU'ON AVAIT RETIRE, ET IL FAUT SAVOIR POURQUOI. Ce chemin
        // empile une commande dans la file de l'IA. Appele A CHAQUE FRAME sur chaque avatar (ce que
        // faisait la branche « immobile »), il a produit l'effondrement des ~3120 commandes/s du
        // 2026-08-06. Ici c'est different sur deux points, mesures : il ne se declenche qu'au-dela
        // de 25 cm de derive, et la cadence reelle des corrections est de 25 a 68 ms — soit 15 a
        // 40 appels/s par avatar, pas 60.
        //
        // ⚠️ NON MESURE, et c'est la borne a garder en tete : le cout a 200 voisins. 40 appels/s
        // x 200 = 8000 commandes/s, ce qui est DANS la zone qui a fait tomber le jeu. Une commande
        // de teleport n'est pas une commande de marche et rien ne dit qu'elle coute pareil — mais
        // tant que ce n'est pas mesure, ce chemin n'est valide qu'a faible densite.
        SetEntityPosition(entityId, pas, pose.yaw);
        { auto& s = g_suiviAvatars[networkId];
          s.placeX = pas.X; s.placeY = pas.Y; s.depuisPlaceS = 0.0f; s.placeZ = pas.Z; s.placeValide = true;
          const auto relupas = Cyberverse::Utils::Entity_GetWorldPosition(entite.value());
          const float rpasx = relupas.X - pas.X, rpasy = relupas.Y - pas.Y, rpasz = relupas.Z - pas.Z;
          g_ecartApresPose = std::sqrt(rpasx * rpasx + rpasy * rpasy + rpasz * rpasz); }
    }

    if (!g_suspendreCorrections && derive > kSautFrancM)
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
        { auto& s = g_suiviAvatars[networkId];
          s.placeX = positionVoulue.X; s.placeY = positionVoulue.Y; s.depuisPlaceS = 0.0f; s.placeZ = positionVoulue.Z; s.placeValide = true;
          const auto relupositionVoulue = Cyberverse::Utils::Entity_GetWorldPosition(entite.value());
          const float rpositionVouluex = relupositionVoulue.X - positionVoulue.X, rpositionVouluey = relupositionVoulue.Y - positionVoulue.Y, rpositionVouluez = relupositionVoulue.Z - positionVoulue.Z;
          g_ecartApresPose = std::sqrt(rpositionVouluex * rpositionVouluex + rpositionVouluey * rpositionVouluey + rpositionVouluez * rpositionVouluez); }
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
    // ⚠️ CES DEUX SEUILS SONT DE LA LATENCE QUE NOUS AJOUTONS NOUS-MÊMES.
    //
    // Le plafond de 0,25 s et l'écart de 1,5 m ont été dimensionnés le 2026-08-06 pour **156
    // pantins de foule**, après qu'un ordre par tick eut fait tomber le jeu. Ils ont été repris
    // tels quels pour les avatars de JOUEURS — et le raisonnement ne se transporte pas : on en a
    // une poignée, pas 156, et c'est précisément sur eux que le retard se voit.
    //
    // 0,10 s / 0,75 m : au pire 10 ordres par seconde et par avatar. À dix joueurs visibles, cent
    // ordres par seconde — un ordre de grandeur en dessous des 3 120/s qui avaient fait tomber le
    // jeu. Le changement d'allure, lui, continue de court-circuiter le plafond : un démarrage ne
    // paie aucun de ces deux seuils.
    static constexpr float kReemissionMaxS = 0.10f;
    static constexpr float kEcartVisee = 0.75f;
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

    // ── EN PILOTAGE PAR ENTREES, C'EST L'ENTREE QUI DECIDE DE LA REEMISSION ────────────────
    //
    // La garde d'origine compare la CIBLE a la precedente. Elle est juste quand la cible vient de
    // la position recue : celle-ci avance avec le joueur, donc la garde s'ouvre. Mais en pilotage
    // par entrees la cible est calculee depuis la position COURANTE de l'avatar — tant qu'il
    // n'avance pas, elle ne bouge pas, la garde se referme sur elle-meme et l'avatar ne recoit
    // qu'UNE seule commande. Mesure du 2026-08-16 : 4,4 m parcourus en ligne droite, sinuosite 1,0,
    // pour 88,8 m de marche reelle.
    //
    // On compare donc les ENTREES : un changement de direction (move_dir) ou de regard (yaw) est
    // un evenement qui doit reemettre, meme si la cible calculee se ressemble.
    bool entreeAChange = false;
    if (g_pilotageParEntrees)
    {
        const int ecartDir = std::abs(static_cast<int>(pose.moveDir)
                                      - static_cast<int>(suivi.derniereMoveDir));
        // 8 crans sur 256 = ~11 degres. Sous ce seuil, c'est du bruit de quantization.
        const bool dirAChange = std::min(ecartDir, 256 - ecartDir) > 8;
        const bool yawAChange =
            std::fabs(Tessera::Sync::EcartAngulaire(suivi.dernierYawEntree, pose.yaw)) > 10.0f;
        entreeAChange = dirAChange || yawAChange;
        if (entreeAChange || !suivi.commande)
        {
            suivi.derniereMoveDir = pose.moveDir;
            suivi.dernierYawEntree = pose.yaw;
        }
    }

    if (suivi.commande && !entreeAChange && !cibleABouge && !allureAChange
        && suivi.depuisS < kReemissionMaxS)
    {
        return; // il est deja en route, dans la bonne direction : on le laisse marcher.
    }

    RED4ext::Vector4 visee = { cx, cy, cz, 1.0f };

    // ── PILOTAGE PAR LES ENTREES (ADR 0032) ────────────────────────────────────────────────
    //
    // La destination ne vient plus de la position INTERPOLEE du serveur, mais du couple
    // (`yaw`, `move_dir`) applique a la position ou l'avatar se trouve DEJA. `move_dir` est la
    // direction du deplacement RELATIVE au regard, sur 256 crans (protocol.fbs) — c'est donc
    // exactement une entree de joueur, et elle voyage depuis le gel du palier 2 sans avoir jamais
    // ete consommee ici.
    //
    // `move_dir == 0` signifie IMMOBILE (et non « droit devant ») : dans ce cas on ne vise rien de
    // neuf, la branche « locomotion == 0 » plus haut a deja fige l'avatar.
    // ⚠️ PAS DE GARDE SUR `moveDir != 0` — ELLE SAUTAIT LE CAS LE PLUS FREQUENT.
    //
    // `move_dir` est la direction du deplacement RELATIVE AU REGARD. Marcher droit devant donne un
    // angle nul, donc `move_dir == 0`. Or le schema documente aussi 0 comme « immobile » : la
    // valeur porte DEUX sens et rien ne les separe — l'emetteur lui-meme ne peut pas les
    // distinguer, puisque l'angle est identique.
    //
    // Mesure du 2026-08-16, joueur marchant 81,8 m : sur 1352 emissions en mouvement, `move_dir`
    // vaut 0 dans 56 % des cas et se groupe autour de 0 dans le reste (255, 1, 2, 3, 254). En
    // sautant les zeros, on ignorait donc TOUTE la marche en ligne droite — l'avatar ne recevait
    // d'ordre que pendant les virages, d'ou 2,3 m parcourus pour 90,8 m de marche reelle.
    //
    // Le test d'immobilite se fait sur `locomotion == 0`, qui est sans ambiguite, et il est deja
    // fait plus haut dans cette fonction.
    if (g_pilotageParEntrees)
    {
        const auto ici = Cyberverse::Utils::Entity_GetWorldPosition(entite.value());

        // ── LE SIGNE DU YAW, ET C'EST TOUT LE BUG ──────────────────────────────────────────
        //
        // `pose.yaw` est le yaw d'EULER du moteur : il tourne dans le sens TRIGONOMETRIQUE, donc
        // l'avant vaut `(-sin(yaw), cos(yaw))` et non `(+sin(yaw), cos(yaw))`. La premiere version
        // de ce calcul prenait `+sin` : elle visait le point SYMETRIQUE par rapport a l'axe nord.
        // L'ordre partait, il etait accepte, le corps marchait — dans une direction miroir qui
        // changeait avec le regard du joueur. D'ou 903 commandes acceptees pour 5,3 m parcourus,
        // un symptome qui ressemblait a s'y meprendre a une cadence trop rapide.
        //
        // MESURE (2026-08-17, telemetrie deja au disque, sans relancer le jeu) : cap boussole reel
        // du deplacement compare au cap predit, sur 1298 pas de marche —
        //   ecart median  +sin : 154,4 deg      -sin : 0,2 deg
        // Et le signe de `move_dir`, sur 214 echantillons de strafe/recul (|move_dir| > 25 deg) :
        //   `-yaw + move_dir` : 0,6 deg   ·   `-yaw - move_dir` : 90,6   ·   sans : 45,6
        //
        // Le meme resultat se lit sur la premiere ligne de n'importe quel journal : `yaw` et
        // `lyaw` (le cap boussole de la camera, `atan2(f.X, f.Y)`) somment a 360,0 exactement.
        // Et `PointDeRegard` cote redscript, ecrit bien avant, fait deja
        // `RotByAngleXY((0,1,0), yaw)` — c'est-a-dire exactement `(-sin, cos)`. Trois sources
        // concordantes, dont deux anterieures : c'etait ce calcul-ci qui etait seul de son cote.
        const float capBoussole =
            -pose.yaw + static_cast<float>(pose.moveDir) * (360.0f / 256.0f);
        const float rad = capBoussole * 3.14159265f / 180.0f;
        // 6 m : assez loin pour que l'allure s'exprime (mesure du 2026-07-23 : `movementType` ne
        // montre son effet qu'avec de la distance a couvrir), assez court pour que la direction
        // reste fraiche entre deux snapshots.
        visee = { ici.X + std::sin(rad) * 6.0f, ici.Y + std::cos(rad) * 6.0f, ici.Z, 1.0f };
    }
    bool enRoute = false;
    // `pose.yaw` en quatrieme argument : la direction du REGARD, distincte de celle du
    // deplacement. C'est ce qui donne la marche arriere et le pas de cote — voir
    // `TesseraSuivreAvatar` cote redscript.
    // ── LE REGARD, POUSSE A CHAQUE PASSAGE ────────────────────────────────────────────────
    //
    // Le seuil de re-pose (8 deg) vit cote redscript, la ou l'etat precedent est memorise : on
    // appelle donc sans condition et c'est le consommateur qui decide s'il y a lieu d'agir.
    // Cout d'un appel qui ne fait rien : une comparaison d'angles.
    //
    // Pose AVANT la garde de suspension : le regard n'a rien a voir avec la commande de marche, et
    // suspendre l'une ne doit pas eteindre l'autre — sinon un test de placement rendrait aussi les
    // avatars aveugles, et on melangerait deux effets.
    if (pose.lookYaw != 0.0f || pose.lookPitch != 0.0f)
    {
        bool poseOk = false;
        Red::CallVirtual(this, "TesseraPousserRegard", poseOk, entityId, pose.lookYaw,
                         pose.lookPitch);
    }

    // ⚠️ Suspension de mesure (voir `g_suspendreCommandes`) : on n'emet plus la commande de
    // marche, pour qu'un test de placement ne soit pas defait par notre propre boucle.
    if (g_suspendreCommandes)
    {
        return;
    }
    ++suivi.commandesEmises;
    if (Red::CallVirtual(this, "TesseraSuivreAvatar", enRoute, entityId, visee,
                         static_cast<int32_t>(pose.locomotion), pose.yaw)
        && (suivi.dernierRetourCommande = enRoute ? 1 : 0, enRoute))
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

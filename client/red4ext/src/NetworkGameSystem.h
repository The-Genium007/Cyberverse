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
#include <set>   // suivi des apparences statiques deja appliquees (hydratation discrete)
#include <string>
#include <vector>
#include <steam/isteamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

#include <serverbound/WorldPacketsServerBound.h>

// Protocole TesseraSynth (FlatBuffers) — forward decl pour ne pas tirer l'en-tête généré ici.
namespace cyberpunk_rp::protocol {
    struct Snapshot; struct PositionCorrection; struct ShardAssignment; struct StaticAppearance;
    struct WorldState; struct Kicked; struct AppearanceSync; struct ConfigSync;
    struct PlayerEvent;
    // ⚠️ Oublier une declaration avancee ici ne donne PAS « type inconnu » : le compilateur lit
    // `const CharacterList*` comme `const int` et l'erreur sort a l'APPEL, sous la forme
    // « impossible de convertir 'const CharacterList *' en 'const int' » — un message qui pointe
    // vers l'appelant alors que le defaut est ici. Piege deja paye une fois (2026-08-07).
    struct CharacterList; struct CharacterResult;
}
    // Piege paye une SECONDE fois le 2026-08-08 : `HandleHealthSync` a ete declaree plus bas sans
    // passer par ici, et le build entier tombait sur « 'HealthSync' n'est pas membre de
    // cyberpunk_rp::protocol ». Consequence en chaine : plus de DLL, donc un `.reds` deja deploye
    // (jonction vers le jeu) qui declare `Tessera_RapporterDegats` sans backing natif, donc TOUT
    // r6/scripts par terre au prochain lancement. Tout nouveau `Handle<X>` se declare ICI en meme
    // temps qu'il se declare plus bas.
    struct HealthSync;

// Apparence faisant autorité pour chaque PNJ STATIQUE, par EntityID — définie dans le .cpp.
// Hors de la classe : `NetworkGameSystem` est alloué par le moteur (`RTTI_IMPL_ALLOCATOR`), lui
// ajouter un membre corrompt la mémoire voisine (mesuré le 2026-08-06).
extern std::map<uint64_t, uint64_t> g_apparencesStatiques;
// Statiques dont l'apparence autoritaire a REELLEMENT ete appliquee. La difference avec
// `g_apparencesStatiques` est la file de travail de l'hydratation discrete.
extern std::set<uint64_t> g_apparencesAppliquees;
// Sonde d'apparence : premiere apparence vue par record, et garde one-shot.
extern std::map<uint64_t, uint64_t> g_premiereApparence;
extern bool g_sondeApparenceFaite;

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
    void SetEntityPose(uint64_t networkId, RED4ext::ent::EntityID entityId,
                       RED4ext::Vector4 worldPosition, float yaw, uint8_t locomotion,
                       const RED4ext::Vector4* moveTarget = nullptr);

protected:
    void PollIncomingMessages();
    void TrackPlayerPosition(float deltaTime);

    // --- Couture protocole TesseraSynth (FlatBuffers) ---
    // Envoient un ClientEnvelope (Join / PositionUpdate) au serveur Rust autoritaire.
    void SendJoin(const std::string& displayName);
    void SendPositionUpdate(float x, float y, float z, float yaw);
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
    // Demande de prise d'autorite sur un figurant local. Porte de quoi le REFABRIQUER, pas un
    // identifiant : voir `PromotionRequest` dans protocol.fbs.
    /// Renvoie true si la requete est REELLEMENT partie. Un false signifie deduplication, absence
    /// de connexion ou record nul — et l'appelant ne doit alors surtout pas masquer son pantin
    /// local : il effacerait un corps sans qu'aucun ne le remplace.
    bool SendPromotionRequest(uint64_t record, uint64_t apparence, float x, float y, float z,
                              float yaw, bool mort);
    // Rapporte un PNJ STATIQUE et l'apparence qu'on lui voit. Le serveur arbitre laquelle fait foi.
    void SendStaticNpcReport(uint64_t entityId, uint64_t record, uint64_t apparence);
    // Vide la file des rapports de statiques, UN PAR TICK au plus et pas plus vite que la cadence
    // fixee. Appelee depuis `OnNetworkUpdate`.
    void DrainerRapportsStatiques();
    // Applique les apparences autoritaires encore en attente, UNE par tick au plus, et seulement
    // quand le PNJ est hors du champ de vision du joueur. Voir le commentaire dans le .cpp.
    void HydraterApparencesDiscretement();
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
    // Liste des personnages du compte, poussee par le serveur apres le Join et apres chaque
    // creation. REMPLACE l'etat local : le serveur envoie toujours la liste complete, jamais un
    // delta — donc pas de fusion a faire, et un personnage supprime disparait de lui-meme.
    void HandleCharacterList(const cyberpunk_rp::protocol::CharacterList* list);
    // Verdict d'une creation de personnage (succes, ou motif de refus).
    void HandleCharacterResult(const cyberpunk_rp::protocol::CharacterResult* result);

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
    Red::CString Tessera_NomPersonnage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_personnages[static_cast<size_t>(index)].pseudonyme.c_str());
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
                                   RED4ext::CName apparence)
    {
        SendStaticNpcReport(cible.hash, record, apparence.hash);
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
    RTTI_METHOD(Tessera_Journal);
    RTTI_METHOD(Tessera_RapporterStatique);
    RTTI_METHOD(Tessera_ApparenceStatiqueConnue);
    RTTI_METHOD(Tessera_CobayeApparence);
    RTTI_METHOD(Tessera_DemanderPromotion);
    RTTI_METHOD(Tessera_NombrePersonnages);
    RTTI_METHOD(Tessera_ListePersonnagesRecue);
    RTTI_METHOD(Tessera_NomPersonnage);
    RTTI_METHOD(Tessera_IdPersonnage);
    RTTI_METHOD(Tessera_DernierResultat);
    RTTI_METHOD(Tessera_CreerPersonnage);
    RTTI_METHOD(Tessera_ChoisirPersonnage);
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

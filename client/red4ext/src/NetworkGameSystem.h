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
#include <deque>
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
    // ⚠️⚠️ QUATRIEME FOIS, 2026-08-30, et le message d'erreur etait EXACTEMENT celui que le
    // commentaire du haut decrit : « 'CommandCatalog' n'est pas membre de cyberpunk_rp::protocol »
    // sur la LIGNE DE DECLARATION, plus « impossible de convertir ... en 'const int' » a l'appel,
    // 640 lignes plus loin. Le piege est documente ici depuis le 2026-08-07 — le lire ne suffit
    // pas, il faut y penser au moment d'ajouter un `Handle<X>`.
    // ⭐ Le type est pourtant BIEN dans `generated/protocol_generated.h` : verifier le header
    // genere ne dit RIEN sur ce bloc, qui est une liste tenue A LA MAIN.
    struct CommandCatalog;
    // Inventaire sous autorite serveur (ADR 0026). QUATRIEME fois que ce bloc est oublie —
    // 2026-08-13, avec exactement le message annonce ci-dessus (« impossible de convertir
    // 'const InventaireAutoritaire *' en 'const int' », qui pointe vers l'appelant alors que le
    // defaut est ICI). Le build local l'a attrape en une minute ; la relecture, non.
    struct InventaireAutoritaire;
    // Ascenseurs (ADR 0012). CINQUIEME fois que ce bloc est oublie -- 2026-08-24, meme
    // message a la lettre ("impossible de convertir 'const ElevatorStateMsg *' en 'const
    // int'", pointe sur l'appelant, le defaut est ICI). Le commentaire du dessus l'annoncait
    // mot pour mot et je l'ai lu APRES l'erreur. Le build local l'attrape en une minute.
    struct ElevatorStateMsg;
    struct DeviceStateMsg;
    // Coffre de vehicule. SIXIEME fois que ce bloc est oublie -- 2026-08-25. Les deux commentaires
    // ci-dessus l'annoncent mot pour mot, et je les ai lus APRES l'erreur, comme les deux fois
    // precedentes. Le message ne designe jamais ce bloc : il pointe l'APPELANT.
    //
    // ⚠️ Six oublis identiques ne sont plus une inattention, c'est un defaut de conception : ce
    // fichier oblige a declarer le type DEUX fois, a deux endroits eloignes, sans que rien ne relie
    // l'un a l'autre. Le correctif structurel serait d'inclure `protocol_generated.h` ici — au prix
    // d'un en-tete lourd, ce qui est un vrai cout et un vrai arbitrage. Tant qu'on ne le fait pas,
    // ce commentaire est le seul garde-fou, et il ne marche visiblement pas.
    struct InteractionOpen;
}

// Apparence faisant autorité pour chaque PNJ STATIQUE, par EntityID — définie dans le .cpp.
// Hors de la classe : `NetworkGameSystem` est alloué par le moteur (`RTTI_IMPL_ALLOCATOR`), lui
// ajouter un membre corrompt la mémoire voisine (mesuré le 2026-08-06).
// ── ASCENSEURS : la file des états reçus, DRAINÉE PAR REDSCRIPT ──────────────────────────────
//
// Pourquoi une file et pas un appel direct dans redscript : trois voies de PUSH ont été essayées
// le 2026-08-24 et ont toutes échoué à la résolution de nom
// (`@addMethod` sur une classe de module, `Red::CallGlobal` sur une fonction globale,
// `Red::CallStatic` sur une classe globale) — chacune compile, se déploie, et laisse le netcode
// journaliser « absent du modset » au runtime. Le PULL est le patron déjà utilisé partout ici
// (`Tessera_SacRecu`, `Tessera_NombreActions`, `Tessera_ListePersonnagesRecue`) : il ne dépend
// d'aucune résolution de symbole scripté, seulement des natifs que la DLL enregistre elle-même.
//
// ⚠️ HORS DE LA CLASSE, comme `g_apparencesStatiques` : `NetworkGameSystem` est alloué par le
// moteur (`RTTI_IMPL_ALLOCATOR`), lui ajouter un membre corrompt la mémoire voisine (mesuré le
// 2026-08-06).
struct EtatAscenseurRecu
{
    uint64_t elevatorId = 0;
    int32_t etageActif = 0;
    int32_t etageCible = -1;
    int32_t departTick = 0;
    int32_t elapsedMs = 0;
};
extern std::deque<EtatAscenseurRecu> g_ascenseursRecus;

// APPAREILS DU MONDE (portes, portiques, contenants...) — spec 2026-08-26.
//
// Meme forme que la file ascenseur, et pour la meme raison : le redscript ne peut pas recevoir un
// push du C++ (les trois voies echouent a la resolution de nom au RUNTIME, en silence — voir
// l'en-tete de `ElevatorRelaisReseau.reds`). Il vient donc CHERCHER, et cette file est le guichet.
//
// ⚠️ HORS DE LA CLASSE, comme les autres : `NetworkGameSystem` est alloue par le moteur, lui
// ajouter un membre corrompt la memoire voisine (mesure le 2026-08-06).
struct EtatAppareilRecu
{
    uint64_t device = 0;
    int32_t famille = 0;
    int32_t etat = 0;
    uint64_t proprietaire = 0;
    // Combien de joueurs tiennent cette porte ouverte (spec propagation, 2026-08-28). Non nul =
    // aucun client ne doit la laisser se refermer toute seule.
    int32_t tenants = 0;
};
extern std::deque<EtatAppareilRecu> g_appareilsRecus;

/// LA CONSOLE — une ligne que le SERVEUR a envoyee et que le client doit pouvoir AFFICHER.
///
/// ⚠️ Avant le 2026-08-30, `CommandResult` et `ConsoleLine` tombaient dans le `default` du switch
/// et n'etaient que journalises comme « message non gere ». Le serveur repondait, le client
/// jetait : tous les verdicts du palier 1 de la console ont du etre lus cote SERVEUR, et aucune
/// interface n aurait rien eu a montrer.
///
/// `niveau` : 0 info · 1 succes · 2 avertissement · 3 erreur · 4 staff. Il voyage jusqu au script
/// (encode en tete de la chaine rendue par le natif) — sans lui, la console ne peut ni colorer ni
/// prioriser, et il faudrait rouvrir ce cablage pour un seul octet.
struct LigneConsole
{
    uint8_t niveau = 0;
    std::string texte;
};
extern std::deque<LigneConsole> g_lignesConsole;
/// Compteur MONOTONE de tout ce qui est arrive, jamais decremente — meme role que
/// `g_appareilsTotalRecus` : distinguer « rien n arrive » de « tout arrive et le script n en fait
/// rien ». Deux pannes opposees, un seul ecran.
extern int32_t g_lignesConsoleTotalRecues;
/// ⚠️ La file est BORNEE. Un client qui ne depile pas — parce que la console n est pas ouverte —
/// ne doit pas faire croitre la memoire indefiniment. Au-dela, on jette les PLUS ANCIENNES et on
/// le DIT dans la ligne suivante, jamais en silence.
inline constexpr size_t kMaxLignesConsole = 200;
// Meme role que `g_ascenseursTotalRecus` : distinguer « rien n'arrive » de « tout arrive et le
// script n'en fait rien ». Deux pannes opposees, un seul ecran.
extern int32_t g_appareilsTotalRecus;
// Compteur MONOTONE de tout ce que le C++ a recu, jamais decremente. La file, elle, est
// drainee toutes les 50 ms : la lire ne dit donc RIEN sur ce qui est arrive. Ce compteur
// est le seul moyen de distinguer « rien n'arrive au client » de « tout arrive et le script
// n'en fait rien » -- deux pannes opposees qui produisent le meme ecran.
extern int32_t g_ascenseursTotalRecus;

/// Un joueur DISTANT est-il porte par cette cabine ? Lit `g_porteurParAvatar`, alimente par
/// `PlayerState.frame` a chaque snapshot. Fonction libre parce que la table est un statique de
/// fichier, et que le natif qui l'expose est une methode INLINE de l'en-tete.
bool CabinePorteQuelquun(uint64_t cabineHash);

/// La cabine qui porte cet avatar reseau, ou 0 s'il est a pied. Meme raison d'etre une fonction
/// libre que `CabinePorteQuelquun` : la table est un statique de fichier.
uint64_t CabineDeAvatarReseau(uint64_t networkId);

/// La derniere pose MONDE voulue pour un avatar attache (0,0,0 s'il n'y en a pas).
RED4ext::Vector4 PoseVoulueAttachee(uint32_t entiteHash);

/// L'allure annoncee pour un avatar attache (0 s'il n'y en a pas).
uint8_t AllureAttachee(uint32_t entiteHash);

/// ── LA HAUTEUR VIVANTE DU PLANCHER D'UNE CABINE ────────────────────────────────────────────
///
/// Publiee par redscript (tick de 50 ms du mod ascenseur), qui lit le composant `movingPlatform`.
/// C'est LE seul endroit ou cette valeur existe : l'ENTITE de l'ascenseur, elle, ne bouge pas d'un
/// millimetre pendant le trajet (F-ASC-032, mesure du 2026-08-25), seuls ses composants descendent.
void PoserHauteurCabine(uint64_t cabineHash, float z, double instant);

/// Garde une POIGNEE sur le composant qui porte le plancher, pour le lire dans la frame ou l'on
/// rend plutot que d'echantillonner sa hauteur a 20 Hz. C'est ce qui supprime la derniere source
/// de tremblement : la pente reconstituee entre deux releves, et la gigue de leur cadence.
void PoserPlancherCabine(uint64_t cabineHash, const Red::Handle<RED4ext::IScriptable>& composant);

/// Hauteur du plancher a `instant`, EXTRAPOLEE lineairement depuis les deux derniers releves.
/// Rend `false` si on n'a pas encore deux points — l'appelant doit alors garder son comportement
/// habituel plutot que d'inventer une hauteur.
bool HauteurCabineA(uint64_t cabineHash, double instant, float& sortie, bool& enMouvement);

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
/// Le joueur LOCAL etait-il en l'air au dernier envoi ?
///
/// Sert a dater le DECOLLAGE — le front montant, pas l'etat. Un `PlayerActionReport` part alors une
/// fois par saut, en canal FIABLE, au lieu de laisser l'observateur deduire le geste d'une allure
/// echantillonnee a 25 Hz.
///
/// Membre libre plutot que champ de `SuiviAvatar` : celui-ci decrit les avatars DISTANTS, un par
/// id reseau. Le joueur local n'y a pas d'entree, et lui en fabriquer une melangerait deux
/// populations dont rien d'autre ne se ressemble.
extern bool g_localEtaitEnLair;

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
    /// Derniere ENTREE de direction commandee (pilotage par entrees, ADR 0032). La garde
    /// d'anti-reemission doit porter sur le changement d'ENTREE, pas sur le deplacement de la
    /// cible : celle-ci etant calculee depuis la position courante de l'avatar, elle ne bouge
    /// presque pas tant qu'il n'avance pas — le garde se refermait sur lui-meme et l'avatar ne
    /// recevait qu'UNE commande (mesure : 4,4 m en ligne droite pour 88,8 m de marche reelle).
    /// Nombre de commandes de marche REELLEMENT emises, et resultat du dernier appel.
    /// Instrumente apres QUATRE correctifs poses sans savoir si le chemin s'executait — et
    /// re-applique SEUL apres un crash, parce qu'un instrument doit survivre au patch qu'il
    /// mesure. La premiere fois, le revert du correctif avait emporte les compteurs avec lui.
    std::uint32_t commandesEmises = 0;
    int dernierRetourCommande = -1;
    std::uint8_t derniereMoveDir = 0;
    float dernierYawEntree = 0.0f;
    /// ── HABILLAGE ────────────────────────────────────────────────────────────────────────
    /// Signature de la tenue annoncee par le serveur. Un changement la remet a zero et relance
    /// les passes : c'est ce qui fait qu'une tenue changee en cours de partie est ramassee sans
    /// qu'aucun evenement n'ait a etre cable.
    std::uint64_t signatureVetements = 0;
    /// Passes d'habillage deja tentees pour CETTE tenue. Bornees : marteler le
    /// `TransactionSystem` a chaque instantane est le regime qui a fait tomber le jeu deux fois
    /// le 2026-08-06. `kPassesHabillage` = plus aucune tentative.
    std::uint32_t passesHabillage = 0;
    /// Quand tenter la prochaine passe. ⚠️ UNE HORLOGE, PAS UN COMPTEUR D'APPELS — et ça a
    /// coûté un aller-retour en jeu. La première version comptait « 20 instantanés » en supposant
    /// que `PiloterAvatar` tournait à 20 Hz ; il tourne beaucoup plus vite, et les dix passes
    /// tenaient en 2,6 s — TOUTES dans la fenêtre où un pantin qui vient de naître accepte les
    /// ordres sans les exécuter. `ArmeAvatar.reds` attend 2 s pour cette raison exacte, mesurée
    /// le 2026-08-10 ; je l'avais lu et je l'ai quand même reperdu en changeant d'unité.
    std::chrono::steady_clock::time_point prochainePasseHabillage{};

    /// ── OU ON A LAISSE L'AVATAR A LA FRAME PRECEDENTE ────────────────────────────────────────
    ///
    /// Sert a mesurer UNE seule chose, et c'est la question ouverte de F-PLY-064 : de combien
    /// l'avatar se deplace TOUT SEUL entre deux frames, apres qu'on l'a place.
    ///
    /// Le raisonnement : le correcteur ferme 15 % de l'ecart par frame (`kFractionCorrection`), ce
    /// qui referme 99,99 % d'un ecart STATIQUE en ~0,1 s a 60 fps. Or on mesure 9 m de derive
    /// SOUTENUE. Un correcteur qui fonctionne et un ecart qui persiste ne sont conciliables que si
    /// quelque chose eloigne l'avatar entre deux corrections — et le seul candidat qui bouge un
    /// pantin sans qu'on le lui demande frame par frame, c'est le moteur lui-meme, executant notre
    /// commande de marche continue a SA vitesse vers un point a 5 m devant.
    ///
    /// `ecartLibre` = distance(position placee a la frame N-1, position lue a la frame N). C'est du
    /// deplacement que NOUS n'avons pas ordonne. S'il est proche de zero, le correcteur n'est pas
    /// distance et il faut chercher ailleurs ; s'il est du meme ordre que la derive, le coupable
    /// est nomme sans ambiguite.
    float placeX = 0.0f;
    float placeY = 0.0f;
    float placeZ = 0.0f;
    bool placeValide = false;
    /// Secondes ecoulees depuis ce placement. Dit a quelle CADENCE le correcteur
    /// passe reellement — la variable manquante de F-PLY-065.
    float depuisPlaceS = 0.0f;

    /// Secondes ecoulees depuis le dernier replacement d'un avatar IMMOBILE.
    ///
    /// Compteur distinct de `depuisPlaceS` a dessein : la branche « immobile » de `PiloterAvatar`
    /// sort AVANT le bloc qui incremente celui-la, donc le reutiliser ne mesurerait rien. Il borne
    /// la cadence de `SetEntityPosition` sur cette branche — le seul chemin de placement dont on
    /// ait la preuve qu'il applique (F-PLY-085), mais aussi celui qui avait fait tomber le jeu en
    /// etant appele a chaque frame (2026-08-06).
    float depuisPlacementImmobileS = 0.0f;

    /// ⭐ LA CIBLE PRECEDENTE, pour savoir si elle BOUGE — et pas si l'avatar bouge.
    ///
    /// Un passager d'ascenseur est « immobile » au sens de la locomotion (il ne marche pas), mais
    /// sa position monde DEFILE avec la cabine. La branche immobile le corrigeait donc toutes les
    /// deux secondes, au-dela de 5 cm, par un appel inerte — autant dire jamais. Pendant ce temps
    /// la cabine descendait autour de lui : « le personnage monte au plafond » (Lucas, 2026-08-27).
    ///
    /// Comparer la cible d'une frame a l'autre distingue les deux cas sans rien deviner : une cible
    /// figee = un corps reellement immobile (cadence lente, c'est le cas de loin le plus frequent) ;
    /// une cible qui defile = un corps porte, ou tire par autre chose que ses jambes.
    RED4ext::Vector4 cibleImmobilePrecedente{};
    bool cibleImmobileConnue = false;

    /// ⭐ VITESSE VERTICALE LISSEE de la cible, en m/s — et le lissage n'est pas du confort.
    ///
    /// L'extrapolation multiplie la vitesse par le delai du tampon. Une vitesse estimee sur UNE
    /// image est donc aussi bruitee que la duree de cette image : a deux instances sur la meme
    /// machine, une image longue fait bondir l'estimation, et l'extrapolation avec elle.
    ///
    /// Mesure du 2026-08-27 : l'ecart oscillait a ±5 cm, avec DEUX pics isoles a 0,69 m — tous deux
    /// sur une image ou l'ecart de cible par frame passait de 0,09 a 0,26 m. Le lissage supprime
    /// ces pics sans toucher au regime etabli : une plateforme va a vitesse quasi constante, donc
    /// lisser ne coute rien en justesse et gagne tout en stabilite.
    float vitesseZLissee = 0.0f;
    bool vitesseZConnue = false;

    /// Derniere posture POUSSEE a cet avatar : 0 debout, 1 accroupi.
    ///
    /// ⚠️ INITIALISE A 0, PAS A -1, et ce n'est pas un detail de style. Un pantin naît DEBOUT :
    /// initialiser a -1 (« on ne lui a rien pousse ») provoquait une ecriture de graphe
    /// d'animation dans la frame meme de sa naissance, avant que son apparence ne soit posee.
    /// Elle n'apprenait rien au moteur — il etait deja debout — et coûtait une ecriture par
    /// avatar au pire moment de sa vie.
    ///
    /// Un avatar qui apparaît DEJA accroupi recoit quand meme sa pousse : son etat voulu vaut 1,
    /// donc different de 0. Le tri-etat ne servait a rien.
    std::int8_t dernierePostureAccroupie = 0;
    /// Derniere POSE TENUE (assis, adosse) vue pour cet avatar. Sentinelle a 0xFFFFFFFF et non
    /// a 0 : `0` est une valeur LEGITIME (« aucune posture »), donc l'initialiser a 0 ferait
    /// taire la premiere transition d'un avatar qui naît deja assis. Meme piege que le -1 de
    /// l'accroupissement, et meme famille que le `Bool` non initialise de `UiKitPosture.reds`.
    std::uint32_t derniereSustained = 0xFFFFFFFFu;

    /// L'avatar etait-il EN VOL au dernier passage ? Sert a ne pousser l'animation de
    /// franchissement qu'aux deux transitions -- decollage et atterrissage -- et jamais entre les
    /// deux. Un bool suffit ici, contrairement a la posture : un pantin naît AU SOL, donc l'etat
    /// initial `false` est le bon et ne provoque aucune ecriture dans la frame de naissance
    /// (F-PLY-119, qui rendait les corps invisibles).
    bool dernierEnVol = false;

    /// L'arme de cet avatar etait-elle DEGAINEE au dernier passage ? Sert a ne pousser la couche
    /// `WeaponRight` qu'aux transitions. `false` initial sans risque : un pantin naît les mains
    /// vides, donc rien n'est ecrit dans la frame de sa naissance (F-PLY-119).
    bool derniereArmeDegainee = false;

    /// Derniere valeur lue de `TesseraLireLocomotion` (`action * 10 + exploration`), pour
    /// n'ecrire au journal que les CHANGEMENTS d'etat de la machine de deplacement.
    ///
    /// -2 et non -1 comme sentinelle : -1 est une valeur LEGITIME (composant injoignable), et la
    /// confondre avec « jamais lu » ferait taire le tout premier releve -- c'est-a-dire celui qui
    /// dit qu'un avatar naît sans composant de mouvement, exactement le genre de fait qu'on
    /// cherche.
    std::int32_t derniereLocoMoteur = -2;

    /// Derniere hauteur de tete relevee, en centimetres au-dessus de la racine de l'entite.
    /// -9999 = jamais relevee, pour que le tout premier releve passe toujours le seuil.
    std::int32_t derniereHauteurTeteCm = -9999;
};
extern std::map<uint64_t, SuiviAvatar> g_suiviAvatars;

/// SUSPEND la reemission de la commande de marche sur les avatars distants.
///
/// ⚠️ EXISTE POUR RENDRE LES MESURES INTERPRETABLES, pas pour la production. Trois tests du
/// 2026-08-16 ont ete invalides par le meme confondeur : on mesure le placement pendant que notre
/// propre boucle de rendu reemet la commande de marche toutes les ~25 ms. Annuler la commande
/// depuis Lua ne sert a rien — elle revient avant que le placement n'arrive.
///
/// Bascule par la sonde `suspendre` du harnais. Par defaut faux : aucun effet en jeu normal.
extern bool g_suspendreCommandes;

/// SUSPEND les corrections de position, en LAISSANT tourner la commande de marche.
///
/// C'est l'instrument de la mesure de determinisme exigee par l'ADR 0032. La question a trancher :
/// deux clients nourris des memes ordres de marche produisent-ils des positions comparables ? Tant
/// que les corrections tournent, elles masquent la reponse — meme mortes (F-PLY-080), elles
/// dictent le point VISE et donc la commande suivante.
///
/// ⚠️ Ne pas confondre avec `g_suspendreCommandes`, qui fait l'INVERSE : il coupe la marche et
/// laisse les corrections. Les deux existent parce qu'ils repondent a deux questions opposees, et
/// les melanger produirait un avatar immobile dans les deux cas — donc un resultat ininterpretable.
extern bool g_suspendreCorrections;

/// PILOTAGE PAR LES ENTREES (ADR 0032, prototype minimal).
///
/// Quand ce drapeau est pose, la destination de la commande de marche n'est plus derivee de la
/// position INTERPOLEE recue du serveur, mais du couple (`yaw`, `move_dir`) — c'est-a-dire des
/// ENTREES du joueur distant, relativement a la position ou l'avatar se trouve DEJA.
///
/// C'est le prototype que l'ADR 0032 reclame, et il est bon marche parce que le canal existe deja :
/// `locomotion` et `move_dir` voyagent depuis le gel du palier 2 et n'etaient presque pas lus.
///
/// ⚠️ A COMBINER AVEC `g_suspendreCorrections`. Sinon la correction ramene l'avatar sur la position
/// autoritaire et masque tout : on mesurerait le correcteur, comme trois fois le 2026-08-16.
///
/// LA MESURE TOMBE ALORS TOUTE SEULE : l'ecart entre l'avatar pilote par les entrees et la position
/// autoritaire (que le serveur continue d'envoyer sans qu'on l'applique) EST le determinisme. Un
/// seul observateur suffit — le protocole a trois instances etait inutilement lourd, et il produisait
/// un zero parfait parce que les deux observateurs lisaient le meme fil.
extern bool g_pilotageParEntrees;

/// Pose voulue par le serveur pour une entite qui n'etait PAS ENCORE RESOLVABLE quand elle est
/// arrivee — a rejouer des que `GetDynamicEntity` repond enfin.
///
/// ── CREUX B0 : mesure du 2026-08-05, reproduit sur les vehicules le 2026-08-14 ───────────────
///
/// `CreateEntity` rend un id IMMEDIATEMENT mais instancie l'entite EN DIFFERE. Le client interroge
/// `GetDynamicEntity` quelques dizaines de millisecondes plus tard et tombe dans le trou : 65
/// spawns pour 75 « Failed to get the entity » a la premiere mesure.
///
/// Jusqu'ici la position etait alors JETEE — un `Warn`, rien d'autre. L'entite restait donc
/// exactement la ou elle etait nee, pour toujours. C'est la cause des PNJ figes du 2026-08-05, et
/// celle des vehicules serveur visibles chez un client et pas chez l'autre le 2026-08-14 : les
/// trois ids de vehicule figuraient dans les echecs de resolution des DEUX clients, mais en
/// nombre tres inegal (10 contre 36).
///
/// On retient la derniere pose voulue au lieu de la perdre, et elle se rejoue des que l'entite
/// existe. Aucune fenetre a deviner, aucun delai a calibrer — ce qui compte, puisque le delai reel
/// d'instanciation n'a JAMAIS ete mesure (la sonde reste ouverte au backlog).
///
/// ⚠️ HORS DE LA CLASSE, comme ses voisines : `NetworkGameSystem` est allouee par le moteur
/// (`RTTI_IMPL_ALLOCATOR`) et lui ajouter un membre corrompt la memoire voisine (mesure le
/// 2026-08-06). J'allais poser un `std::map` membre par reflexe.
struct PoseEnAttente
{
    RED4ext::Vector4 position;
    float yaw;
};
extern std::map<uint64_t, PoseEnAttente> g_posesEnAttente;

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
    // ── LE SEXE DU CORPS, et ce qu'il commande VRAIMENT ───────────────────────────────────────
    //
    // ⚠️ IL NE CHOISIT PAS LE CORPS. Le sexe du corps suit la CHARGE d'esthetique, jamais le record
    // (F-PLY-267) — un V feminin s'applique tres bien a l'entite masculine, et c'est mesure.
    //
    // Il choisit les MESHES DE VETEMENTS. Ceux-ci sont des composants de l'entite (F-PLY-286), donc
    // figes a sa construction (F-PLY-191) : il faut donc connaitre le sexe AVANT le spawn, et
    // basculer sur une entite derivee differente. Sans lui, une joueuse porte un t-shirt `_ma_` sur
    // un corps de femme — defaut rapporte par Lucas le 2026-08-24.
    //
    // ⚠️ DEFAUT `true`. Le champ est arrive en fin de table le 2026-08-24 ; un serveur qui ne
    // l'emet pas, ou un personnage anterieur a la capture d'esthetique, donnent « masculin » — le
    // comportement d'avant, a l'identique.
    bool corpsMasculin = true;
    // ── L'ESTHETIQUE DE V, transportee telle quelle ───────────────────────────────────────────
    //
    // Blob `TSV1` opaque : magic, trois compteurs de section, puis des paires (uiSlot, name). 440
    // octets pour un V complet (27 paires). Le serveur ne l'interprete PAS — il verifie sa forme et
    // le relaie (ADR 0036) ; le client non plus ne le lit pas champ par champ, il le rend au natif
    // qui sait l'appliquer.
    //
    // ⚠️ VIDE EST LEGITIME, et ce n'est pas la meme chose que `arme = 0`. Tant que le createur de
    // personnage n'existe pas, aucun joueur n'en a : l'avatar retombe alors sur son record de repli,
    // un passant generique. Traiter le vide comme une erreur ferait echouer le spawn de tout le
    // monde aujourd'hui.
    std::vector<uint8_t> esthetique;
    // ── CE QUE LE PERSONNAGE PORTE, decide par le SERVEUR ─────────────────────────────────────
    //
    // Hashes TweakDBID de vetements, DANS L'ORDRE DE POSE. Transportes par les `garments` dont le
    // drapeau `drawn` est faux — le vrai designe l'arme en main, qui suit une tout autre recette
    // (F-PLY-203 : l'habillage se branche sur `gamedataEquipmentArea`).
    //
    // L'autorite est la base de donnees du serveur (colonne `contenus.porte`), jamais le client et
    // plus `dotation.toml` : un joueur qui se change doit rester change apres reconnexion.
    //
    // ⚠️ VIDE = NE PORTE RIEN, un etat a part entiere. Aujourd'hui c'est le cas de tout personnage
    // cree avant la migration 0012 : ceux-la sont nus, et c'est exact.
    std::vector<uint64_t> vetements;
};

class NetworkGameSystem : public Red::IGameSystem
{
private:
    /// ⚠️ INITIALISÉ, et ce n'est pas cosmétique : c'est LUI qui garde `ConnectToServer` depuis
    /// l'ajout de la reconnexion. Laissé indéterminé, le premier appel pouvait être refusé au
    /// hasard de la pile.
    HSteamNetConnection m_hConnection = k_HSteamNetConnection_Invalid;
    ISteamNetworkingSockets *m_pInterface;
    /// Adresse du serveur joint, `host:port`. Mémorisée au seul usage des journaux de playtest :
    /// un fichier ramassé chez un joueur doit dire CONTRE QUOI il a été produit, sinon on ne peut
    /// pas l'apparier au journal serveur correspondant.
    std::string m_serverAddress;
    bool m_hasTriedToConnect = false;

    // --- RECONNEXION AUTOMATIQUE (2026-08-16) -------------------------------------------------
    //
    // Avant : la connexion était à USAGE UNIQUE. `m_hasTriedToConnect` interdisait un second essai,
    // et un garde `m_pInterface != nullptr` aurait de toute façon refusé — `SteamNetworkingSockets()`
    // est un singleton de processus, il ne redevient jamais nul. Perdre le Gateway mettait donc fin
    // à la session, sans recours autre que relancer le jeu.
    //
    // Host et port sont mémorisés SÉPARÉMENT de `m_serverAddress` : celle-ci est une chaîne
    // d'affichage pour les journaux, la reparser pour reconnecter serait la détourner de son usage.
    std::string m_host;
    uint16_t m_port = 0;
    /// Secondes restantes avant le prochain essai. <= 0 = aucun essai armé.
    double m_reconnexionDansS = 0.0;
    /// Essais consécutifs depuis la dernière connexion réussie. Remis à zéro par une connexion.
    int32_t m_tentativesReconnexion = 0;
    /// Dernier personnage incarné, à rejouer après une reconnexion — sans lui le joueur revient
    /// connecté mais SPECTATEUR (le serveur ignore tout d'un client resté en `AwaitingSelection`,
    /// F-PLF-024), et le symptôme est un monde vide qu'on prend pour une panne réseau.
    uint64_t m_personnageIncarne = 0;
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
        // L'origine telle que le SERVEUR la connait : "corpo" | "gosse_des_rues" | "nomade".
        // Vide pour un personnage cree avant que le champ n'existe — le lobby n'affiche alors
        // rien plutot qu'une valeur inventee.
        std::string origine;
        // Le CORPS et le CERVEAU choisis au createur : deux genres INDEPENDANTS chez CDPR — le
        // corps decide du pantin monte, le cerveau decide de la voix.
        bool corpsMasculin = false;
        bool cerveauMasculin = false;
        // Le blob `TSV1` du personnage, tel que capture a sa creation. VIDE pour un personnage
        // anterieur a la capture — le client retombe alors sur le pantin de base.
        std::vector<uint8_t> esthetique;
        // ⭐ La RECETTE d'esthetique, deja reserialisee en « nom:index;nom:index ». C'est elle que
        // le client rejoue sur le V local en entrant en jeu (F-PLY-246/248) ; le blob ci-dessus
        // habille les avatars DISTANTS. Vide pour un personnage cree avant la capture.
        std::string recette;
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
    bool m_vidageEtatFait = false;

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

    /// UNE COMMANDE DE LA CONSOLE, telle que la console a besoin de l'afficher.
    ///
    /// `nom` sert a COMPLETER (on tape `/veh`, on obtient `/vehicle`), `affichage` sert a MONTRER
    /// (`/vehicle <action> <id> [record] — give a car to a character`). Deux champs et pas un,
    /// parce que completer avec la ligne d'affichage ecrirait l'aide dans le champ de saisie.
    ///
    /// La forme `<requis>` / `[optionnel]` est construite ICI, une fois, plutot que cote script :
    /// le script recevrait sinon la liste d'arguments a plat et devrait la reformater a chaque
    /// frappe, pour un resultat identique.
    struct CommandeRecue
    {
        std::string nom;
        std::string affichage;
    };
    /// Ce que CE joueur a le droit de TAPER. Filtre par le serveur avant l'envoi, comme
    /// `m_actions` : le client n'apprend jamais l'existence des commandes qu'il n'a pas, donc une
    /// suggestion ne peut pas reveler l'outillage du staff a un joueur ordinaire.
    std::vector<CommandeRecue> m_commandes;
    /// Les noms qu'on CONNAIT, par id reseau. C'est la seule source du nametag.
    ///
    /// ⚠️ Une entree n'apparait ici que parce que le serveur l'a envoyee, et il ne l'envoie qu'a
    /// qui s'est fait presenter. Un nom inconnu n'est pas masque a l'affichage : il n'a JAMAIS
    /// traverse le fil. La discretion est une propriete du serveur — un binaire modifie ne peut pas
    /// reveler ce qu'il n'a jamais recu.
    std::map<uint64_t, std::string> m_nomsConnus;

    // --- Inventaire sous autorite serveur (ADR 0026 et 0027) ---
    /// Le sac AUTORITAIRE, tel que le serveur l'a enonce. Le client s'y ALIGNE ; il ne decide
    /// jamais de ce qu'il possede.
    struct ItemAutoritaire
    {
        std::string id;
        uint32_t quantite = 0;
    };
    std::vector<ItemAutoritaire> m_sacAutoritaire;
    /// Ce qu'il ne faut JAMAIS retirer, envoye par le serveur avec le sac.
    ///
    /// ⚠️ Le sac du joueur contient des pieces de son CORPS — tete, bras, poings nus, connecteur
    /// d'interaction (sept entrees mesurees, F-MND-047). Le serveur ne les connait pas et ne peut
    /// donc pas les enoncer dans le sac. Sans cette liste, un client qui s'aligne retirerait la
    /// tete de son personnage, SANS lever d'erreur.
    std::vector<std::string> m_aPreserver;
    /// Un sac a-t-il ete recu ? Distinct de « le sac est vide » : un sac vide est un ORDRE (« tu ne
    /// possedes rien »), l'absence de message n'en est pas un. Les confondre ferait vider les
    /// joueurs d'un serveur qui n'a jamais parle d'inventaire.
    bool m_sacRecu = false;
    /// Combien de sacs autoritaires ont ete appliques. Voir `Tessera_SacSeq`.
    int32_t m_sacSeq = 0;

    // --- LE COFFRE D'UN VEHICULE, meme doctrine que le sac ---
    //
    // Le serveur enonce l'etat complet du coffre a l'ouverture ; le client s'y aligne. C'est
    // volontairement la MEME structure que le sac : « voici le contenu du conteneur X » est un
    // seul concept, et le sac du joueur n'est que le conteneur par defaut.
    std::vector<ItemAutoritaire> m_coffreAutoritaire;
    /// L'id RESEAU du vehicule dont on tient le coffre. 0 = aucun coffre en cours.
    uint64_t m_coffreVehicule = 0;

    /// Cette session de stockage est-elle celle d'un CONTENANT DU MONDE (caisse, casier, planque)
    /// plutot que d'un coffre de vehicule ?
    ///
    /// ⭐ TOUT LE RESTE EST PARTAGE, ET C'EST DELIBERE. Le contenu autoritaire, le porteur, l'ecran
    /// natif, la lecture a la fermeture : un contenant du monde et un coffre de voiture sont le
    /// MEME ecran sur le meme mecanisme. Seules deux choses different — le `ui_kind` qui arrive, et
    /// le verbe de choix qui repart. Dupliquer les neuf natifs du coffre pour ca aurait garanti que
    /// les deux copies divergent au premier correctif.
    ///
    /// ⚠️ Cote redscript, RIEN ne change : `Tessera_CoffreVehicule()` porte ici l'`EntityID` de la
    /// caisse, et la veille ne s'en sert que comme temoin de session ouverte.
    bool m_coffreEstContenant = false;
    /// L'ordre d'invocation courant — voir `HandleInteractionOpen` et F-VEH-054.
    uint64_t m_invocationVehicule = 0;
    std::string m_invocationRecord;
    int32_t m_invocationSeq = 0;
    /// La session d'interaction ouverte par le serveur — a renvoyer telle quelle a la fermeture.
    uint64_t m_coffreSession = 0;
    uint16_t m_coffreCapacite = 0;
    /// ⚠️ UN NUMERO DE SEQUENCE, PAS UN BOOLEEN « recu ». Le sac, lui, se contente d'un drapeau
    /// parce qu'il n'arrive qu'une fois par etat. Un coffre s'ouvre PLUSIEURS fois, et deux
    /// ouvertures successives du MEME coffre avec le MEME contenu sont indiscernables par un
    /// booleen — redscript croirait n'avoir rien recu la seconde fois. Le compteur, lui, change
    /// toujours.
    int32_t m_coffreSeq = 0;
    /// Le rapport en cours de construction (fermeture du coffre). Vide entre deux rapports.
    std::vector<ItemAutoritaire> m_coffreRapport;

    // --- LA CASSE VUE PAR LES TEMOINS ---
    //
    // Le serveur diffuse `VehicleState.degats` a tout le monde. Chez un temoin, le moteur n'a
    // AUCUN moyen de le savoir : il ne simule pas cette voiture, donc il ne lui calcule pas de
    // degats (F-VEH-041). On lui donne le nombre, et il deroule le reste tout seul.
    //
    // ⚠️ UNE FILE, PAS UNE TABLE INTERROGEABLE. Redscript ne peut pas balayer tous les vehicules
    // proches a chaque battement pour voir lesquels ont change — ce serait un cout par frame pour
    // un evenement rare. On ne pousse que les CHANGEMENTS, et il draine.
    std::map<uint64_t, uint8_t> m_degatsConnus;
    std::deque<std::pair<uint64_t, uint8_t>> m_degatsAAppliquer;

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
    /// Arme un essai de reconnexion, avec un recul qui croît (1, 2, 4, 8… plafonné). Le recul n'est
    /// pas de la politesse : un serveur qui redémarre met quelques secondes, et le marteler pendant
    /// ce temps ne le fait pas revenir plus vite — ça ne fait que remplir ses journaux.
    void ArmerReconnexion();
    /// Rejoue `ConnectToServer` sur la dernière adresse connue.
    void TenterReconnexion();
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

    /// Rejoue les poses retenues quand `GetDynamicEntity` avait echoue (creux B0). Appelee une
    /// fois par snapshot ; ne coute rien quand le tampon est vide, ce qui est le regime etabli.
    void RejouerPosesEnAttente();
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
    /// RTT courant de la connexion, en millisecondes ; -1 si inconnu (pas encore connecté).
    ///
    /// ⚠️ **Ce chiffre ne CORRIGE aucune horloge, et c'est important de savoir pourquoi.** Un RTT
    /// donne la LARGEUR de l'incertitude, jamais le SENS du décalage : deux horloges désynchronisées
    /// d'une heure produisent exactement le même ping que deux horloges parfaites. La correction
    /// d'horloge vient de `Snapshot.ts_ms` et du min-filtre de `HorlogeServeur`.
    ///
    /// Il sert à SÉPARER DEUX CAUSES qui donnent le même symptôme dans un journal de playtest :
    /// « ce joueur a une mauvaise connexion » et « ce joueur a une horloge fausse ». Sans lui, les
    /// deux se lisent pareil — un `ts` qui ne colle pas avec les autres.
    int PingCourantMs() const;
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
    void HandleCommandCatalog(const cyberpunk_rp::protocol::CommandCatalog* msg);
    // Noms que ce joueur CONNAIT. En lot au join, a une entree a chaque presentation recue. On
    // ACCUMULE ici (contrairement au catalogue) : le message a une entree est un ajout, pas un
    // remplacement, et le traiter comme tel effacerait toutes les connaissances a chaque poignee
    // de main.
    void HandleIdentitesConnues(const cyberpunk_rp::protocol::IdentitesConnues* msg);
    // Declenche une recette du catalogue sur une cible. `kind = 2` (Interagit), `param` = l'id de
    // la recette : le canal montant existe depuis le gel, zero octet ajoute au fil.
    void SendActionJoueur(uint64_t target, uint32_t recette);
    void SendVehiculeVerbe(uint64_t target, uint8_t verbe, uint32_t param);
    void SendCoffreRapport();
    void HandleInteractionOpen(const cyberpunk_rp::protocol::InteractionOpen* msg);
    // ── ASCENSEURS (ADR 0012) ────────────────────────────────────────────────────────────
    // Rapporte au serveur qu'un joueur a demande un etage. Le serveur ARBITRE (file SCAN) et
    // renvoie un `ElevatorStateMsg` a tout le monde ; c'est ce message-la qui fait partir la
    // cabine, jamais l'appui local — la boucle locale est coupee cote redscript.
    void SendElevatorCall(uint64_t elevatorId, int32_t floor);
    void SendDeviceCall(uint64_t device, uint8_t famille, uint8_t action, uint8_t etatObserve);
    void SendAdminCommand(const char* texte);
    // Signale l'entree (mount=true) ou la sortie d'une cabine. `kind=6/7` d'EntityInteraction.
    // Sert au RENDU chez les autres : le serveur relaie le porteur dans `PlayerState.frame`, et
    // l'observateur accroche l'interpolation de l'avatar a la cabine (ADR 0039).
    void SendElevatorMount(uint64_t elevatorId, bool mount);
    bool EnvoyerPosture(uint64_t emplacementId, uint32_t code);
    // Etat autoritaire d'une cabine -> redscript, qui rejoue l'ordre ou recale l'etage.
    void HandleElevatorState(const cyberpunk_rp::protocol::ElevatorStateMsg* msg);
    void HandleDeviceState(const cyberpunk_rp::protocol::DeviceStateMsg* msg);
    // Le sac autoritaire. REMPLACE integralement l'etat precedent (contrairement aux identites, qui
    // s'accumulent) : c'est un ETAT, pas un ajout. Le rejouer ne fait donc rien de plus, et un
    // message perdu se rattrape au suivant.
    void HandleInventaireAutoritaire(const cyberpunk_rp::protocol::InventaireAutoritaire* msg);

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

    /// Rapporte au serveur qu'un joueur monte (`monte=true`) ou descend d'un vehicule.
    ///
    /// `EntityInteraction{target, kind=3|4, param=<index de siege>}` — canal GELE depuis juillet,
    /// que **personne n'emettait**. C'est ce qui manquait pour que le serveur sache qui est assis
    /// ou : il comprenait ces deux verbes depuis toujours, aucun client ne les a jamais envoyes.
    /// Zero octet de protocole ajoute.
    void RapporterMontage(uint64_t vehiculeReseau, uint32_t siege, bool monte);

    /// Voir la definition — rapporte la position choisie par le jeu, et FERME la session.
    bool SendRapportInvocation(float x, float y, float z);

    /// Id RESEAU d'une entite du jeu, ou 0 si elle n'en a pas (objet purement local).
    ///
    /// Balayage lineaire de `m_networkedEntitiesLookup` — assume : le roster reseau est petit, et
    /// on n'appelle ceci qu'au montage/demontage, pas par frame. Un index inverse serait une
    /// deuxieme table a garder synchronisee pour un gain nul a cette cadence.
    uint64_t IdReseauDe(RED4ext::ent::EntityID entityId) const;

    template <typename T>
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

    /// Quel personnage le mode dev doit prendre — `--tessera-dev=2` rend 2, en BASE 1.
    /// `0` = rien de demandé, le script tire alors au sort (voir `PersonnageDemande`).
    std::int32_t Tessera_PersonnageDemande() const
    {
        return static_cast<std::int32_t>(PersonnageDemande(GetCommandLineA()));
    }
    Red::CString Tessera_NomPersonnage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_personnages[static_cast<size_t>(index)].pseudonyme.c_str());
    }
    // L'origine du personnage a cet index — chaine VIDE si l'index est hors bornes ou si le
    // serveur ne l'a pas renseignee. Le client ne la traduit pas : "corpo" reste "corpo", et
    // c'est l'UI qui decide comment l'ecrire pour un joueur.
    Red::CString Tessera_OriginePersonnage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_personnages[static_cast<size_t>(index)].origine.c_str());
    }
    /// L'esthetique du personnage a cet index, en HEXADECIMAL — meme forme que ce que
    /// `Tessera_LireEsthetique` produit, pour que les deux bouts parlent la meme langue.
    /// ⚠️ Chaine VIDE si l'index est hors bornes, si le serveur n'a rien, ou si ce qu'il a n'est
    /// PAS un `TSV1` : `characters.appearance` est un champ a double usage (16 octets = l'ancien
    /// couple record/apparence). On reconnait le blob a sa magie, jamais a sa longueur seule.
    Red::CString Tessera_EsthetiquePersonnage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size())
        {
            return Red::CString("");
        }
        const auto& blob = m_personnages[static_cast<size_t>(index)].esthetique;
        if (blob.size() < 4 || blob[0] != 'T' || blob[1] != 'S' || blob[2] != 'V' || blob[3] != '1')
        {
            return Red::CString("");
        }
        static const char* kHex = "0123456789abcdef";
        std::string hex;
        hex.reserve(blob.size() * 2);
        for (uint8_t o : blob)
        {
            hex.push_back(kHex[o >> 4]);
            hex.push_back(kHex[o & 0x0F]);
        }
        return Red::CString(hex.c_str());
    }
    /// ⭐ La RECETTE d'esthetique du personnage a cet index — « nom:index;nom:index », prete a
    /// etre rejouee par `ApplyChangeToOption` + `ReFinalizeState` (F-PLY-246).
    /// Chaine VIDE si l'index est hors bornes ou si le personnage est anterieur a la capture : le
    /// client n'applique alors rien, et V garde l'apparence de la souche.
    Red::CString Tessera_RecettePersonnage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_personnages[static_cast<size_t>(index)].recette.c_str());
    }
    /// ⭐⭐ La recette du personnage QU'ON INCARNE — sans avoir a connaitre son index.
    ///
    /// C'est le point d'entree de l'hydratation a l'arrivee en jeu. Passer par l'index aurait
    /// oblige a transporter un nombre a travers le chargement du monde, qui detruit les
    /// controleurs de menu ; `m_personnageIncarne` est deja retenu ici pour la reprise apres
    /// reconnexion, et il survit — c'est donc lui la source, pas un etat cote script.
    ///
    /// Chaine VIDE si aucun personnage n'est incarne, ou s'il est anterieur a la capture.
    Red::CString Tessera_RecetteIncarnee() const
    {
        if (m_personnageIncarne == 0) return Red::CString("");
        for (const auto& p : m_personnages)
        {
            if (p.id == m_personnageIncarne) return Red::CString(p.recette.c_str());
        }
        return Red::CString("");
    }
    // Le corps du personnage a cet index. ⚠️ `false` couvre DEUX cas que FlatBuffers ne distingue
    // pas : un corps feminin choisi, et un personnage cree avant que le champ n'existe. La base
    // garde la nuance (`NULL`), le fil ne peut pas.
    bool Tessera_CorpsMasculin(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size()) { return false; }
        return m_personnages[static_cast<size_t>(index)].corpsMasculin;
    }
    bool Tessera_CerveauMasculin(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_personnages.size()) { return false; }
        return m_personnages[static_cast<size_t>(index)].cerveauMasculin;
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
    /// ⚠️ `esthetiqueHex` : le blob `TSV1` en hexadecimal, ou une chaine VIDE.
    /// Le fork TRANSPORTE l'esthetique, il ne la CAPTURE pas (ADR 0036) : c'est a l'appelant
    /// redscript de la fournir — le createur de personnage demain, une sonde aujourd'hui.
    /// Vide est le cas NORMAL tant que le createur n'existe pas.
    /// ⚠️ `corpsMasculin` et `cerveauMasculin` sont deux genres INDEPENDANTS (conception CDPR) :
    /// le corps decide du pantin monte chez les autres joueurs, le cerveau decide de la voix.
    /// Aucun des deux n'est deductible du blob d'esthetique — son en-tete ne porte que des
    /// compteurs de paires, et les paires decrivent des details, pas le corps qui les porte.
    /// ⭐ `optionsApparence` (2026-08-22) : l'esthetique TRANSPARENTE, « nom:index;nom:index ».
    /// Le blob hexadecimal ci-dessus RESTE et garde son emploi — habiller les avatars DISTANTS.
    /// Celle-ci est la RECETTE du V du joueur : la seule forme que le moteur de customisation sait
    /// rejouer (F-PLY-246), et la seule qu'une edition future pourra lire et modifier. Decision de
    /// Lucas, 2026-08-22 : « il faut qu'on puisse savoir qu'il a voulu tels yeux, telle coiffure ».
    /// ⚠️ Une chaine, et non un tableau : c'est ce qui traverse commodement la frontiere
    /// redscript -> C++, et ca reste lisible a l'oeil dans un journal — ce qui est tout l'interet.
    bool Tessera_CreerPersonnage(const Red::CString& pseudonyme, uint64_t record, uint64_t apparence,
                                 const Red::CString& origine, const Red::CString& esthetiqueHex,
                                 bool corpsMasculin, bool cerveauMasculin,
                                 const Red::CString& optionsApparence);
    // ── LA CAPTURE : lire l'esthetique du V LOCAL, pour la proposer au serveur ───────────────
    //
    // Rend le blob `TSV1` en hexadecimal, a passer tel quel en dernier argument de
    // `Tessera_CreerPersonnage`. C'est la moitie EMISSION de la boucle d'apparence, et elle vit
    // ici plutot que dans la sonde parce que le createur de personnage tourne dans le client
    // LIVRE, pas dans `tools/re-probe`.
    //
    // `aEtat` : le `gameuiCharacterCustomizationState`, obtenu cote redscript par
    // `GameInstance.GetCharacterCustomizationSystem().GetState()`. Le type est verifie.
    //
    // ⚠️ RETOUR VIDE = REFUS, et c'est un cas NORMAL, pas une panne : l'etat est parfois non
    // finalise (F-PLY-160). L'appelant doit traiter le vide comme « pas encore », journaliser, et
    // surtout NE PAS envoyer de descripteur vide — ca donnerait un avatar sans visage chez les
    // autres joueurs, avec un symptome tres loin de sa cause. La raison exacte part au journal.
    Red::CString Tessera_LireEsthetique(const Red::Handle<RED4ext::IScriptable>& aEtat);
    // Entre dans le monde avec ce personnage. Meme remarque : `true` = « parti », pas « accepte ».
    /// ⭐ Depart VOLONTAIRE : le serveur libere la place immediatement, au lieu de la reserver
    /// quelques minutes comme apres une coupure subie. Rien ne l'envoyait avant le 2026-08-22 —
    /// tout depart passait donc pour une coupure, et bloquait un slot pour rien.
    bool Tessera_ChoisirPersonnage(uint64_t id);
    /// ⭐ Depart VOLONTAIRE : le serveur libere la place IMMEDIATEMENT, au lieu de la reserver
    /// quelques minutes comme apres une coupure subie. Rien ne l'envoyait avant le 2026-08-22 —
    /// tout depart passait donc pour une coupure, et bloquait un slot pour rien.
    /// ⚠️ Vide la file d'envoi : le processus se ferme dans la foulee.
    bool Tessera_QuitterServeur();
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

    // Ce que le serveur annonce pour une entite de JEU. `nullptr` = entite inconnue de nos tables.
    //
    // ⚠️ Balayage lineaire de `m_networkedEntitiesLookup` : la table est indexee par id RESEAU et
    // la question arrive avec un id d'ENTITE. Le cout est celui du voisinage visible (quelques
    // dizaines), et ces appels partent une poignee de fois par avatar, pas par frame.
    const NetworkAppearance* ApparencePourEntite(RED4ext::ent::EntityID cible) const
    {
        for (const auto& paire : m_networkedEntitiesLookup)
        {
            if (paire.second == cible)
            {
                const auto it = m_appearances.find(paire.first);
                return it == m_appearances.end() ? nullptr : &it->second;
            }
        }
        return nullptr;
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
    // Rend `true` UNE SEULE FOIS par session — le verrou d'une sonde qui ne doit pas se repeter.
    //
    // ⚠️ Le verrou vit ICI et pas cote script parce que la fonction appelante est rappelee pour
    // CHAQUE avatar : sans lui, une sonde destructrice s'executerait vingt fois, et sa mesure
    // n'aurait aucun sens.
    bool Tessera_PremierVidageEtat()
    {
        if (m_vidageEtatFait) { return false; }
        m_vidageEtatFait = true;
        return true;
    }

    RED4ext::TweakDBID Tessera_ArmeDeLEntite(RED4ext::ent::EntityID cible) const
    {
        const auto* a = ApparencePourEntite(cible);
        return RED4ext::TweakDBID(a == nullptr ? 0 : a->arme);
    }

    // Combien de vetements le serveur veut sur CETTE entite. `0` = elle ne porte rien.
    //
    // ⚠️ `0` NE DIT PAS « entite inconnue » — les deux rendent zero, et c'est assume : dans les
    // deux cas il n'y a rien a poser. Le cote redscript n'a donc aucune decision a prendre sur la
    // difference, et une valeur sentinelle de plus serait un cas de plus a oublier.
    std::int32_t Tessera_NombreDeVetements(RED4ext::ent::EntityID cible) const
    {
        const auto* a = ApparencePourEntite(cible);
        return a == nullptr ? 0 : static_cast<std::int32_t>(a->vetements.size());
    }

    // Le sexe du CORPS de cet avatar — `true` masculin. Vient du serveur
    // (`AppearanceSpec.corps_masculin`), qui le lit dans `characters.corps_masculin`.
    //
    // ⚠️ IL NE CHOISIT PAS LE CORPS. Le corps suit la charge d'esthetique, jamais le record
    // (F-PLY-267). Il choisit quelle moitie de la GARDE-ROBE allumer : chaque vetement est cuit en
    // deux exemplaires dans l'entite (`..._m` et `..._w`) parce qu'un vetement est genre a la
    // source — `t1_004_wa_tshirt__longsleeve` n'existe tout simplement pas (F-PLY-300).
    //
    // ⚠️ Defaut `true` quand l'entite est inconnue de nos tables : c'est le comportement d'avant ce
    // champ, donc aucun avatar existant ne change d'aspect a cause d'une table pas encore remplie.
    bool Tessera_AvatarCorpsMasculin(RED4ext::ent::EntityID cible)
    {
        const auto* a = ApparencePourEntite(cible);
        return a == nullptr ? true : a->corpsMasculin;
    }

    // Le n-ieme vetement, dans l'ORDRE DE POSE decide par le serveur.
    //
    // ⚠️ RENVOIE UN `TweakDBID`, PAS UN `uint64_t`, pour exactement la raison ecrite au-dessus de
    // `Tessera_ArmeDeLEntite` : redscript expose `TDBID.ToNumber` mais AUCUNE conversion inverse
    // (verifie dans `core/data/tweakDBID.script` : `Create(String)`, `IsValid`, `Prepend`,
    // `Append`, `ToNumber`, `None`, `ToStringDEBUG` — et rien d'autre). Un hash 64 bits passe cote
    // script serait un cul-de-sac.
    //
    // ⚠️ Un index hors bornes rend un `TweakDBID` INVALIDE plutot que de lire a cote. L'appelant
    // boucle sur `Tessera_NombreDeVetements`, mais les deux appels sont separes par des frames
    // pendant lesquelles un `AppearanceSync` peut avoir raccourci la liste.
    RED4ext::TweakDBID Tessera_VetementDeLEntite(RED4ext::ent::EntityID cible, std::int32_t index) const
    {
        const auto* a = ApparencePourEntite(cible);
        if (a == nullptr || index < 0 || static_cast<std::size_t>(index) >= a->vetements.size())
        {
            return RED4ext::TweakDBID(static_cast<uint64_t>(0));
        }
        return RED4ext::TweakDBID(a->vetements[static_cast<std::size_t>(index)]);
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

    // Millisecondes ecoulees depuis le dernier `Snapshot` RECU. -1 = aucun snapshot n'est encore
    // arrive (chargement, session sans serveur) — a ne jamais confondre avec zero, qui veut dire
    // « fil parfaitement frais ».
    //
    // ⚠️ POURQUOI CETTE MESURE EXISTE, ALORS QUE `FullyConnected` EST DEJA LA. Les deux repondent a
    // des questions differentes, et c'est le croisement qui identifie la panne :
    //
    //   FullyConnected=false            -> la socket GNS est tombee : le GATEWAY est parti.
    //   FullyConnected=true + silence   -> la socket tient, mais plus rien n'arrive : un SHARD est
    //                                      tombe, ou il est gele.
    //
    // Le second cas est INVISIBLE sans ce compteur : le client ne parle qu'au Gateway (les shards
    // sont derriere, en TCP interne), donc un shard qui meurt ne bouge aucun etat de connexion cote
    // client. Le Gateway cesse simplement d'emettre (`gateway.rs`, envoi conditionne a la
    // fraicheur), et le joueur continue de jouer dans un monde qui ne l'ecoute plus.
    //
    // ⚠️ ET SURTOUT : la profondeur du tampon d'interpolation NE REPOND PAS a cette question. Mesure
    // le 2026-08-15 (F-PLY-054) — un tampon plein d'echantillons perimes affiche `ech=48`,
    // `extrapolation=0 %`, un tableau de bord parfaitement sain, pendant que plus rien ne bouge a
    // l'ecran. C'est l'instant de la DERNIERE ARRIVEE qui dit si le fil est vivant, rien d'autre.
    int32_t Tessera_SilenceMs() const
    {
        if (!g_horlogeRendu.Amorcee())
        {
            return -1;
        }
        return static_cast<int32_t>(g_horlogeRendu.DepuisDernierSnapshot() * 1000.0);
    }

    // Essais de reconnexion consecutifs depuis la derniere connexion reussie. 0 = aucun en cours.
    // Lu par l'ecran d'attente pour n'annoncer une reconnexion que si elle a REELLEMENT lieu : tant
    // que ce compteur ne bouge pas, l'ecran constate la panne au lieu de promettre un retour.
    int32_t Tessera_TentativesReconnexion() const { return m_tentativesReconnexion; }

    // Rejoue la connexion tout de suite, sans attendre la fin du backoff — la demande explicite du
    // joueur passe avant la temporisation, qui n'existe que pour ne pas marteler un serveur mort.
    void Tessera_ReconnecterMaintenant()
    {
        m_reconnexionDansS = 0.0;
        TenterReconnexion();
    }

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
    /// LE CATALOGUE DE COMMANDES, cote lecture — ce qui rend les suggestions possibles.
    ///
    /// Rend `""` hors bornes plutot que de lever : le script interroge par index dans une boucle,
    /// et un catalogue qui retrecit entre deux images (changement de permission) ne doit pas faire
    /// tomber `r6/scripts` entier.
    int32_t Tessera_NombreCommandes() const { return static_cast<int32_t>(m_commandes.size()); }

    Red::CString Tessera_CommandeNom(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_commandes.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_commandes[static_cast<size_t>(index)].nom.c_str());
    }

    Red::CString Tessera_CommandeAffichage(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_commandes.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_commandes[static_cast<size_t>(index)].affichage.c_str());
    }

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

    // --- Le sac autoritaire, lu depuis redscript ---
    //
    // ⚠️ `Tessera_SacRecu` est SEPARE de la taille, et c'est essentiel : un sac VIDE est un ordre
    // (« tu ne possedes rien, vide-toi »), l'absence de message n'en est pas un. Si redscript ne
    // pouvait distinguer les deux, il viderait les joueurs de tout serveur qui n'a jamais parle
    // d'inventaire — une taille de 0 se lit alors comme « retire tout ».
    bool Tessera_SacRecu() const { return m_sacRecu; }

    /// Le numero du sac autoritaire courant. Change a CHAQUE sac recu.
    ///
    /// ⚠️ Un COMPTEUR, pas un booleen, et pour la meme raison que la sequence du coffre : deux sacs
    /// successifs au contenu identique seraient indiscernables par un drapeau. C'est ce qui permet
    /// a la veille d'attendre un sac *neuf* apres avoir ferme un coffre, plutot que de repartir sur
    /// un cache perime et de rendre au joueur ce qu'il vient de ranger.
    int32_t Tessera_SacSeq() const { return m_sacSeq; }

    int32_t Tessera_SacTaille() const { return static_cast<int32_t>(m_sacAutoritaire.size()); }

    Red::CString Tessera_SacItemId(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_sacAutoritaire.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_sacAutoritaire[static_cast<size_t>(index)].id.c_str());
    }

    int32_t Tessera_SacItemQuantite(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_sacAutoritaire.size())
        {
            return 0;
        }
        return static_cast<int32_t>(m_sacAutoritaire[static_cast<size_t>(index)].quantite);
    }

    // ── LE COFFRE : lecture (serveur -> client) ──────────────────────────────────────────────
    //
    // ⚠️ On rend le numero de sequence, jamais un booleen. Voir `m_coffreSeq` : un coffre s'ouvre
    // plusieurs fois, et un booleen ne distingue pas deux ouvertures identiques.
    /// Ce vehicule est-il connu du SERVEUR ? Vrai pour un vehicule ne d'un snapshot, faux pour la
    /// circulation native.
    ///
    /// ⚠️ C'est le predicat qui remplace `GetIsPlayerVehicle()` cote redscript — le drapeau vanilla
    /// qui decide si le coffre est propose. Sans lui, il faudrait ecrire dans `m_playerVehicle`,
    /// un champ PERSISTANT du systeme de sauvegarde : un etat qu'on pose et qu'il faut ensuite
    /// penser a retirer. Repondre a une question coute moins cher que modifier un etat.
    // ── L'INVOCATION : « fais naitre ta voiture, et dis-moi OU » ────────────────────────────
    //
    // ⚠️ Un COMPTEUR, pas un booleen : un joueur sort sa voiture plusieurs fois, et deux ordres
    // successifs pour le meme vehicule seraient indiscernables par un drapeau — le second ne
    // partirait jamais.
    int32_t Tessera_InvocationSeq() const { return m_invocationSeq; }

    /// Le record a invoquer. Chaine VIDE quand aucun ordre n'est en cours.
    ///
    /// ⚠️ C'est le record de la variante **JOUEUR** que le serveur doit envoyer
    /// (`..._player`) : le garage du jeu ne connait pas les variantes de circulation, et
    /// `EnablePlayerVehicle` les refuse (F-VEH-055). Le client ne corrige PAS ce nom — s'il est
    /// faux, l'invocation echoue et le dit, plutot que de deviner.
    Red::CString Tessera_InvocationRecord() const
    {
        return Red::CString(m_invocationRecord.c_str());
    }

    /// Rapporte la position que le JEU a choisie. Rend `false` si aucun ordre n'est en cours —
    /// donc si on rapporte deux fois, ou sans avoir ete sollicite.
    bool Tessera_RapporterInvocation(float x, float y, float z)
    {
        return SendRapportInvocation(x, y, z);
    }

    bool Tessera_EstVehiculeReseau(RED4ext::ent::EntityID cible) const
    {
        if (!cible.IsDefined())
        {
            return false;
        }
        for (const auto& paire : m_networkedEntitiesLookup)
        {
            if (paire.second == cible)
            {
                return true;
            }
        }
        return false;
    }

    // ── LA CASSE : la file que redscript draine ──────────────────────────────────────────────
    /// La derniere casse que le SERVEUR a annoncee pour ce vehicule, ou -1 s'il n'en a jamais
    /// parle.
    ///
    /// ⚠️ C'EST LE GARDE-FOU CONTRE L'ECHO, et il n'est pas cosmetique. Un temoin qui ecrit les PV
    /// que le serveur lui donne redeclenche `ReactToHPChange` chez lui ; s'il conduit, il
    /// RENVERRAIT la casse qu'il vient de recevoir. Comme la casse est MONOTONE cote serveur, un
    /// aller-retour ne peut que la faire MONTER — les voitures se degraderaient toutes seules,
    /// sans que personne ne les touche.
    ///
    /// Un simple drapeau « je suis en train d'appliquer » ne suffirait pas : l'ecriture passe par
    /// `RequestSettingStatPoolValue`, une DEMANDE, dont le rappel arrive plus tard — apres que le
    /// drapeau soit retombe. Comparer a ce que le serveur sait deja est la seule garde qui ne
    /// depende pas du moment.
    int32_t Tessera_DegatsConnus(RED4ext::ent::EntityID cible) const
    {
        for (const auto& paire : m_networkedEntitiesLookup)
        {
            if (paire.second == cible)
            {
                const auto it = m_degatsConnus.find(paire.first);
                return (it == m_degatsConnus.end()) ? -1 : static_cast<int32_t>(it->second);
            }
        }
        return -1;
    }

    int32_t Tessera_DegatsEnAttente() const
    {
        return static_cast<int32_t>(m_degatsAAppliquer.size());
    }

    /// L'`EntityID` LOCALE du vehicule en tete de file. Vide si la file est vide, ou si le
    /// vehicule n'est plus dans le monde — auquel cas l'appelant doit quand meme defiler.
    RED4ext::ent::EntityID Tessera_DegatsVehicule() const
    {
        RED4ext::ent::EntityID vide{};
        if (m_degatsAAppliquer.empty())
        {
            return vide;
        }
        const auto it = m_networkedEntitiesLookup.find(m_degatsAAppliquer.front().first);
        return (it == m_networkedEntitiesLookup.end()) ? vide : it->second;
    }

    int32_t Tessera_DegatsValeur() const
    {
        return m_degatsAAppliquer.empty() ? 0 : static_cast<int32_t>(m_degatsAAppliquer.front().second);
    }

    /// Defile. ⚠️ A appeler MEME quand le vehicule est introuvable, sinon la file se bouche sur
    /// une entree impossible a traiter et plus aucune casse n'arrive — une panne qui ressemble a
    /// « le serveur n'envoie plus rien ».
    void Tessera_DegatsDefiler()
    {
        if (!m_degatsAAppliquer.empty())
        {
            m_degatsAAppliquer.pop_front();
        }
    }

    int32_t Tessera_CoffreSeq() const { return m_coffreSeq; }

    /// L'`EntityID` LOCALE du vehicule dont on tient le coffre — pas l'id reseau. C'est celle-la
    /// que redscript sait manipuler ; la traduction vit ici, ou vit la table.
    RED4ext::ent::EntityID Tessera_CoffreVehicule() const
    {
        RED4ext::ent::EntityID vide{};
        if (m_coffreVehicule == 0)
        {
            return vide;
        }
        const auto it = m_networkedEntitiesLookup.find(m_coffreVehicule);
        return (it == m_networkedEntitiesLookup.end()) ? vide : it->second;
    }

    int32_t Tessera_CoffreCapacite() const { return static_cast<int32_t>(m_coffreCapacite); }

    int32_t Tessera_CoffreTaille() const { return static_cast<int32_t>(m_coffreAutoritaire.size()); }

    Red::CString Tessera_CoffreItemId(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_coffreAutoritaire.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_coffreAutoritaire[static_cast<size_t>(index)].id.c_str());
    }

    int32_t Tessera_CoffreItemQuantite(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_coffreAutoritaire.size())
        {
            return 0;
        }
        return static_cast<int32_t>(m_coffreAutoritaire[static_cast<size_t>(index)].quantite);
    }

    // ── LE COFFRE : rapport (client -> serveur) ──────────────────────────────────────────────
    //
    // Trois natifs plutot qu'un seul prenant des tableaux : le marshalling d'un `array<String>`
    // par RTTI est exactement le genre d'endroit ou l'on echoue EN SILENCE (une table Lua convertie
    // en struct vide a deja fait accepter un `Mount` qui n'a rien fait, 2026-07-21). Empiler ligne
    // a ligne ne peut pas se tromper a moitie.
    void Tessera_CoffreViderRapport() { m_coffreRapport.clear(); }

    void Tessera_CoffreAjouterAuRapport(const Red::CString& id, int32_t quantite)
    {
        if (quantite <= 0 || id.Length() == 0)
        {
            return;
        }
        ItemAutoritaire ligne;
        ligne.id = id.c_str();
        ligne.quantite = static_cast<uint32_t>(quantite);
        m_coffreRapport.push_back(std::move(ligne));
    }

    /// Envoie le rapport accumule. Renvoie true si le message est PARTI — jamais qu'il a ete
    /// accepte : le serveur revalide la capacite et peut REFUSER, auquel cas il renvoie la verite
    /// et le client se recale.
    bool Tessera_CoffreEnvoyerRapport()
    {
        if (m_coffreSession == 0 || m_coffreVehicule == 0)
        {
            return false;
        }
        SendCoffreRapport();
        m_coffreRapport.clear();
        // ⛔ LA SESSION DE COFFRE SE FERME ICI, ET SON ABSENCE ETAIT UNE PERTE DE DONNEES.
        //
        // `m_coffreVehicule` n'etait jamais remis a zero. Or c'est LUI que redscript interroge
        // pour savoir si l'ecran qui vient de se fermer etait un coffre :
        //
        //     OnUninitialize -> Tessera_CoffreVehicule() defini ? -> rapporter le contenu
        //
        // Il reste donc defini pour toute la session APRES la premiere ouverture. Le prochain
        // ecran de VENDEUR — un ripperdoc, un armurier — se ferme, le crochet se declenche, lit
        // un contenant qui n'a rien a voir, et annonce au serveur que le coffre est VIDE. Le
        // serveur, qui fait confiance a l'etat complet (c'est la doctrine), l'efface.
        //
        // Le joueur perdrait le contenu de son coffre en allant acheter des munitions, sans que
        // rien ne le signale nulle part.
        //
        // ⚠️ Remettre a zero rend aussi l'appel IDEMPOTENT — un second `OnUninitialize` pour le
        // meme ecran ne renvoie plus rien. C'est la regle du depot sur les effets de bord
        // destructifs : ils DOIVENT l'etre, sinon leur appelant est un champ de mines.
        m_coffreVehicule = 0;
        m_coffreSession = 0;
        // Le genre aussi : une session de contenant ne doit pas teindre la suivante.
        m_coffreEstContenant = false;
        return true;
    }

    int32_t Tessera_PreserverTaille() const { return static_cast<int32_t>(m_aPreserver.size()); }

    Red::CString Tessera_PreserverId(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= m_aPreserver.size())
        {
            return Red::CString("");
        }
        return Red::CString(m_aPreserver[static_cast<size_t>(index)].c_str());
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

    // ── CEUX-LÀ SONT DES JOUEURS, ET C'EST TOUTE LA DIFFÉRENCE ────────────────────────────────
    //
    // ⚠️ `Tessera_GetVisiblePlayerCount` et `Tessera_AvatarParIndex` ci-dessus ne comptent PAS des
    // joueurs : ils parcourent `m_networkedEntitiesLookup`, qui contient TOUTE entité réseau — PNJ
    // du serveur compris. C'est **F-PLY-047**, et ça a coûté une tentative de mesure le
    // 2026-08-15 : le compte rendait **4** pour deux joueurs, et `AvatarParIndex(0)` tombait sur un
    // `Character.CitizenRichMale`. La sonde a joué sa séquence sur un passant.
    //
    // Le discriminant existait pourtant déjà, à portée de main : `g_tamponsJoueurs` n'est alimenté
    // que depuis `snapshot->players()` (`HandleSnapshot`), jamais depuis `npcs` ni `vehicles`. Un
    // id qui s'y trouve EST un joueur, par construction — pas par heuristique sur la valeur de
    // l'id, piège déjà payé côté serveur (`d3dcc67` : filtrer par plage numérique excluait à tort
    // les PNJ nominatifs).
    //
    // On ne rend que les avatars qui ont un CORPS : un joueur connu dont l'entité n'est pas encore
    // née n'est pas désignable, et le rendre ferait échouer l'appelant sur un `EntityID` nul sans
    // qu'il sache pourquoi. Le recensement `voisins` de la télémétrie distingue déjà les deux
    // (`connus=` contre `corps=`) — c'est le même critère ici.
    //
    // ⚠️ Même avertissement que ci-dessus : l'index n'est PAS un identifiant. `g_tamponsJoueurs`
    // est un `std::map` dont l'ordre change quand un joueur entre ou sort de portée. Énumérer dans
    // la foulée, mémoriser l'`EntityID`, jamais l'index.
    /// Suspend/reprend la reemission de la commande de marche sur les avatars distants.
    /// Outil de MESURE (voir `g_suspendreCommandes`), jamais un mecanisme de production : sans lui
    /// aucun test de placement n'est interpretable, notre boucle reemettant l'ordre toutes les
    /// ~25 ms — trois tests du 2026-08-16 en sont morts.
    bool Tessera_SuspendreCommandes(bool actif);

    /// Suspend/reprend les CORRECTIONS de position en laissant la marche tourner — instrument de
    /// la mesure de determinisme (ADR 0032). Inverse de `Tessera_SuspendreCommandes`.
    /// La pose MONDE que le netcode VOUDRAIT pour cet avatar attache. Le redscript en deduit
    /// l'ecart local a ecrire — le petit terme du repere relatif, celui du mouvement du passager
    /// DANS la cabine.
    /// L'allure annoncee pour cet avatar attache.
    int32_t Tessera_AllureAttachee(uint32_t entiteHash) const
    {
        return static_cast<int32_t>(AllureAttachee(entiteHash));
    }

    RED4ext::Vector4 Tessera_PoseVoulueAttachee(uint32_t entiteHash) const
    {
        return PoseVoulueAttachee(entiteHash);
    }

    bool Tessera_EcrireOffsetLocal(const Red::Handle<RED4ext::IScriptable>& moveComponent,
                                   float x, float y, float z);
    bool Tessera_PoserJoueurLocalPorte(bool actif);
    bool Tessera_JoueurLocalPorte();
    bool Tessera_AvatarPorteParPlateforme(uint32_t entiteHash, bool actif);
    bool Tessera_SuspendreCorrections(bool actif);

    /// Bascule le pilotage par les ENTREES (ADR 0032). Voir `g_pilotageParEntrees`.
    bool Tessera_PilotageParEntrees(bool actif);

    /// Allume ou eteint le SPAWN ENRICHI — le corps d'un joueur distant porte SON V au lieu du
    /// visage d'un passant. ⚠️ **Eteint par defaut** : couche 3 (ADR 0015), mode d'echec = crash du
    /// processus. Rend l'etat effectif, pour qu'un depouillement puisse dire si le mode etait actif
    /// — la sonde du 2026-08-16 a conclu au succes sur un dispositif qui ne mesurait rien.
    bool Tessera_SpawnEnrichi(bool actif);

    /// Applique les drapeaux de ligne de commande qui règlent des globales d'autres unités de
    /// compilation. Idempotent, appelé au premier spawn réseau.
    void TesseraAppliquerDrapeauxUneFois();

    /// Vrai tant qu'on accepte d'attendre l'apparence complete de ce voisin (delai borne).
    bool AttendreEncore(uint64_t networkId);

    /// Guette le moment ou l'entite d'un avatar devient resolvable, et le dit UNE seule fois.
    void GuetterResolution(uint64_t networkId, RED4ext::ent::EntityID entityId,
                           const Tessera::Sync::PoseRendue& pose);
    bool m_drapeauxAppliques = false;

    /// SONDE (F-PLY-101, etape 1) — LIT la table d'alias FPP/TPP de l'etat de customisation, sans
    /// rien modifier. Ecrit le releve dans `TesseraLogs\alias-apparence.txt` et rend un resume.
    ///
    /// POURQUOI. La decompilation de `FUN_14038582c` (RVA 0x38582C, resolution de groupe commune aux
    /// trois sections Head/Body/Arms) montre que `isFPP` ne fait que CHOISIR UNE COLONNE dans une
    /// table de remappage portee par l'etat :
    ///
    ///     table  = *(etat + 0xa0)                  entrees de 3 longlong
    ///     nombre = *(uint32_t*)(etat + 0xac)
    ///     si table[i][0] == groupe : groupe = table[i][(isFPP & 0xff ^ 1) + 1]
    ///
    /// soit la colonne 1 quand `isFPP` est VRAI, la colonne 2 quand il est FAUX. La table est donc
    /// une DONNEE lisible, et cette sonde la lit AVANT qu'on ecrive le moindre detour.
    ///
    /// Elle tranche deux choses d'un coup : (a) que les colonnes se lisent bien `nom | FPP | TPP`,
    /// sans quoi l'interpretation serait fausse et le detour inutile ; (b) le nom de la variante TPP
    /// des deux groupes qui nous manquent — la peau du visage et celle des bras (F-PLY-100).
    ///
    /// ⚠️ LECTURE SEULE, et par des voies MULTIPLES journalisees. Le nom exact de l'accesseur du
    /// systeme de customisation ne se verifie pas depuis l'exterieur du jeu : la sonde en essaie
    /// plusieurs et ECRIT laquelle passe. Un echec muet est precisement ce qui a coute deux jours sur
    /// ce chantier — ici chaque voie ratee laisse une ligne.
    Red::CString Tessera_LireTableAlias();

    int32_t Tessera_CompteAvatarsJoueurs() const
    {
        int32_t n = 0;
        for (const auto& [networkId, tampon] : g_tamponsJoueurs)
        {
            if (m_networkedEntitiesLookup.find(networkId) != m_networkedEntitiesLookup.end())
            {
                ++n;
            }
        }
        return n;
    }

    // L'EntityID du N-ième avatar de JOUEUR pourvu d'un corps. `EntityID{}` si l'index est hors
    // bornes — l'appelant doit tester, comme pour `Tessera_AvatarParIndex`.
    /// La cabine qui PORTE l'avatar `index` — meme ordre que `Tessera_AvatarJoueurParIndex`, pour
    /// que les deux se lisent dans la meme boucle. Rend un `EntityID` vide si ce joueur est a pied.
    ///
    /// ⭐ C'est le declencheur de l'attache. Le C++ SAIT deja qui est passager de quelle cabine —
    /// `g_porteurParAvatar`, alimente par `PlayerState.frame` (ADR 0039) —, mais il ne peut pas
    /// attacher lui-meme : `BindToComponent` prend des `EntityGameInterface`, que seul le script
    /// fabrique proprement. D'ou le patron PULL, le seul prouve sur ce pont (memoire de travail
    /// « pont-cpp-vers-redscript ») : le C++ tient l'etat, le redscript le lit et agit.
    RED4ext::ent::EntityID Tessera_CabineDeAvatar(int32_t index) const
    {
        if (index < 0)
        {
            return RED4ext::ent::EntityID{};
        }
        int32_t n = 0;
        for (const auto& [networkId, tampon] : g_tamponsJoueurs)
        {
            const auto corps = m_networkedEntitiesLookup.find(networkId);
            if (corps == m_networkedEntitiesLookup.end())
            {
                continue;   // meme filtre que ci-dessus : les index doivent coincider.
            }
            if (n == index)
            {
                const uint64_t cabine = CabineDeAvatarReseau(networkId);
                if (cabine == 0)
                {
                    return RED4ext::ent::EntityID{};   // a pied — et c'est un VERDICT, pas un echec.
                }
                return RED4ext::ent::EntityID{cabine};
            }
            ++n;
        }
        return RED4ext::ent::EntityID{};
    }

    RED4ext::ent::EntityID Tessera_AvatarJoueurParIndex(int32_t index) const
    {
        if (index < 0)
        {
            return RED4ext::ent::EntityID{};
        }
        int32_t n = 0;
        for (const auto& [networkId, tampon] : g_tamponsJoueurs)
        {
            const auto corps = m_networkedEntitiesLookup.find(networkId);
            if (corps == m_networkedEntitiesLookup.end())
            {
                continue;
            }
            if (n == index)
            {
                return corps->second;
            }
            ++n;
        }
        return RED4ext::ent::EntityID{};
    }

    // ── LE ROSTER DES VEHICULES — parce que la requete spatiale du JEU ne les voit pas ────────
    //
    // ⛔ MESURE DU 2026-08-26, et elle invalide un idiome utilise partout dans le harnais :
    // `GetEntitiesAroundObject(40)` NE REND PAS les entites nees du netcode. Sonde faite dans
    // l'appartement de V, avec un avatar distant a 4 m et une voiture serveur a 1 m :
    //
    //     128 entites trouvees — 115 objets statiques, 5 items, 5 PNJ. ZERO vehicule, ZERO joueur.
    //
    // Les entites EXISTENT pourtant : `Tessera_AvatarJoueurParIndex(0)` rend un `EntityID` que
    // `FindEntityByID` resout, avec ses 188 composants — et l'avatar est visible a l'ecran sur la
    // capture prise a la meme minute. Ce que la requete spatiale indexe, ce n'est donc pas « ce
    // qu'il y a autour », c'est ce que le STREAMING du monde a pose. Nos entites sont dans le
    // monde sans etre dans cet index-la.
    //
    // Consequence pratique : tout designateur de cible base sur la proximite est AVEUGLE a nos
    // propres objets — il ne trouve que le decor natif. C'est ce qui a fait rendre une PORTE a
    // `veh_etat` plus tot dans la journee : les trois voies du designateur echouaient sur nos
    // vehicules, et la seule qui repondait rendait n'importe quoi.
    //
    // La reponse est de demander au netcode, qui tient SA propre table. `m_degatsConnus` est
    // alimente pour CHAQUE vehicule de CHAQUE snapshot (les deux branches y ecrivent, y compris
    // celle du vehicule intact) : ses cles sont donc le roster complet des vehicules serveur.
    // Aucune table supplementaire a tenir synchronisee.
    int32_t Tessera_CompteVehiculesReseau() const
    {
        int32_t n = 0;
        for (const auto& [reseauId, casse] : m_degatsConnus)
        {
            if (m_networkedEntitiesLookup.find(reseauId) != m_networkedEntitiesLookup.end())
            {
                ++n;
            }
        }
        return n;
    }

    /// L'id RESEAU du N-ieme vehicule serveur, en decimal. Chaine VIDE hors bornes.
    ///
    /// ⚠️ POURQUOI UNE CHAINE POUR UN NOMBRE. C'est un id sur 64 bits, et il ne sert qu'a etre LU
    /// et COMPARE par un humain ou par un script de sonde — jamais a calculer. Le rendre en texte
    /// evite la question du transport d'un entier 64 bits jusqu'a Lua, qui n'en a pas.
    ///
    /// ⭐ CE QU'IL DEBLOQUE, ET CE N'EST PAS DU CONFORT. Sans lui, un designateur ne peut viser que
    /// « le plus proche » — or les voitures du serveur sont groupees a quelques metres les unes des
    /// autres, et surtout elles ne se valent pas : celles du MANIFESTE n'ont aucune ligne en base,
    /// donc **aucun coffre**. Une sonde de coffre tiree sur l'une d'elles ne mesure rien et le dit
    /// mal : le serveur ignore la demande en silence, ce qui ressemble exactement a une chaine
    /// cassee. Mesure du 2026-08-26 : `tick N seq=0` a l'infini, pour une sonde parfaitement saine
    /// tiree sur la mauvaise voiture.
    Red::CString Tessera_VehiculeReseauIdParIndex(int32_t index) const
    {
        if (index < 0)
        {
            return Red::CString("");
        }
        int32_t n = 0;
        for (const auto& [reseauId, casse] : m_degatsConnus)
        {
            if (m_networkedEntitiesLookup.find(reseauId) == m_networkedEntitiesLookup.end())
            {
                continue;
            }
            if (n == index)
            {
                return Red::CString(std::to_string(reseauId).c_str());
            }
            ++n;
        }
        return Red::CString("");
    }

    /// L'`EntityID` LOCALE du N-ieme vehicule serveur. Vide hors bornes — l'appelant doit tester.
    ///
    /// ⚠️ L'index n'est PAS un identifiant : il se decale des qu'un vehicule est destreame. Il ne
    /// sert qu'a parcourir le roster dans l'instant, jamais a designer une voiture d'un appel a
    /// l'autre.
    RED4ext::ent::EntityID Tessera_VehiculeReseauParIndex(int32_t index) const
    {
        if (index < 0)
        {
            return RED4ext::ent::EntityID{};
        }
        int32_t n = 0;
        for (const auto& [reseauId, casse] : m_degatsConnus)
        {
            const auto corps = m_networkedEntitiesLookup.find(reseauId);
            if (corps == m_networkedEntitiesLookup.end())
            {
                continue;
            }
            if (n == index)
            {
                return corps->second;
            }
            ++n;
        }
        return RED4ext::ent::EntityID{};
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

    /// UN SEUL natif pour TOUS les verbes vehicule (verrou, revendication, radio, degats, coffre).
    ///
    /// ⚠️ POURQUOI IL N'EXISTAIT PAS, ET CE QUE CA COUTAIT. Le serveur ecoute les kinds 9 a 12
    /// depuis le 2026-08-15 — verrou, revendication, radio, casse — avec leurs tests verts, et
    /// AUCUN client ne les a jamais envoyes. Exactement la panne des postures (kind 13, ecoute
    /// depuis le 19 aout, jamais emise) : du code serveur juste, teste, et injoignable. Un canal
    /// montant qui manque ne casse rien et ne dit rien ; il rend simplement toute la
    /// fonctionnalite inerte, et on croit que c'est le serveur qui ne repond pas.
    ///
    /// ⚠️ LA PLAGE EST GARDEE, ET CE N'EST PAS DE LA PRUDENCE DECORATIVE. Un natif « envoie
    /// n'importe quel kind » laisserait une faute de frappe partir en verbe d'ascenseur ou de
    /// posture — et cote serveur, `target` serait alors lu dans un AUTRE espace d'identifiants.
    /// La collision kinds 6/7 (MountElevator reutilise par erreur pour le vehicule) a fait tomber
    /// dix-sept tests d'ascenseur d'un coup. Ici elle serait silencieuse.
    ///
    /// Renvoie true si le message est PARTI — jamais qu'il a ete accepte (D1). Le serveur
    /// reverifie propriete, siege et portee, et peut refuser sans le dire.
    bool Tessera_VehiculeVerbe(RED4ext::ent::EntityID cible, uint8_t verbe, uint32_t param)
    {
        // 9=verrou 10=revendiquer 11=radio 12=degats 14=ouvrir le coffre (13 = POSTURE, pas nous).
        if (verbe < 9 || verbe > 14 || verbe == 13)
        {
            return false;
        }
        if (!cible.IsDefined())
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
            // Une voiture de la circulation native ne designe personne chez le serveur. Lui
            // inventer une identite serait un mensonge sur le fil.
            return false;
        }
        SendVehiculeVerbe(idReseau, verbe, param);
        return true;
    }

    /// Combien d'états de cabine attendent d'être appliqués. Redscript draine cette file dans sa
    /// boucle de veille (`TesseraAscenseurVeille`, ElevatorBridge.reds).
    /// Total MONOTONE des etats recus depuis le lancement. Voir `g_ascenseursTotalRecus`.
    /// POSTURES — l'annonce montante. `emplacement == 0` libere. Le serveur decide.
    ///
    /// ⚠️ DECLAREE ICI, EN PORTEE PUBLIQUE, ET C'EST LA RAISON D'ETRE DE CE COMMENTAIRE.
    /// Premiere version posee a cote de `SendElevatorMount` (portee `protected`) : la DLL a
    /// compile et lie sans un mot, et le nom N'ETAIT PAS dans le binaire — donc le `native
    /// func` cote redscript n'aurait eu aucun backing et TOUT `r6/scripts` serait tombe au
    /// lancement. Le controle en une ligne de la skill (chercher le nom dans la DLL) l'a
    /// attrape avant le jeu. Les natifs se declarent avec les autres, en public.
    /// ⚠️ CORPS EN LIGNE, ET C'EST LA LECON DU 2026-08-24. Premiere version : declaree ici,
    /// definie dans le .cpp. Ca compile, ca lie — et le nom N'ARRIVE PAS dans la DLL. Controle :
    /// sur les 78 `RTTI_METHOD` du fichier, 77 sont dans le binaire et seule celle-la manquait.
    /// Un `native func` sans backing fait tomber TOUT `r6/scripts` au lancement, sans un mot
    /// (F-PLF-020). Les 77 qui marchent ont toutes leur corps ICI : on copie ce qui marche, et
    /// le travail reel (flatbuffers, socket) reste dans le .cpp derriere ce relais d'une ligne.
    /// ⚠️ RETOURNE UN BOOL, ET CE N'EST PAS UN CHOIX DE STYLE. Premiere version en `void` :
    /// declaree, compilee, liee — et ABSENTE du binaire. Controle : sur les 78 `RTTI_METHOD` du
    /// fichier, 77 dans la DLL, seule celle-la manquante ; et la fonction de travail derriere
    /// elle etait eliminee comme inutilisee, preuve que la macro ne la referencait pas. Les 77
    /// qui marchent rendent toutes une valeur (`Tessera_EnvoyerAction` prend meme des parametres
    /// et rend `bool`). Un `native func` sans backing fait tomber TOUT `r6/scripts` au lancement,
    /// sans un mot (F-PLF-020) : le controle en une ligne l'a attrape avant le jeu.
    bool Tessera_SignalerPosture(uint64_t emplacementId, uint32_t code)
    {
        return EnvoyerPosture(emplacementId, code);
    }

    int32_t Tessera_AscenseurTotalRecus()
    {
        return g_ascenseursTotalRecus;
    }

    int32_t Tessera_AscenseurEnAttente()
    {
        return static_cast<int32_t>(g_ascenseursRecus.size());
    }

    /// Les cinq champs de l'état EN TÊTE de file, puis `Tessera_AscenseurRetirer` pour avancer.
    /// Cinq accesseurs plutôt qu'un objet : redscript ne sait pas recevoir une struct C++ non
    /// enregistrée au RTTI, et enregistrer un type pour cinq entiers coûterait plus que ces cinq
    /// lignes.
    RED4ext::ent::EntityID Tessera_AscenseurCabine()
    {
        if (g_ascenseursRecus.empty()) { return RED4ext::ent::EntityID{}; }
        return RED4ext::ent::EntityID{g_ascenseursRecus.front().elevatorId};
    }
    int32_t Tessera_AscenseurEtageActif()
    {
        return g_ascenseursRecus.empty() ? 0 : g_ascenseursRecus.front().etageActif;
    }
    int32_t Tessera_AscenseurEtageCible()
    {
        return g_ascenseursRecus.empty() ? -1 : g_ascenseursRecus.front().etageCible;
    }
    int32_t Tessera_AscenseurDepart()
    {
        return g_ascenseursRecus.empty() ? 0 : g_ascenseursRecus.front().departTick;
    }
    int32_t Tessera_AscenseurElapsedMs()
    {
        return g_ascenseursRecus.empty() ? 0 : g_ascenseursRecus.front().elapsedMs;
    }
    void Tessera_AscenseurRetirer()
    {
        if (!g_ascenseursRecus.empty()) { g_ascenseursRecus.pop_front(); }
    }

    // ── APPAREILS DU MONDE ───────────────────────────────────────────────────────────────────
    //
    // Le guichet du PULL. Meme decoupage que les ascenseurs : un accesseur par champ, parce que
    // redscript ne sait pas recevoir une struct C++ non enregistree au RTTI, et qu'enregistrer un
    // type pour quatre entiers couterait plus cher que ces quatre lignes.

    /// Total MONOTONE recu depuis le lancement, jamais decremente. La file est drainee toutes les
    /// 50 ms : la lire ne dit RIEN sur ce qui est arrive. Ce compteur, si.
    int32_t Tessera_AppareilTotalRecus()
    {
        return g_appareilsTotalRecus;
    }
    int32_t Tessera_AppareilEnAttente()
    {
        return static_cast<int32_t>(g_appareilsRecus.size());
    }
    RED4ext::ent::EntityID Tessera_AppareilId()
    {
        if (g_appareilsRecus.empty()) { return RED4ext::ent::EntityID{}; }
        return RED4ext::ent::EntityID{g_appareilsRecus.front().device};
    }
    int32_t Tessera_AppareilFamille()
    {
        return g_appareilsRecus.empty() ? 0 : g_appareilsRecus.front().famille;
    }
    int32_t Tessera_AppareilEtat()
    {
        return g_appareilsRecus.empty() ? -1 : g_appareilsRecus.front().etat;
    }
    /// `0` = appareil du monde, sans proprietaire. Ce n'est pas une sentinelle d'erreur : c'est
    /// l'etat par defaut de toute la carte.
    ///
    /// ⚠️ Tronque en int32 : redscript n'a d'operateur de comparaison ni pour `Uint64` ni pour
    /// `Uint32` (meme piege que `depart_tick`, paye sur les ascenseurs). Un `characters.id` reste
    /// tres en dessous de 2^31 sur toute duree d'exploitation plausible.
    int32_t Tessera_AppareilProprietaire()
    {
        return g_appareilsRecus.empty()
            ? 0
            : static_cast<int32_t>(g_appareilsRecus.front().proprietaire & 0x7FFFFFFF);
    }
    /// Combien de joueurs tiennent cet appareil ouvert. `0` = la fermeture automatique locale
    /// reprend ses droits.
    ///
    /// ⚠️ Un client dont la DLL est en retard rend toujours 0 : il retombe sur le comportement
    /// d'avant (fermeture locale), jamais sur une porte bloquee ouverte. Le defaut va dans le sens
    /// sur.
    int32_t Tessera_AppareilTenants()
    {
        return g_appareilsRecus.empty() ? 0 : g_appareilsRecus.front().tenants;
    }

    /// ⚠️ REND UN BOOL alors qu'il n'a rien a rendre. Voir le commentaire de
    /// `Tessera_SignalerPosture` : une `RTTI_METHOD` en `void` a deja ete declaree, compilee,
    /// liee — et ABSENTE du binaire, ce qui fait tomber TOUT `r6/scripts` au lancement sans un mot
    /// (F-PLF-020). On ne rejoue pas ce diagnostic pour economiser un `return`.
    bool Tessera_AppareilRetirer()
    {
        if (g_appareilsRecus.empty()) { return false; }
        g_appareilsRecus.pop_front();
        return true;
    }

    /// LA CONSOLE, cote LECTURE — le pendant de `Tessera_EnvoyerCommandeAdmin`.
    ///
    /// Depile UNE ligne et la rend sous la forme **`"<niveau>|<texte>"`**. Rend `""` quand la file
    /// est vide — ce qui est un etat NORMAL, pas une erreur.
    ///
    /// ⚠️ Pourquoi une seule chaine plutot que deux natifs (le niveau, puis le texte) : deux
    /// appels couples partagent un etat implicite entre eux, et le jour ou l un est appele sans
    /// l autre, le script lit le niveau d une ligne et le texte d une autre — sans que rien ne le
    /// signale. Un seul appel ne peut pas se desynchroniser.
    Red::CString Tessera_ConsoleLireLigne()
    {
        if (g_lignesConsole.empty())
        {
            return Red::CString("");
        }
        const LigneConsole ligne = g_lignesConsole.front();
        g_lignesConsole.pop_front();
        const std::string encode = std::to_string(static_cast<int>(ligne.niveau)) + "|" + ligne.texte;
        return Red::CString(encode.c_str());
    }

    /// Combien de lignes attendent. Sert a la pastille « non lus » quand la console est fermee —
    /// et a distinguer, dans un diagnostic, « la file est vide » de « le script ne depile pas ».
    int32_t Tessera_ConsoleEnAttente() { return static_cast<int32_t>(g_lignesConsole.size()); }

    /// Tout ce qui est ARRIVE depuis le lancement, jamais decremente. Un `EnAttente = 0` avec un
    /// `TotalRecu = 0` dit « le serveur n a rien envoye » ; avec un total non nul, il dit « tout a
    /// ete consomme ». Deux pannes opposees que la file seule confondrait.
    int32_t Tessera_ConsoleTotalRecu() { return g_lignesConsoleTotalRecues; }

    /// LE CANAL DE COMMANDE D'ADMINISTRATION — et il MANQUAIT ENTIEREMENT.
    ///
    /// ⚠️ Constate le 2026-08-26 : le serveur comprend `ClientMsg::AdminCommand` depuis toujours
    /// (`gateway_routing::extract_admin_command`), et **AUCUN client ne l'a jamais emis**. Tout le
    /// vocabulaire d'administration — `/promote`, `/grant`, `/ban`, `/vehicule`, `/besoins`, et
    /// maintenant `/porte` — etait donc du code injoignable depuis le jeu. Seuls les tests Rust en
    /// construisaient.
    ///
    /// C'est le meme mode de panne que les sept fils debranches du chantier « autorite totale » :
    /// un producteur complet, teste, documente, dont la sortie n'allait nulle part.
    ///
    /// L'AUTORITE NE BOUGE PAS D'UN POUCE. Le serveur decide seul de ce qu'il accepte : il
    /// revalide le rang de l'appelant (`is_root`) et ses permissions avant d'executer quoi que ce
    /// soit. Ce natif ne fait qu'ouvrir le tuyau — il ne donne aucun droit.
    ///
    /// Rend true si le message est PARTI. Jamais qu'il a ete accepte (doctrine D1).
    bool Tessera_EnvoyerCommandeAdmin(const Red::CString& texte)
    {
        if (texte.Length() == 0)
        {
            return false;
        }
        SendAdminCommand(texte.c_str());
        return true;
    }

    /// APPAREILS — le joueur vient d'agir sur `device`, et l'a laisse dans l'etat `etat`.
    ///
    /// `device` est l'`EntityID` de l'appareil, LU sur l'entite et jamais recalcule (meme regle
    /// que les cabines, F-ASC-027 : le hash n'est pas reconstructible hors jeu). Aucune traduction
    /// par `m_networkedEntitiesLookup` : un appareil est du decor que les deux cotes designent par
    /// la meme cle stable, pas une entite repliquee.
    ///
    /// Rend true si le message est PARTI. Jamais qu'il a ete accepte (doctrine D1) : le serveur
    /// revalide la famille, l'etat, la distance et les droits, et refuse en silence cote fil.
    /// « Ouvre-moi ce contenant du monde. » Le serveur repond par un `InteractionOpen` portant
    /// `ui_kind = 8` et le contenu autoritaire — exactement comme pour un coffre de vehicule.
    ///
    /// ⚠️ La cible part TELLE QUELLE (`device.hash`), sans passer par `m_networkedEntitiesLookup`.
    /// Un appareil du monde n'est pas une entite reseau : c'est une entite du DECOR, que les deux
    /// cotes designent par son `EntityID` de jeu. C'est la meme convention que les ascenseurs
    /// (ADR 0012 §2.5) et que `Tessera_RapporterAppareil` juste en dessous.
    ///
    /// Rend true si le message est PARTI — jamais qu'il a ete accepte (D1).
    bool Tessera_OuvrirContenant(RED4ext::ent::EntityID device)
    {
        if (!device.IsDefined())
        {
            return false;
        }
        // kind 15 = OuvrirContenant (14 = coffre de vehicule, 13 = posture, 8 volontairement vide).
        SendVehiculeVerbe(device.hash, 15, 0);
        return true;
    }

    bool Tessera_RapporterAppareil(RED4ext::ent::EntityID device, int32_t famille, int32_t action,
        int32_t etat)
    {
        if (!device.IsDefined() || famille < 0 || famille > 255 || action < 0 || action > 255
            || etat < 0 || etat > 255)
        {
            return false;
        }
        SendDeviceCall(device.hash, static_cast<uint8_t>(famille), static_cast<uint8_t>(action),
            static_cast<uint8_t>(etat));
        return true;
    }

    /// ASCENSEURS — le joueur a demande `etage` sur la cabine `cabine`.
    ///
    /// `cabine` est l'EntityID STATIQUE de la cabine, LU sur l'entite par redscript et jamais
    /// recalcule (ADR 0012 §5 : l'algorithme du hash n'est pas confirme, F-ASC-011). On passe donc
    /// `cabine.hash` tel quel — contrairement a `Tessera_EnvoyerAction`, aucune traduction par
    /// `m_networkedEntitiesLookup` : une cabine n'est pas une entite reseau, c'est un decor que
    /// les deux cotes designent par la meme cle stable.
    ///
    /// Rend true si le message est PARTI. Jamais qu'il a ete accepte (D1) : le serveur revalide
    /// l'existence de la cabine et la viabilite de l'etage, et ignore en silence ce qui ne va pas.
    bool Tessera_AppelerAscenseur(RED4ext::ent::EntityID cabine, int32_t etage)
    {
        if (!cabine.IsDefined())
        {
            return false;
        }
        SendElevatorCall(cabine.hash, etage);
        return true;
    }

    /// ASCENSEURS — le joueur vient d'entrer (monte=true) ou de sortir d'une cabine.
    bool Tessera_MonterAscenseur(RED4ext::ent::EntityID cabine, bool monte)
    {
        if (!cabine.IsDefined())
        {
            return false;
        }
        SendElevatorMount(cabine.hash, monte);
        return true;
    }

    /// ASCENSEURS — un joueur DISTANT est-il dans cette cabine ?
    ///
    /// ⭐ C'est la notion d'occupation qui MANQUAIT au moteur. Le sien, `IsPlayerInsideLift()`,
    /// interroge le tableau noir du joueur LOCAL (`door.script:589`, `liftController.script:567`) :
    /// une cabine pleine de monde est donc « vide » pour tous ceux qui n'y sont pas. Trois
    /// comportements natifs en dependent et divergent donc d'un ecran a l'autre — la vitesse
    /// (x2 a vide, `GetLiftSpeed`), l'ouverture des portes de palier, et l'obstruction du passage.
    ///
    /// Le remede n'est pas de repliquer chaque comportement, c'est de partager la QUESTION : le
    /// redscript compose ce resultat avec le verdict local, et les trois se remettent d'accord.
    ///
    /// Ne compte QUE les distants, volontairement : le joueur local est deja couvert par le
    /// tableau noir, et l'ajouter ici en ferait un cas double.
    bool Tessera_CabineOccupee(RED4ext::ent::EntityID cabine)
    {
        if (!cabine.IsDefined())
        {
            return false;
        }
        return CabinePorteQuelquun(cabine.hash);
    }

    /// ASCENSEURS — redscript PUBLIE la hauteur vivante du plancher d'une cabine.
    ///
    /// ⭐ C'est la piece qui manquait pour rendre un passager distant sans sautillement, et elle ne
    /// pouvait venir que de la : le C++ ne sait pas atteindre un COMPOSANT d'entite, et l'entite
    /// elle-meme ne bouge pas (F-ASC-032). Redscript, lui, lit `movingPlatform` sans peine.
    ///
    /// Avec ca, la verticale d'un passager cesse de venir du reseau — donc cesse d'avoir un retard,
    /// donc cesse de sautiller. C'est le patron des moteurs du metier : repliquer la position
    /// RELATIVE au porteur et recomposer avec la position LOCALE du porteur au moment du rendu
    /// (Unreal `ReplicatedBasedMovement`, Unity `NetworkTransform` en espace local).
    bool Tessera_PoserHauteurCabine(RED4ext::ent::EntityID cabine, float z);

    /// ASCENSEURS — redscript confie le COMPOSANT du plancher, une fois pour toutes.
    ///
    /// Preferable a publier sa hauteur : le rendu la lit alors dans SA frame, sans echantillonnage
    /// ni extrapolation. `Tessera_PoserHauteurCabine` reste comme repli.
    bool Tessera_PoserPlancherCabine(RED4ext::ent::EntityID cabine,
                                     const Red::Handle<RED4ext::IScriptable>& plancher);

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
    RTTI_METHOD(Tessera_SignalerPosture);
    RTTI_METHOD(EnqueueLoadLastCheckpoint);
    RTTI_METHOD(Tessera_GetServerShard);
    RTTI_METHOD(Tessera_GetServerOverlaps);
    RTTI_METHOD(Tessera_GetVisiblePlayerCount);
    RTTI_METHOD(Tessera_ReportStim);
    RTTI_METHOD(Tessera_EstEntiteReseau);
    RTTI_METHOD(Tessera_RapporterDegats);
    RTTI_METHOD(Tessera_RapporterArme);
    RTTI_METHOD(Tessera_PremierVidageEtat);
    RTTI_METHOD(Tessera_ArmeDeLEntite);
    RTTI_METHOD(Tessera_NombreDeVetements);
    RTTI_METHOD(Tessera_AvatarCorpsMasculin);
    RTTI_METHOD(Tessera_VetementDeLEntite);
    RTTI_METHOD(Tessera_DemanderReapparition);
    RTTI_METHOD(Tessera_RapporterVariation);
    RTTI_METHOD(Tessera_SecondesSecours);
    RTTI_METHOD(Tessera_HopitalOuvert);
    RTTI_METHOD(Tessera_Faim);
    RTTI_METHOD(Tessera_Soif);
    RTTI_METHOD(Tessera_SilenceMs);
    RTTI_METHOD(Tessera_TentativesReconnexion);
    RTTI_METHOD(Tessera_ReconnecterMaintenant);
    RTTI_METHOD(Tessera_Journal);
    RTTI_METHOD(Tessera_RapporterStatique);
    RTTI_METHOD(Tessera_ApparenceStatiqueConnue);
    RTTI_METHOD(Tessera_CelluleConnue);
    RTTI_METHOD(Tessera_CobayeApparence);
    RTTI_METHOD(Tessera_DemanderPromotion);
    RTTI_METHOD(Tessera_NombrePersonnages);
    RTTI_METHOD(Tessera_ListePersonnagesRecue);
    RTTI_METHOD(Tessera_ModeDeveloppement);
    RTTI_METHOD(Tessera_PersonnageDemande);
    RTTI_METHOD(Tessera_NomPersonnage);
    RTTI_METHOD(Tessera_OriginePersonnage);
    RTTI_METHOD(Tessera_EsthetiquePersonnage);
    RTTI_METHOD(Tessera_RecettePersonnage);
    RTTI_METHOD(Tessera_RecetteIncarnee);
    RTTI_METHOD(Tessera_QuitterServeur);
    RTTI_METHOD(Tessera_CorpsMasculin);
    RTTI_METHOD(Tessera_CerveauMasculin);
    RTTI_METHOD(Tessera_IdPersonnage);
    RTTI_METHOD(Tessera_RecordPersonnage);
    RTTI_METHOD(Tessera_ApparencePersonnage);
    RTTI_METHOD(Tessera_DernierResultat);
    RTTI_METHOD(Tessera_CreerPersonnage);
    RTTI_METHOD(Tessera_LireEsthetique);
    RTTI_METHOD(Tessera_ChoisirPersonnage);
    RTTI_METHOD(Tessera_SupprimerPersonnage);
    // Interactions joueur<->joueur. ⚠️ Chacune de ces cinq lignes a son `public native func` dans
    // `RedscriptModule/src/Network/NetworkGameSystem.reds` : les deux cotes se posent ET se
    // deploient ENSEMBLE. Un `native func` sans backing dans la DLL deployee fait tomber TOUT
    // r6/scripts et le jeu se ferme sans un mot (F-PLF-020, F-PLF-023).
    RTTI_METHOD(Tessera_NombreCommandes);
    RTTI_METHOD(Tessera_CommandeNom);
    RTTI_METHOD(Tessera_CommandeAffichage);
    RTTI_METHOD(Tessera_NombreActions);
    RTTI_METHOD(Tessera_ActionId);
    RTTI_METHOD(Tessera_ActionLibelle);
    RTTI_METHOD(Tessera_ActionPorteeM);
    RTTI_METHOD(Tessera_NomConnu);
    RTTI_METHOD(Tessera_EnvoyerAction);
    RTTI_METHOD(Tessera_VehiculeVerbe);
    RTTI_METHOD(Tessera_EstVehiculeReseau);
    RTTI_METHOD(Tessera_DegatsConnus);
    RTTI_METHOD(Tessera_DegatsEnAttente);
    RTTI_METHOD(Tessera_DegatsVehicule);
    RTTI_METHOD(Tessera_DegatsValeur);
    RTTI_METHOD(Tessera_DegatsDefiler);
    RTTI_METHOD(Tessera_CoffreSeq);
    RTTI_METHOD(Tessera_CoffreVehicule);
    RTTI_METHOD(Tessera_CoffreCapacite);
    RTTI_METHOD(Tessera_CoffreTaille);
    RTTI_METHOD(Tessera_CoffreItemId);
    RTTI_METHOD(Tessera_CoffreItemQuantite);
    RTTI_METHOD(Tessera_CoffreViderRapport);
    RTTI_METHOD(Tessera_CoffreAjouterAuRapport);
    RTTI_METHOD(Tessera_CoffreEnvoyerRapport);
    RTTI_METHOD(Tessera_InvocationSeq);
    RTTI_METHOD(Tessera_InvocationRecord);
    RTTI_METHOD(Tessera_RapporterInvocation);
    /// Annonce montante d'une demande de POSTURE (s'asseoir, s'appuyer). `emplacementId == 0`
    /// libere. Le serveur decide — le client ne fait que demander.
    RTTI_METHOD(Tessera_AppareilTotalRecus);
    RTTI_METHOD(Tessera_AppareilEnAttente);
    RTTI_METHOD(Tessera_AppareilId);
    RTTI_METHOD(Tessera_AppareilFamille);
    RTTI_METHOD(Tessera_AppareilEtat);
    RTTI_METHOD(Tessera_AppareilProprietaire);
    RTTI_METHOD(Tessera_AppareilTenants);
    RTTI_METHOD(Tessera_AppareilRetirer);
    RTTI_METHOD(Tessera_OuvrirContenant);
    RTTI_METHOD(Tessera_RapporterAppareil);
    RTTI_METHOD(Tessera_EnvoyerCommandeAdmin);
    RTTI_METHOD(Tessera_ConsoleLireLigne);
    RTTI_METHOD(Tessera_ConsoleEnAttente);
    RTTI_METHOD(Tessera_ConsoleTotalRecu);
    RTTI_METHOD(Tessera_AscenseurTotalRecus);
    RTTI_METHOD(Tessera_AscenseurEnAttente);
    RTTI_METHOD(Tessera_AscenseurCabine);
    RTTI_METHOD(Tessera_AscenseurEtageActif);
    RTTI_METHOD(Tessera_AscenseurEtageCible);
    RTTI_METHOD(Tessera_AscenseurDepart);
    RTTI_METHOD(Tessera_AscenseurElapsedMs);
    RTTI_METHOD(Tessera_AscenseurRetirer);
    RTTI_METHOD(Tessera_AppelerAscenseur);
    RTTI_METHOD(Tessera_MonterAscenseur);
    RTTI_METHOD(Tessera_CabineOccupee);
    RTTI_METHOD(Tessera_PoserHauteurCabine);
    RTTI_METHOD(Tessera_PoserPlancherCabine);
    RTTI_METHOD(Tessera_AvatarParIndex);
    // ⚠️ Ces deux-là comptent des JOUEURS, contrairement aux deux ci-dessus (F-PLY-047).
    RTTI_METHOD(Tessera_SuspendreCommandes);
    RTTI_METHOD(Tessera_CabineDeAvatar);
    RTTI_METHOD(Tessera_AllureAttachee);
    RTTI_METHOD(Tessera_PoseVoulueAttachee);
    RTTI_METHOD(Tessera_EcrireOffsetLocal);
    RTTI_METHOD(Tessera_PoserJoueurLocalPorte);
    RTTI_METHOD(Tessera_JoueurLocalPorte);
    RTTI_METHOD(Tessera_AvatarPorteParPlateforme);
    RTTI_METHOD(Tessera_SuspendreCorrections);
    RTTI_METHOD(Tessera_PilotageParEntrees);
    RTTI_METHOD(Tessera_SpawnEnrichi);
    RTTI_METHOD(Tessera_LireTableAlias);
    RTTI_METHOD(Tessera_CompteAvatarsJoueurs);
    RTTI_METHOD(Tessera_AvatarJoueurParIndex);
    RTTI_METHOD(Tessera_CompteVehiculesReseau);
    RTTI_METHOD(Tessera_VehiculeReseauParIndex);
    RTTI_METHOD(Tessera_VehiculeReseauIdParIndex);
    RTTI_METHOD(Tessera_SacRecu);
    RTTI_METHOD(Tessera_SacSeq);
    RTTI_METHOD(Tessera_SacTaille);
    RTTI_METHOD(Tessera_SacItemId);
    RTTI_METHOD(Tessera_SacItemQuantite);
    RTTI_METHOD(Tessera_PreserverTaille);
    RTTI_METHOD(Tessera_PreserverId);
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

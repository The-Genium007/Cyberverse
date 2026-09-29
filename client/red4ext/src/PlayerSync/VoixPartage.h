#pragma once
// =====================================================================================
// VOIX PARTAGEE — le format binaire de la mémoire partagée jeu -> launcher (V4.1, lot 4
// du plan `docs/superpowers/plans/2026-09-26-voix-spatiale.md`, dépôt Tessera).
//
// POURQUOI CE FICHIER EXISTE. La voix minimale (VM3/VM4) a besoin d'un canal LOCAL, sans
// port ni pare-feu : le launcher, qui capture le micro et parle au relais réseau
// `tessera-voix`, doit savoir où est l'auditeur (le joueur local) et où sont les avatars
// qu'il affiche À L'ÉCRAN — pas ceux que le serveur connaît, ceux que CE client a
// effectivement posés, pour que l'atténuation entende ce que l'œil voit. Une mémoire
// partagée nommée est le mécanisme le plus simple : pas de socket local, un launcher
// relancé la relit telle quelle.
//
// ⚠️ CE FICHIER NE FAIT QUE DÉFINIR LE FORMAT. Ni la native `Tessera_VoixParle`, ni
// l'écriture dans la boucle qui pose les avatars (V4.2), ni la lecture côté launcher
// (V4.3, `tessera-administration/launcher/`, hors de ce dépôt) ne sont dans ce lot — voir
// `docs/chantiers/voix-playtest-3.md`, VM3 reste à faire (nécessite une session de jeu,
// hors périmètre de cette passe : « compilé et jamais lancé »).
//
// ── LE NOM DE L'OBJET, ET LA CADENCE ────────────────────────────────────────────────
//
// `Local\TesseraVoix-<pid du jeu>` (le launcher connaît le pid du processus qu'il a
// lancé). Réécrit à CHAQUE IMAGE dans la boucle qui pose déjà les avatars — c'est un
// fichu-à-jour, pas un événement : le launcher lit ce qu'il trouve, quand il veut.
//
// ── LE VERROU SANS VERROU (SEQLOCK), ET POURQUOI ────────────────────────────────────
//
// Le jeu écrit à ~60 Hz, le launcher lit à sa propre cadence, sur deux processus : pas de
// mutex nommé (coût, portabilité), donc une lecture peut tomber EN PLEIN MILIEU d'une
// écriture (« lecture déchirée »). `compteurEcriture` protège contre ça, à la manière
// d'un seqlock classique :
//   - l'écrivain l'incrémente à IMPAIR avant de toucher un seul champ, écrit tout, puis
//     l'incrémente à PAIR une fois fini ;
//   - le lecteur lit le compteur, lit les champs, relit le compteur ; si les deux lectures
//     diffèrent OU si le compteur lu est impair, la lecture est déchirée — il relit.
// Aucun champ n'a donc besoin d'être atomique individuellement : le compteur encadre tout.
//
// ── PAS DE PADDING LAISSÉ AU HASARD DU COMPILATEUR ──────────────────────────────────
//
// `#pragma pack(1)` ici, `#[repr(C, packed)]` côté Rust (miroir dans
// `tessera-core/voix-audio/src/partage_memoire.rs`, dépôt Tessera) : deux compilateurs
// différents (MSVC, rustc) sur la MÊME plateforme s'accordent déjà sur l'alignement naturel
// d'un `struct` C, mais rien n'oblige à leur faire confiance là-dessus pour un contrat
// d'ABI entre deux processus — le figer explicitement rend le format un FAIT écrit, pas une
// coïncidence de deux valeurs par défaut qui se trouvent être identiques aujourd'hui.
//
// ── LE TEST DE DISPOSITION ──────────────────────────────────────────────────────────
//
// `static_assert` ci-dessous, côté C++ : si un champ est ajouté/retiré/réordonné sans
// toucher le commentaire de taille, LA COMPILATION ÉCHOUE. Le miroir Rust porte le MÊME
// test sur les MÊMES tailles littérales — un dérive d'un seul côté casse SON build, pas
// l'autre en silence (c'est tout l'intérêt : « un test de disposition de chaque côté »,
// plan §Lot 4, V4.1).
// =====================================================================================

#include <cstddef>
#include <cstdint>

namespace Tessera::Voix
{

/// Version du FORMAT de cette structure (pas celle du jeu ni du launcher). À incrémenter à
/// chaque changement de disposition — le launcher peut alors refuser de lire un format
/// qu'il ne connaît pas au lieu d'interpréter des octets au hasard.
inline constexpr std::uint32_t kVersionStructure = 1;

/// Nombre maximal d'avatars distants portés par la structure. 64 est large pour un
/// plafond de voix de 8 (V3.3) : cette liste porte tous les avatars AFFICHÉS, pas
/// seulement ceux qui parlent — le launcher choisit qui atténuer/spatialiser parmi eux.
inline constexpr std::size_t kAvatarsMax = 64;

#pragma pack(push, 1)

/// Un avatar distant tel qu'affiché À L'ÉCRAN par CE client — pas la position serveur, qui
/// peut différer de plusieurs dizaines de ms (V5.3, S21) : l'auditeur doit entendre ce
/// qu'il VOIT, pas ce qu'un fil réseau affirme au même instant.
struct AvatarPartage
{
    /// Le cid (ClientId, u64 — voir `tessera-core/voix/src/paquet.rs`, dépôt Tessera, pour
    /// pourquoi ce n'est PAS un u32) sous lequel cet avatar est identifié partout ailleurs :
    /// `PlayerState.id()` du protocole, celui que le locuteur apprend de lui-même par
    /// `HealthSync` (`mine = true`).
    std::uint64_t cid = 0;
    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 0.0f;
};
static_assert(sizeof(AvatarPartage) == 20, "disposition figée : 8 (cid) + 3*4 (position)");

/// L'état complet partagé à chaque image. Champs dans l'ordre d'écriture logique, pas
/// alphabétique — ça n'a aucune importance avec `pack(1)`, mais ça reste plus lisible.
struct EtatVoixPartage
{
    /// Format de la structure elle-même (`kVersionStructure`) — PAS une version du jeu.
    std::uint32_t version = kVersionStructure;

    /// Seqlock : impair pendant l'écriture, pair une fois stable. Voir l'en-tête du fichier.
    std::uint32_t compteurEcriture = 0;

    /// Incrémenté à chaque écriture (donc à chaque image) — c'est CE compteur, pas l'horloge
    /// murale, que le launcher observe pour détecter un jeu figé/fermé (règle R8 : couper
    /// l'émission si le battement se tait 500 ms, VM4/V4.6).
    std::uint32_t battementDeVie = 0;

    /// Le cid de l'auditeur (CE client), appris de lui-même par `HealthSync` (`mine = true`,
    /// spec §0 « L'identité »). Le launcher s'identifie ainsi sans redemander de session.
    std::uint64_t monCid = 0;

    /// 0/1, jamais `bool` : la taille d'un `bool` n'est garantie ni par le C++ ni par le
    /// FFI Rust au-delà d'« au moins 1 octet » — un entier de taille fixe est le seul choix
    /// qui ne dépend d'aucune convention de compilateur.
    std::uint32_t vAppuyee = 0;

    /// Position de l'auditeur (le joueur local), monde.
    float auditeurPositionX = 0.0f;
    float auditeurPositionY = 0.0f;
    float auditeurPositionZ = 0.0f;

    /// Orientation (vecteur avant) de l'auditeur. ÉCRITE mais NON CONSOMMÉE en mono (la
    /// voix minimale n'a pas de spatialisation) — gardée dès ce lot pour que le format
    /// n'ait pas à changer quand la spatialisation (lot 7 et au-delà) en aura besoin.
    float auditeurOrientationX = 0.0f;
    float auditeurOrientationY = 0.0f;
    float auditeurOrientationZ = 0.0f;

    /// Nombre d'entrées valides dans `avatars` (0..=kAvatarsMax). Les entrées au-delà ne
    /// sont pas nettoyées entre deux écritures — le lecteur DOIT s'arrêter à ce compte,
    /// jamais parcourir tout le tableau en espérant un `cid == 0` comme terminateur.
    std::uint32_t nombreAvatars = 0;

    /// Les avatars AFFICHÉS par ce client, dans un ordre non garanti.
    AvatarPartage avatars[kAvatarsMax]{};
};

#pragma pack(pop)

// 4 (version) + 4 (compteur) + 4 (battement) + 8 (monCid) + 4 (v) + 3*4 (position)
// + 3*4 (orientation) + 4 (nombreAvatars) = 52, plus 64 avatars de 20 octets = 1280.
// Total 1332 octets — l'estimation « ~1,4 Ko » du plan (comptée sur un cid u32) est un peu
// EN DESSOUS de ce nombre EXACT : le cid en u64 (voir `paquet.rs`, correction du
// 2026-09-29) ajoute 4 octets par avatar, soit 256 octets sur les 64. Le nombre qui compte
// est celui-ci, mesuré par le compilateur, pas l'estimation du plan.
static_assert(sizeof(EtatVoixPartage) == 1332, "disposition figée à 1332 octets — voir le calcul ci-dessus");

} // namespace Tessera::Voix

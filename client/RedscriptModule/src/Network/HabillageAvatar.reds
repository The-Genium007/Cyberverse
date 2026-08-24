module Cyberverse.Network.Managers

// ⛔ L'HABILLAGE NE SE FAIT PLUS ICI — et ce fichier sert maintenant à MESURER le mélange.
//
// L'habillage de l'avatar distant a fini par marcher (F-PLY-286, 2026-08-24), mais dans l'ASSET :
// les vêtements sont des composants de `avatar_distant_ma.ent`, posés par
// `tools/re-probe/entites/habiller-avatar.py`. Rien ne se joue plus à l'exécution.
//
// ── LES TROIS VOIES ESSAYÉES ICI, ET POURQUOI AUCUNE NE TIENT ────────────────────────────────
//
//   1. **L'ÉQUIPEMENT** (`GiveItem` + `AddItemToSlot`) — 🔴 impasse F-PLY-275. L'appel met en file
//      une naissance d'entité d'item qui n'aboutit **jamais**, sur un pantin de photomode enrichi
//      comme sur un pantin de passant, avec ou sans `EquipmentSystemPlayerData`.
//   2. **DEMANDER UNE APPARENCE APRÈS LA NAISSANCE** — l'ordre PREND et le corps DISPARAÎT
//      (F-PLY-283).
//   3. **RIEN NE SE DÉCIDE APRÈS LA CONSTRUCTION** — la loi générale (F-PLY-191).
//
// ⚠️ Ne pas rouvrir ces voies sans une mesure neuve : elles ont coûté sept lancements.
//
// ── ⭐ CE QUE CE FICHIER MESURE MAINTENANT : D'OÙ VIENT LE MÉLANGE ───────────────────────────
//
// Le corps d'un joueur distant montre un mélange avec le V du SPECTATEUR — Lucas joue REDDA et voit
// la coiffure de REDDA sur l'avatar de LUCAS1 (F-PLY-290). Deux causes sont déjà écartées, hors jeu
// (F-PLY-291) : le transport (la charge est copiée en profondeur, `size × 16` octets) et les
// données (les deux blobs stockés n'ont AUCUNE paire en commun).
//
// Reste un phénomène d'exécution. Cette sonde le tranche **sans l'œil de personne** : elle lit
// l'apparence RÉELLEMENT posée sur chaque composant de l'avatar, et la journalise en hexadécimal.
// Hors jeu, on confronte ces valeurs aux deux blobs — celui du joueur qu'on affiche et celui du
// spectateur. Une valeur qui vient du second est la preuve du mélange, et elle NOMME la pièce.
//
// ⚠️ ON LIT, ON N'ÉCRIT PAS. Écrire `meshAppearance` est une impasse mesurée (F-PLY-191) ; la lire
// ne l'est pas, et c'est exactement la différence entre une sonde et une tentative.
//
//   grep "\[Melange\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log

func TesseraJournalMelange(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Melange] " + texte);
    }
}

// Le corps derrière un `EntityID`, quelle que soit la voie qui l'a fait naître.
//
// ⚠️ `DynamicEntitySystem.GetEntity` ne connaît pas les corps de la voie enrichie : ils ne sont pas
// nés par lui. `GameInstance.FindEntityByID`, lui, interroge le monde.
func TesseraCorpsDeLEntite(cible: EntityID) -> ref<Entity> {
    let entite = GameInstance.GetDynamicEntitySystem().GetEntity(cible);
    if !IsDefined(entite) {
        entite = GameInstance.FindEntityByID(GetGameInstance(), cible);
    }
    return entite;
}

// Appelée par `PiloterAvatar` (C++) par créneaux bornés — le seul chemin qui atteint les corps de la
// voie enrichie (F-PLY-278). Rend `true` quand le relevé est fait : l'appelant cesse de repasser.
func TesseraHabillerLeCorps(cible: EntityID, passe: Uint32) -> Bool {
    // ⚠️ On attend quelques passes : un corps qui vient de naître n'a pas fini de monter ses
    // composants, et relever trop tôt donnerait une liste courte qu'on lirait comme une absence.
    // C'est la même leçon que la fenêtre morte de l'habillage (F-PLY-279).
    if passe < 3u {
        return false;
    }
    let corps = TesseraCorpsDeLEntite(cible);
    if !IsDefined(corps) {
        return false; // pas encore né — l'appelant repassera
    }

    // Le V du SPECTATEUR, relevé dans la même frame. Sans ce témoin, les valeurs de l'avatar ne se
    // comparent à rien : c'est lui qui transforme une liste de hachages en verdict.
    let joueur = GameInstance.GetPlayerSystem(GetGameInstance())
        .GetLocalPlayerControlledGameObject();

    TesseraJournalMelange(s"=== avatar \(EntityID.ToDebugString(cible)) ===");
    TesseraReleverApparences(corps, "avatar");
    if IsDefined(joueur) {
        TesseraReleverApparences(joueur, "TEMOIN-spectateur");
    }
    return true;
}

// Journalise, pour chaque composant de mesh, le nom et l'apparence RÉELLEMENT posée.
//
// ⚠️ Le `meshAppearance` est un `CName` : son hachage est directement comparable aux valeurs des
// paires du blob `TSV1`, qui sont des hachages de `CName`. La confrontation se fait donc hors jeu,
// sans interpréter quoi que ce soit ici.
func TesseraReleverApparences(entite: ref<Entity>, etiquette: String) -> Void {
    let composants = entite.GetComponents();
    let n = 0;
    let i = 0;
    while i < ArraySize(composants) {
        let mesh = composants[i] as entSkinnedMeshComponent;
        if IsDefined(mesh) {
            TesseraJournalMelange(
                s"  \(etiquette) · \(NameToString(mesh.name)) = \(NameToString(mesh.meshAppearance))");
            n += 1;
        }
        i += 1;
    }
    TesseraJournalMelange(s"  \(etiquette) : \(n) composant(s) de mesh sur \(ArraySize(composants))");
}

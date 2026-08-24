module Cyberverse.Network.Managers

// ⛔ L'HABILLAGE NE SE FAIT PLUS ICI — ce fichier MESURE le mélange, et prépare son correctif.
//
// Les vêtements sont des composants de `avatar_distant_ma.ent` depuis F-PLY-286. Ce qui reste ici
// est l'instrument qui a permis de comprendre pourquoi les avatars se mélangent.
//
// ── LES TROIS IMPASSES DE CE FICHIER, à ne pas rouvrir sans mesure neuve ─────────────────────
//
//   1. **L'ÉQUIPEMENT** — 🔴 F-PLY-275 : l'entité d'item ne naît jamais.
//   2. **L'APPARENCE APRÈS LA NAISSANCE** — F-PLY-283 : l'ordre prend, le corps disparaît.
//   3. **RIEN NE SE DÉCIDE APRÈS LA CONSTRUCTION** — F-PLY-191, la loi qui explique les deux.
//
// ── ⭐ CE QU'ON SAIT DU MÉLANGE, ET CE QUE CE FICHIER MESURE ─────────────────────────────────
//
// **Chaque spectateur imprime son propre V sur l'avatar qu'il regarde** (F-PLY-296, prouvé en
// réduisant la charge à une seule paire : la coiffure et le teint du spectateur restaient sur le
// corps d'en face). Le corps est donc bâti à partir de notre charge **et** de l'état de
// customisation, qui est un singleton portant le V du joueur local.
//
// ⚠️ Et « couvrir tous les slots du voisin » ne suffirait pas : les deux esthétiques de test ne
// partagent que **8 slots sur 21 et 18** — les slots encodent le sexe. Treize slots du spectateur
// resteraient non surchargés, et ce sont exactement ceux qui fuient (coiffure, organes).
//
// D'où la question que ce fichier a posée : **peut-on faire taire l'état ?**
//
// ⛔ **RÉPONSE : NON, ET C'EST MESURÉ (F-PLY-297).** `ClearState()` rend **`false`** en cours de
// partie — elle refuse — et le joueur local est rigoureusement inchangé : 28 composants de mesh
// avant, 28 après ; 26 avant, 26 après sur l'autre instance. La méthode n'est utilisable que dans
// le parcours du créateur de personnage.
//
// La voie est donc fermée, et elle l'a été en un lancement, sans rien casser. La sonde reste ici
// parce qu'elle est **inerte** et qu'elle documente le refus : quelqu'un aura l'idée à nouveau.
//
//   grep "\[Melange\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log

func TesseraJournalMelange(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Melange] " + texte);
    }
}

// Le corps derrière un `EntityID`, quelle que soit la voie qui l'a fait naître.
func TesseraCorpsDeLEntite(cible: EntityID) -> ref<Entity> {
    let entite = GameInstance.GetDynamicEntitySystem().GetEntity(cible);
    if !IsDefined(entite) {
        entite = GameInstance.FindEntityByID(GetGameInstance(), cible);
    }
    return entite;
}

// Appelée par `PiloterAvatar` (C++) par créneaux bornés — le seul chemin qui atteint les corps de la
// voie enrichie (F-PLY-278). Rend `true` quand le relevé est fait.
func TesseraHabillerLeCorps(cible: EntityID, passe: Uint32) -> Bool {
    // ⚠️ On attend quelques passes : un corps qui vient de naître n'a pas fini de monter ses
    // composants, et relever trop tôt donnerait une liste courte qu'on lirait comme une absence.
    if passe < 3u {
        return false;
    }
    let corps = TesseraCorpsDeLEntite(cible);
    if !IsDefined(corps) {
        return false;
    }
    TesseraJournalMelange(s"=== avatar \(EntityID.ToDebugString(cible)) ===");
    TesseraReleverApparences(corps, "avatar");

    // ── ⭐ LA SONDE DU JOUR : VIDER L'ÉTAT CASSE-T-IL LE JOUEUR LOCAL ? ──────────────────────
    //
    // Une seule fois par session — le drapeau vit dans le système réseau, pas ici, parce que cette
    // fonction est rappelée pour chaque avatar et qu'on ne veut pas vider l'état vingt fois.
    TesseraSonderVidageEtat();
    return true;
}

// Relève, pour chaque composant de mesh, le nom et l'apparence RÉELLEMENT posée.
//
// ⚠️ Le `meshAppearance` est un `CName` ; c'est le contenu réel de l'entité, pas ce qu'on croit lui
// avoir donné. Lire ce champ est fiable ; l'écrire est une impasse (F-PLY-191).
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

// ⚠️ SONDE DESTRUCTIVE POTENTIELLE — elle appelle `ClearState()` sur le système de customisation.
//
// C'est le geste dont on veut savoir s'il est sûr : si l'état peut être vidé sans abîmer le V du
// joueur local, alors on tient la voie pour empêcher ce V de contaminer les avatars des voisins
// (F-PLY-296). On relève donc le joueur AVANT et APRÈS, dans la même session, et la comparaison des
// deux listes est le verdict.
//
// ⚠️ Une seule fois par session : vider l'état à chaque avatar serait une mesure ininterprétable, et
// un martèlement d'un système partagé.
func TesseraSonderVidageEtat() -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if !IsDefined(reseau) || !reseau.Tessera_PremierVidageEtat() {
        return;
    }
    let joueur = GameInstance.GetPlayerSystem(GetGameInstance())
        .GetLocalPlayerControlledGameObject();
    if !IsDefined(joueur) {
        TesseraJournalMelange("sonde vidage : pas de joueur local, abandon");
        return;
    }
    let systeme = GameInstance.GetCharacterCustomizationSystem(GetGameInstance());
    if !IsDefined(systeme) {
        TesseraJournalMelange("sonde vidage : systeme de customisation INJOIGNABLE");
        return;
    }
    TesseraJournalMelange("--- sonde vidage : AVANT ClearState ---");
    TesseraReleverApparences(joueur, "joueur-AVANT");
    let vide = systeme.ClearState();
    TesseraJournalMelange(s"--- ClearState rend \(vide) ---");
    TesseraReleverApparences(joueur, "joueur-APRES");
}

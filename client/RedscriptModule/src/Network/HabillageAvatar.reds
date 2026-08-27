module Cyberverse.Network.Managers

// L'AVATAR DISTANT PORTE LES VÊTEMENTS DE L'INVENTAIRE DE SON JOUEUR.
//
// ⭐ Demande de Lucas, 2026-08-24 : *« il faut que ce soit les mêmes »*. Le serveur décide déjà ce
// qui est porté (`contenus.porte`) et l'envoie ; ce fichier est le dernier mètre.
//
// ── ⚠️ POURQUOI ON ALLUME, ET POURQUOI ON N'ÉQUIPE PAS ──────────────────────────────────────
//
// Équiper un pantin distant est une impasse **mesurée deux fois, par deux mécanismes distincts** :
// item réel (F-PLY-275) et item de prévisualisation façon `EquipmentEx` (F-PLY-304). Dans les deux
// cas `AddItemToSlot` rend `true`, le slot passe en « naissance d'entité en cours », et **il y
// reste indéfiniment** — neuf passes, `GetItemInSlot` toujours `null`. Le wiki de modding dit la
// même chose dans l'autre sens : sur un PNJ, les vêtements sont cuits dans l'apparence, **à la
// construction** (F-PLY-305), ce qui est exactement la loi F-PLY-191.
//
// Ce que la mesure autorise, c'est d'**éteindre et rallumer un composant déjà monté** — `Toggle`,
// vérifié en capture (F-PLY-306). D'où l'architecture, décidée le 2026-08-24 :
//
//     tous les vêtements du catalogue serveur sont CUITS dans l'entité, `isEnabled = 0` ;
//     ici, on ALLUME ceux que le serveur déclare portés.
//
// La cuisson est faite hors jeu par `tools/re-probe/entites/cuire-garde-robe.py`.
//
// ── ⭐ LA CONVENTION QUI ÉVITE TOUT FICHIER DE CORRESPONDANCE ────────────────────────────────
//
//     nom du composant  =  <appearanceName de TweakDB> + <m|w>
//     exemple           =  t1_tshirt_04_old_02_m
//
// Le client reçoit un `TweakDBID` d'item ; `appearanceName` est un flat de ce record, lisible au
// script. Il n'y a donc **aucune table à embarquer** — donc rien qui puisse diverger en silence de
// l'entité livrée. Et le préfixe (`t1_`, `t2_`, `l1_`, `s1_`) est conservé, ce que le moteur exige
// pour reconnaître un vêtement (« Garment Support »).
//
// ⚠️ Un vêtement peut avoir PLUSIEURS meshes (une veste en a deux) : ils sont suffixés `__1`,
// `__2`. On allume donc tout ce qui **commence par** la clé — jamais ce qui lui est égal.
//
// ── ⚠️ LES SOUS-VÊTEMENTS SUIVENT LE RÉGLAGE DE NUDITÉ ──────────────────────────────────────
//
// Consigne de Lucas, 2026-08-24 : *« il y a un réglage sur le jeu pour la nudité et j'aimerais que
// ça respecte ce réglage »*. Le jeu l'expose : `IsNudityAllowed()` sur le système de customisation.
// Les sous-vêtements viennent de l'apparence de base (ils sont déjà montés et allumés) ; la règle
// est donc de les ÉTEINDRE uniquement si la nudité est permise ET que le joueur n'en porte pas.
//
//   grep "\[Habillage\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log

func TesseraJournalHabillage(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Habillage] " + texte);
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

// La clé de composant d'un item, pour une morphologie donnée.
//
// ⚠️ Rend `""` quand l'item n'a pas de `appearanceName` — ce qui arrive (nourriture, argent,
// cyberware). Un item sans apparence n'est pas une erreur : il n'a simplement rien à montrer.
func TesseraCleVetement(record: TweakDBID, corpsMasculin: Bool) -> String {
    let app = TweakDBInterface.GetCName(record + t".appearanceName", n"");
    if Equals(app, n"") {
        return "";
    }
    return NameToString(app) + (corpsMasculin ? "m" : "w");
}

// Appelée par `PiloterAvatar` (C++) par créneaux bornés — le seul chemin qui atteint les corps de
// la voie enrichie (F-PLY-278). Rend `true` quand il n'y a plus rien à faire.
func TesseraHabillerLeCorps(cible: EntityID, passe: Uint32) -> Bool {
    // ⚠️ On attend quelques passes : un corps qui vient de naître n'a pas fini de monter ses
    // composants, et compter trop tôt donnerait une liste courte qu'on lirait comme une absence.
    if passe < 2u {
        return false;
    }
    let corps = TesseraCorpsDeLEntite(cible) as GameObject;
    if !IsDefined(corps) {
        return false;
    }
    let reseau = GameInstance.GetNetworkGameSystem();
    if !IsDefined(reseau) {
        return false;
    }

    let combien = reseau.Tessera_NombreDeVetements(cible);
    let masculin = reseau.Tessera_AvatarCorpsMasculin(cible);

    // Les clés à allumer, calculées une fois.
    let cles: array<String>;
    let i = 0;
    while i < combien {
        let cle = TesseraCleVetement(reseau.Tessera_VetementDeLEntite(cible, i), masculin);
        if NotEquals(cle, "") {
            ArrayPush(cles, cle);
        }
        i += 1;
    }

    // ── LA POSE ────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ On parcourt les composants UNE fois et on décide pour chacun, plutôt que de chercher
    // chaque clé dans la liste : le coût est linéaire au lieu de quadratique, et à des dizaines de
    // joueurs autour, ce parcours tourne pour chaque avatar à chaque créneau.
    let composants = corps.GetComponents();
    let allumes = 0;
    let j = 0;
    while j < ArraySize(composants) {
        let nom = NameToString(composants[j].GetName());
        let k = 0;
        let voulu = false;
        while k < ArraySize(cles) {
            if StrBeginsWith(nom, cles[k]) {
                voulu = true;
            }
            k += 1;
        }
        // On ne touche QU'aux composants de garde-robe : tout ce qui ne commence pas par un
        // préfixe de vêtement appartient au corps, et l'éteindre le mutilerait.
        if voulu && !composants[j].IsEnabled() {
            composants[j].Toggle(true);
            allumes += 1;
        }
        j += 1;
    }

    // ── LES SOUS-VÊTEMENTS SUIVENT LE RÉGLAGE DE NUDITÉ ────────────────────────────────────
    //
    // Consigne de Lucas, 2026-08-24 : *« les sous-vêtements doivent être activés (…) et il y a un
    // réglage sur le jeu pour la nudité, j'aimerais que ça respecte ce réglage »*.
    //
    // Les sous-vêtements viennent de l'apparence de base : ils sont **déjà montés et allumés**.
    // La règle n'a donc qu'un seul cas à traiter — les ÉTEINDRE — et il est doublement gardé :
    //
    //     on éteint  ⟺  la nudité est permise  ET  aucun vêtement ne couvre cette zone
    //
    // ⚠️ LE DÉFAUT EST « HABILLÉ », ET C'EST VOULU. Si le système de customisation est injoignable,
    // si le réglage ne se lit pas, si la liste des vêtements est vide — dans tous ces cas on ne
    // touche à rien. Une panne ne doit jamais pouvoir déshabiller quelqu'un devant les autres.
    let systeme = GameInstance.GetCharacterCustomizationSystem(GetGameInstance());
    if IsDefined(systeme) && systeme.IsNudityAllowed() {
        // Une zone est couverte si un vêtement demandé porte son préfixe. Le préfixe est celui de
        // la nomenclature CDPR : `t` pour le torse, `l` pour les jambes.
        let torseCouvert = false;
        let jambesCouvertes = false;
        let z = 0;
        while z < ArraySize(cles) {
            if StrBeginsWith(cles[z], "t") { torseCouvert = true; }
            if StrBeginsWith(cles[z], "l") { jambesCouvertes = true; }
            z += 1;
        }
        let eteints = 0;
        let c = 0;
        while c < ArraySize(composants) {
            let nom = NameToString(composants[c].GetName());
            // ⚠️ On ne vise QUE les sous-vêtements de l'apparence de base, nommés par CDPR :
            // `__bra`, `__panties`, `__boxers`. Jamais un préfixe seul — `t1_` désigne aussi les
            // t-shirts, et on éteindrait la tenue qu'on vient d'allumer.
            let estSousVetement = StrContains(nom, "__bra") || StrContains(nom, "__panties")
                || StrContains(nom, "__boxers");
            let zoneLibre = (StrBeginsWith(nom, "t") && !torseCouvert)
                || (StrBeginsWith(nom, "l") && !jambesCouvertes);
            if estSousVetement && zoneLibre && composants[c].IsEnabled() {
                composants[c].Toggle(false);
                eteints += 1;
            }
            c += 1;
        }
        if eteints > 0 {
            TesseraJournalHabillage(s"  nudite permise : \(eteints) sous-vetement(s) eteint(s)");
        }
    }

    // ── ⛔ LA SONDE DE WRAPPERS D ARME A ETE RETIREE — elle etait FAUSSE ET NUISIBLE ────────
    //
    // Elle poussait `Wea_Fists` et `WeaponRight` a 1.0 pour voir si l avatar prenait une garde.
    //
    // 1. **Elle ne marchait pas.** Verdict de Lucas, 2026-08-25 : « ca ne veut pas se mettre en
    //    garde ». F-PLY-311 est REFUTE — j avais lu un changement de posture dans deux captures
    //    prises a trente minutes d ecart, ce qui n est pas un A/B.
    // 2. **⚠️ Et elle CASSAIT le corps.** Capture du 2026-08-26 : l avatar n a plus ni visage ni
    //    mains, les manches s arretent sur du vide. Tenir une arme change le JEU DE MESHES DE BRAS
    //    dans le rig CDPR ; forcer la couche sans arme reelle bascule vers un jeu qui n est pas la.
    //
    // ⚠️ LA LECON, au-dela de cette sonde : un poids de wrapper n est pas une ecriture inoffensive.
    // Il selectionne des ressources. Une sonde d animation doit donc etre RETIREE des son verdict
    // pris — pas laissee en place « au cas ou ».

    TesseraJournalHabillage(
        s"avatar \(EntityID.ToDebugString(cible)) : \(ArraySize(cles)) vetement(s) demande(s), \(allumes) composant(s) allume(s) [corps \(masculin ? "M" : "F")]");

    // ⚠️ On rend `true` dès qu'on a fait le tour : rallumer ce qui est déjà allumé ne sert à rien,
    // et laisser la boucle tourner ferait relire tous les composants de tous les avatars à chaque
    // créneau — pour rien, et à l'échelle de dizaines de joueurs ça se paierait.
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
            TesseraJournalHabillage(
                s"  \(etiquette) · \(NameToString(mesh.name)) = \(NameToString(mesh.meshAppearance))");
            n += 1;
        }
        i += 1;
    }
    TesseraJournalHabillage(s"  \(etiquette) : \(n) composant(s) de mesh sur \(ArraySize(composants))");
}

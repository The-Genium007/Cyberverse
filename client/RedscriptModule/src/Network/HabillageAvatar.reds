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
    let app = TesseraApparenceNue(record);
    if Equals(app, "") {
        return "";
    }
    return app + (corpsMasculin ? "m" : "w");
}

// Le `appearanceName` d'un item, SANS la lettre de morphologie.
//
// ⭐ POURQUOI IL FAUT LES DEUX FORMES. Un composant cuit se nomme `<apparence><m|w>`, parce qu'un
// vêtement est genré à la source. Un EFFET, lui, ne l'est pas : `eye_glow_blue` est déclaré une
// seule fois sur l'entité, pour les deux morphologies. Suffixer son nom le rendrait introuvable.
func TesseraApparenceNue(record: TweakDBID) -> String {
    let app = TweakDBInterface.GetCName(record + t".appearanceName", n"");
    if Equals(app, n"") {
        return "";
    }
    return NameToString(app);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// LES EFFETS PILOTÉS PAR LE SERVEUR — un état visuel qui n'est PAS un mesh
// ═══════════════════════════════════════════════════════════════════════════════════════════════
//
// Demande de Lucas, 2026-09-05 : *« quand quelqu'un reçoit un appel, il a les yeux qui passent
// bleu »*.
//
// ⛔ LA RECETTE DES VÊTEMENTS NE S'APPLIQUE PAS ICI, et c'est structurel. Le globe oculaire n'est
// pas un composant de notre entité d'avatar : il vient de la charge d'esthétique, assemblé à la
// naissance. Il n'y a donc rien à cuire, et rien à allumer.
//
// ⭐⭐ MAIS L'EFFET EST DÉJÀ LÀ. L'entité d'avatar descend du pantin photomode de V, et porte à ce
// titre **177 descripteurs d'effets** — dont `eye_glow_blue`, `eye_glow_gold`, `eye_glow_purple`,
// `eye_glow_red` et `eye_flare` (relevé du 2026-09-06 sur `avatar_distant_wa.ent`). Un effet
// déclaré se joue par son nom, et c'est tout ce qu'il faut.
//
// ⚠️ CETTE LISTE EST FERMÉE, ET C'EST DÉLIBÉRÉ. On pourrait « tenter » de jouer n'importe quelle
// clé orpheline comme un effet : l'appel ne planterait pas, il ne ferait rien. Mais on perdrait le
// seul instrument qui dit qu'un visuel manque — le journal `NON CUIT`, qui a nommé lui-même les
// trois composants à cuire le 2026-09-05. Une clé inconnue doit rester une clé inconnue.
//
// Les 145 noms déclarés sur l'entité (`status_*`, `trail_*`, `strong_arms_block`…) sont autant
// d'états visuels disponibles sans une ligne de cuisson. On les ouvre un par un, en les nommant.
func TesseraEffetsPilotes() -> array<CName> {
    return [n"eye_glow_blue", n"eye_glow_gold", n"eye_glow_purple", n"eye_glow_red"];
}

// Vrai si ce composant est une pièce de GARDE-ROBE — donc à nous — et faux s'il appartient au CORPS.
//
// ⚠️ CETTE FONCTION DÉCIDE CE QU'ON A LE DROIT D'ÉTEINDRE. Se tromper ne dégrade pas : ça mutile
// l'avatar (visage manquant, manches sur du vide — vécu le 2026-08-26 avec les wrappers d'arme).
// Elle n'est donc pas devinée d'après la nomenclature CDPR : elle est tirée du relevé des 54
// composants d'un avatar vivant (`avatar_corps`, 2026-09-03).
//
// Ce relevé donne un discriminant net, et un seul :
//
//     VÊTEMENTS   t1_tshirt_04_old_02_w · t2_coat_04_old_01_w__1 · l1_pants_06_old_03_w
//                 s1_boots_05_old_02_w · t2_jacket_19_basic_02_w__3   → finissent par `_m`/`_w`
//     CORPS       t0_000_pma_base__full_shadow · l0_000_pwa_base__cs_flat · hh_040_wa__pixie_bob
//                 heb_000_pwa__basehead · i0_000_pwa_base__genitals_none · a0_000_pwa_base_nails_l
//                 t1_057_pwa_tank__bra0730 · l1_047_pwa_shorts__panties7731  → AUCUN ne finit ainsi
//
// ⭐ Le suffixe `m`/`w` n'est pas décoratif : c'est celui que `TesseraCleVetement` AJOUTE lui-même
// à l'`appearanceName`. On reconnaît donc exactement ce qu'on sait fabriquer — pas une famille de
// noms qu'on espère.
//
// ⚠️ Un même vêtement monte PLUSIEURS meshes, suffixés `__1`, `__2`… — d'où les deux formes
// contenant. Les sous-vêtements de l'apparence de base (`__bra`, `__panties`, `__boxers`) sont
// exclus par construction, et c'est voulu : ils ont leur propre règle, plus bas, adossée au
// réglage de nudité.
func TesseraEstGardeRobe(nom: String) -> Bool {
    return StrEndsWith(nom, "_m") || StrEndsWith(nom, "_w")
        || StrContains(nom, "_m__") || StrContains(nom, "_w__");
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
    //
    // ⭐ CETTE LISTE EST LA SEULE VÉRITÉ. Elle vient du SERVEUR, et l'avatar distant n'affichera
    // rien d'autre : ce qui n'y est pas est éteint plus bas, quelle qu'en soit l'origine. Un joueur
    // qui se moderait des vêtements les verrait chez lui et **chez personne d'autre** — c'est la
    // règle posée par Lucas le 2026-09-03, et c'est ce qui empêche un client de décider seul de ce
    // que les autres voient.
    let cles: array<String>;
    // Parallèle à `cles` : cette clé a-t-elle trouvé au moins un composant ? Une clé orpheline
    // désigne un vêtement que le serveur connaît mais que notre entité ne sait pas montrer.
    let trouvees: array<Bool>;
    // Parallèle à `cles` : le nom d'apparence NU, sans la lettre de morphologie. C'est lui que le
    // vocabulaire d'effets emploie — voir `TesseraEffetsPilotes`.
    let apparences: array<String>;
    let i = 0;
    while i < combien {
        let record = reseau.Tessera_VetementDeLEntite(cible, i);
        let cle = TesseraCleVetement(record, masculin);
        if NotEquals(cle, "") {
            ArrayPush(cles, cle);
            ArrayPush(apparences, TesseraApparenceNue(record));
            ArrayPush(trouvees, false);
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
    let retires = 0;
    let j = 0;
    while j < ArraySize(composants) {
        let nom = NameToString(composants[j].GetName());
        let k = 0;
        let voulu = false;
        while k < ArraySize(cles) {
            if StrBeginsWith(nom, cles[k]) {
                voulu = true;
                trouvees[k] = true;
            }
            k += 1;
        }
        // On ne touche QU'aux composants de garde-robe : tout ce qui ne commence pas par un
        // préfixe de vêtement appartient au corps, et l'éteindre le mutilerait.
        if voulu && !composants[j].IsEnabled() {
            composants[j].Toggle(true);
            allumes += 1;
        } else {
            // ── ⭐⭐⭐ LE RETRAIT, QUI MANQUAIT — cette passe était ADDITIVE SEULE ─────────
            //
            // ⚠️ MESURÉ le 2026-09-03, et ça explique le symptôme d'origine. Le fil du hot-swap
            // marche de bout en bout — retirer un vêtement chez A arrive chez B, et cette
            // fonction est bien rappelée (4 → 3 vêtements demandés, lu au journal). Et pourtant
            // le vêtement RESTAIT à l'écran, parce qu'aucune ligne de ce fichier ne l'éteignait.
            // On ne pouvait que s'habiller, jamais se déshabiller : la tenue ne faisait que
            // s'empiler, ce que Lucas décrivait dès le premier jour.
            //
            // ⭐ C'est le cran d'après « accepté ≠ exécuté » : ici TOUT le fil était bien exécuté,
            // et le dernier consommateur n'avait simplement pas la branche. Un compteur posé sur
            // l'émetteur aurait confirmé la panne comme il aurait confirmé le bon fonctionnement.
            if !voulu && composants[j].IsEnabled() && TesseraEstGardeRobe(nom) {
                composants[j].Toggle(false);
                retires += 1;
            }
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
        s"avatar \(EntityID.ToDebugString(cible)) : \(ArraySize(cles)) vetement(s) demande(s), \(allumes) allume(s), \(retires) retire(s) [corps \(masculin ? "M" : "F")]");

    // ── ⚠️ LE PLAFOND DEVIENT MESURÉ, AU LIEU DE RESTER INDISCERNABLE D'UNE PANNE ──────────────
    //
    // Notre entité d'avatar ne porte que DOUZE racines de vêtement, cuites une fois pour toutes
    // (`avatar_distant_ma.ent` / `_wa.ent`). Un vêtement que le serveur connaît mais qui n'y est pas
    // ne s'affichera JAMAIS — et jusqu'ici, en silence.
    //
    // ⭐ C'est acceptable par décision : « on accepte de ne rien afficher s'il y a un élément non
    // compatible » (Lucas, 2026-09-03). Ce qui ne l'est pas, c'est de ne pas le SAVOIR. Sans cette
    // ligne, « le manteau n'apparaît pas » a deux causes indiscernables — le fil est cassé, ou le
    // vêtement n'est pas cuit — et on repart chercher la panne du côté du réseau.
    //
    // ⚠️ On journalise la CLÉ, pas l'item : c'est elle qui manque à l'entité, et c'est elle qu'il
    // faudra cuire. Le nom de l'item ne dit pas quel mesh il faudrait ajouter.
    // ── LES EFFETS, AVANT LE RELEVÉ DES ORPHELINES ─────────────────────────────────────────────
    //
    // Un effet piloté n'a pas de composant à allumer : il se joue. On le fait AVANT de compter les
    // clés orphelines, et on marque la clé comme trouvée — sinon un état visuel qui fonctionne
    // serait journalisé comme un visuel manquant, à chaque passe, chez chaque observateur.
    let effets = TesseraEffetsPilotes();
    let e = 0;
    while e < ArraySize(effets) {
        let nomEffet = NameToString(effets[e]);
        let veut = false;
        let a = 0;
        while a < ArraySize(apparences) {
            if Equals(apparences[a], nomEffet) {
                veut = true;
                trouvees[a] = true;
            }
            a += 1;
        }
        if veut {
            GameObjectEffectHelper.StartEffectEvent(corps, effets[e]);
        } else {
            // ⚠️ ARRÊT INCONDITIONNEL, et c'est ce qui rend la passe idempotente. On ne sait pas
            // si l'effet joue — aucune API ne le dit — donc on ne peut pas conditionner l'arrêt à
            // son état. Arrêter un effet qui ne joue pas est sans conséquence ; NE PAS arrêter
            // celui qui joue laisserait les yeux allumés pour toujours après la fin de l'appel.
            // C'est le pendant exact du retrait de vêtement qui manquait le 2026-09-03 : une passe
            // additive seule ne sait pas revenir en arrière.
            GameObjectEffectHelper.StopEffectEvent(corps, effets[e]);
        }
        e += 1;
    }

    let orphelines = "";
    let o = 0;
    while o < ArraySize(cles) {
        if !trouvees[o] {
            orphelines += (Equals(orphelines, "") ? "" : ", ") + cles[o];
        }
        o += 1;
    }
    if NotEquals(orphelines, "") {
        TesseraJournalHabillage(
            s"  ⚠ NON CUIT dans l'entite — invisible pour les autres : \(orphelines)");
    }

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

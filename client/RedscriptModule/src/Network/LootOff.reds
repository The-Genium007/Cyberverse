module Cyberverse.Network.Managers

// On ne récupère RIEN sur un corps. Ni PNJ, ni joueur.
//
// Décision de Lucas, 2026-08-08 : « je ne veux pas que les gens récupèrent des choses en tuant
// d'autres personnages ». Ce n'est pas un réglage d'équilibrage, c'est une règle d'autorité : tout
// objet obtenu par le client serait obtenu SANS le serveur, donc dupliqué, invisible à l'arbitrage
// et impossible à révoquer. L'inventaire est une architecture serveur (F-MND-042), et tant que le
// serveur n'arbitre pas le butin, la seule position tenable est zéro butin client.
//
// ── Deux canaux, deux entonnoirs ──────────────────────────────────────────────────────────────
//
// 1. **Le corps lui-même.** `ScriptedPuppet.UpdateLootInteraction` est le SEUL endroit du jeu qui
//    allume la couche d'interaction `'Loot'` (`scriptedPuppet.script:4589`, vérifié : aucune autre
//    occurrence dans tous les scripts décompilés). Ses trois appelants sont internes à la classe —
//    l'attachement (`:674`), l'événement d'accessibilité (`:2642`) et le délai posé à la mort
//    (`:2820` → `:3339`). Un entonnoir au sens de `tessera-prise-autorite`, pas un point où ça se
//    voit : la couche ne s'allume plus, par aucune voie.
//
// 2. **L'arme lâchée en mourant.** `DropHeldItems` (`:3092`), appelée depuis un seul site — le
//    statut portant le tag `DropHeldItems` (`:2501`). Sans elle, l'arme reste dans les mains du
//    cadavre au lieu de tomber au sol en objet ramassable. C'est le butin le plus évident qu'une
//    mise à mort rapporte, et il n'aurait pas traversé la porte n°1.
//
// ⚠️ **On ne VIDE pas l'inventaire d'un PANTIN, et c'est délibéré.** « Rien dedans » se lit
// naturellement comme « effacer le contenu du corps » — sauf que l'inventaire d'un PNJ EST son
// équipement : le vider le déshabille et le désarme. Fermer l'accès donne le résultat voulu sans
// produire une ville de PNJ nus. ⚠️ Cette contrainte est PROPRE AUX PANTINS — voir plus bas, elle
// ne vaut pas pour une caisse, et confondre les deux mènerait à la mauvaise solution des deux côtés.
//
// ⚠️ **Ce qui n'est PAS couvert**, et qui relève d'une autre décision : le désarmement au TIR
// (`ScriptedPuppet.DropWeaponFromSlot`, appelée par la réaction de coup et par les finisher) fait
// tomber une arme au sol sans passer par `DropHeldItems`. Deux sites, faciles à couper — mais ça
// supprime aussi le « lui faire sauter l'arme des mains », qui est du jeu, pas du butin.
//
// Entonnoirs, unicité et fausses pistes : **F-PNJ-135** (`docs/connaissances/moteur-pnj-foule.md`).

// `wrappedMethod()` n'est PAS appelée : son corps entier est `EnableInteraction('Loot', <condition>)`,
// et on veut justement que la condition n'existe plus. On garde `@wrapMethod` plutôt que
// `@replaceMethod` pour rester composable avec un autre mod qui toucherait la même méthode.
@wrapMethod(ScriptedPuppet)
protected func UpdateLootInteraction() -> Void {
    this.EnableInteraction(n"Loot", false);
}

// `false` = « rien n'a été lâché », ce que l'appelant natif attend d'un pantin qui ne lâche rien —
// c'est exactement la valeur que rendait déjà le vanilla quand le record disait
// `DropsWeaponOnDeath() == false`. On ne fabrique pas un état que le jeu ne connaît pas.
//
// ⚠️ `@replaceMethod` ET NON `@wrapMethod`, parce que la méthode est `private` : le projet a mesuré
// qu'un `@wrapMethod` sur une méthode privée **compile et ne se déclenche jamais** (mémoire
// `native-ui-reinvocation-inventory`, rappelé dans `BACKLOG-INGAME.md` Q-UI2), là où
// `@replaceMethod` remplace le CORPS en place — les appelants existants exécutent bien le nôtre.
// C'est le même choix, pour la même raison, que `UiKitDeath.reds` sur `PopulateMenuItemList`.
// La première version de ce fichier utilisait `@wrapMethod` : elle était donc probablement inerte,
// et « le patch marche » ne le disait pas, puisque l'autre moitié (l'invite de fouille) marchait.
@replaceMethod(ScriptedPuppet)
private func DropHeldItems() -> Bool {
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════
// LE MONDE AUSSI — caisses, casiers, planques, sacs, et l'objet posé par terre.
//
// Demande de Lucas, 2026-08-14 : « tous les objets sur la map — les coffres, les objets par
// terre — qu'on puisse les RETIRER de manière naturelle, que ça les rende inopérant, qu'un
// cendrier par terre soit enlevé, que tout ce qui est items ne puisse pas être récupéré. Sauf si
// plus tard on vient l'ajouter avec un système serveur. »
//
// Même règle d'autorité que ci-dessus, appliquée à l'autre moitié du problème — et de loin la plus
// abondante : le corps d'un PNJ est le canal le plus visible, Night City est un entrepôt à ciel
// ouvert. Un objet obtenu par le client est obtenu SANS le serveur : dupliqué, invisible à
// l'arbitrage, irrévocable (ADR 0026 — le serveur tient le sac, le client n'en est qu'une vue).
//
// ── POURQUOI VIDER ICI, ALORS QU'ON FERME L'ACCÈS PLUS HAUT ──────────────────────────────────
//
// La contrainte de F-PNJ-135 (« vider est la mauvaise porte ») est PROPRE AUX PANTINS : l'inventaire
// d'un PNJ *est* son équipement. **Une caisse ne porte pas son contenu.** La vider ne la déshabille
// pas — elle la met exactement dans l'état où le vanilla la laisse une fois fouillée.
//
// Et c'est ce qui fait qu'ici vider est STRICTEMENT MEILLEUR que fermer l'accès :
//
// 1. **C'est airtight par construction.** Fermer un accès demande d'avoir trouvé TOUTES les voies
//    d'ouverture ; vider ne laisse rien à prendre par aucune voie — offerte, automatique, native ou
//    scriptée. On ne pariera pas sur une énumération exhaustive de portes.
// 2. **Le retrait visuel vient GRATUITEMENT, et c'est le mécanisme du jeu lui-même.** `IsEmpty()`
//    étant natif et vrai, le vanilla éteint de lui-même le halo (`ToggleLootHighlight`), le contour
//    (`GetDefaultHighlight` → NULL), l'icône de carte (`DeterminGameplayRoleMappinVisuaState` →
//    Inactive), `IsContainer()` et la couche d'interaction de proximité
//    (`ResolveQualityRangeInteractionLayer`, quand la qualité retombe à `Invalid`). C'est
//    littéralement le « de manière naturelle » demandé : on ne peint rien, on met le jeu dans un
//    état qu'il sait déjà tenir.
//
// ⚠️ **CE QUI A ÉTÉ ESSAYÉ ET ÉCARTÉ, pour qu'on ne le re-tente pas.** Le premier jet de ce bloc
// posait un `@wrapMethod(Inventory) IsChoiceAvailable -> Invisible` — le composant `Inventory` est
// bien porté par les trois familles, et cette fonction est bien l'unique point de décision scripté
// du corpus. **Elle ne gouverne pourtant PAS le « prendre ».** La preuve est dans le vanilla :
// `IsChoiceAvailable` construit son action par `ItemActionsHelper.SetupItemAction`, dont le `switch`
// n'a **aucun cas pour `'Loot'`** et tombe sur `default: return NULL` — donc pour l'action de prise,
// le vanilla lui-même rendrait `Invisible`, alors que prendre marche. La fonction ne décide donc que
// des actions SECONDAIRES (Lire / Apprendre / Consommer / Démonter). Un hook posé là aurait compilé,
// aurait eu l'air juste, et aurait laissé le « prendre » intact.
//   Sources : `itemActionsHelper.script:226-295` (le switch et son défaut) ·
//   `object_actions/item_actions.tweak:4` (`ItemAction.Loot`, `actionName = "Loot"`).
//
// ── LES TROIS FAMILLES, ET POURQUOI TROIS ACCROCHES ──────────────────────────────────────────
//
// `gameLootContainerBase` (caisses, casiers, `ShardCaseContainer`, `ContainerObjectSingleItem`,
// planques), `gameLootObject`/`gameItemDropObject` (l'objet au sol) et `gameLootBag` (le sac lâché)
// descendent de `GameObject` **séparément** — aucun ancêtre commun exploitable. Monter d'un cran
// jusqu'à `GameObject` ferait tourner ce code sur chaque entité du monde : c'est exactement le
// risque de `@wrapMethod` sur classe parent que F-ASC-025 documente. Trois accroches ciblées valent
// mieux qu'une accroche large.
//
// ── POURQUOI CETTE COUCHE, ET PAS PLUS HAUT (obligation ADR 0020 / skill prise-autorité) ──────
//
// • **1b TweakXL** — vider les `LootTable_Record` couperait les caisses à contenu aléatoire, mais
//   PAS l'objet posé à la main par un level designer : `ContainerObjectSingleItem.itemTDBID` est de
//   la donnée d'INSTANCE, dans le `.ent`, pas dans TweakDB. Le cendrier y survivrait.
// • **1c ArchiveXL** — `nodeDeletions` retire réellement un nœud (F-PLF-015, mesuré) mais c'est
//   statique, déclaré secteur par secteur sur 47 950 `.streamingsector` (F-PNJ-084), et surtout
//   **irréversible au runtime** : un objet supprimé au chargement ne peut plus être ré-autorisé par
//   le serveur, ce que la demande exige explicitement. Et retirer un `worldStaticMeshNode` laisse la
//   collision — on livrerait des murs invisibles.
// Descendre en 2b est donc justifié par un blocage nommé de chaque côté, pas par défaut.
//
// ── LA DÉROGATION SERVEUR, ET POURQUOI ELLE N'EST PAS CÂBLÉE ──────────────────────────────────
//
// « Sauf si plus tard le serveur dit que ça a été ajouté » : le point d'insertion est le test de
// `Tessera_ViderButinMonde`, et rien d'autre. Tant qu'aucun système serveur ne produit cette liste,
// la câbler livrerait un mécanisme d'exception qui n'autorise jamais rien — du code non exercé,
// donc non testé, sur le chemin de l'arbitrage d'items. Quand le serveur saura le dire, c'est une
// condition de plus, à un seul endroit.
//
// ⚠️ **NON MESURÉ — hypothèse** (D2). Tout ce bloc est établi par lecture du corpus décompilé et du
// RTTI, et compile. Rien n'a encore été observé en jeu. Protocole de mesure et critère falsifiable :
// `docs/chantiers/butin-du-monde.md`.

// L'unique endroit qui retire. Journalise ce qu'il a fait, et RELIT après coup — « accepté » n'est
// pas « exécuté » (D1) : `RemoveAllItems` rend un booléen qui dit qu'on a demandé, pas qu'il ne
// reste rien. Sans la relecture, un retrait à moitié fait est indiscernable d'un retrait réussi.
//
// Le journal ne sort QUE si l'objet portait quelque chose : la ville en compte des milliers, la
// plupart vides, et un journal noyé ne se lit pas. Un compteur nul n'est donc pas un silence — c'est
// « il n'y avait rien », ce que la ligne dit explicitement quand il restait quelque chose.
public func Tessera_ViderButinMonde(objet: ref<GameObject>, ou: String) -> Void {
    if !IsDefined(objet) { return; }
    let ts: ref<TransactionSystem> = GameInstance.GetTransactionSystem(objet.GetGame());
    if !IsDefined(ts) { return; }

    let avant: Int32 = ts.GetTotalItemQuantity(objet);
    if avant <= 0 { return; }

    ts.RemoveAllItems(objet);
    let apres: Int32 = ts.GetTotalItemQuantity(objet);

    // Le contenant disparaît AVEC son contenu — demande explicite de Lucas, 2026-08-14 :
    // « vire les caisses s'il y a des objets à l'intérieur ». La condition est déjà là : on n'arrive
    // ici que si `avant > 0`.
    let eteints: Int32 = Tessera_EffacerDuDecor(objet);

    if apres > 0 {
        FTLog(s"[Tessera/ButinMonde] \(ou) : \(avant) -> \(apres) — RESTE QUELQUE CHOSE, retrait incomplet");
    } else {
        FTLog(s"[Tessera/ButinMonde] \(ou) : \(avant) item(s) retire(s), \(eteints) composant(s) eteint(s)");
    }
}

// ── EFFACER LE CONTENANT, PAS SEULEMENT LE CONTENU ───────────────────────────────────────────
//
// Vider suffit à la règle d'autorité ; ça ne suffit pas à la demande. Une caisse vidée reste une
// caisse — le vanilla la rend seulement terne. Lucas veut qu'elle parte.
//
// ⚠️ **UNIQUEMENT sur les objets qui PORTAIENT quelque chose.** Une caisse de décor qui n'a jamais
// rien contenu reste en place : l'effacer trouerait le décor sans rien gagner, et Night City en est
// pleine. C'est la condition littérale de la demande, et c'est aussi la prudente.
//
// ── LES DEUX GESTES, ET POURQUOI IL EN FAUT DEUX ─────────────────────────────────────────────
//
// 1. `IVisualComponent.TemporaryHide(true)` — le geste du jeu lui-même : `WireRepairable` cache
//    ainsi son mesh cassé (`wireRepairable.script:25,53-54`), et `ShardCaseContainer` fait
//    disparaître son shard ramassé par `m_shardMesh.Toggle(false)` (`shardCaseContainer.script:53`).
//    On ne fabrique pas un état que le moteur ne connaît pas : on rejoue le sien.
//
// 2. `ColliderComponent.Toggle(false)` — **sans ça on livre des MURS INVISIBLES.** C'est le piège
//    déjà mesuré du côté ArchiveXL : retirer un `worldStaticMeshNode` laisse le `worldCollisionNode`,
//    l'objet disparaît et la collision reste. Un joueur qui bute dans une caisse invisible est un
//    bug pire que la caisse visible. Les familles de butin nomment d'ailleurs leurs colliders
//    (`'Collider'`, `'ColliderWithInteraction'`, `'ColliderWithoutInteraction'`) — on les prend par
//    leur CLASSE plutôt que par leur nom, pour ne pas dépendre d'une convention de nommage.
//
// ── COMMENT ON ATTEINT LES COMPOSANTS, ET POURQUOI CE N'ÉTAIT PAS ÉVIDENT ────────────────────
//
// `GameObject.GetComponents()` est **ABSENT du dump RTTI** (`tools/nativedb`) — ce qui a d'abord
// fait conclure « pas d'énumération de composants depuis redscript », et envoyé chercher du côté de
// Codeware (qui déclare bien `entComponentsStorage.components`, mais ne le relie à aucune entité)
// puis d'ArchiveXL. Le dump est connu incomplet (F-SCR-018) ; le compilateur, lui, résout contre le
// RTTI RÉEL. Vérifié par compilation, avec contrôle positif (un nom de méthode inventé au même
// endroit échoue en `[UNRESOLVED_METHOD]`). Le harnais l'appelait déjà depuis Lua CET — ce qui
// aurait dû mettre la puce à l'oreille plus tôt.
//
// Noms de classe pris au script décompilé, jamais au dump (F-SCR-001) : `IVisualComponent` et
// `ColliderComponent` — pas `entIVisualComponent` ni `entColliderComponent`.
//
// ⚠️ **NON MESURÉ EN JEU — hypothèse** (D2). Compile et s'appuie sur deux patrons vanilla, mais
// « l'objet a disparu de l'écran » ne se lit pas dans un log. Ce qui trancherait : deux captures
// au même endroit, avant et après, sur une caisse dont on sait qu'elle portait du butin.
func Tessera_EffacerDuDecor(objet: ref<GameObject>) -> Int32 {
    let eteints: Int32 = 0;
    let composants: array<ref<IComponent>> = objet.GetComponents();
    for c in composants {
        let visuel: ref<IVisualComponent> = c as IVisualComponent;
        if IsDefined(visuel) {
            visuel.TemporaryHide(true);
            eteints += 1;
        }
        let collision: ref<ColliderComponent> = c as ColliderComponent;
        if IsDefined(collision) {
            collision.Toggle(false);
            eteints += 1;
        }
    }
    return eteints;
}

// Le moment JUSTE : `ContainerFilledEvent` est l'événement par lequel le jeu annonce qu'il vient de
// garnir le conteneur. Vider à l'attachement seul ne suffirait pas — le remplissage est différé
// (`wasLootInitalized`, `EvaluateLootQualityByTask`), on viderait donc un conteneur encore vide et
// le butin arriverait juste après. C'est aussi cet événement qui rattrape les REGARNISSAGES
// (`RegenerateLootEvent`, `ResetContainerEvent` existent tous les deux).
//
// `wrappedMethod()` est appelée D'ABORD, et ce n'est pas une politesse : elle pose
// `wasLootInitalized` et lance la ré-évaluation de qualité. En vidant après, cette ré-évaluation
// tombe sur un conteneur vide et éteint elle-même halo, contour, icône et couche de proximité.
// L'inverse laisserait le jeu croire qu'il reste du butin de qualité dans une caisse vide.
@wrapMethod(gameLootContainerBase)
protected cb func OnInventoryFilledEvent(evt: ref<ContainerFilledEvent>) -> Bool {
    let r: Bool = wrappedMethod(evt);
    Tessera_ViderButinMonde(this, "conteneur garni");
    return r;
}

// Le rattrapage : un conteneur déjà garni qui revient par le streaming n'émet pas forcément un
// nouveau `ContainerFilledEvent`. Sans cette accroche, tout ce qui a été rempli une fois hors de
// notre portée reviendrait plein.
@wrapMethod(gameLootContainerBase)
protected cb func OnGameAttached() -> Bool {
    let r: Bool = wrappedMethod();
    Tessera_ViderButinMonde(this, "conteneur attache");
    return r;
}

// LE CENDRIER PAR TERRE — l'objet nommé par Lucas.
//
// 🔴 MESURÉ EN JEU LE 2026-08-14, et corrigé : **l'attachement ne suffit PAS.** Le raisonnement
// d'origine (« `ResolveInvotoryContent()` est appelé dans `OnGameAttached`, donc le contenu est là »)
// était faux — cette fonction ne fait que LIRE `GetTotalItemQuantity` pour poser `m_isEmpty` ; elle
// ne garantit pas que l'item existe déjà. Trois `gameItemDropObject` portant 1 item chacun ont
// traversé une session entière avec le hook actif, intacts, et **aucune** ligne de journal n'est
// sortie : le compte valait 0 à l'attachement, donc `Tessera_ViderButinMonde` sortait avant d'agir.
//
// Le mécanisme, lui, est bon : appelé à la main sur ces mêmes trois objets, `RemoveAllItems` rend
// `1>0 rend=true` sur les trois, sac du joueur inchangé (32 → 32). C'était donc le MOMENT, pas
// l'outil — et sans cette sonde on aurait conclu « RemoveAllItems ne marche pas sur les objets au
// sol » et cherché un outil ailleurs.
//
// Le bon moment est `OnItemEntitySpawned` : le natif l'appelle **quand l'entité de l'item a été
// créée** — c'est là que le vanilla lui-même lit `GetItemObject().GetItemID()` pour remplir
// `m_spawnedItemID`. C'est, pour l'objet au sol, l'exact équivalent de `ContainerFilledEvent` pour
// la caisse : l'annonce par le jeu que le contenu existe.
// `protected export function` → `func`, pas `cb func` : ce n'est pas un `event`.
// Source : `core/components/inventoryComponent.script:451-457`.
@wrapMethod(gameItemDropObject)
protected func OnItemEntitySpawned(entID: EntityID) -> Void {
    wrappedMethod(entID);
    Tessera_ViderButinMonde(this, "objet au sol");
}

// Le rattrapage pour l'objet au sol, symétrique de celui des conteneurs : un objet dont l'entité a
// déjà été créée hors de portée ne rejouera pas forcément `OnItemEntitySpawned` au re-streaming.
// ⚠️ MESURÉ INERTE dans la session du 2026-08-14 (compte nul à l'attachement, sortie anticipée,
// zéro ligne de journal). Conservé quand même : il ne coûte rien quand il n'y a rien à retirer, et
// il couvre un cas nommé que l'autre accroche ne couvre pas. Mais il ne faut PAS le lire comme la
// porte principale — c'est `OnItemEntitySpawned` qui fait le travail.
@wrapMethod(gameItemDropObject)
protected cb func OnGameAttached() -> Bool {
    let r: Bool = wrappedMethod();
    Tessera_ViderButinMonde(this, "objet au sol (rattrapage)");
    return r;
}

// Le sac lâché. Il ne DEVRAIT plus s'en produire — `DropHeldItems` rend `false` plus haut — mais
// « ne devrait plus » n'est pas une garantie, et c'est une famille de butin connue et nommée. La
// laisser dehors serait un trou qu'on aurait choisi en connaissance de cause.
@wrapMethod(gameLootBag)
protected cb func OnGameAttached() -> Bool {
    let r: Bool = wrappedMethod();
    Tessera_ViderButinMonde(this, "sac lache");
    return r;
}

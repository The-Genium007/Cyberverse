module Cyberverse.Network.Managers

// Une entité sous autorité SERVEUR ne joue plus de PNJ chez le client qui la regarde.
//
// ── Le symptôme ───────────────────────────────────────────────────────────────────────────────
//
// Observé en jeu le 2026-08-08 (Lucas, deux clients) : viser l'avatar d'un autre joueur le fait
// réagir comme un passant braqué — mains en l'air, fuite, arme dégainée — alors que le joueur
// derrière cet avatar n'a rien fait du tout. La réaction naît LOCALEMENT, chez celui qui vise, dans
// l'IA native du pantin ; elle n'existe chez personne d'autre. Deux clients voient donc deux
// personnages différents au même endroit. Même famille de résidu pour l'invite d'interaction PNJ
// (parler, saisir), qui persiste elle aussi sur l'avatar — F-PLY-011.
//
// ── ⚠️ LA LEÇON DE LA PREMIÈRE VERSION, ET POURQUOI CELLE-CI EST DIFFÉRENTE ────────────────────
//
// La première version éteignait les COMPOSANTS : `ReactionManagerComponent.Toggle(false)`,
// `InteractionComponent.Toggle(false)`, plus une immortalité posée par `GodModeSystem`. Elle
// obtenait bien l'effet voulu — et Lucas a ensuite constaté qu'il ne pouvait **plus viser** les
// autres joueurs.
//
// Cette version-là violait la doctrine, et c'est exactement là qu'elle s'est punie :
// `tessera-prise-autorite` étape 4 dit « **neutraliser le déclencheur, jamais le mécanisme** ».
// Éteindre un composant, c'est retirer un mécanisme entier de l'entité — avec tout ce que le NATIF
// en tire et que les scripts décompilés ne montrent pas (F-SCR-018 : le C++ jamais exposé au script
// n'apparaît nulle part). La visée est entièrement native : impossible de prouver par lecture
// qu'elle ne dépend pas d'un composant éteint, et donc impossible de s'y fier.
//
// Cette version n'ÉTEINT PLUS AUCUN COMPOSANT : le pantin garde tous les mécanismes que le moteur
// lui a construits, et on intercepte seulement deux fonctions, aux deux endroits où la décision se
// prend. La SEULE écriture d'état qui subsiste est l'immortalité posée en fin d'attachement — et
// elle est là parce qu'une mesure l'exige, pas par confort : sans elle, l'avatar s'écroule au
// premier tir, tué par les PV de son propre record de foule (voir le bloc en fin de fonction).
//
// ── Les deux points de décision ───────────────────────────────────────────────────────────────
//
// 1. **Les réactions.** `ReactionManagerComponent.HandleStimEvent` est l'entonnoir unique de
//    RÉCEPTION d'un stimulus (`reactionComponent.script:649`) : `OnEventReceived`, seul point
//    d'entrée des `StimuliEvent` (`:1436-1442`), y mène par `HandleStimEventByTask`, et le rejeu
//    différé (`DelayStimEvent`, `:2970`) passe par la même porte. On sort avant, pour une entité
//    réseau, et le composant continue par ailleurs sa vie entière — presets, attitude, blackboards.
//
// 2. **Les interactions.** `ScriptedPuppet.ToggleInteractionLayers` (`:1133`) est le seul endroit
//    qui ALLUME les couches d'interaction d'un pantin. On le laisse tourner, puis on éteint les
//    couches pour une entité réseau : le composant d'interaction reste vivant et enregistré, seules
//    ses couches sont vides.
//
// ── Portée : toute entité réseau, pas seulement les avatars ───────────────────────────────────
//
// `Tessera_EstEntiteReseau` couvre les avatars de joueurs ET les PNJ promus. C'est délibéré et
// c'est la même règle : ce que le serveur arbitre ne doit pas être décidé en double par l'IA
// locale. Une entité promue qui panique chez un client et pas chez l'autre est exactement le bug
// qu'on corrige ici sur les avatars.
//
// ⚠️ La foule LOCALE n'est pas touchée — c'est elle qui porte la panique répliquée (F-PNJ-115), et
// elle continue de réagir normalement.
//
// Entonnoir, garde-fou natif et fausses pistes : **F-PNJ-133** (`docs/connaissances/moteur-pnj-foule.md`).

// Cette entité est-elle sous autorité serveur ? Un seul point, pour que les deux hooks ci-dessous
// posent EXACTEMENT la même question — et pour que le jour où le critère change, il change en un
// seul endroit.
public func TesseraEstSousAutoriteServeur(cible: EntityID) -> Bool {
    if !EntityID.IsDefined(cible) {
        return false;
    }
    let reseau = GameInstance.GetNetworkGameSystem();
    return IsDefined(reseau) && reseau.Tessera_EstEntiteReseau(cible);
}

// ⚠️ `wrappedMethod()` N'est PAS appelée pour une entité réseau : c'est tout le propos, le stimulus
// s'arrête là. Pour tout le reste du monde — la foule locale, les PNJ de quête, le joueur — la
// fonction se déroule intégralement, à l'instruction près.
@wrapMethod(ReactionManagerComponent)
protected func HandleStimEvent(stimData: ref<StimEventTaskData>) -> Void {
    // ⚠️ `GetOwner()` est testé, pas supposé. Cette fonction tourne pour CHAQUE pantin de la ville à
    // chaque stimulus, y compris pendant un détachement : un propriétaire nul déréférencé ici ne
    // ferait pas une ligne d'erreur, il ferait tomber le jeu.
    let proprietaire = this.GetOwner();
    if IsDefined(proprietaire) && TesseraEstSousAutoriteServeur(proprietaire.GetEntityID()) {
        return;
    }
    wrappedMethod(stimData);
}

// ⚠️ ON N'ANNOTE PAS `ToggleInteractionLayers`, ET C'EST DÉLIBÉRÉ. C'est pourtant l'endroit exact
// où les couches s'allument (`scriptedPuppet.script:1133`) — mais elle est `private`, et le projet
// a déjà mesuré qu'un `@wrapMethod` sur une méthode privée **compile et ne se déclenche jamais**
// (mémoire `native-ui-reinvocation-inventory`, rappelé dans `BACKLOG-INGAME.md` Q-UI2). Un hook
// silencieusement inerte est pire qu'un hook absent : il se relit comme fait.
//
// On passe donc par `OnGameAttached`, qui est `protected` (donc réellement annotable) et qui APPELLE
// `ToggleInteractionLayers` (`:671`). `wrappedMethod()` d'abord, sans condition : le vanilla pose ses
// couches selon le type de pantin, et on ne les éteint qu'ensuite. Refaire son raisonnement pour
// l'inverser serait le dupliquer — et un jour le laisser dériver.
//
// La liste est celle de `ToggleInteractionLayers` lui-même (`:1133-1190`), plus `Loot` par sécurité :
// `LootOff.reds` la coupe déjà pour tout le monde, mais une couche citée deux fois ne coûte rien
// quand une couche oubliée se voit en jeu.
@wrapMethod(ScriptedPuppet)
protected cb func OnGameAttached() -> Bool {
    let resultat = wrappedMethod();
    if !TesseraEstSousAutoriteServeur(this.GetEntityID()) {
        return resultat;
    }
    this.EnableInteraction(n"GenericTalk", false);
    this.EnableInteraction(n"ReturnTalk", false);
    this.EnableInteraction(n"Grapple", false);
    this.EnableInteraction(n"TakedownLayer", false);
    this.EnableInteraction(n"AerialTakedown", false);
    this.EnableInteraction(n"RemoteTakedownLayer", false);
    this.EnableInteraction(n"NewPerkFinisherLayer", false);
    this.EnableInteraction(n"OnePunchAttackLayer", false);
    this.EnableInteraction(n"BossTakedownLayer", false);
    this.EnableInteraction(n"MassiveTargetTakedownLayer", false);
    this.EnableInteraction(n"Loot", false);

    // ── LA MORT D'UNE ENTITÉ RÉSEAU EST DÉCIDÉE PAR LE SERVEUR, PAS PAR SON PANTIN ──────────
    //
    // L'avatar d'un joueur distant est un pantin de foule : il porte les PV de SON record, qui
    // n'ont aucun rapport avec la santé que le serveur tient pour la personne qu'il représente.
    // Un citoyen d'ambiance encaisse bien moins qu'un joueur — donc **au premier tir il s'écroule**,
    // mort de sa propre comptabilité locale, pendant que le serveur le sait à 76 % de vie.
    // Observé en jeu le 2026-08-09 : « dès le premier tir, le personnage tombe comme s'il était
    // mort ».
    //
    // `Immortal` et non `Invulnerable`, et la distinction est TOUT :
    //   · `Immortal`     — les dégâts sont calculés, appliqués et JOUÉS (sang, recul, impacts),
    //                      seule la mort est empêchée (la barre se bloque à 1). C'est ce calcul
    //                      qu'on rapporte au serveur (`DegatsPvp.reds`) : il reste intact.
    //   · `Invulnerable` — aucun dégât n'est calculé. C'est ce que portait le record de repli
    //                      `Character.Panam` par un tag TweakDB, et c'est ce qui a rendu le PvP
    //                      impossible pendant deux jours (sonde `sonde_cible`, 2026-08-09).
    //
    // ⚠️ Cette ligne a été RETIRÉE le 2026-08-08 en cherchant une régression de visée, puis remise
    // ici — parce que la sonde a établi entre-temps que la cause était le record de repli, pas ce
    // god mode. C'est la mesure qui la remet, pas le confort. Le verrou est levé par
    // `NetworkGameSystem.TesseraRendreMort`, au moment où le serveur annonce la mort — sans quoi
    // `Kill` serait accepté sans effet et le corps resterait debout.
    GameInstance.GetGodModeSystem(GetGameInstance())
        .AddGodMode(this.GetEntityID(), gameGodModeType.Immortal, n"Tessera");

    // ── LA MISE À TERRE EST UNE AFFIRMATION QUE LE SERVEUR N'A PAS FAITE ────────────────────
    //
    // Observé en jeu le 2026-08-09, trois fois de suite : « le personnage se relève quand même à
    // un moment ». Le pantin est immortel localement, donc la balle le met à terre, il se relève —
    // et la mort décidée par le serveur arrive après. Il tombe deux fois, dont une pour rien, et
    // ça se lit comme un bug parce que c'en est un : la chute affirmait un état que personne
    // n'avait arbitré.
    //
    // On ne prédit localement que ce qu'il est bon marché d'avoir eu tort. Un impact, du sang, un
    // son, un tressaillement : gratuits même s'ils s'avèrent inutiles, et ce sont EUX qui donnent
    // la sensation du tir. Une chute au sol, non — elle engage.
    //
    // ⚠️ C'EST LE BOUTON DU MOTEUR, PAS UN CONTOURNEMENT. La chaîne native, tracée entièrement
    // dans `hitReactionComponent.script` :
    //
    //   stat `KnockdownImmunity` > 0
    //     └─► `UpdateOwnerKnockdownImmunityData` (:82, par l'écouteur de stats)
    //         └─► `m_currentKnockdownImmunity`
    //             └─► `SetHitReactionImmunities` (:1623) — rappelée À CHAQUE COUP (:1409)
    //                 └─► `m_immuneToKnockDown = true`
    //                     └─► `GetReactionType` (:2954) ne peut plus renvoyer `Knockdown`
    //
    // C'est exactement ce que CDPR pose sur ses BOSS (`bosses.tweak`, `npcrarities.tweak`) et ce
    // que le jeu s'applique à lui-même quand le joueur porte un corps (`carriedObject.script:1157`)
    // ou pendant certaines transitions de mêlée. Le mécanisme reste entier : seule la BRANCHE
    // knockdown devient inatteignable, et le pantin continue de tressaillir, saigner et encaisser.
    //
    // ⚠️ Le `Stagger` (bousculade sur place) est DÉLIBÉRÉMENT laissé : il ne comporte pas de relevé,
    // donc il n'affirme rien — et il fait partie du retour visuel qu'on veut garder.
    //
    // ⚠️ CE QUI ÉTAIT ÉCRIT AVANT ET QUI NE MARCHAIT PAS. `DegatsPvp.reds` retirait trois status
    // effects APRÈS coup : `BaseStatusEffect.Knockdown`, `.KnockdownWithGetUp`, `.Stagger`. Les
    // deux derniers **n'existent nulle part dans le jeu** (zéro référence dans les scripts
    // décompilés et dans TweakDB — vérifié le 2026-08-09), et le premier arrivait de toute façon
    // trop tard : `GetReactionType` avait déjà choisi l'animation. Trois lignes qui se relisaient
    // comme faites et ne faisaient rien. Un identifiant TweakDB inventé ne lève aucune erreur —
    // il se résout en silence sur un enregistrement vide.
    // ⚠️ `Cast<StatsObjectID>` obligatoire. Le script décompilé écrit
    // `statSys.AddModifier(m_owner.GetEntityID(), modifier)` — sans cast — parce que la chaîne
    // d'origine de CDPR convertit implicitement. redscript, lui, refuse :
    // `[NO_MATCHING_OVERLOAD] 1st argument: expected 'StatsObjectID', given 'EntityID'`. Le script
    // décompilé fait foi sur les NOMS et les mécanismes, jamais sur ce que le compilateur accepte.
    let modificateurs = GameInstance.GetStatsSystem(GetGameInstance());
    modificateurs.AddModifier(
        Cast<StatsObjectID>(this.GetEntityID()),
        RPGManager.CreateStatModifier(gamedataStatType.KnockdownImmunity,
                                      gameStatModifierType.Additive, 1.0));

    return resultat;
}

// ⚠️ HISTORIQUE, pour que personne ne refasse le trajet. L'immortalité ci-dessus a été posée le
// 2026-08-08, retirée le jour même en cherchant une régression de visée, puis remise le 2026-08-09
// quand la sonde a montré que la visée était bloquée par tout autre chose (le record de repli
// `Character.Panam` et son tag `Invulnerable`). Retirer une pièce pour chercher une panne est une
// bonne méthode ; conclure qu'elle était coupable parce que la panne a persisté n'en est pas une.

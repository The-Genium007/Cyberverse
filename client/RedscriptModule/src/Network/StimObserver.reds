module Cyberverse.Network.Managers

// Remonte au serveur les stimulus produits par le JOUEUR LOCAL.
//
// ── Pourquoi ici, et pas sur le tir ────────────────────────────────────────────────────────────
//
// `StimBroadcasterComponent.TriggerSingleBroadcast` est l'ENTONNOIR UNIQUE au sens de la skill
// `tessera-prise-autorite` : 83 sites d'appel dans les scripts du jeu y aboutissent, et les trois
// façades statiques (`BroadcastStim`, `BroadcastActiveStim`, `SendStimDirectly`) y passent aussi.
// Un hook ici couvre donc les 67 types d'un coup — tir, corps à corps, klaxon, action illégale —
// là où un hook sur `ShootEvents.OnEnter` n'aurait attrapé que le tir.
//
// Il donne en prime le type ET le rayon **déjà calculés par le jeu** : la portée d'un coup de feu
// dépend de l'intérieur/extérieur, du silencieux, du district (Dogtown), et de l'état de combat
// (weaponTransitions.script:2420-2447). Les redériver serait du code à maintenir en miroir du
// vanilla, qui divergerait au premier patch. On lit ce que le jeu a décidé.
//
// ── Couche (ADR 0015) ─────────────────────────────────────────────────────────────────────────
//
// Couche 2b, redscript nu. Les couches 1a-1c ne peuvent PAS observer un événement : TweakXL écrit
// de la donnée, ArchiveXL ajoute des ressources. Codeware (2a) n'apporte rien ici — la classe est
// scriptée et directement annotable, aucune réflexion RTTI n'est nécessaire.
//
// ── Observation pure ──────────────────────────────────────────────────────────────────────────
//
// ⚠️ On ne NEUTRALISE rien. `wrappedMethod` est appelé le premier, sans condition : la foule locale
// du tireur réagit nativement, comme avant. On ajoute une remontée, on ne retire pas un
// comportement. C'est la spec §4.4 : « le client remonte l'observation, il ne décide jamais de la
// réaction. »

@wrapMethod(StimBroadcasterComponent)
public func TriggerSingleBroadcast(contextOwner: wref<GameObject>, gdStimType: gamedataStimType, opt radius: Float, opt investigateData: stimInvestigateData, opt propagationChange: Bool) -> Void {
    wrappedMethod(contextOwner, gdStimType, radius, investigateData, propagationChange);
    TesseraRemonterStim(contextOwner, gdStimType, radius);
}

// ⚠️ SANS CE FILTRE, boucle infinie. À la réception d'un stimulus distant, `ApplyServerStim` appelle
// `BroadcastStim`, qui appelle `TriggerSingleBroadcast`, qui repasse ici. Ce qui casse la boucle
// n'est pas un drapeau mais l'ÉMETTEUR : on rejoue toujours avec l'avatar du joueur DISTANT, jamais
// avec le joueur local. Le test ci-dessous suffit donc, et il n'y a pas d'état à maintenir.
public func TesseraRemonterStim(contextOwner: wref<GameObject>, gdStimType: gamedataStimType, radius: Float) -> Void {
    if !IsDefined(contextOwner) {
        return;
    }
    let local = GameInstance.GetPlayerSystem(GetGameInstance()).GetLocalPlayerControlledGameObject();
    if !IsDefined(local) || contextOwner.GetEntityID() != local.GetEntityID() {
        return;
    }
    if !TesseraStimRemontable(gdStimType) {
        return;
    }
    let reseau = GameInstance.GetNetworkGameSystem();
    if !IsDefined(reseau) {
        return;
    }
    reseau.Tessera_ReportStim(Cast<Uint32>(EnumInt(gdStimType)), radius, TesseraCibleVisee(local));
}

// Sur QUOI le joueur agit, à l'instant où il agit.
//
// C'est une INTERROGATION, pas un hook : on demande au jeu ce que le joueur regarde. `TargetingSystem`
// est natif (`importonly`) et répond directement à la question — rien à assembler, rien à deviner
// (F-PLY-028).
//
// `withLOS = true` : sans le test de ligne de vue, on désignerait un pantin à travers un mur. Un
// joueur qui tire en l'air ne doit désigner personne.
//
// ⚠️ Renvoie une `EntityID` LOCALE. Elle n'a de sens que sur cette machine — c'est le C++ qui la
// traduit en identité réseau, et qui envoie 0 si la cible n'est pas une entité serveur. Ne jamais
// la mettre telle quelle sur le fil.
public func TesseraCibleVisee(local: ref<GameObject>) -> EntityID {
    let visee = GameInstance.GetTargetingSystem(GetGameInstance()).GetLookAtObject(local, true, false);
    // `EMPTY_ENTITY_ID()` et non `EntityID.None()` : cette dernière n'existe pas. L'API réelle est
    // un `importonly struct` sans constructeur, plus une fonction globale (`entityID.script`).
    if !IsDefined(visee) {
        return EMPTY_ENTITY_ID();
    }
    return visee.GetEntityID();
}

// Le fil porte les 67 ; on en REMONTE une poignée. Ce n'est pas une contradiction, c'est la
// doctrine « câbler large, activer étroit » (ADR 0023) : élargir cette liste est une ligne à
// ajouter ici, sans toucher au protocole, au serveur, ni à l'en-tête C++.
//
// ⚠️ Ce qui est délibérément EXCLU, et pourquoi. Le joueur émet en permanence des stimulus de
// bruit — `FootStepRegular` à chaque pas, `Bump`, `LandingRegular`. Les remonter noierait le budget
// de 40 messages/s du Gateway (rate_limit.rs) et ferait déconnecter pour flood un joueur qui marche.
// Le critère de tri : un stimulus se remonte s'il produit une scène que les AUTRES joueurs doivent
// voir. Une foule qui fuit, oui. Un pas, non.
//
// Statut : aucun de ces types n'a d'effet MESURÉ en multijoueur — seul `Gunshot` est mesuré, et
// seulement en local (F-PNJ-104). Voir la colonne « effet mesuré » du catalogue.
public func TesseraStimRemontable(gdStimType: gamedataStimType) -> Bool {
    switch gdStimType {
        // Armes à feu — le cas mesuré, et le plus fort visuellement.
        case gamedataStimType.Gunshot: return true;
        case gamedataStimType.SilencedGunshot: return true;
        // Explosifs — même famille d'effet, portée plus grande.
        case gamedataStimType.Explosion: return true;
        case gamedataStimType.GrenadeLanded: return true;
        // Menace sans tir : ce qui fait qu'une rue se fige avant qu'un coup ne parte.
        case gamedataStimType.WeaponDisplayed: return true;
        // Violence au contact.
        case gamedataStimType.MeleeAttack: return true;
        case gamedataStimType.Dying: return true;
        // Alarmes — un événement de zone, pas un geste individuel.
        case gamedataStimType.Alarm: return true;
        case gamedataStimType.CarAlarm: return true;
        default: return false;
    }
}

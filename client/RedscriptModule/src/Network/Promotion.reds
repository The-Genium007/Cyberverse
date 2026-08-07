module Cyberverse.Network.Managers

// LA PROMOTION (ADR 0022) — un figurant devient un personnage parce que quelqu'un s'y intéresse.
//
// ── Le déclencheur : la MORT ──────────────────────────────────────────────────────────────────
//
// Choix de Lucas, 2026-08-07, après avoir vu la promotion sur visée fonctionner sans être
// observable : « il faut qu'on le branche sur la létalité, quand tu piques déjà les cadavres par
// terre. Comme ça c'est quelque chose de statique, ça me laisse le temps d'observer. »
//
// Il y a une raison plus forte que le confort d'observation, et c'est l'ADR qui la donne : **un
// PNJ tué doit rester mort pour tout le monde**. C'est le cas où la réplication cesse d'être un
// agrément pour devenir une obligation — deux joueurs qui ne s'accordent pas sur qui est mort ne
// jouent pas dans le même monde.
//
// Trois propriétés en font le bon premier cas, là où la visée était le mauvais :
//   · la cible ne bouge plus — l'observateur a le temps de regarder, et de comparer les apparences ;
//   · le promu ne bouge pas non plus — aucune divergence de comportement ne brouille la lecture ;
//   · l'événement est RARE, donc pas de rafale de promotions à filtrer.
//
// ── L'entonnoir ───────────────────────────────────────────────────────────────────────────────
//
// `ScriptedPuppet.OnDied()` : déclarée EN PROPRE sur la classe (les deux conditions de la skill
// redscript), et surtout appelée depuis **un seul endroit** — `HandleDeath`, `scriptedPuppet.script:415`.
// Un entonnoir unique au sens de `tessera-prise-autorite`, pas un point où ça se voit.
//
// ⚠️ On n'intercepte RIEN : `wrappedMethod()` d'abord, sans condition. La mort se déroule
// normalement, on ajoute seulement une demande.

@wrapMethod(ScriptedPuppet)
protected func OnDied() -> Void {
    wrappedMethod();
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        TesseraPromouvoirSiFigurant(reseau, this.GetEntityID());
    }
}

// Met un PNJ répliqué dans l'état MORT, sur ordre du serveur (`NpcState.behavior = ATerre`).
//
// Pourquoi ça existe : un figurant promu parce qu'il vient de mourir naissait VIVANT chez les autres
// joueurs — et rejoignait la panique que le relais de stimulus venait de déclencher. Mesuré en jeu
// le 2026-08-07 : « c'est une personne qui était déjà debout et qui est tout de suite partie parce
// qu'elle avait le statut effrayé ». L'initiateur avait un cadavre, l'autre joueur un passant qui
// s'enfuit — deux mondes qui ne racontent pas la même histoire.
//
// ⚠️ `skipNPCDeathAnim = true` : le personnage est mort AVANT d'arriver ici. Rejouer l'animation
// d'agonie ferait tomber un cadavre qui vient d'apparaître debout, ce qui serait plus troublant que
// l'apparition elle-même. Le ragdoll, lui, est CONSERVÉ (`disableNPCRagdoll = false`) : c'est lui
// qui pose le corps au sol au lieu de le laisser figé debout.
public func TesseraRendreMort(cible: EntityID) -> Bool {
    let pantin = GameInstance.FindEntityByID(GetGameInstance(), cible) as ScriptedPuppet;
    if !IsDefined(pantin) {
        return false;
    }
    pantin.Kill(null, true, false);
    return true;
}

// Demande au serveur de prendre un figurant sous autorité.
//
// ⚠️ On envoie de quoi le REFABRIQUER, jamais un identifiant. Le pantin n'existe que sur cette
// machine — les autres joueurs ont d'autres passants au même endroit, et les leurs divergent
// fortement (F-PNJ-117). Un `EntityID` local ne désignerait rien pour eux.
//
// `Tessera_EstEntiteReseau` écarte ce qui est DÉJÀ sous autorité : sans ce test, un PNJ serveur qui
// meurt demanderait sa propre promotion, en boucle.
//
// `IsCrowd()` restreint aux figurants : un PNJ de quête ou un vendeur relève du registre nominatif.
//
// ⚠️ DEUX LIMITES ASSUMÉES, à lever séparément.
//   1. Le pantin local n'est pas masqué — on voit le cadavre ET le promu.
//   2. Le promu naît DEBOUT et VIVANT : le serveur crée un PNJ ordinaire, il ne sait pas encore
//      répliquer un état de mort. L'autre joueur verra donc quelqu'un debout là où l'initiateur a
//      un cadavre. C'est incohérent, c'est su, et c'est utile en attendant : un mannequin immobile
//      est exactement ce qu'il faut pour vérifier que l'apparence transmise est la bonne.
public func TesseraPromouvoirSiFigurant(reseau: ref<NetworkGameSystem>, cible: EntityID) -> Void {
    if !EntityID.IsDefined(cible) || reseau.Tessera_EstEntiteReseau(cible) {
        return;
    }
    let pantin = GameInstance.FindEntityByID(GetGameInstance(), cible) as ScriptedPuppet;
    if !IsDefined(pantin) || !pantin.IsCrowd() {
        return;
    }
    let pos = pantin.GetWorldPosition();
    reseau.Tessera_DemanderPromotion(
        TDBID.ToNumber(pantin.GetRecordID()),
        pantin.GetCurrentAppearanceName(),
        // `GetWorldYaw()` rend déjà des DEGRÉS (`entity.script:26`) — pas de conversion, et surtout
        // pas de `Rad2Deg` : cette fonction n'existe pas dans les scripts du jeu.
        pos.X, pos.Y, pos.Z, pantin.GetWorldYaw(),
        // `true` sans condition : le seul déclencheur de promotion est aujourd'hui `OnDied`. Le jour
        // où un autre s'y branche, ce booléen devra venir de lui — d'où un paramètre plutôt qu'une
        // constante côté serveur.
        true);
}

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

// ⚠️ `TesseraRendreMort` N'EST PAS ICI, et ce n'est pas un choix de rangement : elle est appelée
// par le C++ via `Red::CallVirtual(this, "TesseraRendreMort", ...)`, qui cherche une MÉTHODE sur la
// classe de l'objet — pas une fonction de module. Déclarée ici, l'appel échouait silencieusement
// (`appel=echec`, 2 500 essais sans qu'une seule ligne de la fonction ne s'exécute). Elle vit donc
// dans `NetworkGameSystem.reds`, avec `ApplyServerConfig` et `ApplyServerStim` qui suivent la même
// règle.

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
    let partie = reseau.Tessera_DemanderPromotion(
        TDBID.ToNumber(pantin.GetRecordID()),
        pantin.GetCurrentAppearanceName(),
        // `GetWorldYaw()` rend déjà des DEGRÉS (`entity.script:26`) — pas de conversion, et surtout
        // pas de `Rad2Deg` : cette fonction n'existe pas dans les scripts du jeu.
        pos.X, pos.Y, pos.Z, pantin.GetWorldYaw(),
        // `true` sans condition : le seul déclencheur de promotion est aujourd'hui `OnDied`. Le jour
        // où un autre s'y branche, ce booléen devra venir de lui — d'où un paramètre plutôt qu'une
        // constante côté serveur.
        true);

    // Le pantin LOCAL cède la place à celui du serveur — sinon le tireur voit DEUX cadavres, le
    // sien et le promu. Mesuré en jeu le 2026-08-08 : « ça fait un doublon ».
    //
    // ⚠️ CONDITIONNÉ à l'envoi réel. Un `false` signifie déduplication, absence de connexion ou
    // record nul : masquer quand même effacerait un corps que RIEN ne remplacerait. C'est la raison
    // d'être de la valeur de retour — sans elle, un serveur coupé ferait disparaître les cadavres.
    //
    // On MASQUE, on ne détruit pas. Le pantin appartient à la foule native : le supprimer sortirait
    // du périmètre de la promotion et toucherait des systèmes (communauté, population de secteur)
    // que F-PNJ-091 et F-PNJ-093 décrivent comme rétifs. Un masquage est réversible et local.
    //
    // ⚠️ Il reste un écart d'environ 150 ms entre le masquage et l'arrivée du promu (mesuré :
    // F-PNJ-116). Le corps disparaît puis revient. Imperceptible en pratique, mais c'est une
    // approximation, pas une propriété — si ça se voit un jour, il faudra masquer à la RÉCEPTION
    // du promu plutôt qu'à l'émission.
    if partie {
        GameObject.ToggleForcedVisibilityInAnimSystemEvent(pantin, n"Tessera_Promotion", false, 0.0);
    }
}

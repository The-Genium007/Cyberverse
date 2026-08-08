module Cyberverse.Network.Managers

// SONDE DE RECHERCHE — chantier `foule-reproductible`. Outil de mesure, pas de production.
//
// ── La question, et pourquoi elle passe avant tout le reste ───────────────────────────────────
//
// L'objectif est que deux joueurs voient les MÊMES passants, aux mêmes endroits, habillés pareil
// (direction de Lucas, 2026-08-08). L'idée naturelle est de dériver l'apparence d'un hachage de la
// place — c'est ce que l'ADR 0022 propose, et c'est ce qui contourne le piège de la graine commune
// (le générateur du jeu est consommé dans l'ordre du streaming, donc une graine ne suffit pas).
//
// ⚠️ MAIS TOUT CE RAISONNEMENT REPOSE SUR UNE PRÉMISSE NON MESURÉE : que les deux clients fassent
// naître leurs passants **aux mêmes positions**. Si les positions divergent, aucun hachage par la
// place ne peut fonctionner — il n'y aurait pas de place commune à hacher. C'est l'inconnue n°3 du
// chantier, et la seule qui puisse invalider l'approche entière.
//
// Lucas a déjà observé « de gros écarts sur les passants » (F-PNJ-117). Cette sonde transforme cette
// impression en chiffres comparables.
//
// ── Ce qu'on a appris en cherchant où se brancher ─────────────────────────────────────────────
//
// Le système de foule est **entièrement natif** : `CrowdMemberComponent` et `CommunitySystem` sont
// `importonly`, et ce dernier n'expose qu'un modificateur de densité et des zones vides. **Aucun
// point d'entrée scripté pour le spawn.** On ne pourra donc pas décider où naissent les passants
// depuis redscript — au mieux les intercepter APRÈS coup, ce que fait cette sonde.
//
// `OnGameAttached` est un `event`, pas un `func` : transcrit en `cb func` (règle de la skill
// redscript, `event` non annotable).

@wrapMethod(ScriptedPuppet)
protected cb func OnGameAttached() -> Bool {
    let resultat = wrappedMethod();
    // `IsCrowd()` restreint aux figurants d'ambiance : sans ce filtre on journaliserait aussi les
    // PNJ de quête, les vendeurs et nos propres promus, ce qui rendrait la comparaison illisible.
    if this.IsCrowd() {
        let reseau = GameInstance.GetNetworkGameSystem();
        if !IsDefined(reseau) {
            return resultat;
        }
        let p = this.GetWorldPosition();
        // Format volontairement PLAT et stable, pour être diffé entre deux journaux sans outil :
        // position au décimètre, record, apparence. L'heure de jeu n'y est pas — elle est déjà
        // synchronisée par le serveur, donc commune par construction.
        reseau.Tessera_Journal(s"[Foule] \(Cast<Int32>(p.X * 10.0));\(Cast<Int32>(p.Y * 10.0));\(Cast<Int32>(p.Z * 10.0));\(TDBID.ToNumber(this.GetRecordID()));\(this.GetCurrentAppearanceName())");
    }
    return resultat;
}

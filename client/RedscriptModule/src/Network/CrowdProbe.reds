module Cyberverse.Network.Managers

// SONDE DE RECHERCHE — chantier `foule-reproductible`. Outil de mesure, pas de production.
//
// ── Ce que la première version a manqué ───────────────────────────────────────────────────────
//
// Elle journalisait TOUS les pantins `IsCrowd()` à l'attachement, statiques compris. Or les PNJ
// statiques sont déjà partagés entre clients (F-PNJ-117) : ils gonflaient le taux de concordance
// et masquaient le seul chiffre qui compte. Remarque de Lucas, 2026-08-08 : « c'est essentiellement
// des personnes statiques, il faut faire la différence — nous on parle de ce qui MARCHE, c'est là
// qu'on a le moins d'informations. »
//
// ── Comment on distingue un marcheur ──────────────────────────────────────────────────────────
//
// Pas à l'attachement : à ce moment-là tout le monde est immobile, et le composant de foule n'a pas
// encore de trajet. On CLASSE donc 3 secondes plus tard, par `CrowdMemberComponent.CheckIsMoving()`.
//
// ⚠️ La ligne de classement reporte la position D'ORIGINE, pas celle du moment. Un marcheur a bougé
// entre-temps, et deux clients ne l'auront pas fait avancer pareil : comparer les positions tardives
// mesurerait la dérive, pas le placement. C'est le point de NAISSANCE qui est comparable, et c'est
// lui qui sert de clé de jointure entre les deux journaux.
//
// ── Ce qu'on a appris en cherchant où se brancher ─────────────────────────────────────────────
//
// Le système de foule est **entièrement natif** : `CrowdMemberComponent` et `CommunitySystem` sont
// `importonly`, ce dernier n'exposant qu'un modificateur de densité et des zones vides. **Aucun
// point d'entrée scripté pour le spawn** — on ne peut pas décider où naissent les passants, seulement
// les intercepter après coup.
//
// `OnGameAttached` est un `event`, pas un `func` : transcrit en `cb func` (règle de la skill).

// Classe le pantin APRÈS coup. Porte la position de naissance pour rester joignable au journal
// d'attachement.
class TesseraClassementFoule extends DelayCallback {
    let pantin: wref<ScriptedPuppet>;
    let cle: String;

    public func Call() -> Void {
        if !IsDefined(this.pantin) {
            return;
        }
        let reseau = GameInstance.GetNetworkGameSystem();
        if !IsDefined(reseau) {
            return;
        }
        let foule = this.pantin.GetCrowdMemberComponent();
        // Pas de composant de foule = ni marcheur ni statique au sens du système : on le dit plutôt
        // que de le ranger d'office dans « statique », ce qui fausserait la comparaison.
        if !IsDefined(foule) {
            reseau.Tessera_Journal(s"[Classe] \(this.cle);sans-composant");
            return;
        }
        let statique = !foule.CheckIsMoving();
        reseau.Tessera_Journal(s"[Classe] \(this.cle);\(statique ? "statique" : "marcheur")");
        if !statique {
            return;
        }
        // ── SYNCHRONISATION DES STATIQUES ────────────────────────────────────────────────────
        //
        // C'est ICI, et pas à l'attachement, parce qu'on ne sait qu'un pantin est statique qu'après
        // l'avoir vu ne pas bouger. Rapporter à l'attachement mélangerait les deux populations, et
        // le serveur arbitrerait l'apparence de passants — qui n'ont aucune identité partagée.
        //
        // Deux gestes, dans cet ordre : on APPLIQUE ce que le serveur a déjà décidé (le cas d'un
        // PNJ streamé après coup), et sinon on RAPPORTE ce qu'on voit pour qu'il tranche.
        // Sonde one-shot : `ScheduleAppearanceChange` a-t-il un effet ? Voir AppearanceProbe.reds.
        TesseraSonderApparence(reseau, this.pantin);

        let connue = reseau.Tessera_ApparenceStatiqueConnue(this.pantin.GetEntityID());
        if IsNameValid(connue) {
            reseau.AppliquerApparenceStatique(this.pantin.GetEntityID(), connue);
        } else {
            reseau.Tessera_RapporterStatique(
                this.pantin.GetEntityID(),
                TDBID.ToNumber(this.pantin.GetRecordID()),
                this.pantin.GetCurrentAppearanceName());
        }
    }
}

@wrapMethod(ScriptedPuppet)
protected cb func OnGameAttached() -> Bool {
    let resultat = wrappedMethod();
    if this.IsCrowd() {
        let reseau = GameInstance.GetNetworkGameSystem();
        if !IsDefined(reseau) {
            return resultat;
        }
        let p = this.GetWorldPosition();
        // Clé = position de naissance au décimètre. Format plat, diffable entre deux journaux sans
        // outil.
        let cle = s"\(Cast<Int32>(p.X * 10.0));\(Cast<Int32>(p.Y * 10.0));\(Cast<Int32>(p.Z * 10.0))";
        // L'`EntityID` est journalisée EN PLUS de la position, pour tester F-ASC-010 sur les PNJ :
        // l'identifiant d'une entité STATIQUE y dérive du `GlobalNodeRef`, donc il devrait être
        // IDENTIQUE sur les deux machines. Si c'est vrai pour les statiques de foule, la clé
        // partagée que le chantier cherche existe déjà (F-PNJ-127) et il n'y a plus rien à
        // inventer pour les désigner.
        //
        // ⚠️ Attendu pour les MARCHEURS : des identifiants tous différents. Ils sont créés
        // dynamiquement, rien ne les rattache à un nœud de secteur. Le contraste entre les deux
        // populations est le résultat, pas l'égalité brute.
        reseau.Tessera_Journal(s"[Foule] \(cle);\(TDBID.ToNumber(this.GetRecordID()));\(this.GetCurrentAppearanceName());\(EntityID.ToDebugString(this.GetEntityID()))");

        let classement = new TesseraClassementFoule();
        classement.pantin = this;
        classement.cle = cle;
        GameInstance.GetDelaySystem(GetGameInstance()).DelayCallback(classement, 3.0, false);
    }
    return resultat;
}

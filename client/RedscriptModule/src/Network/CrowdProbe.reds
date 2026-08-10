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

        // ── RELEVÉ D'ÉTAT PÉRIODIQUE (2026-08-09) ────────────────────────────────────────────
        //
        // POURQUOI. Tout ce qu'on mesurait jusqu'ici portait sur l'INSTANT d'une application. Aucun
        // instrument ne disait ce que les deux clients AFFICHENT, ni si ça le reste. Or l'hydratation
        // marque une entité « faite » définitivement : si le PNJ est déchargé puis rechargé, le jeu
        // lui retire une apparence au hasard et personne ne la recorrige — la cohérence se
        // dégraderait en silence, exactement comme « ça ne s'hydrate pas naturellement ».
        //
        // La clé de jointure est `cle` (le point de NAISSANCE), la même que le classement : c'est
        // elle qui permet de recouper les deux journaux sans supposer que les EntityID concordent.
        let releve = new TesseraReleveEtatStatique();
        releve.cible = this.pantin.GetEntityID();
        releve.cle = this.cle;
        GameInstance.GetDelaySystem(GetGameInstance()).DelayCallback(releve, 30.0, false);

        let pos = this.pantin.GetWorldPosition();
        let connue = reseau.Tessera_ApparenceStatiqueConnue(this.pantin.GetEntityID());
        if IsNameValid(connue) {
            reseau.AppliquerApparenceStatique(this.pantin.GetEntityID(), connue);
            return;
        }
        // ⚠️ ON NE FILTRE PLUS PAR CELLULE — et c'est une correction, pas un relâchement.
        //
        // La version précédente se taisait dès que la cellule avait été servie une fois. Or une
        // cellule est servie dès le PREMIER rapport, alors que l'éclaireur n'a classé qu'une
        // poignée de ses PNJ (le classement prend 3 s chacun, ils arrivent au fil du streaming).
        // Tout le reste de la cellule n'était donc JAMAIS rapporté, par personne, à jamais.
        // Mesuré le 2026-08-09 : ~128 apparences connues du serveur pour ~207 statiques vus, et
        // une cohérence de 44 % entre deux clients — les deux chiffres se suivent.
        //
        // Le bon garde-fou n'est pas la cellule mais l'ENTITÉ, et il est juste au-dessus : si le
        // serveur connaît déjà ce PNJ, on a appliqué et on est déjà sorti. Arriver ici signifie
        // qu'il ne le connaît pas — donc que ce rapport apporte quelque chose.
        //
        // Le trafic reste borné : chaque client rapporte chaque entité au plus une fois, et les
        // suivants n'ont rien à dire puisqu'ils reçoivent la table. C'est le premier visiteur d'un
        // quartier qui paie, pas les deux mille suivants.
        reseau.Tessera_RapporterStatique(
            this.pantin.GetEntityID(),
            TDBID.ToNumber(this.pantin.GetRecordID()),
            this.pantin.GetCurrentAppearanceName(),
            pos.X, pos.Y, pos.Z, this.pantin.GetWorldYaw());
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
        // Le `yaw` est journalisé pour une raison précise (spec 2026-08-09 §5, cas 14) : si
        // l'orientation d'un statique diffère entre deux machines, un remplaçant spawné par le
        // client qui manque le PNJ regarderait ailleurs — visible, et rédhibitoire pour la voie de
        // complétion. Non mesuré à ce jour.
        // ⚠️ `EntityID.ToDebugString` ne rend PAS le numéro pour une entité dynamique : il écrit
        // « dynamic: » et rien d'autre. Comparer ce champ entre deux journaux donne donc 100 % de
        // concordance qui ne veut rien dire (piège payé le 2026-08-09).
        reseau.Tessera_Journal(s"[Foule] \(cle);\(TDBID.ToNumber(this.GetRecordID()));\(this.GetCurrentAppearanceName());\(EntityID.ToDebugString(this.GetEntityID()));\(Cast<Int32>(this.GetWorldYaw()))");

        let classement = new TesseraClassementFoule();
        classement.pantin = this;
        classement.cle = cle;
        GameInstance.GetDelaySystem(GetGameInstance()).DelayCallback(classement, 3.0, false);
    }
    return resultat;
}

// Relevé d'ÉTAT d'un PNJ statique, toutes les 30 s, tant qu'il est chargé.
//
// C'est le seul instrument qui mesure ce que le client AFFICHE, et dans la DURÉE. Tous les
// précédents mesuraient un instant (un ordre passé, une relecture à 3 s) — aucun ne pouvait voir
// une cohérence qui se DÉGRADE, ce qui est le mode de défaillance soupçonné : un PNJ déchargé puis
// rechargé reçoit une nouvelle apparence au hasard, et l'hydratation le croit déjà fait.
//
// ⚠️ Se ré-arme lui-même : `DelayCallback` est one-shot. Un relevé unique ne dirait rien de la
// durée, et c'est précisément la durée qui est en cause.
public class TesseraReleveEtatStatique extends DelayCallback {
    // Une EntityID, pas une `wref<ScriptedPuppet>`. Deux raisons, et la seconde est le coeur du
    // chantier : (1) une reference faible tombe des que le PNJ est decharge, alors que l'identifiant
    // reste valide et permet de le retrouver s'il revient ; (2) un REMPLACANT n'est pas `IsCrowd()`,
    // donc `CrowdProbe` ne le classe jamais — sans ce suivi par identifiant, la mesure ne verrait
    // pas les PNJ reparees et compterait la reparation comme un echec.
    let cible: EntityID;
    let cle: String;
    // Apparence que ce PNJ DOIT porter. Vide pour un natif (le serveur tranche ailleurs), posee
    // pour un remplacant — c'est nous qui l'avons fabrique, donc nous qui garantissons sa tenue.
    let attendue: CName;

    public func Call() -> Void {
        let reseau = GameInstance.GetNetworkGameSystem();
        if !IsDefined(reseau) {
            return;
        }
        let entite = GameInstance.FindEntityByID(GetGameInstance(), this.cible);
        let pantin = entite as ScriptedPuppet;
        // ── RECONCILIATION (2026-08-09) ──────────────────────────────────────────────────────
        // Ce releve ne se contente plus d'OBSERVER : s'il constate un ecart avec l'apparence
        // attendue, il le CORRIGE. Mesure qui l'a impose : deux clients avaient bien cree chacun
        // un remplacant pour le meme PNJ, meme record — et affichaient deux variantes DIFFERENTES.
        // `DynamicEntitySpec.appearanceName` n'est donc pas honore pour un record de foule : le
        // moteur tire dans le pool au hasard (F-PNJ-049). L'apparence doit etre imposee APRES le
        // spawn, par le chemin qui est mesure comme fonctionnel (ScheduleAppearanceChange).
        if IsDefined(pantin) && IsNameValid(this.attendue)
            && NotEquals(pantin.GetCurrentAppearanceName(), this.attendue) {
            reseau.AppliquerApparenceStatique(this.cible, this.attendue);
        }
        if !IsDefined(pantin) {
            // On le DIT et on continue de surveiller : un PNJ absent d'un journal et present dans
            // l'autre explique une divergence a lui seul, et il peut revenir.
            reseau.Tessera_Journal(s"[Etat] \(this.cle);0;decharge;decharge");
        } else {
            let voulue = reseau.Tessera_ApparenceStatiqueConnue(this.cible);
            reseau.Tessera_Journal(
                s"[Etat] \(this.cle);\(TDBID.ToNumber(pantin.GetRecordID()));\(pantin.GetCurrentAppearanceName());\(IsNameValid(voulue) ? NameToString(voulue) : "inconnue-du-serveur")");
        }
        let suivant = new TesseraReleveEtatStatique();
        suivant.cible = this.cible;
        suivant.cle = this.cle;
        suivant.attendue = this.attendue;
        GameInstance.GetDelaySystem(GetGameInstance()).DelayCallback(suivant, 30.0, false);
    }
}

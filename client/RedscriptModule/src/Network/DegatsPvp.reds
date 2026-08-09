module Cyberverse.Network.Managers

// Les dégâts entre joueurs : le client CALCULE, le serveur DÉCIDE.
//
// ── Ce qui manquait ───────────────────────────────────────────────────────────────────────────
//
// Tirer sur l'avatar d'un autre joueur ne lui faisait rien. La balle partait, l'avatar encaissait
// — mais chez le tireur, et seulement chez lui. La personne derrière cet avatar ne perdait pas un
// point de vie et ne voyait jamais l'écran de mort. Chaque client simulait sa propre version de
// la bagarre.
//
// ── Le partage du travail ─────────────────────────────────────────────────────────────────────
//
// Le moteur calcule très bien les dégâts : arme, mods, armure, critiques, zone touchée. Ce qu'il ne
// sait pas faire, c'est les faire exister ailleurs que sur la machine qui a tiré. On garde donc le
// calcul natif — on ne réimplémente RIEN — et on ne lui retire qu'une chose : le droit de conclure.
//
//   tireur : le moteur calcule les dégâts ─┐
//                                          ├─► serveur : écrête, cadence, tient la barre (sante.rs)
//   victime : écrit la barre que le serveur lui rend ─► 0 ─► mort NATIVE ─► écran de mort garni
//
// ── L'entonnoir ───────────────────────────────────────────────────────────────────────────────
//
// `ScriptedPuppet.DamagePipelineFinalized` : le moment, et le seul, où le pipeline de dégâts a fini
// de calculer et où `attackComputed` porte enfin le nombre définitif. Elle est déclarée EN PROPRE
// sur `ScriptedPuppet` (`scriptedPuppet.script:4909`), donc annotable, et elle s'exécute sur la
// CIBLE — `this` est donc l'avatar touché, sans avoir à le retrouver.
//
// Pourquoi pas `OnHit` : il arrive AVANT le calcul, `attackComputed` y est vide. Pourquoi pas
// `DamageSystem.DealDamages` : elle voit tous les dégâts du monde, il faudrait y refaire le test de
// cible que `this` nous donne gratuitement ici.
//
// ⚠️ `wrappedMethod()` D'ABORD et sans condition : on n'intercepte rien du tout, le pipeline vanilla
// se déroule entier. On ajoute un rapport, on ne retire pas un comportement.

@wrapMethod(ScriptedPuppet)
protected func DamagePipelineFinalized(evt: ref<gameHitEvent>) -> Void {
    wrappedMethod(evt);

    // ⚠️ `attackData` est testé au même titre que `attackComputed` : les deux sont des champs
    // `import` remplis par le pipeline natif, et rien ne garantit au script qu'ils le soient. Un
    // déréférencement nul ici ne donnerait pas une erreur, il ferait tomber le jeu.
    let reseau = GameInstance.GetNetworkGameSystem();
    if !IsDefined(reseau) || !IsDefined(evt) || !IsDefined(evt.attackComputed) || !IsDefined(evt.attackData) {
        return;
    }

    // ⚠️ SEULEMENT les coups portés par le JOUEUR LOCAL. Le serveur attribue les dégâts à
    // l'expéditeur du message : rapporter un coup qu'un PNJ a donné les mettrait sur notre compte,
    // et deux témoins d'une même bagarre les rapporteraient tous les deux — les dégâts seraient
    // comptés autant de fois qu'il y a de spectateurs.
    let local = GameInstance.GetPlayerSystem(GetGameInstance()).GetLocalPlayerControlledGameObject();
    let instigateur = evt.attackData.GetInstigator();
    if !IsDefined(local) || !IsDefined(instigateur) || instigateur.GetEntityID() != local.GetEntityID() {
        return;
    }

    // La santé, et pas la somme de tous les pools : le poise, l'endurance et les autres ne
    // descendent pas la barre de vie et n'ont rien à faire sur le fil.
    let degats = evt.attackComputed.GetTotalAttackValue(gamedataStatPoolType.Health);
    if degats <= 0.0 {
        return;
    }

    // `Tessera_RapporterDegats` traduit l'`EntityID` locale en identité réseau et ne renvoie rien
    // si la cible n'en a pas. C'est ce test — côté C++, où vit la table — qui écarte les figurants
    // de la foule native : on ne rapporte que ce que le serveur peut reconnaître.
    reseau.Tessera_RapporterDegats(this.GetEntityID(), Cast<Uint32>(degats));

    // ⚠️ CE FICHIER NE TOUCHE PLUS À LA MISE À TERRE — et l'histoire vaut d'être gardée.
    //
    // Il retirait ici trois status effects après coup (`BaseStatusEffect.Knockdown`,
    // `.KnockdownWithGetUp`, `.Stagger`) pour empêcher l'avatar de tomber puis de se relever. Deux
    // défauts, chacun suffisant :
    //
    //   1. **Deux des trois identifiants n'existent pas.** Zéro référence dans les scripts
    //      décompilés comme dans TweakDB (vérifié le 2026-08-09). Un `t"..."` inventé ne lève
    //      AUCUNE erreur — ni à la compilation, ni à l'exécution : il se résout en silence sur un
    //      enregistrement vide. Trois lignes qui se relisaient comme faites, dont deux inertes.
    //   2. **Même la bonne arrivait trop tard.** `GetReactionType` a déjà choisi son animation
    //      quand le pipeline nous rend la main : on retirait un statut sur un pantin déjà en train
    //      de tomber.
    //
    // La mise à terre se coupe désormais AVANT la décision, par le bouton natif prévu pour ça —
    // le stat `KnockdownImmunity`, posé une fois à l'attachement dans `AvatarNeutre.reds`, qui rend
    // la branche knockdown inatteignable dans `GetReactionType` sans rien retirer au pantin.
    //
    // La leçon, elle, n'est pas sur le knockdown : deviner un identifiant TweakDB au lieu de le
    // vérifier produit du code qui ne signale jamais son inefficacité.
}

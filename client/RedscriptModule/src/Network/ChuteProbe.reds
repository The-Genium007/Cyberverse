module Cyberverse.Network.Managers

// SONDE — pourquoi l'avatar d'un joueur distant tombe-t-il, et combien de fois ?
//
// ── Pourquoi une sonde plutôt qu'un correctif de plus ─────────────────────────────────────────
//
// Trois tentatives ont échoué sur ce défaut, chacune fondée sur un raisonnement plausible et
// aucune sur une mesure : retirer des status effects après coup (deux identifiants sur trois
// n'existaient pas, F-PNJ-145), puis couper la mise à terre par le stat `KnockdownImmunity`
// (F-PNJ-144). Après la troisième, Lucas rapporte « il y a encore DEUX chutes ».
//
// « Deux » est l'information neuve, et elle change la question. On ne cherche plus à empêcher UNE
// chute : on cherche à savoir laquelle des QUATRE familles se déclenche, dans quel ordre, et si le
// correctif posé a seulement pris. Aucune de ces réponses ne se lit dans le code — le natif décide,
// et le natif ne se relit pas.
//
// ── Les quatre familles, et le point où chacune se voit ───────────────────────────────────────
//
// Un pantin peut se retrouver au sol par quatre chemins INDÉPENDANTS. Sonder un seul et n'y rien
// voir ne prouve rien sur les trois autres — c'est exactement l'erreur des trois tentatives.
//
// | Famille | Ce qui la déclenche | Point d'observation |
// | --- | --- | --- |
// | animation de réaction | le moteur choisit `Knockdown`/`Ragdoll` après un coup | `HitReactionComponent.GetReactionType` — la valeur RETOURNÉE |
// | status effect | un `Defeated`, `DefeatedWithRecover`, `Unconscious`, `Knockdown` posé sur l'entité | `ScriptedPuppet.OnStatusEffectApplied` — traversé par TOUS, quel que soit le système qui l'a posé |
// | incapacité / mort | les PV atteignent le plancher, ou le serveur tue | `OnDefeated`, `OnIncapacitated`, `OnDeath` |
// | ragdoll physique | poussé par l'IA, la mort, un véhicule, un piège | `NPCPuppet.OnRagdollEnabledEvent` |
//
// ⚠️ AUCUN de ces hooks ne modifie quoi que ce soit. Tous appellent `wrappedMethod()` et se
// contentent d'écrire une ligne. Une sonde qui change le comportement qu'elle observe ne mesure
// plus rien — et ce dépôt a déjà payé ça (F-PNJ-141 : des composants éteints « pour voir »).
//
// ── Portée : les entités réseau ET le joueur local ────────────────────────────────────────────
//
// `GetReactionType` et `OnStatusEffectApplied` s'exécutent pour CHAQUE pantin de la ville, plusieurs
// fois par seconde. Sans filtre, le journal se remplit de la foule et devient illisible — donc
// inutile, ce qui est le seul vrai échec possible pour une sonde. Ce qu'on exclut, c'est la foule ;
// le joueur local, lui, est DANS la portée — voir `TesseraChuteSurveille` et l'angle mort qu'il
// corrige.
//
// ── Lire le résultat ──────────────────────────────────────────────────────────────────────────
//
// `Tessera_Journal` écrit dans le log du PLUGIN, donc **un fichier par instance** — indispensable
// ici, puisque la question est justement de savoir ce que voit chaque client. `FTLog` aurait
// mélangé les deux dans le gamelog CET partagé.
//
//   grep "\[Chute\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log
//
// À retirer une fois la cause établie et corrigée : une sonde laissée en place devient du bruit,
// puis du code que plus personne n'ose enlever.

// Une seule porte de journalisation, pour que le préfixe reste vraiment unique et greppable.
func TesseraJournalChute(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Chute] " + texte);
    }
}

// ⚠️ LE JOUEUR LOCAL EST DANS LA PORTÉE, ET C'EST LA CORRECTION D'UN ANGLE MORT.
//
// La première version de cette sonde ne filtrait que les entités réseau. Elle a produit un journal
// riche et inutile : Lucas voyait les deux chutes sur l'écran de la VICTIME, donc sur son propre
// pantin — qui n'est pas une entité réseau. La sonde regardait précisément le seul endroit où le
// défaut n'était pas.
//
// Un « surveillé » se lit donc ici : entité arbitrée par le serveur **ou** joueur local. Le second
// n'inonde pas le journal — il n'y en a qu'un, et il ne prend des coups que quand on lui tire
// dessus. C'est la foule locale qu'il fallait exclure, pas nous.
func TesseraChuteSurveille(cible: EntityID) -> Bool {
    if TesseraEstSousAutoriteServeur(cible) {
        return true;
    }
    let local = GameInstance.GetPlayerSystem(GetGameInstance()).GetLocalPlayerControlledGameObject();
    return IsDefined(local) && local.GetEntityID() == cible;
}

// ── FAMILLE 1 · L'ANIMATION CHOISIE PAR LE MOTEUR ─────────────────────────────────────────────
//
// LE point de décision : c'est la valeur retournée ici qui détermine si le pantin tressaille,
// titube ou s'écroule. On journalise le VERDICT du moteur, pas notre supposition de ce qu'il fera.
//
// On journalise aussi, sur la même ligne, la valeur du stat `KnockdownImmunity` — parce que la
// question « mon correctif a-t-il seulement pris ? » et la question « qu'a choisi le moteur ? » ne
// se répondent pas séparément. Si l'immunité vaut 0, le correctif n'est pas posé et le reste de la
// ligne n'a rien à expliquer. Si elle vaut 1 et que le moteur renvoie quand même `Knockdown`, alors
// c'est la chaîne native qui ne fonctionne pas comme lue, et c'est une découverte.
//
// Ordinaux de `animHitReactionType` (`core/animation/animFeature.script:118`) :
//   0=None 1=Twitch 2=Impact 3=Stagger 4=Pain 5=Knockdown 6=Ragdoll 7=Death 8=Block 9=GuardBreak
//   10=Parry 11=Bump
@wrapMethod(HitReactionComponent)
protected final func GetReactionType(guardBreakImpulse: Float, newHitEvent: ref<gameHitEvent>) -> animHitReactionType {
    let choix = wrappedMethod(guardBreakImpulse, newHitEvent);

    // ⚠️ `GetOwner()` testé, jamais supposé : cette fonction tourne pour toute la ville, y compris
    // pendant un détachement. Un propriétaire nul déréférencé ici ne donnerait pas une ligne
    // d'erreur, il ferait tomber le jeu — et une sonde qui fait tomber le jeu n'est pas une sonde.
    let proprietaire = this.GetOwner();
    if !IsDefined(proprietaire) || !TesseraChuteSurveille(proprietaire.GetEntityID()) {
        return choix;
    }

    let immunite = GameInstance.GetStatsSystem(GetGameInstance())
        .GetStatValue(Cast<StatsObjectID>(proprietaire.GetEntityID()), gamedataStatType.KnockdownImmunity);
    TesseraJournalChute(s"reaction=\(EnumInt(choix)) (5=Knockdown 6=Ragdoll 7=Death 3=Stagger 2=Impact 1=Twitch)"
        + s" · KnockdownImmunity=\(immunite)");
    return choix;
}

// ── FAMILLE 2 · LES STATUS EFFECTS ────────────────────────────────────────────────────────────
//
// Entonnoir de RÉCEPTION, et c'est ce qui en fait le bon point : le pipeline de dégâts pose ses
// statuts par deux voies distinctes (`StatusEffectHelper.ApplyStatusEffect` et
// `statusEffectSystem.ApplyStatusEffect`), dont une seule est annotable. Sonder l'émission serait
// donc à moitié aveugle. Ici, tout arrive, quelle que soit la voie.
//
// C'est la famille la plus prometteuse pour « deux chutes » : `Defeated` fait tomber sans tuer, et
// `DefeatedWithRecover` fait tomber PUIS RELEVER — ce qui décrirait exactement ce que Lucas voit.
// Un avatar immortel (le serveur seul décide de sa mort) est précisément le cas où le moteur
// choisit d'incapaciter au lieu de tuer.
//
// Ordinaux utiles de `gamedataStatusEffectType` : on journalise l'identifiant EN TOUTES LETTRES via
// `TDBID.ToStringDEBUG`, pour ne pas avoir à les décoder de mémoire — c'est en devinant des noms
// d'identifiants qu'on a perdu les trois tentatives précédentes (F-PNJ-145).
@wrapMethod(ScriptedPuppet)
protected cb func OnStatusEffectApplied(evt: ref<ApplyStatusEffectEvent>) -> Bool {
    let resultat = wrappedMethod(evt);
    if !IsDefined(evt) || !TesseraChuteSurveille(this.GetEntityID()) {
        return resultat;
    }
    let donnees = evt.staticData;
    if !IsDefined(donnees) {
        TesseraJournalChute("statut applique — staticData nulle (cas non prevu, a signaler)");
        return resultat;
    }
    TesseraJournalChute(s"statut=\(TDBID.ToStringDEBUG(donnees.GetID()))"
        + s" type=\(EnumInt(donnees.StatusEffectType().Type()))"
        + s" nouveau=\(evt.isNewApplication) auSpawn=\(evt.isAppliedOnSpawn)");
    return resultat;
}

// ── FAMILLE 3 · INCAPACITÉ ET MORT ────────────────────────────────────────────────────────────
//
// Trois portes distinctes, journalisées séparément : leur ORDRE est l'information. « Defeated puis
// Death » et « Death seule » ne décrivent pas le même défaut et n'appellent pas le même correctif.
@wrapMethod(ScriptedPuppet)
protected cb func OnDefeated(evt: ref<DefeatedEvent>) -> Bool {
    if TesseraChuteSurveille(this.GetEntityID()) {
        TesseraJournalChute("DEFEATED (mis a terre sans etre tue)");
    }
    return wrappedMethod(evt);
}

@wrapMethod(ScriptedPuppet)
protected func OnIncapacitated() -> Void {
    if TesseraChuteSurveille(this.GetEntityID()) {
        TesseraJournalChute("INCAPACITATED");
    }
    wrappedMethod();
}

@wrapMethod(ScriptedPuppet)
protected cb func OnDeath(evt: ref<gameDeathEvent>) -> Bool {
    if TesseraChuteSurveille(this.GetEntityID()) {
        TesseraJournalChute("DEATH (mort effective du pantin)");
    }
    return wrappedMethod(evt);
}

// ── FAMILLE 4 · LE RAGDOLL PHYSIQUE ───────────────────────────────────────────────────────────
//
// Indépendante des trois autres : l'IA, la mort, un véhicule ou un piège peuvent le pousser
// directement, sans passer ni par une animation de réaction ni par un status effect. C'est la
// famille qu'on oublie, et donc celle qui explique les chutes qu'on ne s'explique pas.
@wrapMethod(NPCPuppet)
protected cb func OnRagdollEnabledEvent(evt: ref<RagdollNotifyEnabledEvent>) -> Bool {
    if TesseraChuteSurveille(this.GetEntityID()) {
        TesseraJournalChute("RAGDOLL active");
    }
    return wrappedMethod(evt);
}

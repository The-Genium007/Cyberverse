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
// ⚠️ **On ne VIDE pas l'inventaire, et c'est délibéré.** « Rien dedans » se lit naturellement comme
// « effacer le contenu du corps » — sauf que l'inventaire d'un PNJ EST son équipement : le vider le
// déshabille et le désarme. Fermer l'accès donne le résultat voulu (rien n'est récupérable) sans
// produire une ville de PNJ nus.
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
@wrapMethod(ScriptedPuppet)
private func DropHeldItems() -> Bool {
    return false;
}

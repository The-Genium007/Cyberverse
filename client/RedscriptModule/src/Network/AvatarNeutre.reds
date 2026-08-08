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
// ── Le point de décision ──────────────────────────────────────────────────────────────────────
//
// Ni le stimulus émis (il vaut pour toute la foule, on ne veut pas y toucher), ni la réaction jouée.
// C'est la RÉCEPTION du stimulus par le pantin : `ReactionManagerComponent.HandleStimEvent` est
// l'entonnoir unique — `OnEventReceived`, seul point d'entrée des `StimuliEvent`, y mène
// (`reactionComponent.script:1442`), et le rejeu différé aussi (`:2970`).
//
// Et le moteur y porte DÉJÀ son propre interrupteur, en tête de fonction (`:664`) :
//
//     if( !( IsEnabled() ) && !( StimFilters.IsForTheDead( stimType ) ) ) { … return; }
//
// On n'écrit donc aucun hook de réaction : on éteint le composant et le natif fait le tri. Noter
// que les stimulus « pour les morts » passent quand même — ce qui est voulu, la promotion d'un
// cadavre en dépend.
//
// Même raisonnement pour l'interaction : `IComponent.Toggle(false)` sur le composant retire
// l'invite d'un coup, là où éteindre les couches une à une raterait des chemins (neuf couches dans
// `ToggleInteractionLayers`, et d'autres les rallument à la mort et à la saisie).
//
// ── Portée : toute entité réseau, pas seulement les avatars ───────────────────────────────────
//
// `Tessera_EstEntiteReseau` couvre les avatars de joueurs ET les PNJ promus. C'est délibéré et
// c'est la même règle : ce que le serveur arbitre ne doit pas être décidé en double par l'IA
// locale. Une entité promue qui panique chez un client et pas chez l'autre est exactement le bug
// qu'on vient de corriger sur les avatars.
//
// ⚠️ Conséquence sur le butin : un cadavre promu n'est plus lootable localement — sans importance
// depuis `LootOff.reds`, qui coupe le butin sur TOUS les pantins, pour la même raison de fond (pas
// d'autorité serveur sur l'inventaire).
//
// ⚠️ La foule LOCALE n'est pas touchée — c'est elle qui porte la panique répliquée (F-PNJ-115), et
// elle continue de réagir normalement.
//
// Entonnoir, garde-fou natif et fausses pistes : **F-PNJ-133** (`docs/connaissances/moteur-pnj-foule.md`).

@wrapMethod(ScriptedPuppet)
protected cb func OnGameAttached() -> Bool {
    // `wrappedMethod()` en premier et sans condition : on n'intercepte pas l'attachement, on ajoute
    // seulement une extinction. Les composants sont résolus avant (`OnTakeControl`), donc ils sont
    // là ; c'est bien `OnGameAttached` qui est le premier moment où on peut les éteindre.
    let resultat = wrappedMethod();

    let reseau = GameInstance.GetNetworkGameSystem();
    if !IsDefined(reseau) || !reseau.Tessera_EstEntiteReseau(this.GetEntityID()) {
        return resultat;
    }

    let reactions = this.GetStimReactionComponent();
    if IsDefined(reactions) {
        reactions.Toggle(false);
    }

    // Boucle plutôt que `FindComponentByType` : rien ne garantit qu'un pantin n'en porte qu'un, et
    // en rater un rendrait l'invite par intermittence — panne bien plus coûteuse à diagnostiquer
    // qu'un parcours de composants fait UNE fois, à l'attachement.
    for composant in this.GetComponents() {
        if IsDefined(composant as InteractionComponent) {
            composant.Toggle(false);
        }
    }

    return resultat;
}

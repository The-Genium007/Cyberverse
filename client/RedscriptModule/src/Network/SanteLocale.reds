module Cyberverse.Network.Managers

// Ce que le serveur ne peut pas voir de notre vie — et qu'il faut donc lui dire.
//
// ── Pourquoi ce fichier existe ────────────────────────────────────────────────────────────────
//
// Le serveur ne connaît qu'UNE cause de variation de vie : le coup d'un autre joueur. Régénération,
// soin, chute, feu, PNJ, véhicule se produisent dans le moteur du client et n'existent pas pour lui.
// Deux pannes observables en découlent (spec 2026-08-09 §2) :
//
//   · la barre qu'on regagne REDESCEND au coup suivant — le serveur diffuse son ancien chiffre ;
//   · on tombe d'un toit, on meurt chez soi, et on reste vivant pour tout le monde.
//
// ── Comment on le mesure : par SONDAGE, et c'est un choix ─────────────────────────────────────
//
// Il n'existe pas d'entonnoir scripté unique pour « ma vie a changé » : elle bouge par le pipeline
// de dégâts, par les effets de statut, par la régénération native, par un consommable. Plutôt que
// d'annoter cinq chemins et d'en oublier un sixième, on LIT le pool deux fois par seconde et on
// compare. Un sondage rate la forme de la variation ; il ne rate pas son résultat, et c'est le
// résultat qui compte ici.
//
// ⚠️ Le rythme n'est PAS le débit du fil. Rien ne part sous le seuil (2 % de la barre, côté C++) :
// une régénération lente n'émet presque rien, un soin brutal part tout de suite. C'est la règle
// « un seuil, pas une cadence » de la spec.
//
// ── Le piège désamorcé, et il est structurel ──────────────────────────────────────────────────
//
// Le SERVEUR écrit aussi cette barre (`AppliquerSanteJoueur`, sur `HealthSync`). Sans garde, on
// observerait son écriture, on la lui renverrait comme une variation locale, il la réappliquerait :
// une boucle qui diverge. La référence de comparaison vit donc côté C++ et `HealthSync` la met à
// jour lui-même — l'écriture serveur est ainsi invisible à cette détection. Ne jamais dupliquer
// cette référence ici : deux références dérivent.

@wrapMethod(PlayerPuppet)
protected cb func OnGameAttached() -> Bool {
  let resultat = wrappedMethod();
  // Le premier passage n'émet rien : il établit la référence (garde côté C++). C'est aussi pour ça
  // qu'on démarre à l'attachement plutôt qu'au chargement — la vie n'a pas de valeur utile avant.
  GameInstance.GetDelaySystem(GetGameInstance())
    .DelayCallback(TesseraSondeSanteLocale.Creer(), 0.5, false);

  // ── LA MORT DU JOUEUR LOCAL APPARTIENT AU SERVEUR, COMME CELLE DES AVATARS ──────────────────
  //
  // ⚠️ MESURÉ, pas déduit — c'est la trace qui a désigné ce point, après trois correctifs posés au
  // mauvais endroit. Journal du client de la VICTIME, 2026-08-09 :
  //
  //   22:01:43.330  serveur 0 %  · local 12.5 %  · écriture true   ← on écrit 0 → CHUTE 1
  //   22:01:43.494  serveur 0 %  · local 0 %     · écriture false
  //   22:01:48.518  serveur 0 %  · local 100 %   · écriture true   ← LE MOTEUR A RENDU 100 %
  //   22:01:49.481  serveur 0 %  · local 0 %     · écriture false  ← on réécrit 0 → CHUTE 2
  //
  // Cinq secondes après la mort, le jeu **rend 100 % de vie au joueur de sa propre initiative** :
  // c'est sa séquence native de mort-puis-réapparition. Notre réconciliation, idempotente et donc
  // auto-corrective, constate `local 100 ≠ serveur 0` et réécrit 0 — d'où la deuxième chute. Les
  // deux moitiés du système faisaient chacune leur travail correctement, et se combattaient.
  //
  // La cause n'était donc ni le knockdown (F-PNJ-144 : `KnockdownImmunity=1` était bien posé et le
  // moteur n'a jamais choisi `Knockdown`), ni un status effect de réaction. C'était qu'on LAISSAIT
  // TOURNER la machinerie de mort native du joueur.
  //
  // ── CE QUI RESSUSCITAIT LE JOUEUR : LE SECOND CŒUR ───────────────────────────────────────────
  //
  // La sonde a nommé le coupable, et ce n'était aucune des trois pistes précédentes :
  //
  //   22:14:58.038  [Chute] DEATH                                 ← chute 1
  //   22:15:01.116  statut=BaseStatusEffect.SecondHeart           ← ★ le cyberware
  //   22:15:03.144  statut=BaseStatusEffect.SecondHeartCooldown
  //   22:15:03.454  serveur 0 % · local 100 %  · écriture true    ← il rend toute la vie
  //   22:15:03.458  [Chute] DEATH                                 ← chute 2
  //
  // Le **Second Cœur** est un cyberware qui ressuscite son porteur une fois, à pleine vie. Il
  // explique les cinq secondes, le 100 %, et la deuxième chute — que notre réconciliation
  // provoquait en corrigeant, correctement, une vie qui ne devait pas être remontée.
  //
  // ── Pourquoi `ForcePreventResurrect` et pas « couper le Second Cœur » ────────────────────────
  //
  // `SetIsResurrectionAllowedBasedOnState` (`psm/highLevelTransitions.script:228`) teste, DANS CET
  // ORDRE :
  //
  //   1. `ForcePreventResurrect == 0` — sinon on saute directement à « pas de résurrection »
  //   2. seulement ensuite : le Second Cœur, l'état de locomotion, `ForceKill`…
  //
  // Viser le premier test plutôt que le second couvre **toute la famille** — Second Cœur
  // aujourd'hui, n'importe quel autre implant ou perk de résurrection demain — au lieu de courir
  // derrière chacun. Et ce n'est pas un détournement : CDPR s'en sert lui-même dans Phantom Liberty
  // (`cerberus_status_effects.tweak`) pour interdire la résurrection quand un Cerberus vous tue.
  // Même sémantique, même bouton.
  //
  // Dans un serveur autoritaire, la question ne se discute même pas : décider seul, chez soi, qu'on
  // n'est pas mort est précisément ce qu'un client n'a pas le droit de faire.
  //
  // ── ⚠️ CE QUI A ÉTÉ ESSAYÉ ICI ET QUI NE MARCHE PAS — deux verdicts, gardés pour personne ne les
  //    refasse ─────────────────────────────────────────────────────────────────────────────────
  //
  // · **`AddGodMode(Immortal)` sur le joueur local : SANS EFFET.** La mort est survenue quand même
  //   (`[Chute] DEATH` journalisé alors que le god mode était posé). La raison est dans le nom de
  //   notre propre écriture : `RequestSettingStatPoolValueIgnoreChangeMode` **contourne les modes
  //   de changement**, donc les protections. Le jeu lui-même s'y heurte — le Second Cœur pose un
  //   `EnableOverride(Immortal)` (`playerListeners.script:633`) qui n'a pas davantage empêché cette
  //   mort-là.
  // · **`ApplyStatusEffect(BaseStatusEffect.Defeated)` sur le joueur : JAMAIS APPLIQUÉ.** Aucune
  //   trace dans le journal, alors que la sonde capte tous les autres statuts du joueur. L'état
  //   `Defeated` est un état de PANTIN ; le joueur a sa propre machine à états.
  //
  // Les deux compilaient, et les deux se relisaient comme faits. C'est la sonde qui les a démasqués,
  // pas la relecture.
  // ⚠️⚠️ `this`, PAS `GetLocalPlayerControlledGameObject()` — ET C'EST LA CAUSE DE TROIS CORRECTIFS
  // INERTES D'AFFILÉE.
  //
  // Les trois tentatives précédentes sur ce fichier (`Immortal`, puis `Defeated`, puis
  // `ForcePreventResurrect`) cherchaient le joueur par le `PlayerSystem`, **pendant son propre
  // `OnGameAttached`** — c'est-à-dire avant qu'il y soit enregistré. `GetLocalPlayerControlledGameObject()`
  // renvoie alors `null`, le `if IsDefined(joueur)` est faux, et le bloc entier est sauté **sans
  // rien signaler**. Trois correctifs différents, un seul et même défaut, invisible à la lecture :
  // le code était juste, il ne s'exécutait pas.
  //
  // Le fichier contenait pourtant déjà l'indice : la sonde de santé, juste au-dessus, s'arme avec
  // 0,5 s de retard précisément parce que rien n'est prêt à l'attachement. Je ne l'ai pas lu.
  //
  // Dans un `@wrapMethod(PlayerPuppet)`, `this` EST le pantin du joueur, valide immédiatement —
  // c'est d'ailleurs ce que fait `AvatarNeutre.reds` pour les avatars, et son `KnockdownImmunity`
  // est le seul de ces correctifs qui ait jamais fonctionné. La différence était là, et nulle part
  // ailleurs.
  let stats = GameInstance.GetStatsSystem(GetGameInstance());
  let moi = Cast<StatsObjectID>(this.GetEntityID());

  // 1. Interdiction générale de résurrection (le premier test de `SetIsResurrectionAllowedBasedOnState`).
  stats.AddModifier(moi, RPGManager.CreateStatModifier(
      gamedataStatType.ForcePreventResurrect, gameStatModifierType.Additive, 1.0));

  // 2. ⚠️ CONTRÔLE TEMPORAIRE, DEMANDÉ PAR LUCAS LE 2026-08-09 — À RETIRER APRÈS VERDICT.
  //
  // On coupe le Second Cœur À LA SOURCE, en plus de l'interdiction ci-dessus. Ce n'est pas une
  // ceinture-et-bretelles par confort : c'est une EXPÉRIENCE DE CONTRÔLE. Si les deux chutes
  // disparaissent, on saura que toute la chaîne de mort était déjà correcte et que cet implant
  // était le seul défaut restant — hypothèse de Lucas, et elle vaut d'être tranchée nettement
  // plutôt que noyée dans un correctif qui marche « pour une raison ou une autre ».
  //
  // `Multiplier 0` et non `Additive -1` : le stat est accordé DEUX fois (l'`OnEquip` du cyberware
  // et l'`abilityPackage` de `HasSecondHeart`, +1 chacun). Un `-1` laisserait 1 et on chercherait
  // pourquoi. Un multiplicateur nul est insensible au nombre de sources.
  //
  // ⚠️ Ceci retire une capacité PAYÉE par le joueur. Ça n'a rien à faire dans un serveur de
  // production tel quel : la bonne forme sera un arbitrage SERVEUR (l'implant demande, le serveur
  // accorde ou refuse la résurrection), pas une amputation silencieuse côté client.
  stats.AddModifier(moi, RPGManager.CreateStatModifier(
      gamedataStatType.HasSecondHeart, gameStatModifierType.Multiplier, 0.0));

  return resultat;
}

// Sondage de la vie locale, deux fois par seconde, en boucle tant que le joueur existe.
//
// Se ré-arme TOUJOURS en dernier et sans condition — la leçon du battement de l'écran de mort
// (2026-08-09) : un ré-armement enfermé dans un test s'arrête un jour, et personne ne comprend
// pourquoi l'affichage s'est figé.
public class TesseraSondeSanteLocale extends DelayCallback {
  public static func Creer() -> ref<TesseraSondeSanteLocale> {
    return new TesseraSondeSanteLocale();
  }

  public func Call() -> Void {
    let jeu = GetGameInstance();
    let reseau = GameInstance.GetNetworkGameSystem();
    let joueur = GameInstance.GetPlayerSystem(jeu).GetLocalPlayerControlledGameObject();

    if IsDefined(reseau) && IsDefined(joueur) {
      // `perc = true` : le pool est lu en POURCENTAGE, jamais en points. Les points dépendent du
      // maximum de vie du joueur — donc de son chrome — et ne seraient pas comparables d'une
      // session à l'autre, ni d'un joueur à l'autre. Une fraction l'est.
      let pourcent = GameInstance.GetStatPoolsSystem(jeu)
        .GetStatPoolValue(Cast<StatsObjectID>(joueur.GetEntityID()), gamedataStatPoolType.Health, true);
      // `cause = 0` (inconnu) : ce sondage voit le RÉSULTAT, pas l'origine. Inventer une cause
      // serait un mensonge sur le fil, et le serveur n'en a pas besoin pour arbitrer.
      reseau.Tessera_RapporterVariation(pourcent, 0u);

      // ── LA RÉGÉNÉRATION NATIVE EST COUPÉE ICI ─────────────────────────────────────────────
      //
      // Décision de Lucas dès l'ouverture du chantier : « pas de régénération native ». Mesurée le
      // 2026-08-09 avant d'être coupée, et le chiffre justifie la décision : **+32 pour mille par
      // seconde**, sans interruption — elle efface un tir de 133 points en quatre secondes et
      // remplit la barre en une trentaine. C'est elle, et pas un bug de réplication, qui donnait
      // l'impression que « le personnage se relève quand il reçoit des dégâts ».
      //
      // ⚠️ PAR LE CODE, PAS PAR UN FLAT TweakDB. Le fichier `config-overrides.toml` porte la leçon :
      // `SetFlat` ACCEPTE des chemins inexistants — six flats inventés ont été « appliqués » sans
      // rien commander (F-PLF-021). Deviner `BaseStatPools.PlayerBase*HealthRegen.value` aurait
      // donné un succès de journal et zéro effet. `RequestSettingModifier` agit sur le pool réel.
      //
      // ⚠️ RÉAPPLIQUÉE À CHAQUE PASSE, et c'est délibéré : le jeu repose son propre modificateur de
      // régénération à chaque changement d'état de combat (`ModifyStatPoolModifierEffector`). Une
      // coupe posée une seule fois serait défaite à la première entrée en combat — exactement le
      // genre de correctif qui « marche au test » et lâche en jeu.
      // ⚠️ LA PLAGE COUVRE TOUTE LA BARRE, ET C'EST LE PIÈGE QUE J'AI PAYÉ.
      //
      // Première version : `rangeBegin = 0` ET `rangeEnd = 0`, en croyant décrire « aucun effet ».
      // Le moteur l'a lu comme « ramène la barre vers 0 » et a vidé le joueur à -32 pour mille
      // toutes les deux ou trois secondes (mesuré le 2026-08-09 : 779 → 510 sans qu'on lui tire
      // dessus). `enabled = false` ne l'a pas empêché.
      //
      // La bonne façon de dire « pas de régénération » n'est pas une plage vide : c'est une plage
      // COMPLÈTE avec un débit NUL. Le modificateur existe, couvre 0 à 100 %, et ne déplace rien.
      // Un modificateur qui ne s'applique nulle part laisse le champ libre à celui du jeu ; un
      // modificateur à débit nul le remplace.
      let modif: StatPoolModifier;
      modif.enabled = true;
      modif.rangeBegin = 0.0;
      modif.rangeEnd = 100.0;
      modif.startDelay = 0.0;
      modif.valuePerSec = 0.0;
      modif.delayOnChange = false;
      GameInstance.GetStatPoolsSystem(jeu).RequestSettingModifier(
        Cast<StatsObjectID>(joueur.GetEntityID()),
        gamedataStatPoolType.Health,
        gameStatPoolModificationTypes.Regeneration,
        modif);
    }

    GameInstance.GetDelaySystem(jeu)
      .DelayCallback(TesseraSondeSanteLocale.Creer(), 0.5, false);
  }
}

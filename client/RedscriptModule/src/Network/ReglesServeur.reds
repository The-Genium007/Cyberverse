module Cyberverse.Network.Managers

// LES RÈGLES DU SERVEUR — ce que l'opérateur décide, et que le client obéit.
//
// ── Le besoin ─────────────────────────────────────────────────────────────────────────────────
//
// Lucas, 2026-08-26 : des PNJ hostiles cassaient la voiture pendant une mesure, et le seul levier
// existant était une case à cocher du harnais CET — un outil de développement, jamais livré aux
// joueurs. *« Fais une belle implémentation serveur pour qu'on puisse désactiver côté serveur. »*
//
// ⚠️ Et sa remarque de fond, qui est la vraie raison : **ces PNJ n'ont aucune réplication d'état**.
// Un PNJ hostile qui n'existe que chez un client casse une voiture que l'autre voit intacte. Il ne
// produit pas du jeu, il produit de la divergence — précisément là où on essaie d'en mesurer.
//
// ── Pourquoi ce canal, et pas un nouveau ─────────────────────────────────────────────────────
//
// `ConfigSync` existe déjà : le serveur pousse des valeurs à chaque client à l'entrée en session,
// et le jeu les applique À CHAUD (F-PLF-018, mesuré avec contrôle positif). Il est conçu pour des
// flats TweakDB, mais il transporte un **couple (chemin, flottant)** — et c'est tout ce dont une
// règle a besoin.
//
// On réserve donc l'espace de noms `Tessera.` : le client l'intercepte AVANT TweakDB et le traite
// comme une règle serveur. Aucun changement de protocole, aucun C++, aucune régénération d'en-tête.
//
// ⚠️ La convention est la seule chose qui tienne les deux bouts ensemble, et rien ne la vérifie :
// un chemin `Tessera.X` que personne n'interprète serait refusé par TweakDB et journalisé
// (« ConfigSync : « X » refuse par TweakDB »). C'est le filet — le lire après un ajout.

public class TesseraReglesServeur extends ScriptableSystem {
  /// ⚠️ VALEUR PAR DÉFAUT = COMPORTEMENT VANILLA, et ce n'est pas un détail. Tant que le serveur
  /// n'a rien dit, le jeu se comporte comme le jeu. Un défaut « tout désactivé » ferait qu'un
  /// serveur qui ne parle pas de règles neutraliserait ses PNJ sans l'avoir demandé — et personne
  /// ne saurait pourquoi.
  private let m_hostilitePnj: Bool = true;
  /// Le serveur a-t-il parlé ? Distinct de la valeur elle-même : « le serveur impose `true` » et
  /// « le serveur n'a rien dit » donnent le même booléen et ne veulent pas dire la même chose.
  /// Sans cette distinction, on ne peut pas savoir si une règle a été reçue.
  private let m_recu: Bool = false;

  public static func Get(game: GameInstance) -> ref<TesseraReglesServeur> {
    return GameInstance.GetScriptableSystemsContainer(game).Get(n"Cyberverse.Network.Managers.TesseraReglesServeur") as TesseraReglesServeur;
  }

  /// Applique une règle. Rend `false` si le nom n'est pas reconnu — l'appelant retombe alors sur
  /// TweakDB, ce qui garde le canal utilisable pour ce à quoi il servait.
  public func Poser(nom: String, valeur: Float) -> Bool {
    if Equals(nom, "Tessera.HostilitePnj") {
      let avant = this.m_hostilitePnj;
      this.m_hostilitePnj = valeur != 0.0;
      this.m_recu = true;
      TesseraReglesLog(s"HostilitePnj = \(this.m_hostilitePnj) (etait \(avant))");
      this.AppliquerHostilite();
      return true;
    }
    TesseraReglesLog(s"regle INCONNUE : \(nom) — ignoree");
    return false;
  }

  public func HostilitePnj() -> Bool {
    return this.m_hostilitePnj;
  }

  /// Remet les relations de groupe à neutre (ou à hostile) DANS LA SESSION EN COURS.
  ///
  /// ⚠️ Ceci ne suffit PAS à soi seul, et c'est mesuré : rester dans la zone d'un gang, ou l'avoir
  /// frappé une fois, finit par le repasser hostile malgré la relation neutre — un autre chemin du
  /// jeu (stimulus, détection de menace) re-déclenche l'escalade indépendamment de la relation de
  /// base (Lucas, 2026-07-05). C'est pour ça que le crochet sur l'entonnoir d'escalade, plus bas,
  /// est indispensable : les deux se complètent, aucun ne remplace l'autre.
  private func AppliquerHostilite() -> Void {
    let systeme = GameInstance.GetAttitudeSystem(this.GetGameInstance());
    if !IsDefined(systeme) {
      TesseraReglesLog("AttitudeSystem injoignable — relations de groupe non touchees");
      return;
    }
    let attitude: EAIAttitude;
    if this.m_hostilitePnj {
      attitude = EAIAttitude.AIA_Hostile;
    } else {
      attitude = EAIAttitude.AIA_Neutral;
    }
    let joueur = t"Attitudes.Group_Player";

    // ⚠️ LES QUATRE PREMIERS SONT LES STRUCTURELS, ET ILS MANQUAIENT à la liste du désossage.
    // `Group_Hostile` couvre PAR HÉRITAGE tout groupe dont il est le parent — c'est de très loin
    // le plus couvrant, et il était absent. Sans lui, on neutralisait seize gangs nommés en
    // laissant hostile la catégorie qui les englobe.
    let groupes: array<TweakDBID>;
    ArrayPush(groupes, t"Attitudes.Group_Hostile");
    ArrayPush(groupes, t"Attitudes.Group_EnemyHostile");
    ArrayPush(groupes, t"Attitudes.Group_SecurityHostile");
    // ⚠️ `Group_PoliceHostile` est DÉLIBÉRÉMENT absent : la police a son propre levier
    // (`PreventionSystem`, DesossageOrder.reds), et mélanger les deux périmètres rendrait
    // impossible de savoir lequel a agi quand l'un des deux ne marche pas.

    // Les gangs nommés, chacun avec sa variante monde ouvert.
    ArrayPush(groupes, t"Attitudes.Group_Maelstrom");
    ArrayPush(groupes, t"Attitudes.Group_Maelstrom_OW");
    ArrayPush(groupes, t"Attitudes.Group_TygerClaws");
    ArrayPush(groupes, t"Attitudes.Group_TygerClaws_OW");
    ArrayPush(groupes, t"Attitudes.Group_Animals");
    ArrayPush(groupes, t"Attitudes.Group_Animals_OW");
    ArrayPush(groupes, t"Attitudes.Group_Scavenger");
    ArrayPush(groupes, t"Attitudes.Group_Scavenger_OW");
    ArrayPush(groupes, t"Attitudes.Group_Valentinos");
    ArrayPush(groupes, t"Attitudes.Group_Valentinos_OW");
    ArrayPush(groupes, t"Attitudes.Group_VoodooBoys");
    ArrayPush(groupes, t"Attitudes.Group_VoodooBoys_OW");
    ArrayPush(groupes, t"Attitudes.Group_6thStreet");
    ArrayPush(groupes, t"Attitudes.Group_6thStreet_OW");
    ArrayPush(groupes, t"Attitudes.Group_Aldecaldos");
    ArrayPush(groupes, t"Attitudes.Group_Aldecaldos_OW");

    let i: Int32 = 0;
    while i < ArraySize(groupes) {
      systeme.SetAttitudeRelationFromTweak(groupes[i], joueur, attitude);
      i += 1;
    }
    TesseraReglesLog(s"relations de groupe posees : \(ArraySize(groupes)) groupe(s)");
  }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// L'ENTONNOIR D'ESCALADE — un seul crochet ferme vingt et un sites
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// `AIActionHelper.TryChangingAttitudeToHostile(owner, target)` est le point par lequel passe toute
// l'hostilité ACQUISE : riposte, alerte, stimulus, escouade, quickhack subi, véhicule percuté.
// Vingt et un sites y entonnent (relevé exhaustif, 2026-08-26) — et on annote le CORPS, pas les
// sites, donc un seul crochet les ferme tous.
//
// ⚠️ Pourquoi PAS la lecture (`GetAttitudeTowards`) : ~90 sites aux usages hétérogènes — scanner,
// dégâts, interface, ciblage. La hooker toucherait tout ça pour n'en viser qu'une part.
//
// ⚠️ Un second crochet existe déjà sur cette même fonction (`DesossageHostility.reds:83`), gardé
// par la config du harnais CET. Les deux coexistent : redscript les chaîne. Celui-ci est gardé par
// le SERVEUR, ce qui le rend valable en production — l'autre est un outil de développement.

@wrapMethod(AIActionHelper)
public final static func TryChangingAttitudeToHostile(owner: ref<ScriptedPuppet>, target: ref<GameObject>) -> Bool {
  if IsDefined(target) && target.IsPlayer() {
    let regles = TesseraReglesServeur.Get(GetGameInstance());
    if IsDefined(regles) && !regles.HostilitePnj() {
      return false;
    }
  }
  return wrappedMethod(owner, target);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// LA VOIE CONCURRENTE — celle qui contourne l'entonnoir
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// `GameObject.ChangeAttitudeToHostile` écrit l'attitude SANS passer par l'entonnoir ci-dessus.
// Trois appelants, dont une commande d'IA venue des quêtes et des scènes.
//
// ⚠️ « Une désactivation qui laisse une voie de réactivation non gardée est un bug de réplication
// en attente, pas un raccourci » — c'est la règle de la prise d'autorité, et c'est exactement ce
// cas : sans ce second crochet, une scène de quête rendrait un PNJ hostile et le premier crochet
// n'en saurait rien.

@wrapMethod(GameObject)
public final static func ChangeAttitudeToHostile(instigator: wref<GameObject>, target: wref<GameObject>) -> Void {
  if IsDefined(target) && target.IsPlayer() {
    let regles = TesseraReglesServeur.Get(GetGameInstance());
    if IsDefined(regles) && !regles.HostilitePnj() {
      return;
    }
  }
  wrappedMethod(instigator, target);
}

public static func TesseraReglesLog(message: String) -> Void {
  FTLog(s"[Tessera/Regles] \(message)");
}

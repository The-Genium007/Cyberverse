module Cyberverse.Network.Managers

// La casse d'un véhicule : le conducteur MESURE, le serveur DÉCIDE, tout le monde VOIT.
//
// ── Ce qui manquait ───────────────────────────────────────────────────────────────────────────
//
// Emboutir une voiture ne l'abîmait que chez soi. Le moteur calculait très bien la casse — impact,
// force, angle, pièces touchées — et ce calcul mourait sur la machine qui avait tapé. Un joueur
// voyait une épave là où son voisin voyait une carrosserie neuve.
//
// Et côté serveur, le verbe existait DÉJÀ : `EntityInteraction` kind=12 est écouté depuis le
// 2026-08-15, avec ses tests verts et sa monotonie. **Aucun client ne l'a jamais envoyé.** C'est
// exactement la panne des postures (kind 13, écouté depuis le 19 août, jamais émis) : du code
// serveur juste, testé, et injoignable. Un canal montant qui manque ne casse rien et ne dit rien.
//
// ── Le partage du travail ─────────────────────────────────────────────────────────────────────
//
// On ne réplique NI la tôle NI la géométrie de l'impact — on réplique **un nombre**.
//
//   conducteur : le moteur calcule les PV ─┐
//                                          ├─► serveur : écrête, rend MONOTONE, persiste
//   témoins    : écrivent les PV que le serveur leur donne ─► le moteur déroule le reste
//
// La dernière flèche est ce qui rend l'approche rentable (F-VEH-042) : écrire les PV suffit. Le
// listener natif `VehicleHealthStatPoolListener` (`vehicleComponent.script:6302`) enchaîne tout
// seul le niveau de dégâts, les effets de fumée et le tableau noir. Répliquer la déformation
// aurait exigé la grille de destruction — qui n'a **aucune lecture** (F-VEH-043 : le seul getter
// du domaine est du code mort qui rend toujours `false`).
//
// ── L'entonnoir ───────────────────────────────────────────────────────────────────────────────
//
// `VehicleComponent.ReactToHPChange(destruction: Float)` (`vehicleComponent.script:4479`) : le seul
// point où les PV du véhicule ont fini de changer, **quelle qu'en soit la cause** — collision,
// balle, explosion, quête. Elle est `public function` déclarée EN PROPRE sur `VehicleComponent`,
// donc annotable (une méthode héritée donnerait `[UNRESOLVED_METHOD]`).
//
// Pourquoi pas `OnGridDestruction` : c'est un `event`, et `@wrapMethod` n'annote que `func`/`cb
// func` — l'annoter donne un `syntax error, expected "@"` trompeur. Pourquoi pas le listener de
// statpool : il n'est pas déclaré sur une classe qu'on puisse atteindre proprement, et il ne fait
// que déléguer ici.

// ─────────────────────────────────────────────────────────────────────────────────────────────
// LA FENÊTRE DE CALME — pour qu'une casse NÔTRE ne devienne jamais la casse DU JOUEUR
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// ⛔ POURQUOI. Décision de Lucas (2026-08-27) : **la casse d'un véhicule se conserve d'une session
// à l'autre, et aucun garage magique ne la répare.** Une voiture rangée à 52 de casse ressort à 52.
// C'est ce qui donnera de la valeur au métier de réparateur.
//
// Le corollaire est impitoyable : puisque la casse est DÉFINITIVE, une casse produite par **nous**
// est une avarie permanente que le joueur n'a pas causée. Et il en existe une, observée par Lucas
// le même jour : *« selon comment tu les fais spawner, au moment où un joueur rentre à l'intérieur,
// la voiture prend possession de sa physique, et des fois elles se mettent à faire des tonneaux et
// à prendre des dégâts »*.
//
// Le mécanisme est connu depuis le 2026-08-15 (F-VEH-039) : nos véhicules sont de VRAIES entités
// physiques, et notre placement autoritaire par frame se bat contre le moteur. Tant que le serveur
// tient la voiture, elle ne bouge pas ; à la seconde où le joueur monte, le contrôle bascule au
// local — et le moteur résout d'un coup l'interpénétration qu'on maintenait. La voiture part en
// tonneaux, perd des PV pour de bon, et le conducteur l'annonce au serveur.
//
// ⚠️ ON NE CORRIGE PAS LE SYMPTÔME EN LE RENDANT RÉPARABLE — ce serait exactement le garage magique
// que Lucas refuse. On l'empêche d'ÊTRE ENREGISTRÉ : pendant les trois secondes qui suivent une
// prise de volant, le conducteur **n'annonce rien**.
//
// ⚠️ Ce que ça coûte, et c'est assumé : un joueur qui emboutit quelque chose dans les trois
// premières secondes ne verra pas cette casse-là persistée. Le compromis penche du bon côté — une
// casse manquée s'ajoute au prochain choc, une casse fantôme reste pour toujours.
//
// ⚠️ Ce que ça ne fait PAS : arrêter les tonneaux. C'est un pansement sur la CONSÉQUENCE. La cause
// est le placement, et elle reste ouverte (voir F-VEH-039 et le chantier véhicules).
@addField(PlayerPuppet)
public let m_tesseraCasseSilence: Int32;

@addField(PlayerPuppet)
public let m_tesseraEtaitConducteur: Bool;

@wrapMethod(VehicleComponent)
public func ReactToHPChange(destruction: Float) -> Void {
  wrappedMethod(destruction);

  let vehicule = this.GetVehicle();
  if !IsDefined(vehicule) {
    return;
  }
  let gi = vehicule.GetGame();

  // ── SEUL LE CONDUCTEUR PARLE, ET C'EST UNE GARANTIE D'INTÉGRITÉ, PAS UNE OPTIMISATION ──────
  //
  // Décision de Lucas (2026-08-15) : « c'est le conducteur qui a l'autorité pour tous les autres
  // membres ». Sans elle, n'importe quel client enverrait `degats=100` sur toute voiture croisée
  // — et comme la casse est MONOTONE côté serveur, le grief serait IRRÉVERSIBLE sans réparation.
  // Le serveur revérifie de toute façon (`server_loop.rs`, kind 12 : refusé si le demandeur n'est
  // pas au siège 0) : ce test-ci évite le trafic, il ne fait pas respecter la règle.
  let joueur = GameInstance.GetPlayerSystem(gi).GetLocalPlayerControlledGameObject();
  if !IsDefined(joueur) || !VehicleComponent.IsDriver(gi, joueur) {
    return;
  }
  let conduit: wref<VehicleObject>;
  if !VehicleComponent.GetVehicle(gi, joueur, conduit) || !IsDefined(conduit) {
    return;
  }
  if conduit.GetEntityID() != vehicule.GetEntityID() {
    // Le joueur conduit une AUTRE voiture : celle-ci a changé de PV sans lui.
    return;
  }

  // La fenêtre de calme (voir le bloc en tête de fichier). On le DIT, sinon « la casse ne remonte
  // pas » aurait deux causes indiscernables : le fil est cassé, ou on vient d'entrer.
  let pion = joueur as PlayerPuppet;
  if IsDefined(pion) && pion.m_tesseraCasseSilence > 0 {
    FTLog(s"[Tessera/Casse] fenetre de calme (\(pion.m_tesseraCasseSilence) battement(s)) — casse NON annoncee");
    return;
  }

  // ── L'ÉCHELLE, LUE CHEZ LE CONSOMMATEUR (ADR 0034) ────────────────────────────────────────
  //
  // Cible        : `EntityInteraction.param` pour kind=12, côté serveur
  // Lecteurs     : un — `server_loop.rs`, qui appelle `VehicleRecord::rapporter_degats`
  // Alimente     : `vehicules.degats` en base, et le champ `degats` de `VehicleState` sur le fil
  // Domaine      : 0..100, où 0 = intact et 100 = épave. MONOTONE : ne décroît que par réparation
  // Hors domaine : écrêté à 100 côté serveur (`mesure.min(100)`), jamais rejeté
  //
  // `destruction` est le NOUVEAU niveau de PV, en pourcentage : `EvaluateDamageLevel`
  // (`vehicleComponent.script:4787`) le compare à 25 et 50, et au `RangeEnd()` d'un statpool.
  // La casse est donc le complément — 100 PV = 0 de casse.
  //
  // ⚠️ On envoie une valeur PLUS FINE que ce que le jeu sait afficher : `DamageState` n'a que
  // quatre crans (0/1/2/3). C'est délibéré — un scalaire à cent crans est ce qui permettra une
  // facture de réparation proportionnelle, et le jeu n'a pas besoin de le savoir pour l'afficher.
  // ⚠️ Deux conversions explicites, et aucune n'est du zèle : redscript n'a pas de littéral
  // `Uint8` (`12u` est un `Uint32`), et il ne convertit pas un `Float` en `Uint32` directement.
  // Les deux fautes donnaient le même message — « arguments do not match any of the overloads » —
  // qui ne dit pas LEQUEL est en cause.
  let casse: Int32 = RoundF(100.0 - ClampF(destruction, 0.0, 100.0));
  let reseau = GameInstance.GetNetworkGameSystem();
  if !IsDefined(reseau) {
    return;
  }

  // ⚠️ CE QUI N'EST PAS FAIT ICI, ET QUI MORDRA LE JOUR OÙ LES TÉMOINS APPLIQUERONT.
  //
  // Quand un témoin écrira les PV que le serveur lui donne, `ReactToHPChange` se redéclenchera
  // chez lui — et s'il se trouve être conducteur, il RENVERRA la casse qu'il vient de recevoir.
  // L'écho serait invisible (la monotonie le rend idempotent) mais il doublerait le trafic pour
  // rien. Le garde-fou est un drapeau posé pendant l'application. Il n'est pas écrit tant que
  // l'application n'existe pas : un garde-fou pour un chemin qui n'existe pas est du code qu'on
  // ne peut pas tester.
  // ⚠️ LE GARDE-FOU CONTRE L'ÉCHO — et il remplace le drapeau que la version précédente de ce
  // commentaire annonçait. Un drapeau « je suis en train d'appliquer » ne marcherait pas :
  // l'écriture des PV passe par `RequestSettingStatPoolValue`, une DEMANDE dont le rappel arrive
  // plus tard, après que le drapeau soit retombé. Comparer à ce que le serveur sait DÉJÀ est la
  // seule garde qui ne dépende pas du moment.
  //
  // Sans elle : le témoin applique, `ReactToHPChange` se redéclenche, il renvoie la même casse —
  // et comme elle est MONOTONE côté serveur, chaque aller-retour ne peut que la faire MONTER. Des
  // voitures qui se dégradent toutes seules, sans que personne ne les touche.
  if casse <= reseau.Tessera_DegatsConnus(vehicule.GetEntityID()) {
    return;
  }
  reseau.Tessera_VehiculeVerbe(vehicule.GetEntityID(), Cast<Uint8>(12), Cast<Uint32>(casse));
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// LE CÔTÉ TÉMOIN : écrire les PV, et laisser le moteur faire le reste
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// C'est ici que l'approche se paie (F-VEH-042) : on n'écrit qu'un nombre. Le listener natif
// `VehicleHealthStatPoolListener` (`vehicleComponent.script:6302`) enchaîne ensuite tout seul le
// niveau de dégâts, les effets de fumée et le tableau noir — donc l'aiguille, la carrosserie
// noircie et les FX arrivent gratuitement.
//
// ⚠️ Appelée depuis la veille de `VehiculeCoffre.reds` plutôt que par une boucle à elle : une
// seconde boucle de rappel coûterait à chaque frame de chaque joueur pour un événement rare.

public static func TesseraCasseDrainer(game: GameInstance) -> Void {
  let reseau: ref<NetworkGameSystem> = GameInstance.GetNetworkGameSystem();
  if !IsDefined(reseau) {
    return;
  }
  let systeme: ref<StatPoolsSystem> = GameInstance.GetStatPoolsSystem(game);
  if !IsDefined(systeme) {
    return;
  }

  // ── LA FENÊTRE DE CALME SE COMPTE ICI, sur un battement qui existe déjà ──────────────────
  //
  // ⚠️ En BATTEMENTS et non en secondes : le dépôt n'a pas d'horloge monotone en redscript
  // (`GetGameTime` rend l'heure du MONDE, qui saute quand le serveur la recale — voir
  // `ApplyServerConfig`). Un compteur sur une veille à 200 ms est exact, gratuit, et ne peut pas
  // reculer.
  //
  // ⚠️ On détecte la TRANSITION, pas l'état : « il est conducteur » est vrai pendant tout le
  // trajet, et armer la fenêtre à chaque battement la rendrait permanente — le conducteur
  // n'annoncerait plus jamais rien.
  let joueurLocal = GameInstance.GetPlayerSystem(game).GetLocalPlayerControlledGameObject() as PlayerPuppet;
  if IsDefined(joueurLocal) {
    let conducteur: Bool = VehicleComponent.IsDriver(game, joueurLocal);
    if conducteur && !joueurLocal.m_tesseraEtaitConducteur {
      // 15 battements a 200 ms = 3 s, le temps que le moteur resolve la reprise en main.
      joueurLocal.m_tesseraCasseSilence = 15;
      FTLog("[Tessera/Casse] prise de volant — fenetre de calme de 3 s (aucune casse annoncee)");
    }
    joueurLocal.m_tesseraEtaitConducteur = conducteur;
    if joueurLocal.m_tesseraCasseSilence > 0 {
      joueurLocal.m_tesseraCasseSilence -= 1;
    }
  }
  // ⚠️ TRACE D'ÉTAPE. Une exception en redscript ne dit ni où ni pourquoi : elle interrompt, point.
  // La seule façon de la localiser est de laisser une empreinte avant chaque geste risqué, et de
  // regarder laquelle est la dernière écrite. Bornée à un tick sur 50 pour ne pas noyer le journal.
  let enAttente: Int32 = reseau.Tessera_DegatsEnAttente();
  if enAttente > 0 {
    FTLog(s"[Tessera/Casse] file : \(enAttente) en attente");
  }
  // Borne dure : une file qui explose ne doit pas geler la frame. Ce qui reste sera drainé au
  // battement suivant — la file est ordonnée, rien ne se perd.
  let restant: Int32 = 32;
  while reseau.Tessera_DegatsEnAttente() > 0 && restant > 0 {
    restant -= 1;
    let cible: EntityID = reseau.Tessera_DegatsVehicule();
    let casse: Int32 = reseau.Tessera_DegatsValeur();
    // ⚠️ DÉFILER D'ABORD, TOUJOURS. Un véhicule introuvable (pas encore né, déjà despawné) ne
    // doit pas boucher la file : elle se bloquerait sur une entrée impossible et plus aucune
    // casse n'arriverait — une panne qui ressemble à « le serveur n'envoie plus rien ».
    reseau.Tessera_DegatsDefiler();
    // ⚠️ Condition INVERSÉE plutôt qu'un `continue` : redscript n'a pas de `continue`, et l'erreur
    // qu'il rend (« unresolved reference 'continue' ») ressemble à un identifiant manquant.
    FTLog(s"[Tessera/Casse] defile : cible definie=\(EntityID.IsDefined(cible)) casse=\(casse)");
    if EntityID.IsDefined(cible) && !TesseraCasseEstMaVoiture(game, cible) {
      FTLog("[Tessera/Casse] etape : ce n'est pas ma voiture, j'applique");
      // Les PV sont le complément de la casse, sur la même échelle de pourcentage que celle que
      // `EvaluateDamageLevel` compare à 25 et 50 (`vehicleComponent.script:4787`).
      let pv: Float = 100.0 - ClampF(Cast<Float>(casse), 0.0, 100.0);
      // ⚠️ Le transtypage EST exigé : sans lui, `NO_MATCHING_OVERLOAD` à la compilation. Le code
      // décompilé de CDPR passe une `EntityID` nue parce que sa signature déclare un
      // `StatsObjectID` et que le compilateur du jeu convertit — redscript, lui, ne le fait pas.
      // Ce n'était donc PAS la cause de l'exception ; elle est ailleurs, et c'est la trace
      // ci-dessous qui la nommera.
      systeme.RequestSettingStatPoolValue(Cast<StatsObjectID>(cible), gamedataStatPoolType.Health, pv, null);
      TesseraCasseDeformer(game, cible, casse);
    }
  }
}


/// Le véhicule que le joueur LOCAL conduit ne doit jamais recevoir l'état venu du serveur.
///
/// ⚠️ Sans ce test, le conducteur s'écrirait à lui-même les PV qu'il vient d'annoncer — au mieux
/// un no-op, au pire une écriture qui se bat contre le moteur en plein choc, pendant que celui-ci
/// calcule sa propre casse. Le serveur ne lui apprend rien sur sa propre voiture : c'est LUI la
/// source.
public static func TesseraCasseEstMaVoiture(game: GameInstance, cible: EntityID) -> Bool {
  let joueur = GameInstance.GetPlayerSystem(game).GetLocalPlayerControlledGameObject();
  if !IsDefined(joueur) {
    return false;
  }
  let conduit: wref<VehicleObject>;
  if !VehicleComponent.GetVehicle(game, joueur, conduit) || !IsDefined(conduit) {
    return false;
  }
  return conduit.GetEntityID() == cible;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// LA TÔLE : la rendre VISIBLE chez le témoin, sans rien ajouter sur le fil
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// ── Le problème que ça règle ─────────────────────────────────────────────────────────────────
//
// Écrire les PV fait dérouler au moteur du témoin le niveau de dégâts, la fumée et le tableau noir
// (F-VEH-042, mesuré : le témoin passe au cran 1 vers 50 de casse, au cran 2 vers 77). Ce que ça
// ne fait PAS, c'est **froisser la carrosserie** : la déformation vit dans une grille que rien ne
// pilote depuis les PV. Lucas, qui regardait la tôle, a donc conclu « les dégâts ne sont pas
// synchronisés » alors que le nombre l'était — il regardait la seule couche qui ne suivait pas.
//
// ── Pourquoi RIEN sur le fil ─────────────────────────────────────────────────────────────────
//
// La grille se pilote par `VehicleQuestVisualDestructionEvent` : **neuf flottants plats**, dont le
// gestionnaire construit lui-même le `[15]Float` que le Lua ne savait pas fabriquer (F-VEH-043).
// On pourrait donc envoyer neuf nombres de plus par véhicule. On ne le fait pas : les neuf se
// **dérivent** de la casse, qui traverse déjà le fil. Même entrée chez tout le monde, donc même
// tôle — ce qui est exactement le but.
//
// ⚠️ ponytail: dérivation DÉTERMINISTE au lieu des vrais points d'impact. Le plafond est réel et
// il faut le nommer : une voiture heurtée uniquement à l'avant sera cabossée **partout**, parce
// que la casse est un scalaire qui a perdu la direction. Ce qu'on gagne, c'est que tous les
// témoins voient LA MÊME tôle sans un octet de plus, et sans dépendre d'une détection d'impact
// qui n'existe pas encore. La montée en gamme est connue : ajouter la direction du choc au verbe
// 12 (deux octets suffiraient) et s'en servir ici comme pondération.
//
// ⚠️ `accumulate = false` : la valeur est une FONCTION de la casse, pas un incrément. Avec le
// cumul, deux applications de la même casse donneraient deux tôles différentes — et l'état cesserait
// d'être reconstructible à partir du fil, ce qui est toute la raison d'être du modèle.
public static func TesseraCasseDeformer(game: GameInstance, cible: EntityID, casse: Int32) -> Void {
  let vehicule = GameInstance.FindEntityByID(game, cible) as VehicleObject;
  if !IsDefined(vehicule) {
    return;
  }
  // La grille attend 0..1. En dessous d'un quart de casse, on ne déforme pas : le moteur lui-même
  // n'affiche son premier cran de dégâts que bien plus tard, et froisser une voiture à peine
  // éraflée donnerait un rendu plus sévère chez le témoin que chez le conducteur.
  let t: Float = ClampF((Cast<Float>(casse) - 25.0) / 75.0, 0.0, 1.0);
  if t <= 0.0 {
    return;
  }
  let evt: ref<VehicleQuestVisualDestructionEvent> = new VehicleQuestVisualDestructionEvent();
  evt.accumulate = false;
  // Les faces avant prennent davantage : la très grande majorité des chocs de conduite sont
  // frontaux, donc c'est le profil qui ressemblera le plus souvent à la réalité — un compromis
  // assumé, pas une mesure.
  evt.front = t;
  evt.frontLeft = t * 0.85;
  evt.frontRight = t * 0.85;
  evt.left = t * 0.55;
  evt.right = t * 0.55;
  evt.backLeft = t * 0.35;
  evt.backRight = t * 0.35;
  evt.back = t * 0.45;
  evt.roof = t * 0.20;
  vehicule.QueueEvent(evt);
  // ⚠️ On journalise le TIR, jamais l'effet. Le domaine de la grille n'a AUCUNE lecture — son seul
  // getter est du code mort qui rend toujours faux (F-VEH-043). Cette ligne distingue donc « la
  // deformation n'a pas ete demandee » de « elle a ete demandee et ne se voit pas », ce qui est la
  // seule chose qu'un agent sans yeux puisse trancher.
  FTLog(s"[Tessera/Casse] deformation demandee : casse=\(casse) t=\(t)");
}

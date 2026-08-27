module Cyberverse.Network.Managers

// LE COFFRE DE VÉHICULE, SOUS AUTORITÉ SERVEUR.
//
// ⚠️⚠️ UN PIÈGE PAYÉ LE 2026-08-25, ET IL EST TOTALEMENT SILENCIEUX.
//
// `NetworkGameSystem` est un `IGameSystem` (C++ du fork), PAS un `ScriptableSystem`. L'obtenir par
// `GameInstance.GetScriptableSystemsContainer(g).Get(n"NetworkGameSystem") as NetworkGameSystem`
// **compile parfaitement** — le nom existe, le transtypage est légal — et rend `null` à CHAQUE
// appel. Tout le code en aval se contente alors de sortir sans rien faire, sans une ligne de
// journal, sans une erreur.
//
// Symptôme observé : l'événement de coffre partait, le crochet se déclenchait, et le serveur ne
// recevait jamais rien. On soupçonne le fil, le kind, la traduction d'identifiant — tout sauf
// l'accesseur, qui a l'air juste.
//
// **Le seul accesseur correct est `GameInstance.GetNetworkGameSystem()`.**
//
// ── Ce qu'on ne construit PAS, et c'est l'essentiel ───────────────────────────────────────────
//
// Le coffre est NATIF. `VehicleComponent.OnVehiclePlayerTrunk` (`vehicleComponent.script:4088`)
// ouvre l'écran de rangement en écrivant `StorageUserData{storageObject = GetVehicle()}` dans
// `StorageBlackboard.StorageData` — et **le stockage EST le véhicule lui-même**, un `GameObject`
// ordinaire dont le contenu passe par `TransactionSystem`, exactement comme le sac du joueur.
//
// On n'écrit donc ni inventaire, ni interface, ni logique de transfert. On ajoute au coffre natif
// les deux seules choses qui lui manquent pour le jeu de rôle : une **mémoire** qui survit à la
// session, et un **arbitre** qui n'est pas le client.
//
// ── Les trois interventions, et pourquoi chacune ──────────────────────────────────────────────
//
// 1. `GetTrunkActions` — en vanilla, l'action de rangement n'est offerte QUE si
//    `GetIsPlayerVehicle()` (`vehicleComponentPS.script:757`), c'est-à-dire sur la voiture de la
//    garde-robe du joueur solo. Aucun véhicule Tessera n'en est une : sans ce crochet, **aucun
//    coffre ne s'ouvrirait jamais**. On l'offre sur tout véhicule que le SERVEUR connaît.
//
// 2. `OnVehiclePlayerTrunk` — on ne laisse pas le natif ouvrir tout de suite : il montrerait le
//    contenu LOCAL, c'est-à-dire n'importe quoi. On demande d'abord au serveur.
//
// 3. `OnUninitialize` de l'écran — à la fermeture, on annonce l'état complet du coffre.
//
// ⚠️ **Le verrou du véhicule ne garde PAS le coffre en vanilla** (mesuré 2026-08-25) :
// `GetTrunkActions` ne teste jamais l'état `Locked` des portes. La règle « coffre fermé si la
// voiture est verrouillée » est donc un comportement NEUF, et elle vit côté serveur — ici on ne
// fait que ne rien afficher quand le serveur ne répond pas.

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 1 · OFFRIR L'ACTION
// ─────────────────────────────────────────────────────────────────────────────────────────────

// ⚠️ ON ENVELOPPE LE GETTER, ON N'ÉCRIT PAS LE DRAPEAU — et la fiche du consommateur (ADR 0034)
// justifie exactement ce choix :
//
//   Cible        : `VehicleComponentPS.m_playerVehicle` (script, PERSISTANT)
//   Lecteurs     : UN SEUL — `GetTrunkActions` (`vehicleComponentPS.script:757`)
//   Alimente     : l'offre de l'action « rangement » sur le coffre, rien d'autre
//   Domaine      : booléen
//   Hors domaine : sans objet
//
// Un seul lecteur, et c'est précisément celui qu'on vise : le champ ne fait rien d'autre. Mais il
// est **persistant** — l'écrire poserait un état dans le système de sauvegarde qu'il faudrait
// ensuite penser à retirer. Répondre à une question coûte moins cher que modifier un état.
//
// ⚠️⚠️ ET LE PIÈGE DE NOM, QUI A FAILLI ME COÛTER UNE FAUSSE MANŒUVRE. Il existe DEUX choses
// appelées presque pareil :
//   · `VehicleComponentPS.GetIsPlayerVehicle()` — drapeau SCRIPT, **un** lecteur (celui-ci) ;
//   · `VehicleObject.IsPlayerVehicle()` — NATIF (`vehicles.script:75`), lu par le pilotage
//     automatique, le système de prévention (police) et la détection de vol.
// Toucher le premier n'a aucun effet sur le second. Confondre les deux ferait croire qu'on vient
// de déclarer toutes les voitures du serveur « propriété du joueur » aux yeux de la police.

@wrapMethod(VehicleComponentPS)
public func GetIsPlayerVehicle() -> Bool {
  if wrappedMethod() {
    return true;
  }
  let proprietaire = this.GetOwnerEntity();
  if !IsDefined(proprietaire) {
    return false;
  }
  let reseau = GameInstance.GetNetworkGameSystem();
  if !IsDefined(reseau) {
    return false;
  }
  // Tout véhicule que le SERVEUR connaît offre son coffre. Le droit d'y accéder, lui, est arbitré
  // par le serveur à l'ouverture — ici on ne décide que de l'AFFICHAGE de l'invite.
  let reseauConnu: Bool = reseau.Tessera_EstVehiculeReseau(proprietaire.GetEntityID());
  return reseauConnu;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 2 · L'OUVERTURE : on demande au serveur AVANT de montrer quoi que ce soit
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// ⚠️ `OnVehiclePlayerTrunk` est un `event` dans le script décompilé : `@wrapMethod` n'annote que
// `func` / `cb func`, et l'annoter tel quel donne un `syntax error, expected "@"` trompeur — le
// parseur a consommé l'annotation et n'a trouvé aucune cible annotable.

@wrapMethod(VehicleComponent)
protected cb func OnVehiclePlayerTrunk(evt: ref<VehiclePlayerTrunk>) -> Bool {
  TesseraCoffreLog("crochet OnVehiclePlayerTrunk atteint");
  let vehicule = this.GetVehicle();
  if !IsDefined(vehicule) {
    return wrappedMethod(evt);
  }
  let reseau = GameInstance.GetNetworkGameSystem();
  if !IsDefined(reseau) {
    // Pas de netcode : partie solo, le coffre natif reprend ses droits. Un mod qui se bloque
    // quand le serveur est absent est un mod qu'on ne peut plus déboguer hors ligne.
    return wrappedMethod(evt);
  }

  // ⚠️ ON N'APPELLE PAS `wrappedMethod` ICI, et c'est tout l'intérêt. Le natif ouvrirait
  // immédiatement l'écran sur le contenu LOCAL du véhicule — qui, pour une voiture née d'un
  // snapshot serveur, est vide ou hérité d'une autre session. Le joueur verrait un coffre vide,
  // puis le verrait se remplir sous ses yeux, et pourrait agir entre les deux.
  //
  // La demande part ; c'est la veille (plus bas) qui ouvrira quand le contenu sera arrivé.
  // Si le serveur refuse, rien ne s'ouvre — et c'est le comportement voulu : un refus explicite
  // apprendrait à un client modifié quels véhicules sont verrouillés, en les sondant un par un.
  let parti: Bool = reseau.Tessera_VehiculeVerbe(vehicule.GetEntityID(), Cast<Uint8>(14), Cast<Uint32>(0));
  // ⚠️ On journalise le DÉPART, pas l'acceptation. `false` ici veut dire « le message n'est même
  // pas parti » — véhicule inconnu du serveur, ou pas de connexion. C'est la seule chose que le
  // client puisse savoir, et la distinguer d'un refus serveur vaut une session de diagnostic.
  TesseraCoffreLog(s"ouverture demandee : parti=\(parti)");
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 3 · LA FERMETURE : on annonce l'ÉTAT COMPLET, jamais un delta
// ─────────────────────────────────────────────────────────────────────────────────────────────

@wrapMethod(FullscreenVendorGameController)
protected cb func OnUninitialize() -> Bool {
  let resultat = wrappedMethod();
  TesseraCoffreRapporter();
  return resultat;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// LA VEILLE : elle ouvre quand le contenu arrive, et n'ouvre rien sinon
// ─────────────────────────────────────────────────────────────────────────────────────────────

public class TesseraCoffreTick extends DelayCallback {
  public let m_veille: wref<TesseraCoffreVeille>;
  public func Call() -> Void {
    if IsDefined(this.m_veille) {
      this.m_veille.OnTick();
    }
  }
}

public class TesseraCoffreVeille extends IScriptable {
  private let m_game: GameInstance;
  /// La dernière séquence traitée. ⚠️ Un COMPTEUR, pas un booléen « reçu » : un coffre s'ouvre
  /// plusieurs fois, et deux ouvertures successives du même coffre avec le même contenu seraient
  /// indiscernables par un drapeau — la seconde ne s'ouvrirait jamais.
  private let m_derniereSeq: Int32;
  private let m_ticks: Int32;
  /// L'entité qui PORTE réellement les objets du coffre. Voir le bloc « LE PORTEUR » plus bas.
  private let m_porteur: EntityID;
  /// Un écran de coffre est-il ouvert EN CE MOMENT ? Posé quand on ouvre, retiré quand on
  /// rapporte.
  ///
  /// ⚠️ POURQUOI NOTRE PROPRE DRAPEAU PLUTÔT QUE CELUI DU NETCODE. `Tessera_CoffreVehicule()`
  /// reste défini après un rapport tant que le natif ne l'efface pas — et le natif vit dans une
  /// DLL qui se déploie séparément du script. Faire dépendre une décision DESTRUCTRICE (annoncer
  /// un contenu complet, donc autoriser un effacement) d'un état qu'on ne possède pas, c'est
  /// accepter qu'une paire DLL/script désaccordée efface le coffre d'un joueur.
  ///
  /// Ici les deux gardes se cumulent et aucune ne suffit seule : le netcode ferme sa session, et
  /// nous fermons la nôtre. C'est la même discipline que l'anti-écho de la casse.
  private let m_sessionOuverte: Bool;
  /// Combien de ticks on a déjà attendu que le porteur naisse, pour cette ouverture-ci.
  /// ⚠️ Borné : une attente non bornée transformerait un porteur qui ne naît jamais en coffre qui
  /// ne s'ouvre jamais, sans un mot — la panne la plus chère à diagnostiquer.
  private let m_attentePorteur: Int32;

  public func Init(game: GameInstance) -> Void {
    this.m_game = game;
    this.m_derniereSeq = 0;
    TesseraCoffreLog("veille demarree (200 ms)");
    this.Planifier();
  }

  private func Planifier() -> Void {
    let tick: ref<TesseraCoffreTick> = new TesseraCoffreTick();
    tick.m_veille = this;
    // 200 ms : un coffre s'ouvre à la demande d'un humain, pas à la fréquence du réseau. Battre
    // plus vite ne gagnerait rien de perceptible et coûterait à chaque frame de chaque joueur.
    GameInstance.GetDelaySystem(this.m_game).DelayCallback(tick, 0.2, false);
  }

  public func OnTick() -> Void {
    // ⚠️⚠️ ON SE REPLANIFIE EN PREMIER, PAS EN DERNIER — et ça a coûté une session.
    //
    // La version d'avant appelait `Planifier()` à la FIN. Une exception n'importe où dans le corps
    // tuait donc la veille DÉFINITIVEMENT, et sans un mot : le journal montrait « tick 1 » puis
    // plus rien, et l'absence de battement se lit comme « le serveur n'envoie rien ». Mesuré le
    // 2026-08-26 : l'ajout du drainage de casse a levé, et toute la boucle est morte avec — coffre
    // compris, alors qu'il n'avait rien à voir.
    //
    // redscript n'a pas de `try`/`catch` : replanifier d'abord est la SEULE façon de rendre la
    // boucle survivante à son propre contenu. Le coût est nul, le pire cas est un tick sauté.
    this.Planifier();
    this.m_ticks += 1;
    let reseau: ref<NetworkGameSystem> = GameInstance.GetNetworkGameSystem();
    if !IsDefined(reseau) {
      // ⚠️ On le DIT, rarement mais on le dit. Un instrument qui abandonne sans trace ne se
      // distingue pas d'un instrument mort.
      if this.m_ticks % 100 == 1 {
        TesseraCoffreLog(s"tick \(this.m_ticks) — pas de NetworkGameSystem");
      }
      return;
    }
    let seq: Int32 = reseau.Tessera_CoffreSeq();
    // ⚠️ LE BATTEMENT. Il ne sert à aucune logique : il PROUVE que la veille bat encore, et il
    // montre la séquence qu'elle LIT. Sans lui, « la veille ne réagit pas » a deux causes
    // indiscernables — la boucle ne se replanifie plus, ou elle tourne et voit toujours le même
    // numéro. C'est l'instrument que la boucle des ascenseurs a depuis toujours et que celle-ci
    // n'avait pas ; il a coûté une session à ne pas l'avoir.
    if this.m_ticks % 25 == 1 {
      TesseraCoffreLog(s"tick \(this.m_ticks) seq=\(seq) vue=\(this.m_derniereSeq)");
    }
    if seq != this.m_derniereSeq {
      // ⚠️ On ne note la séquence comme VUE que si elle a été TRAITÉE. Voir `Appliquer` : le
      // porteur naît de façon asynchrone, et consommer avant qu'il existe perdrait l'ouverture.
      if this.Appliquer(reseau) {
        this.m_derniereSeq = seq;
      }
    }
    // La casse des véhicules voisins, drainée sur le MÊME battement. Une seconde boucle de rappel
    // coûterait à chaque frame de chaque joueur pour un événement rare.
    TesseraCasseDrainer(this.m_game);
    // L'invocation, pour la même raison. Elle ne fait rien tant qu'aucun ordre n'est arrivé — et
    // quand il arrive, c'est le battement qui lui donne son rythme, sans classe ni rappel de plus.
    TesseraInvocationDrainer(this.m_game);
  }

  // ───────────────────────────────────────────────────────────────────────────────────────
  // LE PORTEUR — l'identité et le contenant sont DEUX choses
  // ───────────────────────────────────────────────────────────────────────────────────────
  //
  // ⛔ POURQUOI IL EXISTE. Un véhicule REFUSE tout dépôt d'objet : les trois routes de
  // `TransactionSystem` rendent `false`, avec témoin joueur vert dans la même passe, et une
  // voiture de police NATIVE refuse à l'identique (F-VEH-044). Toute la chaîne du coffre est
  // mesurée verte — demande, arbitrage serveur, réponse, alignement — et bute sur ce dernier
  // centimètre. Le véhicule ne peut pas être le contenant.
  //
  // ⭐ CE QUI FAIT QU'UN OBJET PEUT EN CONTENIR D'AUTRES, c'est un composant `Inventory` sur son
  // gabarit — pas sa classe, pas sa présence au monde. Un pantin en a un (relevé de composants du
  // 2026-08-25 : `gameInventory`, 1 exemplaire), et nous en faisons naître à chaque session.
  // Mieux : `ArmeAvatar.reds` DONNE déjà des objets à un pantin spawné, en production, et la
  // mesure est explicite — « GiveItem = true · AddItemToSlot = true · slot vide apres = false ».
  // L'échec d'ArmeAvatar était purement VISUEL ; l'inventaire, lui, marchait.
  //
  // Donc : **le véhicule reste l'IDENTITÉ** (quel coffre, à qui, verrouillé ou non — tout cela est
  // déjà arbitré par le serveur) et **le porteur est le CONTENANT** (où les objets vivent le temps
  // que l'écran est ouvert). Le porteur n'est pas un stockage : c'est un TAMPON D'AFFICHAGE. La
  // vérité est en base, côté serveur, et elle y reste.
  //
  // ⚠️ UN SEUL PORTEUR POUR TOUTE LA SESSION, pas un par véhicule. On le vide et on le remplit à
  // chaque ouverture : deux coffres ne sont jamais ouverts en même temps, et un porteur par
  // voiture serait N entités à faire naître, à suivre et à détruire pour un gain nul.
  //
  // ⚠️ « NON MESURÉ — HYPOTHÈSE » sur un point, et il faut le dire : que l'écran natif de stockage
  // accepte un PANTIN comme `storageObject`. Ce qui est lu dans les scripts du jeu le permet —
  // `StorageUserData.storageObject` est déclaré `weak<GameObject>` (`storageScenario.script:40`),
  // `VendorDataManager.Initialize` ne fait qu'un `FindEntityByID` suivi d'un cast en `GameObject`
  // (`vendor.script:23-26`), et le listage passe par `GetItemList` sur cet objet
  // (`vendor.script:200`), sans aucun contrôle de classe nulle part. Mais « aucun contrôle dans le
  // script » n'est pas « le natif l'accepte » : le verdict se prend en jeu, en une ouverture.
  private func Porteur() -> ref<GameObject> {
    // Déjà là ? On le réutilise. C'est le cas courant.
    if EntityID.IsDefined(this.m_porteur) {
      let existant = GameInstance.FindEntityByID(this.m_game, this.m_porteur) as GameObject;
      if IsDefined(existant) {
        return existant;
      }
      // ⚠️ Il a disparu. `alwaysSpawned = true` est censé l'en empêcher, et on ne PARIE pas
      // là-dessus : on refait naître. Un code qui dépend d'une propriété qu'on n'a pas mesurée
      // tombe le jour où elle est fausse, et il tombe en silence.
      TesseraCoffreLog("porteur disparu — on le refait naitre");
    }

    let reseau: ref<NetworkGameSystem> = GameInstance.GetNetworkGameSystem();
    if !IsDefined(reseau) {
      return null;
    }
    let joueur = GetPlayer(this.m_game);
    if !IsDefined(joueur) {
      return null;
    }
    // ⚠️ LOIN SOUS LE MONDE, et l'endroit n'a aucune importance pour la fonction : un inventaire
    // ne dépend pas d'une position. Il en a une pour ce qu'on veut ÉVITER — qu'un pantin de foule
    // apparaisse à côté du joueur, marche, parle, ou se fasse tirer dessus. À moins 400 mètres,
    // personne ne le voit et rien ne l'atteint.
    //
    // ⚠️ On le pose sous le JOUEUR plutôt qu'à une coordonnée fixe : le streaming du monde est
    // organisé autour de lui, et une entité à l'autre bout de la carte est le cas où « elle
    // disparaît » est le plus probable. `Porteur()` sait la refaire naître, mais mieux vaut ne pas
    // avoir à s'en servir à chaque ouverture.
    let ou: Vector4 = joueur.GetWorldPosition();
    ou.Z -= 400.0;
    // `Character.CitizenBikerMale` : le record de repli du netcode, donc un record dont on sait
    // qu'il existe chez tout joueur et qu'il fait naître un pantin complet.
    this.m_porteur = reseau.SpawnTransientEntity(t"Character.CitizenBikerMale", ou, new Quaternion(0.0, 0.0, 0.0, 1.0));
    TesseraCoffreLog(s"porteur demande (naissance asynchrone) sous le joueur, -400 m");
    // ⚠️ On rend `null` À DESSEIN : `CreateEntity` est ASYNCHRONE, le corps n'existe pas encore à
    // cette frame. Rendre l'entité « bientôt » serait le mensonge exact que D1 interdit. L'appelant
    // réessaie au tick suivant.
    return null;
  }

  /// La session de coffre était-elle ouverte ? **Consomme la réponse** : un second appel rend
  /// `false`.
  ///
  /// ⚠️ Consommer plutôt que lire est délibéré. `OnUninitialize` peut se déclencher plusieurs fois
  /// pour un même écran, et chaque déclenchement annoncerait l'état complet du coffre. Le second
  /// le trouverait VIDE — le porteur ayant été vidé par le premier — et le serveur effacerait.
  public func ConsommerSession() -> Bool {
    let ouverte: Bool = this.m_sessionOuverte;
    this.m_sessionOuverte = false;
    return ouverte;
  }

  /// Le porteur, **sans jamais le faire naître**. C'est ce que lit la fermeture.
  ///
  /// ⚠️ La distinction avec `Porteur()` n'est pas cosmétique. Faire naître un porteur à la
  /// FERMETURE produirait un contenant vide, donc un rapport « le coffre est vide », donc un
  /// effacement en base. Une fonction qui crée ce qu'elle cherche est dangereuse partout où
  /// « absent » est une réponse qui compte.
  public func PorteurExistant() -> ref<GameObject> {
    if !EntityID.IsDefined(this.m_porteur) {
      return null;
    }
    return GameInstance.FindEntityByID(this.m_game, this.m_porteur) as GameObject;
  }

  /// Vide le porteur, puis le remplit de ce que le SERVEUR énonce.
  ///
  /// ⚠️ ON VIDE D'ABORD, ET COMPLÈTEMENT. Un pantin naît avec sa dotation — vêtements, arme de
  /// foule, pièces de corps. Sans `RemoveAllItems`, le joueur verrait la garde-robe d'un motard
  /// dans son coffre, et — bien pire — la FERMETURE l'annoncerait au serveur comme étant le
  /// contenu du coffre, qui le persisterait. Un tampon d'affichage doit partir vide.
  ///
  /// C'est aussi ce qui rend inutile ici la liste de préservation du sac joueur (`F-MND-047`,
  /// `Tessera_PreserverId`) : on ne préserve rien sur un porteur, puisqu'il n'est le corps de
  /// personne.
  private func Garnir(porteur: ref<GameObject>, reseau: ref<NetworkGameSystem>) -> Bool {
    let ts: ref<TransactionSystem> = GameInstance.GetTransactionSystem(this.m_game);
    // ⚠️ DÉSÉQUIPER AVANT DE VIDER. Mesuré le 2026-08-26, à l'écran : un pistolet de la dotation
    // du pantin apparaissait dans le coffre malgré `RemoveAllItems`. Le jeu protège ce qui est
    // ÉQUIPÉ — retirer un item d'un slot est une opération distincte de le retirer de
    // l'inventaire, et `RemoveAllItems` ne fait que la seconde.
    ts.ClearAllSlots(porteur);
    ts.RemoveAllItems(porteur);

    // ── UN PORTEUR QUI NE SE VIDE PAS FAIT REFUSER L'OUVERTURE ───────────────────────────
    //
    // ⚠️ C'EST UNE QUESTION D'ÉCONOMIE, PAS DE PROPRETÉ. Ce qui traîne dans le porteur à
    // l'ouverture sera, à la fermeture, annoncé au serveur comme le contenu du coffre — donc
    // persisté, donc offert. Un objet gratuit à chaque ouverture, pour chaque joueur.
    //
    // ⚠️ ON REFUSE PLUTÔT QU'ON NE COMPENSE, et c'est un choix qu'on a d'abord fait à l'envers. La
    // première version RETENAIT ces lignes pour les soustraire du rapport de fermeture. Ça marche
    // — tant que la soustraction a raison. Quand elle se trompe, elle retire du rapport des objets
    // que le joueur avait bel et bien déposés : une PERTE, silencieuse, et dans le sens le plus
    // difficile à croire pour lui. Une compensation muette échange un problème visible contre un
    // problème invisible.
    //
    // Refuser ne perd rien, ne donne rien, et se voit tout de suite. Mesuré le 2026-08-26 : le
    // porteur sort VIDE du nettoyage — ce refus ne devrait jamais se déclencher, et le jour où il
    // se déclenche il faudra corriger le NETTOYAGE, pas le rapport.
    let restant: array<wref<gameItemData>>;
    ts.GetItemList(porteur, restant);
    if ArraySize(restant) > 0 {
      TesseraCoffreLog(s"porteur PAS VIDE apres ClearAllSlots + RemoveAllItems : \(ArraySize(restant)) ligne(s) — OUVERTURE REFUSEE (elles seraient offertes au joueur a la fermeture)");
      return false;
    }

    let n: Int32 = reseau.Tessera_CoffreTaille();
    let i: Int32 = 0;
    while i < n {
      let tdb: TweakDBID = TDBID.Create(reseau.Tessera_CoffreItemId(i));
      ts.GiveItemByTDBID(porteur, tdb, reseau.Tessera_CoffreItemQuantite(i));
      i += 1;
    }
    return true;
  }

  /// Aligne le contenant sur ce que le serveur vient d'énoncer, puis ouvre l'écran.
  ///
  /// ⚠️ Même doctrine que le sac du joueur (ADR 0026) : le serveur énonce un ÉTAT, le client s'y
  /// aligne. Ici l'alignement est même plus simple qu'ailleurs — on vide et on remplit — parce que
  /// le contenant est un tampon d'affichage et non un corps qu'il faudrait ménager.
  ///
  /// Rend `true` quand la séquence est TRAITÉE (ouverte, ou définitivement abandonnée) et `false`
  /// tant qu'il faut réessayer. L'appelant ne consomme le numéro de séquence que sur `true`.
  ///
  /// ⚠️ CETTE DISTINCTION EST CE QUI ÉVITE DEUX PANNES OPPOSÉES. Consommer trop tôt perd
  /// l'ouverture pour toujours — le joueur a appuyé, rien ne s'ouvre, plus jamais. Ne jamais
  /// consommer bloque la veille sur une entrée intraitable, et le coffre suivant n'arrive plus
  /// (c'est exactement F-VEH-048, payé sur la file de casse). D'où : on réessaie SEULEMENT
  /// l'attente du porteur, et on la BORNE.
  private func Appliquer(reseau: ref<NetworkGameSystem>) -> Bool {
    // ── L'IDENTITÉ : de quel coffre parle-t-on ? ──────────────────────────────────────────
    //
    // Le véhicule ne portera pas les objets (voir « LE PORTEUR »), mais il reste ce que le joueur
    // a désigné. S'il n'est plus là, il n'y a plus de coffre à montrer — et c'est DÉFINITIF, pas
    // une attente : on consomme.
    let idVehicule: EntityID = reseau.Tessera_CoffreVehicule();
    if !EntityID.IsDefined(idVehicule) {
      TesseraCoffreLog("contenu recu mais le vehicule n'est pas (ou plus) dans le monde");
      return true;
    }

    // ── LE CONTENANT ──────────────────────────────────────────────────────────────────────
    let porteur: ref<GameObject> = this.Porteur();
    if !IsDefined(porteur) {
      this.m_attentePorteur += 1;
      // 25 ticks à 200 ms = 5 s. Au-delà, le porteur ne naîtra pas : on le DIT et on passe, plutôt
      // que de battre indéfiniment sur une ouverture qui n'aboutira jamais.
      if this.m_attentePorteur > 25 {
        TesseraCoffreLog("porteur JAMAIS ne apres 5 s — ouverture abandonnee (le coffre suivant repartira)");
        this.m_attentePorteur = 0;
        return true;
      }
      return false;
    }
    this.m_attentePorteur = 0;

    let ts: ref<TransactionSystem> = GameInstance.GetTransactionSystem(this.m_game);
    if !IsDefined(ts) {
      return true;
    }

    if !this.Garnir(porteur, reseau) {
      // Refus DÉFINITIF pour cette ouverture : on consomme la séquence, sinon la veille
      // réessaierait indéfiniment sur un porteur qui ne se videra pas davantage au tick suivant.
      return true;
    }

    // ⚠️ ON RELIT CE QU'ON VIENT D'ÉCRIRE. `GiveItemByTDBID` rend un booléen, et un booléen à
    // `true` a déjà menti dans ce dépôt (F-VEH-014, « accepté ≠ exécuté »). Compter les lignes
    // réellement présentes distingue « le porteur a pris les objets » de « il a dit oui » — et
    // c'est la seule ligne de journal qui vaudra quelque chose au premier essai en jeu.
    let presents: array<wref<gameItemData>>;
    ts.GetItemList(porteur, presents);
    let annonce: Int32 = reseau.Tessera_CoffreTaille();
    TesseraCoffreLog(s"coffre garni : serveur annonce \(annonce) ligne(s), porteur en contient \(ArraySize(presents)), capacite \(reseau.Tessera_CoffreCapacite())");

    // ── ET SEULEMENT MAINTENANT, OUVRIR L'ÉCRAN NATIF ─────────────────────────────────────
    //
    // Exactement ce que fait `OnVehiclePlayerTrunk` en vanilla (`vehicleComponent.script:4088`), à
    // deux différences près : le contenu a été aligné AVANT, et `storageObject` désigne le porteur
    // au lieu du véhicule. `ProcessStashRetroFixes` est délibérément OMIS — ce sont des migrations
    // de sauvegardes 1.5→2.1, et l'état de Tessera est 100 % serveur : aucune save héritée.
    let donneesStockage: ref<StorageUserData> = new StorageUserData();
    donneesStockage.storageObject = porteur;
    let tableau: ref<IBlackboard> = GameInstance.GetBlackboardSystem(this.m_game).Get(GetAllBlackboardDefs().StorageBlackboard);
    if !IsDefined(tableau) {
      TesseraCoffreLog("StorageBlackboard injoignable — l'ecran ne peut pas s'ouvrir");
      return true;
    }
    tableau.SetVariant(GetAllBlackboardDefs().StorageBlackboard.StorageData, ToVariant(donneesStockage), true);
    this.m_sessionOuverte = true;
    return true;
  }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// DÉMARRAGE DE LA VEILLE — à l'attache du joueur, comme toutes les boucles du dépôt.
// `PlayerPuppet` est une classe GLOBALE du jeu, donc `@wrapMethod` la résout sans histoire.
// ─────────────────────────────────────────────────────────────────────────────────────────────

@addField(PlayerPuppet)
public let m_tesseraVeilleCoffre: ref<TesseraCoffreVeille>;

@wrapMethod(PlayerPuppet)
protected cb func OnGameAttached() -> Bool {
  let res = wrappedMethod();
  if !IsDefined(this.m_tesseraVeilleCoffre) {
    this.m_tesseraVeilleCoffre = new TesseraCoffreVeille();
    this.m_tesseraVeilleCoffre.Init(this.GetGame());
  }
  return res;
}

/// Lit le contenu réel du coffre et l'annonce au serveur. Appelée à la fermeture de l'écran.
///
/// ⚠️ ÉTAT COMPLET, PAS DELTA — même raison que pour les sièges et pour le sac : un modèle par
/// événements suppose qu'aucun message ne se perd et qu'aucun n'arrive deux fois, et les deux
/// arrivent. Un état complet se recale tout seul.
public static func TesseraCoffreRapporter() -> Void {
  let game: GameInstance = GetGameInstance();
  let reseau: ref<NetworkGameSystem> = GameInstance.GetNetworkGameSystem();
  if !IsDefined(reseau) {
    return;
  }
  // ⚠️ LA GARDE QUI ÉVITE D'EFFACER UN COFFRE. `OnUninitialize` se déclenche pour TOUT écran
  // plein écran de type vendeur — un ripperdoc, un armurier, une réserve. Sans ce test, la
  // fermeture d'un vendeur annoncerait au serveur le contenu d'un coffre qu'on n'a pas ouvert.
  //
  // Le netcode remet `m_coffreVehicule` à zéro dès qu'un rapport part, donc « défini » veut bien
  // dire « une session de coffre est ouverte, et n'a pas encore été rapportée ».
  let idVehicule: EntityID = reseau.Tessera_CoffreVehicule();
  if !EntityID.IsDefined(idVehicule) {
    return;
  }

  // ── ON LIT LE CONTENANT, PAS LE VÉHICULE ──────────────────────────────────────────────────
  //
  // Le véhicule n'a jamais rien contenu (F-VEH-044) : lire son inventaire rendait invariablement
  // zéro ligne, ce qui se lit comme « le joueur a tout retiré » et fait effacer le coffre en base.
  // Le contenant est le porteur — celui-là même que l'écran vient de montrer.
  let joueur = GetPlayer(game);
  if !IsDefined(joueur) {
    return;
  }
  let veille: ref<TesseraCoffreVeille> = joueur.m_tesseraVeilleCoffre;
  if !IsDefined(veille) {
    TesseraCoffreLog("fermeture sans veille — rien annonce (le coffre en base reste tel quel)");
    return;
  }
  // La garde qui nous appartient. Elle se lit ET se consomme : voir `ConsommerSession`.
  if !veille.ConsommerSession() {
    return;
  }
  let porteur: ref<GameObject> = veille.PorteurExistant();
  if !IsDefined(porteur) {
    // ⚠️ ON NE RAPPORTE RIEN, et surtout pas « vide ». Un porteur disparu ne prouve pas que le
    // joueur a vidé le coffre — il prouve qu'on ne sait pas. Le serveur garde alors sa vérité,
    // et le pire cas est un dépôt perdu, jamais un coffre effacé.
    TesseraCoffreLog("fermeture sans porteur — rien annonce (ne pas confondre 'inconnu' et 'vide')");
    return;
  }
  let ts: ref<TransactionSystem> = GameInstance.GetTransactionSystem(game);
  if !IsDefined(ts) {
    return;
  }
  let presents: array<wref<gameItemData>>;
  ts.GetItemList(porteur, presents);

  reseau.Tessera_CoffreViderRapport();
  let i: Int32 = 0;
  while i < ArraySize(presents) {
    let donnees = presents[i];
    if IsDefined(donnees) {
      // Aucune soustraction : le porteur est parti VIDE (l'ouverture aurait été refusée sinon),
      // donc tout ce qui s'y trouve à la fermeture y a été mis par le joueur.
      reseau.Tessera_CoffreAjouterAuRapport(
        TDBID.ToStringDEBUG(ItemID.GetTDBID(donnees.GetID())), donnees.GetQuantity());
    }
    i += 1;
  }
  reseau.Tessera_CoffreEnvoyerRapport();
  // Le tampon repart vide. `Garnir` le viderait de toute façon à la prochaine ouverture — mais
  // laisser un pantin porter l'inventaire d'un coffre entre deux ouvertures, c'est laisser un
  // état qui n'appartient à personne. Le porteur ne contient quelque chose que pendant qu'un
  // écran le montre.
  ts.RemoveAllItems(porteur);
  TesseraCoffreLog(s"coffre ferme : \(ArraySize(presents)) ligne(s) annoncee(s), porteur vide");
}

public static func TesseraCoffreLog(message: String) -> Void {
  // `FTLog` et non `LogChannel` : c'est l'idiome du depot (ElevatorBridge.reds:51), et
  // `LogChannel` n'existe pas dans cette version de redscript.
  FTLog(s"[Tessera/Coffre] \(message)");
}

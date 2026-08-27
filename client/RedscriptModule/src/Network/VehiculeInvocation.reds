// ─────────────────────────────────────────────────────────────────────────────────────────────
// L'INVOCATION — le serveur ordonne, le JEU choisit l'emplacement, le serveur adopte
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// ⛔ LE PROBLÈME QU'ELLE FERME. Nos véhicules naissaient à des coordonnées ÉCRITES À LA MAIN :
// manifeste, base de données, reprise de session. Rien ne garantissait qu'elles soient sur le sol,
// sur une route, ni dégagées. Le moteur maintient la voiture là où on la pose ; à la seconde où un
// joueur monte, le contrôle bascule au local et la pénétration se résout d'un coup. La voiture part
// en tonneaux et perd des PV **pour de bon** — la casse se conserve par décision de Lucas
// (2026-08-27, pas de garage magique), donc une casse produite par NOUS est une avarie permanente
// que le joueur n'a pas causée.
//
// ⭐ LE RENVERSEMENT (F-VEH-054). Le serveur n'a pas besoin de CHOISIR la position — il a besoin
// qu'elle soit la même pour tous, et valide. Le jeu, lui, sait exactement ce qu'est un emplacement
// de route dégagé : c'est ce que fait le bouton du téléphone. On le lui demande donc, et le serveur
// adopte la réponse.
//
//     serveur  « sors la voiture <record> »      →  un ordre, aucun emplacement
//     client   invoque, lit OÙ le jeu l'a posée  →  la seule chose qu'il apporte
//     serveur  persiste, ré-annonce à tous       →  l'autorité reste entière
//
// ⚠️ LA VOITURE INVOQUÉE EST JETÉE. On ne l'adopte pas : c'est une voiture LOCALE, que les autres
// joueurs ne verraient pas, et l'adopter demanderait de réconcilier deux systèmes d'identité. Elle
// ne sert qu'à répondre à une question — **où** — et on la renvoie au garage aussitôt lue. C'est un
// ORACLE DE POSITION, pas un mécanisme de spawn.
//
// ── La recette, mesurée le 2026-08-27 (F-VEH-055) ────────────────────────────────────────────
//
// Les trois étapes sont nécessaires ; aucune ne suffit :
//
//     EnablePlayerVehicle("<record>_player", true)           → déverrouille au garage
//     TogglePlayerActiveVehicle(entrée.vehicleID, Car, true) → rend actif
//     SpawnActivePlayerVehicle(Car)                          → la voiture naît
//
// ⚠️ `SpawnPlayerVehicle` — la voie EXPLICITE — refuse toujours, avec `spawnOnlyOnValidRoad` à
// `true` comme à `false`, véhicule déverrouillé et actif, en pleine rue. Mesuré quatre fois. Deux
// fonctions du même système, l'une marche et l'autre pas, et rien dans leur signature ne le disait.
//
// ── Pourquoi PAS une veille à soi ────────────────────────────────────────────────────────────
//
// ⚠️ Ce fichier n'a NI classe de veille, NI `DelayCallback`, NI second `@wrapMethod(PlayerPuppet)`
// sur `OnGameAttached`. Le battement du coffre existe déjà, à la bonne cadence (200 ms), et il
// draine déjà la casse. Une seconde boucle coûterait à chaque frame de chaque joueur pour un
// événement rare, et un second wrap de la même méthode est une question qu'on n'a pas besoin de se
// poser. L'état vit sur le joueur, comme celui de la casse.

// ⚠️ Le MÊME module que ses voisins, pas un `import`. `NetworkGameSystem` est déclarée dans
// `Cyberverse.Network.Managers` ; un fichier du même module la voit sans rien importer, et
// `import Cyberverse.Network.NetworkGameSystem` — le chemin de la CLASSE et non du MODULE — donne
// un `unresolved import` qui fait ensuite échouer chaque usage du type, en cascade.
module Cyberverse.Network.Managers

// ── L'ÉTAT, sur le joueur ────────────────────────────────────────────────────────────────────

/// Le dernier ordre traité. ⚠️ Un COMPTEUR, pas un booléen : un joueur sort sa voiture plusieurs
/// fois, et deux ordres successifs pour le même véhicule seraient indiscernables par un drapeau.
@addField(PlayerPuppet)
public let m_tesseraInvocationSeq: Int32;

/// Combien de battements il reste à attendre la naissance. `0` = aucune invocation en vol.
@addField(PlayerPuppet)
public let m_tesseraInvocationAttente: Int32;

/// Le record demandé — mémorisé parce que le netcode l'efface dès que le rapport part, et qu'on en
/// a besoin pour reconnaître la voiture née puis la renvoyer au garage.
@addField(PlayerPuppet)
public let m_tesseraInvocationRecord: String;

// ── LE DRAINAGE, appelé par le battement du coffre ───────────────────────────────────────────

/// À appeler à chaque battement. Ne fait rien tant qu'aucun ordre n'est arrivé.
public static func TesseraInvocationDrainer(game: GameInstance) -> Void {
  let reseau: ref<NetworkGameSystem> = GameInstance.GetNetworkGameSystem();
  if !IsDefined(reseau) {
    return;
  }
  let joueur = GetPlayer(game) as PlayerPuppet;
  if !IsDefined(joueur) {
    return;
  }

  let seq: Int32 = reseau.Tessera_InvocationSeq();
  if seq != joueur.m_tesseraInvocationSeq {
    joueur.m_tesseraInvocationSeq = seq;
    TesseraInvocationLancer(game, reseau, joueur);
    return;
  }

  // La naissance est ASYNCHRONE : on regarde à chaque battement, et on borne l'attente.
  if joueur.m_tesseraInvocationAttente > 0 {
    joueur.m_tesseraInvocationAttente -= 1;
    if TesseraInvocationRecolter(game, reseau, joueur) {
      joueur.m_tesseraInvocationAttente = 0;
    } else {
      if joueur.m_tesseraInvocationAttente == 0 {
        // ⚠️ BORNÉ, et on le DIT. Une attente sans fin laisserait le joueur devant une commande qui
        // a répondu « ta voiture arrive » et une voiture qui n'arrive jamais — sans que rien nulle
        // part ne dise pourquoi.
        TesseraInvocationLog("AUCUNE voiture nee apres 6 s — invocation abandonnee (rien rapporte au serveur)");
      }
    }
  }
}

/// Relit, une par une, les conditions que le JEU consulte avant d'autoriser une invocation.
///
/// ⭐ La liste n'est pas devinée : c'est celle de `VehicleSystem.IsSummoningVehiclesRestricted`
/// (`vehicleSystem.script:49`), lue chez le consommateur. Chaque ligne nomme sa cause et son état,
/// pour qu'un refus se lise en une fois au lieu d'un lancement de jeu par hypothèse.
public static func TesseraInvocationDiagnostic(game: GameInstance, vs: ref<VehicleSystem>) -> Void {
  TesseraInvocationLog("  >>> POURQUOI le jeu refuse — les conditions qu'il consulte :");

  // 1. Un FAIT DE QUÊTE. Rien à voir avec le véhicule : tant qu'il vaut 0, le jeu considère que le
  //    joueur n'a pas encore débloqué l'appel de véhicule. Un monde sans prologue le laisse à 0.
  let fait: Int32 = GameInstance.GetQuestsSystem(game).GetFact(n"unlock_car_hud_dpad");
  TesseraInvocationLog(s"     fait 'unlock_car_hud_dpad' = \(fait)   (0 = BLOQUE)");

  // 2. Le joueur est-il monté ? Le jeu refuse l'invocation depuis un véhicule (hors menu radial).
  let dedans: Bool = VehicleSystem.IsPlayerInVehicle(game);
  TesseraInvocationLog(s"     joueur dans un vehicule = \(dedans)   (true = BLOQUE)");

  // 3. L'état du garage, lu sur le tableau noir que le jeu alimente.
  let bb: ref<IBlackboard> = GameInstance.GetBlackboardSystem(game).Get(GetAllBlackboardDefs().VehicleSummonData);
  let etat: Uint32 = bb.GetUint(GetAllBlackboardDefs().VehicleSummonData.GarageState);
  TesseraInvocationLog(s"     etat du garage = \(etat)   (0=aucun vehicule, 1=SummonAvailable, 2=SummonDisabled)");

  // ⚠️ L ETAT DE LA LIVRAISON EN COURS. Une invocation deja EnRoute fait refuser la suivante — et
  // cet etat est COLLANT : si la voiture n arrive jamais (monde vide, pas de trafic), il ne
  // redescend pas tout seul et toute invocation ulterieure echoue en silence.
  let livraison: Uint32 = bb.GetUint(GetAllBlackboardDefs().VehicleSummonData.SummonState);
  TesseraInvocationLog(s"     etat de la livraison = \(livraison)   (0=Idle attendu ; >0 = une livraison est deja en cours)");
  let dejaNee: EntityID = bb.GetEntityID(GetAllBlackboardDefs().VehicleSummonData.SummonedVehicleEntityID);
  TesseraInvocationLog(s"     vehicule deja invoque = \(EntityID.IsDefined(dejaNee))   (true = une voiture occupe deja la place)");

  // 4. Le garage contient-il seulement quelque chose ? Un garage vide bloque, même déverrouillé.
  let debloques: array<PlayerVehicle>;
  vs.GetPlayerUnlockedVehicles(debloques);
  TesseraInvocationLog(s"     vehicules debloques = \(ArraySize(debloques))   (0 = BLOQUE)");

  // 5. Le temps de recharge : deux invocations rapprochées, la seconde est refusée.
  let recharge: Bool = vs.IsActivePlayerVehicleOnCooldown(gamedataVehicleType.Car);
  TesseraInvocationLog(s"     temps de recharge actif = \(recharge)   (true = BLOQUE)");

  // 6. Le verdict d'ensemble du jeu, celui que son UI utilise pour griser le bouton.
  let interdit: Bool = VehicleSystem.IsSummoningVehiclesRestricted(game);
  TesseraInvocationLog(s"     >>> verdict du jeu : invocation interdite = \(interdit)");
}

/// Exécute la recette du jeu. Journalise CHAQUE étape : « accepté » n'est pas « exécuté »
/// (F-VEH-014), et trois booléens `true` ont déjà menti dans ce domaine.
public static func TesseraInvocationLancer(game: GameInstance, reseau: ref<NetworkGameSystem>, joueur: ref<PlayerPuppet>) -> Void {
  let record: String = reseau.Tessera_InvocationRecord();
  if StrLen(record) == 0 {
    TesseraInvocationLog("ordre recu SANS record — rien a invoquer");
    return;
  }
  joueur.m_tesseraInvocationRecord = record;
  TesseraInvocationLog(s"ordre : invoquer \(record)");

  let vs: ref<VehicleSystem> = GameInstance.GetVehicleSystem(game);
  if !IsDefined(vs) {
    TesseraInvocationLog("VehicleSystem injoignable");
    return;
  }

  // ⚠️ ÉTAPE 1 — le garage ne connaît QUE les variantes `_player`. Une variante de circulation est
  // refusée, et le refus serait muet côté joueur (F-VEH-055).
  let ouvert: Bool = vs.EnablePlayerVehicle(record, true, false);
  TesseraInvocationLog(s"  EnablePlayerVehicle -> \(ouvert)");
  if !ouvert {
    TesseraInvocationLog(s"  >>> record REFUSE par le garage — le serveur doit envoyer une variante _player : \(record)");
    return;
  }

  // ⚠️ ÉTAPE 2 — rendre ACTIF. Sans elle la naissance échoue : mesuré, véhicule déverrouillé, en
  // pleine rue, et l'invocation refusait tant qu'aucun véhicule n'était actif.
  //
  // ⭐ `GarageVehicleID.Resolve(record)` est le résolveur DU JEU (`vehicleSystem.script:157`), et il
  // remplace tout un parcours de `GetPlayerVehicles()`. Une première version cherchait l'entrée dans
  // la liste pour lire un champ `vehicleID` — **qui n'existe pas** : `PlayerVehicle` porte `name`,
  // `recordID`, `vehicleType`, `isUnlocked`… mais aucun identifiant de garage. Lire la structure
  // avant d'écrire aurait évité deux allers-retours de compilation.
  // ⭐ ON COPIE LA RECETTE DU JEU, ligne pour ligne — `quickSlotsManager.script:1047`
  // (`SetActiveVehicle`). Elle ne fait pas ce qu'on faisait :
  //
  //   * elle passe le **`recordID` de l'entrée de garage** (un `TweakDBID`, converti implicitement
  //     en `GarageVehicleID`), pas un `GarageVehicleID.Resolve(<chaîne>)` ;
  //   * elle passe le **`vehicleType` de l'entrée**, jamais un `Car` supposé ;
  //   * et elle vérifie `TDBID.IsValid` avant de toucher à quoi que ce soit.
  //
  // ⚠️ Mesuré le 2026-08-27 : avec la version approchée, le jeu répondait pourtant
  // `invocation interdite = false` — donc AUCUNE de ses six conditions n'était en cause — et
  // `SpawnActivePlayerVehicle` refusait quand même. Un refus sans condition nommée, c'est le signe
  // qu'il n'y a **rien à faire naître** : le véhicule actif n'avait pas été posé.
  let entree: PlayerVehicle;
  let trouvee: Bool = false;
  let garage: array<PlayerVehicle>;
  vs.GetPlayerUnlockedVehicles(garage);
  let i: Int32 = 0;
  while i < ArraySize(garage) {
    // ⛔ ON COMPARE DES IDENTIFIANTS, JAMAIS DES NOMS.
    //
    // La resolution TweakDBID -> chaine est INTERMITTENTE : selon ce que le jeu a charge,
    // `ToStringDEBUG` rend le nom lisible... ou `<TDBID:7B4A...>`. Mesure du 2026-08-27 : la
    // premiere version comparait des chaines et declarait le record ABSENT du garage alors qu il y
    // etait — le garage rendait des hachages ce jour-la.
    //
    // `TDBID.Create(record)` hache la meme chaine avec la meme fonction : la comparaison est alors
    // exacte et ne depend plus de ce que le jeu sait re-traduire.
    if Equals(garage[i].recordID, TDBID.Create(record)) {
      entree = garage[i];
      trouvee = true;
    }
    i += 1;
  }
  if !trouvee {
    TesseraInvocationLog(s"  >>> record DEVERROUILLE mais ABSENT du garage — \(ArraySize(garage)) entree(s) listee(s)");
    let j: Int32 = 0;
    while j < ArraySize(garage) {
      TesseraInvocationLog(s"       garage[\(j)] = \(TDBID.ToStringDEBUG(garage[j].recordID))");
      j += 1;
    }
    return;
  }
  vs.TogglePlayerActiveVehicle(Cast<GarageVehicleID>(entree.recordID), entree.vehicleType, true);

  // Le jeu ne se contente pas de basculer : il RELIT. Nous aussi — « accepté » n'est pas « fait ».
  let actif: PlayerVehicle = vs.GetActivePlayerVehicle(entree.vehicleType);
  TesseraInvocationLog(s"  vehicule ACTIF apres bascule = \(TDBID.ToStringDEBUG(actif.recordID))");

  // ⚠️ ÉTAPE 3 — la voie du véhicule ACTIF. La voie explicite refuse toujours.
  let ne: Bool = vs.SpawnActivePlayerVehicle(entree.vehicleType);
  TesseraInvocationLog(s"  SpawnActivePlayerVehicle -> \(ne)");
  if !ne {
    // ⛔ UN `false` NU N'EST PAS UN DIAGNOSTIC — et le natif ne dit jamais POURQUOI.
    //
    // `SpawnActivePlayerVehicle` est un `import function` : aucun corps à lire, aucun message. Mais
    // le jeu, lui, PUBLIE ses conditions dans `VehicleSystem.IsSummoningVehiclesRestricted`
    // (`vehicleSystem.script:49`) — six causes distinctes, dont deux qui n'ont rien à voir avec le
    // véhicule. On les relit ici, une par une, au moment exact du refus.
    //
    // ⚠️ Sans ça, le refus se diagnostique par élimination, une hypothèse par lancement de jeu.
    // Mesuré le 2026-08-27 : deux hypothèses écartées à la main (joueur assis, record refusé) avant
    // de comprendre qu'il fallait simplement demander au jeu.
    TesseraInvocationDiagnostic(game, vs);

    // ⚠️ DERNIER DISCRIMINANT, et il isole la SEULE cause qui ne soit pas dans le script du jeu.
    //
    // Les six conditions publiées passent, le véhicule actif est le bon, et le natif refuse quand
    // même. Ce qui reste est invisible depuis le script : la recherche d'un **emplacement de route
    // valable**. `SpawnPlayerVehicle` prend justement un parametre `spawnOnlyOnValidRoad` — le
    // mettre a `false` demande au jeu de poser la voiture SANS cette exigence.
    //
    // Si cet appel réussit là où l'autre échoue, la cause est nommée : le joueur n'est pas sur une
    // route que le système d'invocation sait desservir.
    let horsRoute: Bool = vs.SpawnPlayerVehicle(entree.vehicleType, entree.recordID, false);
    TesseraInvocationLog(s"     SpawnPlayerVehicle(sans exigence de route) -> \(horsRoute)");
    if horsRoute {
      TesseraInvocationLog("     >>> CAUSE NOMMEE : aucune ROUTE valable ici. La voiture peut naitre, pas se livrer.");
      joueur.m_tesseraInvocationAttente = 30;
    }
    return;
  }
  // 30 battements a 200 ms = 6 s. La naissance passe par le streaming.
  joueur.m_tesseraInvocationAttente = 30;
}

/// Cherche la voiture qui vient de naître, lit sa position, la renvoie au garage, et rapporte.
///
/// ⚠️ **LE DISCRIMINANT EST LE RECORD, PAS UN SUIVI D'ENTITÉS.** Une première version notait les
/// voitures présentes AVANT pour repérer l'intruse. C'était plus fragile pour rien : la voiture
/// invoquée porte la variante `_player`, que **rien d'autre ne porte** dans notre monde — nos
/// véhicules serveur naissent avec la variante de circulation. Un discriminant qui tient dans une
/// comparaison vaut mieux qu'un état à tenir entre deux battements.
///
/// ⚠️ `GetEntitiesAroundObject` ne rend AUCUNE entité du netcode (F-PLF-045), et c'est ce qui rend
/// cet instrument JUSTE ici : on cherche précisément une voiture née du JEU. Le même appel serait le
/// mauvais outil pour chercher un véhicule serveur.
public static func TesseraInvocationRecolter(game: GameInstance, reseau: ref<NetworkGameSystem>, joueur: ref<PlayerPuppet>) -> Bool {
  let vise: TweakDBID = TDBID.Create(joueur.m_tesseraInvocationRecord);
  // ⚠️ `array<ref<Entity>>`, PAS `array<wref<GameObject>>` — la signature réelle de
  // `GetEntitiesAroundObject` (`gameObject.script`). Le compilateur refuse la coercition, et c'est
  // tant mieux : un `wref` là où le jeu rend un `ref` aurait compilé chez d'autres et cassé ici.
  let autour: array<ref<Entity>> = joueur.GetEntitiesAroundObject(120.0);
  let i: Int32 = 0;
  while i < ArraySize(autour) {
    let v = autour[i] as VehicleObject;
    // ⚠️ DEUX conditions, et la seconde n'est pas du zèle. Le record seul suffisait tant que nos
    // véhicules serveur portaient la variante de CIRCULATION — mais le garage n'accepte que les
    // variantes `_player` (F-VEH-055), donc la voiture du serveur finira par porter le même record
    // que l'invoquée. À ce moment-là, « la première qui a ce record » désignerait une fois sur deux
    // la voiture du serveur, garée ailleurs, et on lui rapporterait SA propre position comme si le
    // jeu venait de la choisir.
    //
    // `Tessera_EstVehiculeReseau` tranche sans ambiguïté : la voiture invoquée est LOCALE, le
    // serveur ne la connaît pas.
    if IsDefined(v) && Equals(v.GetRecordID(), vise) && !reseau.Tessera_EstVehiculeReseau(v.GetEntityID()) {
      let ou: Vector4 = v.GetWorldPosition();
      TesseraInvocationLog(s"  voiture NEE en \(ou.X) \(ou.Y) \(ou.Z) — position rapportee au serveur");
      reseau.Tessera_RapporterInvocation(ou.X, ou.Y, ou.Z);

      // ⚠️ ON LA RENVOIE AU GARAGE. Sans ça, le joueur se retrouve avec DEUX voitures : la locale,
      // que lui seul voit, et celle du serveur qui va naître au même endroit. Il monterait dans la
      // mauvaise une fois sur deux, et personne ne le verrait conduire.
      let vs: ref<VehicleSystem> = GameInstance.GetVehicleSystem(game);
      if IsDefined(vs) {
        vs.DespawnPlayerVehicle(GarageVehicleID.Resolve(joueur.m_tesseraInvocationRecord));
        TesseraInvocationLog("  voiture locale renvoyee au garage (seule celle du serveur reste)");
      }
      return true;
    }
    i += 1;
  }
  return false;
}

public static func TesseraInvocationLog(message: String) -> Void {
  FTLog(s"[Tessera/Invocation] \(message)");
}

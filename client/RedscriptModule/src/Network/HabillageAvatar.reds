module Cyberverse.Network.Managers

// L'AVATAR D'UN JOUEUR PORTE CE QUE LE SERVEUR DIT QU'IL PORTE.
//
// ── Ce que ce fichier fait ────────────────────────────────────────────────────────────────────
//
// Le serveur est l'autorité sur l'inventaire ET sur l'équipement (demande de Lucas du 2026-08-23 :
// « il faut que ce soit le serveur qui arbitre et qui soit l'autorité pour ce qu'on possède et ce
// qu'on a d'équipé »). Sa base de données porte, pour chaque item d'un personnage, s'il est
// simplement possédé ou effectivement **porté** (colonne `contenus.porte`, migration 0012). Ces
// items descendent dans `AppearanceSync.garments` avec `drawn = false`, le C++ les range
// (`NetworkAppearance::vetements`) et les expose par deux natives. Ce fichier les pose sur le corps.
//
// ── ⚠️⚠️ QUI DÉCLENCHE, ET POURQUOI CE N'EST PAS `OnGameAttached` ────────────────────────────
//
// La première version accrochait `@wrapMethod(ScriptedPuppet) OnGameAttached`, en copiant
// `ArmeAvatar.reds`. Mesuré le 2026-08-24 : sur un corps né de la **voie enrichie** — celui qui
// porte le V du joueur — ce hook ne se déclenche **jamais**. Zéro ligne de journal, deux instances.
// Il ne marchait que sur les pantins de passant de la voie sûre.
//
// Le déclencheur est donc `PiloterAvatar`, côté C++ : le seul chemin dont on sait qu'il atteint ces
// corps, puisque c'est lui qui les fait marcher. Il appelle `TesseraHabillerAvatar` par créneaux,
// borné, et s'arrête dès que cette fonction rend `true`.
//
// ── ⭐ AVEC QUOI ON MESURE, ET LES TROIS INSTRUMENTS QUI ÉTAIENT FAUX ────────────────────────
//
// Ça vaut d'être écrit plutôt que réappris :
//
//   1. `GetItemInSlot` — ⚠️ **PAS FIABLE**. Mesuré le 2026-08-24 avec un témoin : sur le JOUEUR
//      LOCAL, qui porte visiblement des vêtements, il rend `null` pour `AttachmentSlots.Chest`
//      pendant que `IsSlotEmpty` rend `false`. Un objet peut donc être là sans que cette fonction
//      le rende. F-PLY-185 recommandait cet appel — c'était vrai pour son cas, pas pour celui-ci.
//   2. `InitializeSlots` — ⚠️ **NE COMPTE RIEN D'UTILE**. Rend `0` emplacement sur notre avatar…
//      **et `0` sur le joueur local aussi**. Le chiffre ne distingue pas « ce corps ne peut rien
//      porter » de « rien de neuf à monter ». Hypothèse morte en un seul lancement, grâce au témoin.
//   3. La valeur de retour de `AddItemToSlot` — elle rend `true` en ayant seulement accepté
//      (D1 : « accepté » ≠ « exécuté »).
//
// **Le verdict retenu est `IsSlotEmpty`**, corroboré par le témoin : `false` sur le joueur habillé,
// `true` sur notre avatar nu. Et `IsSlotSpawningAnyItem` à côté, parce qu'attacher un item fait
// NAÎTRE une entité — c'est asynchrone, et « rien » ne doit pas se confondre avec « en cours ».
//
// ── ⭐ QUELLE RECETTE, ET POURQUOI PAS CELLE DU PHOTOMODE ────────────────────────────────────
//
// La recette du photomode (F-PLY-203, `photoModePlayerEntity.script:91-101`) a été exécutée le
// 2026-08-24. Chaque geste réussit — l'item est donné, le slot est valide, `CanPlaceItemInSlot`
// accepte, `AddItemToSlot` rend `true`, et `IsSlotEmpty` rend ensuite `false` : l'item OCCUPE
// réellement le slot. Et pourtant Lucas confirme à l'œil, deux fois : **le corps est nu**. Un item
// de prévisualisation ne se rend apparemment que dans le contexte qui l'a inventé.
//
// La recette retenue est celle de l'**aperçu d'inventaire**
// (`BaseGarmentItemPreviewGameController`, `ItemPreviewGameController.script:195-210`) : c'est le
// seul témoin QUI MARCHE dont on dispose — il habille un `gamePuppet`, comme le nôtre, et le rend
// visible à l'écran. Deux gestes, sans item de prévisualisation et sans haute priorité.
//
//   grep "\[Habillage\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log

func TesseraJournalHabillage(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Habillage] " + texte);
    }
}

// Le corps derrière un `EntityID`, quelle que soit la voie qui l'a fait naître.
//
// ⚠️ `DynamicEntitySystem.GetEntity` ne connaît pas les corps de la voie enrichie : ils ne sont pas
// nés par lui. `GameInstance.FindEntityByID`, lui, interroge le monde. Un corps non résolu n'est
// pas une erreur, c'est « pas encore né » — l'appelant repassera.
func TesseraCorpsDeLEntite(cible: EntityID) -> ref<GameObject> {
    let jeu = GetGameInstance();
    let entite = GameInstance.GetDynamicEntitySystem().GetEntity(cible);
    if !IsDefined(entite) {
        entite = GameInstance.FindEntityByID(jeu, cible);
    }
    return entite as GameObject;
}

// Habille le corps avec CE QUE LE SERVEUR ANNONCE. Rend `true` quand tout est en place.
func TesseraHabillerLeCorps(cible: EntityID, passe: Uint32) -> Bool {
    let reseau = GameInstance.GetNetworkGameSystem();
    if !IsDefined(reseau) {
        return false;
    }
    let corps = TesseraCorpsDeLEntite(cible);
    if !IsDefined(corps) {
        return false; // pas encore né — l'appelant repassera
    }
    let transactions = GameInstance.GetTransactionSystem(GetGameInstance());
    if !IsDefined(transactions) {
        return false;
    }

    // On ne parle qu'à la première passe et à la dernière : une ligne par vêtement et par passe
    // ferait quarante lignes par avatar et par seconde — le régime qui a produit 19 000 lignes le
    // 2026-08-22 et rendu le journal inutilisable.
    let bavard = passe == 0u || passe == 9u;

    // ── ⭐ LE DÉCLENCHEUR NATIF SÉPARÉ, ENFIN NOMMÉ ───────────────────────────────────────────
    //
    // Le registre énonçait déjà la loi, trois fois vérifiée : « sur un pantin, écrire la DONNÉE
    // d'équipement réussit toujours, et l'instanciation du VISUEL exige un déclencheur natif
    // SÉPARÉ — arme par commande d'IA (F-PLY-036), garment par `EquipVisualsRequest` (F-PLY-010) ».
    // Ce déclencheur passe par `EquipmentSystemPlayerData`, et notre pantin n'en a pas :
    // `EquipmentSystem.GetData(corps)` rend `NULL`, parce que `GetPlayerData` ne cherche que dans
    // `m_ownerData`, la liste des propriétaires ENREGISTRÉS.
    //
    // ⭐ Et la porte d'enregistrement n'est pas réservée au joueur. `OnPlayerAttach`
    // (`equipmentSystem.script:5019`) prend `request.owner`, le caste en `ScriptedPuppet`, et lui
    // crée ses données — **sans jamais vérifier que c'est un joueur**. Un `PlayerAttachRequest` mis
    // en file sur le système suffit donc, en principe, à doter notre avatar du pipeline visuel
    // d'habillement.
    //
    // ⚠️ NON MESURÉ au moment où ces lignes sont écrites : c'est une lecture de script décompilé,
    // donc une hypothèse. Ce qui la tranche est la ligne de journal ci-dessous — `donnees=` dit si
    // l'enregistrement a pris. Si elle reste `absentes`, la voie est fermée et il faut le dire.
    //
    // ⚠️ L'enregistrement est ASYNCHRONE (mise en file), donc il ne peut pas servir dès la passe 0.
    // C'est sans conséquence : on a dix passes, et l'habillage se retente à chacune.
    if passe == 0u {
        let systeme = EquipmentSystem.GetInstance(corps);
        if IsDefined(systeme) {
            let demande = new PlayerAttachRequest();
            demande.owner = corps;
            systeme.QueueRequest(demande);
        }
    }
    if bavard {
        TesseraJournalHabillage(
            s"donnees d'equipement du corps : "
            + s"\(IsDefined(EquipmentSystem.GetData(corps)) ? "PRESENTES" : "absentes")");
    }

    if passe == 0u {
        // ⚠️ ON N'HABILLE PAS À LA PASSE 0, ET C'EST LE POINT. L'enregistrement ci-dessus est
        // mis en FILE : les données n'existent pas encore dans cette frame. Équiper maintenant,
        // c'est équiper avant que le pipeline visuel ne soit là — et comme le slot passe alors
        // en « apparition en cours », plus rien ne le réessaie. C'est exactement ce qui s'est
        // produit au premier tir : `donnees=PRESENTES` à la passe 9, et quatre vêtements posés
        // à la passe 0, donc trop tôt.
        return false;
    }

    let voulus = reseau.Tessera_NombreDeVetements(cible);
    let manquants = 0;
    let i = 0;
    while i < voulus {
        // ⚠️ RELU À CHAQUE TOUR, jamais mis en cache : un `AppearanceSync` peut avoir changé la
        // tenue entre deux passes. Un index hors bornes rend un TweakDBID invalide, que la ligne
        // suivante écarte.
        let vetement = reseau.Tessera_VetementDeLEntite(cible, i);
        if TDBID.IsValid(vetement)
            && !TesseraPoserVetement(transactions, corps, vetement, passe, bavard) {
            manquants += 1;
        }
        i += 1;
    }

    if manquants > 0 {
        if passe == 9u {
            TesseraJournalHabillage(
                s"⚠ \(manquants)/\(voulus) pièce(s) toujours absentes après 10 passes");
        }
        return false;
    }
    if voulus > 0 && passe > 0u {
        TesseraJournalHabillage(s"\(voulus) pièce(s) posée(s) en \(passe + 1u) passe(s)");
    }
    return true;
}

// Pose UN vêtement, et rend « ce vêtement est bien sur le dos du corps ».
func TesseraPoserVetement(transactions: ref<TransactionSystem>, corps: ref<GameObject>,
                          vetement: TweakDBID, passe: Uint32, bavard: Bool) -> Bool {
    let identifiant = ItemID.FromTDBID(vetement);
    let slot = EquipmentSystem.GetPlacementSlot(identifiant);
    if !TDBID.IsValid(slot) {
        // ⚠️ RENDU « POSÉ » DÉLIBÉRÉMENT, alors que rien n'a été posé. Un item sans slot de
        // placement ne l'aura jamais — le retenter dix fois ne ferait que masquer les vraies pièces
        // manquantes derrière du bruit. C'est le défaut de F-PLY-185 : `equipArea` vide, donc aucun
        // slot, donc pipeline visuel sans objet. On le NOMME, parce qu'un serveur qui distribue un
        // item inportable a un problème que personne ne verrait autrement.
        if bavard {
            TesseraJournalHabillage(
                s"⚠ \(TDBID.ToStringDEBUG(vetement)) n'a aucun slot de placement — ignoré");
        }
        return true;
    }

    // ── LE VERDICT, avant qu'on touche à quoi que ce soit ────────────────────────────────────
    //
    // `IsSlotEmpty` et lui seul : voir l'en-tête du fichier pour les trois instruments qui se sont
    // révélés faux avant celui-ci.
    // ⚠️ DEUX CONDITIONS, ET LA SECONDE EST LA LEÇON DU JOUR. « Le slot n'est pas vide » ne veut
    // PAS dire « le vêtement est là » : le `TransactionSystem` expose `IsSlotEmptySpawningItem` et
    // `IsSlotSpawningAnyItem` précisément parce qu'un slot peut être RÉSERVÉ par une apparition
    // d'entité en attente. Attacher un item fait NAÎTRE une entité, et c'est asynchrone.
    //
    // Se contenter de `!IsSlotEmpty` faisait déclarer « 4 pièces posées en 2 passes » sur un corps
    // que Lucas voyait nu. C'est le même défaut que la veille avec `GetItemInSlot`, dans l'autre
    // sens : un signal qui répond à côté de la question posée.
    let enCoursDApparition = transactions.IsSlotSpawningAnyItem(corps, slot);
    if !transactions.IsSlotEmpty(corps, slot) && !enCoursDApparition {
        return true;
    }
    // ⚠️ UN SLOT BLOQUÉ SE VIDE, IL NE S'ATTEND PAS. Mesuré : une « apparition en cours » sans
    // objet dure indéfiniment — dix secondes, puis toute la session. Ce n'est pas une latence,
    // et patienter dessus revient à ne rien faire. On repart donc de zéro sur ce slot, ce que
    // fait aussi l'aperçu d'inventaire avant de poser (`RemoveItemFromSlot` en tête de sa
    // séquence).
    if enCoursDApparition && !IsDefined(transactions.GetItemInSlot(corps, slot)) {
        transactions.RemoveItemFromSlot(corps, slot, true);
        enCoursDApparition = transactions.IsSlotSpawningAnyItem(corps, slot);
    }
    if enCoursDApparition {
        // On n'ordonne PAS par-dessus une naissance en cours : ce serait la relancer indéfiniment.
        // Mais on le DIT à la dernière passe — une apparition qui dure dix secondes n'est plus une
        // latence, c'est une panne, et c'est la seule ligne qui pourrait le révéler.
        if passe == 9u {
            // ⭐ ET LE TEMOIN, dans la meme ligne : le meme slot sur le JOUEUR LOCAL, qui porte un
            // vetement visible. C'est la quatrieme fois aujourd'hui qu'un signal est interroge sans
            // temoin ; les trois premieres fois il repondait a cote de la question
            // (F-PLY-276, F-PLY-277). Si `apparition` est vrai la-bas aussi, ce drapeau est
            // COLLANT et ne dit rien — et toute la conclusion tombe.
            let joueur = GameInstance.GetPlayerSystem(GetGameInstance())
                .GetLocalPlayerControlledGameObject();
            let sTemoin = "pas de joueur";
            if IsDefined(joueur) {
                sTemoin = s"vide=\(transactions.IsSlotEmpty(joueur, slot))"
                    + s" apparition=\(transactions.IsSlotSpawningAnyItem(joueur, slot))"
                    + s" objet=\(IsDefined(transactions.GetItemInSlot(joueur, slot)))";
            }
            TesseraJournalHabillage(
                s"⚠ \(TDBID.ToStringDEBUG(vetement)) — apparition TOUJOURS en cours après 10 passes"
                + s" · avatar : vide=\(transactions.IsSlotEmpty(corps, slot))"
                + s" objet=\(IsDefined(transactions.GetItemInSlot(corps, slot)))"
                + s" · TEMOIN joueur : \(sTemoin)");
        }
        return false;
    }

    // ── LA RECETTE DE L'APERÇU D'INVENTAIRE ──────────────────────────────────────────────────
    //
    // ⭐ C'est LE témoin qui marche, et il a mis longtemps à être trouvé :
    // `BaseGarmentItemPreviewGameController` (`ItemPreviewGameController.script:195-210`) habille
    // un `gamePuppet` — pas le joueur — et **le rend visible à l'écran**, dans l'écran d'inventaire.
    // Notre corps est un `gamePuppet`. C'est donc exactement notre cas, en fonctionnement.
    //
    //     transactionSystem.GiveItem( puppet, m_givenItem, 1 );
    //     transactionSystem.AddItemToSlot( puppet, m_placementSlot, m_givenItem );
    //
    // ⚠️ NI ITEM DE PRÉVISUALISATION, NI HAUTE PRIORITÉ. La recette du photomode (F-PLY-203) posait
    // un item créé par `CreatePreviewItemID` : mesuré le 2026-08-24, il OCCUPE bien le slot
    // (`IsSlotEmpty` rend `false`, sur les deux instances) et **n'affiche rien** — Lucas l'a
    // confirmé à l'œil deux fois. Un item de prévisualisation ne se rend apparemment que dans le
    // contexte qui l'a inventé.
    //
    // ⚠️ ET L'ITEM SE DONNE PAR LE MÊME IDENTIFIANT que celui qu'on équipe : un item donné par une
    // autre voie reçoit un `ItemID` dynamique différent, et l'équipement échoue ensuite sur un item
    // que le pantin possède pourtant (`ArmeAvatar.reds`, mesuré le 2026-07-24).
    let donne = true;
    if !transactions.HasItem(corps, identifiant) {
        donne = transactions.GiveItem(corps, identifiant, 1);
    }
    let peut = transactions.CanPlaceItemInSlot(corps, slot, identifiant);
    let pose = transactions.AddItemToSlot(corps, slot, identifiant);

    if bavard {
        TesseraJournalHabillage(
            s"\(TDBID.ToStringDEBUG(vetement)) · slot=\(TDBID.ToStringDEBUG(slot))"
            + s" · donne=\(donne) · peut=\(peut) · pose=\(pose)"
            + s" · slot_vide=\(transactions.IsSlotEmpty(corps, slot))"
            + s" · apparition=\(transactions.IsSlotSpawningAnyItem(corps, slot))");
    }
    // Jamais `true` ici : c'est la passe SUIVANTE qui constatera, sur le corps lui-même.
    return false;
}

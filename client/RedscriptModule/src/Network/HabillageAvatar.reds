module Cyberverse.Network.Managers

// L'AVATAR D'UN JOUEUR PORTE CE QUE LE SERVEUR DIT QU'IL PORTE.
//
// ── Ce que ce fichier fait, et d'où vient sa recette ──────────────────────────────────────────
//
// Le serveur est l'autorité sur l'inventaire ET sur l'équipement (demande de Lucas du 2026-08-23 :
// « il faut que ce soit le serveur qui arbitre et qui soit l'autorité pour ce qu'on possède et ce
// qu'on a d'équipé »). Sa base de données porte, pour chaque item d'un personnage, s'il est
// simplement possédé ou effectivement **porté** (colonne `contenus.porte`, migration 0012). Ces
// items descendent dans `AppearanceSync.garments` avec `drawn = false`, et le C++ les range
// (`NetworkAppearance::vetements`). Ce fichier les pose sur le pantin.
//
// La recette vient de F-PLY-203 : c'est celle du **photomode**, lue dans
// `photoModePlayerEntity.script:91-101` (`PutOnFakeItem`), et nos pantins sont justement des
// pantins de photomode enrichis. Elle tient en quatre gestes :
//
//     GivePreviewItemByItemID  →  CreatePreviewItemID  →  GetPlacementSlot  →  AddItemToSlot(…, true)
//
// ⚠️ **STATUT : F-PLY-203 est une HYPOTHÈSE.** Elle a été lue dans le script décompilé de CDPR et
// JAMAIS exécutée ici. Ce fichier est donc la sonde qui la tranche — pas l'application d'un fait
// acquis. Si l'habillage ne prend pas, la recette est en cause avant le câblage.
//
// ⚠️ **CE QUE L'IMPASSE F-PLY-185 NE COUVRE PAS.** Elle dit que « les items de customization ne se
// posent pas sur un pantin du monde », et elle a essayé cette voie exacte — mais avec
// `Items.CharacterCustomizationMaHead`, dont l'`equipArea` est VIDE : `GetPlacementSlot` ne rend
// rien, et le pipeline visuel sort immédiatement. Un vêtement réel a une zone d'équipement valide.
// L'impasse porte sur les items de CUSTOMISATION, pas sur les VÊTEMENTS — c'est pourquoi D3 ne
// nous arrête pas ici.
//
// ── Pourquoi une boucle qui VÉRIFIE, et pas une pose unique ───────────────────────────────────
//
// C'est la leçon déjà payée par `ArmeAvatar.reds` le 2026-08-10 : un système qui envoie un ordre et
// tient sa propre comptabilité pour la vérité ne remarque jamais qu'un ordre a été avalé. Un pantin
// qui vient de naître n'a fini ni son inventaire ni ses slots d'attache — les ordres y sont
// acceptés **sans effet**, ce que D1 appelle un succès trompeur.
//
// On ne mémorise donc rien : à chaque passe on lit ce que l'avatar porte RÉELLEMENT
// (`GetItemInSlot`, jamais la valeur de retour de `AddItemToSlot` — c'est la règle que F-PLY-185 a
// établie au prix de quatre voies) et on repose ce qui manque. La seule mémoire est l'avatar.
//
// ⚠️ ET LA BOUCLE SE RÉ-ARME TOUJOURS, sans condition — la leçon du battement de l'écran de mort
// (2026-08-09) : un ré-armement enfermé dans un test s'arrête un jour, et plus personne ne comprend
// pourquoi l'état s'est figé. C'est aussi ce qui fait qu'un CHANGEMENT de tenue en cours de partie
// est ramassé sans qu'aucun événement n'ait à être câblé.
//
//   grep "\[Habillage\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log

func TesseraJournalHabillage(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Habillage] " + texte);
    }
}

// ⚠️ Différé de 2 s après l'attachement, exactement comme l'arme et pour la même raison mesurée :
// avant ça, le pantin accepte les ordres sans les exécuter.
@wrapMethod(ScriptedPuppet)
protected cb func OnGameAttached() -> Bool {
    let resultat = wrappedMethod();
    if TesseraEstSousAutoriteServeur(this.GetEntityID()) {
        GameInstance.GetDelaySystem(GetGameInstance())
            .DelayCallback(TesseraHabillageAvatar.Creer(this.GetEntityID(), 0u), 2.0, false);
    }
    return resultat;
}

public class TesseraHabillageAvatar extends DelayCallback {
    let cible: EntityID;
    // Passes depuis la dernière convergence. Sert au JOURNAL, pas au contrôle : une boucle qui
    // retente en silence cache sa propre panne.
    let essais: Uint32;

    public static func Creer(cible: EntityID, essais: Uint32) -> ref<TesseraHabillageAvatar> {
        let h = new TesseraHabillageAvatar();
        h.cible = cible;
        h.essais = essais;
        return h;
    }

    public func Call() -> Void {
        let jeu = GetGameInstance();
        let reseau = GameInstance.GetNetworkGameSystem();
        if !IsDefined(reseau) {
            return; // plus de session réseau — on s'arrête, et c'est une condition nommée
        }
        // ⚠️ L'AVATAR DISPARU ARRÊTE LA BOUCLE. Une sortie d'AoI détruit l'entité ; se ré-armer
        // dessus laisserait une callback par avatar jamais revu tourner jusqu'à la fin de la
        // session. Au respawn, `OnGameAttached` en repose une neuve.
        let avatar = GameInstance.GetDynamicEntitySystem().GetEntity(this.cible) as ScriptedPuppet;
        if !IsDefined(avatar) {
            return;
        }
        let transactions = GameInstance.GetTransactionSystem(jeu);
        if !IsDefined(transactions) {
            return;
        }

        let voulus = reseau.Tessera_NombreDeVetements(this.cible);
        let manquants = 0;
        let i = 0;
        while i < voulus {
            // ⚠️ RELU À CHAQUE TOUR, jamais mis en cache : un `AppearanceSync` peut avoir changé la
            // tenue entre deux passes. Un index hors bornes rend un TweakDBID invalide, que la
            // ligne suivante écarte.
            let vetement = reseau.Tessera_VetementDeLEntite(this.cible, i);
            if TDBID.IsValid(vetement) && !this.Poser(transactions, avatar, vetement) {
                manquants += 1;
            }
            i += 1;
        }

        if manquants == 0 {
            // On ne journalise QUE si ça a demandé des essais : une ligne par tour et par avatar
            // noierait le fichier — c'est ce qui est arrivé le 2026-08-22 (19 000 lignes).
            if this.essais > 0u {
                TesseraJournalHabillage(
                    s"\(voulus) pièce(s) posée(s) en \(this.essais + 1u) passe(s)");
            }
            // Converge : on repasse lentement, pour ramasser un changement de tenue.
            GameInstance.GetDelaySystem(jeu)
                .DelayCallback(TesseraHabillageAvatar.Creer(this.cible, 0u), 2.0, false);
            return;
        }

        // ⚠️ ON NOMME CE QUI RÉSISTE, et on continue quand même. Dix passes sans effet est une
        // information — mais abandonner rendrait l'avatar définitivement nu là où la passe suivante
        // aurait pu réussir (l'inventaire du pantin finit de se monter à son rythme).
        if this.essais == 9u {
            TesseraJournalHabillage(
                s"⚠ \(manquants)/\(voulus) pièce(s) toujours absentes après 10 passes");
        }
        GameInstance.GetDelaySystem(jeu)
            .DelayCallback(TesseraHabillageAvatar.Creer(this.cible, this.essais + 1u), 0.5, false);
    }

    // Pose UN vêtement, et rend « ce vêtement est bien sur le dos de l'avatar ».
    //
    // ⚠️ LE VERDICT EST `GetItemInSlot`, PAS LA VALEUR DE RETOUR DES ORDRES. `AddItemToSlot` rend
    // `true` en ayant seulement accepté (D1 : « accepté » ≠ « exécuté »). C'est la règle que
    // F-PLY-185 a établie au prix de quatre voies explorées.
    //
    // ── DEUX RECETTES, EN ALTERNANCE — et c'est délibéré ──────────────────────────────────────
    //
    // Mesuré le 2026-08-23, premier passage en jeu : la recette du photomode (F-PLY-203) ne pose
    // RIEN sur nos pantins — « 4/4 pièces toujours absentes après 10 passes ». C'était une
    // hypothèse, jamais exécutée ; elle vient d'être exécutée, et elle ne suffit pas.
    //
    // Or ce dépôt en connaît une SECONDE, celle-là mesurée : `ArmeAvatar.reds` équipe réellement
    // une arme sur un pantin distant, par `GiveItem` + `AddItemToSlot` avec l'`ItemID` DIRECT — pas
    // un item de prévisualisation. Elle marche depuis le 2026-07-24.
    //
    // Plutôt que de choisir à l'aveugle, on les alterne : passe paire = photomode, passe impaire =
    // arme. Chacune a cinq tentatives sur les dix, le journal nomme celle qui a été essayée, et
    // celle qui converge gagne. Un aller-retour en jeu coûte cinq minutes ; en faire un par
    // hypothèse quand une seule suffit est du temps qu'on ne remet pas.
    private func Poser(transactions: ref<TransactionSystem>, avatar: ref<ScriptedPuppet>,
                       vetement: TweakDBID) -> Bool {
        let identifiant = ItemID.FromTDBID(vetement);
        let slot = EquipmentSystem.GetPlacementSlot(identifiant);
        // On ne parle qu'à la première passe et à la dixième : une ligne par vêtement et par passe
        // ferait quarante lignes par avatar et par seconde — le régime qui a produit 19 000 lignes
        // le 2026-08-22 et rendu le journal inutilisable.
        let bavard = this.essais == 0u || this.essais == 9u;

        if !TDBID.IsValid(slot) {
            // ⚠️ RENDU « POSÉ » DÉLIBÉRÉMENT, alors que rien n'a été posé. Un item sans slot de
            // placement ne l'aura jamais — le retenter dix fois par avatar ne ferait que masquer
            // les vraies pièces manquantes derrière du bruit. C'est exactement le défaut de
            // F-PLY-185 : `equipArea` vide, donc aucun slot, donc pipeline visuel sans objet.
            if this.essais == 0u {
                TesseraJournalHabillage(
                    s"⚠ \(TDBID.ToStringDEBUG(vetement)) n'a aucun slot de placement — ignoré");
            }
            return true;
        }

        // ── CE QUE L'AVATAR PORTE VRAIMENT, avant qu'on touche à quoi que ce soit ────────────
        let present = transactions.GetItemInSlot(avatar, slot);
        let occupant = "vide";
        if IsDefined(present) {
            occupant = TDBID.ToStringDEBUG(ItemID.GetTDBID(present.GetItemID()));
            if TDBID.ToNumber(ItemID.GetTDBID(present.GetItemID())) == TDBID.ToNumber(vetement) {
                return true;
            }
        }

        let recette = "";
        let peut = false;
        let pose = false;
        if this.essais % 2u == 0u {
            // ── RECETTE PHOTOMODE (F-PLY-203) — item de PRÉVISUALISATION ────────────────
            recette = "photomode";
            transactions.GivePreviewItemByItemID(avatar, identifiant);
            let apercu = transactions.CreatePreviewItemID(identifiant);
            peut = transactions.CanPlaceItemInSlot(avatar, slot, apercu);
            if peut {
                pose = transactions.AddItemToSlot(avatar, slot, apercu, true);
            }
        } else {
            // ── RECETTE ARME (mesurée sur pantin distant depuis le 2026-07-24) ──────────
            //
            // ⚠️ On donne l'item AVANT de l'équiper, et par le MÊME identifiant que celui demandé.
            // Un item donné par une autre voie reçoit un `ItemID` dynamique différent, et
            // l'équipement échoue ensuite sur un item que le pantin possède pourtant
            // (`ArmeAvatar.reds`, mesuré).
            recette = "arme";
            if !transactions.HasItem(avatar, identifiant) {
                transactions.GiveItem(avatar, identifiant, 1);
            }
            peut = transactions.CanPlaceItemInSlot(avatar, slot, identifiant);
            pose = transactions.AddItemToSlot(avatar, slot, identifiant, true);
        }

        if bavard {
            TesseraJournalHabillage(
                s"\(TDBID.ToStringDEBUG(vetement)) · slot=\(TDBID.ToStringDEBUG(slot))"
                + s" · occupant=\(occupant) · recette=\(recette) · peut=\(peut) · pose=\(pose)");
        }
        // Jamais `true` ici : c'est la passe SUIVANTE qui constatera, sur l'avatar lui-même.
        return false;
    }
}

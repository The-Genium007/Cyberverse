module Cyberverse.Network.Managers

// L'arme en main d'un joueur, vue par tous les autres.
//
// ── Ce qui est MESURÉ, et qui a demandé trois tours ───────────────────────────────────────────
//
// Donner l'arme au pantin et remplir son slot **réussit** et **ne montre rien** :
//
//     GiveItem = true · AddItemToSlot = true · slot vide apres = false   →  mains VIDES
//
// C'est le cas d'école « accepté ≠ affiché » (D1), et la même frontière que F-PLY-010 avait
// établie pour les vêtements : sur un pantin de foule, l'esthétique est une couche à part, portée
// par le natif, qu'une écriture de données n'atteint pas. `EquipVisualsRequest`, que le registre
// citait comme ce porteur natif, est une impasse ici — il n'existe que pour le JOUEUR
// (`PlayerScriptableSystemRequest`, `equipmentSystem.script:6519`).
//
// Ce qui marche : **la commande d'IA native**. `AIEquipCommand` (`aiCommand.script:376`) est ce que
// le jeu s'envoie à lui-même pour armer ses propres PNJ ; elle joue l'animation de dégainage, donc
// le rattachement visuel. `AIUnequipCommand` est son symétrique. Les trois transitions — dégainer,
// ranger, changer — ont été confirmées à l'œil par Lucas le 2026-08-10. F-PLY-036.
//
// ── Les deux moitiés de ce fichier, et pourquoi elles sont séparées ───────────────────────────
//
// 1. **L'ÉMETTEUR**, un par client : il regarde ce que MOI je tiens et l'annonce au serveur.
// 2. **L'APPLICATEUR**, un par avatar : il applique ce que le SERVEUR annonce pour CET avatar.
//
// Une version intermédiaire les confondait — elle reflétait mon arme sur les avatars que je voyais,
// ce qui prouvait les transitions sans dépendre du serveur, mais montrait évidemment la mauvaise
// personne. Les séparer, c'est ce qui fait passer d'un échafaudage à la fonctionnalité : l'arme
// prend le chemin `moi → serveur → eux` au lieu de rester chez moi.
//
// ── Pourquoi un SONDAGE plutôt qu'un entonnoir d'événement ────────────────────────────────────
//
// Il faut savoir trois choses de mon arme : est-elle dégainée, laquelle, et quand ça change.
// Chercher l'entonnoir de chacune, c'est cinq hooks — dégainage, rangement, bascule
// primaire/secondaire, véhicule — et un sixième qu'on oublie. Or `AttachmentSlots.WeaponRight`
// **n'est rempli que lorsqu'une arme est en main**, et il contient laquelle : un seul slot répond
// aux trois questions. C'est le raisonnement que `SanteLocale.reds` tient déjà pour la vie — un
// sondage rate la FORME d'une variation, jamais son RÉSULTAT, et ici seul le résultat compte.
//
// ── La cadence, et pourquoi elle est passée de 0,5 s à 0,15 s ─────────────────────────────────
//
// ⚠️ MESURÉ, pas ressenti. Lucas a rapporté « un lag très important au niveau des performances
// serveur ». Le serveur n'y était pour rien : il tournait à **13 µs par tick**, et la mesure
// bout-en-bout (annonce d'un client → application chez l'autre) donnait **120 à 790 ms sur les
// quinze transitions d'une session, aucune perdue**.
//
// Tout ce délai venait de NOS deux sondages en série : jusqu'à 0,5 s pour que l'émetteur remarque
// le changement, puis jusqu'à 0,5 s pour que l'applicateur le voie arriver. Une seconde dans le
// pire cas, sans qu'un seul octet traîne sur le fil.
//
// À 0,15 s le pire cas tombe à ~0,3 s. Le coût est nul côté réseau — ces sondages ne LISENT que
// de l'état local (`GetItemInSlot`, un getter natif) et n'émettent toujours que sur CHANGEMENT.
//
// La leçon est plus large que ce fichier : « ça lague » ne désigne pas une cause. Sans la mesure,
// on serait allé optimiser un serveur qui dormait.
//
//   grep "\[Arme\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log

func TesseraJournalArme(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Arme] " + texte);
    }
}

// ════ 1. L'ÉMETTEUR — ce que JE tiens, annoncé au serveur ════════════════════════════════════

@wrapMethod(PlayerPuppet)
protected cb func OnGameAttached() -> Bool {
    let resultat = wrappedMethod();
    GameInstance.GetDelaySystem(GetGameInstance())
        .DelayCallback(TesseraRapporteurArme.Creer(0ul), 1.0, false);
    return resultat;
}

public class TesseraRapporteurArme extends DelayCallback {
    // Dernier hash ANNONCÉ au serveur. `0` = mains vides annoncées.
    let annonce: Uint64;

    public static func Creer(annonce: Uint64) -> ref<TesseraRapporteurArme> {
        let r = new TesseraRapporteurArme();
        r.annonce = annonce;
        return r;
    }

    public func Call() -> Void {
        let jeu = GetGameInstance();
        let suivant = this.annonce;
        let joueur = GameInstance.GetPlayerSystem(jeu).GetLocalPlayerControlledGameObject();
        let transactions = GameInstance.GetTransactionSystem(jeu);
        let reseau = GameInstance.GetNetworkGameSystem();
        if IsDefined(joueur) && IsDefined(transactions) && IsDefined(reseau) {
            let enMain = transactions.GetItemInSlot(joueur, t"AttachmentSlots.WeaponRight");
            let voulu: Uint64 = 0ul;
            let identifiant: TweakDBID;
            if IsDefined(enMain) {
                identifiant = ItemID.GetTDBID(enMain.GetItemID());
                voulu = TDBID.ToNumber(identifiant);
            }
            // ⚠️ ON N'ÉMET QUE SUR CHANGEMENT. Ce sondage tourne deux fois par seconde : émettre à
            // chaque tour inonderait le fil ET ferait rejouer l'animation de dégainage en boucle
            // chez tous ceux qui me regardent. C'est le filtre le plus important du fichier, et il
            // est ici plutôt que côté serveur parce que c'est ici qu'on SAIT ce qui a changé.
            if voulu != this.annonce {
                reseau.Tessera_RapporterArme(voulu, voulu != 0ul);
                if voulu == 0ul {
                    TesseraJournalArme("annonce au serveur : mains vides");
                } else {
                    TesseraJournalArme(s"annonce au serveur : \(TDBID.ToStringDEBUG(identifiant))");
                }
                suivant = voulu;
            }
        }
        // Se ré-arme TOUJOURS, sans condition — la leçon du battement de l'écran de mort
        // (2026-08-09) : un ré-armement enfermé dans un test s'arrête un jour, et plus personne ne
        // comprend pourquoi l'état s'est figé.
        GameInstance.GetDelaySystem(jeu)
            .DelayCallback(TesseraRapporteurArme.Creer(suivant), 0.15, false);
    }
}

// ════ 2. L'APPLICATEUR — ce que le SERVEUR annonce pour CET avatar ═══════════════════════════

// ⚠️ Différé de 2 s après l'attachement. Un pantin qui vient de naître n'a fini ni son inventaire
// ni ses slots d'attache : les ordres y sont acceptés sans effet — le « succès trompeur » que D1
// interdit de compter.
@wrapMethod(ScriptedPuppet)
protected cb func OnGameAttached() -> Bool {
    let resultat = wrappedMethod();
    if TesseraEstSousAutoriteServeur(this.GetEntityID()) {
        GameInstance.GetDelaySystem(GetGameInstance())
            .DelayCallback(TesseraArmeAvatar.Creer(this.GetEntityID(), 0u, 0u, null), 2.0, false);
    }
    return resultat;
}


// Applicateur — il ne SUPPOSE pas que l'ordre a été exécuté, il VÉRIFIE et recommence.
//
// ── Pourquoi cette réécriture ─────────────────────────────────────────────────────────────────
//
// Verdict de Lucas, 2026-08-10 : « des fois ça marche, des fois ça ne marche pas ». Et il a
// identifié la bonne cause avant moi : ce n'était pas une latence, c'était un système qui envoyait
// un ordre et **tenait sa propre comptabilité pour la vérité**.
//
// La version précédente notait « j'ai appliqué X » dès l'envoi de la commande. Si le pantin
// l'avalait — IA occupée, animation en cours, état transitoire — personne ne s'en apercevait
// jamais : notre registre disait X, l'avatar tenait Y, et plus rien ne les réconciliait. Un ordre
// perdu l'était définitivement.
//
// C'est exactement la classe de défaut qui a coûté la journée entière : « accepté » traité comme
// « exécuté » (D1). Et la parade est celle qu'on a déjà écrite ce matin pour la mort
// (`TesseraVerifieCadavre`) : ordonner, puis CONSTATER, puis recommencer si l'effet manque.
//
// ── La boucle ─────────────────────────────────────────────────────────────────────────────────
//
// Toutes les 0,15 s, on lit ce que l'avatar tient RÉELLEMENT dans son slot, et on le compare à ce
// que le serveur veut. S'ils diffèrent, on réordonne. Il n'y a plus d'état à désynchroniser : la
// seule mémoire est l'avatar lui-même.
//
// Cette boucle est auto-cicatrisante par construction. Peu importe ce qui a fait échouer un ordre —
// on ne le saura peut-être jamais — la passe suivante le refait. C'est la différence entre un
// système qui marche quand tout va bien et un système robuste.
//
// ⚠️ LE DÉLAI DE RETENTE EST LA PIÈCE DÉLICATE. Réordonner toutes les 0,15 s relancerait
// l'animation de dégainage avant qu'elle ait fini, et le pantin passerait son temps à ressortir son
// arme sans jamais la tenir — on remplacerait une panne par une pire. On laisse donc ~1 s à
// l'animation avant de conclure à l'échec.
public class TesseraArmeAvatar extends DelayCallback {
    let cible: EntityID;
    // Passes restantes avant de retenter. `0` = on peut ordonner maintenant.
    let attente: Uint32;
    // Ordres émis depuis la dernière convergence — pour que le journal dise en COMBIEN d'essais on
    // y arrive, ou qu'on n'y arrive pas. Un système qui retente en silence cache sa propre panne.
    let essais: Uint32;
    // Dernière commande d'IA envoyée, gardée pour l'ANNULER avant d'en envoyer une autre.
    //
    // ⚠️ Mesuré le 2026-08-10 : sans cette annulation, la commande précédente restait `Enqueued` et
    // bloquait toutes les suivantes — seul le premier dégainage avait un effet.
    let derniere: ref<AICommand>;

    public static func Creer(cible: EntityID, attente: Uint32, essais: Uint32,
                             derniere: ref<AICommand>) -> ref<TesseraArmeAvatar> {
        let a = new TesseraArmeAvatar();
        a.cible = cible;
        a.attente = attente;
        a.essais = essais;
        a.derniere = derniere;
        return a;
    }

    public func Call() -> Void {
        let jeu = GetGameInstance();
        let avatar = GameInstance.FindEntityByID(jeu, this.cible) as ScriptedPuppet;
        if !IsDefined(avatar) {
            // L'avatar a disparu (destreaming, déconnexion). Seule condition d'arrêt légitime.
            return;
        }
        let reseau = GameInstance.GetNetworkGameSystem();
        if IsDefined(reseau) {
            this.Converger(jeu, avatar, reseau);
        }
        // Se ré-arme TOUJOURS, en dernier et sans condition — la leçon du battement de l'écran de
        // mort (2026-08-09) : un ré-armement enfermé dans un test s'arrête un jour, et plus
        // personne ne comprend pourquoi l'état s'est figé.
        GameInstance.GetDelaySystem(jeu)
            .DelayCallback(
                TesseraArmeAvatar.Creer(this.cible, this.attente, this.essais, this.derniere),
                0.15, false);
    }

    private func Converger(jeu: GameInstance, avatar: ref<ScriptedPuppet>,
                           reseau: ref<NetworkGameSystem>) -> Void {
        let transactions = GameInstance.GetTransactionSystem(jeu);
        if !IsDefined(transactions) {
            return;
        }
        let slot = t"AttachmentSlots.WeaponRight";

        // ── CE QUE LE SERVEUR VEUT, TRADUIT POUR UN PANTIN ───────────────────────────────
        let voulue = reseau.Tessera_ArmeDeLEntite(this.cible);
        let attendue = this.PourPantin(voulue);

        // ── CE QUE L'AVATAR TIENT VRAIMENT ───────────────────────────────────────────────
        //
        // C'est LA différence avec la version précédente : la vérité est lue sur l'entité, pas dans
        // une variable à nous. Rien ne peut plus diverger en silence.
        let enMain = transactions.GetItemInSlot(avatar, slot);
        let porte: Uint64 = 0ul;
        if IsDefined(enMain) {
            porte = TDBID.ToNumber(ItemID.GetTDBID(enMain.GetItemID()));
        }

        if porte == TDBID.ToNumber(attendue) {
            // Convergé. On ne journalise que si ça a demandé des essais — une ligne par tour serait
            // du bruit, et le bruit finit par cacher les vraies pannes.
            if this.essais > 0u {
                TesseraJournalArme(s"convergé en \(this.essais) ordre(s)");
                this.essais = 0u;
            }
            this.attente = 0u;
            return;
        }

        // Divergence. On laisse à l'ordre précédent le temps de produire son effet avant d'en
        // renvoyer un — sinon on relance l'animation avant qu'elle ait fini.
        if this.attente > 0u {
            this.attente -= 1u;
            return;
        }

        let controleur = avatar.GetAIControllerComponent();
        if !IsDefined(controleur) {
            return;
        }

        // ⚠️ Annuler l'ordre précédent : une commande d'IA reste ACTIVE tant qu'elle n'a pas
        // conclu, et en empiler une seconde ne la remplace pas.
        if IsDefined(this.derniere) {
            if NotEquals(EnumInt(controleur.GetCommandState(this.derniere)),
                         EnumInt(AICommandState.Success)) {
                controleur.CancelCommand(this.derniere);
            }
            this.derniere = null;
        }

        this.essais += 1u;
        // ⚠️ Un plafond BRUYANT plutôt qu'une boucle infinie muette. Au-delà, on continue d'essayer
        // (l'état peut redevenir applicable) mais on le DIT, sinon un avatar définitivement désarmé
        // ressemblerait à un avatar simplement désarmé.
        if this.essais == 10u {
            TesseraJournalArme(
                s"⚠ l'avatar refuse l'arme depuis 10 ordres — porte=\(porte) attendu=\(TDBID.ToNumber(attendue))");
        }
        // ~1 s à 0,15 s par passe : le temps qu'une animation de dégainage se joue.
        this.attente = 7u;

        if TDBID.ToNumber(attendue) == 0ul {
            // ── RANGEMENT ────────────────────────────────────────────────────────────────
            // `AIUnequipCommand` et non un retrait de slot : c'est la commande native, donc elle
            // joue l'animation de rangement au lieu de faire disparaître l'arme d'un coup.
            let rangement = new AIUnequipCommand();
            rangement.slotId = slot;
            controleur.SendCommand(rangement);
            controleur.ForceTickNextFrame();
            this.derniere = rangement;
            TesseraJournalArme(s"ordre : rangement (essai \(this.essais))");
            return;
        }

        // ── DÉGAINAGE OU CHANGEMENT ──────────────────────────────────────────────────────
        //
        // ⚠️ On donne l'item AVANT de l'équiper, et par le MÊME identifiant que celui demandé.
        // Mesuré le 2026-07-24 : un item donné par une autre voie reçoit un `ItemID` dynamique
        // différent, et l'équipement échoue ensuite sur un item que le pantin possède pourtant.
        let identifiant = ItemID.FromTDBID(attendue);
        if !transactions.HasItem(avatar, identifiant) {
            transactions.GiveItem(avatar, identifiant, 1);
        }
        let equipement = new AIEquipCommand();
        equipement.slotId = slot;
        equipement.itemId = attendue;
        equipement.failIfItemNotFound = false;
        controleur.SendCommand(equipement);
        // ⚠️ RÉVEILLER L'IA, SINON L'ORDRE DORT. Mesuré le 2026-08-10 : l'ordre arrive chez
        // l'observateur en 128-314 ms (fil et serveur hors de cause, shard à 117 µs par tick), et
        // pourtant l'arme mettait « des dizaines de secondes » à apparaître. Une commande envoyée à
        // un pantin de foule reste `Enqueued` jusqu'à ce que son IA soit tickée — et un figurant
        // inactif l'est rarement. Même appel que la téléportation d'avatar (`NetworkGameSystem.reds`).
        controleur.ForceTickNextFrame();
        this.derniere = equipement;
        TesseraJournalArme(
            s"ordre : \(TDBID.ToStringDEBUG(attendue)) (essai \(this.essais), porte=\(porte))");
    }

    // Traduit l'arme du JOUEUR en son équivalent PANTIN quand les deux diffèrent.
    //
    // ⚠️ Le jeu a deux familles d'items de poings, non interchangeables : `Items.Player_Fists` /
    // `Items.StrongArms` pour le joueur, `Items.Npc_fists_ma` / `Items.NPC_Strong_Arms` pour les
    // PNJ. Donner celui du joueur à un pantin : accepté, sans posture.
    //
    // Substitution par le TYPE et non par une liste de noms — ça couvre les variantes de qualité et
    // survit à une mise à jour du jeu. ⚠️ DEUX types, pas un : le premier essai ne testait que
    // `Wea_Fists` et le journal a répondu **zéro substitution** sur une session entière, parce que
    // les bras renforcés portent `Cyb_StrongArms` (`strong_arms.tweak:141`), un type DISTINCT.
    private func PourPantin(voulue: TweakDBID) -> TweakDBID {
        if TDBID.ToNumber(voulue) == 0ul {
            return voulue;
        }
        let fiche = TweakDBInterface.GetItemRecord(voulue);
        if !IsDefined(fiche) {
            return voulue;
        }
        let genre = fiche.ItemType().Type();
        if Equals(genre, gamedataItemType.Wea_Fists) {
            return t"Items.Npc_fists_ma";
        }
        if Equals(genre, gamedataItemType.Cyb_StrongArms) {
            return t"Items.NPC_Strong_Arms";
        }
        return voulue;
    }
}

module Cyberverse.Network.Managers

// SONDE — peut-on mettre une arme dans les mains d'un avatar de joueur distant ?
//
// ── Ce qu'on cherche, et pourquoi une sonde avant du câblage ──────────────────────────────────
//
// Lucas veut voir l'arme que l'autre joueur a en main. Le protocole porte DÉJÀ le vocabulaire pour
// ça — `EquippedItem { item, slot, drawn }` et `EquipmentReport` sont dans `protocol.fbs` — mais
// rien ne l'alimente ni ne le consomme : le serveur pose `garments: None` explicitement
// (`gateway_routing.rs:329`), et aucun code C++ n'émet ni n'applique quoi que ce soit.
//
// Le câblage complet, c'est trois étages (émettre, tenir côté serveur, appliquer) dont DEUX sont du
// plombier — le patron existe déjà pour l'apparence. Le troisième est le seul risque réel : **est-ce
// qu'un pantin de foule accepte de tenir une arme et de la MONTRER ?** Écrire les trois étages avant
// de le savoir, c'est risquer de jeter les trois.
//
// ── L'échec précédent, et ce qu'il enseigne ───────────────────────────────────────────────────
//
// Une première tentative (2026-08-09) donnait `Items.Preset_Nue_Copperhead` au pantin. Elle a échoué,
// et l'échec ressemblait à « on ne peut pas armer un pantin ». En réalité **cet identifiant n'existe
// pas** : un `t"..."` inventé est un hachage, pas une référence — il ne lève aucune erreur et se
// résout en silence sur un enregistrement vide (F-PNJ-145).
//
// D'où le choix central de cette sonde : **on n'écrit AUCUN nom d'item.** On lit l'arme réellement
// portée par le joueur local, dans son slot, et on donne CELLE-LÀ. L'identifiant vient du jeu, donc
// il existe par construction. C'est la seule façon d'obtenir un « ça ne marche pas » qui veuille
// dire quelque chose.
//
// ── La leçon déjà mesurée qu'on réutilise (2026-07-24, sonde `npc_equip_weapon`) ───────────────
//
// `GiveItem(itemID)` et **jamais** `GiveItemByTDBID` : un item donné par TDBID reçoit un `ItemID`
// DYNAMIQUE, différent du `ItemID.FromTDBID` nu — et `AddItemToSlot` échoue alors sur un item que le
// pantin possède pourtant. On donne donc exactement l'`ItemID` qu'on posera dans le slot.
//
// ── Ce que la sonde ne prouvera PAS ───────────────────────────────────────────────────────────
//
// `AddItemToSlot` qui renvoie `true` ne prouve rien — « accepté » n'est pas « affiché » (D1). C'est
// pour ça qu'on relit `IsSlotEmpty` après, et surtout que le verdict final appartient à **l'œil de
// Lucas** : le pantin tient-il visiblement l'arme ? Un slot rempli avec un pantin aux mains vides
// est un résultat parfaitement possible, et ce serait une information, pas un échec de la sonde.
//
// ⚠️ TEMPORAIRE. Cette sonde arme TOUS les avatars réseau avec MON arme, ce qui n'a évidemment rien
// à voir avec la fonctionnalité finale (où chacun porte la sienne, décidée par le serveur). Elle
// mesure la FAISABILITÉ de l'affichage, rien d'autre, et se retire dès que le verdict est tombé.
//
//   grep "\[Arme\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log

func TesseraJournalArme(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Arme] " + texte);
    }
}

// ⚠️ Différé de 2 s après l'attachement, et ce n'est pas de la prudence décorative. Un pantin qui
// vient de naître n'a pas fini de monter son inventaire ni ses slots d'attache : `AddItemToSlot` y
// est accepté sans effet — exactement le « succès trompeur » que D1 interdit de compter. Le projet
// a déjà payé ce délai ailleurs (la sonde de santé s'arme à 0,5 s pour la même raison).
@wrapMethod(ScriptedPuppet)
protected cb func OnGameAttached() -> Bool {
    let resultat = wrappedMethod();
    if TesseraEstSousAutoriteServeur(this.GetEntityID()) {
        GameInstance.GetDelaySystem(GetGameInstance())
            .DelayCallback(TesseraSondeArmeAvatar.Creer(this.GetEntityID(), 1u), 2.0, false);
    }
    return resultat;
}

public class TesseraSondeArmeAvatar extends DelayCallback {
    let cible: EntityID;
    let essai: Uint32;

    public static func Creer(cible: EntityID, essai: Uint32) -> ref<TesseraSondeArmeAvatar> {
        let s = new TesseraSondeArmeAvatar();
        s.cible = cible;
        s.essai = essai;
        return s;
    }

    public func Call() -> Void {
        let jeu = GetGameInstance();
        let avatar = GameInstance.FindEntityByID(jeu, this.cible) as ScriptedPuppet;
        if !IsDefined(avatar) {
            return;
        }
        let joueur = GameInstance.GetPlayerSystem(jeu).GetLocalPlayerControlledGameObject();
        if !IsDefined(joueur) {
            TesseraJournalArme("joueur local introuvable — sonde annulee");
            return;
        }
        let transactions = GameInstance.GetTransactionSystem(jeu);
        if !IsDefined(transactions) {
            TesseraJournalArme("TransactionSystem indisponible");
            return;
        }

        // ── 1. LIRE UNE ARME QUI EXISTE VRAIMENT ───────────────────────────────────────────
        //
        // Le slot du joueur local, pas un nom écrit de mémoire. Si le joueur n'a rien en main, on
        // le DIT au lieu de continuer avec un identifiant nul : une sonde qui échoue doit dire
        // POURQUOI, sinon son silence se lit comme « la fonctionnalité est impossible ».
        let slot = t"AttachmentSlots.WeaponRight";
        let armeDuJoueur = transactions.GetItemInSlot(joueur, slot);
        if !IsDefined(armeDuJoueur) {
            // ⚠️ FENÊTRE DE 30 s PLUTÔT QU'UN INSTANT — et c'est ce qui rend la sonde utilisable.
            //
            // Le slot `WeaponRight` n'est rempli que quand une arme est DÉGAINÉE. Lire une seule
            // fois, 2 s après l'apparition de l'avatar, exigerait que Lucas ait déjà son arme en
            // main à cette seconde précise : un cycle complet (deux relances + un test) perdu sur
            // une question de timing, pas sur la question posée. Ce dépôt en a déjà brûlé plusieurs
            // aujourd'hui pour de bonnes idées mal minutées.
            //
            // On réessaie donc toutes les 3 s pendant 30 s : il lui suffit de dégainer quand il
            // veut dans cette fenêtre. Et on ne journalise que le premier et le dernier essai —
            // dix lignes identiques n'apprennent rien et noient le reste.
            if this.essai == 1u {
                TesseraJournalArme(
                    "aucune arme dans WeaponRight du joueur local — DEGAINE dans les 30 s "
                    + "(la sonde reessaie toutes les 3 s)");
            }
            if this.essai >= 10u {
                TesseraJournalArme(
                    "abandon apres 30 s sans arme degainee — RIEN N'A ETE MESURE "
                    + "(ce n'est pas un verdict sur la faisabilite)");
                return;
            }
            GameInstance.GetDelaySystem(jeu)
                .DelayCallback(TesseraSondeArmeAvatar.Creer(this.cible, this.essai + 1u), 3.0, false);
            return;
        }
        let identifiant = armeDuJoueur.GetItemID();
        TesseraJournalArme(s"arme lue sur le joueur local : \(TDBID.ToStringDEBUG(ItemID.GetTDBID(identifiant)))");

        // ── 2. LA DONNER, PUIS LA POSER DANS LE SLOT ───────────────────────────────────────
        //
        // ⚠️ `GiveItem(identifiant)` — le MÊME `ItemID` que celui qu'on posera. Mesuré le
        // 2026-07-24 : passer par `GiveItemByTDBID` produit un `ItemID` dynamique différent, et
        // `AddItemToSlot` échoue ensuite sur un item que le pantin possède pourtant.
        let possede = transactions.HasItem(avatar, identifiant);
        if !possede {
            let donne = transactions.GiveItem(avatar, identifiant, 1);
            TesseraJournalArme(s"GiveItem = \(donne)");
        }

        let videAvant = transactions.IsSlotEmpty(avatar, slot);
        let pose = transactions.AddItemToSlot(avatar, slot, identifiant);
        let videApres = transactions.IsSlotEmpty(avatar, slot);

        // ⚠️ `pose` est ce que le moteur a ACCEPTÉ, `videApres` ce qu'il a FAIT. Les deux sont
        // journalisés séparément parce qu'ils peuvent diverger — et c'est précisément cette
        // divergence qui a coûté des heures ailleurs dans ce dépôt.
        TesseraJournalArme(
            s"AddItemToSlot = \(pose) · slot vide avant = \(videAvant) · apres = \(videApres)"
            + " (false attendu apres)");
        TesseraJournalArme(
            ">>> VERDICT A L'OEIL DE LUCAS : l'avatar TIENT-il l'arme en main ? "
            + "Un slot rempli avec des mains vides est un resultat possible, et c'est une info.");
    }
}

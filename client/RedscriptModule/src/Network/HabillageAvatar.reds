module Cyberverse.Network.Managers

// L'AVATAR DISTANT PORTE LES VÊTEMENTS DE L'INVENTAIRE DE SON JOUEUR.
//
// ⭐ Demande de Lucas, 2026-08-24 : *« il faut que ce soit les mêmes »*. Le serveur décide déjà ce
// qui est porté (`contenus.porte`) et l'envoie ; ce fichier est le dernier mètre.
//
// ── ⚠️ POURQUOI ON ALLUME, ET POURQUOI ON N'ÉQUIPE PAS ──────────────────────────────────────
//
// Équiper un pantin distant est une impasse **mesurée deux fois, par deux mécanismes distincts** :
// item réel (F-PLY-275) et item de prévisualisation façon `EquipmentEx` (F-PLY-304). Dans les deux
// cas `AddItemToSlot` rend `true`, le slot passe en « naissance d'entité en cours », et **il y
// reste indéfiniment** — neuf passes, `GetItemInSlot` toujours `null`. Le wiki de modding dit la
// même chose dans l'autre sens : sur un PNJ, les vêtements sont cuits dans l'apparence, **à la
// construction** (F-PLY-305), ce qui est exactement la loi F-PLY-191.
//
// Ce que la mesure autorise, c'est d'**éteindre et rallumer un composant déjà monté** — `Toggle`,
// vérifié en capture (F-PLY-306). D'où l'architecture, décidée le 2026-08-24 :
//
//     tous les vêtements du catalogue serveur sont CUITS dans l'entité, `isEnabled = 0` ;
//     ici, on ALLUME ceux que le serveur déclare portés.
//
// La cuisson est faite hors jeu par `tools/re-probe/entites/cuire-garde-robe.py`.
//
// ── ⭐ LA CONVENTION QUI ÉVITE TOUT FICHIER DE CORRESPONDANCE ────────────────────────────────
//
//     nom du composant  =  <appearanceName de TweakDB> + <m|w>
//     exemple           =  t1_tshirt_04_old_02_m
//
// Le client reçoit un `TweakDBID` d'item ; `appearanceName` est un flat de ce record, lisible au
// script. Il n'y a donc **aucune table à embarquer** — donc rien qui puisse diverger en silence de
// l'entité livrée. Et le préfixe (`t1_`, `t2_`, `l1_`, `s1_`) est conservé, ce que le moteur exige
// pour reconnaître un vêtement (« Garment Support »).
//
// ⚠️ Un vêtement peut avoir PLUSIEURS meshes (une veste en a deux) : ils sont suffixés `__1`,
// `__2`. On allume donc tout ce qui **commence par** la clé — jamais ce qui lui est égal.
//
// ── ⚠️ LES SOUS-VÊTEMENTS SUIVENT LE RÉGLAGE DE NUDITÉ ──────────────────────────────────────
//
// Consigne de Lucas, 2026-08-24 : *« il y a un réglage sur le jeu pour la nudité et j'aimerais que
// ça respecte ce réglage »*. Le jeu l'expose : `IsNudityAllowed()` sur le système de customisation.
// Les sous-vêtements viennent de l'apparence de base (ils sont déjà montés et allumés) ; la règle
// est donc de les ÉTEINDRE uniquement si la nudité est permise ET que le joueur n'en porte pas.
//
//   grep "\[Habillage\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log

func TesseraJournalHabillage(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Habillage] " + texte);
    }
}

// Le corps derrière un `EntityID`, quelle que soit la voie qui l'a fait naître.
func TesseraCorpsDeLEntite(cible: EntityID) -> ref<Entity> {
    let entite = GameInstance.GetDynamicEntitySystem().GetEntity(cible);
    if !IsDefined(entite) {
        entite = GameInstance.FindEntityByID(GetGameInstance(), cible);
    }
    return entite;
}

// La clé de composant d'un item, pour une morphologie donnée.
//
// ⚠️ Rend `""` quand l'item n'a pas de `appearanceName` — ce qui arrive (nourriture, argent,
// cyberware). Un item sans apparence n'est pas une erreur : il n'a simplement rien à montrer.
func TesseraCleVetement(record: TweakDBID, corpsMasculin: Bool) -> String {
    let app = TesseraApparenceNue(record);
    if Equals(app, "") {
        return "";
    }
    return app + (corpsMasculin ? "m" : "w");
}

// Le `appearanceName` d'un item, SANS la lettre de morphologie.
//
// ⭐ POURQUOI IL FAUT LES DEUX FORMES. Un composant cuit se nomme `<apparence><m|w>`, parce qu'un
// vêtement est genré à la source. Un EFFET, lui, ne l'est pas : `eye_glow_blue` est déclaré une
// seule fois sur l'entité, pour les deux morphologies. Suffixer son nom le rendrait introuvable.
func TesseraApparenceNue(record: TweakDBID) -> String {
    let app = TweakDBInterface.GetCName(record + t".appearanceName", n"");
    if Equals(app, n"") {
        return "";
    }
    return NameToString(app);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// LES EFFETS PILOTÉS PAR LE SERVEUR — un état visuel qui n'est PAS un mesh
// ═══════════════════════════════════════════════════════════════════════════════════════════════
//
// Demande de Lucas, 2026-09-05 : *« quand quelqu'un reçoit un appel, il a les yeux qui passent
// bleu »*.
//
// ⛔ LA RECETTE DES VÊTEMENTS NE S'APPLIQUE PAS ICI, et c'est structurel. Le globe oculaire n'est
// pas un composant de notre entité d'avatar : il vient de la charge d'esthétique, assemblé à la
// naissance. Il n'y a donc rien à cuire, et rien à allumer.
//
// ⭐⭐ MAIS L'EFFET EST DÉJÀ LÀ. L'entité d'avatar descend du pantin photomode de V, et porte à ce
// titre **177 descripteurs d'effets** — dont `eye_glow_blue`, `eye_glow_gold`, `eye_glow_purple`,
// `eye_glow_red` et `eye_flare` (relevé du 2026-09-06 sur `avatar_distant_wa.ent`). Un effet
// déclaré se joue par son nom, et c'est tout ce qu'il faut.
//
// ⚠️ CETTE LISTE EST FERMÉE, ET C'EST DÉLIBÉRÉ. On pourrait « tenter » de jouer n'importe quelle
// clé orpheline comme un effet : l'appel ne planterait pas, il ne ferait rien. Mais on perdrait le
// seul instrument qui dit qu'un visuel manque — le journal `NON CUIT`, qui a nommé lui-même les
// trois composants à cuire le 2026-09-05. Une clé inconnue doit rester une clé inconnue.
//
// Les 145 noms déclarés sur l'entité (`status_*`, `trail_*`, `strong_arms_block`…) sont autant
// d'états visuels disponibles sans une ligne de cuisson. On les ouvre un par un, en les nommant.
func TesseraEffetsPilotes() -> array<CName> {
    return [n"eye_glow_blue", n"eye_glow_gold", n"eye_glow_purple", n"eye_glow_red"];
}

// Vrai si ce composant est une pièce de GARDE-ROBE — donc à nous — et faux s'il appartient au CORPS.
//
// ⚠️ CETTE FONCTION DÉCIDE CE QU'ON A LE DROIT D'ÉTEINDRE. Se tromper ne dégrade pas : ça mutile
// l'avatar (visage manquant, manches sur du vide — vécu le 2026-08-26 avec les wrappers d'arme).
// Elle n'est donc pas devinée d'après la nomenclature CDPR : elle est tirée du relevé des 54
// composants d'un avatar vivant (`avatar_corps`, 2026-09-03).
//
// Ce relevé donne un discriminant net, et un seul :
//
//     VÊTEMENTS   t1_tshirt_04_old_02_w · t2_coat_04_old_01_w__1 · l1_pants_06_old_03_w
//                 s1_boots_05_old_02_w · t2_jacket_19_basic_02_w__3   → finissent par `_m`/`_w`
//     CORPS       t0_000_pma_base__full_shadow · l0_000_pwa_base__cs_flat · hh_040_wa__pixie_bob
//                 heb_000_pwa__basehead · i0_000_pwa_base__genitals_none · a0_000_pwa_base_nails_l
//                 t1_057_pwa_tank__bra0730 · l1_047_pwa_shorts__panties7731  → AUCUN ne finit ainsi
//
// ⭐ Le suffixe `m`/`w` n'est pas décoratif : c'est celui que `TesseraCleVetement` AJOUTE lui-même
// à l'`appearanceName`. On reconnaît donc exactement ce qu'on sait fabriquer — pas une famille de
// noms qu'on espère.
//
// ⚠️ Un même vêtement monte PLUSIEURS meshes, suffixés `__1`, `__2`… — d'où les deux formes
// contenant. Les sous-vêtements de l'apparence de base (`__bra`, `__panties`, `__boxers`) sont
// exclus par construction, et c'est voulu : ils ont leur propre règle, plus bas, adossée au
// réglage de nudité.
func TesseraEstGardeRobe(nom: String) -> Bool {
    return StrEndsWith(nom, "_m") || StrEndsWith(nom, "_w")
        || StrContains(nom, "_m__") || StrContains(nom, "_w__")
        || TesseraEstCyberwareCuit(nom);
}

// ⛔⛔ LE CYBERWARE S'ALLUMAIT ET NE S'ETEIGNAIT JAMAIS — mesuré le 2026-09-06.
//
// Symptôme : `porte` passé à `false` côté serveur, la clé disparaît bien de la liste demandée…
// et le journal dit `2 vetement(s) demande(s), 0 allume(s), **0 retire(s)**`. Les avant-bras
// gorille restaient montés indéfiniment. Verdict de Lucas : « je n'ai pas pu voir de changement
// physique majeur ».
//
// ⭐ LA CAUSE TIENT DANS UN UNDERSCORE. La règle ci-dessus reconnaît une pièce à nous par son
// suffixe de morphologie — `_m` / `_w`. Elle marche pour les vêtements parce que leur
// `appearanceName` CDPR finit déjà par un underscore (`t1_tshirt_04_old_02_` + `w`). Le
// cyberware, lui, n'en a pas : `holstered_strong` + `w` donne **`holstered_strongw`**, qui ne
// finit pas par `_w`. Le composant n'était donc reconnu comme nôtre par personne, et la branche
// de retrait ne le voyait pas.
//
// ⚠️ C'est le MEME défaut que celui du 2026-09-03 — « la passe était ADDITIVE SEULE » — mais
// déplacé d'un cran : la branche de retrait existe désormais, c'est son GARDE qui aveuglait.
// Un correctif qui ajoute une branche doit vérifier que sa condition d'entrée couvre tous les
// cas qu'il prétend traiter, sinon il corrige la moitié du problème et ferme le dossier.
//
// ⚠️⚠️ ET `stage_1_` PASSAIT PAR ACCIDENT : il finit par `_` , donc `stage_1_w` finit par `_w`.
// Une famille sur neuf marchait, pour une raison qui n'a rien à voir avec le mécanisme. C'est
// exactement le genre de coïncidence qui fait conclure « ça marche » sur un échantillon.
//
// ⭐ LA LISTE EST FERMEE, ET C'EST DELIBERE — même raison que `TesseraEffetsPilotes`. On
// n'éteint que ce qu'on sait avoir posé soi-même. Elle doit suivre les groupes de
// `tools/re-probe/entites/cuire-cyberware.py` ; les trois tatouages n'y figurent pas parce que
// leur nom finit déjà par `_m`/`_w` et que la règle générale les couvre.
//
// ⚠️ `StrBeginsWith`, jamais `StrContains` : le CORPS porte `a0_001__personal_link_tpp4537` et
// `a0_001__personal_link_default_holstered`, qui contiennent nos mots-clés sans être à nous.
// Les éteindre arracherait le connecteur de poignet du joueur.
// Vrai si ce composant est la PEAU d'un bras du corps — celle qu'un bras gorille recouvre.
//
// ⛔ MA PREMIÈRE VERSION ÉTAIT FAUSSE et trouvait exactement ZÉRO peau sur un corps féminin, ce
// qui se lisait comme « rien à faire ». Elle exigeait `a0_000_` puis `_base__`, noms tirés d'un
// `grep` qui recollait le nom du composant à son `meshAppearance` : dans
// `a0_000_pwa_base__03_ca_senna`, seul `a0_…_base` est le composant, `03_ca_senna` est la
// carnation qui suit. Un artefact d'outil lu comme une mesure.
//
// ⭐ LES VRAIS NOMS, relevés sur deux avatars vivants simultanés le 2026-09-06 :
//
//     PEAU     a0_001_pwa_base_hq__full · a0_001_pwa_base_hq__full8640      (féminin)
//              a0_000_ma_base__full_ag_hq1491 · …hq6168                     (masculin)
//     ONGLES   a0_000_pwa_base_nails_l/_r · a0_000_pma_base__nails_l/_r
//     TATOUAGE a0_000__tattoo_yakuza_r_tattoo_yakuza_w
//     POIGNET  a0_001__personal_link_tpp4537
//
// ⚠️ Trois pièges qu'aucune nomenclature ne laisse deviner : le numéro varie (`a0_000_` ET
// `a0_001_`), le féminin n'écrit pas le même motif que le masculin (`base_hq__full` contre
// `base__full_ag_hq`), et les ongles portent `base_nails` avec UN underscore au féminin, DEUX au
// masculin. Le seul discriminant qui tient sur les quatre peaux et sur aucune autre famille est
// le mot `full`. Les exclusions nominales qui suivent sont gratuites et disent ce qu'on refuse
// d'éteindre.
func TesseraEstPeauDeBras(nom: String) -> Bool {
    return StrBeginsWith(nom, "a0_")
        && StrContains(nom, "_base")
        && StrContains(nom, "full")
        && !StrContains(nom, "nails")
        && !StrContains(nom, "tattoo")
        && !StrContains(nom, "personal_link");
}

func TesseraEstCyberwareCuit(nom: String) -> Bool {
    return StrBeginsWith(nom, "holstered_strong")
        || StrBeginsWith(nom, "holstered_mantis")
        || StrBeginsWith(nom, "holstered_nanowire")
        || StrBeginsWith(nom, "holstered_launcher")
        || StrBeginsWith(nom, "personal_link_advanced")
        || StrBeginsWith(nom, "stage_1_");
}

// Appelée par `PiloterAvatar` (C++) par créneaux bornés — le seul chemin qui atteint les corps de
// la voie enrichie (F-PLY-278). Rend `true` quand il n'y a plus rien à faire.
func TesseraHabillerLeCorps(cible: EntityID, passe: Uint32) -> Bool {
    // ⚠️ On attend quelques passes : un corps qui vient de naître n'a pas fini de monter ses
    // composants, et compter trop tôt donnerait une liste courte qu'on lirait comme une absence.
    if passe < 2u {
        return false;
    }
    let corps = TesseraCorpsDeLEntite(cible) as GameObject;
    if !IsDefined(corps) {
        return false;
    }
    let reseau = GameInstance.GetNetworkGameSystem();
    if !IsDefined(reseau) {
        return false;
    }

    let combien = reseau.Tessera_NombreDeVetements(cible);
    let masculin = reseau.Tessera_AvatarCorpsMasculin(cible);

    // Les clés à allumer, calculées une fois.
    //
    // ⭐ CETTE LISTE EST LA SEULE VÉRITÉ. Elle vient du SERVEUR, et l'avatar distant n'affichera
    // rien d'autre : ce qui n'y est pas est éteint plus bas, quelle qu'en soit l'origine. Un joueur
    // qui se moderait des vêtements les verrait chez lui et **chez personne d'autre** — c'est la
    // règle posée par Lucas le 2026-09-03, et c'est ce qui empêche un client de décider seul de ce
    // que les autres voient.
    let cles: array<String>;
    // Parallèle à `cles` : cette clé a-t-elle trouvé au moins un composant ? Une clé orpheline
    // désigne un vêtement que le serveur connaît mais que notre entité ne sait pas montrer.
    let trouvees: array<Bool>;
    // Parallèle à `cles` : le nom d'apparence NU, sans la lettre de morphologie. C'est lui que le
    // vocabulaire d'effets emploie — voir `TesseraEffetsPilotes`.
    let apparences: array<String>;
    let i = 0;
    while i < combien {
        let record = reseau.Tessera_VetementDeLEntite(cible, i);
        let cle = TesseraCleVetement(record, masculin);
        if NotEquals(cle, "") {
            ArrayPush(cles, cle);
            ArrayPush(apparences, TesseraApparenceNue(record));
            ArrayPush(trouvees, false);
        }
        i += 1;
    }

    // ── ⭐⭐⭐ LE CYBERWARE QUI REMPLACE L'AVANT-BRAS, DÉCIDÉ ICI ET PAS AILLEURS ───────────
    //
    // ⛔ POURQUOI CETTE RÈGLE A DÉMÉNAGÉ. Elle vivait dans la boucle de purge, cadencée à 2 s,
    // pendant que l'habillage est cadencé par le C++. Deux horloges, donc un décalage — mesuré le
    // 2026-09-06 : les meshes gorille s'éteignent à T+2,2 s, la peau du bras ne revient qu'à
    // T+3,7 s. **Pendant 1,5 s le personnage n'a NI bras gorille NI peau de bras.** Verdict de
    // Lucas : « on a vu l'allumage et l'extinction de ses anciens et l'allumage de ses nouveaux ».
    // Ce n'était pas une impression : c'était deux horloges.
    //
    // ⭐ Et le déménagement corrige plus que le décalage. Ici on lit l'INTENTION DU SERVEUR — les
    // clés demandées — au lieu de déduire l'état depuis les composants allumés. C'est une source
    // plus directe : elle est juste dès la première passe, même avant que le mesh soit monté.
    let remplaceAvantBras = false;
    let ka = 0;
    while ka < ArraySize(cles) {
        // ⚠️ UNE SEULE FAMILLE SUR QUATRE, et c'est délibéré. Mantis, monofil et lance-projectiles
        // se replient DANS l'avant-bras en vanilla : la peau reste visible autour d'eux. Les
        // masquer donnerait des bras manquants — on échangerait un défaut contre un pire.
        // Non mesuré pour ces trois-là, et un non mesuré ne s'applique pas.
        if StrBeginsWith(cles[ka], "holstered_strong") {
            remplaceAvantBras = true;
        }
        ka += 1;
    }

    // ── LA POSE ────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ On parcourt les composants UNE fois et on décide pour chacun, plutôt que de chercher
    // chaque clé dans la liste : le coût est linéaire au lieu de quadratique, et à des dizaines de
    // joueurs autour, ce parcours tourne pour chaque avatar à chaque créneau.
    let composants = corps.GetComponents();
    let allumes = 0;
    let retires = 0;
    let brasBascules = 0;
    let brasNommes = "";
    let peauVue = 0;
    // ⚠️ Vrai dès qu'UNE peau de bras a été allumée : les suivantes seront éteintes. Voir le
    // commentaire dans la boucle — le natif en monte deux, et deux, ça fait quatre bras.
    let peauDejaAllumee = false;
    let j = 0;
    while j < ArraySize(composants) {
        let nom = NameToString(composants[j].GetName());

        // La peau du bras suit le cyberware, DANS LA MÊME PASSE que lui.
        //
        // ⚠️ C'est une PROJECTION, pas une bascule : retirer le cyberware rend le membre. Sans
        // cette symétrie on ne saurait que masquer, jamais revenir — le défaut du 2026-09-03,
        // transposé.
        if TesseraEstPeauDeBras(nom) {
            peauVue += 1;
            // ── ⛔ UNE SEULE PEAU DE BRAS ALLUMÉE, JAMAIS DEUX ──────────────────────────────
            //
            // Défaut rapporté par Lucas le 2026-09-06 : « LUCAS1 a un bug, il a deux paires de
            // bras — il a été créé comme ça ». Mesuré : le spawner natif monte DEUX meshes de
            // peau de bras, au même nom de base et à suffixe numérique différent —
            // `a0_000_ma_base__full_ag_hq1491` et `…hq6168` au masculin,
            // `a0_001_pwa_base_hq__full` et `…full8640` au féminin.
            //
            // ⚠️ `PurgerDoublons` ne peut PAS les voir : il compare des noms IDENTIQUES, et
            // ceux-là diffèrent par leur suffixe. Le défaut était invisible chez KIMY tant que
            // ses bras gorille recouvraient les deux copies — il ne se voyait que sur un
            // personnage SANS cyberware de bras.
            //
            // ⭐ On applique donc le patron déjà en place pour les jambes (`TesseraJambes`) :
            // une seule variante allumée, toutes les autres éteintes. La première rencontrée
            // fait l'affaire — ce sont des copies du même mesh.
            let peauVoulue = !remplaceAvantBras && !peauDejaAllumee;
            if peauVoulue {
                peauDejaAllumee = true;
            }
            if !Equals(composants[j].IsEnabled(), peauVoulue) {
                composants[j].Toggle(peauVoulue);
                brasBascules += 1;
                brasNommes += (Equals(brasNommes, "") ? "" : ", ")
                            + nom + (peauVoulue ? "[on]" : "[off]");
            }
        }

        let k = 0;
        let voulu = false;
        while k < ArraySize(cles) {
            if StrBeginsWith(nom, cles[k]) {
                voulu = true;
                trouvees[k] = true;
            }
            k += 1;
        }
        // On ne touche QU'aux composants de garde-robe : tout ce qui ne commence pas par un
        // préfixe de vêtement appartient au corps, et l'éteindre le mutilerait.
        if voulu && !composants[j].IsEnabled() {
            composants[j].Toggle(true);
            allumes += 1;
        } else {
            // ── ⭐⭐⭐ LE RETRAIT, QUI MANQUAIT — cette passe était ADDITIVE SEULE ─────────
            //
            // ⚠️ MESURÉ le 2026-09-03, et ça explique le symptôme d'origine. Le fil du hot-swap
            // marche de bout en bout — retirer un vêtement chez A arrive chez B, et cette
            // fonction est bien rappelée (4 → 3 vêtements demandés, lu au journal). Et pourtant
            // le vêtement RESTAIT à l'écran, parce qu'aucune ligne de ce fichier ne l'éteignait.
            // On ne pouvait que s'habiller, jamais se déshabiller : la tenue ne faisait que
            // s'empiler, ce que Lucas décrivait dès le premier jour.
            //
            // ⭐ C'est le cran d'après « accepté ≠ exécuté » : ici TOUT le fil était bien exécuté,
            // et le dernier consommateur n'avait simplement pas la branche. Un compteur posé sur
            // l'émetteur aurait confirmé la panne comme il aurait confirmé le bon fonctionnement.
            if !voulu && composants[j].IsEnabled() && TesseraEstGardeRobe(nom) {
                composants[j].Toggle(false);
                retires += 1;
            }
        }
        j += 1;
    }

    // ── LES SOUS-VÊTEMENTS SUIVENT LE RÉGLAGE DE NUDITÉ ────────────────────────────────────
    //
    // Consigne de Lucas, 2026-08-24 : *« les sous-vêtements doivent être activés (…) et il y a un
    // réglage sur le jeu pour la nudité, j'aimerais que ça respecte ce réglage »*.
    //
    // Les sous-vêtements viennent de l'apparence de base : ils sont **déjà montés et allumés**.
    // La règle n'a donc qu'un seul cas à traiter — les ÉTEINDRE — et il est doublement gardé :
    //
    //     on éteint  ⟺  la nudité est permise  ET  aucun vêtement ne couvre cette zone
    //
    // ⚠️ LE DÉFAUT EST « HABILLÉ », ET C'EST VOULU. Si le système de customisation est injoignable,
    // si le réglage ne se lit pas, si la liste des vêtements est vide — dans tous ces cas on ne
    // touche à rien. Une panne ne doit jamais pouvoir déshabiller quelqu'un devant les autres.
    let systeme = GameInstance.GetCharacterCustomizationSystem(GetGameInstance());
    if IsDefined(systeme) && systeme.IsNudityAllowed() {
        // Une zone est couverte si un vêtement demandé porte son préfixe. Le préfixe est celui de
        // la nomenclature CDPR : `t` pour le torse, `l` pour les jambes.
        let torseCouvert = false;
        let jambesCouvertes = false;
        let z = 0;
        while z < ArraySize(cles) {
            if StrBeginsWith(cles[z], "t") { torseCouvert = true; }
            if StrBeginsWith(cles[z], "l") { jambesCouvertes = true; }
            z += 1;
        }
        let eteints = 0;
        let c = 0;
        while c < ArraySize(composants) {
            let nom = NameToString(composants[c].GetName());
            // ⚠️ On ne vise QUE les sous-vêtements de l'apparence de base, nommés par CDPR :
            // `__bra`, `__panties`, `__boxers`. Jamais un préfixe seul — `t1_` désigne aussi les
            // t-shirts, et on éteindrait la tenue qu'on vient d'allumer.
            let estSousVetement = StrContains(nom, "__bra") || StrContains(nom, "__panties")
                || StrContains(nom, "__boxers");
            let zoneLibre = (StrBeginsWith(nom, "t") && !torseCouvert)
                || (StrBeginsWith(nom, "l") && !jambesCouvertes);
            if estSousVetement && zoneLibre && composants[c].IsEnabled() {
                composants[c].Toggle(false);
                eteints += 1;
            }
            c += 1;
        }
        if eteints > 0 {
            TesseraJournalHabillage(s"  nudite permise : \(eteints) sous-vetement(s) eteint(s)");
        }
    }

    // ── ⛔ LA SONDE DE WRAPPERS D ARME A ETE RETIREE — elle etait FAUSSE ET NUISIBLE ────────
    //
    // Elle poussait `Wea_Fists` et `WeaponRight` a 1.0 pour voir si l avatar prenait une garde.
    //
    // 1. **Elle ne marchait pas.** Verdict de Lucas, 2026-08-25 : « ca ne veut pas se mettre en
    //    garde ». F-PLY-311 est REFUTE — j avais lu un changement de posture dans deux captures
    //    prises a trente minutes d ecart, ce qui n est pas un A/B.
    // 2. **⚠️ Et elle CASSAIT le corps.** Capture du 2026-08-26 : l avatar n a plus ni visage ni
    //    mains, les manches s arretent sur du vide. Tenir une arme change le JEU DE MESHES DE BRAS
    //    dans le rig CDPR ; forcer la couche sans arme reelle bascule vers un jeu qui n est pas la.
    //
    // ⚠️ LA LECON, au-dela de cette sonde : un poids de wrapper n est pas une ecriture inoffensive.
    // Il selectionne des ressources. Une sonde d animation doit donc etre RETIREE des son verdict
    // pris — pas laissee en place « au cas ou ».

    TesseraJournalHabillage(
        s"avatar \(EntityID.ToDebugString(cible)) : \(ArraySize(cles)) vetement(s) demande(s), \(allumes) allume(s), \(retires) retire(s) [corps \(masculin ? "M" : "F")]");

    // ── LE VERDICT DES BRAS, DANS LA MÊME LIGNE DE TEMPS QUE L'HABILLAGE ────────────────────
    if brasBascules > 0 {
        // ⚠️ LE VERBE EST « BASCULÉ », PAS « ÉTEINT » NI « RALLUMÉ », et c'est une correction.
        // La première rédaction déduisait le verbe de `remplaceAvantBras` — donc elle écrivait
        // « 1 composant rallumé — …hq6168[off] », qui se contredit dans la même ligne : sans
        // cyberware on RALLUME la première peau et on ÉTEINT la copie en trop, deux gestes
        // opposés dans la même passe. Un journal qui résume deux gestes par un seul verbe ment
        // à moitié. C'est le suffixe [on]/[off] de chaque nom qui porte la vérité.
        TesseraJournalHabillage(
            s"  bras remplaces : \(brasBascules) composant(s) de peau bascule(s) [cyberware \(remplaceAvantBras ? "OUI" : "non")] — \(brasNommes)");
    } else {
        // ── ⭐⭐⭐ LE SEUL SILENCE QU'ON REFUSE ────────────────────────────────────────────
        //
        // « 0 basculé » a deux causes opposées : rien à faire (cas nominal), ou le cyberware est
        // demandé ET la peau ne se reconnaît plus. Le second est ce qui arrivera à la prochaine
        // montée de version du jeu — ces noms viennent de la charge d'esthétique, donc de CDPR —
        // et la règle deviendrait alors **inerte en silence**, bras dupliqués à nouveau, sans une
        // ligne d'erreur.
        //
        // ⚠️ CE N'EST PAS THÉORIQUE : ma première version de `TesseraEstPeauDeBras` était fausse
        // et trouvait zéro peau sur le corps féminin. Sans ce test elle serait passée pour
        // « rien à faire ». C'est cette ligne qui l'a nommée en une lecture.
        if remplaceAvantBras && peauVue == 0 {
            TesseraJournalHabillage(
                "  ⚠ bras gorille demandes mais AUCUNE peau de bras reconnue — la regle est inerte");
        }
    }

    // ── ⚠️ LE PLAFOND DEVIENT MESURÉ, AU LIEU DE RESTER INDISCERNABLE D'UNE PANNE ──────────────
    //
    // Notre entité d'avatar ne porte que DOUZE racines de vêtement, cuites une fois pour toutes
    // (`avatar_distant_ma.ent` / `_wa.ent`). Un vêtement que le serveur connaît mais qui n'y est pas
    // ne s'affichera JAMAIS — et jusqu'ici, en silence.
    //
    // ⭐ C'est acceptable par décision : « on accepte de ne rien afficher s'il y a un élément non
    // compatible » (Lucas, 2026-09-03). Ce qui ne l'est pas, c'est de ne pas le SAVOIR. Sans cette
    // ligne, « le manteau n'apparaît pas » a deux causes indiscernables — le fil est cassé, ou le
    // vêtement n'est pas cuit — et on repart chercher la panne du côté du réseau.
    //
    // ⚠️ On journalise la CLÉ, pas l'item : c'est elle qui manque à l'entité, et c'est elle qu'il
    // faudra cuire. Le nom de l'item ne dit pas quel mesh il faudrait ajouter.
    // ── LES EFFETS, AVANT LE RELEVÉ DES ORPHELINES ─────────────────────────────────────────────
    //
    // Un effet piloté n'a pas de composant à allumer : il se joue. On le fait AVANT de compter les
    // clés orphelines, et on marque la clé comme trouvée — sinon un état visuel qui fonctionne
    // serait journalisé comme un visuel manquant, à chaque passe, chez chaque observateur.
    let effets = TesseraEffetsPilotes();
    let e = 0;
    while e < ArraySize(effets) {
        let nomEffet = NameToString(effets[e]);
        let veut = false;
        let a = 0;
        while a < ArraySize(apparences) {
            if Equals(apparences[a], nomEffet) {
                veut = true;
                trouvees[a] = true;
            }
            a += 1;
        }
        if veut {
            GameObjectEffectHelper.StartEffectEvent(corps, effets[e]);
        } else {
            // ⚠️ ARRÊT INCONDITIONNEL, et c'est ce qui rend la passe idempotente. On ne sait pas
            // si l'effet joue — aucune API ne le dit — donc on ne peut pas conditionner l'arrêt à
            // son état. Arrêter un effet qui ne joue pas est sans conséquence ; NE PAS arrêter
            // celui qui joue laisserait les yeux allumés pour toujours après la fin de l'appel.
            // C'est le pendant exact du retrait de vêtement qui manquait le 2026-09-03 : une passe
            // additive seule ne sait pas revenir en arrière.
            GameObjectEffectHelper.StopEffectEvent(corps, effets[e]);
        }
        e += 1;
    }

    let orphelines = "";
    let o = 0;
    while o < ArraySize(cles) {
        if !trouvees[o] {
            orphelines += (Equals(orphelines, "") ? "" : ", ") + cles[o];
        }
        o += 1;
    }
    if NotEquals(orphelines, "") {
        TesseraJournalHabillage(
            s"  ⚠ NON CUIT dans l'entite — invisible pour les autres : \(orphelines)");
    }

    // ── ⭐⭐⭐ L'INVENTAIRE COMPLET, UNE FOIS, À LA CONVERGENCE ─────────────────────────────
    //
    // ⛔ POURQUOI IL EXISTE. Le 2026-09-06, trois defauts ont ete rapportes par Lucas dans le
    // meme message — « trois bras sur LUCAS1 », « en local ils sont habilles et rendus nus »,
    // « pas de suppression de l'ancien modele ». Aucun des trois n'etait lisible dans les
    // journaux : on y voit des COMPTES (« 3 vetement(s) demande(s), 5 allume(s) ») qui disent ce
    // qu'on a demande, jamais ce qui est REELLEMENT monte sur le corps.
    //
    // ⭐ Un compte d'intention ne prouve rien sur le rendu. « 5 allumes » est vrai aussi bien
    // avec un bras qu'avec trois. Cet inventaire nomme chaque composant et son etat — c'est la
    // seule chose qui permet de compter les bras sans les yeux de quelqu'un.
    //
    // ⚠️ UNE SEULE FOIS PAR CORPS, a la convergence. Le repeter a chaque passe noierait le
    // journal (54 composants par avatar, plusieurs avatars, toutes les deux secondes) et
    // rendrait illisible la ligne qui compte.
    TesseraJournalHabillage(s"inventaire de \(EntityID.ToDebugString(cible)) :");
    let inv = 0;
    let ligne = "";
    while inv < ArraySize(composants) {
        let n = NameToString(composants[inv].GetName());
        ligne += (Equals(ligne, "") ? "" : " ") + n + (composants[inv].IsEnabled() ? "+" : "-");
        // On coupe en morceaux : une ligne de journal trop longue est tronquee en silence.
        if (inv % 8) == 7 {
            TesseraJournalHabillage("  " + ligne);
            ligne = "";
        }
        inv += 1;
    }
    if NotEquals(ligne, "") {
        TesseraJournalHabillage("  " + ligne);
    }

    // ⚠️ On rend `true` dès qu'on a fait le tour : rallumer ce qui est déjà allumé ne sert à rien,
    // et laisser la boucle tourner ferait relire tous les composants de tous les avatars à chaque
    // créneau — pour rien, et à l'échelle de dizaines de joueurs ça se paierait.
    return true;
}

// Relève, pour chaque composant de mesh, le nom et l'apparence RÉELLEMENT posée.
//
// ⚠️ Le `meshAppearance` est un `CName` ; c'est le contenu réel de l'entité, pas ce qu'on croit lui
// avoir donné. Lire ce champ est fiable ; l'écrire est une impasse (F-PLY-191).
func TesseraReleverApparences(entite: ref<Entity>, etiquette: String) -> Void {
    let composants = entite.GetComponents();
    let n = 0;
    let i = 0;
    while i < ArraySize(composants) {
        let mesh = composants[i] as entSkinnedMeshComponent;
        if IsDefined(mesh) {
            TesseraJournalHabillage(
                s"  \(etiquette) · \(NameToString(mesh.name)) = \(NameToString(mesh.meshAppearance))");
            n += 1;
        }
        i += 1;
    }
    TesseraJournalHabillage(s"  \(etiquette) : \(n) composant(s) de mesh sur \(ArraySize(composants))");
}

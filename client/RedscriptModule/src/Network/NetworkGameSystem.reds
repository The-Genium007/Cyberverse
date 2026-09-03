module Cyberverse.Network.Managers
import Codeware.*

public native class NetworkGameSystem extends IGameSystem {
    //public native func ConnectToServer(host: String, port: Uint16) -> Void;
    native let FullyConnected: Bool;
    native let playerActionTracker: ref<PlayerActionTracker>;
    public native func EnqueueLoadLastCheckpoint(handler: wref<inkISystemRequestsHandler>) -> Void;

    // Remonte un stimulus observé chez le joueur local (les 67 `gamedataStimType`). Backing natif :
    // `RTTI_METHOD(Tessera_ReportStim)` dans NetworkGameSystem.h — sans CETTE déclaration, l'appel
    // ne se résout pas et tout r6/scripts tombe, même si le C++ enregistre bien la méthode.
    // `nature` en Uint32 : redscript n'a pas de type 8 bits.
    // `target` est une EntityID LOCALE, pas un id réseau : le C++ fait la traduction, parce que lui
    // seul tient la table `networkId → EntityID`. Il envoie 0 si la cible n'est pas une entité
    // serveur — un passant de la foule native n'a aucune identité partagée (ADR 0022).
    public native func Tessera_ReportStim(nature: Uint32, radius: Float, target: EntityID) -> Void;

    // Cette entité est-elle déjà répliquée par le serveur ? Le redscript ne peut pas répondre :
    // la table `networkId → EntityID` vit côté C++. C'est ce qui distingue un FIGURANT purement
    // local d'une entité déjà sous autorité.
    public native func Tessera_EstEntiteReseau(cible: EntityID) -> Bool;

    // Rapporte au serveur des dégâts infligés à une entité réseau. Backing natif :
    // `RTTI_METHOD(Tessera_RapporterDegats)` — les deux côtés se posent ET se déploient ensemble,
    // sinon TOUT `r6/scripts` tombe.
    //
    // ⚠️ `cible` est une EntityID LOCALE, traduite côté C++ comme celle de `Tessera_ReportStim`.
    // Rien ne part si la cible n'est pas une entité serveur : un figurant de la foule native ne
    // désigne personne chez le serveur.
    //
    // `degats` en points de vie « jeu », calculés par le moteur du tireur — c'est lui qui sait le
    // faire (arme, mods, armure, critiques, zone touchée). Le serveur ne les croit pas sur parole :
    // il écrête, il cadence, et c'est lui qui tient la seule barre de vie qui compte (`sante.rs`).
    // `false` = rien n'est parti (cible non réseau, dégâts nuls, pas de connexion).
    public native func Tessera_RapporterDegats(cible: EntityID, degats: Uint32) -> Bool;

    // ── L'arme en main (chantier arme distante, 2026-08-10) ────────────────────────────────
    //
    // Ce que le joueur LOCAL tient, annoncé au serveur. `item` = hash TweakDBID, `degainee` dit si
    // elle est en main — une arme rangée s'annonce avec `false`, et le serveur vide alors les mains
    // de l'avatar chez tous les observateurs.
    //
    // ⚠️ N'ÉMETTRE QUE SUR CHANGEMENT. Le détecteur sonde deux fois par seconde ; réémettre à
    // chaque sondage inonderait le fil ET ferait rejouer l'animation de dégainage en boucle chez
    // tous ceux qui regardent. Le filtre vit dans `ArmeAvatar.reds`, qui SAIT ce qui a changé.
    public native func Tessera_RapporterArme(item: Uint64, degainee: Bool) -> Bool;

    // L'arme que le SERVEUR annonce pour CETTE entité réseau. Renvoie un `TweakDBID` INVALIDE si
    // l'entité est inconnue ou si son joueur a les mains vides. C'est ce qui remplace le miroir
    // local : chaque avatar porte l'arme de SON joueur, plus la nôtre.
    //
    // ⚠️ Renvoie un `TweakDBID` et non un `Uint64`, parce que redscript expose `TDBID.ToNumber`
    // mais **aucune conversion inverse** : un hash 64 bits y est un cul-de-sac. La conversion se
    // fait donc côté C++, où le hash EST déjà un TweakDBID.
    // Verrou d'une sonde a usage unique : rend `true` la premiere fois, `false` ensuite.
    // Il vit cote C++ parce que l'appelant est rappele pour CHAQUE avatar.
    public native func Tessera_PremierVidageEtat() -> Bool;

    public native func Tessera_ArmeDeLEntite(cible: EntityID) -> TweakDBID;

    // ── CE QUE LE SERVEUR VEUT VOIR SUR LE DOS DE CETTE ENTITÉ ──────────────────────────────
    //
    // Autorité : la base de données du serveur (colonne `contenus.porte`, migration 0012), jamais
    // le client. Les items arrivent par `AppearanceSync.garments` avec `drawn = false` — le vrai
    // désigne l'arme en main, qui suit une recette entièrement différente (F-PLY-203).
    //
    // ⚠️ DEUX APPELS PLUTÔT QU'UN TABLEAU, et ce n'est pas de la timidité : aucun `DynArray` ne
    // traverse le RTTI dans ce plugin aujourd'hui, alors qu'un `TweakDBID` de retour est mesuré
    // (`Tessera_ArmeDeLEntite`, en service depuis le 2026-07-24). On reste sur la forme prouvée.
    //
    // ⚠️ Et pour la même raison qu'elle : ça rend un `TweakDBID`, pas un `Uint64`. redscript expose
    // `TDBID.ToNumber` mais **aucune conversion inverse** — un hash 64 bits y est un cul-de-sac.
    //
    // `0` vêtement signifie « ne porte rien » AUSSI BIEN QUE « entité inconnue » : dans les deux
    // cas il n'y a rien à poser, donc rien à décider ici.
    public native func Tessera_NombreDeVetements(cible: EntityID) -> Int32;
    // Le sexe du corps de cet avatar — choisit quelle moitie de la garde-robe allumer.
    public native func Tessera_AvatarCorpsMasculin(cible: EntityID) -> Bool;
    public native func Tessera_VetementDeLEntite(cible: EntityID, index: Int32) -> TweakDBID;

    // ── Coma et réapparition (chantier autorité totale, 2026-08-09) ─────────────────────────
    //
    // Le serveur décide, le client demande et affiche. `Tessera_DemanderReapparition` renvoie
    // `true` si le message est PARTI — jamais qu'il a été accepté : le serveur refuse une demande
    // prématurée sans rien répondre (`sante.rs::reapparaitre`). Un bouton grisé n'est pas une
    // sécurité ; ce refus, si.
    public native func Tessera_DemanderReapparition() -> Bool;
    // Rapporte une variation de vie que le serveur ne peut pas connaître (régénération, soin,
    // chute, feu, PNJ, véhicule). On lui donne le pourcentage COURANT, il fait tout le reste :
    // comparaison à la référence, seuil, conversion, envoi.
    //
    // ⚠️ Toute la logique est côté C++ EXPRÈS. C'est lui qui tient la référence, et c'est lui que
    // `HealthSync` met à jour quand le serveur nous impose une valeur — sans quoi on renverrait au
    // serveur sa propre écriture, en boucle. Dupliquer cette référence ici en ferait deux, qui
    // dériveraient.
    //
    // `cause` : 0=inconnu 1=régénération 2=soin 3=chute 4=environnement 5=PNJ 6=véhicule. On envoie
    // 0 tant qu'on ne sait pas distinguer — un code faux serait pire qu'un code absent.
    public native func Tessera_RapporterVariation(pourcentCourant: Float, cause: Uint32) -> Int32;
    // Secondes de coma restantes, poussées par le serveur chaque seconde. **-1 = vivant** — et ce
    // n'est pas la même chose que 0, qui veut dire « mort, et l'hôpital est ouvert ».
    // Jamais décomptées par le client : deux horloges divergeraient, et c'est alors l'écran qui
    // mentirait au joueur sur le temps qu'il lui reste.
    public native func Tessera_SecondesSecours() -> Int32;
    // Le serveur autorise-t-il la réapparition ? À lire tel quel, sans le déduire du décompte.
    public native func Tessera_HopitalOuvert() -> Bool;

    // ── Faim & soif (chantier besoins, 2026-08-09) ──────────────────────────────────────────
    // Pour mille, 1000 = rassasié. Poussées par le serveur dans `HealthSync` (champs `faim`/`soif`,
    // renseignés uniquement dans la copie `mine = true`) et lues par les jauges du HUD
    // (`TesseraHudVitals`). Backing C++ : `RTTI_METHOD(Tessera_Faim)`/`(Tessera_Soif)` dans
    // `NetworkGameSystem.h` — les deux côtés se posent ET se déploient ensemble, sans quoi TOUT
    // `r6/scripts` tombe (piège payé le 2026-08-08).
    //
    // ⚠️ Jamais décomptées par le client, même raison que le coma ci-dessus : la seule horloge qui
    // compte est celle du serveur. Le HUD ne fait que relire une valeur qu'on lui a donnée. Avant
    // le premier `HealthSync`, elles valent 1000 (voir `m_faim` côté C++) : le serveur n'émet que
    // sur CHANGEMENT, donc les premières secondes d'une session n'apportent rien, et un défaut à
    // zéro afficherait deux jauges vides qu'on lirait comme une panne.
    public native func Tessera_Faim() -> Int32;
    public native func Tessera_Soif() -> Int32;

    // Millisecondes depuis le dernier `Snapshot` reçu. -1 = aucun n'est encore arrivé — à ne jamais
    // confondre avec 0, qui veut dire « fil parfaitement frais ». Backing C++ :
    // `RTTI_METHOD(Tessera_SilenceMs)`, même règle de déploiement conjoint que les deux ci-dessus.
    //
    // Lue par `UiKitLienPerdu.reds`, croisée avec `FullyConnected`, pour distinguer les deux façons
    // dont le serveur peut disparaître : socket tombée (Gateway parti) contre socket vivante mais
    // muette (Shard tombé). Le second cas n'a AUCUN autre symptôme côté client.
    public native func Tessera_SilenceMs() -> Int32;

    // Essais de reconnexion consécutifs depuis la dernière connexion réussie. 0 = aucun en cours.
    // L'écran d'attente ne parle de « reconnexion » que si ce compteur bouge : tant qu'il vaut 0,
    // il constate la panne au lieu de promettre un retour qui n'aurait pas lieu.
    public native func Tessera_TentativesReconnexion() -> Int32;

    // Rejoue la connexion immédiatement, sans attendre la fin du recul. La demande explicite du
    // joueur passe avant la temporisation — celle-ci n'existe que pour ne pas marteler un serveur
    // mort, pas pour faire attendre quelqu'un qui vient de cliquer.
    public native func Tessera_ReconnecterMaintenant() -> Void;

    // Journal de SONDE — écrit dans le log du plugin, donc UN FICHIER PAR INSTANCE.
    // `FTLog` écrit dans le gamelog de CET, partagé par toutes les instances : deux clients y
    // mélangent leurs lignes, ce qui interdit toute comparaison entre eux.
    public native func Tessera_Journal(texte: String) -> Void;

    // ── PNJ statiques : apparence arbitrée par le serveur ───────────────────────────────────
    //
    // ⚠️ `cible` est passée TELLE QUELLE, sans traduction — contrairement à `Tessera_ReportStim`.
    // C'est LA différence entre les deux populations : l'identifiant d'un statique dérive des
    // données de secteur et vaut la même chose sur toutes les machines (F-PNJ-128), donc il désigne
    // quelque chose pour le serveur. Celui d'un passant ne désigne rien hors de sa machine.
    // ⚠️ La position est celle du PNJ, pas du joueur : c'est elle qui range le rapport dans la
    // bonne cellule du halo. Un joueur voit à 80 m, donc souvent dans une autre cellule que la
    // sienne — ranger sur la position du rapporteur éparpillerait la table.
    // `yaw` (degres) depuis le 2026-08-09 : il ne sert pas a l'apparence, mais a ce qu'un client
    // a qui ce PNJ MANQUE puisse en fabriquer un remplacant ORIENTE comme le natif.
    public native func Tessera_RapporterStatique(cible: EntityID, record: Uint64, apparence: CName, x: Float, y: Float, z: Float, yaw: Float) -> Void;
    // Cette cellule a-t-elle déjà été servie par le serveur ? Si oui, inutile d'y rapporter : c'est
    // ce qui fait tomber le trafic de 5 rapports/s à un par cellule vierge.
    public native func Tessera_CelluleConnue(x: Float, y: Float) -> Bool;
    // Apparence déjà connue pour ce statique, ou CName nulle. Sert au REJEU : le serveur diffuse
    // sans filtre de distance, donc une apparence peut arriver AVANT que le PNJ ne soit chargé.
    public native func Tessera_ApparenceStatiqueConnue(cible: EntityID) -> CName;
    // SONDE one-shot — voir AppearanceProbe.reds. Renvoie une apparence DIFFÉRENTE déjà vue pour ce
    // record (donc valide), ou une CName nulle.
    public native func Tessera_CobayeApparence(record: Uint64, apparence: CName) -> CName;

    // Demande au serveur de prendre un figurant sous son autorité. On envoie de quoi le
    // REFABRIQUER (record, apparence, position), pas un identifiant : le pantin n'existe que sur
    // cette machine — ADR 0022.
    public native func Tessera_DemanderPromotion(record: Uint64, apparence: CName, x: Float, y: Float, z: Float, yaw: Float, mort: Bool) -> Bool;

    // Autorité serveur (TesseraSynth) — reflètent le dernier ShardAssignment reçu + le nombre de
    // puppets distants suivis. Consommés par le HUD moniteur de cohérence via des wrappers
    // @addMethod(PlayerPuppet) côté modset Tessera (Tessera_GetServerShard/Overlaps/VisiblePlayerCount).
    public native func Tessera_GetServerShard() -> String;
    public native func Tessera_GetServerOverlaps() -> String;
    public native func Tessera_GetVisiblePlayerCount() -> Int32;

    // ── Flux d'arrivée : personnages du compte (lobby Tessera, 2026-08-08) ──────────────────
    // Backing natif : `RTTI_METHOD(...)` dans NetworkGameSystem.h. ⚠️ Sans CES déclarations,
    // l'appel ne se résout pas et TOUT r6/scripts tombe — même si le C++ enregistre bien les
    // méthodes. Les deux côtés se posent ensemble, jamais l'un sans l'autre.
    //
    // Le client n'a AUCUNE autorité ici : il affiche ce que le serveur envoie et demande ce que le
    // joueur clique. Le cap de personnages (`character.slots.N`, défaut 1, illimité pour un joker),
    // l'unicité du pseudonyme et la validité de l'apparence sont arbitrés serveur — un client
    // modifié ne peut donc pas s'octroyer un second personnage.
    //
    // ⚠️ « 0 personnage » ≠ « pas encore reçu ». Toujours tester `Tessera_ListePersonnagesRecue()`
    // avant de conclure qu'un compte est vide : un lobby qui affiche « aucun personnage » trop tôt
    // pousse le joueur à en créer un doublon, que le serveur refusera.
    public native func Tessera_NombrePersonnages() -> Int32;
    public native func Tessera_ListePersonnagesRecue() -> Bool;
    // `--tessera-dev` sur la ligne de commande : sauter le lobby et entrer avec un personnage
    // assigné d'office. Outil de DÉVELOPPEMENT — il court-circuite l'écran d'entrée, donc il n'a
    // rien à faire chez un joueur ; c'est aussi ce qui permet à un agent de tester sans humain.
    public native func Tessera_ModeDeveloppement() -> Bool;

    /// Quel personnage le mode dev doit prendre — `--tessera-dev=2` rend **2**, en base 1.
    /// `0` = rien de demandé : le script tire alors au sort, ce qui évite que deux instances
    /// lancées sans numéro se disputent le même personnage.
    ///
    /// ⚠️ La déclaration ci-dessus n'est PAS redondante avec le `RTTI_METHOD` du C++, et l'oublier
    /// coûte un lancement : `scc` compile **hors du jeu**, donc sans qu'aucun plugin RED4ext n'ait
    /// enregistré quoi que ce soit au RTTI. Sans cette ligne, le compilateur rend
    /// « method not found on NetworkGameSystem » — et comme redscript refuse TOUT `r6/scripts` dès
    /// qu'un fichier échoue, le jeu ne démarre plus du tout.
    public native func Tessera_PersonnageDemande() -> Int32;
    public native func Tessera_NomPersonnage(index: Int32) -> String;
    // L'origine du personnage, telle que le SERVEUR la connait : « corpo » | « gosse_des_rues » |
    // « nomade ». Chaine VIDE pour un personnage cree avant que le champ n'existe — l'UI doit
    // alors n'afficher rien, jamais une valeur de repli qui se lirait comme une donnee.
    public native func Tessera_OriginePersonnage(index: Int32) -> String;
    // Le CORPS et le CERVEAU du personnage — deux genres INDEPENDANTS chez CDPR : le corps decide
    // du pantin monte chez les autres joueurs, le cerveau decide de la voix.
    // ⚠️ `false` couvre deux cas que le fil ne distingue pas : un choix feminin, et un personnage
    // cree avant l'existence du champ. Seule la BASE garde la nuance.
    // L'esthetique du personnage, en hexadecimal — meme forme que `Tessera_LireEsthetique`.
    // Chaine VIDE si le serveur n'en a pas, ou si ce qu'il a n'est pas un `TSV1`.
    public native func Tessera_EsthetiquePersonnage(index: Int32) -> String;
    // ⭐ La RECETTE d'esthétique du personnage à cet index — « nom:index;nom:index », prête à être
    // rejouée par `ApplyChangeToOption` + `ReFinalizeState` (F-PLY-246). Vide pour un personnage
    // antérieur à la capture : le client n'applique alors rien.
    public native func Tessera_RecettePersonnage(index: Int32) -> String;
    // ⭐⭐ La recette du personnage QU'ON INCARNE — sans connaître son index. C'est le point
    // d'entrée de l'hydratation à l'arrivée en jeu : le chargement du monde détruit les
    // contrôleurs de menu, donc rien ne peut transporter un index depuis le lobby. Le netcode,
    // lui, retient déjà le personnage incarné (il en a besoin pour la reprise après reconnexion).
    public native func Tessera_RecetteIncarnee() -> String;
    // ⭐ Départ VOLONTAIRE : le serveur libère la place tout de suite, au lieu de la réserver
    // quelques minutes comme après une coupure. À appeler AVANT de fermer le jeu.
    public native func Tessera_QuitterServeur() -> Bool;
    public native func Tessera_CorpsMasculin(index: Int32) -> Bool;
    public native func Tessera_CerveauMasculin(index: Int32) -> Bool;
    public native func Tessera_IdPersonnage(index: Int32) -> Uint64;
    // L'avatar d'un personnage EXISTANT, pour que le lobby puisse dessiner sa jaquette. 0 = pas
    // d'avatar connu -> silhouette de repli, jamais une carte vide.
    public native func Tessera_RecordPersonnage(index: Int32) -> Uint64;
    public native func Tessera_ApparencePersonnage(index: Int32) -> Uint64;
    // "" = rien de neuf · "ok" = créé · sinon le motif brut du serveur (`slot_full`,
    // `pseudonym_taken`, …). La lecture CONSOMME le résultat : un refus déjà affiché ne revient pas.
    public native func Tessera_DernierResultat() -> String;
    // `true` = la demande est PARTIE, pas qu'elle est acceptée. Le verdict arrive séparément.
    // `record` en Uint64 via `TDBID.ToNumber(...)`, `apparence` en CName passée telle quelle : le
    // couple exact déjà éprouvé par `Tessera_DemanderPromotion`. Côté C++ les deux arrivent en
    // `uint64_t` — une CName EST un hash 64 bits, la conversion est faite par le RTTI.
    // `origine` : corpo / nomade / gosse des rues. Elle gouverne la dotation de départ côté serveur,
    // eurodollars compris (`dotation.toml`).
    //
    // ⚠️ Une chaîne VIDE est légitime, et le serveur ne la refuse pas : l'écran du lobby ne posait
    // pas encore la question quand ce champ est arrivé. Le serveur retombe alors sur sa dotation de
    // repli, volontairement la plus maigre — personne ne doit avoir intérêt à ne pas choisir.
    // `esthetiqueHex` : blob `TSV1` hexadecimal, ou "" — vide est LEGITIME (ADR 0036, le fork
    // transporte, il ne capture pas). Ajoute en DERNIER argument : tout appelant doit le passer.
    public native func Tessera_CreerPersonnage(pseudonyme: String, record: Uint64, apparence: CName, origine: String, esthetiqueHex: String, corpsMasculin: Bool, cerveauMasculin: Bool, optionsApparence: String) -> Bool;

    // ── LA CAPTURE — lire l'esthétique du V LOCAL, pour la proposer au serveur ────────────────
    //
    // Rend le blob `TSV1` en hexadécimal, à passer tel quel en dernier argument de
    // `Tessera_CreerPersonnage`. C'est ce que le créateur de personnage appellera à la validation.
    //
    // `etat` : `GameInstance.GetCharacterCustomizationSystem().GetState()`.
    //
    // ⚠️ CHAÎNE VIDE = REFUS, et c'est un cas NORMAL, pas une panne : l'état de customisation est
    // parfois non finalisé. Traiter le vide comme « pas encore », et surtout NE PAS l'envoyer :
    // un descripteur vide donne un avatar sans visage chez les autres joueurs, avec un symptôme
    // très loin de sa cause. La raison exacte du refus part dans le journal du client.
    public native func Tessera_LireEsthetique(etat: ref<IScriptable>) -> String;
    public native func Tessera_ChoisirPersonnage(id: Uint64) -> Bool;
    // Supprime un personnage. Le serveur arbitre et renvoie la liste à jour — le client ne retire
    // rien de lui-même, sinon il afficherait une suppression qui pourrait être refusée.
    public native func Tessera_SupprimerPersonnage(id: Uint64) -> Bool;

    // ── Interactions joueur↔joueur (spec 2026-08-09) ───────────────────────────────────────
    //
    // ⚠️ Ces six déclarations ont leurs six `RTTI_METHOD` dans `NetworkGameSystem.h`. Les deux
    // côtés se posent ET se déploient ENSEMBLE : un `native func` sans backing dans la DLL déployée
    // fait tomber TOUT `r6/scripts` et le jeu se ferme SANS UN MOT (F-PLF-020, F-PLF-023). Le
    // contrôle avant tout lancement : comparer les `native func Tessera_*` d'ici aux chaînes de la
    // DLL construite.
    //
    // Le catalogue se lit PAR INDEX plutôt que rendu d'un coup : recevoir un `array<struct>` d'un
    // natif obligerait à déclarer la struct des deux côtés, donc à créer une occasion de plus de les
    // désynchroniser — exactement la panne ci-dessus. Trois accesseurs scalaires ne divergent pas.

    // Combien d'actions CE joueur a le droit de proposer. Déjà filtré par le serveur : le client
    // n'apprend jamais l'existence de celles qu'il n'a pas. 0 est un cas légitime, pas une erreur.
    public native func Tessera_NombreActions() -> Int32;
    // L'id de recette à cet index — c'est LUI qu'on renvoie, jamais l'index : le catalogue peut
    // changer entre l'affichage et le clic (`/groupgrant` le repousse à chaud).
    public native func Tessera_ActionId(index: Int32) -> Int32;
    public native func Tessera_ActionLibelle(index: Int32) -> String;
    // Portée en mètres, 0 = sans limite. Sert à décider d'AFFICHER, jamais à autoriser : le serveur
    // revérifie droit, portée et état à l'exécution, systématiquement.
    public native func Tessera_ActionPorteeM(index: Int32) -> Float;

    // Le nom de cette entité, SI on nous l'a donné. Chaîne VIDE pour un inconnu — et c'est la
    // réponse normale, pas une panne.
    //
    // ⚠️ Un nom absent n'est pas un nom masqué : il n'a jamais traversé le fil. Le serveur ne
    // l'envoie qu'à qui s'est fait présenter, donc un client modifié ne peut pas le révéler.
    public native func Tessera_NomConnu(cible: EntityID) -> String;

    // Déclenche une recette sur une cible. `recette` = ce que rend `Tessera_ActionId`.
    // Renvoie true si le message est PARTI — jamais qu'il a été accepté.
    public native func Tessera_EnvoyerAction(cible: EntityID, recette: Uint32) -> Bool;

    // ── LES VERBES VÉHICULE (protocol.fbs, EntityInteraction) ───────────────────────────────
    //
    // UN seul natif pour les cinq verbes : ils ne diffèrent que par deux entiers.
    //
    //    9  → verrou           param : 0 ouvre, non-nul ferme   (PROPRIÉTAIRE, refus serveur)
    //    10 → revendiquer      param ignoré                     (sans effet si déjà possédé)
    //    11 → radio            param : station, 0 = éteinte      (CONDUCTEUR)
    //    12 → casse            param : 0..100, MONOTONE          (CONDUCTEUR)
    //    14 → ouvrir le coffre param ignoré                      (réponse : InteractionOpen)
    //
    // ⚠️ 13 est la POSTURE, pas nous. Le natif refuse toute valeur hors 9..14 et rejette 13 :
    // une faute de frappe partirait sinon dans un AUTRE espace d'identifiants, où `target` ne
    // désigne pas la même chose. La collision des kinds 6/7 a déjà fait tomber dix-sept tests.
    //
    // Renvoie true si le message est PARTI — jamais qu'il a été accepté.
    public native func Tessera_VehiculeVerbe(cible: EntityID, verbe: Uint8, param: Uint32) -> Bool;

    // Ce vehicule est-il connu du SERVEUR ? Faux pour la circulation native.
    public native func Tessera_EstVehiculeReseau(cible: EntityID) -> Bool;

    // ── LA CASSE VUE PAR LES TÉMOINS ────────────────────────────────────────────────────────
    //
    // Le serveur diffuse la casse à tout le monde ; chez un témoin, le moteur ne la calcule pas
    // (il ne simule pas cette voiture, F-VEH-041). On lui donne le nombre, il déroule le reste.
    //
    // ⚠️ `Tessera_DegatsConnus` est le GARDE-FOU CONTRE L'ÉCHO : un témoin qui applique
    // redéclenche `ReactToHPChange` chez lui, et s'il conduit il renverrait ce qu'il vient de
    // recevoir. La casse étant MONOTONE, l'aller-retour ne pourrait que la faire MONTER — les
    // voitures se dégraderaient toutes seules. On ne rapporte que ce que le serveur ignore encore.
    public native func Tessera_DegatsConnus(cible: EntityID) -> Int32;
    public native func Tessera_DegatsEnAttente() -> Int32;
    public native func Tessera_DegatsVehicule() -> EntityID;
    public native func Tessera_DegatsValeur() -> Int32;
    public native func Tessera_DegatsDefiler() -> Void;

    // ── LE COFFRE ────────────────────────────────────────────────────────────────────────────
    //
    // Lecture : le serveur ENONCE le contenu, le client s'y aligne (meme doctrine que le sac).
    // ⚠️ `Tessera_CoffreSeq` est un COMPTEUR, pas un booleen « recu » : un coffre s'ouvre
    // plusieurs fois, et deux ouvertures identiques seraient indiscernables par un drapeau.
    public native func Tessera_CoffreSeq() -> Int32;
    public native func Tessera_CoffreVehicule() -> EntityID;
    public native func Tessera_CoffreCapacite() -> Int32;
    public native func Tessera_CoffreTaille() -> Int32;
    public native func Tessera_CoffreItemId(index: Int32) -> String;
    public native func Tessera_CoffreItemQuantite(index: Int32) -> Int32;

    // Rapport : trois natifs plutot qu'un prenant des tableaux — le marshalling d'un
    // `array<String>` par RTTI est exactement le genre d'endroit ou l'on echoue EN SILENCE.
    public native func Tessera_CoffreViderRapport() -> Void;
    public native func Tessera_CoffreAjouterAuRapport(item: String, quantite: Int32) -> Void;
    public native func Tessera_CoffreEnvoyerRapport() -> Bool;

    // ── ASCENSEURS (ADR 0012) ───────────────────────────────────────────────────────────────
    //
    // `cabine` est l'EntityID STATIQUE de la cabine, LU sur l'entité et jamais recalculé : le hash
    // runtime est un compound hash dont l'algorithme n'est pas confirmé (F-ASC-011, re-mesuré le
    // 2026-08-24 — aucune variante FNV1a64 du chemin `$/...` ne le redonne).
    //
    // ⚠️ Ne PAS passer par `EntityID.GetHash` côté script pour fabriquer une clé : il rend un
    // **Uint32**, donc les 32 bits de poids faible. Le natif lit les 64 bits lui-même.
    //
    // Rend true si le message est PARTI — jamais qu'il a été accepté. Le serveur revalide
    // l'existence de la cabine et la viabilité de l'étage, et ignore en silence ce qui ne va pas.
    public native func Tessera_AppelerAscenseur(cabine: EntityID, etage: Int32) -> Bool;
    // ── La FILE des etats de cabine recus, drainee par redscript ──────────────────────────
    //
    // Un PULL et non un push : trois voies d'appel C++ -> redscript ont ete essayees le
    // 2026-08-24 (`@addMethod` + CallVirtual, fonction globale + CallGlobal, methode statique +
    // CallStatic) et les trois echouent a la RESOLUTION DE NOM au runtime, en journalisant une
    // seule ligne. Le pull ne depend d'aucune resolution de symbole scripte -- et un natif absent
    // fait echouer la COMPILATION, bruyamment.
    // POSTURES — l'annonce montante. Le client DEMANDE, le serveur DECIDE : on n'envoie pas
    // « je suis assis » mais « je voudrais l'emplacement N », et `postures.rs` accorde ou
    // refuse (occupe, trop loin, inconnu) puis pose `PlayerState.sustained` que tous les
    // clients recoivent. `emplacement = 0` libere.
    //
    // ⚠️ Le serveur ecoute ce message depuis le 2026-08-19 et personne ne l'envoyait — comme
    // personne ne LISAIT `sustained` en retour (F-PLY-303). Les deux moities du canal etaient
    // absentes, chacune supposant que l'autre existait.
    public native func Tessera_SignalerPosture(emplacement: Uint64, code: Uint32) -> Bool;
    public native func Tessera_AscenseurTotalRecus() -> Int32;
    public native func Tessera_AscenseurEnAttente() -> Int32;
    public native func Tessera_AscenseurCabine() -> EntityID;
    public native func Tessera_AscenseurEtageActif() -> Int32;
    public native func Tessera_AscenseurEtageCible() -> Int32;
    public native func Tessera_AscenseurDepart() -> Int32;
    public native func Tessera_AscenseurElapsedMs() -> Int32;
    public native func Tessera_AscenseurRetirer() -> Void;

    // ── APPAREILS DU MONDE (portes, portiques, contenants…) — spec 2026-08-26 ─────────────────
    //
    // Le guichet du PULL. Le C++ ne pousse pas : les trois voies de push echouent a la resolution
    // de nom AU RUNTIME, en silence (voir l'en-tete de `ElevatorRelaisReseau.reds`). Le client
    // vient donc chercher — `Tessera_AppareilEnAttente`, les quatre champs de la tete de file,
    // puis `Tessera_AppareilRetirer` pour avancer.
    public native func Tessera_AppareilTotalRecus() -> Int32;
    public native func Tessera_AppareilEnAttente() -> Int32;
    public native func Tessera_AppareilId() -> EntityID;
    public native func Tessera_AppareilFamille() -> Int32;
    public native func Tessera_AppareilEtat() -> Int32;
    public native func Tessera_AppareilProprietaire() -> Int32;
    /// Combien de joueurs tiennent cet appareil ouvert (propagation d'ouverture automatique).
    /// `0` = la fermeture automatique locale reprend ses droits.
    public native func Tessera_AppareilTenants() -> Int32;
    // Rend `false` quand la file est vide. Le `Bool` n'est pas decoratif : une `RTTI_METHOD` en
    // `void` a deja ete compilee, liee et ABSENTE du binaire (F-PLF-020), ce qui fait tomber tout
    // `r6/scripts` sans un mot.
    public native func Tessera_AppareilRetirer() -> Bool;
    /// Rapporte au serveur l'etat ou le joueur vient de laisser un appareil. Rend `true` si le
    /// message est PARTI — jamais qu'il a ete accepte (doctrine D1) : le serveur revalide la
    /// famille, l'etat, la distance et les droits.
    /// « Ouvre-moi ce contenant du monde » — caisse, casier, planque (famille 3 de la spec des
    /// appareils). Le serveur répond par un `InteractionOpen` que le netcode traite comme un coffre
    /// de véhicule : même contenu autoritaire, même porteur, même écran natif. Voir
    /// `NetworkGameSystem.h`, `Tessera_OuvrirContenant`.
    ///
    /// ⚠️ Rend `true` si le message est PARTI, jamais qu'il a été accepté (D1).
    /// « Je vise cet appareil » — le netcode colle la cible aux commandes `door …` qui n'en
    /// portent pas déjà une. `EntityID` non défini efface la visée.
    public native func Tessera_PoserAppareilVise(device: EntityID) -> Void;

    public native func Tessera_OuvrirContenant(device: EntityID) -> Bool;

    public native func Tessera_RapporterAppareil(device: EntityID, famille: Int32, action: Int32, etat: Int32) -> Bool;

    /// ⭐ LE CANAL DE COMMANDE D'ADMINISTRATION — il n'existait PAS avant le 2026-08-26.
    ///
    /// Le serveur comprend `ClientMsg::AdminCommand` depuis toujours, et aucun client ne l'a jamais
    /// emis : `/promote`, `/grant`, `/ban`, `/vehicule`, `/besoins`, `/porte` etaient tous
    /// injoignables depuis le jeu. Meme mode de panne que les sept fils debranches du chantier
    /// « autorite totale » — un producteur complet dont la sortie n'allait nulle part.
    ///
    /// ⚠️ N'ACCORDE AUCUN DROIT. Le serveur revalide le rang et les permissions de l'appelant
    /// avant d'executer quoi que ce soit ; ceci n'ouvre que le tuyau.
    public native func Tessera_EnvoyerCommandeAdmin(texte: String) -> Bool;

    // LA CONSOLE, cote LECTURE (2026-08-30). Depile une ligne au format `"<niveau>|<texte>"`,
    // rend "" si la file est vide. Le PULL est le seul mecanisme qui marche ici : trois voies
    // d appel C++ -> redscript echouent a la resolution de nom EN SILENCE sur ce depot.
    public native func Tessera_ConsoleLireLigne() -> String;
    public native func Tessera_ConsoleEnAttente() -> Int32;
    public native func Tessera_ConsoleTotalRecu() -> Int32;

    // LE CATALOGUE DE COMMANDES (2026-08-30) — ce qui rend les suggestions possibles.
    // `Tessera_CommandeNom` sert a COMPLETER, `Tessera_CommandeAffichage` a MONTRER. Les deux
    // rendent "" hors bornes : le catalogue peut retrecir entre deux images si un droit change.
    public native func Tessera_NombreCommandes() -> Int32;
    public native func Tessera_CommandeNom(index: Int32) -> String;
    public native func Tessera_CommandeAffichage(index: Int32) -> String;

    // MODE STAFF (2026-08-31) — l'etat pousse par le serveur, pas une deduction. Allume le
    // temoin permanent du HUD tant que `/gm on` n'a pas ete defait par `/gm off`.
    public native func Tessera_ModeStaff() -> Bool;

    // Les VALEURS proposables d'une commande (2026-09-01) — les joueurs connectes, pour `/tp`.
    // Zero pour toutes les autres. Rendent "" hors bornes.
    public native func Tessera_NombreValeurs(index: Int32) -> Int32;
    public native func Tessera_CommandeValeur(index: Int32, rang: Int32) -> String;


    // Le joueur local vient d'entrer (monte=true) ou de sortir d'une cabine. Sert au RENDU chez
    // les autres : le serveur relaie le porteur, et l'observateur accroche l'interpolation de
    // l'avatar à la cabine au lieu de le laisser flotter entre deux snapshots (ADR 0039).
    // Aucune coordonnée n'est concernée — la position reste en monde des deux côtés.
    public native func Tessera_MonterAscenseur(cabine: EntityID, monte: Bool) -> Bool;

    // Un joueur DISTANT est-il dans cette cabine ? La notion d'occupation PARTAGÉE qui manque au
    // moteur : le sien (`IsPlayerInsideLift`) ne connaît que le joueur local, si bien qu'une cabine
    // pleine est « vide » pour tous ceux qui n'y sont pas — et la vitesse, les portes de palier et
    // l'obstruction en dépendent toutes les trois.
    public native func Tessera_CabineOccupee(cabine: EntityID) -> Bool;

    // Publie la hauteur VIVANTE du plancher d'une cabine (composant `movingPlatform`).
    //
    // ⭐ Le C++ ne sait pas atteindre un composant d'entité, et l'entité de l'ascenseur ne bouge PAS
    // pendant le trajet (F-ASC-032) : ce natif est le SEUL chemin par lequel le rendu apprend où se
    // trouve réellement le plancher. Sans lui, la verticale d'un passager distant vient du réseau,
    // donc avec un retard, donc elle sautille — d'autant plus que la cabine va vite.
    public native func Tessera_PoserHauteurCabine(cabine: EntityID, z: Float) -> Bool;

    // Confie le COMPOSANT du plancher, une fois pour toutes. Le rendu lit alors sa hauteur dans SA
    // frame, au lieu de l'échantillonner à 20 Hz et d'en reconstituer la pente — c'est ce qui
    // supprime la dernière source de tremblement d'un passager distant.
    public native func Tessera_PoserPlancherCabine(cabine: EntityID, plancher: ref<IScriptable>) -> Bool;

    // L'EntityID du N-ième avatar réseau, pour les PARCOURIR sans que le joueur en vise un.
    // Le compte se lit avec `Tessera_GetVisiblePlayerCount` — même table, donc un seul natif de plus.
    //
    // ⚠️ L'index n'est pas un identifiant : la table est ordonnée et son ordre change dès qu'une
    // entité apparaît ou disparaît. On énumère dans la foulée et on mémorise l'`EntityID`.
    public native func Tessera_AvatarParIndex(index: Int32) -> EntityID;

    // ── ET CEUX-LÀ SONT DES JOUEURS — c'est ce qui manquait pour la réplication de posture ──
    //
    // ⚠️ `Tessera_GetVisiblePlayerCount` et `Tessera_AvatarParIndex` ci-dessus portent des noms
    // qui MENTENT : ils parcourent toutes les entités réseau, PNJ du serveur compris (F-PLY-047).
    // Le 2026-08-15, la première tentative de réplication de posture (chantier `postures-du-monde`,
    // T7) a joué sa séquence sur un `Character.CitizenRichMale` en croyant viser un joueur, et le
    // compte rendait 4 pour deux joueurs. Ces deux-là, eux, ne rendent que des joueurs — par
    // construction, parce qu'ils lisent le tampon d'interpolation, alimenté uniquement depuis
    // `snapshot->players()`.
    //
    // Ne rendent que les avatars pourvus d'un CORPS : un joueur connu dont l'entité n'est pas
    // encore née n'est pas désignable, et le rendre ferait échouer l'appelant sur un `EntityID`
    // nul sans qu'il sache pourquoi.
    //
    // ⚠️ Même règle que ci-dessus : l'index n'est pas un identifiant. Énumérer dans la foulée,
    // mémoriser l'`EntityID`, jamais l'index.
    /// Marque un avatar comme PORTE par une plateforme (attache `BindToComponent`), donc a ne
    /// plus placer du tout. Rend l'etat effectif, pas l'argument.
    /// La cabine qui PORTE l'avatar `index`, ou un EntityID vide s'il est a pied.
    /// Meme ordre que `Tessera_AvatarJoueurParIndex`, pour se lire dans la meme boucle.
    public native func Tessera_CabineDeAvatar(index: Int32) -> EntityID;
    /// Ecrit l'ecart LOCAL d'un corps attache dans sa representation de mouvement.
    /// Rend l'etat EFFECTIF (relu apres ecriture), pas l'argument.
    /// La pose MONDE que le netcode voudrait pour cet avatar attache.
    public native func Tessera_PoseVoulueAttachee(entiteHash: Uint32) -> Vector4;
    /// L'allure annoncee pour cet avatar attache.
    public native func Tessera_AllureAttachee(entiteHash: Uint32) -> Int32;
    public native func Tessera_EcrireOffsetLocal(moveComponent: ref<IScriptable>, x: Float, y: Float, z: Float) -> Bool;
    /// Pose par le module ascenseurs quand le joueur LOCAL embarque ou descend.
    public native func Tessera_PoserJoueurLocalPorte(actif: Bool) -> Bool;
    public native func Tessera_JoueurLocalPorte() -> Bool;
    public native func Tessera_AvatarPorteParPlateforme(entiteHash: Uint32, actif: Bool) -> Bool;
    public native func Tessera_CompteAvatarsJoueurs() -> Int32;
    public native func Tessera_AvatarJoueurParIndex(index: Int32) -> EntityID;

    // Le roster des VÉHICULES serveur. Mêmes règles que la paire ci-dessus.
    //
    // ⛔ Sa raison d'être : `GetEntitiesAroundObject` — la requête spatiale du JEU — ne rend AUCUNE
    // entité née du netcode (mesuré le 2026-08-26 : 128 entités autour du joueur, zéro véhicule et
    // zéro joueur, avec un avatar distant à 4 m et une voiture à 1 m). Toute désignation par
    // proximité est donc aveugle à nos objets ; il faut demander au netcode.
    public native func Tessera_CompteVehiculesReseau() -> Int32;
    public native func Tessera_VehiculeReseauParIndex(index: Int32) -> EntityID;
    /// L'id RÉSEAU du N-ième véhicule, en décimal. Chaîne vide hors bornes.
    /// Sert à distinguer une voiture POSSÉDÉE (ligne en base, donc un coffre) d'une voiture du
    /// MANIFESTE (aucune ligne, donc aucun coffre) — deux objets identiques à l'écran.
    public native func Tessera_VehiculeReseauIdParIndex(index: Int32) -> String;

    // ── L'INVOCATION — le serveur ordonne, le JEU choisit l'emplacement (F-VEH-054) ──────────
    //
    // ⚠️ `Tessera_InvocationSeq` est un COMPTEUR : un joueur sort sa voiture plusieurs fois, et un
    // booléen rendrait le second ordre invisible.
    public native func Tessera_InvocationSeq() -> Int32;
    public native func Tessera_InvocationRecord() -> String;
    public native func Tessera_RapporterInvocation(x: Float, y: Float, z: Float) -> Bool;

    // ── Le sac AUTORITAIRE (ADR 0026) — le serveur énonce, le client s'aligne ────────────────
    //
    // ⚠️ `Tessera_SacRecu` est SÉPARÉ de la taille, et c'est la distinction qui protège les joueurs.
    // Un sac VIDE est un ordre (« tu ne possèdes rien »), l'absence de message n'en est pas un. Les
    // confondre viderait les joueurs de tout serveur qui n'a jamais parlé d'inventaire, puisqu'une
    // taille de 0 se lirait alors comme « retire tout ».
    public native func Tessera_SacRecu() -> Bool;
    /// Le numéro du sac autoritaire courant — change à CHAQUE sac reçu. Permet d'attendre un sac
    /// NEUF après avoir fermé un coffre, au lieu de repartir sur un cache périmé.
    public native func Tessera_SacSeq() -> Int32;
    public native func Tessera_SacTaille() -> Int32;
    public native func Tessera_SacItemId(index: Int32) -> String;
    public native func Tessera_SacItemQuantite(index: Int32) -> Int32;

    // Ce qu'il ne faut JAMAIS retirer — envoyé par le serveur AVEC le sac, parce qu'il contient des
    // pièces du corps du personnage que le serveur ne connaît pas et ne peut donc pas énoncer
    // (F-MND-047 : tête, bras, poings nus, connecteur d'interaction).
    public native func Tessera_PreserverTaille() -> Int32;
    public native func Tessera_PreserverId(index: Int32) -> String;


    public func SpawnTransientEntity(entityName: TweakDBID, worldPosition: Vector4, worldOrientation: Quaternion) -> EntityID {
        let npcSpec = new DynamicEntitySpec();
        //npcSpec.recordID = t"Character.spr_animals_bouncer1_ranged1_omaha_mb";
        npcSpec.recordID = entityName; //t"Character.Panam";
        //npcSpec.appearanceName = n"random"; // TODO

        // Trust the server to properly track the entity state, because otherwise,
        // entities will just disappear for the client and never get back
        // (i.e. being invisible), as the server will only re-spawn them when
        // they have been properly despawned.
        npcSpec.alwaysSpawned = true;
        
        // base\characters\entities\main_npc\panam.ent
        //npcSpec.recordID = t"Vehicle.v_sport2_quadra_type66";
        //npcSpec.appearanceName = n"quadra_type66__basic_bulleat";

        npcSpec.position = worldPosition;
        npcSpec.orientation = worldOrientation;
        npcSpec.persistState = false;
        npcSpec.persistSpawn = false;
        npcSpec.tags = [n"RED4ext"];

        return GameInstance.GetDynamicEntitySystem().CreateEntity(npcSpec);
    }

    // Spawn d'une entité réseau à l'apparence décidée par le SERVEUR.
    //
    // Pourquoi une seconde fonction plutôt que d'élargir SpawnTransientEntity : celle-ci est
    // appelée par le C++ via Red::CallVirtual, donc par NOM et par arité. Changer la signature de
    // l'existante casserait tout client dont le plugin natif et le module redscript ne sont pas
    // exactement de la même version — panne silencieuse et sans diagnostic (le joueur devient un
    // fantôme : les autres le voient, lui ne voit personne). Une fonction neuve échoue proprement.
    //
    // DynamicEntitySpec vient de CODEWARE (dépendance de fondation, ADR 0020) — on ne réimplémente
    // pas le spawn. Champs vérifiés dans la déclaration livrée
    // (red4ext/plugins/Codeware/Scripts/Codeware.Global.reds), pas devinés.
    public func SpawnNetworkAvatar(entityName: TweakDBID, appearanceName: CName, worldPosition: Vector4, worldOrientation: Quaternion) -> EntityID {
        let spec = new DynamicEntitySpec();
        spec.recordID = entityName;

        // Une CName vide (hash 0) n'est PAS « pas d'apparence » pour le moteur : c'est une
        // apparence introuvable. On ne pose le champ que s'il est valide, et on laisse sinon le
        // record choisir son apparence par défaut. Le moteur rejette de toute façon en silence une
        // apparence étrangère au jeu d'apparences de l'entité (F-PNJ-051) — donc un nom invalide
        // ne planterait pas, il donnerait un PNJ à l'apparence inattendue, ce qui est pire à
        // diagnostiquer qu'une absence.
        if IsNameValid(appearanceName) {
            spec.appearanceName = appearanceName;
        }

        // Trust the server to properly track the entity state, because otherwise,
        // entities will just disappear for the client and never get back
        // (i.e. being invisible), as the server will only re-spawn them when
        // they have been properly despawned.
        spec.alwaysSpawned = true;
        spec.position = worldPosition;
        spec.orientation = worldOrientation;
        // Aucune persistance : le serveur est la seule mémoire de la session (aucune save locale).
        spec.persistState = false;
        spec.persistSpawn = false;
        spec.tags = [n"RED4ext"];

        return GameInstance.GetDynamicEntitySystem().CreateEntity(spec);
    }

    // État de locomotion du joueur LOCAL, empaqueté pour le protocole :
    //   bits 0-7   locomotion — 0 Idle · 1 Walk · 2 Run · 3 Sprint · 4 CrouchIdle · 5 CrouchMove · 6 InAir
    //   bits 8-15  move_dir   — direction du déplacement RELATIVE au regard, 0-255 = 0-360° ; 0 si immobile
    //
    // Le client envoyait ces deux champs à 0 EN DUR : les avatars distants glissaient sans jamais
    // s'animer, alors que le protocole les porte depuis le gel du palier 2 et que le serveur les
    // relaie déjà dans PlayerState.
    //
    // ⚠️ La logique ci-dessous n'est PAS déduite du RE — elle est MESURÉE en jeu (2026-07-23,
    // sonde `loco_read` du harnais, table complète des 8 états). Deux corrections que seule la
    // mesure a données, et qu'il ne faut pas « re-simplifier » :
    //   · `LocomotionDetailed` est AMBIGU (3 = marche ET accroupi-immobile ; 1 = debout ET course).
    //     Ne jamais piloter dessus seul.
    //   · le saut donne Locomotion = 5, pas 4 — le RE annonçait 4=Jump/5=Vault, faux en 2.31.
    // L'ordre des tests compte : `IsOnGround` d'abord, c'est le signal le plus fiable.
    public func ReadLocomotionPacked() -> Int32 {
        let player = GameInstance.GetPlayerSystem(GetGameInstance()).GetLocalPlayerControlledGameObject();
        if !IsDefined(player) {
            return 0;
        }
        let bb = GameInstance.GetBlackboardSystem(GetGameInstance())
            .GetLocalInstanced(player.GetEntityID(), GetAllBlackboardDefs().PlayerStateMachine);
        if !IsDefined(bb) {
            return 0;
        }

        let loco = bb.GetInt(GetAllBlackboardDefs().PlayerStateMachine.Locomotion);
        let detailed = bb.GetInt(GetAllBlackboardDefs().PlayerStateMachine.LocomotionDetailed);
        let onGround = bb.GetBool(GetAllBlackboardDefs().PlayerStateMachine.IsOnGround);
        let movingH = bb.GetBool(GetAllBlackboardDefs().PlayerStateMachine.IsMovingHorizontally);

        // `GetVelocity` n'est PAS sur GameObject (vérifié : [UNRESOLVED_METHOD] à la compilation) —
        // elle vit sur `gamePuppet`, que les scripts CDPR atteignent toujours par un cast explicite
        // (`((gamePuppet)(target)).GetVelocity()`). Le joueur local est un PlayerPuppet, qui en
        // hérite. Un cast raté rend `null` : on retombe alors sur l'état sans vitesse plutôt que de
        // planter tout r6/scripts.
        let puppet = player as PlayerPuppet;
        if !IsDefined(puppet) {
            return 0;
        }
        let velocity = puppet.GetVelocity();
        let speed = Vector4.Length(velocity);

        // ── ⭐⭐⭐ L'ALLURE SE JUGE A LA VITESSE HORIZONTALE, JAMAIS A LA VITESSE 3D ──────────
        //
        // ⚠️ LE DEFAUT, ET IL TENAIT DANS `Vector4.Length`. Un joueur DEBOUT dans une cabine qui
        // descend a 6 m/s a une vitesse MONDE de 6 m/s. Le repli `speed > 0.5` le classait donc
        // `state = 2` — EN COURSE — alors qu'il ne bouge pas d'un pouce.
        //
        // C'est la moitie manquante de F-ASC-047 (« un passager est classe EN L'AIR ou EN
        // COURSE, jamais immobile ») : la branche EN L'AIR avait ete traitee par
        // `porteParCabine`, la branche EN COURSE etait restee, et c'est elle qui produisait les
        // symptomes signales par Lucas pendant deux jours — cycle de course joue sur un corps
        // immobile (« il marche sur une poutre »), genoux qui remontent, tremblement.
        //
        // ⭐ ET LE TREMBLEMENT VIENT DE LA MEME LIGNE, par `moveDir`. Sa garde etait `speed > 0.1`,
        // donc toujours vraie dans une cabine en mouvement — alors que l'angle, lui, se calcule
        // sur les seules composantes HORIZONTALES. On calculait donc une direction de deplacement
        // a partir de bruit horizontal, renouvelee a chaque image. D'ou une direction qui saute.
        //
        // ⭐ La correction est plus JUSTE en general, pas seulement pour les ascenseurs : on ne
        // court pas parce qu'on se deplace verticalement. Une chute reelle reste couverte en
        // amont par `!onGround` (state 6), qui est teste avant et reste prioritaire.
        //
        // ⚠️ On garde `speed` (3D) sous la main : il n'est plus utilise pour l'allure, mais le
        // supprimer masquerait qu'un choix a ete fait ici.
        let vitesseH: Float = SqrtF(velocity.X * velocity.X + velocity.Y * velocity.Y);

        // ── UN PASSAGER D'ASCENSEUR N'EST PAS EN TRAIN DE TOMBER ───────────────────────────
        //
        // ⭐ MESURE DU 2026-08-27 (F-ASC-047) : un joueur DEBOUT dans une cabine en mouvement
        // rapporte `locomotion = 6` — EN L'AIR. Le blackboard dit « pas au sol » parce que le
        // plancher bouge sous ses pieds, et cette ligne en fait un signal prioritaire.
        //
        // ⚠️ CE QUE CA PRODUIT CHEZ LES AUTRES, et c'est le defaut que Lucas signale depuis des
        // heures : l'avatar distant joue une animation de CHUTE — jambes repliees, genoux plies,
        // torse droit, pieds a cinquante centimetres du sol. « Le haut du corps ne bouge pas »
        // parce qu'un corps en chute ne bouge que les jambes.
        //
        // Le moteur lui-meme connait ce cas : `IsOnMovingPlatform()` sert precisement a SUPPRIMER
        // la chute dans `LocomotionAirDecisions.ShouldFall`. Mais il ne l'expose qu'a la machine a
        // etats du joueur. On utilise donc ce que le module ascenseurs sait deja : quelle cabine
        // porte le joueur local.
        //
        // ⚠️ On ne force PAS l'immobilite — seulement on cesse de crier « en l'air ». Un passager
        // qui marche dans la cabine sera classe par sa vitesse, comme au sol.
        // ⚠️ Le drapeau est POSE par le module ascenseurs, il n'est pas demande a lui : le coeur
        // reseau ne doit pas dependre d'un module optionnel. Absent le mod, il vaut false et le
        // comportement est celui d'avant — jamais pire.
        let porteParCabine: Bool = this.Tessera_JoueurLocalPorte();

        let state: Int32;
        if !onGround && !porteParCabine {
            state = 6;                                  // InAir/Jump — signal prioritaire
        } else if loco == 2 {
            state = 3;                                  // Sprint
        } else if loco == 1 {
            state = movingH ? 5 : 4;                    // CrouchMove / CrouchIdle
        } else if detailed == 3 {
            state = 1;                                  // Walk (seul signal fiable de la marche)
        } else if vitesseH > 0.5 {
            state = 2;                                  // Run (repli par la vitesse HORIZONTALE)
        } else {
            state = 0;                                  // Idle
        }

        // move_dir : angle SIGNÉ entre le regard et la vélocité horizontale.
        //
        // La sonde mesurait `Vector4.GetAngleBetween`, qui rend un angle NON SIGNÉ (0-180°) : il ne
        // distingue pas la gauche de la droite, donc un strafe gauche et un strafe droit sortiraient
        // identiques et l'AnimGraph choisirait la mauvaise animation une fois sur deux. D'où atan2
        // sur les projections avant/droite, qui couvre les 360°.
        //
        // Sous 0.1 m/s la vélocité est du bruit et l'angle ne veut rien dire — mesuré : à l'arrêt,
        // l'angle valait 90° sur une vélocité nulle.
        let moveDir: Int32 = 0;
        if vitesseH > 0.1 {
            let forward = player.GetWorldForward();
            let right = player.GetWorldRight();
            // Dot2D (X,Y) et non Dot : on veut la direction dans le PLAN horizontal. Avec Dot, une
            // vitesse verticale (chute, saut) contaminerait la direction de déplacement.
            let degrees = Rad2Deg(AtanF(Vector4.Dot2D(velocity, right), Vector4.Dot2D(velocity, forward)));
            if degrees < 0.0 {
                degrees += 360.0;
            }
            moveDir = Cast<Int32>(degrees * 256.0 / 360.0) % 256;
        }

        // Empaquetage ARITHMÉTIQUE et non binaire : redscript n'a ni `<<` ni `|` (erreur de syntaxe
        // à la compilation, vérifié). Équivalent ici, `state` valant au plus 6 donc bien < 256.
        return moveDir * 256 + state;
    }

    // ── LE REGARD, distinct de l'orientation du CORPS ──────────────────────────────────────────
    //
    // `lookState.lookDir` de `gameMuppetState` — le seul champ de la checklist multijoueur de CDPR
    // qui n'existait nulle part chez nous (spec 2026-08-15 §2). Un avatar qui fixe droit devant
    // pendant que le joueur regarde ailleurs ne trompe personne.
    //
    // Source : `gameCameraSystem.GetActiveCameraForward() -> Vector4` (dump RTTI, vérifié). La
    // caméra ACTIVE et non la caméra FPP : en troisième personne (chantier en cours) c'est encore
    // elle qui dit où le joueur regarde, et l'appel ne change pas.
    //
    // ⚠️ **NON MESURÉ — HYPOTHÈSE** (doctrine D2). Le dump RTTI fait foi sur la SIGNATURE, jamais
    // sur l'effet : il dit que la méthode existe et rend un Vector4, pas que ce vecteur est
    // normalisé, ni dans quelle convention d'axes. Deux inconnues concrètes :
    //   · la CONVENTION DE YAW (0° = +Y nord ? sens horaire ?) — si elle diffère de celle du corps,
    //     le regard partira à angle droit ou en miroir ;
    //   · la NORMALISATION — si le vecteur ne l'est pas, `AsinF(Z)` rend n'importe quoi.
    //
    // **La sonde qui tranche, et elle est gratuite** : `ReadLookYaw` doit valoir le yaw du CORPS,
    // à quelques degrés près, quand le joueur se tient immobile et regarde droit devant. C'est
    // exactement ce que compare la sonde `regard` du harnais — un écart constant révèle la
    // convention, un écart aléatoire révèle la normalisation. Coût : une commande, aucune session
    // dédiée.
    //
    // Le repli est SÛR : (0, 0) = « aucun regard rapporté », et le consommateur retombe alors sur
    // le yaw du corps, tête à l'horizontale (cf. `PositionUpdate` dans protocol.fbs). Un appel qui
    // échoue dégrade donc vers le comportement actuel, jamais vers un regard faux.
    //
    // Deux lecteurs plutôt qu'un entier empaqueté : redscript n'a ni `<<` ni `|` (cf.
    // `ReadLocomotionPacked`), et deux valeurs 16 bits ne tiennent pas dans un Int32 par
    // multiplication sans déborder. La quantization reste côté C++, où `QuantYaw` vit déjà —
    // un seul module porte les constantes du fil (quant.rs et son miroir).
    private func LookForward() -> Vector4 {
        let cam = GameInstance.GetCameraSystem(GetGameInstance());
        if !IsDefined(cam) {
            return new Vector4(0.0, 0.0, 0.0, 0.0);
        }
        return cam.GetActiveCameraForward();
    }

    /// Yaw du REGARD en degrés [0, 360). 0 si indisponible → le serveur retombe sur le yaw du corps.
    public func ReadLookYaw() -> Float {
        let f = this.LookForward();
        // Plan horizontal uniquement : un regard vers le sol ne doit pas tordre le yaw.
        if AbsF(f.X) < 0.0001 && AbsF(f.Y) < 0.0001 {
            return 0.0;
        }
        // Même forme qu'`AtanF(droite, avant)` dans ReadLocomotionPacked — X = est, Y = nord.
        let degrees = Rad2Deg(AtanF(f.X, f.Y));
        if degrees < 0.0 {
            degrees += 360.0;
        }
        return degrees;
    }

    /// Pitch du REGARD en degrés, POSITIF vers le haut. Indépendant de la convention de yaw : sur
    /// un vecteur normalisé, Z est l'élévation quel que soit le sens des axes horizontaux.
    public func ReadLookPitch() -> Float {
        let f = this.LookForward();
        let longueur = Vector4.Length(f);
        if longueur < 0.0001 {
            return 0.0;
        }
        return Rad2Deg(AsinF(ClampF(f.Z / longueur, -1.0, 1.0)));
    }

// ── LE REGARD D'UN AVATAR DISTANT ────────────────────────────────────────────────────────
    //
    // `lookDir` de `gameMuppetState` traverse le fil depuis le 2026-08-16 ; il n'avait pas de
    // consommateur. Le voici.
    //
    // POURQUOI CETTE VOIE ET PAS UNE AUTRE. `AIActionLookat` est le chemin que le moteur emploie
    // LUI-MEME pour les PNJ du solo (F-PLY-057) : on ne dispute pas le pantin, on lui parle dans sa
    // langue. C'est la seule piece du chantier qui n'ait jamais eu besoin qu'on neutralise quoi que
    // ce soit.
    //
    // La recette est celle du jeu VIVANT — `ActivateReactionLookAt` (`reactionComponent.script:4648`)
    // — et non celle du code CPO orphelin : `bodyPart = 'Eyes'`, part `Head` poids 0.1, part `Chest`
    // poids 2.0, limites en dur, AUCUN record TweakDB. C'est ce qui rend la question « les records
    // LookatPreset existent-ils encore ? » sans objet (addendum F-PLY-057, correction 2).
    //
    // ⚠️ CIBLE STATIQUE, ET RE-POSEE SUR CHANGEMENT SEULEMENT. Aucun fournisseur de position
    // scriptable n'existe (`IPositionProvider` est `importonly`, correction 3) : pour une cible
    // mobile il faut retirer puis re-poser. A 25 Hz la tete resterait perpetuellement en transition
    // — d'ou le seuil de 8 degres, qui ne re-pose que sur un mouvement de regard reel.
    //
    // ⚠️ PRIORITE POSEE EXPLICITEMENT (correction 4). Personne ne la pose dans tout le jeu, et un
    // pantin de foule porte un `reactionComponent` que le client LOCAL stimule : deux LookAt
    // concurrents a priorite egale seraient arbitres par du C++ illisible, et l'avatar pourrait
    // fixer le joueur local sur decision locale — un regard que le joueur distant n'a jamais eu.
    // Tableaux paralleles : redscript n'a pas de table associative. Indexes par le hash de
    // l'EntityID, quelques dizaines d'entrees au plus.
    private let m_regardCles: array<Uint32>;
    private let m_regardYaw: array<Float>;
    private let m_regardPitch: array<Float>;
    private let m_regardEvent: array<ref<LookAtAddEvent>>;
    // Compte les lignes de trace du pointage deja ecrites — s eteint a 20 (voir plus bas).
    // ⚠️ SANS INITIALISEUR, comme ses quatre voisins. Un `= 0` ici etait le SEUL initialiseur
    // de champ du fichier, et il coincide avec la disparition de l'emetteur d'arme
    // (F-PLF-049). `Int32` vaut zero par defaut : l'initialiseur n'apportait rien.
    private let m_pointageTrace: Int32;

    public func TesseraPousserRegard(entityId: EntityID, lookYaw: Float, lookPitch: Float) -> Bool {
        let ent = GameInstance.FindEntityByID(GetGameInstance(), entityId);
        let puppet = ent as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }

        // (0,0) = aucun regard rapporte (client anterieur au champ) : on ne pose rien, l'avatar
        // garde son comportement d'avant. Degrader vers l'existant, jamais vers un regard faux.
        if AbsF(lookYaw) < 0.001 && AbsF(lookPitch) < 0.001 {
            return false;
        }

        let cle = EntityID.GetHash(entityId);
        let i = 0;
        let trouve = -1;
        while i < ArraySize(this.m_regardCles) {
            if Equals(this.m_regardCles[i], cle) { trouve = i; }
            i += 1;
        }
        if trouve >= 0 {
            if AbsF(this.EcartAngulaireDeg(this.m_regardYaw[trouve], lookYaw)) < 8.0
               && AbsF(this.m_regardPitch[trouve] - lookPitch) < 8.0 {
                return true;   // rien n'a bouge : on laisse la tete finir sa transition
            }
            if IsDefined(this.m_regardEvent[trouve]) {
                LookAtRemoveEvent.QueueRemoveLookatEvent(puppet, this.m_regardEvent[trouve]);
            }
        }

        // Le point vise : 12 m devant, dans la direction du regard. Assez loin pour que l'angle
        // domine la distance, assez pres pour rester dans le monde charge.
        let origine = puppet.GetWorldPosition();
        let cosP = CosF(Deg2Rad(lookPitch));
        let cible = new Vector4(
            origine.X + SinF(Deg2Rad(lookYaw)) * 12.0 * cosP,
            origine.Y + CosF(Deg2Rad(lookYaw)) * 12.0 * cosP,
            origine.Z + 1.6 + SinF(Deg2Rad(lookPitch)) * 12.0,   // +1.6 : hauteur des yeux
            1.0);

        let ev = new LookAtAddEvent();
        ev.SetStaticTarget(cible);
        ev.SetStyle(animLookAtStyle.Normal);

        // ── ⭐ LE POINTAGE : LE BUSTE SUIT LA VISÉE, MAIS SEULEMENT ARME EN MAIN ──────────────
        //
        // Lucas, 2026-08-25 : *« on voit l'arme en main. Par contre, quand on pointe, ça ne
        // fonctionne pas. »* L'arme est tenue (F-PLY-314) ; l'avatar ne l'oriente pas.
        //
        // ⭐ Pourquoi par ici et pas par une feature. Le graphe déclare `NonCombatAim` et
        // `ShootAction`, mais ce sont des features — et F-PLY-138 a mesuré qu'un pantin piloté de
        // l'extérieur ne consomme pas les écritures d'état. Le regard, LUI, atteint déjà l'avatar
        // (F-PLY-110). Le même événement sait orienter le buste ; une arme tenue en main le suit.
        //
        // ⚠️ ON N'AJOUTE QUE `Chest`, ET SURTOUT PAS LA RECETTE DE RÉACTION DE CDPR. Celle-ci
        // (`reactionComponent.script:4657`) met `Head` à `weight 0.1` / `suppress 1.0` : elle
        // **supprime la tête** pour ne tourner que le buste — le coup d'œil d'un passant. C'est
        // précisément l'erreur d'août qui a produit « la tête ne tourne pas ». Ici on veut les
        // deux : la tête regarde, le buste s'oriente.
        //
        // ⚠️ `weight = 2.0` est le poids que CDPR donne au buste dans sa propre recette — repris
        // tel quel plutôt qu'inventé. `suppress = 0.0` : on n'enlève rien.
        //
        // ⚠️ ARME EN MAIN SEULEMENT. Au repos, un buste qui pivote avec le regard donnerait une
        // posture de combat permanente à tous les avatars du serveur.
        //
        // ⚠️ TESTÉ LE 2026-08-26, SANS EFFET VISIBLE (F-PLY-325) — Lucas, arme dégainée et hors
        // de l'appartement : « quand on vise, on voit pas la différence ». Le verdict VISUEL
        // reste humain (consigne : « regarde le BUSTE de l'avatar d'en face pendant qu'il vise
        // sur le côté »), mais la trace ci-dessous dit désormais si le code s'exécute.
        // ⚠️ LA TRACE EST OBLIGATOIRE ICI, et son absence a coute un test entier (2026-08-26).
        // Sans elle, « on voit pas la difference » ne distingue pas « la partie Chest est inerte »
        // de « la condition ne s est jamais franchie ». Deux causes, deux suites opposees.
        //
        // ⚠️ ON NE JOURNALISE QUE ARME EN MAIN, et c'est une correction du 2026-08-27. La version
        // precedente tracait les douze PREMIERES pousses : les douze sont parties sur des avatars
        // au repos, et le compteur etait vide avant que la moindre arme n'apparaisse. Un
        // instrument qui depense sa reserve sur le cas ennuyeux ne mesure pas l'evenement.
        //
        // ⭐ Le silence devient alors un verdict : aucune ligne = la condition ne se franchit
        // jamais ; des lignes = elle se franchit, et l'effet visuel est a chercher ailleurs.
        let armeEnMain = this.Tessera_ArmeDeLEntite(entityId);
        if armeEnMain != TDBID.None() && this.m_pointageTrace < 20 {
            this.m_pointageTrace += 1;
            this.Tessera_Journal(
                s"[Pointage] \(cle) arme=\(TDBID.ToStringDEBUG(armeEnMain)) yaw=\(Cast<Int32>(lookYaw))"
                + " buste=" + (armeEnMain != TDBID.None() ? "AJOUTE" : "non"));
        }
        if armeEnMain != TDBID.None() {
            // ⚠️ CORRIGÉ le 2026-08-26 (chantier appareils) — le fichier ne compilait pas, et
            // faisait donc tomber TOUT `r6/scripts`. Deux défauts sur la même ligne :
            //   · le nom `animLookAtPartRequest` est celui du dump RTTI ; le nom SCRIPTÉ est
            //     `LookAtPartRequest` (`lookAtEvents.script:152`, `reactionComponent.script:4628`).
            //     C'est le piège nommé en tête de la skill `tessera-client-mod-redscript` ;
            //   · c'est une STRUCT, pas une classe — elle se déclare, elle ne se `new` pas. Le
            //     témoin qui marche est `reactionComponent.script:4628`, qui pose exactement ces
            //     quatre champs de la même façon.
            let buste: LookAtPartRequest;
            buste.partName = n"Chest";
            buste.weight = 2.0;
            buste.suppress = 0.0;
            buste.mode = 0;
            let parties: array<LookAtPartRequest>;
            ArrayPush(parties, buste);
            ev.SetAdditionalPartsArray(parties);
        }
        ev.request.limits.softLimitDegrees = 360.0;
        ev.request.limits.hardLimitDegrees = 270.0;
        ev.request.limits.backLimitDegrees = 210.0;
        ev.request.calculatePositionInParentSpace = false;   // cible en espace MONDE
        ev.request.priority = 100;                            // au-dessus des reactions locales
        // ── ON AVAIT COPIÉ LA MAUVAISE RECETTE, ET ELLE SUPPRIME LA TÊTE EXPRÈS ───────────────
        //
        // La version précédente reprenait `ActivateReactionLookAt`
        // (`reactionComponent.script:4650-4672`) : `bodyPart = 'Eyes'`, puis deux parties
        // additionnelles — **`Head` avec `weight = 0.1` et `suppress = 1.0`**, et `Chest` avec
        // `weight = 2.0`.
        //
        // C'est la recette d'un COUP D'ŒIL DE RÉACTION : un passant qui remarque quelque chose
        // sans cesser de marcher. CDPR y **supprime délibérément la tête** et fait pivoter le
        // buste à la place. Appliquée à l'avatar d'un joueur, elle produit exactement ce que Lucas
        // a rapporté le 2026-08-16 : « la tête ne tourne pas quand on tourne la souris ».
        //
        // ⚠️ Et cette recette-là vise une ENTITÉ (`SetEntityTarget(targetEntity,
        // 'pla_default_tgt', …)`), pas un point du monde : ses réglages de parties sont accordés à
        // ce cas-là. Nous visons un point statique — ce n'était donc pas seulement le mauvais
        // dosage, c'était le mauvais patron.
        //
        // LA BONNE RÉFÉRENCE est la seule invocation CDPR à cible statique :
        // `AIGenericStaticLookatTask.ActivateLookat` (`ai/Tasks/aiLookats.script:269-291`). Elle
        // pose la cible, les limites, le style — **et rien d'autre**. Ni `bodyPart`, ni parties
        // additionnelles. Sans partie suppressive, le moteur applique son look-at standard, celui
        // qui tourne la tête.
        //
        // On garde `priority = 100` : c'est un besoin propre au multijoueur (passer devant les
        // réactions locales du pantin, qui lui feraient sinon regarder ailleurs), et il est absent
        // chez CDPR parce que leur tâche EST la réaction locale.
        //
        // ⚠️ NON MESURÉ. L'effet d'un regard ne se lit sur aucun compteur, il se voit. Consigne
        // d'observation : « regarde la TÊTE de l'avatar d'en face pendant que je tourne la souris
        // sans bouger le corps » — le seul geste qui sépare `lookDir` du yaw du corps.
        puppet.QueueEvent(ev);

        if trouve >= 0 {
            this.m_regardYaw[trouve] = lookYaw;
            this.m_regardPitch[trouve] = lookPitch;
            this.m_regardEvent[trouve] = ev;
        } else {
            ArrayPush(this.m_regardCles, cle);
            ArrayPush(this.m_regardYaw, lookYaw);
            ArrayPush(this.m_regardPitch, lookPitch);
            ArrayPush(this.m_regardEvent, ev);
        }
        return true;
    }

    private func EcartAngulaireDeg(de: Float, vers: Float) -> Float {
        let d = (vers - de) % 360.0;
        if d > 180.0 { d -= 360.0; }
        if d < -180.0 { d += 360.0; }
        return d;
    }

    // Applique la météo décidée par le SERVEUR (`WorldState.weather`).
    //
    // ✅ MESURÉ le 2026-08-04 (F-MND-043, sonde `weather_probe`) : `SetWeather` existe et agit
    // dans les deux sens — intensité de pluie 0 → 1 puis 1 → 0, chaque appel suivi de son effet.
    // Ce code n'a PAS été écrit avant cette mesure, précisément parce que le setter est absent du
    // dump RTTI et de la classe `WeatherSystem` des scripts décompilés : seul un test en jeu
    // pouvait dire s'il existait (F-SCR-018 — le dump ne couvre pas 100 % du natif).
    //
    // Renvoie false quand le preset est DÉJÀ appliqué — comportement observé, pas supposé. Ce
    // n'est donc pas une erreur, et il ne faut ni la journaliser en boucle ni réessayer.
    public func ApplyServerWeather(preset: String) -> Bool {
        if StrLen(preset) == 0 {
            return false;
        }
        let ws = GameInstance.GetWeatherSystem(GetGameInstance());
        if !IsDefined(ws) {
            return false;
        }
        // 3 s de transition : assez pour que le ciel ne saute pas, assez court pour qu'un joueur
        // qui vient d'arriver voie la météo du serveur presque tout de suite.
        return ws.SetWeather(StringToName(preset), 3.00, 0u);
    }

    // Applique l'heure décidée par le SERVEUR (`WorldState.hour`/`minute`).
    //
    // Pourquoi ici et pas en C++ : la première version appelait
    // `Red::CallStatic("ScriptGameInstance", "GetTimeSystem", …)` depuis le plugin, et le natif
    // n'était JAMAIS résolu — « TimeSystem introuvable » à chaque message, mesuré en jeu le
    // 2026-08-04. La météo, elle, marchait du premier coup… parce qu'elle passait par redscript.
    // Deux voies pour le même besoin, une seule qui résout : on garde celle qui est prouvée.
    //
    // `SetGameTimeByHMS(Int32, Int32, Int32, opt CName)` — signature lue dans les scripts
    // décompilés CDPR (scripts/core/systems/timeSystem.script:15).
    // `toleranceMinutes` : écart au-delà duquel on RÉÉCRIT l'heure du moteur. Comparé à l'heure
    // LOCALE courante, pas au dernier ordre serveur.
    //
    // ⚠️ La première version comparait au dernier temps SERVEUR appliqué. Elle ne gardait donc
    // rien : le serveur avance de 2 minutes de jeu entre deux diffusions et le seuil valait 2
    // minutes — la condition n'était jamais vraie, et l'heure était réécrite à CHAQUE message.
    // Sans effet visible, mais un garde qui ne garde rien est pire qu'aucun garde : il fait croire
    // qu'un problème est traité.
    //
    // Le bon référent est l'horloge du moteur, parce que c'est elle qui dérive. Tant qu'elle suit
    // le serveur d'assez près, on ne touche à rien et le cycle jour/nuit reste fluide ; dès
    // qu'elle décroche, on corrige d'un coup.
    // Renvoie la TAILLE du saut appliqué, en minutes de jeu (0 = rien à corriger). C'était un
    // `Bool` : « corrigé ou non » ne dit pas si le joueur a vu quelque chose, et c'est pourtant la
    // seule question qui compte pour l'heure — un saut de 4 minutes ne se voit pas, un saut d'une
    // heure fait basculer le ciel. Mesurer la taille du saut côté SERVEUR est impossible sans
    // repliement : le rapport de dérive arrive toutes les 5 s et les corrections tombent toutes
    // les ~4 s, donc l'échantillonnage bat contre la correction et rend une enveloppe fausse
    // (constaté le 2026-08-08). Ici, la valeur est exacte par construction.
    public func ApplyServerTime(hours: Int32, minutes: Int32, toleranceMinutes: Int32) -> Int32 {
        let ts = GameInstance.GetTimeSystem(GetGameInstance());
        if !IsDefined(ts) {
            return 0;
        }

        let now = ts.GetGameTime();
        let localMinutes = GameTime.Hours(now) * 60 + GameTime.Minutes(now);
        let serverMinutes = hours * 60 + minutes;

        // Distance CIRCULAIRE sur 24 h : 23h59 → 00h01 vaut 2 minutes, pas 1438. Sans ça, chaque
        // passage de minuit déclencherait une correction inutile.
        let delta = serverMinutes - localMinutes;
        if delta > 720 {
            delta -= 1440;
        }
        if delta < -720 {
            delta += 1440;
        }
        if delta < 0 {
            delta = -delta;
        }
        if delta < toleranceMinutes {
            return 0;
        }

        ts.SetGameTimeByHMS(hours, minutes, 0);
        return delta;
    }

    // Heure LOCALE observée, en secondes depuis minuit — l'autre moitié de l'horloge partagée.
    // Le C++ l'empaquette en `ClientTimeReport` et l'envoie ; le serveur compare à son horloge
    // autoritaire et journalise l'écart (`world_clock.rs`). Diagnostic pur : rien ici ne corrige
    // quoi que ce soit, la correction descend par `ApplyServerTime` ci-dessus.
    //
    // Pourquoi ici et pas en C++ : même raison que `ApplyServerTime` — `GetTimeSystem` ne se
    // résout pas depuis le plugin (mesuré en jeu le 2026-08-04).
    //
    // `-1` = horloge non lisible (menu principal, pas encore en partie). Le C++ n'envoie alors
    // rien : un zéro rapporté comme « il est minuit » ferait crier le diagnostic serveur à tort.
    //
    // Getters d'INSTANCE `Hours()/Minutes()/Seconds()` (pluriel) : les formes singulières
    // `Hour()/Minute()` sont STATIQUES et renvoient un GameTime (constructeurs d'unité), et
    // `Sec()` n'existe pas — vérifié au dump RTTI, c'est ce qui avait produit un
    // [INVALID_STATIC_USE] ailleurs dans le dépôt.
    public func ReadLocalGameSeconds() -> Int32 {
        let ts = GameInstance.GetTimeSystem(GetGameInstance());
        if !IsDefined(ts) {
            return -1;
        }
        let now = ts.GetGameTime();
        return now.Hours() * 3600 + now.Minutes() * 60 + now.Seconds();
    }

    // Écrit une valeur de TweakDB décidée par le SERVEUR, en cours de partie.
    //
    // C'est la sonde S-E5, écrite comme du code de production plutôt que comme un jetable : la
    // question « un opérateur peut-il changer un prix sans redémarrer la session ? » se tranche par
    // le même appel que celui qui servira ensuite.
    //
    // ⚠️ COUCHE. Une première tentative depuis le Lua CET a échoué (`expected userdata`,
    // F-SCR-023, `impasse`) — mais c'est une couche de DÉVELOPPEMENT, jamais livrée. La voie de
    // production est ici : `TweakDBManager` de TweakXL, dépendance de FONDATION présente chez tout
    // joueur (ADR 0020). Ne pas relire F-SCR-023 comme « l'écriture à chaud est impossible ».
    //
    // `UpdateRecord` est ce qui distingue « la base a changé » de « le jeu a vu le changement » :
    // les systèmes qui ont mis TweakDB en cache au boot ne relisent pas d'eux-mêmes. Sans lui, un
    // SetFlat réussi peut rester parfaitement invisible en jeu — exactement le genre de succès
    // trompeur que la doctrine D1 interdit de compter comme un effet.
    public func ApplyServerConfig(flat: String, value: Float) -> Bool {
        if StrLen(flat) == 0 {
            return false;
        }
        // ── L'ESPACE DE NOMS `Tessera.` : UNE RÈGLE DU SERVEUR, PAS UN FLAT TWEAKDB ──────────
        //
        // Ajouté le 2026-08-26 pour donner à l'opérateur un interrupteur SERVEUR sur des règles
        // de jeu qui ne sont pas des valeurs TweakDB — la première étant l'hostilité des PNJ.
        //
        // ⚠️ POURQUOI RÉUTILISER CE CANAL plutôt qu'en ouvrir un. `ConfigSync` transporte déjà un
        // couple (chemin, flottant) du serveur vers chaque client, à l'entrée en session et à
        // chaque changement de configuration, et il est MESURÉ (F-PLF-018). Une règle n'a besoin
        // de rien de plus. Ouvrir un second canal aurait coûté un message de protocole, une
        // régénération de l'en-tête C++ du fork et une passe de la porte GNS — pour transporter
        // exactement la même chose.
        //
        // ⚠️ Le préfixe est une CONVENTION entre `config-overrides.toml` et ce fichier, et rien
        // ne la vérifie. Le filet est le journal : un `Tessera.X` que personne n'interprète tombe
        // dans le `SetFlat` ci-dessous, échoue, et se voit dans « ConfigSync : « X » refuse par
        // TweakDB ». À lire après tout ajout de règle.
        if StrBeginsWith(flat, "Tessera.") {
            let regles = TesseraReglesServeur.Get(GetGameInstance());
            if IsDefined(regles) {
                return regles.Poser(flat, value);
            }
            return false;
        }
        if !TweakDBManager.SetFlat(TDBID.Create(flat), ToVariant(value)) {
            return false;
        }
        // Le record est le chemin privé de son dernier segment : `Price.GoodQualityDrink.value`
        // → `Price.GoodQualityDrink`. Un flat sans point n'a pas de record parent : on a écrit,
        // mais rien à rafraîchir.
        let cut = StrFindLast(flat, ".");
        if cut > 0 {
            TweakDBManager.UpdateRecord(TDBID.Create(StrLeft(flat, cut)));
        }
        return true;
    }

    // Rejoue sur LA FOULE LOCALE un stimulus produit par un joueur distant.
    //
    // C'est le mécanisme central de l'ADR 0022 : on réplique l'ÉVÉNEMENT, jamais ses conséquences.
    // Le serveur envoie un message ; chaque client fait fuir SES propres passants, avec le système
    // de réaction natif. Tout le monde voit la même rue se vider au même instant — les individus
    // diffèrent, la scène est la même. Un message au lieu de mille positions de fuyants.
    //
    // `nature` est l'ordinal de `gamedataStimType` dans le jeu (v2.31 épinglée), pas une
    // numérotation maison. Les 67 valeurs : docs/connaissances/catalogue-stimulus.md.
    //
    // ⚠️ Un ordinal hors plage donnerait un enum invalide, que le natif accepterait sans rien
    // faire — exactement le « succès trompeur » que D1 interdit de compter comme un effet. D'où le
    // garde-fou explicite plutôt qu'une confiance dans l'émetteur.
    //
    // Mesuré : `BroadcastStim` fait bien paniquer la foule sans qu'aucun coup de feu ne parte
    // (F-PNJ-104, arme rangée). Ce qui n'est PAS mesuré, c'est l'effet des 66 autres types — voir
    // la colonne « effet mesuré » du catalogue avant d'affirmer quoi que ce soit sur l'un d'eux.
    public func ApplyServerStim(actor: EntityID, nature: Uint32, radius: Float) -> Bool {
        if nature > 66u {
            return false;
        }
        let emitter = GameInstance.FindEntityByID(GetGameInstance(), actor) as GameObject;
        if !IsDefined(emitter) {
            return false;
        }
        StimBroadcasterComponent.BroadcastStim(emitter, IntEnum<gamedataStimType>(Cast<Int32>(nature)), radius);
        return true;
    }

    // Met un PNJ répliqué dans l'état MORT, sur ordre du serveur (`NpcState.behavior = ATerre`).
    //
    // ⚠️ MÉTHODE DE CLASSE, pas fonction de module. Le C++ l'appelle par
    // `Red::CallVirtual(this, "TesseraRendreMort", ...)`, qui cherche une méthode sur la classe de
    // l'objet. Déclarée au niveau module, l'appel échouait en silence : 2 500 tentatives sans qu'une
    // seule ligne ne s'exécute (`appel=echec`, mesuré le 2026-08-08).
    //
    // ⚠️ `skipNPCDeathAnim = false` — ET C'EST L'ANIMATION QUI COUCHE LE CORPS. Une première version
    // la sautait, pour éviter de rejouer une agonie sur un personnage déjà mort : le pantin passait
    // bien à l'état mort mais restait PLANTÉ DEBOUT en idle. On préfère une seconde d'animation à un
    // cadavre vertical. `disableNPCRagdoll = false` pour la même raison : le ragdoll pose le corps.
    //
    // ⚠️ Le succès se mesure sur `IsDead()`, PAS sur « l'appel n'a pas échoué ». C'est la classe
    // d'erreur que D1 vise : un appel accepté sans effet. Tant que le pantin n'est pas mort, on
    // renvoie `false` et l'appelant réessaie au snapshot suivant.
    public func TesseraRendreMort(cible: EntityID) -> Bool {
        let entite = GameInstance.FindEntityByID(GetGameInstance(), cible);
        let pantin = entite as ScriptedPuppet;
        if !IsDefined(pantin) {
            return false;
        }
        // Déjà mort : succès, rien à refaire. Ce test doit venir AVANT `IsAttached`, sinon un
        // cadavre en cours de destreaming redeviendrait « à retenter » indéfiniment.
        if pantin.IsDead() {
            return true;
        }
        // Un pantin pas encore ATTACHÉ n'a ni pile d'animation ni pool de vie : `Kill` y serait
        // accepté sans effet. On refuse, et on réessaiera.
        if !pantin.IsAttached() {
            return false;
        }
        // ⚠️ ON LÈVE L'IMMORTALITÉ AVANT DE TUER, et l'ordre n'est pas négociable. Les entités
        // réseau sont rendues `Immortal` à l'attachement (`AvatarNeutre.reds`) pour qu'elles ne
        // meurent pas de la comptabilité de leur propre pantin : c'est le SERVEUR qui décide de la
        // mort. Quand il la décide, il faut donc lever le verrou — sinon `Kill` est accepté sans
        // effet et le corps reste debout, exactement le « succès trompeur » que D1 interdit de
        // compter comme un résultat.
        //
        // La MÊME source (`n"Tessera"`) qu'à la pose : `RemoveGodMode` est comptée par source, et
        // en retirer une autre ne lèverait rien.
        GameInstance.GetGodModeSystem(GetGameInstance())
            .RemoveGodMode(cible, gameGodModeType.Immortal, n"Tessera");
        pantin.Kill(null, false, false);

        // ⚠️ `Kill` est DIFFÉRÉ D'UNE FRAME : `IsDead()` juste en dessous renvoie presque toujours
        // `false`, et ce `false` ne veut pas dire échec — il veut dire « pas encore ».
        //
        // ── Le trou que ça cachait, mesuré le 2026-08-09 ──────────────────────────────────────
        //
        // Le commentaire d'origine disait « l'appelant réessaiera au snapshot suivant ». Le journal
        // dit le contraire : `TesseraRendreMort refuse pour 10` apparaît à CHAQUE mort, et
        // `Cadavre applique` **jamais**. La voie de reprise par snapshot
        // (`NetworkGameSystem.cpp:938`, sur `behavior == kComportementATerre`) ne se déclenche pas,
        // et l'appel one-shot (`:1993`) se contente d'avertir sans rien retenter.
        //
        // Ça marchait quand même — le `Kill` prenait bien effet 4 ms plus tard. Mais ça marchait
        // **sans que personne ne le vérifie** : le jour où `Kill` échoue pour de bon (pantin en
        // cours de détachement, streaming, état transitoire), le corps reste DEBOUT pour toujours,
        // et le seul indice serait une ligne d'avertissement qui apparaît déjà à chaque mort
        // normale — donc que personne ne lit. C'est la panne que Lucas a connue, et elle était
        // structurellement possible à nouveau.
        //
        // On ferme la boucle ICI plutôt que côté C++ : la vérification est du ressort de celui qui
        // connaît l'effet attendu, et ça évite une reconstruction de DLL pour une logique de
        // relance. La valeur de retour reste HONNÊTE (l'effet constaté à l'instant, pas l'intention)
        // — c'est le vérificateur qui garantit le résultat, pas un `true` optimiste.
        if !pantin.IsDead() {
            GameInstance.GetDelaySystem(GetGameInstance())
                .DelayCallback(TesseraVerifieCadavre.Creer(cible, 1u), 0.25, false);
        }
        return pantin.IsDead();
    }

    // Écrit la santé décidée par le SERVEUR sur le joueur LOCAL. `sante` en pour mille : 1000 =
    // barre pleine, 0 = mort.
    //
    // ⚠️ MÉTHODE DE CLASSE : le C++ l'appelle par `Red::CallVirtual`, qui cherche une méthode sur la
    // classe de l'objet. Déclarée au niveau module, l'appel échouerait EN SILENCE — piège payé le
    // 2026-08-08 sur `TesseraRendreMort` (2 500 tentatives, zéro instruction exécutée).
    //
    // `Uint32` et non `Uint16` : redscript n'a pas de type 16 bits, la conversion se fait au
    // franchissement du fil — même règle que `nature` dans `Tessera_ReportStim`.
    //
    // ⚠️ ON NE RÉIMPLÉMENTE NI LA MORT NI SON ÉCRAN. À zéro, c'est la mort NATIVE qui s'enclenche,
    // et l'écran de mort garni (C20, `UiKitDeath.reds`) s'affiche derrière elle — avec le joueur
    // immobilisé et couché par le moteur, gratuitement. C'est exactement le modèle acté le
    // 2026-07-27 après la mesure « la mort n'est pas annulable » : on garde le natif, on le garnit.
    //
    // `IgnoreChangeMode` et non `RequestSettingStatPoolValue` : c'est la variante qu'emploie
    // `ScriptedPuppet.Kill` lui-même (`scriptedPuppet.script:2251`), et la seule qui ne se fasse
    // pas re-lisser par le mode de changement du pool (la régénération de santé).
    public func AppliquerSanteJoueur(sante: Uint32) -> Bool {
        let joueur = GameInstance.GetPlayerSystem(GetGameInstance()).GetLocalPlayerControlledGameObject();
        if !IsDefined(joueur) {
            return false;
        }
        // Pour mille → pourcentage : c'est l'unité des pools de stats du jeu (`perc = true`).
        let pourcent = Cast<Float>(sante) / 10.0;

        // ⚠️ ON N'ÉCRIT QUE SI L'ÉTAT LOCAL CONTREDIT LE SERVEUR. Ni « à chaque battement » ni
        // « une seule fois » — les deux ont été essayés en jeu le 2026-08-09 et ont produit deux
        // pannes opposées :
        //   · à chaque battement → le joueur se relève entre deux, puis se fait retuer : tremblement ;
        //   · une seule fois     → plus rien ne le maintient mort, il ressuscite pour de bon.
        // Le battement de coma répète `0` une fois par seconde pour faire avancer le décompte ; il
        // ne doit REÉCRIRE la barre que si le moteur local a entre-temps rendu sa vie au joueur.
        // Cette comparaison rend l'opération idempotente, et donc auto-corrective.
        let stats = GameInstance.GetStatPoolsSystem(GetGameInstance());
        let cible = Cast<StatsObjectID>(joueur.GetEntityID());
        let actuel = stats.GetStatPoolValue(cible, gamedataStatPoolType.Health, true);
        // 0,5 point de pourcentage : sous cet écart, la barre dit déjà ce que le serveur veut, et
        // réécrire ne ferait que rejouer une animation de dégâts pour rien.
        // ⚠️ INSTRUMENTATION — le dernier point aveugle de la chaîne de mort (2026-08-09).
        //
        // Trois correctifs ont visé « le personnage se relève » sans le faire disparaître, et à
        // chaque fois j'ai DÉDUIT au lieu de mesurer. Ce que personne n'a jamais vu, c'est la
        // valeur de la barre LOCALE à l'instant où le serveur impose la sienne. Elle répond seule à
        // la question : si le local remonte entre deux battements, le moteur ressuscite le joueur ;
        // s'il reste à zéro, ce que Lucas voit se relever est ailleurs (l'avatar du mort chez le
        // tireur, dont `TesseraRendreMort` a été refusé).
        //
        // Journalisé à CHAQUE battement, y compris quand on n'écrit pas — c'est précisément le cas
        // « on n'écrit pas » qui manque au diagnostic.
        // ⚠️ LES DEUX VERROUS SE RELISENT SUR LA MÊME LIGNE, ET C'EST LA LEÇON DE LA JOURNÉE.
        //
        // Trois correctifs posés sur ce joueur se sont révélés INERTES sans jamais le signaler
        // (`Immortal`, `Defeated`, puis `ForcePreventResurrect` — tous sautés parce que le joueur
        // n'était pas encore trouvable à l'attachement). Chacun a coûté un cycle complet de
        // relance + test + lecture de journal pour découvrir qu'il ne s'était rien passé.
        //
        // Un verrou qu'on ne peut pas RELIRE est indiscernable d'un verrou absent. On lit donc les
        // deux valeurs à chaque battement, à côté de la barre qu'elles protègent :
        //   · `interditReanim` doit valoir 1 — sinon le correctif n'est pas posé, point final ;
        //   · `secondCoeur` doit valoir 0 tant que le contrôle temporaire est en place.
        // La prochaine panne se lira en une ligne au lieu d'un cycle.
        let statsLecture = GameInstance.GetStatsSystem(GetGameInstance());
        let interditReanim = statsLecture.GetStatValue(cible, gamedataStatType.ForcePreventResurrect);
        let secondCoeur = statsLecture.GetStatValue(cible, gamedataStatType.HasSecondHeart);
        let reseauJournal = GameInstance.GetNetworkGameSystem();
        if IsDefined(reseauJournal) {
            reseauJournal.Tessera_Journal(
                s"santé : serveur \(pourcent)% · local \(actuel)% · écriture \(AbsF(actuel - pourcent) >= 0.5)"
                + s" · interditReanim=\(interditReanim) secondCoeur=\(secondCoeur)");
        }
        // ⚠️ ON NE POSE PAS DE `Defeated` ICI, ET C'EST UN VERDICT, PAS UN OUBLI.
        //
        // Une version de ce bloc appliquait `BaseStatusEffect.Defeated` au joueur quand le serveur
        // annonçait 0, pour le coucher délibérément. Mesuré le 2026-08-09 : **jamais appliqué** —
        // aucune trace dans le journal, alors que la sonde capte tous les autres statuts du joueur.
        // `Defeated` est un état de PANTIN ; le joueur a sa propre machine à états.
        //
        // Il est de toute façon devenu inutile : la mort native fournit la chute, et c'est bien elle
        // qu'on veut. Ce qu'il fallait supprimer, ce n'était pas la mort — c'était la RÉSURRECTION
        // qui la suivait (le Second Cœur, voir `SanteLocale.reds`).

        if AbsF(actuel - pourcent) < 0.5 {
            return true;
        }
        stats.RequestSettingStatPoolValueIgnoreChangeMode(
            cible,
            gamedataStatPoolType.Health,
            pourcent,
            null,
            true);
        return true;
    }

    // Applique l'apparence décidée par le serveur sur un PNJ statique déjà présent.
    //
    // ⚠️ MÉTHODE DE CLASSE : le C++ l'appelle par `Red::CallVirtual`, qui cherche sur la classe de
    // l'objet. Déclarée au niveau module, l'appel échouerait EN SILENCE — piège payé le 2026-08-08
    // sur `TesseraRendreMort` (2 500 tentatives, zéro instruction exécutée).
    //
    // On ne crée ni ne détruit rien : l'entité existe déjà sur les deux clients, au même endroit,
    // avec le même record. Seule sa variante visuelle change.
    public func AppliquerApparenceStatique(cible: EntityID, apparence: CName) -> Bool {
        if !IsNameValid(apparence) {
            return false;
        }
        let entite = GameInstance.FindEntityByID(GetGameInstance(), cible);
        if !IsDefined(entite) {
            // Pas encore streamé. L'appelant le sait et retentera à l'attachement.
            return false;
        }
        // ⚠️ `ScheduleAppearanceChange` est DIFFÉRÉ : relire l'apparence juste après renvoie encore
        // l'ancienne (F-PNJ-050). D'où la relecture à 3 s ci-dessous, et pas ici.
        //
        // MESURE DE L'EFFET, pas de l'appel (2026-08-09, après le verdict de Lucas : « l'esthétique
        // n'est pas hydratée »). Renvoyer `true` parce que l'ordre est passé ne prouve rien — le
        // moteur REJETTE EN SILENCE une apparence étrangère au jeu d'apparences du PNJ (F-PNJ-051).
        // Trois issues à distinguer, une seule est un vrai défaut :
        //   · DEJA-BON   → les deux clients étaient déjà d'accord, il n'y avait rien à faire ;
        //   · PREND      → l'apparence relue est bien celle demandée ;
        //   · SANS-EFFET → le moteur a refusé — c'est ce cas-là qu'il faut corriger.
        let pantin = entite as ScriptedPuppet;
        if IsDefined(pantin) {
            let origine = pantin.GetCurrentAppearanceName();
            if Equals(origine, apparence) {
                this.Tessera_Journal(s"[Hydra] VERDICT=DEJA-BON \(origine)");
                return true;
            }
            entite.ScheduleAppearanceChange(apparence);
            let verdict = new TesseraVerdictHydratation();
            verdict.pantin = pantin;
            verdict.demandee = apparence;
            verdict.origine = origine;
            GameInstance.GetDelaySystem(GetGameInstance()).DelayCallback(verdict, 3.0, false);
            return true;
        }
        entite.ScheduleAppearanceChange(apparence);
        return true;
    }

    // Applique une apparence autoritaire SEULEMENT si le joueur ne peut pas le voir.
    //
    // ── L'idée, dans les mots de Lucas (2026-08-08) ─────────────────────────────────────────
    // « Une espèce de cône de vision : quand on n'est plus dans le cône, ça change l'esthétique du
    // personnage, pour rendre quelque chose de fidèle pour tout le monde. »
    //
    // Elle résout deux choses à la fois : le changement devient invisible, ET les ordres arrivés
    // trop tôt (PNJ pas encore streamé) finissent par s'appliquer, au lieu d'attendre un
    // réattachement qui peut ne jamais venir.
    //
    // ⚠️ `false` signifie « PAS MAINTENANT », jamais « impossible ». L'appelant réessaiera au tick
    // suivant. Confondre les deux ferait abandonner un PNJ simplement parce qu'on le regardait.
    //
    // Deux échappatoires au cône, et chacune a sa raison :
    //   · au-delà de 60 m, on applique quand même — le changement est indiscernable à cette
    //     distance, et attendre l'occultation d'un PNJ lointain pourrait durer toute la session ;
    //   · dans le dos (produit scalaire négatif), c'est le cas nominal.
    public func AppliquerApparenceDiscrete(cible: EntityID, apparence: CName) -> Bool {
        let joueur = GameInstance.GetPlayerSystem(GetGameInstance()).GetLocalPlayerControlledGameObject();
        let entite = GameInstance.FindEntityByID(GetGameInstance(), cible);
        if !IsDefined(joueur) || !IsDefined(entite) {
            return false;
        }
        // On reste en `Vector4` de bout en bout : `Vector4To3` n'existe pas dans les scripts du
        // jeu, et l'opérateur de soustraction est défini sur `Vector4` (`vector.script:153`).
        let versPnj = entite.GetWorldPosition() - joueur.GetWorldPosition();
        let distance = Vector4.Length(versPnj);
        if distance > 60.0 {
            return this.AppliquerApparenceStatique(cible, apparence);
        }
        // Produit scalaire du regard et de la direction du PNJ, normalisés : > 0 = devant.
        // 0.2 plutôt que 0.0 : une marge, pour ne pas rhabiller quelqu'un en limite de champ que le
        // joueur verrait du coin de l'oeil.
        let regard = Vector4.Normalize(joueur.GetWorldForward());
        let vers = Vector4.Normalize(versPnj);
        if Vector4.Dot(regard, vers) > 0.2 {
            return false;
        }
        return this.AppliquerApparenceStatique(cible, apparence);
    }

    // ── RÉPARATION DU ROSTER (spec 2026-08-09, complétion asymétrique) ──────────────────────────
    //
    // Mesuré le 2026-08-09 : deux clients à la position IDENTIQUE ne voient que 83 % des mêmes PNJ
    // statiques, et le chiffre est PLAT sur neuf minutes — ce n'est pas un retard de streaming, les
    // deux moteurs peuplent durablement deux mondes différents. On ne peut ni piloter la foule
    // native (F-PNJ-069) ni en retirer un membre (F-PNJ-091, F-PNJ-093) : on ne peut que COMPLÉTER.
    //
    // ⚠️ Le remplaçant est LOCAL, et c'est le cœur de la conception. Si le serveur spawnait
    // l'entité manquante comme entité réseau, le client qui possède déjà le natif la recevrait
    // aussi et verrait un DOUBLON qu'on ne sait pas supprimer.
    //
    // Renvoie l'EntityID du remplaçant créé, ou une EntityID vide si on n'a rien fait — le C++
    // retentera au tick suivant. « Rien fait » n'est jamais « impossible ».
    public func ReparerStatique(cible: EntityID, record: TweakDBID, apparence: CName,
                                position: Vector4, orientation: Quaternion) -> EntityID {
        let vide: EntityID;
        // Le natif est là : rien à faire, et surtout rien à créer.
        if IsDefined(GameInstance.FindEntityByID(GetGameInstance(), cible)) {
            return vide;
        }
        let joueur = GameInstance.GetPlayerSystem(GetGameInstance()).GetLocalPlayerControlledGameObject();
        if !IsDefined(joueur) {
            return vide;
        }
        // `Vector4.Length(a - b)` et non `Distance` : c'est la forme déjà utilisée dans ce fichier,
        // donc déjà compilée contre le vrai RTTI.
        let distance = Vector4.Length(position - joueur.GetWorldPosition());
        // Deux bornes, et chacune vient d'un cas du challenge de la spec (§5).
        //   · sous 30 m : un remplaçant se tient DEBOUT là où le natif est assis ou adossé
        //     (F-PNJ-072, pas de workspot). De près, ça se voit plus que l'absence.
        //   · au-delà de 100 m : hors du rayon d'AoI, la géométrie peut ne pas être chargée — on
        //     poserait un PNJ sans sol, ou dans un mur.
        // ⚠️ PLUS DE PLANCHER DE DISTANCE (2026-08-09, arbitrage de Lucas : « peu importe si ça fait
        // un changement brut devant les gens, l'important c'est cent pour cent de fidélité »).
        // Un remplaçant se tient DEBOUT là où le natif est assis (F-PNJ-072, pas de workspot) : de
        // près ça se voit. Mais un PNJ ABSENT chez l'un et présent chez l'autre se voit davantage,
        // et surtout il casse le RP — deux joueurs ne peuvent pas parler de quelqu'un qu'un seul
        // voit. Le plafond, lui, reste : au-delà du rayon d'AoI la géométrie peut ne pas être
        // chargée, et on poserait un PNJ sans sol.
        // 250 m et non 100 : le plafond ne protege que d'une chose — poser un PNJ dans un secteur
        // non charge, donc sans sol. Or la geometrie du monde streame BEAUCOUP plus loin que la
        // foule ; c'est precisement pour ca que des PNJ manquent au loin alors que la rue est la.
        // Mesure du 2026-08-09 : borner a 100 m laissait 10 % de presence non rattrapee, tous
        // au-dela. ⚠️ Si des remplaçants apparaissent en l'air, c'est CE reglage qu'il faut baisser.
        if distance > 250.0 {
            return vide;
        }
        // ⚠️ LA GARDE QUI MANQUAIT. `FindEntityByID` ci-dessus ne suffit pas — il rend nil sur un
        // pantin bien vivant (F-PNJ-088), et un faux « absent » pose un remplaçant PAR-DESSUS un
        // natif présent, avec l'apparence du roster : deux personnes au même endroit, habillées
        // différemment. C'est ce que Lucas a observé après la déduplication par identifiant.
        //
        // 0,6 m : un pantin debout occupe ~0,5 m d'emprise au sol. Assez large pour attraper un
        // natif dont la position rapportée diffère de quelques centimètres, assez étroit pour ne
        // pas refuser un voisin légitime — deux PNJ distincts ne se tiennent pas à 60 cm.
        if this.TesseraQuelquUnIci(position, 0.6, vide) {
            return vide;
        }
        let cree = this.SpawnNetworkAvatar(record, apparence, position, orientation);
        // Journalisé au format `[Etat]`, avec la MÊME clé que la sonde de foule (position au
        // décimètre) : sans ça la mesure de cohérence ne verrait pas les remplaçants — ils ne sont
        // pas `IsCrowd()`, donc `CrowdProbe` ne les classe pas. Un PNJ réparé compterait alors
        // comme absent, et le chiffre dirait exactement le contraire de la vérité.
        // Le remplaçant se fait SUIVRE comme un natif : sans ça la mesure ne le verrait qu'à sa
        // naissance et compterait la réparation comme un échec (il n'est pas `IsCrowd()`).
        let cle = s"\(Cast<Int32>(position.X * 10.0));\(Cast<Int32>(position.Y * 10.0));\(Cast<Int32>(position.Z * 10.0))";
        this.Tessera_Journal(s"[Etat] \(cle);0;\(NameToString(apparence));remplacant-cree");
        let releve = new TesseraReleveEtatStatique();
        releve.cible = cree;
        releve.cle = cle;
        releve.attendue = apparence;
        GameInstance.GetDelaySystem(GetGameInstance()).DelayCallback(releve, 10.0, false);
        return cree;
    }

    // Le natif a-t-il fini par arriver ? C'est la condition de RETRAIT du remplaçant : sans elle on
    // laisserait deux PNJ au même endroit — le nôtre et celui du jeu.
    public func TesseraEntiteExisteLocalement(cible: EntityID) -> Bool {
        return IsDefined(GameInstance.FindEntityByID(GetGameInstance(), cible));
    }

    // Y a-t-il DÉJÀ quelqu'un debout à cet endroit, autre que `sauf` ?
    //
    // ── POURQUOI SPATIALE, ET PAS PAR IDENTIFIANT ──────────────────────────────────────────
    //
    // La déduplication par identifiant, puis par position du roster, ne couvrait qu'une chose :
    // nos remplaçants entre eux. Or **trois** mécanismes créent des PNJ sans se consulter — le
    // figurant natif du jeu, le PNJ promu par le serveur, et notre remplaçant de roster. Lucas les
    // a vus empilés « avec des esthétiques différentes » : c'est la signature d'entités venues de
    // sources différentes, pas de doublons d'une même source.
    //
    // ⚠️ Et le test de présence sur lequel tout reposait n'est pas fiable : `FindEntityByID` rend
    // nil sur un pantin bien vivant (**F-PNJ-088**, mesuré le 2026-08-05 — seule la voie par tag le
    // retrouvait). Un faux « absent » fabrique un remplaçant PAR-DESSUS un natif présent, et comme
    // son apparence vient du roster, il porte une autre tenue. C'est exactement le symptôme.
    //
    // On cesse donc de demander « cet identifiant existe-t-il ? » pour demander « cet ENDROIT
    // est-il occupé ? ». La question spatiale ne dépend d'aucune clé — donc d'aucune des deux
    // hypothèses que F-PNJ-150 laisse ouvertes sur l'origine des identifiants multiples.
    //
    // `GetEntitiesAroundObject` (`gameObject.script:936`) énumère autour de l'APPELANT : la portée
    // demandée couvre donc la distance joueur→cible plus le rayon. Au-delà de ce que la requête de
    // ciblage sait rendre, on répond `false` — dégradation vers le comportement d'avant, jamais un
    // refus de réparer.
    public func TesseraQuelquUnIci(position: Vector4, rayon: Float, sauf: EntityID) -> Bool {
        let joueur = GameInstance.GetPlayerSystem(GetGameInstance()).GetLocalPlayerControlledGameObject();
        if !IsDefined(joueur) {
            return false;
        }
        let portee = Vector4.Length(position - joueur.GetWorldPosition()) + rayon;
        let autour = joueur.GetEntitiesAroundObject(portee);
        // ⚠️ PAS de `continue` : le mot-clé N'EXISTE PAS en redscript, et l'erreur ne le dit pas
        // ainsi — `unresolved reference 'continue'`, comme s'il s'agissait d'une variable. Attrapé
        // par le compile-check hors jeu ; en jeu, il aurait fait tomber TOUT r6/scripts.
        let i = 0;
        while i < ArraySize(autour) {
            let go = autour[i] as GameObject;
            i += 1;
            if IsDefined(go) {
                // Notre propre remplaçant n'occupe pas sa place contre lui-même.
                let estMoi = EntityID.IsDefined(sauf) && go.GetEntityID() == sauf;
                if !estMoi && Vector4.Length(go.GetWorldPosition() - position) <= rayon {
                    return true;
                }
            }
        }
        return false;
    }

    public func DestroyTransientEntity(entityId: EntityID) {
        GameInstance.GetDynamicEntitySystem().DeleteEntity(entityId);
    }

    public func TeleportEntity(game: GameInstance, entity: ref<Entity>, position: Vector4, worldOrientation: EulerAngles) {
        // TODO: there is no SetWorldPosition.
        //let entity = GameInstance.GetDynamicEntitySystem().GetEntity(id);
        //let transform = .GetWorldTransform();
        // let worldPosition = WorldTransform.GetWorldPosition(transform);
        // WorldPosition.SetVector4(worldPosition, position);
        // WorldTransform.SetPosition(transform, position);

        // We need GameObjects, not pure Entites.
        //GameInstance.GetTeleportationFacility(game).Teleport(entity as GameObject, position, worldOrientation);
    }

    public func TeleportPuppet(puppet: ref<ScriptedPuppet>, position: Vector4, rotation: Float) -> ref<AICommand> {
        let teleportCommand = new AITeleportCommand();
        teleportCommand.position = position;
        teleportCommand.rotation = rotation;
        teleportCommand.doNavTest = false;

        // ── ANNULER LE PLACEMENT PRÉCÉDENT — TROISIÈME ET DERNIER ENDROIT OÙ LA RÈGLE MANQUAIT ──
        //
        // *Une commande d'IA ne se remplace pas : elle s'exécute*, avec la destination qu'elle
        // portait AU MOMENT DE L'EMPILAGE. Un placement qui n'a pas encore été consommé va donc
        // téléporter l'avatar vers une position **périmée**, et il le fera même si nous en avons
        // envoyé un meilleur entre-temps.
        //
        // MESURE (2026-08-17, fantôme rejoueur en maintien, personne au clavier) : après l'arrêt,
        // l'avatar dérive lentement pendant ~5 s, **saute de 13 m vers un point FAUX** (dérive
        // 17,9 → 30,4 m), fait 3 m de plus, et n'atterrit qu'ensuite sur la bonne cible. Ce sont
        // nos propres placements qui se rejouent dans l'ordre d'émission.
        //
        // C'est le même défaut que pour le gel et pour la marche (`TesseraFigerAvatar`,
        // `TesseraSuivreAvatar`) — quatre symptômes rapportés séparément, une seule cause.
        // Ici, le fait d'annuler d'abord rend l'excursion impossible : au pire on annule un
        // placement qui allait de toute façon être remplacé par celui qu'on envoie.
        let controleur = puppet.GetAIControllerComponent();
        controleur.CancelOrInterruptCommand(n"AITeleportCommand", true, false);
        controleur.SendCommand(teleportCommand);
        // ⚠️ `DisableCollider()` ÉTAIT ICI, ET C'ÉTAIT LA CAUSE DE « impossible de tirer dessus ».
        //
        // Bricolage de confort hérité du fork Cyberverse, marqué temporaire par son auteur lui-même
        // (« TODO: Temp — in the future this should be controlled by the server, but currently
        // Judy's just annoying :D »). `SetEntityPosition` appelle cette fonction pour TOUTE entité
        // réseau — au spawn, puis à chaque correction de dérive : le collider de chaque avatar était
        // donc coupé, et jamais rendu.
        //
        // Un pantin sans collider se rend et s'anime parfaitement — il n'est simplement plus là pour
        // le monde physique. Les balles le traversent. C'est exactement le symptôme observé le
        // 2026-08-08 : « je vois le joueur bouger parfaitement, mais impossible de tirer dessus ».
        // Et c'est ce que le registre soupçonnait depuis le 2026-07-21 sans l'avoir mesuré
        // (F-VEH-022, `hypothèse`).
        //
        // La gêne d'origine était réelle : un pantin distant solide pousse le joueur local. Mais
        // c'est un problème de jeu multijoueur — des joueurs qui se bousculent — pas une raison de
        // les rendre intangibles. Si ça redevient pénible, ça se règle par la physique, jamais en
        // retirant l'avatar du monde physique.
        puppet.GetAIControllerComponent().ForceTickNextFrame();

        // let attackCommand = new AIMeleeAttackCommand();
        // puppet.GetAIControllerComponent().SendCommand(attackCommand);

        // let weapon = ScriptedPuppet.GetActiveWeapon(puppet);
        // weapon.ShootStraight(true);

        return teleportCommand;
    }

    // Fait MARCHER une entité réseau vers la position décidée par le serveur, au lieu de l'y
    // téléporter. C'est ce qui remplace le glissement par une vraie locomotion animée.
    //
    // ✅ MÉCANISME MESURÉ, pas choisi au jugé : `AIMoveToCommand` fait réellement marcher et
    // naviguer un PNJ commandé par le serveur (F-PNJ-082, en jeu le 2026-07-22), et le même
    // mécanisme rend/anime/suit correctement un avatar de joueur distant (F-PLY-007, 2026-07-25).
    //
    // ⚠️ C'est de l'ANIMATION LOCALE, PAS de l'autorité — F-PNJ-082 le dit explicitement. La
    // position qui fait foi reste celle du Snapshot serveur ; cette commande ne sert qu'à ce que
    // le trajet soit joué par le moteur au lieu d'être sauté. Le C++ garde donc la téléportation
    // comme CORRECTION quand la dérive devient trop grande.
    //
    // `ignoreNavigation` : voir le commentaire détaillé au point d'usage. Il valait `true` tant que
    // le serveur annonçait la position du tick suivant ; il vaut `false` depuis qu'il annonce une
    // destination à 10 m, pour que le moteur navigue par les trottoirs et respecte les feux.
    public func MoveNetworkEntityTo(entityId: EntityID, position: Vector4, locomotion: Int32) -> Bool {
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }
        let controller = puppet.GetAIControllerComponent();
        if !IsDefined(controller) {
            return false;
        }

        let cmd = new AIMoveToCommand();
        let cible: AIPositionSpec;
        let wp: WorldPosition;
        WorldPosition.SetVector4(wp, position);
        AIPositionSpec.SetWorldPosition(cible, wp);
        cmd.movementTarget = cible;

        // Locomotion protocole → allure moteur. Les valeurs accroupies (4, 5) et l'air (6) n'ont
        // pas d'équivalent dans `moveMovementType` : elles retombent sur la marche, qui est le
        // moins faux des choix — mieux vaut un PNJ qui marche qu'un PNJ qui glisse.
        if locomotion == 3 {
            cmd.movementType = moveMovementType.Sprint;
        } else if locomotion == 2 {
            cmd.movementType = moveMovementType.Run;
        } else {
            cmd.movementType = moveMovementType.Walk;
        }

        // ⚠️ `ignoreNavigation` est passé de `true` à `false` le 2026-08-06, et la justification
        // d'origine (juste au-dessus) ne tient plus depuis le même jour.
        //
        // Elle disait : « le serveur a déjà planifié le chemin, laisser le moteur re-naviguer
        // ferait diverger les deux ». C'était vrai quand le serveur annonçait la position du TICK
        // SUIVANT — un point à 5 cm, puis à 1 m : le pantin devait y aller exactement, il n'y avait
        // rien à naviguer.
        //
        // Le serveur annonce désormais une DESTINATION à au moins 10 m (`NpcState.move_target`).
        // Sur ce trajet-là, on VEUT que le moteur navigue : c'est lui qui connaît les trottoirs,
        // les passages piétons et les feux. Observé en jeu tant que le drapeau valait `true` :
        // « ils vont sur la route et traversent les passages piétons alors que c'est rouge » —
        // logique, on lui demandait justement d'ignorer tout ça.
        //
        // L'autorité ne change pas de camp pour autant : la position serveur reste la vérité, et
        // la correction de dérive (8 m, côté C++) rattrape si le moteur choisit un détour trop
        // large. Le serveur décide OÙ l'on va, le moteur décide COMMENT — c'est F-PNJ-095.
        cmd.ignoreNavigation = false;
        cmd.finishWhenDestinationReached = true;
        cmd.desiredDistanceFromTarget = 0.50;

        controller.SendCommand(cmd);
        return true;
    }

    // Fait SUIVRE un point de visée à l'avatar d'un JOUEUR distant.
    //
    // ⚠️ Ce n'est PAS `MoveNetworkEntityTo` avec d'autres valeurs, et la différence est le sujet.
    // Deux populations, deux boucles, et les mélanger casse l'une ou l'autre :
    //
    //   · un PNJ a une destination CONNUE DU SERVEUR (`NpcState.move_target`, planifiée sur le
    //     graphe de nav), et on VEUT que le moteur navigue — trottoirs, passages piétons, feux
    //     (F-PNJ-095). D'où `ignoreNavigation = false` et une commande terminante là-bas ;
    //   · un joueur n'a aucune destination connue du serveur, et sa position FAIT AUTORITÉ. On ne
    //     veut surtout pas que le moteur lui recalcule un chemin autour d'un obstacle : on le veut
    //     là où le serveur le dit. D'où `ignoreNavigation = true` ici.
    //
    // ✅ CETTE configuration est celle qui est MESURÉE viable, pas celle qu'on suppose : sonde
    // `loco_hybrid`, en jeu le 2026-07-23 (backlog Q6/Q6b, registre F-PLY-008). « Pantin sous
    // commande de marche active + Teleport en rafale → il MARCHE correctement, jambes animées,
    // entre chaque snap. Le Teleport ne casse PAS l'anim tant que la commande de marche tourne. »
    // La boucle complète est donc : cette commande pour l'ANIMATION, `TeleportPuppet` pour la
    // POSITION quand la dérive se creuse.
    //
    // `finishWhenDestinationReached = false` : la commande ne se termine JAMAIS d'elle-même. C'est
    // ce qui évite le défaut mesuré le 2026-08-06 côté PNJ — un pantin qui atteint sa cible,
    // s'arrête, et attend le prochain ordre. Le C++ ne la réémet que quand le point de visée a
    // franchement bougé (voir `PiloterAvatar`).
    //
    // `desiredDistanceFromTarget = 0.0` : on vise un point DEVANT l'avatar (dérivé de sa vitesse),
    // pas sa position courante. Y tolérer un rayon d'arrivée le ferait s'arrêter en chemin.
    // ── LA DIRECTION DU REGARD, SÉPARÉE DE LA DIRECTION DU DÉPLACEMENT ─────────────────────
    //
    // Demandé par Lucas le 2026-08-13 : « si on regarde une personne et qu'on recule, il faut
    // qu'on ait l'animation de je marche en arrière. Ou déplacement latéral. Toutes les formes de
    // déplacement n'ont pas été prises en compte. »
    //
    // Il a raison, et le manque était structurel : `AIMoveToCommand` fait marcher un pantin VERS
    // un point, et un pantin qui marche vers un point le REGARDE. Tant que la seule chose qu'on
    // commandait était une destination, l'avatar ne pouvait qu'avancer face à sa marche — jamais
    // reculer, jamais faire un pas de côté. Le `move_dir` du protocole, qui porte exactement cette
    // information depuis le gel du palier 2, n'était lu nulle part.
    //
    // Le moteur sait pourtant faire, et c'est prévu dans la commande elle-même : `facingTarget` +
    // `rotateEntityTowardsFacingTarget` dissocient l'orientation de la trajectoire
    // (`aiCommand.script:83-84`). On donne donc DEUX points — où il va, et ce qu'il regarde — et
    // c'est le graphe d'animation qui choisit tout seul marche avant, arrière ou latérale.
    //
    // Le point de regard se construit depuis le `yaw` du joueur, à cinq mètres devant : assez loin
    // pour que la direction soit stable, assez près pour rester dans le même secteur.
    //
    // ⚠️ L'origine est la position RÉELLE du pantin, jamais le point de visée. Le point de visée est
    // déjà à trois mètres devant, dans la direction du DÉPLACEMENT : partir de lui mélangerait les
    // deux directions au lieu de les séparer. Un pas de côté franc — 90° entre marche et regard —
    // se serait retrouvé à 59° (`atan(3/5)`), et l'animation latérale n'aurait été qu'à moitié
    // jouée, pour une raison invisible à la lecture.
    private func PointDeRegard(depuis: Vector4, yaw: Float) -> Vector4 {
        // Un yaw de 0 regarde +Y dans ce moteur ; `RotByAngleXY` applique la rotation autour de Z.
        let avant = Vector4.RotByAngleXY(new Vector4(0.0, 1.0, 0.0, 0.0), yaw);
        return new Vector4(depuis.X + avant.X * 5.0, depuis.Y + avant.Y * 5.0, depuis.Z, 1.0);
    }

    // ─────────────────────────────────────────────────────────────────────────────────────
    // SONDE T7 — un `ApplyFeature` venant du script ATTEINT-IL le graphe d'un pantin spawné ?
    // ─────────────────────────────────────────────────────────────────────────────────────
    //
    // C'EST LA QUESTION QUI COMMANDE TOUT LE RESTE. F-PNJ-153 a établi, par extraction WolvenKit
    // du `.animgraph` livré, que le graphe humanoïde expose 1 146 entrées `(groupe, champ)` —
    // dont `stanceState.state`, qui décrit la posture. Mais « l'entrée existe dans la donnée » et
    // « un appel script y arrive » sont deux affirmations différentes, et c'est précisément l'écart
    // qui a déjà produit `SendCommand` renvoyant `true` sans rien faire (F-PNJ-082) et
    // `ToggleCollision` accepté 35 fois sans effet.
    //
    // La sonde est choisie pour être INDISCUTABLE : un avatar est accroupi ou il ne l'est pas.
    // Pas de « ça a l'air plus fluide », pas de seuil à interpréter, pas de mesure à instrumenter.
    // C'est le critère du protocole de sondage — un état binaire, visible à l'œil nu.
    //
    // Le mécanisme suit exactement ce que la donnée décrit :
    //   · `n"stanceState"` est le GROUPE du nœud d'entrée, donc l'`inputName` de `ApplyFeature`
    //     (`animationControllerComponent.script:30`) ;
    //   · `AnimFeature_Stance` porte l'unique champ du groupe via `SetStanceState`
    //     (`animFeature.script:113-116`) ;
    //   · `animStanceState.Crouch` est la valeur (`animFeature.script:103-111`).
    //
    // ⚠️ On utilise `ApplyFeature` et NON `ApplyFeatureToReplicate` : la variante « ToReplicate »
    // rejouerait la feature vers le réseau NATIF du jeu, dont nous ne voulons rien. Notre
    // réplication à nous est déjà faite — c'est le serveur qui a dit quoi afficher.
    //
    // SI ÇA MARCHE, la voie est ouverte pour l'allure continue (`crowd_locomotion.speed`), la
    // marche arrière et le pas de côté (`locomotion.directionAngle`) — c'est-à-dire les deux
    // gestes qui portent la moitié des recalages mesurés le 2026-08-14.
    // SI ÇA NE MARCHE PAS, on l'apprend en une session au lieu de bâtir dessus.
    // ── TROISIÈME PASSE, 2026-08-19 (soir) : LE CONSOMMATEUR, ENFIN ──────────────────────
    //
    // Les deux premières passes ont mesuré l'ÉCRITURE et conclu à l'impasse : `ApplyFeature`
    // (F-PLY-053, F-PLY-120), `SetInputInt` (F-PLY-127), `ChangeStanceState` (voie B). Aucune
    // n'avait ouvert le CONSOMMATEUR — ce que le graphe FAIT de la valeur une fois écrite.
    //
    // Il en fait une chose, et toujours la même : `stanceState.state` alimente **13
    // `animAnimNode_Switch` à DEUX entrées** (F-PLY-133). D'où le seul changement de fond de
    // cette passe — l'ÉCHELLE. Les deux défauts qui se masquaient, et ce qui gêne encore
    // l'hypothèse, sont dans le corps de la fonction.
    public func TesseraPousserPosture(entityId: EntityID, accroupi: Bool) -> Bool {
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }

        // ── LES VOIES B ET C SONT RETIRÉES (2026-08-19, second passage) ────────────────────
        //
        // ⛔ Voie C — `SetInputInt(n"stanceState", …)`. Mesurée inerte (F-PLY-127), et de toute
        // façon redondante : le graphe n'expose qu'UN nœud sous ce groupe (`stanceState.state`),
        // que les deux écritures visaient toutes les deux.
        //
        // ⛔ Voie B — `NPCPuppet.ChangeStanceState`. Retirée non parce qu'elle a échoué, mais
        // parce qu'elle NE CHANGE AUCUN ÉTAT — et ça se lit dans le script décompilé, sans le
        // jeu. `NPCPuppet.script:1360` écrit un SIGNAL dans le `gameBoolSignalTable` du pantin.
        // Son unique lecteur est `NPCStatesComponent.OnNPCStateChangeSignalReceived`
        // (`npcStateComponent.script:278`) — un rappel de composant, invoqué par la machine d'IA.
        // Rien n'établit qu'il tourne sur nos avatars, et `UpdateStanceState()` — la fonction
        // qu'on croyait déclencher — n'a donc peut-être jamais tourné une seule fois. C'est
        // l'explication littérale de « accepté, aucun effet » : une intention déposée dans une
        // boîte aux lettres dont on n'a jamais vérifié qu'elle est relevée.
        //
        // ⚠️ Le nom exact du composant est `NPCStatesComponent`, au PLURIEL. Chercher
        // « NPCStateComponent » ne rend rien et fait conclure qu'il n'existe pas.

        // ── ⭐ CE QUE LE GRAPHE FAIT DE LA VALEUR (extraction du 2026-08-19) ────────────────
        //
        // Trois campagnes ont mesuré l'ÉCRITURE et conclu à l'impasse. Aucune n'avait ouvert le
        // CONSOMMATEUR. Extraction de `humanoid.animgraph` (WolvenKit 8.19.0, CP2077 v2.31) puis
        // dépouillement en flux du JSON de 420 Mo — le nœud N'EST PAS MORT, il est lu 14 fois :
        //
        //     animAnimNode_IntInput(group=stanceState, name=state)
        //       → animAnimNode_IntToFloatConverter        (conversion nue, aucune échelle)
        //         → weightNode d'un animAnimNode_Switch    ·  numInputs = 2   ← 13 fois
        //
        // ⚠️⚠️ ET LE CONTRÔLE NÉGATIF QUI M'A ÉVITÉ D'Y CROIRE (F-PLY-135). J'ai d'abord lu
        // « aiguillage à 2 entrées, donc il attend 0 ou 1, donc Crouch=2 et Stand=3 sont hors
        // bornes » — et poussé l'échelle `animStanceState` à la place. **C'est faux.** Le groupe
        // `highLevelState` est câblé À L'IDENTIQUE (7 aiguillages, `numInputs = 2` eux aussi) et
        // son énumération monte à 8 : le jeu y pousse `Relaxed = 5` et `Stealth = 6` sur chaque
        // PNJ de la ville, et ça marche. `numInputs` n'est donc PAS le domaine accepté, et le
        // `weightNode` d'un `animAnimNode_Switch` n'est pas un indice d'entrée.
        //
        // On reste donc sur l'échelle du JEU — celle de `npcStateComponent.script:1155`.
        //
        // ⚠️ Ce que le contrôle négatif change vraiment, et c'est plus utile que l'échelle :
        // `stanceState` et `highLevelState` partagent la MÊME classe, la MÊME API, le MÊME type de
        // consommateur. L'un marche en natif, l'autre ne produit rien chez nous. La différence
        // n'est donc pas dans le câblage du graphe — elle est dans NOTRE PANTIN. D'où
        // `TesseraPousserTrait` ci-dessous : le contrôle positif qui manquait depuis trois
        // semaines se tire sur un AUTRE groupe du même canal.
        // ── LES TROIS MÉCANISMES DU JEU, ET LEUR BISSECTION ÉCRITE D'AVANCE ───────────────
        //
        // `UpdateStanceState` et `UpdateHighLevelState` (`npcStateComponent.script:1151` et `:443`)
        // n'emploient pas UNE voie mais TROIS, et nous n'en copiions qu'une depuis août :
        //
        //   1. `stanceState`       — la posture. Lu par 13 aiguillages du graphe humanoïde.
        //   2. `highLevelState`    — le JEU D'ANIMATIONS. Lu par 7 aiguillages + un mélange.
        //   3. le POIDS DE WRAPPER — `stealthLocomotion`, lu par 18 nœuds `WrapperValue`.
        //
        // ⚠️ Le wrapper est le 3ᵉ, et c'est un TYPE D'ÉVÉNEMENT que nous n'avions jamais émis :
        // `AnimWrapperWeightSetter`, ni `AnimInputSetterAnimFeature` (F-PLY-120) ni
        // `AnimInputSetterInt` (F-PLY-127). C'est chez CDPR la ligne qui bascule vraiment le jeu
        // d'animations d'un pantin.
        //
        // ⚠️ `stealthLocomotion` et NON `inCrouch`. `inCrouch` est ce que renvoie
        // `GetAnimWrapperNameBasedOnStanceState(Crouch)` — mais il **n'existe pas** dans
        // `humanoid.animgraph` : sur les 63 noms de wrapper du graphe, des cinq de la posture seul
        // `inVehicle` y figure (F-PNJ-165). Le pousser serait un no-op par construction. Les
        // wrappers de locomotion de ce graphe sont indexés sur le HAUT NIVEAU, d'où le choix du
        // furtif — qui est aussi, visuellement, la démarche accroupie d'un PNJ.
        //
        // ── POURQUOI TROIS D'UN COUP, ALORS QUE L'ADR 0034 DIT « UNE CHOSE À LA FOIS » ─────
        //
        // Parce que l'ADR l'autorise quand la bissection est écrite AVANT, et qu'ici les trois
        // signatures sont distinctes à la lecture :
        //
        //   · la HAUTEUR DE TÊTE change (1,64 m → ~1,20 m)  → la voie 1 a atteint le graphe
        //   · la DÉMARCHE change sans que la tête descende  → la voie 2 ou 3 a atteint le graphe
        //   · RIEN ne bouge, les trois acceptées            → ce n'est aucune API : c'est NOTRE
        //     PANTIN. Prochaine étape nommée et courte : a-t-il seulement un `NPCStatesComponent`
        //     (F-PLY-134) — le contrôle par `politique|autour`, écrit et jamais tiré.
        //
        // Ce troisième cas est la vraie raison de tout pousser ensemble. `stanceState` et
        // `highLevelState` partagent la même classe, la même API et le même type de consommateur ;
        // l'un marche en natif, l'autre ne produit rien chez nous. **Le contrôle positif qui
        // manquait depuis trois semaines n'est pas une valeur de plus sur `stanceState`, c'est un
        // AUTRE GROUPE du même canal.**
        // ── BISSECTÉ LE 2026-08-19 : UNE SEULE DES TROIS VOIES AGIT ───────────────────────
        //
        // Les trois mécanismes ont été poussés ensemble pour obtenir l'effet — c'était le bon
        // choix pour SORTIR de trois semaines d'impasse. Mais « ça marche » ne dit pas « lequel »,
        // et la question n'était pas cosmétique : `highLevelState = Stealth` change l'état de
        // COMPORTEMENT du pantin, ce qui déborde très largement d'une posture.
        //
        // Trois tours, un mécanisme actif par tour, rechargement à chaud entre chaque (~10 s),
        // fantôme `--allure 4`, verdict sur capture au même cadrage :
        //
        //   | tour | voie active                       | avatar   |
        //   | A    | `stanceState`                     | DEBOUT   |
        //   | B    | `highLevelState`                  | DEBOUT   |
        //   | C    | poids du wrapper `stealthLocomotion` | **ACCROUPI** |
        //
        // **C'est le poids du wrapper, et lui seul.** Les deux écritures de trait — celles-là
        // mêmes qu'on a passé trois semaines à faire aboutir — n'y sont pour rien.
        //
        // ⚠️ CE QUE ÇA DIT DU GRAPHE, ET QUI VAUT AU-DELÀ DE L'ACCROUPI. `stanceState` et
        // `highLevelState` sont des ENTRÉES D'ÉTAT : elles renseignent une machine qui décide
        // ensuite. Un `AnimWrapper` est une COUCHE D'ANIMATION dont on règle le poids — il ne
        // demande rien à personne, il se mélange. Sur un pantin que nous pilotons de l'extérieur,
        // aucune machine d'état ne tourne pour consommer les premières ; la seconde, elle,
        // s'applique parce qu'elle ne dépend de rien.
        //
        // C'est la réponse à F-PLY-115 (« la tenaille ») : le corps qui se rend n'accepte aucune
        // pose **par entrée d'état**, et il en accepte par **poids de wrapper**. La frontière
        // n'était pas entre deux corps, elle était entre deux natures de commande.
        //
        // ⚠️ L'accroupi EST la locomotion furtive dans ce jeu — le wrapper porte le jeu
        // d'animations correspondant, et c'est ce qu'on veut. La différence avec la voie B est
        // qu'on n'a PAS touché à l'état de comportement de l'avatar : il ne « devient » pas
        // furtif, il en emprunte la silhouette.
        AnimationControllerComponent.SetAnimWrapperWeightOnOwnerAndItems(
            puppet, n"stealthLocomotion", accroupi ? 1.0 : 0.0);
        return true;
    }

    // Pousse UN entier dans UN groupe du graphe d'animation, par la recette du jeu.
    //
    // ── POURQUOI GÉNÉRIQUE, ET POURQUOI C'EST L'INSTRUMENT QUI MANQUAIT ────────────────────
    //
    // Trois semaines de sondes sur `stanceState` n'ont jamais pu distinguer « cette entrée-là ne
    // répond pas » de « ce canal est mort sur nos pantins ». Il fallait pour ça pousser un AUTRE
    // groupe — contrôle qu'aucune sonde ne permettait, parce que chaque voie était écrite en dur.
    //
    // `AnimFeature_NPCState` ne porte qu'un `Int32 state`, et c'est la classe que le graphe déclare
    // pour `stanceState`, `highLevelState` ET `upperBodyState` (lu dans les `animAnimFeatureEntry`
    // du graphe livré). Une seule fonction couvre donc les trois, et tout groupe futur de la même
    // forme. `reds_reload` la rend rejouable en ~10 s, sans relancer le jeu.
    //
    // ⚠️ LE NOM DU CHAMP EST LA CLÉ, PAS SEULEMENT LE NOM DU GROUPE. `ApplyFeature` apparie
    // champ ↔ nœud PAR LE NOM : la classe doit exposer un champ nommé comme le nœud visé
    // (`state`). C'est ce qui a fait échouer `AnimFeature_Stance` d'août au 2026-08-18 — son seul
    // champ s'appelle `stanceState`, jamais `state`, donc rien ne s'écrivait (F-PLY-133).
    //
    // ⚠️ NE PAS APPELER PAR FRAME. Une écriture de graphe par avatar et par frame est le régime
    // qui a fait tomber le jeu deux fois le 2026-08-06 : les appelants poussent SUR CHANGEMENT.
    public func TesseraPousserTrait(entityId: EntityID, groupe: CName, valeur: Int32) -> Bool {
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }
        let trait = new AnimFeature_NPCState();
        trait.state = valeur;
        AnimationControllerComponent.ApplyFeature(puppet, groupe, trait);
        return true;
    }

    // ── LE SAUT A UN NOM, ET CE N'EST PAS « JUMP » (F-PNJ-164, 2026-08-19) ─────────────────
    //
    // F-PNJ-163 concluait le 2026-08-18 : « aucune des 51 entrées d'animation du graphe n'est un
    // saut ». Vrai sur le NOM — ni `jump`, ni `inAir`, ni `fall`. Mais le groupe `exploration`
    // était dans la liste, et personne n'avait ouvert son énumération :
    //
    //     moveExplorationType { None, Ladder, Jump = 2, Climb, Vault, ChargedJump, ThrusterJump }
    //     moveLocomotionAction { Undefined, Exploration = 1, Idle, IdleTurn, Reposition, … }
    //
    // Le sélecteur que le graphe « ne laissait toucher à personne » s'appelle donc
    // `exploration.explorationType`, et la classe qui le porte existe au RTTI :
    // `animAnimFeature_NPCExploration { explorationType, state, movementType, isEvenLoop,
    // playbackTime }`. Les clips, eux, étaient déjà là — `jump_walk_*`, `jump_sprint_*`,
    // `jump_idle_*`, chacun en variante `low` ET `high`, ce qui donne raison à l'intuition de
    // Lucas (« c'est peut-être en fonction de la hauteur »).
    //
    // La position du saut est répliquée depuis ce matin (F-PLY-117, amplitude relue 1,500 m) :
    // ce qui manque est uniquement l'ANIMATION. On la demande donc au moment où l'avatar monte.
    //
    // ⚠️ DEUX ÉCRITURES, PAS UNE. Le graphe sélectionne sur `locomotion.action = Exploration`
    // AVANT de regarder `exploration.explorationType` — annoncer le type de franchissement sans
    // dire qu'on franchit ne devrait rien produire. Les deux partent ensemble ou ne servent à rien.
    //
    // ⚠️ NON MESURÉ. C'est une recette lue dans la donnée et les scripts décompilés, pas un
    // résultat. Le verdict est à l'œil, sur un avatar EN L'AIR. Si elle échoue, la suite est
    // nommée : `ExplorationEnteredEvent { type : moveExplorationType }`, un événement natif du
    // moteur, instanciable par son nom RTTI comme `entAnimInputSetterAnimFeature` l'a été.
    // ── L'ARME EN MAIN : LA POSTURE, PAR LA MÊME PORTE QUE L'ACCROUPI ──────────────────────
    //
    // L'arme du joueur voyage déjà sur le fil (`AppearanceSpec.garments`, un `EquippedItem` avec
    // son `drawn`) et **arrive** chez l'observateur : `HandleAppearanceSync` renseigne
    // `appearance.arme`. Personne ne la consommait — troisième fois de la journée qu'une donnée
    // traverse tout le fil pour mourir à l'arrivée, après la posture (poussée par personne hors de
    // la sonde T7) et le canal d'événements (reçu et jeté).
    //
    // ⚠️ ON POUSSE UNE COUCHE, PAS UN ÉTAT — et c'est le résultat de la bissection du même jour
    // (F-PLY-138). `upperBodyState`, que la spec du matin désignait pour ce geste, est une
    // **entrée d'état** : elle renseigne une machine qui, sur nos pantins, ne tourne pas. Le
    // wrapper `WeaponRight` est une **couche** dont on règle le poids — il ne demande rien à
    // personne. C'est la voie qui a produit l'accroupi, et c'est celle du jeu lui-même
    // (`NPCPuppet.SetAnimWrapperBasedOnEquippedItem`, `NPCPuppet.script:1080`).
    //
    // ⚠️ CE QUE ÇA DONNE, ET CE QUE ÇA NE DONNE PAS. Le poids du wrapper pose la **tenue** — bras,
    // buste, port de l'arme. Il ne fait pas apparaître l'objet dans la main : ça, c'est la chaîne
    // d'apparence, et elle n'attache aucun item aujourd'hui. Un avatar dégainé aura donc la
    // silhouette juste et les mains vides tant que l'attachement n'est pas fait. C'est un progrès
    // partiel assumé, pas un oubli — et il vaut mieux qu'un avatar au repos pendant que son joueur
    // vise.
    public func TesseraPousserArme(entityId: EntityID, degainee: Bool) -> Bool {
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }
        AnimationControllerComponent.SetAnimWrapperWeightOnOwnerAndItems(
            puppet, n"WeaponRight", degainee ? 1.0 : 0.0);
        return true;
    }

    // Habille l'avatar avec ce que le SERVEUR annonce. Rend `true` quand tout est en place.
    //
    // ⚠️ Le corps de la fonction vit dans `HabillageAvatar.reds` ; ce pont existe parce que
    // `Red::CallVirtual` ne sait appeler qu'une méthode DE CETTE CLASSE, et que c'est le C++
    // (`PiloterAvatar`) qui déclenche — le seul chemin dont on sait qu'il atteint les corps nés de
    // la voie enrichie. Le hook `OnGameAttached(ScriptedPuppet)`, lui, ne s'y déclenche jamais
    // (mesuré le 2026-08-24 : zéro ligne de journal sur deux instances).
    public func TesseraHabillerAvatar(entityId: EntityID, passe: Uint32) -> Bool {
        return TesseraHabillerLeCorps(entityId, passe);
    }

    public func TesseraPousserFranchissement(entityId: EntityID, enVol: Bool) -> Bool {
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }

        // ⚠️ PAS D'`ApplyFeature` ICI, ET CE N'EST PAS UN CHOIX — c'est une contrainte mesurée.
        // Les classes que le graphe attend pour ces deux groupes, `animAnimFeature_NPCExploration`
        // et `animAnimFeature_Locomotion`, existent au RTTI mais **ne sont déclarées dans AUCUN
        // script décompilé** : redscript ne peut donc pas les construire. C'est le piège de
        // nommage de la skill pris dans l'autre sens — le dump ne fait pas foi sur les noms
        // scriptés, mais l'inverse est vrai aussi : ce qu'il liste n'est pas forcément scriptable.
        // (`AnimFeature_NPCState`, elle, existait — d'où la voie A de la posture.)
        //
        // `SetInputInt` n'a besoin d'aucune classe : il écrit le nœud directement. C'est donc la
        // SEULE voie scriptée vers ces deux groupes, et cela ne coûte rien puisqu'elle est de
        // toute façon celle qu'on teste.
        //
        // `moveLocomotionAction` : Exploration = 1 · Idle = 2 (le retour au sol).
        // `moveExplorationType`  : Jump = 2 · None = 0.
        //
        // ⚠️ `action` et `explorationType` seulement — PAS `state`. Ces deux noms-là sont uniques
        // dans le graphe ; `state` y apparaît **dix-huit fois**, réparti sur autant de machines
        // d'état sans rapport. Le pousser reviendrait à écrire dans dix-sept d'entre elles au
        // hasard (voir la note de `TesseraPousserPosture`). Le groupe `exploration` porte bien un
        // `state`, mais on ne peut pas le viser par son nom sans viser tous les autres.
        AnimationControllerComponent.SetInputInt(puppet, n"action", enVol ? 1 : 2);
        AnimationControllerComponent.SetInputInt(puppet, n"explorationType", enVol ? 2 : 0);
        return true;
    }

    // ── L'INSTRUMENT QUI MANQUAIT : CE QUE LE MOTEUR CROIT ÊTRE EN TRAIN DE FAIRE ──────────
    //
    // Tout ce qu'on sait aujourd'hui d'un avatar distant, on le déduit de l'extérieur : sa
    // position relue, sa dérive, le nombre d'ordres émis. **Rien ne dit ce que le moteur pense
    // faire.** C'est pourquoi « l'accroupi ne marche pas » et « le saut ne marche pas » ont mis
    // trois semaines à se distinguer d'un avatar qu'on ne pilotait tout simplement pas.
    //
    // `MovePoliciesComponent` expose deux LECTEURS (F-PLY-123), et ils valent autant que ses
    // commandes :
    //
    //   `GetCurrentLocomotionAction()` → `moveLocomotionAction`
    //        0 Undefined · 1 Exploration · 2 Idle · 3 IdleTurn · 4 Reposition · 5 Start · 6 Move
    //        · 7 Stop  — c'est-à-dire l'état de la machine de déplacement, à la source.
    //   `GetExplorationOffMeshLinkType()` → `moveExplorationType`
    //        0 None · 1 Ladder · 2 Jump · 3 Climb · 4 Vault · 5 ChargedJump · 6 ThrusterJump
    //        — dit si le moteur est en train de FRANCHIR quelque chose, donc de sauter.
    //
    // ⚠️ Le second est le contrôle négatif du chantier saut. Si l'on pousse
    // `exploration.explorationType = Jump` et que ce lecteur rend toujours `None`, on sait que
    // l'écriture n'atteint pas la machine — au lieu de le déduire d'une capture floue.
    //
    // ⚠️ L'ACCESSEUR S'ÉCRIT `GetMovePolicesComponent`, SANS LE « i ». C'est la faute de frappe de
    // CDPR (`scriptedPuppet.script:1299`) ; la bonne orthographe ne résout pas.
    //
    // Encodage du retour, pour ne coûter qu'un entier sur le fil de la télémétrie :
    //   `action * 10 + exploration`, et **-1** si le composant est injoignable — ce qui est un
    //   résultat, pas une absence de résultat.
    // ── MESURER L'ACCROUPI AU LIEU DE LE REGARDER ─────────────────────────────────────────
    //
    // Le verdict « le pantin est-il accroupi ? » était jusqu'ici réputé inaccessible à un
    // instrument : il fallait un œil. Le 2026-08-19 a montré que l'œil ne suffit pas non plus —
    // les captures de l'avatar distant montrent ses CHEVEUX en travers de l'objectif, des mèches
    // étirées jusqu'à la caméra, alors que l'entité est à 6,50 m, dead centre, écart au cap 0°.
    // Quarante secondes de stabilisation n'y changent rien. Aucune image n'est exploitable, et
    // aucun chiffre existant ne le signale : la garde de cadrage la déclare valide.
    //
    // Or la hauteur de la TÊTE est une grandeur, pas un jugement. `SlotComponent.GetSlotTransform`
    // rend la transformation monde d'un slot nommé, et `'Head'` en est un (utilisé par le jeu pour
    // viser la tête, `aiActionHelper.script:177`). La différence entre la tête et la racine de
    // l'entité — c'est-à-dire les pieds — donne la hauteur du pantin :
    //
    //     debout    ~1,70 m        accroupi    ~1,20 m        écart attendu ~0,50 m
    //
    // C'est binaire, autonome, et immunisé contre tout défaut de rendu. Un pantin dont la tête ne
    // descend pas n'est pas accroupi, quelle que soit l'image.
    //
    // Rend -1.0 si l'entité, le composant ou le slot manquent — ce qui est un résultat, pas une
    // absence de résultat : un avatar sans `SlotComponent` est un fait qu'on veut lire.
    // ⚠️ `Int32`, ET C'EST LE CORRECTIF LUI-MÊME — pas un choix de confort.
    //
    // Cette fonction rendait un `Float`, et le C++ relisait **0.00** quelle que soit la branche
    // prise : ni la valeur, ni aucune des trois sentinelles (-1 entité, -2 composant, -3 slot) ne
    // ressortait. Trois échecs distincts derrière un seul zéro, et l'instrument muet pendant toute
    // la journée du 2026-08-19 — ce qui a forcé à juger l'accroupi **à l'œil** alors qu'il avait
    // été conçu pour ne plus l'être (F-PLY-129).
    //
    // Le suspect : `TesseraZTete` est le **seul** des quinze appels C++→redscript de
    // `NetworkGameSystem.cpp` à renvoyer un `Float`. Les quatorze autres rendent `Bool` ou `Int32`
    // et fonctionnent. On ne cherche pas pourquoi le marshalling du flottant échoue : on emprunte
    // le type dont l'effet est établi.
    //
    // En **centimètres**, ce qui supprime au passage la conversion `× 100` que le C++ faisait —
    // une multiplication en moins et une unité dans le nom.
    public func TesseraZTeteCm(entityId: EntityID) -> Int32 {
        // ── ⚠️ CET INSTRUMENT A RENDU `z=0.00` LE 2026-08-19, ET 0 N'EST PAS UNE DE SES SORTIES ──
        //
        // Relevé dans `tessera-telemetrie-3460.jsonl` : `hauteur_echec | appel=1,z=0.00`. Le C++
        // initialise `zTete` à **-1.0** et `Red::CallVirtual` a rendu **true**. Quelque chose a
        // donc bien écrit 0 — alors qu'aucun chemin de cette fonction ne rend 0.
        //
        // DEUX CAUSES POSSIBLES, ET ELLES APPELLENT DES CORRECTIFS OPPOSÉS :
        //
        //   H1 — le MARSHALLING du retour `Float`. `TesseraZTete` est le SEUL des quatorze appels
        //        C++→redscript de `NetworkGameSystem.cpp` à rendre un `Float` ; les treize autres
        //        rendent `Bool` ou `Int32`. Ce type n'a donc jamais été prouvé sur ce chemin, et
        //        un retour non marshallé arrive naturellement à zéro.
        //   H2 — `GetSlotTransform('Head')` rend **true** avec une transform nulle sur un pantin
        //        distant (slot déclaré, jamais résolu).
        //
        // ⚠️ LE CORRECTIF NE TRANCHE PAS, IL FAIT PARLER — c'est délibéré. Corriger au hasard
        // l'une des deux aurait produit, en cas d'échec, exactement le même `z=0.00` : le piège
        // des deux défauts qui se masquent (ADR 0034, décision nº 3). Les trois sorties d'échec
        // deviennent donc DISTINCTES, et la prochaine campagne tranche toute seule :
        //
        //     z = -1.00  → l'entité ne se résout pas          ) H2 confirmée : le marshalling
        //     z = -2.00  → pas de SlotComponent               ) marche, la cause est le slot
        //     z = -3.00  → slot 'Head' non résolu             )
        //     z =  0.00  → AUCUN de ces chemins n'a pu écrire → H1 : c'est le retour `Float`,
        //                  et le correctif est de passer à `Int32` en centimètres (le type que
        //                  `TesseraLireLocomotion` prouve tous les jours), ce qui supprime au
        //                  passage la conversion `×100` déjà faite côté C++.
        //
        // Le C++ n'a pas besoin de changer : sa branche d'échec teste `zTete <= 0.0f`, donc les
        // trois sentinelles ressortent telles quelles dans `hauteur_echec`, et son `%.2f` les
        // affiche. Aucun rebuild de DLL — donc aucune collision avec l'agent qui tient le jeu.
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return -1;
        }
        let slots = puppet.GetSlotComponent();
        if !IsDefined(slots) {
            return -2;
        }
        let transformation: WorldTransform;
        if !slots.GetSlotTransform(n"Head", transformation) {
            return -3;
        }
        // ⚠️ ON REND LE Z ABSOLU, ET LA SOUSTRACTION SE FAIT CÔTÉ C++.
        //
        // La première version rendait `tete.Z - puppet.GetWorldPosition().Z`, et le relevé donnait
        // **2 468 cm** — vingt-quatre mètres, c'est-à-dire le Z monde de la tête tout entier.
        // `GetWorldPosition()` rend donc **zéro** sur un pantin distant : la soustraction ne
        // soustrayait rien, et le chiffre restait pourtant parfaitement plausible tant qu'on ne
        // le rapprochait pas de l'altitude du sol.
        //
        // Le C++, lui, lit la position de l'entité de façon fiable (`Entity_GetWorldPosition`) et
        // la lit déjà partout ailleurs. On lui laisse la soustraction plutôt que de chercher
        // pourquoi l'accesseur scripté ment — c'est la même donnée, obtenue par le chemin dont
        // l'effet est établi.
        // Le Z ABSOLU de la tête, en centimètres. La soustraction du sol se fait à l'ANALYSE,
        // qui lit le Z des lignes `rx` du même intervalle — un calcul fait à la mesure peut
        // échouer en silence, un calcul fait à l'analyse se rejoue sur des données acquises.
        return Cast<Int32>(WorldPosition.ToVector4(WorldTransform.GetWorldPosition(transformation)).Z * 100.0);
    }

    // ── LE CONTRÔLE QUI MANQUAIT, SANS AVOIR À VISER QUI QUE CE SOIT ───────────────────────
    //
    // F-PLY-132 a laissé la question ouverte : *un piéton **natif** en marche atteint-il `Move` ?*
    // Le premier essai désignait le sujet par `GetLookAtObject` — le seul désignateur du harnais —
    // et attrapait donc statistiquement un badaud immobile. Il fallait une énumération.
    //
    // Elle existe, et depuis toujours : `GameObject.GetEntitiesAroundObject(rayon, filtre)`
    // (`gameObject.script:935`), qui monte une `TargetSearchQuery` et rend les entités. Le filtre
    // `TSF_NPC()` ne garde que les pantins.
    //
    // ⚠️ ON REND UN MASQUE, PAS UNE VALEUR. La question n'est pas « que fait ce PNJ-ci » mais
    // « **l'état `Move` apparaît-il chez quelqu'un** ». Un masque de bits — un bit par valeur de
    // `moveLocomotionAction` — répond à ça sur toute la population d'un coup, tient dans un entier,
    // et se lit sans ambiguïté :
    //
    //     bit 0 Undefined · bit 1 Exploration · bit 2 Idle · bit 3 IdleTurn
    //     bit 4 Reposition · bit 5 Start · **bit 6 Move** · bit 7 Stop
    //
    // Un masque qui ne contient jamais le bit 6 sur des centaines de relevés et des dizaines de
    // piétons est une réponse ; un pantin unique qui rend `Undefined` n'en est pas une.
    //
    // Rend -1 si le joueur ou le système manquent — un résultat, pas une absence de résultat.
    public func TesseraEtatsLocomotionAutour(rayon: Float) -> Int32 {
        let joueur = GameInstance.GetPlayerSystem(GetGameInstance()).GetLocalPlayerControlledGameObject();
        if !IsDefined(joueur) {
            return -1;
        }
        // ⚠️ REDSCRIPT N'A PAS D'OPÉRATEUR DE DÉCALAGE. `1 << action` est une erreur de SYNTAXE,
        // pas un avertissement — et une erreur de syntaxe fait tomber **tout** `r6/scripts`, donc
        // le jeu ne finit jamais de charger et le watchdog du moteur le tue au bout de 120 s
        // (`engineWatchdog.cpp:198`). Vécu deux fois le 2026-08-19, et le symptôme — « aucun
        // journal neuf » — ne dit rien de sa cause : c'est `r6/logs/redscript_rCURRENT.log` qui la
        // porte, et lui seul.
        //
        // On compose donc le masque en arithmétique pure : un tableau de drapeaux, puis une
        // puissance de deux accumulée. Plus long, et sans opérateur exotique.
        let entites = joueur.GetEntitiesAroundObject(rayon, TSF_NPC());
        let vus: array<Bool>;
        ArrayResize(vus, 8);
        let i = 0;
        while i < ArraySize(entites) {
            let pantin = entites[i] as ScriptedPuppet;
            if IsDefined(pantin) {
                let politiques = pantin.GetMovePolicesComponent();
                if IsDefined(politiques) {
                    let action = EnumInt(politiques.GetCurrentLocomotionAction());
                    if action >= 0 && action < 8 {
                        vus[action] = true;
                    }
                }
            }
            i += 1;
        }
        let masque: Int32 = 0;
        let puissance: Int32 = 1;
        let k = 0;
        while k < 8 {
            if vus[k] {
                masque += puissance;
            }
            puissance *= 2;
            k += 1;
        }
        return masque;
    }

    public func TesseraLireLocomotion(entityId: EntityID) -> Int32 {
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return -1;
        }
        let politiques = puppet.GetMovePolicesComponent();
        if !IsDefined(politiques) {
            return -1;
        }
        return EnumInt(politiques.GetCurrentLocomotionAction()) * 10
             + EnumInt(politiques.GetExplorationOffMeshLinkType());
    }

    // L'allure du fil traduite en `moveMovementType`, pour la commande ET pour la mutation.
    //
    // ⚠️ Un seul endroit, délibérément : ces deux chemins doivent toujours dire la même chose.
    // Les valeurs accroupies (4, 5) et l'air (6) n'ont pas d'équivalent dans `moveMovementType` —
    // elles retombent sur la marche, le moins faux des choix tant que la posture n'a pas de voie
    // (F-PLY-127 : les deux portes du composant d'animation sont fermées).
    private func AllureDepuisLocomotion(locomotion: Int32) -> moveMovementType {
        if locomotion == 3 {
            return moveMovementType.Sprint;
        }
        if locomotion == 2 {
            return moveMovementType.Run;
        }
        return moveMovementType.Walk;
    }

    public func TesseraSuivreAvatar(entityId: EntityID, visee: Vector4, locomotion: Int32, yaw: Float) -> Bool {
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }
        let controller = puppet.GetAIControllerComponent();
        if !IsDefined(controller) {
            return false;
        }

        // ── ANNULER LE GEL AVANT DE REPARTIR — SINON ON ATTEND QU'IL EXPIRE ────────────────
        //
        // Signalé par Lucas le 2026-08-14 : « un délai très important entre le moment où je fais
        // mes actions et le moment où c'est répercuté chez l'autre joueur ».
        //
        // `TesseraFigerAvatar` envoie un `AIHoldPositionCommand` de **1 seconde** quand le joueur
        // s'arrête. Une commande d'IA ne se REMPLACE pas : elle s'exécute. Le `AIMoveToCommand`
        // envoyé au redémarrage se mettait donc EN FILE derrière le gel, et le pantin ne bougeait
        // qu'à l'expiration de celui-ci. Un joueur qui s'arrête puis repart dans la seconde —
        // c'est-à-dire le régime NORMAL du déplacement humain — payait jusqu'à une seconde pleine
        // de retard, par-dessus tout le reste de la chaîne.
        //
        // `CancelOrInterruptCommand` annule par NOM DE CLASSE (`aiComponent.script:14`), ce qui
        // évite de retenir un identifiant de commande de notre côté. `useInheritance = true`
        // couvre les sous-classes ; `success = false` dit que le gel n'a pas abouti — c'est exact,
        // on l'interrompt.
        controller.CancelOrInterruptCommand(n"AIHoldPositionCommand", true, false);

        // ── ET ANNULER LA MARCHE PRÉCÉDENTE, POUR EXACTEMENT LA MÊME RAISON ────────────────
        //
        // La ligne au-dessus annule le gel parce qu'« une commande d'IA ne se remplace pas : elle
        // s'exécute ». Le raisonnement vaut mot pour mot pour la commande de marche elle-même, et
        // il n'avait pas été appliqué : chaque réémission s'empilait DERRIÈRE la précédente.
        //
        // À 10 Hz, on construisait donc une file d'ordres de marche vers des points périmés, que
        // le pantin parcourait l'un après l'autre — un chemin d'il y a une seconde, à une vitesse
        // effective bien inférieure à l'allure demandée.
        //
        // MESURE (2026-08-17, fantôme rejoueur, personne au clavier) : allure commandée **Run**
        // (~4 m/s), vitesse réellement parcourue par l'avatar **1,37 m/s médiane** — une vitesse
        // de marche — et une dérive qui se creuse de **+0,17 m/s**, jusqu'à 19 m en 40 s. Le
        // symptôme est exactement celui décrit dans `protocol.fbs` pour les PNJ le 2026-08-06
        // (« la commande suivante arrive avant qu'une marche ait pu s'amorcer »), et c'est la
        // quatrième fois de la journée que la même file explique un défaut différent.
        //
        // ⚠️ Ce que ça ne dit pas : que réémettre à 10 Hz soit devenu gratuit. La garde
        // d'anti-réémission reste indispensable — annuler puis rejouer soixante fois par seconde
        // hacherait l'animation. Elle borne la casse ; cette ligne empêche l'accumulation.
        // ── LA MUTATION DE POLITIQUE A ÉTÉ ESSAYÉE ICI, ET ELLE EST RÉFUTÉE ───────────────
        //
        // F-PLY-130 a mesuré qu'un avatar qui marche n'atteint **jamais** `Move` : sa machine
        // oscille entre `Idle` et `Start`, parce que l'annulation ci-dessous la redémarre à chaque
        // cycle. L'inférence semblait évidente — muter la destination en place
        // (`MovePolicies.SetDestinationPosition` + `ChangeMovementType`, F-PLY-123) au lieu de
        // réémettre, pour que la machine entre en régime.
        //
        // ⚠️ ELLE A ÉTÉ IMPLÉMENTÉE, MESURÉE, ET ELLE DÉGRADE TOUT (F-PLY-131) :
        //
        //     |            | réémission | mutation |
        //     | allure     | 3,40 m/s   | 1,51 m/s |
        //     | dérive méd | 1,84 m     | 17,27 m  |
        //
        // …et la machine atteignait bien `Move` (6 relevés, `Idle` disparu). **Le proxy était
        // faux** : `Move` n'est pas l'objectif, c'est une conséquence d'un régime qu'on ne veut
        // pas. Repose `SetIgnoreNavigation(true)` à la mutation n'y change rien (1,47 m/s,
        // 18,44 m) — donc ce n'est pas la navigation qui se perdait.
        //
        // Ce que la réémission apporte et que la mutation ne reproduit pas n'est **pas identifié**.
        // Ne pas re-tenter sans avoir d'abord répondu à ça : la voie a coûté deux campagnes.
        controller.CancelOrInterruptCommand(n"AIMoveToCommand", true, false);

        let cmd = new AIMoveToCommand();
        let cible: AIPositionSpec;
        let wp: WorldPosition;
        // ⚠️ `AIPositionSpec.SetWorldPosition` attend un `WorldPosition` (virgule fixe), JAMAIS un
        // `Vector4` — piège déjà payé et consigné au backlog Q6.
        WorldPosition.SetVector4(wp, visee);
        AIPositionSpec.SetWorldPosition(cible, wp);
        cmd.movementTarget = cible;

        // Même correspondance que pour les PNJ. Les valeurs accroupies (4, 5) et l'air (6) n'ont
        // pas d'équivalent dans `moveMovementType` : elles retombent sur la marche — le moins faux
        // des choix tant que le backlog Q7 n'a pas tranché le geste.
        //
        // ⚠️ `movementType` ne montre son effet QU'AVEC DE LA DISTANCE à couvrir (mesuré
        // 2026-07-23) : tout près, le pantin marchote quelle que soit l'allure. C'est ce piège qui
        // a produit un premier verdict « allure ignorée » entièrement faux. Le point de visée à 3 m
        // est précisément ce qui donne cette distance.
        cmd.movementType = this.AllureDepuisLocomotion(locomotion);

        // Où il REGARDE, distinct de où il VA. C'est ce qui donne la marche arrière et le pas de
        // côté : le graphe d'animation compare les deux et choisit l'allure lui-même.
        let regard: AIPositionSpec;
        let wpRegard: WorldPosition;
        WorldPosition.SetVector4(wpRegard, this.PointDeRegard(puppet.GetWorldPosition(), yaw));
        AIPositionSpec.SetWorldPosition(regard, wpRegard);
        cmd.facingTarget = regard;
        cmd.rotateEntityTowardsFacingTarget = true;

        cmd.ignoreNavigation = true;
        cmd.finishWhenDestinationReached = false;
        cmd.desiredDistanceFromTarget = 0.0;
        cmd.useStart = true;
        cmd.useStop = true;

        controller.SendCommand(cmd);
        return true;
    }

    // FIGE un avatar sur place : annule sa marche en cours et le tient immobile.
    //
    // ── POURQUOI CETTE FONCTION EXISTE ─────────────────────────────────────────────────────
    //
    // Observé par Lucas le 2026-08-13, quand plusieurs instances chargent en même temps : « il y a
    // des micro-coupures et le PNJ reprend la main sur le joueur » — l'avatar se met à marcher tout
    // seul, comme un passant.
    //
    // Ce n'est pas le moteur qui reprend la main, c'est NOUS qui ne la lâchons pas. La commande de
    // marche est CONTINUE et NON TERMINANTE par conception (c'est ce qui produit une locomotion
    // fluide). Quand le fil se tait, plus rien ne la remplace : le moteur continue d'exécuter le
    // dernier ordre reçu, c'est-à-dire de marcher vers un point de visée périmé.
    //
    // Un avatar figé est un défaut VISIBLE et honnête — le joueur d'en face comprend que quelqu'un
    // a lagué. Un avatar qui part en promenade est un défaut MENSONGER : il raconte une action que
    // personne n'a faite, et en RP c'est bien pire.
    //
    // `AIHoldPositionCommand` avec une durée courte, réémise tant que le fil se tait : elle
    // remplace la commande de marche dans la file, donc elle l'annule de fait.
    public func TesseraFigerAvatar(entityId: EntityID) -> Bool {
        // ⚠️⚠️ LA VOIE DE RESOLUTION ETAIT FAUSSE, ET CE GEL N A DONC JAMAIS FONCTIONNE.
        //
        // Mesure du 2026-08-28, les trois conditions testees separement sur un avatar distant :
        //
        //   voie DYNAMIQUE  : entite=false  pantin=false  controleur=false
        //   voie RECHERCHE  : entite=true   pantin=true   controleur=true
        //
        // `GetDynamicEntitySystem().GetEntity()` rend NULL sur nos avatars, alors que
        // `FindEntityByID` les trouve — avec leur pantin et leur controleur d'IA. La fonction
        // sortait donc sur son premier `return false`, en silence, depuis le debut.
        //
        // ⚠️ La portee depasse le chantier ascenseurs : c'est le gel sur FIL MUET (400 ms sans
        // nouvelles). Il n'a jamais fige personne — un avatar dont le fil se tait continuait
        // d'executer sa derniere commande de marche, c'est-a-dire de se promener. Exactement le
        // defaut que ce code disait corriger depuis aout.
        //
        // On garde la voie dynamique en REPLI : elle est peut-etre la bonne pour d'autres corps,
        // et un remede ne doit pas retirer ce qui marchait ailleurs.
        let entity = GameInstance.FindEntityByID(GetGameInstance(), entityId);
        if !IsDefined(entity) {
            entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        }
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }
        let controller = puppet.GetAIControllerComponent();
        if !IsDefined(controller) {
            return false;
        }
        // ── ANNULER LA MARCHE AVANT DE GELER — SINON LE GEL FAIT LA QUEUE DERRIÈRE ELLE ────
        //
        // Le miroir exact de ce que `TesseraSuivreAvatar` fait déjà dans l'autre sens, et il
        // manquait ici. Une commande d'IA ne se REMPLACE pas : elle s'exécute. Envoyer un
        // `AIHoldPositionCommand` pendant qu'un `AIMoveToCommand` tourne ne l'interrompt donc pas
        // — le gel se met EN FILE, et le pantin finit tranquillement sa marche vers le point de
        // visée d'avant, à six mètres devant.
        //
        // C'est très précisément le symptôme rapporté par Lucas le 2026-08-13 : « quand on arrête
        // de marcher, après quelques mètres le PNJ reprend ses droits et se met à marcher tout
        // seul ». Le gel avait été ajouté POUR ça, et il ne pouvait pas y suffire.
        //
        // MESURE (2026-08-17, fantôme rejoueur en maintien, personne au clavier) : à l'arrêt,
        // l'avatar continue de s'éloigner à ~0,8 m/s pendant **1,8 s** — 1,2 m parcourus après
        // l'ordre d'arrêt — puis saute d'un coup sur sa cible et y reste parfaitement immobile.
        // Ce profil ne s'explique que par une file : notre placement attendait son tour derrière
        // la marche, exactement comme le gel.
        //
        // `useInheritance = true` couvre les sous-classes ; `success = false` dit que la marche
        // n'a pas abouti — c'est exact, on l'interrompt.
        controller.CancelOrInterruptCommand(n"AIMoveToCommand", true, false);

        let cmd = new AIHoldPositionCommand();
        cmd.duration = 1.0;
        controller.SendCommand(cmd);
        return true;
    }

    public func StopAICommand(puppet: ref<ScriptedPuppet>, command: ref<AICommand>) {
        let component = puppet.GetAIControllerComponent();
        if (EnumInt(component.GetCommandState(command)) != EnumInt(AICommandState.Success)) {
            component.CancelCommand(command);
        }
    }
}

// Vérificateur de cadavre — garantit que la mort ORDONNÉE par le serveur a bien EU LIEU.
//
// ⚠️ Il existe parce que « l'appel n'a pas échoué » n'est pas « l'effet s'est produit », et que ce
// dépôt a déjà payé cette confusion plusieurs fois (D1). `ScriptedPuppet.Kill` est différé d'au
// moins une frame : le vérifier tout de suite ne prouve rien, et ne pas le vérifier du tout laisse
// un cadavre debout sans que personne ne l'apprenne.
//
// Trois issues, toutes journalisées — c'est le point : aucune ne peut passer inaperçue.
//   · mort constatée          → une ligne de succès avec le nombre d'essais
//   · pantin disparu          → le destreaming a réglé le problème autrement, on s'arrête
//   · huit essais sans effet  → une ligne d'ALERTE, parce que là c'est un vrai défaut
//
// Se ré-arme TOUJOURS en dernier et sans condition tant qu'il reste des essais — la leçon du
// battement de l'écran de mort (2026-08-09) : un ré-armement enfermé dans un test finit par
// s'arrêter un jour, et plus personne ne comprend pourquoi l'état s'est figé.
public class TesseraVerifieCadavre extends DelayCallback {
    let cible: EntityID;
    let essai: Uint32;

    public static func Creer(cible: EntityID, essai: Uint32) -> ref<TesseraVerifieCadavre> {
        let v = new TesseraVerifieCadavre();
        v.cible = cible;
        v.essai = essai;
        return v;
    }

    public func Call() -> Void {
        let reseau = GameInstance.GetNetworkGameSystem();
        let pantin = GameInstance.FindEntityByID(GetGameInstance(), this.cible) as ScriptedPuppet;

        // Plus de pantin : destreamé ou détruit. Il n'y a plus de corps debout à corriger, donc plus
        // rien à faire — et surtout pas à réessayer indéfiniment sur une entité qui n'existe plus.
        if !IsDefined(pantin) {
            return;
        }
        if pantin.IsDead() {
            if IsDefined(reseau) {
                reseau.Tessera_Journal(s"cadavre confirme apres \(this.essai) essai(s)");
            }
            return;
        }
        // Toujours debout. On relève l'immortalité (elle a pu être reposée à un ré-attachement) et
        // on réordonne la mort — même séquence qu'à l'origine, parce que c'est elle qui marche.
        GameInstance.GetGodModeSystem(GetGameInstance())
            .RemoveGodMode(this.cible, gameGodModeType.Immortal, n"Tessera");
        pantin.Kill(null, false, false);

        if this.essai >= 8u {
            if IsDefined(reseau) {
                reseau.Tessera_Journal(
                    s"⚠ CADAVRE JAMAIS APPLIQUE apres \(this.essai) essais — le corps reste DEBOUT");
            }
            return;
        }
        GameInstance.GetDelaySystem(GetGameInstance())
            .DelayCallback(TesseraVerifieCadavre.Creer(this.cible, this.essai + 1u), 0.25, false);
    }
}

@addMethod(GameInstance)
public static native func GetNetworkGameSystem() -> ref<NetworkGameSystem>

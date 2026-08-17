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
    public native func Tessera_ArmeDeLEntite(cible: EntityID) -> TweakDBID;

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
    public native func Tessera_NomPersonnage(index: Int32) -> String;
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
    public native func Tessera_CreerPersonnage(pseudonyme: String, record: Uint64, apparence: CName, origine: String) -> Bool;
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
    public native func Tessera_CompteAvatarsJoueurs() -> Int32;
    public native func Tessera_AvatarJoueurParIndex(index: Int32) -> EntityID;

    // ── Le sac AUTORITAIRE (ADR 0026) — le serveur énonce, le client s'aligne ────────────────
    //
    // ⚠️ `Tessera_SacRecu` est SÉPARÉ de la taille, et c'est la distinction qui protège les joueurs.
    // Un sac VIDE est un ordre (« tu ne possèdes rien »), l'absence de message n'en est pas un. Les
    // confondre viderait les joueurs de tout serveur qui n'a jamais parlé d'inventaire, puisqu'une
    // taille de 0 se lirait alors comme « retire tout ».
    public native func Tessera_SacRecu() -> Bool;
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

        let state: Int32;
        if !onGround {
            state = 6;                                  // InAir/Jump — signal prioritaire
        } else if loco == 2 {
            state = 3;                                  // Sprint
        } else if loco == 1 {
            state = movingH ? 5 : 4;                    // CrouchMove / CrouchIdle
        } else if detailed == 3 {
            state = 1;                                  // Walk (seul signal fiable de la marche)
        } else if speed > 0.5 {
            state = 2;                                  // Run (repli par la vitesse)
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
        if speed > 0.1 {
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
        ev.request.limits.softLimitDegrees = 360.0;
        ev.request.limits.hardLimitDegrees = 270.0;
        ev.request.limits.backLimitDegrees = 210.0;
        ev.request.calculatePositionInParentSpace = false;   // cible en espace MONDE
        ev.request.priority = 100;                            // au-dessus des reactions locales
        ev.bodyPart = n"Eyes";

        let parts: array<LookAtPartRequest>;
        let tete: LookAtPartRequest;
        tete.partName = n"Head";  tete.weight = 0.1;  tete.suppress = 1.0;  tete.mode = 0;
        ArrayPush(parts, tete);
        let buste: LookAtPartRequest;
        buste.partName = n"Chest"; buste.weight = 2.0; buste.suppress = 0.0; buste.mode = 0;
        ArrayPush(parts, buste);
        ev.SetAdditionalPartsArray(parts);

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

        puppet.GetAIControllerComponent().SendCommand(teleportCommand);
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
    // ── SECONDE PASSE, 2026-08-15 : LA VOIE « SIGNAL DE COMPORTEMENT » ────────────────────
    //
    // La première passe a rendu son verdict, et il est négatif : `ApplyFeature` sur
    // `stanceState` ne montre RIEN (F-PLY-053 — avatar observé DEBOUT par Lucas alors que la
    // sonde tirait toutes les 2 s et rendait `true`). C'est le troisième refus de la même
    // famille, avec F-PLY-036 (l'arme ne s'affiche que par une commande d'IA) et le backlog Q7.
    //
    // La règle qui s'en dégage : **sur un pantin de foule, ce qui se voit passe par un
    // COMPORTEMENT, jamais par une écriture d'état.**
    //
    // `NPCPuppet.ChangeStanceState` appartient justement à l'autre famille, et c'est pour ça
    // qu'elle vaut d'être essayée : elle n'écrit pas une donnée d'animation, elle **émet un
    // signal** (`NPCStateChangeSignal`) dans la table de signaux du pantin, que la machine à
    // états de comportement consomme (`NPCPuppet.script:1360-1380`). C'est la seule voie de la
    // famille « posture » jamais tentée — F-PLY-009, restée en `hypothèse` depuis le 2026-07-23.
    //
    // ⚠️ LE CONTRÔLE EST LA SESSION PRÉCÉDENTE, et c'est ce qui rend la comparaison valide.
    // `ApplyFeature` est conservé ci-dessous, inchangé, et il a été prouvé INERTE une demi-heure
    // plus tôt dans exactement ce montage. Si l'avatar s'accroupit maintenant, le delta n'est
    // imputable qu'à `ChangeStanceState`. Le retirer aurait changé deux choses à la fois.
    //
    // Deux bornes connues, lues au script décompilé :
    //   · la fonction sort tôt si la stance demandée est déjà celle du blackboard — rejouer la
    //     sonde toutes les 2 s est donc sans effet de bord : elle ne fait rien après le premier
    //     coup, et c'est voulu ;
    //   · elle exige un cast vers `NPCPuppet`. Notre avatar est un pantin de foule (F-PLY-007),
    //     donc il devrait passer — mais si le cast échoue, la voie B n'est PAS tentée. D'où le
    //     `return false` distinct : sans lui, « envoye » mentirait sur ce qui a été fait.
    //
    // `NPCPuppet` est bien l'alias redscript (`NPCPuppet.script:119` : `class NPCPuppet extends
    // ScriptedPuppet`), pas un nom natif du dump RTTI — piège documenté dans la skill.
    public func TesseraPousserPosture(entityId: EntityID, accroupi: Bool) -> Bool {
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }

        // Voie A — écriture d'état dans le graphe d'animation. RÉFUTÉE (F-PLY-053), gardée comme
        // TÉMOIN : c'est elle qui rend lisible la comparaison avec la session précédente.
        let posture = new AnimFeature_Stance();
        posture.SetStanceState(accroupi ? animStanceState.Crouch : animStanceState.Stand);
        AnimationControllerComponent.ApplyFeature(puppet, n"stanceState", posture);

        // Voie B — signal de comportement. C'est celle qu'on teste maintenant.
        let pantin = entity as NPCPuppet;
        if !IsDefined(pantin) {
            return false; // cast échoué : la voie B n'a pas été tentée, et il faut le savoir.
        }
        NPCPuppet.ChangeStanceState(pantin, accroupi ? gamedataNPCStanceState.Crouch : gamedataNPCStanceState.Stand);
        return true;
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
        if locomotion == 3 {
            cmd.movementType = moveMovementType.Sprint;
        } else if locomotion == 2 {
            cmd.movementType = moveMovementType.Run;
        } else {
            cmd.movementType = moveMovementType.Walk;
        }

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
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entityId);
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

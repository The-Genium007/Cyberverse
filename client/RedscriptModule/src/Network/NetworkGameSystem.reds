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

    // Journal de SONDE — écrit dans le log du plugin, donc UN FICHIER PAR INSTANCE.
    // `FTLog` écrit dans le gamelog de CET, partagé par toutes les instances : deux clients y
    // mélangent leurs lignes, ce qui interdit toute comparaison entre eux.
    public native func Tessera_Journal(texte: String) -> Void;

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
    public func ApplyServerTime(hours: Int32, minutes: Int32, toleranceMinutes: Int32) -> Bool {
        let ts = GameInstance.GetTimeSystem(GetGameInstance());
        if !IsDefined(ts) {
            return false;
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
            return false;
        }

        ts.SetGameTimeByHMS(hours, minutes, 0);
        return true;
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
        pantin.Kill(null, false, false);
        // ⚠️ `Kill` peut être différé d'une frame : un `false` ici ne veut pas dire échec, il veut
        // dire « pas encore ». L'appelant réessaiera, et ce sera vrai au snapshot suivant.
        return pantin.IsDead();
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
        puppet.GetAIControllerComponent().DisableCollider(); // TODO: Temp - In the future this should be controlled by the server, but currently Judy's just annoying :D 
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

    public func StopAICommand(puppet: ref<ScriptedPuppet>, command: ref<AICommand>) {
        let component = puppet.GetAIControllerComponent();
        if (EnumInt(component.GetCommandState(command)) != EnumInt(AICommandState.Success)) {
            component.CancelCommand(command);
        }
    }
}

@addMethod(GameInstance)
public static native func GetNetworkGameSystem() -> ref<NetworkGameSystem>

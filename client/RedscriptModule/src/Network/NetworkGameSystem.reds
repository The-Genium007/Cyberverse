module Cyberverse.Network.Managers
import Codeware.*

public native class NetworkGameSystem extends IGameSystem {
    //public native func ConnectToServer(host: String, port: Uint16) -> Void;
    native let FullyConnected: Bool;
    native let playerActionTracker: ref<PlayerActionTracker>;
    public native func EnqueueLoadLastCheckpoint(handler: wref<inkISystemRequestsHandler>) -> Void;

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
    public func ApplyServerTime(hours: Int32, minutes: Int32) -> Bool {
        let ts = GameInstance.GetTimeSystem(GetGameInstance());
        if !IsDefined(ts) {
            return false;
        }
        ts.SetGameTimeByHMS(hours, minutes, 0);
        return true;
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

    public func StopAICommand(puppet: ref<ScriptedPuppet>, command: ref<AICommand>) {
        let component = puppet.GetAIControllerComponent();
        if (EnumInt(component.GetCommandState(command)) != EnumInt(AICommandState.Success)) {
            component.CancelCommand(command);
        }
    }
}

@addMethod(GameInstance)
public static native func GetNetworkGameSystem() -> ref<NetworkGameSystem>

import Cyberverse.Network.Managers.*
//import Codeware.*

@addField(DebugDataDef)
public let AutoContinueUsed: Bool;

// ─────────────────────────────────────────────────────────────────────────────────────────────
// TESSERA — LE SAUT AUTOMATIQUE DU MENU DEVIENT CONDITIONNEL (2026-08-09)
//
// Ce hook chargeait la dernière sauvegarde DÈS l'apparition du menu principal. C'était juste quand
// « entrer sur le serveur » voulait dire « charger la save au plus vite ». Ça ne l'est plus : entrer
// sur le serveur veut désormais dire **choisir un personnage**, et ce choix se fait dans le lobby,
// qui vit sur ce menu-là. Sans cette condition, le menu est traversé en une fraction de seconde et
// le lobby n'a pas le temps d'exister — c'est exactement ce qui a été observé le 2026-08-09.
//
// Le drapeau vit sur le blackboard `DebugData`, comme `AutoContinueUsed` juste au-dessus : c'est le
// seul magasin global atteignable depuis un hook statique ET depuis l'écran qui le lèvera.
//
// ⚠️ Il n'est JAMAIS levé automatiquement. Le chargement de la partie devient l'action du bouton
// CONNEXION du lobby, et rien d'autre. Un drapeau levé « au cas où » redonnerait le comportement
// d'avant, en pire : intermittent.
@addField(DebugDataDef)
public let TesseraLobbyValide: Bool;

@wrapMethod(SingleplayerMenuGameController)
protected cb func OnSavesForLoadReady(saves: array<String>) -> Bool {
	let res = wrappedMethod(saves);

    let handler = this.GetSystemRequestsHandler();
    if (!handler.IsPreGame()) {
        return res; // PreGame == MainMenu
    }

    // TODO: In case the connection is too slow, we miss this. What to do then? Should the DLL somehow check the saves and then LoadLastCheckpoint?
    //  technically, it could still hook this method. Or put hooking aside, we could call into it and even pass the SystemRequestsHandler.
    // Tant que le lobby n'a pas rendu son verdict, on RESTE sur le menu. Ce n'est pas un retard :
    // c'est l'écran de choix de personnage, et il est maintenant obligatoire.
    // `--tessera-dev` court-circuite l'attente : on charge tout de suite, comme avant le lobby.
    // Le drapeau de ligne de commande l'emporte sur le verdict du lobby, jamais l'inverse — un mode
    // de développement doit être une DÉROGATION explicite, pas un état qu'on pourrait atteindre
    // sans l'avoir demandé.
    let reseau = GameInstance.GetNetworkGameSystem();
    let modeDev: Bool = IsDefined(reseau) && reseau.Tessera_ModeDeveloppement();
    if !modeDev && !GetAllBlackboardDefs().DebugData.TesseraLobbyValide {
        return res;
    }
	if this.m_savesCount > 0 && !GetAllBlackboardDefs().DebugData.AutoContinueUsed {
        let networkSystem = GameInstance.GetNetworkGameSystem();
        if (networkSystem.FullyConnected) {
		    handler.LoadLastCheckpoint(false);
        } else {
            networkSystem.EnqueueLoadLastCheckpoint(handler);
        }
	}
}

@wrapMethod(SingleplayerMenuGameController)
protected cb func OnUninitialize() -> Bool {
	wrappedMethod();
	GetAllBlackboardDefs().DebugData.AutoContinueUsed = true;
}

// ⛔ « Server-Browser » RETIRE (2026-08-21). Il ouvrait le navigateur de serveurs du multijoueur
// abandonne de CDPR — sans objet pour Tessera, ou la connexion se fait par le LAUNCHER avant meme
// que le jeu demarre. Le hook reste en place, vide de son ajout : c'est lui qui sert desormais a
// composer le menu Tessera (voir `TesseraSondeLobbyNatif.reds`).
@wrapMethod(SingleplayerMenuGameController)
private func PopulateMenuItemList() -> Void {
    wrappedMethod();
}

@wrapMethod(PlayerPuppet)
protected cb func OnAction(action: ListenerAction, consumer: ListenerActionConsumer) -> Bool {
    wrappedMethod(action, consumer);
    let name = ListenerAction.GetName(action);
    let type = ListenerAction.GetType(action);
    let value = ListenerAction.GetValue(action);

    GameInstance.GetNetworkGameSystem().playerActionTracker.RecordPlayerAction(name, type, value);

    // if (StrCmp(NameToString(name), "Jump") == 0 && StrCmp(EnumValueToString("gameinputActionType", Cast(EnumInt(type))), "BUTTON_RELEASED") == 0) {
    //     let npcSpec = new DynamicEntitySpec();
    //     //npcSpec.recordID = t"Character.spr_animals_bouncer1_ranged1_omaha_mb";
    //     npcSpec.recordID = t"Character.Panam";
    //     npcSpec.appearanceName = n"random";

    //     // base\characters\entities\main_npc\panam.ent
    //     //npcSpec.recordID = t"Vehicle.v_sport2_quadra_type66";
    //     //npcSpec.appearanceName = n"quadra_type66__basic_bulleat";
    //     npcSpec.appearanceName = n"random";
    //     npcSpec.position = this.GetWorldPosition();//(4.0, -45.0);
    //     npcSpec.orientation = this.GetWorldOrientation();//(-40.0);
    //     npcSpec.persistState = false;
    //     npcSpec.persistSpawn = false;
    //     npcSpec.tags = [n"MyMod"];

    //     GameInstance.GetDynamicEntitySystem().CreateEntity(npcSpec);
    // }
}

@wrapMethod(PlayerPuppet)
protected cb func OnMountingEvent(evt: ref<MountingEvent>) -> Bool {
    let result = wrappedMethod(evt);
    GameInstance.GetNetworkGameSystem().playerActionTracker.OnMounting(evt);
    return result;
}

@wrapMethod(PlayerPuppet)
protected cb func OnUnmountingEvent(evt: ref<UnmountingEvent>) -> Bool {
    let result = wrappedMethod(evt);
    GameInstance.GetNetworkGameSystem().playerActionTracker.OnUnmounting(evt);
    return result;
}

// replaces the OnWeapoNEquipEvent as we need the counterpart for unequiping anyway?
// protected cb func OnWeaponEquipEvent(evt: ref<WeaponEquipEvent>) -> Bool {
@wrapMethod(PlayerPuppet)
public final func OnItemEquipped(slot: TweakDBID, item: ItemID) -> Void {
    let isWeapon = RPGManager.IsItemWeapon(item);
    GameInstance.GetNetworkGameSystem().playerActionTracker.OnItemEquipped(slot, item, isWeapon);
}

@wrapMethod(PlayerPuppet)
public final func OnItemUnequipped(slot: TweakDBID, item: ItemID) -> Void {
    let isWeapon = RPGManager.IsItemWeapon(item);
    GameInstance.GetNetworkGameSystem().playerActionTracker.OnItemUnequipped(slot, item, isWeapon);
}

@wrapMethod(BaseProjectile)
protected cb func OnShoot(eventData: ref<gameprojectileShootEvent>) -> Bool {
    wrappedMethod(eventData);
    FTLog("Shoot");
    GameInstance.GetNetworkGameSystem().playerActionTracker.OnShoot(eventData);
}

@wrapMethod(BaseProjectile)
protected cb func OnShootTarget(eventData: ref<gameprojectileShootTargetEvent>) -> Bool {
    wrappedMethod(eventData);
    FTLog("Shoot Target");
}

// @wrapMethod(GameObject)
// protected cb func OnHit(evt: ref<gameHitEvent>) -> Bool {
//     wrappedMethod(evt);
//     FTLog("OnHit");
//     GameInstance.GetNetworkGameSystem().playerActionTracker.OnHit(this, evt);
// }

// @wrapMethod(JumpEvents)
// protected cb func OnEnter(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {}

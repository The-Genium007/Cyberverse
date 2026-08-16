#include "PlayerActionTracker.h"
#include "Main.h"
#include <RED4ext/RED4ext.hpp>

#include "NetworkGameSystem.h"
#include "RED4ext/Scripting/Natives/Generated/game/MountEventData.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/damage/AttackData.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/mounting/MountingRequest.hpp"
#include "Utils.h"

#include <serverbound/WorldPacketsServerBound.h>

void PlayerActionTracker::RecordPlayerAction(RED4ext::CName actionName, RED4ext::gameinputActionType actionType,
                                             float value)
{
    const auto name = std::string(actionName.ToString());
    // SDK->logger->InfoF(PLUGIN, "Player Action %s (%d) -> %f", name.c_str(), actionType, value);

    // We ignore movement inputs like Forward, MoveX, MoveY for now, because we don't care about animations but just
    // want the most recent position And for that, those inputs are very unreliable due to physics/collision etc anyway.
    if (actionType == RED4ext::game::input::ActionType::RELATIVE_CHANGE)
    {
        // think about mouse movements, at least mouse_x is important as yaw.
        // alternatively CameraMouseX.
    }
    else if (name == "Jump" && actionType == RED4ext::game::input::ActionType::BUTTON_RELEASED)
    {
        const auto player = Cyberverse::Utils::GetPlayer();
        const auto [X, Y, Z, W] = Cyberverse::Utils::Entity_GetWorldPosition(player);

        PlayerActionTracked tracked = {};
        tracked.action = eACTION_JUMP;

        tracked.worldTransform = {};
        tracked.worldTransform.x = X;
        tracked.worldTransform.y = Y;
        tracked.worldTransform.z = Z;

        Red::GetGameSystem<NetworkGameSystem>()->EnqueueMessage(1, tracked);
    }
}

void PlayerActionTracker::OnShoot(RED4ext::Handle<RED4ext::gameprojectileShootEvent> event)
{
    PlayerShoot player_shoot = {};
    player_shoot.charge = event->params.charge;
    player_shoot.startPoint = Vector3 { event->startPoint.X, event->startPoint.Y, event->startPoint.Z };
    player_shoot.itemIdWeapon = // TODO: FILL
    Red::GetGameSystem<NetworkGameSystem>()->EnqueueMessage(0, player_shoot);
}

void PlayerActionTracker::OnHit(RED4ext::Handle<RED4ext::GameObject> gameObject,
                                RED4ext::Handle<RED4ext::gameHitEvent> event)
{
    SDK->logger->InfoF(PLUGIN, "OnHit! %d", event->attackData->attackType);
}


/// Index de siege a partir du nom de slot du jeu.
///
/// L'ordre suit `EVehicleDoor` (`vehicleComponentPS.script:2194`), que le serveur reprend tel quel
/// dans `param`. Les NOMS sont canoniques meme si la TOPOLOGIE varie d'un modele a l'autre
/// (F-VEH-012 : 2 places sur un coupe, 4 sur une berline) — c'est pour ca qu'on traduit un nom et
/// qu'on ne compte pas des sieges.
///
/// Un slot inconnu rend 0 (place du conducteur) plutot que de faire echouer le rapport : mieux
/// vaut un siege approximatif qu'un serveur qui ignore qu'un joueur est monte.
static uint32_t IndexDeSiege(RED4ext::CName slot)
{
    if (slot == RED4ext::CName("seat_front_left")) return 0;
    if (slot == RED4ext::CName("seat_front_right")) return 1;
    if (slot == RED4ext::CName("seat_back_left")) return 2;
    if (slot == RED4ext::CName("seat_back_right")) return 3;
    return 0;
}

void PlayerActionTracker::OnMounting(RED4ext::Handle<RED4ext::game::mounting::MountingEvent> event)
{
    if (event->relationship.otherMountableType != RED4ext::game::MountingObjectType::Vehicle)
    {
        // mountingSubType would tell us whether bike or car.
        return;
    }

    if (event->relationship.relationshipType != RED4ext::game::MountingRelationshipType::Parent)
    {
        // If the car mounts us, do nothing
        return;
    }

    if (event->relationship.otherObject.Expired())
    {
        SDK->logger->Warn(PLUGIN,
                          "Cannot Process PlayerActionTracker::OnMounting as the vehicle's weak ref has expried");
        return;
    }

    // Keep alive as long as the derived vehicleInstance
    const auto strongLock = event->relationship.otherObject.Lock();
    // TODO: if you figure out how to inline that, you're welcome to do so.
    const RED4ext::game::Object* ptr = strongLock;
    const auto vehicleInstance = RED4ext::Handle((RED4ext::VehicleObject*)ptr);

    // TODO: vehicles.swift, there is VehicleObject.GetRecordID(), but there is also GameObject::GetTDBID(go) which is
    // more generic and handles casting, so it works for puppets, devices _and_ vehicles. Question is if we should here
    // just implement both so we can avoid casting and misleading?

    const auto recordId = Cyberverse::Utils::VehicleObject_GetRecordID(vehicleInstance);
    const auto [X, Y, Z, W] = Cyberverse::Utils::Entity_GetWorldPosition(vehicleInstance);
    const auto orientation = Cyberverse::Utils::Entity_GetWorldOrientation(vehicleInstance);
    const auto [Roll, Pitch, Yaw] = Cyberverse::Utils::Quaternion_ToEulerAngles(orientation);

    PlayerSpawnCar spawnCar = {};
    spawnCar.recordId = recordId.value;
    spawnCar.worldTransform = Vector3{X, Y, Z};
    spawnCar.yaw = Yaw;

    const auto reseau = Red::GetGameSystem<NetworkGameSystem>();
    reseau->EnqueueMessage(0, spawnCar);

    // Rapport de MONTEE au serveur — le fil qui manquait.
    //
    // Sans lui, le serveur ne sait pas qu'un joueur est assis : pas d'occupation de siege, donc
    // aucun autre client ne peut le voir dans la voiture, ni jouer l'animation d'entree. Mesure du
    // 2026-08-14 : « le vehicule est visible des deux cotes, mais quand on rentre dedans on n'a pas
    // la mise a jour de l'etat des gens ».
    //
    // `IdReseauDe` rend 0 pour un vehicule purement LOCAL (une voiture d'appel, une epave du
    // decor) : on ne rapporte alors rien, ce qui est correct — le serveur ne connait pas cet objet
    // et ne pourrait pas le repliquer. C'est aussi ce que resout le TODO historique juste
    // au-dessous dans OnUnmounting (« le serveur ne devrait pas avoir a deviner »).
    const auto idReseau = reseau->IdReseauDe(vehicleInstance->entityID);
    if (idReseau != 0)
    {
        reseau->RapporterMontage(idReseau, IndexDeSiege(event->relationship.slotId.id), true);
    }
}

void PlayerActionTracker::OnUnmounting(RED4ext::Handle<RED4ext::game::mounting::UnmountingEvent> event)
{
    if (event->relationship.otherMountableType != RED4ext::game::MountingObjectType::Vehicle)
    {
        // mountingSubType would tell us whether bike or car.
        return;
    }

    if (event->relationship.relationshipType != RED4ext::game::MountingRelationshipType::Parent)
    {
        // If the car mounts us, do nothing
        return;
    }

    if (event->relationship.otherObject.Expired())
    {
        SDK->logger->Warn(PLUGIN, "Cannot Process PlayerActionTracker::OnUnmounting as the vehicle's weak ref "
                                  "has expried");
        return;
    }

    // Keep alive as long as the derived vehicleInstance
    const auto strongLock = event->relationship.otherObject.Lock();
    // TODO: if you figure out how to inline that, you're welcome to do so.
    const RED4ext::game::Object* ptr = strongLock;
    const auto vehicleInstance = RED4ext::Handle((RED4ext::VehicleObject*)ptr);

    // Le TODO historique — « lire le networkedEntityId depuis l'entite, pour que le serveur n'ait
    // pas a deviner » — est resolu ci-dessous par `IdReseauDe`. Le message herite reste envoye tel
    // quel pour ne rien casser de l'existant.
    const auto reseau = Red::GetGameSystem<NetworkGameSystem>();
    const PlayerUnmountCar unmount_car = {};
    reseau->EnqueueMessage(0, unmount_car);

    // Rapport de DESCENTE. Sans lui, le siege reste occupe cote serveur pour toujours : plus
    // personne ne peut s'y asseoir, et l'invariant convoi continue de poser un joueur qui n'est
    // plus dans la voiture. Le siege est rapporte pour symetrie, mais le serveur n'en a pas besoin
    // pour demonter — `unmount` retire l'occupant de TOUT siege qu'il occupait.
    const auto idReseau = reseau->IdReseauDe(vehicleInstance->entityID);
    if (idReseau != 0)
    {
        reseau->RapporterMontage(idReseau, IndexDeSiege(event->relationship.slotId.id), false);
    }
}

void PlayerActionTracker::OnItemEquipped(const RED4ext::TweakDBID slot, const RED4ext::ItemID item, const bool isWeapon)
{
    SDK->logger->InfoF(PLUGIN, "Item Equipped at slot %llu with item %llu", slot.value, item.tdbid.value);
    PlayerEquipItem player_equip = {};
    player_equip.slot = slot.value;
    player_equip.itemId = item.tdbid.value;
    player_equip.isWeapon = isWeapon;
    player_equip.isUnequipping = false;
    Red::GetGameSystem<NetworkGameSystem>()->EnqueueMessage(0, player_equip);
}

void PlayerActionTracker::OnItemUnequipped(const RED4ext::TweakDBID slot, const RED4ext::ItemID item, const bool isWeapon)
{
    SDK->logger->InfoF(PLUGIN, "Item Unequipped at slot %llu with item %llu", slot.value, item.tdbid.value);
    PlayerEquipItem player_equip = {};
    player_equip.slot = slot.value;
    player_equip.itemId = item.tdbid.value;
    player_equip.isWeapon = isWeapon;
    player_equip.isUnequipping = true;
    Red::GetGameSystem<NetworkGameSystem>()->EnqueueMessage(0, player_equip);
}

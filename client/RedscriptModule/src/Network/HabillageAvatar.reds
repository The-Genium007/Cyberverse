module Cyberverse.Network.Managers

// ⛔ CE FICHIER NE FAIT PLUS RIEN — et il reste, avec ce qu'il a appris.
//
// L'habillage de l'avatar distant a fini par MARCHER (F-PLY-286, 2026-08-24), mais pas ici : les
// vêtements sont des **composants de l'entité** `avatar_distant_ma.ent`, posés à la fabrication de
// l'asset par `tools/re-probe/entites/habiller-avatar.py`. Rien ne se joue plus à l'exécution.
//
// Ce qui reste ici est donc une **fonction vide** et la carte des impasses. Le fichier subsiste
// parce que `NetworkGameSystem.reds` déclare toujours `TesseraHabillerAvatar` et que le C++
// l'appelle par créneaux : retirer les trois d'un coup est un chantier à part, à faire quand le
// serveur pilotera vraiment la tenue.
//
// ── LES TROIS VOIES ESSAYÉES ICI, ET POURQUOI AUCUNE NE TIENT ────────────────────────────────
//
//   1. **L'ÉQUIPEMENT** (`GiveItem` + `AddItemToSlot`) — 🔴 impasse F-PLY-275. L'appel met en file
//      une naissance d'entité d'item qui n'aboutit **jamais** : sur un pantin de photomode enrichi
//      comme sur un pantin de passant, avec ou sans `EquipmentSystemPlayerData`. Les deux recettes
//      du jeu ont été essayées, dont celle de l'aperçu d'inventaire qui, elle, fonctionne sur un
//      `gamePuppet` dans son propre contexte.
//
//   2. **DEMANDER UNE APPARENCE APRÈS LA NAISSANCE** (`ScheduleAppearanceChange`) — l'ordre PREND
//      (`GetCurrentAppearanceName` le confirme à la passe suivante) et **le corps disparaît**
//      (F-PLY-283). Cause : l'apparence demandée n'était déclarée nulle part dans l'entité livrée.
//      Même corrigée, cette voie reste sans objet — voir ci-dessous.
//
//   3. **RIEN NE SE DÉCIDE APRÈS LA CONSTRUCTION.** C'est la loi générale (F-PLY-191) : l'état
//      visuel d'une entité est figé à sa naissance ; ce qui arrive après ne peut que défaire. La
//      charge `TSV1` marche parce qu'elle est appliquée À la construction. C'est pour ça que
//      l'habillage a fini par se faire dans l'ASSET et non dans le script.
//
// ⚠️ Ne pas rouvrir ces voies sans une mesure neuve : elles ont coûté sept lancements à deux
// instances, et chacune est consignée avec ce qui la rouvrirait.
//
// ── CE QUI RESTE À FAIRE, ET QUI NE PASSERA PAS PAR ICI ─────────────────────────────────────
//
// Les vêtements sont aujourd'hui **en dur** dans l'entité — les mêmes pour tout le monde, et
// masculins. Les faire dépendre de ce que le serveur déclare porté (`contenus.porte`, qui arrive
// déjà jusqu'au client par `Tessera_NombreDeVetements` / `Tessera_VetementDeLEntite`) demandera
// plusieurs entités ou plusieurs records, choisis à la NAISSANCE : c'est un travail d'asset et de
// spawn, pas de script d'exécution.

// Appelée par `PiloterAvatar` (C++) par créneaux bornés. Rend `true` immédiatement — « il n'y a
// rien à faire » — et l'appelant cesse de repasser.
//
// ⚠️ Elle n'est pas supprimée parce que son appelant C++ existe toujours. Une fonction absente
// ferait échouer `Red::CallVirtual` en silence, ce qui est pire qu'une fonction vide et honnête.
func TesseraHabillerLeCorps(cible: EntityID, passe: Uint32) -> Bool {
    return true;
}

module Cyberverse.Network.Managers

// L'AVATAR D'UN JOUEUR PORTE CE QUE LE SERVEUR DIT QU'IL PORTE.
//
// ── ⛔ CE QUE CE FICHIER NE FAIT PLUS, ET POURQUOI ────────────────────────────────────────────
//
// Il a porté, du 2026-08-23 au 24, une tentative d'habillage par le **système d'équipement** :
// donner l'item au pantin puis l'attacher à son slot, selon les deux recettes que le jeu emploie
// lui-même. Cette voie est une **impasse mesurée** (F-PLY-275) : `AddItemToSlot` met en file une
// naissance d'entité d'item qui n'aboutit **jamais**, sur un pantin de photomode enrichi comme sur
// un pantin de passant, avec ou sans `EquipmentSystemPlayerData`. Le code est retiré plutôt que
// commenté — un chemin mort qu'on laisse en place se refait essayer.
//
// ── CE QU'IL FAIT MAINTENANT : DEMANDER NOTRE APPARENCE ──────────────────────────────────────
//
// F-PLY-281 a montré que notre apparence `tessera_v_ma` — celle que **nous** livrons, et qui
// contient déjà des pièces de vêtement — n'est **jamais demandée** : le serveur envoie un preset
// PNJ, et la voie de spawn enrichie ne transporte aucun nom d'apparence. Le fichier existe, il est
// correct, il est livré, et personne ne le lit.
//
// Ce fichier pose donc la question qui n'avait jamais été posée en conditions réelles :
// **que se passe-t-il si on la demande ?**
//
// ⚠️ ET LA RÉPONSE PEUT ÊTRE MAUVAISE, C'EST PRÉVU. F-PLY-282 énonce la fourche : la charge `TSV1`
// donne le V individuel du joueur sans vêtements ; notre apparence donne des vêtements mais les
// variantes du V par défaut de CDPR. Demander l'apparence peut donc **effacer le visage du joueur**.
// C'est une information, pas un accident — et l'ADR 0040 rend une reconstruction acceptable.
//
// Les trois issues sont utiles, et le journal les distingue :
//   · l'avatar s'habille et garde son visage  → les deux canaux se cumulent, on a gagné ;
//   · l'avatar s'habille et perd son visage   → la fourche est confirmée, et la suite est
//                                                ArchiveXL (une apparence, des variantes) ;
//   · rien ne change                          → l'apparence n'est pas appliquée sur ce corps, et
//                                                c'est `GetCurrentAppearanceName` qui le dira.
//
//   grep "\[Habillage\]" <jeu>/red4ext/logs/cyberverse.red4ext-*.log

// Le nom de l'apparence que NOUS livrons, déclarée dans `avatar_distant_ma.ent` et définie dans
// `avatar_distant_ma.app` — les deux dans `tessera-avatar-marche.archive`.
//
// ⚠️ Le `.app` MANQUAIT de l'archive livrée jusqu'au 2026-08-24 : l'entité le référençait, et il
// n'était présent que sur la machine de Lucas. Demander cette apparence aurait donc marché chez lui
// et chez personne d'autre — le défaut exact que l'ADR 0038 existe pour empêcher, sur ce paquet-là.
// ⚠️ Une FONCTION, pas une constante globale : redscript ne supporte pas les liaisons `let`
// globales (« global let binding is not supported »). Le compile-check l'attrape en quinze
// secondes ; le jeu, lui, aurait refusé de démarrer avec TOUT `r6/scripts` par terre.
func TesseraApparenceTessera() -> CName {
    return n"tessera_v_ma";
}

func TesseraJournalHabillage(texte: String) -> Void {
    let reseau = GameInstance.GetNetworkGameSystem();
    if IsDefined(reseau) {
        reseau.Tessera_Journal("[Habillage] " + texte);
    }
}

// Le corps derrière un `EntityID`, quelle que soit la voie qui l'a fait naître.
//
// ⚠️ `DynamicEntitySystem.GetEntity` ne connaît pas les corps de la voie enrichie : ils ne sont pas
// nés par lui. `GameInstance.FindEntityByID`, lui, interroge le monde. Un corps non résolu n'est pas
// une erreur, c'est « pas encore né » — l'appelant repassera.
func TesseraCorpsDeLEntite(cible: EntityID) -> ref<Entity> {
    let entite = GameInstance.GetDynamicEntitySystem().GetEntity(cible);
    if !IsDefined(entite) {
        entite = GameInstance.FindEntityByID(GetGameInstance(), cible);
    }
    return entite;
}

// Demande notre apparence sur le corps, et rend `true` quand elle est en place.
//
// Appelé par `PiloterAvatar` (C++) par créneaux bornés — le seul chemin dont on sait qu'il atteint
// les corps de la voie enrichie (F-PLY-278 : le hook `OnGameAttached` ne s'y déclenche jamais).
func TesseraHabillerLeCorps(cible: EntityID, passe: Uint32) -> Bool {
    let corps = TesseraCorpsDeLEntite(cible);
    if !IsDefined(corps) {
        return false; // pas encore né — l'appelant repassera
    }
    // ⛔⛔ VOIE DESACTIVEE — MESUREE LE 2026-08-24, ET ELLE FAIT DISPARAITRE LE CORPS.
    //
    // `ScheduleAppearanceChange(tessera_v_ma)` PREND : le journal a rendu « apparence tessera_v_ma
    // EN PLACE (passe 1) » sur les deux instances, l'apparence precedente etant `None`. Et le
    // verdict visuel de Lucas est sans appel : **le personnage devient INVISIBLE**.
    //
    // Ce n'est pas « les vetements ne s'affichent pas » : c'est le corps entier qui s'eteint.
    // Appliquer une apparence sur ce pantin REMPLACE donc ce que la charge `TSV1` avait construit,
    // et notre `.app` ne rend rien a la place — exactement le comportement decrit par F-PLY-083
    // (le corps n'est pas absent, il est ETEINT) et par F-PLY-191 (l'etat visuel est fige a la
    // construction : ce qui arrive apres ne peut que defaire).
    //
    // ⚠️ ON REND `true` : la boucle s'arrete et ne redemande plus rien. Le corps reste NU mais
    // VISIBLE, ce qui est l'etat d'avant — un avatar nu vaut mieux qu'un avatar absent.
    //
    // La suite ne passe donc pas par une apparence appliquee APRES la naissance. Elle passe par
    // l'apparence avec laquelle l'avatar NAIT — voir F-PLY-283 et l'ADR 0040.
    return true;

    let actuelle = corps.GetCurrentAppearanceName();
    if Equals(actuelle, TesseraApparenceTessera()) {
        // ⚠️ LA RÉUSSITE SE JOURNALISE, et c'est la ligne qui distingue « l'apparence a PRIS » de
        // « l'ordre est parti ». `ScheduleAppearanceChange` a un effet DIFFÉRÉ : relire juste après
        // rend encore l'ancienne (F-PNJ-050), donc le verdict ne peut venir que d'une passe
        // ultérieure — jamais de la même.
        TesseraJournalHabillage(s"apparence \(NameToString(actuelle)) EN PLACE (passe \(passe))");
        return true;
    }

    // ⚠️ ON N'ORDONNE QU'UNE FOIS PAR CRÉNEAU, jamais en rafale : une écriture d'apparence par frame
    // et par avatar est le régime qui a fait tomber le jeu deux fois le 2026-08-06. La cadence est
    // tenue par l'appelant (une passe par seconde, dix au plus).
    corps.ScheduleAppearanceChange(TesseraApparenceTessera());
    if passe == 0u || passe == 9u {
        TesseraJournalHabillage(
            s"apparence demandee : \(NameToString(TesseraApparenceTessera()))"
            + s" · actuelle : \(NameToString(actuelle))"
            + s" · passe \(passe)");
    }
    return false;
}

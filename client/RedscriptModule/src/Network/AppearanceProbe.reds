module Cyberverse.Network.Managers

// SONDE — `ScheduleAppearanceChange` a-t-il un EFFET sur un PNJ de communauté ?
//
// Toute la voie A du chantier statiques y suspend, et elle n'a jamais été mesurée. On sait
// seulement que **l'appel réussit** (F-PNJ-130), ce qui ne prouve rien : c'est le « succès
// trompeur » que la doctrine interdit de compter, et F-PNJ-132 a montré que la sonde précédente ne
// pouvait pas voir l'effet — elle journalisait l'apparence AVANT l'ordre.
//
// ── Protocole ─────────────────────────────────────────────────────────────────────────────────
//
// On impose à un statique l'apparence d'un AUTRE statique du même record, puis on RELIT 3 s plus
// tard.
//
// ⚠️ Une apparence prise sur le même record est forcément valide : le moteur rejette en silence une
// apparence étrangère au jeu d'apparences de l'entité (F-PNJ-051). Inventer un nom aurait donné un
// échec ininterprétable — refus du moteur, ou ordre sans effet ?
//
// ⚠️ La relecture est DIFFÉRÉE de 3 s : `ScheduleAppearanceChange` l'est aussi, et relire
// immédiatement rend l'ancienne valeur (F-PNJ-050) — on aurait conclu à tort à un échec.
//
// ⚠️ L'état (première apparence par record, garde one-shot) vit côté C++ : une classe
// `ScriptableSystem` maison n'a PAS été résolue par le conteneur de systèmes — 171 pantins classés,
// zéro appel, en silence.
//
// Trois verdicts, tous instructifs :
//   · PREND       → la voie A est viable ;
//   · SANS-EFFET  → l'ordre est accepté sans rien faire, il faudra promouvoir ;
//   · entité absente → destreamée avant relecture, mesure à refaire.

public func TesseraSonderApparence(reseau: ref<NetworkGameSystem>, pantin: ref<ScriptedPuppet>) -> Void {
    let origine = pantin.GetCurrentAppearanceName();
    let cobaye = reseau.Tessera_CobayeApparence(TDBID.ToNumber(pantin.GetRecordID()), origine);
    if !IsNameValid(cobaye) {
        return;
    }
    reseau.Tessera_Journal(s"[SondeApp] cobaye origine=\(origine) imposee=\(cobaye)");
    reseau.AppliquerApparenceStatique(pantin.GetEntityID(), cobaye);

    let relecture = new TesseraRelectureApparence();
    relecture.pantin = pantin;
    relecture.imposee = cobaye;
    relecture.origine = origine;
    GameInstance.GetDelaySystem(GetGameInstance()).DelayCallback(relecture, 3.0, false);
}

public class TesseraRelectureApparence extends DelayCallback {
    let pantin: wref<ScriptedPuppet>;
    let imposee: CName;
    let origine: CName;

    public func Call() -> Void {
        let reseau = GameInstance.GetNetworkGameSystem();
        if !IsDefined(reseau) {
            return;
        }
        if !IsDefined(this.pantin) {
            reseau.Tessera_Journal(s"[SondeApp] VERDICT=entite-absente (destreamee avant relecture)");
            return;
        }
        let relue = this.pantin.GetCurrentAppearanceName();
        let verdict = Equals(relue, this.imposee) ? "PREND" : (Equals(relue, this.origine) ? "SANS-EFFET" : "AUTRE");
        reseau.Tessera_Journal(s"[SondeApp] VERDICT=\(verdict) relue=\(relue) imposee=\(this.imposee) origine=\(this.origine)");
    }
}

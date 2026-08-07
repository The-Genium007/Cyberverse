module Cyberverse.Network.Managers

// Coupe l'ACCUMULATION de crime native, sans démonter le système de police.
//
// ── Pourquoi ce levier, et pas les deux autres ────────────────────────────────────────────────
//
// Trois voies existaient. Celle-ci a été retenue après les avoir toutes regardées :
//
//  1. `PreventionSystem.OnAttach` avec retour anticipé (écrit dans `tessera-desossage`, corroboré
//     par des mods publiés). Efficace mais BRUTAL : le système n'existe plus, donc radio police,
//     barre de recherche, télémétrie et sirènes cassent en silence.
//  2. `ChangeHeatStage`, le vrai entonnoir — seul site d'affectation de `m_heatStage`. Une sonde
//     est déjà écrite (`Tessera/reprobe/PoliceFunnelProbe.reds`) et elle DÉCONSEILLE explicitement
//     d'en faire un coupe-circuit avant d'avoir mesuré les raisons et le débit. On respecte.
//  3. Celle-ci : `SetHeatCounterMultiplier`, une `ScriptableSystemRequest` que **les quêtes du jeu
//     utilisent elles-mêmes** (`preventionSystem.script:4034`). Elle pose
//     `m_crimeScoreMultiplierByQuest`, qui multiplie CHAQUE addition de score de crime
//     (lignes 945, 991, 1037, 1051). À zéro, plus rien ne s'accumule.
//
// Le système continue donc de vivre et de fonctionner — il ne reçoit simplement jamais de raison
// de monter. C'est le point de DÉCISION (l'accumulation), pas l'effet visible (le spawn des
// voitures) : couper le spawn aurait laissé le heat monter, les étoiles s'allumer et les PNJ
// hostiles réagir. Symptôme masqué, cause intacte.
//
// ── Ce qui a été essayé AVANT, et qui n'a pas marché ─────────────────────────────────────────
//
// Six flats TweakDB `PreventionSystem.*.crimeScoreMultiplier` poussés par ConfigSync. Le client a
// répondu « 22 appliquées, 0 refusée » — et les étoiles sont montées quand même. `SetFlat` accepte
// des identifiants qui ne commandent rien : l'acceptation ne prouve ni l'existence ni l'effet.
//
// ⚠️ PROVISOIRE, sur deux points. (1) Le réglage est appliqué INCONDITIONNELLEMENT : à terme il
// doit venir du serveur, comme le reste — un opérateur pourrait vouloir sa police native. (2) Ce
// fichier vit dans le module réseau par commodité de déploiement ; sa vraie place est le levier
// `police` de `tessera-desossage`, qui porte déjà une configuration pour ça.
//
// ✅ MESURÉ EN JEU le 2026-08-07 (F-PNJ-114) : agression d'un PNJ, aucune étoile. Jugement de
// Lucas : « c'est parfait, on garde ce comportement ». Ne pas rouvrir sans une raison de jeu.

@wrapMethod(PlayerPuppet)
protected cb func OnGameAttached() -> Bool {
    let resultat = wrappedMethod();
    TesseraCouperAccumulationDeCrime();
    return resultat;
}

public func TesseraCouperAccumulationDeCrime() -> Void {
    let systemes = GameInstance.GetScriptableSystemsContainer(GetGameInstance());
    let prevention = systemes.Get(n"PreventionSystem") as PreventionSystem;
    if !IsDefined(prevention) {
        return;
    }
    let requete = new SetHeatCounterMultiplier();
    // m_reset = true remettrait le multiplicateur à 1.0 — c'est le chemin INVERSE, à ne pas
    // confondre : le handler teste `m_reset` AVANT de lire `m_heatMultiplier`.
    requete.m_reset = false;
    requete.m_heatMultiplier = 0.0;
    requete.source = n"Tessera";
    prevention.QueueRequest(requete);
}

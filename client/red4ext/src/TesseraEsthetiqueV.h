#pragma once

// ── LIRE L'ESTHETIQUE DU V LOCAL, DANS LE CLIENT LIVRE ───────────────────────────────────────────
//
// POURQUOI CE FICHIER EXISTE. Toute la chaine d'apparence a ete etablie dans la SONDE
// (`tools/re-probe`), qui n'est pas distribuee aux joueurs. Le createur de personnage, lui, tourne
// dans le client livre : il lui faut la lecture ici.
//
// Ce que ce module fait, et rien d'autre : rendre l'esthetique du personnage local sous forme d'un
// blob `TSV1` en hexadecimal, pret a etre passe a `Tessera_CreerPersonnage`. Il ne l'APPLIQUE pas,
// il ne SPAWNE pas, il ne decide pas — le serveur est autorite sur l'esthetique (ADR 0036), le
// client capture et PROPOSE.
//
// Le format `TSV1` est partage a l'octet pres avec le serveur (`tessera-core/server/src/esthetique_v.rs`) :
//   magic "TSV1" (4 o) · nHead · nBody · nArms (1 o chacun) · 1 o reserve · puis N paires de 16 o.
// Un V complet fait 27 paires, soit 440 octets.
//
// ⚠️ CE CODE EST EPINGLE A LA VERSION 2.31 DU JEU. Il appelle des fonctions du binaire par RVA et
// lit un drapeau a un offset fixe. Toute montee de version du jeu doit le REVERIFIER — pas le
// reporter tel quel. Les RVA et l'offset viennent de mesures datees, citees sur place.

#include <RED4ext/RED4ext.hpp>

#include <string>

namespace Tessera::EsthetiqueV
{
/// Rend l'esthetique du personnage local en hexadecimal (blob `TSV1`).
///
/// `aEtat` doit etre un `gameuiCharacterCustomizationState` — celui que redscript obtient par
/// `GameInstance.GetCharacterCustomizationSystem().GetState()`. Le type est VERIFIE ici : ce module
/// calcule des adresses a offset fixe et appelle des fonctions natives, donc un objet inattendu
/// serait un crash, pas une erreur.
///
/// Rend `true` et remplit `aHexOut` en cas de succes. Rend `false` et remplit `aErreurOut` d'une
/// raison lisible sinon — et il y a plusieurs raisons legitimes de refuser, notamment un etat NON
/// FINALISE, qui n'est pas une panne mais un moment.
bool Lire(RED4ext::IScriptable* aEtat, std::string& aHexOut, std::string& aErreurOut);
}  // namespace Tessera::EsthetiqueV

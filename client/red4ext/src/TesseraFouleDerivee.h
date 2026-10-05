// Foule derivee — les hooks A' a H du monde partage (Tessera). Interface MINIMALE, sans
// dependance a NetworkGameSystem.h : ce fichier, TesseraFouleDerivee.cpp, FouleDerivee.hpp,
// PopulationDerivee.hpp et tests/verif_foule_derivee.cpp demenagent ensemble.
//
// ⚠️ EFFET EN JEU NON MESURE (2026-10-05) : compile et teste hors jeu seulement.
#pragma once

#include <RED4ext/RED4ext.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace Tessera::Foule
{
/// Lit les interrupteurs (une fois) et attache les detours. A appeler au chargement du plugin.
void Attacher(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle);

/// Fil du jeu, a chaque snapshot : l'heure serveur estimee (0 = pas encore d'horloge).
void PoserHeureServeur(std::uint64_t aServeurMs);

/// Shard -> client `MondePartage` : graine, verdict, cles suspendues (promues).
void RecevoirMondePartage(std::uint64_t aGraine, std::uint8_t aMode, std::uint64_t aEmpreinteServeur,
                          const std::uint64_t* aSuspendus, std::size_t aNombre);
/// Shard -> clients `Rendu` : ces cles reviennent dans la derivation.
void RecevoirRendu(const std::uint64_t* aCles, std::size_t aNombre);

/// Sonde : `mode= graine= actif= controle= hooks= horsCle= decisions=…`.
std::string Etat();
/// Sonde S-L4h / S-L6b : le dernier prolongement de trajet vu par le hook E.
std::string Scores();
/// Sonde T14 : UN appel de la porte de teleportation du moteur (`1403beea0`) sur le stub de
/// l'entite `aEntite`, deplace de (dx, dy) metres. Pietons rendus seulement (F-PNJ-226).
std::string StubTeleportSonde(std::uint64_t aEntite, float aDx, float aDy);
}  // namespace Tessera::Foule

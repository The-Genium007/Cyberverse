#pragma once

// ── FABRIQUER UN CORPS QUI PORTE LE V D'UN AUTRE JOUEUR ──────────────────────────────────────────
//
// POURQUOI CE FICHIER EXISTE. Jusqu'ici, l'avatar d'un joueur distant etait fabrique par
// `DynamicEntitySpec` (Codeware) avec un record pris dans le CATALOGUE DES PASSANTS
// (`Character.CitizenRichFemale`...). Tout le monde ressemblait donc a un pieton de Night City.
//
// Et la fiche d'apparence de chacun ARRIVAIT DEJA : elle traverse la base, le Gateway, le Shard, le
// fil, et se range dans `m_appearances[...].esthetique` — ou personne ne la lisait. Le chainon
// manquant tenait en un appel.
//
// Codeware ne peut pas le poser : `DynamicEntitySpec` n'a AUCUN champ de customisation. Le seul
// vecteur mesure est la voie enrichie du spawner du MODE PHOTO, atteinte en natif (F-PLY-172 :
// trois charges, trois rendus visiblement distincts ; F-PLY-170 : 200 corps a 25,9 fps, zero refus).
//
// ⚠️⚠️ CE MODULE EST DE COUCHE 3 (ADR 0015) : SON MODE D'ECHEC EST LE CRASH DU PROCESSUS.
//
// Il appelle une fonction du binaire par RVA, avec une requete assemblee a la main. Quatre
// garde-fous, decides avec Lucas le 2026-08-23, et aucun n'est decoratif :
//
//   1. le drapeau `g_actif` est **ETEINT par defaut** — rien ne s'execute tant qu'on ne l'allume
//      pas, exactement comme `g_pilotageParEntrees` avant sa mesure ;
//   2. la charge est **validee** avant tout appel (magic, compteurs, taille coherente) ;
//   3. les gardes du spawner sont **relues a l'instant**, jamais supposees ;
//   4. tout echec **rend la main** a la voie sure (`SpawnNetworkAvatar`), il n'y a pas de chemin
//      ou l'on reste sans corps.
//
// ⚠️ EPINGLE A CYBERPUNK 2077 **2.31**, comme `TesseraEsthetiqueV`. Toute montee de version doit
// REVERIFIER les RVA et les offsets — jamais les reporter tels quels.

#include <RED4ext/RED4ext.hpp>
// ⚠️ `RED4ext.hpp` ne tire PAS `ent::EntityID` — il faut son en-tete propre. Sans lui, le message du
// compilateur (« 'ent' : le symbole a gauche de '::' doit etre un type ») ressemble a une faute de
// frappe alors que c'est un include manquant. `NetworkGameSystem.h` le declare de la meme facon.
#include <RED4ext/Scripting/Natives/entEntityID.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Tessera::SpawnEnrichi
{
/// L'interrupteur. ⚠️ **ETEINT par defaut, et c'est le premier garde-fou.**
///
/// La lecon vient du pilotage par entrees : le correctif de glissement etait ECRIT et JUSTE, et il
/// est reste inerte des semaines parce que personne n'avait decide quand l'allumer (F-PLY-223). On
/// livre donc l'interrupteur AVEC le code, et l'A/B en jeu decide — un seul parametre change entre
/// les deux moities de la mesure.
extern bool g_actif;

/// Ce qu'une tentative rend. Volontairement bavard : cette voie se mesure en UNE session, et un
/// resultat qu'on ne sait pas interpreter coute la session entiere.
struct Resultat
{
    /// A-t-on seulement essaye ? `false` = drapeau eteint, ou pas de fiche d'apparence.
    bool tente = false;
    /// L'appel natif est-il parti ? Distinct de `tente` : tout ce qui precede peut refuser.
    bool appelFait = false;
    /// ⭐ **L'appel est parti, le corps n'est pas encore la : l'appelant doit PATIENTER**, surtout
    /// pas retomber sur la voie sure (il fabriquerait le doublon qu'on repare).
    ///
    /// ⚠️ CE CHAMP EXISTE PARCE QUE J'AI DEDUIT CET ETAT D'AUTRE CHOSE, ET QUE C'ETAIT FAUX. La
    /// premiere version disait « patiente » quand `diag` etait vide — or le tout premier appel
    /// ECRIT un diagnostic (« appel enrichi PARTI »). La condition ne se declenchait donc jamais,
    /// et le doublon revenait. Mesure en jeu le 2026-08-23 a 17:26.
    ///
    /// La lecon est la meme que celle de F-PLY-270, d'un cran plus haut : **un etat qui commande
    /// une decision se DECLARE, il ne se deduit pas de l'absence d'autre chose.**
    bool attente = false;
    /// L'entite RETROUVEE apres l'appel. Vide si on ne l'a pas retrouvee.
    ///
    /// ⚠️ **Elle ne vient PAS du retour de l'appel.** Le retour de la voie enrichie n'est pas un
    /// objet de script : le dereferencer a FAIT PLANTER LE JEU le 2026-08-21 (F-PLY-225). On ne le
    /// touche pas, on le journalise. L'entite se retrouve par ENUMERATION du monde.
    RED4ext::ent::EntityID entite{};
    /// Ce qu'un humain lit dans le journal. Contient les nombres, jamais un simple « echec ».
    std::string diag;
};

/// Tente de fabriquer un corps portant `aBlob` (format `TSV1`, tel que le serveur le stocke).
///
/// ⚠️ **Ne prend PAS de record.** Le record du serveur designe un passant, et la voie enrichie ne
/// produit rien d'observable sur un record de foule (F-PLY-207). Ce module choisit donc le sien —
/// voir `kRecordEnrichi` dans le `.cpp`, avec la raison complete.
///
/// Rend toujours — jamais d'exception, jamais de sortie sans diagnostic. `resultat.entite` vide
/// signifie « l'appelant doit reprendre la voie sure », quelle qu'en soit la raison.
/// ⚠️ `aCorpsMasculin` choisit l'ENTITE, donc la TENUE — jamais le corps, qui suit la charge
/// (F-PLY-267). Deux records derivés existent, un par sexe ; voir `kRecordEnrichi*`.
Resultat Tenter(std::uint64_t aNetworkId, const std::vector<std::uint8_t>& aBlob,
                const RED4ext::Vector4& aPosition, bool aCorpsMasculin = true);
}  // namespace Tessera::SpawnEnrichi

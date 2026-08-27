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

#include <cstddef>
#include <RED4ext/RED4ext.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Tessera::EsthetiqueV
{
// ── CE QUE CE MODULE PARTAGE, ET POURQUOI IL LE PARTAGE ──────────────────────────────────────────
//
// `TesseraSpawnEnrichi` a besoin des memes primitives : la meme disposition de tableau, la meme
// garde de lisibilite, et surtout LA MEME SELECTION DE GROUPES. Cette selection a coute trois jours
// de mesure et une erreur en jeu (« les bras sont dupliques », F-PLY-197 : les groupes d'une section
// sont des ALTERNATIVES, pas des couches). **Deux copies d'une telle liste sont une divergence en
// attente** — elles vivent donc ici, en un seul endroit, et s'exposent au lieu de se recopier.

/// Le tableau dynamique du moteur, tel que le binaire le dispose.
///
/// ⚠️ **La capacite PRECEDE la taille** — l'inverse de l'ordre qu'on suppose spontanement.
/// Confirme par QUATRE chemins independants : F-PLY-182, F-PLY-195, puis F-PLY-267 qui l'a relu
/// dans l'ajout unitaire du moteur ET dans l'assertion de debug de CDPR
/// (`redContainers/src/dynamicBuffer.cpp`, message nommant `capacity` et `elementSize`).
struct DynArray
{
    void* entries;
    std::uint32_t capacity;
    std::uint32_t size;
};

/// Vrai si `[aPtr, aPtr+aSize)` est mappe et lisible.
///
/// ⚠️ Exige un alignement sur 8 octets : c'est une garde pour les lectures de POINTEURS. Interroger
/// une adresse impaire rend TOUJOURS faux, et le refus est credible (F-PLY-171) — pour un drapeau a
/// offset impair, interroger le mot ALIGNE qui le contient.
///
/// ⚠️⚠️ « Lisible » n'est PAS « c'est un objet de la classe que je crois ». Entre les deux il y a un
/// crash, et il a ete paye le 2026-08-21 (F-PLY-225).
bool Lisible(std::uintptr_t aPtr, std::size_t aSize);

/// Un groupe d'esthetique : ou il vit dans l'etat, quelle fonction native le recolte, son nom.
///
/// ⚠️ `conteneur` est un OFFSET DANS L'ETAT (0x70 tete, 0x80 corps, 0x90 bras), `rva` est la RVA
/// de la fonction de recolte. Les deux se ressemblent a la lecture et ne sont pas interchangeables.
struct Groupe
{
    std::size_t conteneur;
    std::uint64_t rva;
    const char* nom;
    int section;
};

/// La table des six groupes — UNE SEULE definition, partagee par la recolte et par la sonde
/// `--tessera-sans-recolte`. En recopier une seconde les ferait diverger en silence.
const std::vector<Groupe>& Groupes();

/// Recolte les six groupes de customisation du V local dans `aCharge`, et rend le compte PAR
/// SECTION (Head, Body, Arms). `aEtat` doit etre un `gameuiCharacterCustomizationState` VERIFIE.
void Recolter(void* aEtat, std::uintptr_t aBase, DynArray& aCharge, std::uint32_t aParSection[3]);

/// RVA du tampon « tableau vide » partage du jeu : le point de depart d'une recolte.
std::uint64_t RvaSentinelle();

/// Decode un blob `TSV1` (octets bruts, tels que le serveur les stocke et les redistribue).
///
/// C'est la FRONTIERE DE CONFIANCE cote client : le blob vient du reseau. Tout y est verifie —
/// magic, octet reserve, coherence des compteurs avec la taille. Rend `false` et remplit
/// `aPourquoi` au moindre doute : ces octets vont servir a calculer des adresses d'ecriture.
bool Decoder(const std::vector<std::uint8_t>& aBlob, std::uint32_t& aHead, std::uint32_t& aBody,
             std::uint32_t& aArms, std::vector<std::uint64_t>& aPaires, std::string& aPourquoi);

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

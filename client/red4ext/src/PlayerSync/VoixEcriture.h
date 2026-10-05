#pragma once
// =====================================================================================
// ECRIVAIN DE LA MEMOIRE PARTAGEE VOIX (V4.2, lot 4 du plan
// `docs/superpowers/plans/2026-09-26-voix-spatiale.md`, dépôt Tessera) — LE CÔTÉ JEU, qui
// écrit RÉELLEMENT `Local\TesseraVoix-<pid>` chaque image, avec le protocole seqlock
// documenté par `VoixPartage.h`.
//
// Miroir volontaire du « faux jeu » de test déjà écrit et prouvé côté launcher
// (`EcrivainMemoirePartagee`, `tessera-core/voix-audio/src/memoire_partagee.rs`, testé par
// `le_lecteur_retrouve_ce_que_l_ecrivain_a_ecrit` le 2026-09-29) : même nom d'objet
// (`Local\TesseraVoix-<pid du PROCESSUS DU JEU>`), même protocole. Le `LecteurMemoirePartagee`
// du launcher, déjà prouvé contre ce faux jeu, n'a donc RIEN à changer pour lire le vrai.
//
// Ce fichier ne fait qu'ÉCRIRE. Rien, côté jeu, ne relit cette section — elle n'existe que
// pour le launcher.
// =====================================================================================

#include "VoixPartage.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace Tessera::Voix
{

class EcrivainVoixPartagee
{
public:
    ~EcrivainVoixPartagee() { Fermer(); }

    // Crée la section au premier appel (paresseux : rien à faire avant que le process — et
    // donc son pid — existe). Idempotent : un second appel ne recrée rien.
    bool AssurerOuverte()
    {
        if (m_handle != nullptr)
        {
            return true;
        }
        const DWORD pid = ::GetCurrentProcessId();
        char nom[64];
        std::snprintf(nom, sizeof(nom), "Local\\TesseraVoix-%lu", static_cast<unsigned long>(pid));
        m_handle = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         static_cast<DWORD>(sizeof(EtatVoixPartage)), nom);
        if (m_handle == nullptr)
        {
            return false;
        }
        m_vue = static_cast<EtatVoixPartage*>(
            ::MapViewOfFile(m_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EtatVoixPartage)));
        if (m_vue == nullptr)
        {
            ::CloseHandle(m_handle);
            m_handle = nullptr;
            return false;
        }
        // État initial cohérent AVANT la première vraie écriture : un lecteur qui ouvrirait la
        // section entre `CreateFileMappingA` et le premier `Ecrire()` ne doit jamais voir un
        // compteur impair (mémoire du pagefile à zéro = pair, donc déjà sûr), ni une version
        // fausse — même raison que le côté Rust (`EcrivainMemoirePartagee::creer`).
        std::memset(m_vue, 0, sizeof(EtatVoixPartage));
        m_vue->version = kVersionStructure;
        return true;
    }

    // Écrit un état complet, encadré par le seqlock (impair juste avant de toucher un champ,
    // pair une fois `etat` entièrement recopié). `etat.compteurEcriture` en entrée est ignoré :
    // ce protocole gère lui-même le compteur, pour qu'un appelant ne puisse pas accidentellement
    // publier un compteur impair et bloquer tous les lecteurs.
    void Ecrire(EtatVoixPartage etat)
    {
        if (!AssurerOuverte())
        {
            return;
        }
        const std::uint32_t impair = (m_vue->compteurEcriture + 1u) | 1u;
        m_vue->compteurEcriture = impair;
        etat.compteurEcriture = impair;
        etat.version = kVersionStructure;
        *m_vue = etat;
        m_vue->compteurEcriture = impair + 1u;
    }

private:
    void Fermer()
    {
        if (m_vue != nullptr)
        {
            ::UnmapViewOfFile(m_vue);
            m_vue = nullptr;
        }
        if (m_handle != nullptr)
        {
            ::CloseHandle(m_handle);
            m_handle = nullptr;
        }
    }

    HANDLE m_handle = nullptr;
    EtatVoixPartage* m_vue = nullptr;
};

} // namespace Tessera::Voix

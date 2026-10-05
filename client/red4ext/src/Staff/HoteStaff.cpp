// Les crochets de l'interface staff (lot H3, ADR 0055). Deplaces de la DLL de sonde
// (`tessera-staff-sonde/src/main.cpp`, Tessera) vers le netcode, et rendus PARESSEUX.
//
// ⚠️ JAMAIS VU EN JEU. Hypotheses non mesurees (D2), tranchees par le bloc STAFF :
//   · `hooking->Attach` pose A CHAUD (apres le demarrage du jeu, depuis le fil de la fenetre) --
//     la sonde le posait au chargement du plugin ; le plan H3 le veut « au demarrage de l'hote,
//     pas avant ». S'il refuse, le journal dit « crochet de presentation REFUSE » et la page ne
//     se dessine pas (le jeu continue). Repli : le poser au `Load`, inerte tant que non arme.
//   · la disposition de `pContext` (CET 1.37.1) : lue sous SEH, coupee si elle est fausse.
//
// FICHES DE CONSOMMATEUR (skill tessera-prise-autorite, six lignes) :
//
//  1. Procedure de fenetre du jeu
//     Cible        : GWLP_WNDPROC de la fenetre `W2ViewportClass` du processus
//     Lecteurs     : un, Windows (DispatchMessage) ; la chaine en aval = CET (canal dev) puis le jeu
//     Alimente     : les entrees que voit la page (souris libre) ou le jeu (sinon)
//     Domaine      : une WNDPROC ; tout message non consomme est rendu a la procedure precedente
//     Hors domaine : ne pas rendre la main = fenetre gelee -> on appelle TOUJOURS la suivante
//     Qui d'autre  : CET sous-classe la meme fenetre (une fois, a son demarrage). On ne se
//                    retire JAMAIS a chaud : on se desarme (retirer casserait sa chaine).
//
//  2. Presentation (`GpuApi::Present`, empreinte 2468877568)
//     Cible        : la fonction de presentation interne, detournee par RED4ext (Detours)
//     Lecteurs     : un, le fil de rendu, une fois par image
//     Alimente     : la composition de la page par-dessus le tampon d'arriere-plan
//     Domaine      : on LIT `pContext` (chaine d'echange, file de commandes), on n'y ecrit rien
//     Hors domaine : disposition inconnue -> QueryInterface echoue ou SEH -> composition coupee
//     Qui d'autre  : CET detourne la MEME fonction (canal dev) ; les detours s'empilent (tir A2)
//
//  3. `souris{libre}` -> `Tessera_SourisStaffLibre` (lu par `UiKitStaffSouris.reds`)
//     Cible        : un booleen atomique, a nous
//     Lecteurs     : un, la veille redscript, toutes les 0,1 s (PULL)
//     Alimente     : le contexte modal du jeu (`InGamePopup` de Codeware)
//     Domaine      : vrai / faux
//     Hors domaine : sans objet
//     Qui d'autre  : l'hote seul (F2), via `Rappel` ; remis a faux quand le rang est retire
//
// Fils : `Droits`, `Connexion`, `VersPage`, `Tirer` viennent du fil du reseau ; l'hote exige que
// `demarrer`, `entree`, `message` soient appeles sur le fil de la fenetre -> tout passe par un
// message prive poste a la fenetre. `image` est appelee sur le fil de rendu.

#include "HoteStaff.h"

#include "PontStaff.h"
#include "tessera_staff_hote.h"

#include "../CommandLine.h"
#include "../Main.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace Tessera::Staff::Hote
{
namespace
{
constexpr std::uint32_t kEmpreintePresent = 2468877568UL;  // GpuApi::Present
constexpr std::uint32_t kEmpreinteContexte = 1239944840UL; // CRenderGlobal, pointeur d'instance
// Disposition de `RenderContext` : CET v1.37.1, `src/reverse/RenderContext.h` (lue au TAG, F-PLF-252).
constexpr std::size_t kDecalagePeripheriques = 0xC97F38;
constexpr std::size_t kTaillePeripherique = 0xB0;
constexpr std::size_t kDecalageFile = 0x13BC4D0;
constexpr UINT kMessageDemarrer = WM_APP + 0x7E4;
constexpr UINT kMessageVersPage = WM_APP + 0x7E3;
constexpr std::size_t kFileMax = 256; // un catalogue de joueurs tient en UN evenement : 256 suffit

enum Etat { Absent = 0, Demarre = 1, Refuse = 2, EnCours = 3 };

using Present_t = void* (*)(std::int32_t*, std::uint8_t, UINT);
Present_t g_presentReel = nullptr;
void** g_contexte = nullptr;

HMODULE g_hote = nullptr;
decltype(&staff_hote_demarrer) g_demarrer = nullptr;
decltype(&staff_hote_arreter) g_arreter = nullptr;
decltype(&staff_hote_image) g_image = nullptr;
decltype(&staff_hote_entree) g_entree = nullptr;
decltype(&staff_hote_message) g_message = nullptr;

std::atomic<int> g_etat{Absent};
std::atomic<bool> g_arme{false};    // l'hote a demarre
std::atomic<bool> g_visible{false}; // le joueur a un rang staff MAINTENANT
std::atomic<bool> g_pagePrete{false};
std::atomic<bool> g_sourisLibre{false};
std::atomic<bool> g_contexteFaux{false};
std::atomic<std::uint64_t> g_rejets{0}, g_perdus{0};

HWND g_fenetre = nullptr;
WNDPROC g_procReelle = nullptr;
std::string g_dossier;

std::mutex g_verrou; // protege les quatre suivants
std::deque<std::string> g_versPage;
std::deque<std::string> g_versServeur;
std::string g_droits = JsonDroits({});
bool g_connecte = false;

std::wstring DossierHote()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH); // <jeu>\bin\x64\Cyberpunk2077.exe
    std::wstring chemin(exe);
    for (int i = 0; i < 3; ++i)
    {
        const auto p = chemin.find_last_of(L'\\');
        if (p == std::wstring::npos)
            break;
        chemin.erase(p);
    }
    // PAS sous `red4ext\plugins\` : le launcher y charge chaque .dll pour l'essayer (F-PLF-256).
    return chemin + L"\\tessera\\staff-hote";
}

std::string Utf8(const std::wstring& w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Fil de la fenetre.
void Donner(const std::string& json)
{
    const int code = g_message(json.data(), json.size());
    if (code != STAFF_HOTE_OK)
    {
        ++g_perdus;
        SDK->logger->WarnF(PLUGIN, "[Staff] staff_hote_message -> %d (message perdu : %.80s)", code, json.c_str());
    }
}

// Page -> netcode, sur le fil de la fenetre (pendant `staff_hote_entree`).
void Rappel(void*, const char* json, std::size_t longueur)
{
    const std::string texte(json, longueur);
    MessagePage m;
    if (!LireMessagePage(texte, m))
    {
        if (g_rejets++ == 0)
            SDK->logger->WarnF(PLUGIN, "[Staff] message de la page REFUSE (hors liste fermee) : %.120s", texte.c_str());
        return;
    }
    switch (m.type)
    {
    case TypePage::Souris:
        g_sourisLibre = m.libre && g_visible.load();
        SDK->logger->InfoF(PLUGIN, "[Staff] souris=%s", m.libre ? "libre" : "jeu");
        return;
    case TypePage::Pret:
    {
        // La page vient de (re)charger : elle ne sait rien. On lui rend l'etat courant.
        std::string droits;
        bool connecte;
        {
            std::lock_guard<std::mutex> v(g_verrou);
            droits = g_droits;
            connecte = g_connecte;
        }
        g_pagePrete = true;
        Donner(JsonEtatConnexion(connecte));
        Donner(droits);
        SDK->logger->Info(PLUGIN, "[Staff] page prete : etat de connexion et droits rendus");
        return;
    }
    default:
    {
        // Commande, abonnement, accuse : au fil du reseau. Sans rang, rien ne part.
        if (!g_visible.load())
            return;
        std::lock_guard<std::mutex> v(g_verrou);
        if (g_versServeur.size() >= kFileMax)
        {
            ++g_perdus;
            return;
        }
        g_versServeur.push_back(texte);
    }
    }
}

bool LireContexte(const std::int32_t* indice, IUnknown** chaine, IUnknown** file)
{
    __try
    {
        const auto* base = static_cast<const std::uint8_t*>(*g_contexte);
        const std::int32_t i = *indice - 1;
        if (!base || i < 0 || i >= 0x30)
            return false;
        auto* c = *reinterpret_cast<IUnknown* const*>(base + kDecalagePeripheriques + kTaillePeripherique * i);
        auto* f = *reinterpret_cast<IUnknown* const*>(base + kDecalageFile);
        if (!c || !f)
            return false;
        IDXGISwapChain3* c3 = nullptr;
        ID3D12CommandQueue* f1 = nullptr;
        const bool bon = SUCCEEDED(c->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&c3))) &&
                         SUCCEEDED(f->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&f1)));
        if (c3)
            c3->Release();
        if (f1)
            f1->Release();
        *chaine = c;
        *file = f;
        return bon;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Fil de rendu, une fois par image.
void* PresentCrochet(std::int32_t* indice, std::uint8_t synchro, UINT intervalle)
{
    static std::uint64_t presentations = 0, debutFenetre = GetTickCount64();
    static std::int64_t imagesAvant = 0, dernierCode = 0;
    static bool contexteVerifie = false;
    ++presentations;

    if (g_arme.load(std::memory_order_relaxed) && g_visible.load(std::memory_order_relaxed) &&
        !g_contexteFaux.load(std::memory_order_relaxed))
    {
        IUnknown* chaine = nullptr;
        IUnknown* file = nullptr;
        if (contexteVerifie)
        {
            const auto* base = static_cast<const std::uint8_t*>(*g_contexte);
            chaine = *reinterpret_cast<IUnknown* const*>(base + kDecalagePeripheriques +
                                                         kTaillePeripherique * (*indice - 1));
            file = *reinterpret_cast<IUnknown* const*>(base + kDecalageFile);
        }
        else if (LireContexte(indice, &chaine, &file))
        {
            contexteVerifie = true;
            SDK->logger->InfoF(PLUGIN, "[Staff] pContext lu : peripherique %d, chaine %p, file %p", *indice,
                               static_cast<void*>(chaine), static_cast<void*>(file));
        }
        else
        {
            g_contexteFaux = true;
            SDK->logger->ErrorF(PLUGIN, "[Staff] pContext ILLISIBLE (indice %d) : composition coupee, le jeu continue.",
                                indice ? *indice : -1);
        }
        if (chaine && file && !g_contexteFaux.load())
            dernierCode = g_image(chaine, file);
    }

    // Un zero y est toujours qualifie (D1) : on dit si on a PU regarder.
    const std::uint64_t maintenant = GetTickCount64();
    if (maintenant - debutFenetre >= 5000)
    {
        const bool recues = dernierCode >= 0;
        SDK->logger->InfoF(PLUGIN,
                           "[Staff] presentations=%llu (%.1f i/s) images_page=+%lld (code %lld) souris=%s visible=%d "
                           "page_prete=%d rejets=%llu perdus=%llu contexte=%s",
                           presentations, presentations * 1000.0 / static_cast<double>(maintenant - debutFenetre),
                           recues ? dernierCode - imagesAvant : 0, dernierCode, g_sourisLibre.load() ? "libre" : "jeu",
                           g_visible.load() ? 1 : 0, g_pagePrete.load() ? 1 : 0, g_rejets.load(), g_perdus.load(),
                           g_contexteFaux.load() ? "ILLISIBLE" : "ok");
        if (recues)
            imagesAvant = dernierCode;
        presentations = 0;
        debutFenetre = maintenant;
    }
    return g_presentReel(indice, synchro, intervalle);
}

// Fil de la fenetre. Le crochet de presentation n'est pose QUE si l'hote a demarre.
void DemarrerHote(HWND fenetre)
{
    const std::wstring dossier = DossierHote();
    const std::wstring dll = dossier + L"\\tessera_staff_hote.dll";
    // Chemin ABSOLU + recherche alteree : `libcef.dll` se resout a cote de l'hote, jamais ailleurs.
    g_hote = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_hote)
    {
        g_etat = Refuse;
        SDK->logger->ErrorF(PLUGIN, "[Staff] hote=refuse : LoadLibraryEx(%ls) erreur Windows %lu", dll.c_str(),
                            GetLastError());
        return;
    }
#define SYMBOLE(var, nom) var = reinterpret_cast<decltype(var)>(GetProcAddress(g_hote, nom))
    SYMBOLE(g_demarrer, "staff_hote_demarrer");
    SYMBOLE(g_arreter, "staff_hote_arreter");
    SYMBOLE(g_image, "staff_hote_image");
    SYMBOLE(g_entree, "staff_hote_entree");
    SYMBOLE(g_message, "staff_hote_message");
#undef SYMBOLE
    if (!g_demarrer || !g_arreter || !g_image || !g_entree || !g_message)
    {
        g_etat = Refuse;
        SDK->logger->Error(PLUGIN, "[Staff] hote=refuse : une des cinq fonctions manque dans tessera_staff_hote.dll");
        return;
    }
    RECT zone = {};
    GetClientRect(fenetre, &zone);
    g_dossier = Utf8(dossier);
    static const char page[] = "tessera://staff/index.html";
    staff_hote_config config = {};
    config.version = STAFF_HOTE_VERSION;
    config.fenetre = fenetre;
    config.largeur = static_cast<std::uint32_t>(zone.right - zone.left);
    config.hauteur = static_cast<std::uint32_t>(zone.bottom - zone.top);
    config.dossier = g_dossier.c_str();
    config.dossier_longueur = g_dossier.size();
    config.page = page;
    config.page_longueur = sizeof(page) - 1;
    // Port de debogage 9229 : sous --tessera-dev seulement.
    config.outils_dev = ModeDeveloppementDemande(GetCommandLineA()) ? 1u : 0u;
    config.rappel = &Rappel;
    const int code = g_demarrer(&config);
    SDK->logger->InfoF(PLUGIN, "[Staff] staff_hote_demarrer -> %d (fenetre %p, %ux%u, dossier %s)", code,
                       static_cast<void*>(fenetre), config.largeur, config.hauteur, g_dossier.c_str());
    if (code != STAFF_HOTE_OK)
    {
        g_etat = Refuse;
        return;
    }
    g_etat = Demarre;
    g_arme = true;

    const HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
    using Resoudre_t = std::uintptr_t (*)(std::uint32_t);
    // Pas `UniversalRelocPtr` : une empreinte inconnue y TUE le jeu sur une boite de dialogue.
    const auto resoudre =
        red4ext ? reinterpret_cast<Resoudre_t>(GetProcAddress(red4ext, "RED4ext_ResolveAddress")) : nullptr;
    const auto present = resoudre ? resoudre(kEmpreintePresent) : 0;
    const auto contexte = resoudre ? resoudre(kEmpreinteContexte) : 0;
    if (!present || !contexte)
    {
        SDK->logger->Error(PLUGIN, "[Staff] crochet de presentation REFUSE : empreinte d'adresse inconnue. "
                                   "La page tourne mais ne se dessine pas ; le jeu continue.");
        return;
    }
    g_contexte = reinterpret_cast<void**>(contexte);
    if (!SDK->hooking->Attach(PLUGIN, reinterpret_cast<void*>(present), reinterpret_cast<void*>(&PresentCrochet),
                              reinterpret_cast<void**>(&g_presentReel)))
    {
        SDK->logger->Error(PLUGIN, "[Staff] crochet de presentation REFUSE (Attach a chaud). "
                                   "La page tourne mais ne se dessine pas ; le jeu continue.");
        return;
    }
    SDK->logger->Info(PLUGIN, "[Staff] crochet de presentation pose a chaud");
}

LRESULT CALLBACK Procedure(HWND fenetre, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == kMessageDemarrer)
    {
        if (g_etat.load() == EnCours)
            DemarrerHote(fenetre);
        return 0;
    }
    if (g_arme.load(std::memory_order_relaxed))
    {
        if (message == kMessageVersPage)
        {
            std::deque<std::string> lot;
            {
                std::lock_guard<std::mutex> v(g_verrou);
                lot.swap(g_versPage);
            }
            for (const auto& json : lot)
                Donner(json);
            // Rang retire pendant que la souris etait libre : on rend la souris au jeu par la
            // voie de l'hote lui-meme (F2), pour que SON etat et le notre restent egaux.
            if (!g_visible.load() && g_sourisLibre.exchange(false))
            {
                const staff_hote_entree_win f2{WM_KEYDOWN, VK_F2, 0};
                g_entree(&f2);
            }
            return 0;
        }
        if (message == WM_DESTROY)
        {
            g_arme = false;
            g_sourisLibre = false;
            g_arreter();
            SDK->logger->Info(PLUGIN, "[Staff] WM_DESTROY : hote arrete");
        }
        else if (g_visible.load(std::memory_order_relaxed) || message == STAFF_HOTE_WM_PONT || message == WM_SIZE)
        {
            const staff_hote_entree_win entree{message, static_cast<std::uint64_t>(wparam),
                                               static_cast<std::int64_t>(lparam)};
            if (g_entree(&entree) == 1)
                return 0;
        }
    }
    return CallWindowProcW(g_procReelle, fenetre, message, wparam, lparam);
}

BOOL CALLBACK Enumerer(HWND fenetre, LPARAM sortie)
{
    DWORD processus = 0;
    GetWindowThreadProcessId(fenetre, &processus);
    if (processus == GetCurrentProcessId())
    {
        wchar_t classe[64] = {};
        RealGetWindowClassW(fenetre, classe, 63);
        if (wcscmp(classe, L"W2ViewportClass") == 0)
        {
            *reinterpret_cast<HWND*>(sortie) = fenetre;
            return FALSE;
        }
    }
    return TRUE;
}

void Reveiller()
{
    if (g_arme.load() && g_fenetre)
        PostMessageW(g_fenetre, kMessageVersPage, 0, 0);
}

// Premier rang staff du processus : maillon de fenetre, puis demarrage sur le fil de la fenetre.
void Demander()
{
    int attendu = Absent;
    if (!g_etat.compare_exchange_strong(attendu, EnCours))
        return;
    HWND trouvee = nullptr;
    EnumWindows(&Enumerer, reinterpret_cast<LPARAM>(&trouvee));
    if (!trouvee)
    {
        // Le `PermissionSync` suivant reessaiera.
        g_etat = Absent;
        SDK->logger->Warn(PLUGIN, "[Staff] fenetre W2ViewportClass introuvable : hote non demarre");
        return;
    }
    g_fenetre = trouvee;
    g_procReelle =
        reinterpret_cast<WNDPROC>(SetWindowLongPtrW(trouvee, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Procedure)));
    SDK->logger->InfoF(PLUGIN, "[Staff] rang staff : fenetre %p sous-classee (suivant : %p), demarrage de l'hote",
                       static_cast<void*>(trouvee), reinterpret_cast<void*>(g_procReelle));
    PostMessageW(trouvee, kMessageDemarrer, 0, 0);
}
} // namespace

void Droits(const std::vector<std::string>& noeuds)
{
    const bool staff = RangEstStaff(noeuds);
    {
        std::lock_guard<std::mutex> v(g_verrou);
        // Sans rang, la page ne recoit AUCUN noeud : elle n'a pas a connaitre ceux d'un joueur.
        g_droits = JsonDroits(staff ? noeuds : std::vector<std::string>{});
        if (g_pagePrete.load())
            g_versPage.push_back(g_droits);
    }
    const bool avant = g_visible.exchange(staff);
    if (avant != staff)
        SDK->logger->InfoF(PLUGIN, "[Staff] rang staff : %s", staff ? "OUI" : "NON (page masquee, CEF reste)");
    if (staff)
        Demander();
    Reveiller();
}

void Connexion(bool connecte)
{
    {
        std::lock_guard<std::mutex> v(g_verrou);
        g_connecte = connecte;
        if (g_pagePrete.load())
            g_versPage.push_back(JsonEtatConnexion(connecte));
    }
    Reveiller();
}

void VersPage(std::string json)
{
    if (!g_pagePrete.load() || !g_visible.load())
    {
        ++g_perdus;
        return;
    }
    {
        std::lock_guard<std::mutex> v(g_verrou);
        if (g_versPage.size() >= kFileMax)
        {
            ++g_perdus;
            return;
        }
        g_versPage.push_back(std::move(json));
    }
    Reveiller();
}

bool Tirer(std::string& json)
{
    std::lock_guard<std::mutex> v(g_verrou);
    if (g_versServeur.empty())
        return false;
    json = std::move(g_versServeur.front());
    g_versServeur.pop_front();
    return true;
}

bool SourisLibre()
{
    return g_sourisLibre.load(std::memory_order_relaxed);
}

void Arreter()
{
    g_arme = false;
    g_visible = false;
    g_sourisLibre = false;
}
} // namespace Tessera::Staff::Hote

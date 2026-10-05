// =====================================================================================
// RELAIS D'ESSAI HORS JEU du pont staff (lot H3, preuve D1 reportee du lot H2-D).
//
// Une DLL de TEST, jamais livree. `essai.exe` (tessera-staff-hote) la charge par
// `--relais <dll> --serveur <ip:port> --nom <compte>` : elle joue le NETCODE, sans le jeu.
// La chaine est alors : page reelle (dist) -> hote CEF reel -> rappel -> CE FICHIER ->
// GameNetworkingSockets reel -> serveur local-dev reel, et retour.
//
// CE QUI EST LE VRAI CODE DU CLIENT ICI : `Staff/PontStaff.h` (lecture des cinq types, JSON vers
//   la page, regle du rang, registre des abonnements), `generated/protocol_generated.h`, et la
//   bibliotheque GNS que le client lie.
// CE QUI NE L'EST PAS : le cablage. Ce fichier REECRIT, en plus court, ce que font
//   `NetworkGameSystem.cpp` (les `case` du routeur, `RelayerPontStaff`, les trois `Send*`) et
//   `HoteStaff.cpp` (l'etat rendu sur `pret`). Une faute dans CES fichiers-la ne se voit pas ici.
//   Ni les fils (tout tourne sur un seul), ni RED4ext, ni le crochet de presentation.
//
// Cible CMake `relais_staff_essai`, EXCLUE de la construction par defaut.
// =====================================================================================

#include "../src/Staff/PontStaff.h"
#include "../src/generated/protocol_generated.h"

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace proto = cyberpunk_rp::protocol;
using namespace Tessera::Staff;

namespace
{
ISteamNetworkingSockets* g_interface = nullptr;
HSteamNetConnection g_connexion = k_HSteamNetConnection_Invalid;
std::string g_nom;
std::deque<std::string> g_versPage;
std::string g_droits = JsonDroits({});
bool g_connecte = false, g_selection = false, g_enJeu = false;
Abonnements g_abonnements;
std::chrono::steady_clock::time_point g_dernierePose;

void Dire(const char* format, ...)
{
    va_list a;
    va_start(a, format);
    std::printf("relais : ");
    std::vprintf(format, a);
    std::printf("\n");
    std::fflush(stdout);
    va_end(a);
}

void Envoyer(flatbuffers::FlatBufferBuilder& b, bool fiable = true)
{
    g_interface->SendMessageToConnection(g_connexion, b.GetBufferPointer(), b.GetSize(),
                                         fiable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable,
                                         nullptr);
}

template <typename T> void Enveloppe(flatbuffers::FlatBufferBuilder& b, proto::ClientMsg type, flatbuffers::Offset<T> m)
{
    b.Finish(proto::CreateClientEnvelope(b, type, m.Union()));
    Envoyer(b);
}

void Abonner(const std::string& sujet, bool actif)
{
    flatbuffers::FlatBufferBuilder b;
    const auto s = b.CreateString(sujet);
    Enveloppe(b, proto::ClientMsg_StaffSubscribe, proto::CreateStaffSubscribe(b, s, actif));
    Dire("-> StaffSubscribe sujet=%s actif=%d", sujet.c_str(), actif ? 1 : 0);
}

void EtatChange(SteamNetConnectionStatusChangedCallback_t* info)
{
    const auto etat = info->m_info.m_eState;
    Dire("etat de connexion %d %s", etat, info->m_info.m_szEndDebug);
    if (etat == k_ESteamNetworkingConnectionState_Connected)
    {
        g_connecte = true;
        g_versPage.push_back(JsonEtatConnexion(true));
        flatbuffers::FlatBufferBuilder b;
        const auto nom = b.CreateString(g_nom);
        // Comme le client (`SendJoin`) : le jeton vient de l'environnement, vide sur un serveur prive.
        const char* jetonEnv = std::getenv("TESSERA_JOIN_TOKEN");
        const auto jeton = b.CreateString(jetonEnv ? jetonEnv : "");
        Enveloppe(b, proto::ClientMsg_Join, proto::CreateJoin(b, nom, jeton, 2 /* kTesseraProtocolVersion */));
        Dire("-> Join %s", g_nom.c_str());
    }
    else if (etat == k_ESteamNetworkingConnectionState_ClosedByPeer ||
             etat == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
    {
        g_connecte = false;
        g_versPage.push_back(JsonEtatConnexion(false));
        g_interface->CloseConnection(info->m_hConn, 0, nullptr, false);
        g_connexion = k_HSteamNetConnection_Invalid;
    }
}

void Recevoir(const proto::ServerEnvelope* env)
{
    switch (env->msg_type())
    {
    case proto::ServerMsg_CharacterList:
    {
        const auto* liste = env->msg_as_CharacterList()->characters();
        if (!liste || liste->size() == 0)
        {
            Dire("AUCUN personnage sur ce compte : en creer un d'abord (ghost --nom %s)", g_nom.c_str());
            break;
        }
        if (!g_selection)
        {
            g_selection = true;
            flatbuffers::FlatBufferBuilder b;
            Enveloppe(b, proto::ClientMsg_SelectCharacter, proto::CreateSelectCharacter(b, liste->Get(0)->id()));
            Dire("-> SelectCharacter %llu", static_cast<unsigned long long>(liste->Get(0)->id()));
        }
        break;
    }
    case proto::ServerMsg_ConfigSync:
    case proto::ServerMsg_Snapshot:
        if (g_selection && !g_enJeu)
        {
            g_enJeu = true;
            Dire("dans le monde");
        }
        break;
    case proto::ServerMsg_PermissionSync:
    {
        std::vector<std::string> noeuds;
        if (const auto* n = env->msg_as_PermissionSync()->nodes())
            for (const auto* x : *n)
                noeuds.push_back(x->str());
        const bool staff = RangEstStaff(noeuds);
        g_droits = JsonDroits(staff ? noeuds : std::vector<std::string>{});
        g_versPage.push_back(g_droits);
        Dire("<- PermissionSync %zu noeud(s), rang staff=%d", noeuds.size(), staff ? 1 : 0);
        if (staff)
            for (const auto& s : g_abonnements.Actifs())
                Abonner(s, true);
        else
            g_abonnements.Vider();
        break;
    }
    case proto::ServerMsg_CommandResult:
    {
        const auto* r = env->msg_as_CommandResult();
        const std::string texte = r->message() ? r->message()->str() : std::string();
        Dire("<- CommandResult request_id=%u ok=%d : %.200s", r->request_id(), r->success() ? 1 : 0, texte.c_str());
        if (r->request_id() != 0)
            g_versPage.push_back(JsonReponse(r->request_id(), r->success(), texte));
        break;
    }
    case proto::ServerMsg_StaffEvent:
    {
        const auto* e = env->msg_as_StaffEvent();
        if (!e->sujet())
            break;
        const std::string_view charge = e->charge_json() ? e->charge_json()->string_view() : std::string_view();
        Dire("<- StaffEvent sujet=%s : %.300s", e->sujet()->c_str(), std::string(charge).c_str());
        g_versPage.push_back(JsonEvenement(e->sujet()->string_view(), charge));
        break;
    }
    case proto::ServerMsg_Kicked:
        Dire("<- Kicked");
        break;
    default:
        break;
    }
}
} // namespace

extern "C"
{
__declspec(dllexport) int relais_demarrer(const char* adresse, const char* nom)
{
    SteamDatagramErrMsg erreur;
    if (!GameNetworkingSockets_Init(nullptr, erreur))
    {
        Dire("GameNetworkingSockets_Init : %s", erreur);
        return -1;
    }
    g_interface = SteamNetworkingSockets();
    g_nom = nom;
    SteamNetworkingIPAddr ip = {};
    if (!ip.ParseString(adresse))
        return -2;
    SteamNetworkingConfigValue_t option = {};
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&EtatChange));
    g_connexion = g_interface->ConnectByIPAddress(ip, 1, &option);
    Dire("connexion a %s en tant que %s", adresse, nom);
    return g_connexion == k_HSteamNetConnection_Invalid ? -3 : 0;
}

// Page -> serveur : ce que `Rappel` (HoteStaff.cpp) et `RelayerPontStaff` (NetworkGameSystem.cpp) font.
__declspec(dllexport) void relais_page(const char* json, size_t longueur)
{
    MessagePage m;
    if (!LireMessagePage(std::string_view(json, longueur), m))
    {
        Dire("message de la page REFUSE : %.120s", std::string(json, longueur).c_str());
        return;
    }
    if (m.type == TypePage::Pret)
    {
        g_versPage.push_back(JsonEtatConnexion(g_connecte));
        g_versPage.push_back(g_droits);
        return;
    }
    if (g_connexion == k_HSteamNetConnection_Invalid)
        return;
    flatbuffers::FlatBufferBuilder b;
    switch (m.type)
    {
    case TypePage::Commande:
    {
        const auto t = b.CreateString(m.texte);
        Enveloppe(b, proto::ClientMsg_AdminCommand, proto::CreateAdminCommand(b, t, m.requestId));
        Dire("-> AdminCommand request_id=%u texte=%s", m.requestId, m.texte.c_str());
        break;
    }
    case TypePage::Abonner:
        if (g_abonnements.Appliquer(m.sujet, m.actif))
            Abonner(m.sujet, m.actif);
        break;
    case TypePage::AvertissementVu:
        Enveloppe(b, proto::ClientMsg_StaffWarningAck, proto::CreateStaffWarningAck(b, m.idAvertissement));
        break;
    default:
        break;
    }
}

// Pompe le reseau, puis rend UN message pour la page (0 = rien). A appeler a chaque image.
__declspec(dllexport) int relais_tirer(char* tampon, size_t capacite)
{
    if (g_interface)
    {
        g_interface->RunCallbacks();
        while (g_connexion != k_HSteamNetConnection_Invalid)
        {
            ISteamNetworkingMessage* message = nullptr;
            if (g_interface->ReceiveMessagesOnConnection(g_connexion, &message, 1) <= 0)
                break;
            const auto* octets = static_cast<const uint8_t*>(message->GetData());
            flatbuffers::Verifier v(octets, static_cast<size_t>(message->GetSize()));
            if (v.VerifyBuffer<proto::ServerEnvelope>(nullptr))
                Recevoir(flatbuffers::GetRoot<proto::ServerEnvelope>(octets));
            message->Release();
        }
        // Le serveur ne compte « en jeu » qu'un client qui donne sa position.
        const auto maintenant = std::chrono::steady_clock::now();
        if (g_enJeu && g_connexion != k_HSteamNetConnection_Invalid &&
            maintenant - g_dernierePose > std::chrono::milliseconds(100))
        {
            g_dernierePose = maintenant;
            const auto q = [](float m) { return static_cast<int32_t>(std::lround(m * 131072.0f)); };
            const proto::QVec3 pos(q(-1430.0f), q(1261.0f), q(23.0f));
            flatbuffers::FlatBufferBuilder b;
            b.Finish(proto::CreateClientEnvelope(b, proto::ClientMsg_PositionUpdate,
                                                 proto::CreatePositionUpdate(b, &pos).Union()));
            Envoyer(b, false);
        }
    }
    if (g_versPage.empty())
        return 0;
    const std::string& json = g_versPage.front();
    if (json.size() > capacite)
    {
        Dire("message trop gros pour le tampon (%zu octets), jete", json.size());
        g_versPage.pop_front();
        return 0;
    }
    std::memcpy(tampon, json.data(), json.size());
    const int n = static_cast<int>(json.size());
    g_versPage.pop_front();
    return n;
}

__declspec(dllexport) void relais_arreter()
{
    if (g_interface && g_connexion != k_HSteamNetConnection_Invalid)
        g_interface->CloseConnection(g_connexion, 0, "fin de l'essai", true);
    g_connexion = k_HSteamNetConnection_Invalid;
}
}

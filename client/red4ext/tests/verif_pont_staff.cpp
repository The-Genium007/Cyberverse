// =====================================================================================
// Auto-verification du relais du pont de l'interface staff (lot H3). AUCUN framework, AUCUN
// type moteur, aucun reseau -- meme patron que `verif_voix_partage.cpp`.
//
// PROUVE : la regle « rang staff » (demarrage paresseux de l'hote), la lecture stricte des
//   cinq types que la page a le droit d'envoyer, le numero de demande qui tient dans le
//   `request_id:uint` du protocole, le JSON ecrit vers la page (echappement compris), et le
//   registre des abonnements rejoue a la reconnexion.
// NE PROUVE PAS : que l'hote CEF se charge dans le jeu, que le crochet de presentation se pose
//   a chaud, ni qu'un message traverse GNS -- tout cela exige le jeu (bloc STAFF).
//
// N'est PAS dans `src/CMakeLists.txt` (porte un `main()`).
//   cl /nologo /std:c++20 /W4 /WX /EHsc /utf-8 /I ..\src verif_pont_staff.cpp && verif_pont_staff.exe
// =====================================================================================

#include "../src/Staff/PontStaff.h"

#include <cstdio>

using namespace Tessera::Staff;

static int g_echecs = 0;

#define VERIFIER(cond)                                                                             \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            ++g_echecs;                                                                            \
            std::printf("ECHEC ligne %d : %s\n", __LINE__, #cond);                                 \
        }                                                                                          \
    } while (0)

static bool Lit(const char* json, MessagePage& m)
{
    m = MessagePage{};
    return LireMessagePage(json, m);
}

static void le_rang_staff_decide_du_chargement()
{
    VERIFIER(!RangEstStaff({}));
    VERIFIER(!RangEstStaff({"player.chat", "server.queue_bypass"}));
    VERIFIER(!RangEstStaff({"administrateur.x", "admin", "xadmin.fly"}));
    VERIFIER(RangEstStaff({"player.chat", "admin.moderation.warn"}));
    VERIFIER(RangEstStaff({"admin.*"}));
    VERIFIER(RangEstStaff({"*"}));
}

static void les_cinq_types_se_lisent()
{
    MessagePage m;
    VERIFIER(Lit(R"({"type":"commande","charge":{"request_id":7,"texte":"/fiche 3"}})", m));
    VERIFIER(m.type == TypePage::Commande && m.requestId == 7 && m.texte == "/fiche 3");
    // L'ordre des champs est libre, les espaces aussi ; les echappements JSON sont defaits.
    VERIFIER(Lit("{ \"charge\" : { \"texte\" : \"/msg 3 \\\"h\\u00e9\\\"\\n\\ud83d\\ude00\" , \"request_id\" : 4294967295 } , \"type\" : \"commande\" }", m));
    VERIFIER(m.requestId == 4294967295u && m.texte == "/msg 3 \"h\xC3\xA9\"\n\xF0\x9F\x98\x80");
    VERIFIER(Lit(R"({"type":"abonner","charge":{"sujet":"joueurs","actif":true}})", m));
    VERIFIER(m.type == TypePage::Abonner && m.sujet == "joueurs" && m.actif);
    VERIFIER(Lit(R"({"type":"abonner","charge":{"sujet":"chat.staff","actif":false}})", m));
    VERIFIER(m.sujet == "chat.staff" && !m.actif);
    VERIFIER(Lit(R"({"type":"souris","charge":{"libre":true}})", m));
    VERIFIER(m.type == TypePage::Souris && m.libre);
    VERIFIER(Lit(R"({"type":"pret","charge":{}})", m));
    VERIFIER(m.type == TypePage::Pret);
    // u64 entier : le bit de poids fort pose (serveur sans base) ne tient pas dans un double.
    VERIFIER(Lit(R"({"type":"avertissement_vu","charge":{"id":"9223372036854775809"}})", m));
    VERIFIER(m.type == TypePage::AvertissementVu && m.idAvertissement == 9223372036854775809ull);
}

static void tout_le_reste_est_refuse()
{
    MessagePage m;
    for (const char* mauvais : {
             "",
             "pas du json",
             "[1,2]",
             R"({"type":"reponse","charge":{"request_id":1,"ok":true,"message":""}})",
             R"({"type":"droits","charge":{"noeuds":[]}})",
             R"({"type":3,"charge":{}})",
             R"({"type":"pret"})",
             R"({"type":"pret","charge":[]})",
             R"({"type":"pret","charge":{"en_trop":1}})",
             R"({"type":"pret","charge":{},"x":1})",
             R"({"type":"pret","charge":{}} suite)",
             R"({"type":"commande","charge":{"request_id":1}})",
             R"({"type":"commande","charge":{"request_id":"1","texte":"x"}})",
             R"({"type":"commande","charge":{"request_id":-1,"texte":"x"}})",
             R"({"type":"commande","charge":{"request_id":1.5,"texte":"x"}})",
             // 0 est le numero de la console tapee a la main : une reponse numerotee 0 irait a
             // la console, jamais a la page.
             R"({"type":"commande","charge":{"request_id":0,"texte":"x"}})",
             // Au-dela de `uint` : le serveur recopierait un numero tronque.
             R"({"type":"commande","charge":{"request_id":4294967296,"texte":"x"}})",
             R"({"type":"commande","charge":{"request_id":1,"texte":""}})",
             R"({"type":"commande","charge":{"request_id":1,"texte":"x","texte":"y"}})",
             R"({"type":"commande","charge":{"request_id":1,"texte":"a\u0000b"}})",
             R"({"type":"commande","charge":{"request_id":1,"texte":"\ud83d"}})",
             R"({"type":"abonner","charge":{"sujet":"","actif":true}})",
             R"({"type":"abonner","charge":{"sujet":"joueurs","actif":"oui"}})",
             R"({"type":"souris","charge":{"libre":1}})",
             R"({"type":"avertissement_vu","charge":{"id":5}})",
             R"({"type":"avertissement_vu","charge":{"id":""}})",
             R"({"type":"avertissement_vu","charge":{"id":"12a"}})",
             R"({"type":"avertissement_vu","charge":{"id":"18446744073709551616"}})",
         })
    {
        if (Lit(mauvais, m))
        {
            ++g_echecs;
            std::printf("ECHEC : accepte a tort : %s\n", mauvais);
        }
    }
    // Un sujet demesure ne part pas vers le serveur.
    const std::string long_sujet = R"({"type":"abonner","charge":{"sujet":")" + std::string(65, 'a') + R"(","actif":true}})";
    VERIFIER(!Lit(long_sujet.c_str(), m));
}

static void le_json_vers_la_page_est_echappe()
{
    VERIFIER(JsonReponse(7, true, "ok") == R"({"type":"reponse","charge":{"request_id":7,"ok":true,"message":"ok"}})");
    VERIFIER(JsonReponse(8, false, "nom \"x\\y\"\nligne\x01\t") ==
             "{\"type\":\"reponse\",\"charge\":{\"request_id\":8,\"ok\":false,\"message\":\"nom \\\"x\\\\y\\\"\\nligne\\u0001\\t\"}}");
    // La charge de l'evenement est du JSON DANS une chaine : la page le decode elle-meme.
    VERIFIER(JsonEvenement("joueurs", R"({"joueurs":[{"nom":"Fantome"}]})") ==
             R"({"type":"evenement","charge":{"sujet":"joueurs","charge_json":"{\"joueurs\":[{\"nom\":\"Fantome\"}]}"}})");
    VERIFIER(JsonDroits({"admin.moderation.warn", "a\"b"}) ==
             R"({"type":"droits","charge":{"noeuds":["admin.moderation.warn","a\"b"]}})");
    VERIFIER(JsonDroits({}) == R"({"type":"droits","charge":{"noeuds":[]}})");
    VERIFIER(JsonEtatConnexion(true) == R"({"type":"etat_connexion","charge":{"connecte":true}})");
    VERIFIER(JsonEtatConnexion(false) == R"({"type":"etat_connexion","charge":{"connecte":false}})");
    // U+2028 / U+2029 : valides en JSON, fin de ligne en JavaScript d'avant 2019 -- echappes.
    VERIFIER(JsonReponse(1, true, "a\xE2\x80\xA8z").find("\\u2028") != std::string::npos);
}

static void les_abonnements_se_rejouent()
{
    Abonnements a;
    VERIFIER(a.Appliquer("joueurs", true));
    VERIFIER(a.Appliquer("joueurs", true)); // deux fois : une seule entree
    VERIFIER(a.Appliquer("tickets", true));
    VERIFIER(a.Appliquer("tickets", false));
    VERIFIER(a.Appliquer("jamais_vu", false)); // se desabonner de rien part quand meme
    VERIFIER(a.Actifs() == std::vector<std::string>{"joueurs"});
    for (int i = 0; i < 40; ++i)
    {
        a.Appliquer("s" + std::to_string(i), true);
    }
    VERIFIER(a.Actifs().size() == Abonnements::kMax);
    VERIFIER(!a.Appliquer("un_de_trop", true)); // plein : refuse, et l'appelant n'envoie pas
    VERIFIER(a.Appliquer("joueurs", true));     // deja la : accepte meme plein
    a.Vider();
    VERIFIER(a.Actifs().empty());
}

int main()
{
    le_rang_staff_decide_du_chargement();
    les_cinq_types_se_lisent();
    tout_le_reste_est_refuse();
    le_json_vers_la_page_est_echappe();
    les_abonnements_se_rejouent();
    if (g_echecs == 0)
    {
        std::printf("verif_pont_staff : TOUT VERT\n");
        return 0;
    }
    std::printf("verif_pont_staff : %d ECHEC(S)\n", g_echecs);
    return 1;
}

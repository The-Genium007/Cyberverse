// Le relais du pont de l'interface staff, partie PURE (lot H3, ADR 0055 §7). Aucun type moteur,
// aucun reseau, aucun Windows : teste par `tests/verif_pont_staff.cpp`.
//
// La page (CEF) parle en JSON `{type, charge}`, liste FERMEE (`tessera-staff-ui/src/pont.ts`,
// `tessera-staff-hote/src/pont.rs`). L'hote a deja filtre ce qui en sort ; on relit quand meme
// ici, strictement : c'est ce fichier qui fabrique des messages du protocole, et il ne doit pas
// dependre d'un filtre ecrit dans une autre DLL. Le serveur reste la seule autorite (il revalide
// chaque commande) : ceci est une hygiene, pas une garde.
//
// Pourquoi un lecteur JSON maison : le netcode n'embarque aucune bibliotheque JSON, et la forme a
// lire est FIXE (un objet, `type` en chaine, `charge` en objet PLAT de chaines, booleens et
// entiers positifs). Tout le reste est refuse.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Tessera::Staff
{
/// Le joueur a-t-il un rang staff ? C'est LA condition du chargement de l'hote CEF (demarrage
/// paresseux, ADR §8) : un noeud `admin.<quelque chose>` ou le joker racine `*`. Un joueur sans
/// rang ne charge ni `tessera_staff_hote.dll` ni `libcef.dll`.
inline bool RangEstStaff(const std::vector<std::string>& noeuds)
{
    return std::any_of(noeuds.begin(), noeuds.end(),
                       [](const std::string& n) { return n == "*" || n.rfind("admin.", 0) == 0; });
}

enum class TypePage { Inconnu, Commande, Abonner, Souris, Pret, AvertissementVu };

struct MessagePage
{
    TypePage type = TypePage::Inconnu;
    std::uint32_t requestId = 0; // commande : 1..=u32::MAX (0 = la console tapee a la main)
    std::string texte;           // commande
    std::string sujet;           // abonner
    bool actif = false;          // abonner
    bool libre = false;          // souris
    std::uint64_t idAvertissement = 0; // avertissement_vu
};

constexpr std::size_t kSujetMax = 64;

namespace detail
{
struct Valeur
{
    enum Genre { Chaine, Booleen, Entier } genre = Chaine;
    std::string chaine;
    bool booleen = false;
    std::uint64_t entier = 0;
};

struct Lecteur
{
    std::string_view s;
    std::size_t i = 0;

    void Blancs()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }
    bool Prend(char c)
    {
        Blancs();
        if (i < s.size() && s[i] == c)
        {
            ++i;
            return true;
        }
        return false;
    }
    bool Mot(std::string_view mot)
    {
        if (s.substr(i, mot.size()) == mot)
        {
            i += mot.size();
            return true;
        }
        return false;
    }
    bool Hex4(std::uint32_t& v)
    {
        if (i + 4 > s.size())
            return false;
        v = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    static void Utf8(std::uint32_t c, std::string& o)
    {
        if (c < 0x80)
            o += static_cast<char>(c);
        else if (c < 0x800)
        {
            o += static_cast<char>(0xC0 | (c >> 6));
            o += static_cast<char>(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            o += static_cast<char>(0xE0 | (c >> 12));
            o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (c & 0x3F));
        }
        else
        {
            o += static_cast<char>(0xF0 | (c >> 18));
            o += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    // Une chaine JSON. Refuse U+0000 (il couperait la chaine C en aval) et un demi-couple isole.
    bool Chaine(std::string& o)
    {
        Blancs();
        if (i >= s.size() || s[i] != '"')
            return false;
        ++i;
        o.clear();
        while (i < s.size())
        {
            const unsigned char c = static_cast<unsigned char>(s[i++]);
            if (c == '"')
                return true;
            if (c < 0x20)
                return false;
            if (c != '\\')
            {
                o += static_cast<char>(c);
                continue;
            }
            if (i >= s.size())
                return false;
            switch (s[i++])
            {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u':
            {
                std::uint32_t u = 0;
                if (!Hex4(u) || u == 0)
                    return false;
                if (u >= 0xDC00 && u <= 0xDFFF)
                    return false;
                if (u >= 0xD800 && u <= 0xDBFF)
                {
                    std::uint32_t bas = 0;
                    if (!Mot("\\u") || !Hex4(bas) || bas < 0xDC00 || bas > 0xDFFF)
                        return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (bas - 0xDC00);
                }
                Utf8(u, o);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }
    // Entier positif SANS signe, sans fraction ni exposant, sans depassement de u64.
    bool Entier(std::uint64_t& v)
    {
        const std::size_t debut = i;
        v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        {
            const std::uint64_t d = static_cast<std::uint64_t>(s[i] - '0');
            if (v > (UINT64_MAX - d) / 10)
                return false;
            v = v * 10 + d;
            ++i;
        }
        if (i == debut || (i - debut > 1 && s[debut] == '0'))
            return false;
        return !(i < s.size() && (s[i] == '.' || s[i] == 'e' || s[i] == 'E'));
    }
    bool Scalaire(Valeur& v)
    {
        Blancs();
        if (i >= s.size())
            return false;
        if (s[i] == '"')
        {
            v.genre = Valeur::Chaine;
            return Chaine(v.chaine);
        }
        if (Mot("true"))
        {
            v.genre = Valeur::Booleen;
            v.booleen = true;
            return true;
        }
        if (Mot("false"))
        {
            v.genre = Valeur::Booleen;
            v.booleen = false;
            return true;
        }
        v.genre = Valeur::Entier;
        return Entier(v.entier);
    }
    // `{ "nom": scalaire, ... }`, sans doublon.
    bool ObjetPlat(std::vector<std::pair<std::string, Valeur>>& champs)
    {
        if (!Prend('{'))
            return false;
        if (Prend('}'))
            return true;
        do
        {
            std::string nom;
            Valeur v;
            if (!Chaine(nom) || !Prend(':') || !Scalaire(v))
                return false;
            for (const auto& c : champs)
                if (c.first == nom)
                    return false;
            champs.emplace_back(std::move(nom), std::move(v));
        } while (Prend(','));
        return Prend('}');
    }
};

inline const Valeur* Champ(const std::vector<std::pair<std::string, Valeur>>& champs, const char* nom, Valeur::Genre genre)
{
    for (const auto& c : champs)
        if (c.first == nom)
            return c.second.genre == genre ? &c.second : nullptr;
    return nullptr;
}

inline void Echapper(std::string_view s, std::string& o)
{
    o += '"';
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        // U+2028 / U+2029 (E2 80 A8 / A9) : valides en JSON, mais la page recoit ce texte par
        // `ExecuteJavaScript` -- on ne laisse aucune fin de ligne brute.
        if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9))
        {
            o += static_cast<unsigned char>(s[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
            i += 2;
            continue;
        }
        switch (c)
        {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char tampon[8];
                std::snprintf(tampon, sizeof(tampon), "\\u%04x", c);
                o += tampon;
            }
            else
            {
                o += static_cast<char>(c);
            }
        }
    }
    o += '"';
}
} // namespace detail

/// Lit un message de la page. `false` = hors liste fermee : l'appelant le COMPTE et ne relaie rien.
inline bool LireMessagePage(std::string_view json, MessagePage& m)
{
    using detail::Valeur;
    detail::Lecteur l{json};
    std::string type;
    std::vector<std::pair<std::string, Valeur>> charge;
    bool aType = false, aCharge = false;
    if (!l.Prend('{'))
        return false;
    do
    {
        std::string nom;
        if (!l.Chaine(nom) || !l.Prend(':'))
            return false;
        if (nom == "type" && !aType)
        {
            aType = l.Chaine(type);
            if (!aType)
                return false;
        }
        else if (nom == "charge" && !aCharge)
        {
            aCharge = l.ObjetPlat(charge);
            if (!aCharge)
                return false;
        }
        else
        {
            return false;
        }
    } while (l.Prend(','));
    if (!l.Prend('}'))
        return false;
    l.Blancs();
    if (l.i != json.size() || !aType || !aCharge)
        return false;

    const auto chaine = [&](const char* n) { return detail::Champ(charge, n, Valeur::Chaine); };
    const auto booleen = [&](const char* n) { return detail::Champ(charge, n, Valeur::Booleen); };
    if (type == "commande")
    {
        const auto* id = detail::Champ(charge, "request_id", Valeur::Entier);
        const auto* texte = chaine("texte");
        if (charge.size() != 2 || !id || !texte || id->entier == 0 || id->entier > UINT32_MAX || texte->chaine.empty())
            return false;
        m.type = TypePage::Commande;
        m.requestId = static_cast<std::uint32_t>(id->entier);
        m.texte = texte->chaine;
        return true;
    }
    if (type == "abonner")
    {
        const auto* sujet = chaine("sujet");
        const auto* actif = booleen("actif");
        if (charge.size() != 2 || !sujet || !actif || sujet->chaine.empty() || sujet->chaine.size() > kSujetMax)
            return false;
        m.type = TypePage::Abonner;
        m.sujet = sujet->chaine;
        m.actif = actif->booleen;
        return true;
    }
    if (type == "souris")
    {
        const auto* libre = booleen("libre");
        if (charge.size() != 1 || !libre)
            return false;
        m.type = TypePage::Souris;
        m.libre = libre->booleen;
        return true;
    }
    if (type == "pret")
    {
        if (!charge.empty())
            return false;
        m.type = TypePage::Pret;
        return true;
    }
    if (type == "avertissement_vu")
    {
        // L'id est un u64 : il voyage en chaine decimale (il ne tient pas dans un nombre JS).
        const auto* id = chaine("id");
        if (charge.size() != 1 || !id)
            return false;
        detail::Lecteur n{id->chaine};
        std::uint64_t v = 0;
        if (!n.Entier(v) || n.i != id->chaine.size())
            return false;
        m.type = TypePage::AvertissementVu;
        m.idAvertissement = v;
        return true;
    }
    return false;
}

// ── Vers la page : les formes de `ENTRANTS` (pont.ts), a la lettre ───────────────────────────
inline std::string JsonReponse(std::uint32_t requestId, bool ok, std::string_view message)
{
    std::string o = R"({"type":"reponse","charge":{"request_id":)" + std::to_string(requestId) +
                    R"(,"ok":)" + (ok ? "true" : "false") + R"(,"message":)";
    detail::Echapper(message, o);
    return o + "}}";
}

/// `chargeJson` est du JSON que le SERVEUR a ecrit : il reste une CHAINE, la page le decode.
inline std::string JsonEvenement(std::string_view sujet, std::string_view chargeJson)
{
    std::string o = R"({"type":"evenement","charge":{"sujet":)";
    detail::Echapper(sujet, o);
    o += R"(,"charge_json":)";
    detail::Echapper(chargeJson, o);
    return o + "}}";
}

/// `rang` n'est pas ecrit : la page le deduit des noeuds (pont.ts, champ facultatif).
inline std::string JsonDroits(const std::vector<std::string>& noeuds)
{
    std::string o = R"({"type":"droits","charge":{"noeuds":[)";
    for (std::size_t i = 0; i < noeuds.size(); ++i)
    {
        if (i)
            o += ',';
        detail::Echapper(noeuds[i], o);
    }
    return o + "]}}";
}

inline std::string JsonEtatConnexion(bool connecte)
{
    return std::string(R"({"type":"etat_connexion","charge":{"connecte":)") + (connecte ? "true" : "false") + "}}";
}

/// Les sujets auxquels la page est abonnee. Le serveur oublie les abonnements a la deconnexion :
/// le netcode les REJOUE au `PermissionSync` de la connexion suivante, sinon la liste des joueurs
/// se fige apres une coupure sans que la page le sache.
class Abonnements
{
public:
    static constexpr std::size_t kMax = 32;

    /// `false` = registre plein : l'appelant n'envoie PAS (sinon le serveur tiendrait un
    /// abonnement que la reconnexion ne rejouerait pas).
    bool Appliquer(const std::string& sujet, bool actif)
    {
        const auto it = std::find(m_actifs.begin(), m_actifs.end(), sujet);
        if (!actif)
        {
            if (it != m_actifs.end())
                m_actifs.erase(it);
            return true;
        }
        if (it != m_actifs.end())
            return true;
        if (m_actifs.size() >= kMax)
            return false;
        m_actifs.push_back(sujet);
        return true;
    }
    const std::vector<std::string>& Actifs() const { return m_actifs; }
    void Vider() { m_actifs.clear(); }

private:
    std::vector<std::string> m_actifs;
};
} // namespace Tessera::Staff

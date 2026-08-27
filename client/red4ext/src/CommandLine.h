#pragma once
#include <utility>
#include <vector>
#include <optional>
#include <cstring>
#include <string>

inline std::optional<std::string> ArgumentFromCommandLineUntilNextSpace(char* commandLine, const char* needle, const size_t needleLen)
{
    const auto line = std::string(commandLine);
    const auto argumentStart = line.find(needle);
    if (argumentStart == -1)
    {
        return {};
    }

    // We need a manual passing of needleLen as we can't easily constexpr strlen inside here.
    const auto argumentValueStart = argumentStart + needleLen;
    const auto argumentValueEnd = line.find(' ', argumentValueStart);

    return line.substr(argumentValueStart, argumentValueEnd - argumentValueStart);
}

inline std::optional<std::string> ParseHostFromCommandLine(char* commandLine) {
    constexpr auto needle = "--cyberverse-server-address=";
    constexpr auto needleLen = std::char_traits<char>::length(needle);
    return ArgumentFromCommandLineUntilNextSpace(commandLine, needle, needleLen);
}

inline std::optional<uint16_t> ParsePortFromCommandLine(char* commandLine) {
    constexpr auto needle = "--cyberverse-server-port=";
    constexpr auto needleLen = std::char_traits<char>::length(needle);
    const auto portString = ArgumentFromCommandLineUntilNextSpace(commandLine, needle, needleLen);
    if (!portString.has_value())
    {
        return {};
    }

    return std::stoi(portString.value());
}

// MODE DÉVELOPPEMENT — `--tessera-dev` sur la ligne de commande.
//
// Saute le lobby et entre directement dans le monde avec un personnage assigné d'office. Existe
// pour une raison précise : itérer sur le JEU sans repasser par un écran de choix à chaque
// lancement — et surtout pour qu'un agent puisse tester seul, sans un humain pour cliquer.
//
// Absent = parcours normal, lobby obligatoire.
//
// ⚠️ CE DRAPEAU PREND DÉSORMAIS UNE VALEUR OPTIONNELLE (`--tessera-dev=2`), et le commentaire
// d'origine disait l'inverse : « présence/absence, pas de valeur ; un drapeau qui prend un paramètre
// invite à en inventer d'autres ». Le raisonnement était bon en général, et faux ici.
//
// Ce qui l'a renversé, c'est l'usage : sans valeur, le mode dev TIRE UN PERSONNAGE AU HASARD parmi
// ceux du compte. Lucas l'a signalé deux fois — « on sélectionne un des personnages random, il
// faudrait une solution pour activer la sélection de la bonne sauvegarde ». Un tirage au sort rend
// une mesure NON REPRODUCTIBLE : deux lancements identiques peuvent donner deux corps, deux voix,
// deux souches. C'est exactement ce qu'un instrument ne doit pas faire.
inline bool ModeDeveloppementDemande(char* commandLine)
{
    if (commandLine == nullptr) { return false; }
    return std::string(commandLine).find("--tessera-dev") != std::string::npos;
}

// Quel personnage le mode dev doit prendre : `--tessera-dev=2` → 2 (le DEUXIÈME, en base 1).
//
// Rend **0 quand rien n'est demandé** — et `0` veut dire « choisis pour moi », pas « le premier ».
// Le tirage au sort est conservé dans ce cas, et ce n'est pas de la nostalgie : c'est ce qui évite
// que deux instances lancées sans précision se disputent le MÊME personnage, ce que le serveur
// refuse (`already_playing`). Demander explicitement, c'est prendre la responsabilité de ne pas
// lancer deux fois le même numéro.
//
// La base 1 est délibérée : elle correspond à ce qu'un humain lit à l'écran (« le deuxième carré »),
// pas à un index de tableau. La conversion vers l'index réel se fait d'un seul côté, dans le
// redscript, et elle y est commentée.
inline int PersonnageDemande(char* commandLine)
{
    if (commandLine == nullptr) { return 0; }
    const std::string ligne(commandLine);
    const auto pos = ligne.find("--tessera-dev=");
    if (pos == std::string::npos) { return 0; }
    const auto debut = pos + std::strlen("--tessera-dev=");
    int n = 0;
    // Lecture chiffre à chiffre plutôt que `std::stoi` : la ligne de commande n'est pas terminée
    // là où le nombre l'est, et `stoi` sur « 2 --autre-chose » lèverait ou lirait de travers selon
    // la plateforme. Un chiffre non numérique arrête la lecture, ce qui rend `--tessera-dev=abc`
    // équivalent à `--tessera-dev` — dégradation silencieuse mais SÛRE, et journalisée côté script.
    for (auto i = debut; i < ligne.size() && ligne[i] >= '0' && ligne[i] <= '9'; ++i)
    {
        n = n * 10 + (ligne[i] - '0');
        if (n > 999) { return 999; }   // borne : un compte n'a pas mille personnages
    }
    return n;
}

// `--tessera-spawn-enrichi` — allume la voie de spawn ENRICHIE dès le lancement.
//
// ⭐ POURQUOI CE DRAPEAU EXISTE. L'interrupteur n'était atteignable que par la console CET, donc il
// exigeait qu'un humain tape une ligne dans CHAQUE instance, à CHAQUE lancement, et AVANT que les
// joueurs ne se voient — faute de quoi les corps naissaient par la voie sûre et le tir ne mesurait
// rien. C'était le dernier geste humain d'une boucle par ailleurs automatisable (ADR 0033), et il
// tombait au pire moment : celui où l'on est occupé à regarder.
//
// ⚠️ Il reste ÉTEINT PAR DÉFAUT. Ce drapeau ne change pas le garde-fou, il change qui l'actionne.
inline bool SpawnEnrichiDemande(char* commandLine)
{
    if (commandLine == nullptr) { return false; }
    return std::string(commandLine).find("--tessera-spawn-enrichi") != std::string::npos;
}

// `--tessera-charge-minimale` — SONDE. N'injecte qu'UNE SEULE paire d'esthetique au lieu de toutes.
//
// ⭐ CE QU'ELLE TRANCHE. L'avatar d'un joueur porte des options qui ne sont PAS dans sa charge
// (F-PLY-295 : une teinte de cheveux presente dans le blob de l'AUTRE personnage). Le corps est
// donc bati a partir de la charge PLUS autre chose — mais quelle part vient de quoi ?
//
// Avec une charge reduite a une paire, tout ce qui apparait quand meme vient forcement d'ailleurs.
// Et la sonde `[Melange]` liste les composants sans l'oeil de personne : la reponse se lit dans un
// journal, pas dans une impression visuelle.
//
// ⚠️ SONDE, PAS REGLAGE. Elle degrade deliberement l'avatar ; elle n'a rien a faire dans un
// lancement joueur, ni meme dans un test qui ne porte pas sur cette question.
// `--tessera-drapeaux=E8:1,EA:0` — SONDE. Ecrase des octets de la requete de spawn enrichi.
//
// ⭐ Les octets de drapeaux `+0xe8`, `+0xea`, `+0xeb`, `+0xee` ont un effet inconnu (F-PLY-297).
// Les faire varier un par un est la seule facon de le decouvrir, et recompiler entre chaque essai
// coute une minute et une occasion de deployer le mauvais binaire.
//
// Format : une liste de `OFFSET:VALEUR` en hexadecimal pour l'offset, decimal pour la valeur.
// Les offsets hors de la requete (0xF0 octets) sont IGNORES en silence cote appelant — c'est lui
// qui garde, pas ce parseur.
//
// ⚠️ SONDE, PAS REGLAGE : ecrire un octet dont on ignore le sens dans une structure passee a une
// fonction native est exactement le genre de geste qui a fait tomber le jeu le 2026-08-21
// (F-PLY-225). Eteinte par defaut, jamais dans un lancement joueur.
inline std::vector<std::pair<std::size_t, std::uint8_t>> DrapeauxRequete(char* commandLine)
{
    std::vector<std::pair<std::size_t, std::uint8_t>> out;
    if (commandLine == nullptr) { return out; }
    const std::string ligne(commandLine);
    const std::string cle = "--tessera-drapeaux=";
    auto i = ligne.find(cle);
    if (i == std::string::npos) { return out; }
    i += cle.size();
    auto fin = ligne.find_first_of(" \t", i);
    const std::string liste = ligne.substr(i, fin == std::string::npos ? std::string::npos : fin - i);
    std::size_t debut = 0;
    while (debut < liste.size())
    {
        auto virgule = liste.find(',', debut);
        const std::string item = liste.substr(debut, virgule == std::string::npos ? std::string::npos
                                                                                  : virgule - debut);
        const auto deuxpoints = item.find(':');
        if (deuxpoints != std::string::npos)
        {
            try
            {
                const auto offset = static_cast<std::size_t>(std::stoul(item.substr(0, deuxpoints), nullptr, 16));
                const auto valeur = static_cast<std::uint8_t>(std::stoul(item.substr(deuxpoints + 1)));
                out.emplace_back(offset, valeur);
            }
            catch (...)
            {
                // Un item malforme est ignore : une sonde ne doit pas faire tomber le jeu sur une
                // faute de frappe dans un raccourci de lancement.
            }
        }
        if (virgule == std::string::npos) { break; }
        debut = virgule + 1;
    }
    return out;
}

// `--tessera-sans-recolte` — SONDE. Neutralise la recolte sur le V LOCAL le temps de l'appel de
// spawn enrichi, en mettant a zero le COMPTE de chaque groupe (`desc + 0x14`), puis en le
// restaurant.
//
// ⭐ C'est la seule voie qui reste contre la contamination (F-PLY-296) : le spawner ne recoit aucun
// pointeur d'etat (F-PLY-317), donc rien dans la requete ne peut lui dire « prends cet etat-la ».
//
// ⚠️ ECRITURE DANS UNE STRUCTURE NATIVE. Eteinte par defaut, jamais dans un lancement joueur.
inline bool SansRecolteDemandee(char* commandLine)
{
    return commandLine != nullptr && std::string(commandLine).find("--tessera-sans-recolte") != std::string::npos;
}

inline bool ChargeMinimaleDemandee(char* commandLine)
{
    if (commandLine == nullptr) { return false; }
    return std::string(commandLine).find("--tessera-charge-minimale") != std::string::npos;
}

// `--tessera-telemetrie` — écrit un journal JSONL de tout ce qu'on émet et de tout ce qu'on rend
// (`PlayerSync/Telemetrie.h`). Séparé de `--tessera-dev` À DESSEIN : on veut pouvoir mesurer un
// parcours JOUEUR normal, lobby compris, sans la dérogation qui saute l'écran d'entrée. Confondre
// les deux ferait qu'on ne mesurerait jamais que le chemin de développement.
// `--tessera-robot` — cette instance n'envoie plus sa vraie position mais celle d'un SCÉNARIO
// reproductible (`PlayerSync/Robot.h`). Sert de partenaire de mesure : l'autre instance observe et
// journalise, et deux sessions deviennent comparables. Jamais dans un lancement joueur.
// `--tessera-sonde-accroupi` — SONDE T7, jamais un mécanisme de production. Pousse
// `stanceState.state = Crouch` dans le graphe d'animation de CHAQUE avatar distant, en boucle.
// L'observation qui tranche est binaire : l'avatar d'en face est accroupi, ou il ne l'est pas.
inline bool SondeAccroupiDemandee(char* commandLine)
{
    if (commandLine == nullptr) { return false; }
    return std::string(commandLine).find("--tessera-sonde-accroupi") != std::string::npos;
}

inline bool RobotDemande(char* commandLine)
{
    if (commandLine == nullptr) { return false; }
    return std::string(commandLine).find("--tessera-robot") != std::string::npos;
}

inline bool TelemetrieDemandee(char* commandLine)
{
    if (commandLine == nullptr) { return false; }
    return std::string(commandLine).find("--tessera-telemetrie") != std::string::npos;
}

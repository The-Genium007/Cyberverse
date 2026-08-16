#pragma once
#include <optional>
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
// Présence/absence, pas de valeur : un drapeau qui prend un paramètre invite à en inventer d'autres,
// et celui-ci ne doit rester qu'un interrupteur. Absent = parcours normal, lobby obligatoire.
inline bool ModeDeveloppementDemande(char* commandLine)
{
    if (commandLine == nullptr) { return false; }
    return std::string(commandLine).find("--tessera-dev") != std::string::npos;
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

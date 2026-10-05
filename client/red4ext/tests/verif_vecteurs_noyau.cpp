// Confronte le VRAI code livre (Robot.h, CommandLine.h, TamponInterpolation.h) aux vecteurs produits
// par l'implementation de reference Rust `tessera-core/partage` (depot Tessera,
// `vecteurs.json`, sections "robot", "ligne_commande", "tampon" — chantier client-natif-en-rust,
// taches C1.3/C1.4).
//
// Contrairement a verif_horloge.cpp (scenarios recopies), celui-ci LIT le fichier : le tampon
// d'interpolation porte des centaines de nombres, que personne ne recopie a la main sans se
// tromper. Le chemin se passe en argument (les deux depots sont des worktrees separes).
//
//   cl /nologo /std:c++20 /W4 /WX /wd4244 /EHsc /I ..\src verif_vecteurs_noyau.cpp
//   verif_vecteurs_noyau.exe <chemin>\tessera-core\partage\vecteurs.json
//
// Les flottants se comparent en BITS. Sortie 0 = accord, 1 = desaccord, 2 = fichier illisible.

#include <cstdint>

#include "CommandLine.h"
#include "PlayerSync/Robot.h"
#include "PlayerSync/TamponInterpolation.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Tessera::Sync;

namespace
{
int g_echecs = 0;
int g_total = 0;

void Verifie(bool ok, const std::string& libelle)
{
    ++g_total;
    if (!ok)
    {
        std::printf("  ECHEC %s\n", libelle.c_str());
        ++g_echecs;
    }
}

// Lecteur minimal d'un fichier GENERE, objets plats (voir examples/ecrire_vecteurs.rs).
std::string Chaine(const std::string& objet, const char* cle)
{
    const std::string motif = std::string("\"") + cle + "\": \"";
    const auto p = objet.find(motif);
    if (p == std::string::npos) { return {}; }
    const auto debut = p + motif.size();
    return objet.substr(debut, objet.find('"', debut) - debut);
}

long long Entier(const std::string& objet, const char* cle)
{
    const std::string motif = std::string("\"") + cle + "\":";
    const auto p = objet.find(motif);
    return p == std::string::npos ? 0 : std::strtoll(objet.c_str() + p + motif.size(), nullptr, 10);
}

std::vector<std::string> Objets(const std::string& contenu, const char* cle)
{
    std::vector<std::string> out;
    const auto debut = contenu.find(std::string("\"") + cle + "\": [");
    if (debut == std::string::npos) { return out; }
    const auto fin = contenu.find("\n  ]", debut);
    for (auto i = debut; i < fin;)
    {
        const auto o = contenu.find('{', i);
        if (o == std::string::npos || o > fin) { break; }
        const auto f = contenu.find('}', o);
        out.push_back(contenu.substr(o, f - o + 1));
        i = f + 1;
    }
    return out;
}

std::vector<std::string> Coupe(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep)) { out.push_back(item); }
    return out;
}

std::uint64_t U(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }
float F32(const std::string& s)
{
    const auto b = static_cast<std::uint32_t>(U(s));
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
double F64(const std::string& s)
{
    const auto b = U(s);
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
std::uint32_t Bits(float f)
{
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
std::uint64_t Bits(double d)
{
    std::uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}

// Rejeu d'un scenario de tampon — meme format de trace que `rejeu.rs`.
std::string Rejouer(const std::string& ops)
{
    HorlogeRendu h;
    TamponPose t;
    std::vector<std::string> trace;
    for (const auto& op : Coupe(ops, ','))
    {
        const auto c = Coupe(op, ':');
        if (c[0] == "H" || c[0] == "A")
        {
            if (c[0] == "H") { h.ObserverSnapshot(U(c[1])); }
            else { h.Avancer(F64(c[1])); }
            trace.push_back("h:" + std::to_string(Bits(h.TempsRendu())) + ":"
                            + std::to_string(Bits(h.DelaiCourant())) + ":"
                            + std::to_string(Bits(h.Gigue())) + ":"
                            + std::to_string(Bits(h.DepuisDernierSnapshot())));
        }
        else if (c[0] == "P")
        {
            Pose p;
            p.x = F32(c[2]); p.y = F32(c[3]); p.z = F32(c[4]); p.yaw = F32(c[5]);
            p.locomotion = static_cast<std::uint8_t>(U(c[6]));
            p.flags = static_cast<std::uint8_t>(U(c[7]));
            p.moveDir = static_cast<std::uint8_t>(U(c[8]));
            p.sustained = static_cast<std::uint32_t>(U(c[9]));
            p.postureSpot = U(c[10]);
            p.lookYaw = F32(c[11]); p.lookPitch = F32(c[12]);
            p.frameX = F32(c[13]); p.frameY = F32(c[14]); p.frameZ = F32(c[15]);
            p.framePositionValid = U(c[16]) != 0;
            t.Pousser(U(c[1]), p);
            trace.push_back("p:" + std::to_string(t.Nombre()) + ":" + std::to_string(t.Regressions())
                            + ":" + std::to_string(t.DernierTick()));
        }
        else if (c[0] == "S")
        {
            PoseRendue r;
            if (!t.Echantillonner(F64(c[1]), r)) { trace.push_back("s0"); continue; }
            float vx, vy, vz;
            PointDeVisee(r, 5.0f, vx, vy, vz);
            const float fl[] = {r.x, r.y, r.z, r.yaw, r.lookYaw, r.lookPitch, r.frameX, r.frameY,
                                r.frameZ, r.vx, r.vy, r.vz, vx, vy, vz};
            std::string s = "s1:";
            for (const float v : fl) { s += std::to_string(Bits(v)) + ":"; }
            s += std::to_string(r.framePositionValid ? 1 : 0) + ":" + std::to_string(r.locomotion) + ":"
                 + std::to_string(r.flags) + ":" + std::to_string(r.moveDir) + ":"
                 + std::to_string(r.sustained) + ":" + std::to_string(r.postureSpot) + ":"
                 + std::to_string(r.extrapolee ? 1 : 0);
            trace.push_back(s);
        }
    }
    std::string out;
    for (std::size_t i = 0; i < trace.size(); ++i) { out += (i ? "," : "") + trace[i]; }
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage : verif_vecteurs_noyau <vecteurs.json>\n"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { std::printf("fichier introuvable : %s\n", argv[1]); return 2; }
    std::stringstream buf;
    buf << f.rdbuf();
    const std::string doc = buf.str();

    std::printf("-- robot --\n");
    const auto robots = Objets(doc, "robot");
    Verifie(robots.size() >= 200, "assez de vecteurs robot");
    for (const auto& r : robots)
    {
        // t_bits depasse l'intervalle d'un entier signe : lecture non signee.
        const double temps = F64(std::to_string(
            std::strtoull(r.c_str() + r.find("\"t_bits\":") + 9, nullptr, 10)));
        const PoseRobot p = PoseDuRobot(temps);
        const std::string ou = " a t=" + std::to_string(temps);
        Verifie(Bits(p.dx) == static_cast<std::uint32_t>(Entier(r, "dx_bits")), "dx" + ou);
        Verifie(Bits(p.dy) == static_cast<std::uint32_t>(Entier(r, "dy_bits")), "dy" + ou);
        Verifie(Bits(p.dz) == static_cast<std::uint32_t>(Entier(r, "dz_bits")), "dz" + ou);
        Verifie(Bits(p.yaw) == static_cast<std::uint32_t>(Entier(r, "yaw_bits")), "yaw" + ou);
        Verifie(p.locomotion == Entier(r, "loco"), "locomotion" + ou);
        Verifie(p.phase == Entier(r, "phase"), "phase" + ou);
    }

    std::printf("-- ligne_commande --\n");
    const auto lignes = Objets(doc, "ligne_commande");
    Verifie(lignes.size() >= 15, "assez de vecteurs ligne_commande");
    for (const auto& r : lignes)
    {
        const bool nul = Entier(r, "nul") == 1;
        std::string ligne = Chaine(r, "ligne");
        char* l = nul ? nullptr : ligne.data();
        const std::string ou = " [" + ligne + "]";
        Verifie(ModeDeveloppementDemande(l) == (Entier(r, "dev") == 1), "dev" + ou);
        Verifie(PersonnageDemande(l) == Entier(r, "perso"), "personnage" + ou);
        Verifie(SpawnEnrichiDemande(l) == (Entier(r, "spawn") == 1), "spawn" + ou);
        Verifie(SansSpawnEnrichiDemande(l) == (Entier(r, "sans_spawn") == 1), "sans_spawn" + ou);
        Verifie(ChargeExhaustiveDemandee(l) == (Entier(r, "exhaustive") == 1), "exhaustive" + ou);
        Verifie(ChargeMinimaleDemandee(l) == (Entier(r, "minimale") == 1), "minimale" + ou);
        Verifie(SansRecolteDemandee(l) == (Entier(r, "sans_recolte") == 1), "sans_recolte" + ou);
        Verifie(AlignerSectionsDemande(l) == (Entier(r, "aligner") == 1), "aligner" + ou);
        Verifie(EffacerResiduDemande(l) == (Entier(r, "effacer") == 1), "effacer" + ou);
        Verifie(SondeAccroupiDemandee(l) == (Entier(r, "accroupi") == 1), "accroupi" + ou);
        Verifie(RobotDemande(l) == (Entier(r, "robot") == 1), "robot" + ou);
        Verifie(TelemetrieDemandee(l) == (Entier(r, "telemetrie") == 1), "telemetrie" + ou);
        std::string drap;
        for (const auto& [off, val] : DrapeauxRequete(l))
        {
            drap += (drap.empty() ? "" : ";") + std::to_string(off) + ":" + std::to_string(val);
        }
        Verifie(drap == Chaine(r, "drapeaux"), "drapeaux" + ou + " -> " + drap);
        if (!nul)
        {
            const auto hote = ParseHostFromCommandLine(l);
            Verifie(hote.has_value() == (Entier(r, "hote_present") == 1), "hote present" + ou);
            Verifie(hote.value_or("") == Chaine(r, "hote"), "hote" + ou);
            long long port = -1;
            try
            {
                const auto p = ParsePortFromCommandLine(l);
                port = p.has_value() ? static_cast<long long>(p.value()) : -1;
            }
            catch (...)
            {
                port = -2; // std::stoi leve : le client tomberait
            }
            Verifie(port == Entier(r, "port"), "port" + ou + " -> " + std::to_string(port));
        }
    }

    std::printf("-- tampon --\n");
    const auto scenarios = Objets(doc, "tampon");
    Verifie(scenarios.size() >= 7, "assez de scenarios tampon");
    for (std::size_t i = 0; i < scenarios.size(); ++i)
    {
        const std::string attendu = Chaine(scenarios[i], "trace");
        const std::string obtenu = Rejouer(Chaine(scenarios[i], "ops"));
        Verifie(obtenu == attendu, "scenario " + std::to_string(i) + " : trace identique");
        if (obtenu != attendu)
        {
            const auto a = Coupe(attendu, ',');
            const auto b = Coupe(obtenu, ',');
            for (std::size_t k = 0; k < a.size() && k < b.size(); ++k)
            {
                if (a[k] != b[k])
                {
                    std::printf("       1ere divergence a l'operation %zu\n  attendu %s\n  obtenu  %s\n", k,
                                a[k].c_str(), b[k].c_str());
                    break;
                }
            }
        }
    }

    std::printf("\n%d verifications, %d echec(s)\n", g_total, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

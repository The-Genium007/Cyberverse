// Vérification de HorlogeServeur (le VRAI header, pas un miroir) contre les vecteurs produits par
// l'implémentation de référence Rust `tessera-core/partage/src/horloge.rs` (dépôt Tessera,
// `tessera-core/partage/vecteurs.json`, section "horloge").
//
// Contrairement à `quant`/`esthetique` (portés en miroir C++ dans le dépôt Tessera,
// `tools/client-noyau-partage-cpp/` — voir l'arbitrage de
// `docs/chantiers/client-natif-en-rust.md` dans ce dépôt) : `HorlogeServeur.h` n'a AUCUNE
// dépendance au SDK RED4ext (« ZÉRO DÉPENDANCE AU MOTEUR », son propre en-tête le dit), donc rien
// n'empêche de tester ICI le code RÉELLEMENT LIVRÉ, pas une transcription. C'est plus fort que la
// preuve `quant`/`esthetique` : une divergence trouvée ici serait une vraie régression du client,
// pas un défaut de miroir.
//
// Les scénarios ci-dessous sont recopiés depuis
// Tessera/tessera-core/partage/vecteurs.json (section "horloge") au moment du portage
// (2026-09-29) — PAS lus dynamiquement : les deux dépôts sont des worktrees Git séparés, et une
// lecture inter-dépôt par chemin relatif serait fragile hors de la machine de Lucas (même choix
// que verif_robot.cpp/verif_tampon_interpolation.cpp, qui embarquent déjà leurs propres attendus).
// Si `horloge.rs` change, ces nombres doivent être regénérés à la main
// (`cargo run -p tessera-partage --example ecrire_vecteurs` côté Tessera) et recopiés ici — sinon
// ce test et les tests Rust divergent en silence, exactement le risque documenté en tête de
// `tools/population-derivee-cpp/population_derivee.hpp`.
//
// Compilation :
//   cl /nologo /std:c++20 /W4 /WX /EHsc /I ..\src verif_horloge.cpp

#include "PlayerSync/HorlogeServeur.h"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace
{
int g_echecs = 0;
int g_total = 0;

void Verifie(bool condition, const std::string& libelle)
{
    ++g_total;
    if (condition)
    {
        std::printf("  ok   %s\n", libelle.c_str());
    }
    else
    {
        std::printf("  ECHEC %s\n", libelle.c_str());
        ++g_echecs;
    }
}

struct Observation
{
    std::uint64_t ts;
    std::uint64_t recu;
};

struct Scenario
{
    const char* nom;
    std::vector<Observation> obs;
    std::int64_t decalageAttendu;
    std::int64_t etalementAttendu;
    std::size_t echantillonsAttendus;
    std::uint64_t observationsAttendues;
    std::uint64_t requeteMs;
    std::uint64_t tempsServeurAttendu;
};

std::vector<Observation> Repete(std::uint64_t ts, std::uint64_t recu, int n)
{
    std::vector<Observation> v;
    v.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        v.push_back({ts, recu});
    }
    return v;
}

std::vector<Observation> Concatene(std::vector<Observation> a, const std::vector<Observation>& b,
                                   const std::vector<Observation>& c)
{
    a.insert(a.end(), b.begin(), b.end());
    a.insert(a.end(), c.begin(), c.end());
    return a;
}
} // namespace

int main()
{
    using Tessera::Sync::HorlogeServeur;

    // ── Recopiés depuis Tessera/tessera-core/partage/vecteurs.json ("horloge"), 2026-09-29 ──
    const std::vector<Scenario> scenarios = {
        {"jamais amorcee", {}, 0, 0, 0, 0, 12345, 12345},
        {"ts=0 ignore", {{0, 999999}}, 0, 0, 0, 0, 5000, 5000},
        {"minimum de la fenetre",
         {{1000, 1050}, {1000, 1010}, {1000, 1030}},
         10, 40, 3, 3, 5000, 4990},
        {"horloge locale en retard, delta negatif", {{10000, 9500}}, -500, 0, 1, 1, 9500, 10000},
        {"temps_serveur_ms sature a 0", {{100, 1000}}, 900, 0, 1, 1, 500, 0},
        {"fenetre renouvelee : le minimum suit, ne reste pas colle",
         Concatene(Repete(1000, 1100, 128), Repete(1000, 1005, 128), Repete(1000, 1200, 128)),
         200, 0, 128, 384, 10000, 9800},
    };

    for (const auto& sc : scenarios)
    {
        std::printf("-- %s --\n", sc.nom);
        HorlogeServeur h;
        for (const auto& o : sc.obs)
        {
            h.Observer(o.ts, o.recu);
        }
        std::ostringstream ctx;
        ctx << " (" << sc.nom << ")";
        Verifie(h.DecalageMs() == sc.decalageAttendu, "decalage_ms" + ctx.str());
        Verifie(h.EtalementMs() == sc.etalementAttendu, "etalement_ms" + ctx.str());
        Verifie(h.Echantillons() == sc.echantillonsAttendus, "echantillons" + ctx.str());
        Verifie(h.Observations() == sc.observationsAttendues, "observations" + ctx.str());
        Verifie(h.TempsServeurMs(sc.requeteMs) == sc.tempsServeurAttendu, "temps_serveur_ms" + ctx.str());
    }

    std::printf("\n%d/%d verifications reussies.\n", g_total - g_echecs, g_total);
    if (g_echecs != 0)
    {
        std::printf("ECHEC — HorlogeServeur.h diverge du portage Rust (tessera-partage::horloge).\n");
        return 1;
    }
    std::printf("OK — HorlogeServeur.h (code REEL, pas un miroir) s'accorde avec tessera-partage::horloge au bit pres.\n");
    return 0;
}

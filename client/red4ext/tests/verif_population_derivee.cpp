// Confronte le portage C++ au MÊME fichier de vecteurs que le test Rust.
//
//   cl /std:c++17 /EHsc /W4 test_vecteurs.cpp && test_vecteurs.exe ..\..\tessera-core\server\population-derivee-vecteurs.json
//   (ou : build.cmd)
//
// Si ce test et `cargo test -p server --lib population_derivee` passent tous les deux, les deux
// implémentations s'accordent — vérifié **sans lancer le jeu**, ce qui est tout l'intérêt.
//
// ⚠️ Les comparaisons portent sur les BITS du flottant, jamais sur sa valeur décimale : deux
// implémentations peuvent imprimer `0.42` et différer du dernier bit, et ce dernier bit suffit à
// faire basculer un tirage pondéré d'une entrée à l'autre.

#include "../src/PopulationDerivee.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace tessera::population;

namespace {

// Lecteur minimal, suffisant pour un fichier GÉNÉRÉ dont la forme est fixe. Pas de dépendance
// JSON : elle ne paierait pas sa complexité pour sept objets plats.
// Rend false si la clé n'est pas trouvée — un vecteur incomplet doit faire échouer le test, jamais
// passer silencieusement.
bool lire_entier(const std::string& objet, const char* cle, unsigned long long& sortie) {
    const std::string motif = std::string("\"") + cle + "\":";
    const std::size_t p = objet.find(motif);
    if (p == std::string::npos) {
        return false;
    }
    sortie = std::strtoull(objet.c_str() + p + motif.size(), nullptr, 10);
    return true;
}

int echecs = 0;

void verifier(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "ECHEC : %s\n", message.c_str());
        ++echecs;
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* chemin = (argc > 1) ? argv[1]
                                    : "../../tessera-core/server/population-derivee-vecteurs.json";
    std::ifstream f(chemin);
    if (!f) {
        std::fprintf(stderr, "ECHEC : fichier de vecteurs introuvable : %s\n", chemin);
        return 2; // distinct de 1 : un fichier absent n'est pas un désaccord de valeurs
    }
    std::stringstream tampon;
    tampon << f.rdbuf();
    const std::string contenu = tampon.str();

    // Découpe sur les objets `{...}` d'une section `"nom": [ ... ]` — le fichier en porte
    // quatre depuis le 2026-09-29, et un balayage depuis `"vecteurs"` jusqu'à la fin les
    // mélangerait toutes.
    const auto objets_de = [&contenu](const char* nom) {
        std::vector<std::string> objets;
        const std::size_t debut = contenu.find(std::string("\"") + nom + "\"");
        if (debut == std::string::npos) {
            return objets;
        }
        const std::size_t fin = contenu.find(']', debut);
        for (std::size_t i = debut; i < fin;) {
            const std::size_t ouvre = contenu.find('{', i);
            if (ouvre == std::string::npos || ouvre > fin) {
                break;
            }
            const std::size_t ferme = contenu.find('}', ouvre);
            if (ferme == std::string::npos) {
                break;
            }
            objets.push_back(contenu.substr(ouvre, ferme - ouvre + 1));
            i = ferme + 1;
        }
        return objets;
    };
    const std::vector<std::string> objets = objets_de("vecteurs");
    verifier(!objets.empty(), "clé `vecteurs` absente du fichier");
    if (objets.empty()) {
        return 2;
    }

    verifier(objets.size() >= 5,
             "trop peu de vecteurs pour verrouiller quoi que ce soit : " + std::to_string(objets.size()));

    int verifies = 0;
    for (const std::string& o : objets) {
        unsigned long long graine = 0, creneau = 0, rang = 0, attendu_cle = 0;
        const bool complet = lire_entier(o, "graine", graine) &&
                             lire_entier(o, "cle_creneau", creneau) && lire_entier(o, "rang", rang) &&
                             lire_entier(o, "cle_individu", attendu_cle);
        verifier(complet, "vecteur incomplet : " + o);
        if (!complet) {
            continue;
        }

        const std::uint64_t cle = cle_individu(graine, static_cast<std::uint32_t>(creneau),
                                               static_cast<std::uint32_t>(rang));
        verifier(cle == attendu_cle, "clé d'individu divergente pour (graine " +
                                         std::to_string(graine) + ", créneau " +
                                         std::to_string(creneau) + ", rang " + std::to_string(rang) +
                                         ") : " + std::to_string(cle) + " au lieu de " +
                                         std::to_string(attendu_cle));

        const struct {
            const char* champ;
            Selecteur sel;
        } usages[] = {
            {"f_abscisse", Selecteur::Abscisse},
            {"f_archetype", Selecteur::Archetype},
            {"f_apparence", Selecteur::Apparence},
            {"f_vitesse", Selecteur::Vitesse},
            {"f_delai_a", Selecteur::DelaiConduiteA},
            {"f_delai_b", Selecteur::DelaiConduiteB},
            {"f_conduite", Selecteur::Conduite},
        };
        // Lots 3 à 6 : les prolongements et la roue normalisée, sur la même clé.
        for (std::uint32_t n = 0; n < 2; ++n) {
            unsigned long long attendu_bits = 0;
            const std::string champ = "f_trajet_" + std::to_string(n);
            if (!lire_entier(o, champ.c_str(), attendu_bits)) {
                verifier(false, "champ absent : " + champ);
                continue;
            }
            const float valeur = fraction(cle_prolongement(cle, n), Selecteur::Trajet);
            std::uint32_t bits = 0;
            std::memcpy(&bits, &valeur, 4);
            verifier(bits == attendu_bits, champ + " divergent pour la clé " + std::to_string(cle));
        }
        {
            unsigned long long attendu = 0;
            verifier(lire_entier(o, "trajet_3_sur_4", attendu), "champ absent : trajet_3_sur_4");
            const float scores[] = {1.0f, 2.0f, 0.5f, 0.0f};
            const int i = tirage_pondere_normalise(scores, 4, cle, Selecteur::Trajet);
            verifier(i >= 0 && static_cast<unsigned long long>(i) == attendu,
                     "roue normalisée divergente pour la clé " + std::to_string(cle) + " : " +
                         std::to_string(i) + " au lieu de " + std::to_string(attendu));
        }
        for (const auto& u : usages) {
            unsigned long long attendu_bits = 0;
            if (!lire_entier(o, u.champ, attendu_bits)) {
                verifier(false, std::string("champ absent : ") + u.champ);
                continue;
            }
            const float valeur = fraction(cle, u.sel);
            std::uint32_t bits = 0;
            std::memcpy(&bits, &valeur, 4);
            verifier(bits == attendu_bits,
                     std::string(u.champ) + " divergent pour (graine " + std::to_string(graine) +
                         ", créneau " + std::to_string(creneau) + ", rang " + std::to_string(rang) +
                         ") : " + std::to_string(bits) + " au lieu de " + std::to_string(attendu_bits));
        }
        ++verifies;
    }

    // Les propriétés que le Rust vérifie aussi, rejouées ici : le portage doit les tenir seul.
    {
        const float poids[] = {0.0f, 0.5f, 0.0f, 0.5f};
        int vus[4] = {0, 0, 0, 0};
        for (std::uint32_t r = 0; r < 2000; ++r) {
            const int i = tirage_pondere(poids, 4, cle_individu(3, 3, r), Selecteur::Archetype);
            verifier(i >= 0 && i < 4, "index hors bornes rendu par la roue");
            if (i >= 0 && i < 4) {
                ++vus[i];
            }
        }
        verifier(vus[0] == 0 && vus[2] == 0, "une entrée de poids nul est sortie");
        verifier(vus[1] > 0 && vus[3] > 0, "une entrée de poids non nul n'est jamais sortie");
        verifier(tirage_pondere(poids, 0, 1, Selecteur::Archetype) == -1, "liste vide mal gérée");
    }
    {
        for (std::uint32_t r = 0; r < 200; ++r) {
            const float s = abscisse_naissance(12.5f, 12.5f, cle_individu(6, 6, r));
            verifier(s == 12.5f, "une fenêtre écrasée ne rend pas une valeur unique");
        }
    }

    {
        // L'aller-retour metres -> point fixe -> metres doit tenir bien en dessous du plancher de
        // re-indexation : sinon la conversion elle-meme produirait des corrections fantomes.
        const float cas[] = {0.0f, 1.0f, -1.0f, 12.5f, 45.4f, -1430.0f, 1262.0f, 0.0001f};
        for (float m : cas) {
            const float retour = point_fixe_vers_metres(metres_vers_point_fixe(m));
            const float ecart = retour > m ? retour - m : m - retour;
            verifier(ecart < 1e-4f, "aller-retour point fixe imprecis pour " + std::to_string(m));
        }
        // Le facteur, verifie par une valeur que l'on peut poser a la main : 1 m = 131072 unites.
        verifier(metres_vers_point_fixe(1.0f) == 131072, "facteur de point fixe faux");
        verifier(metres_vers_point_fixe(-1.0f) == -131072, "point fixe negatif faux");
    }

    // ── Lots 3 à 6 : statiques, places garées, émetteurs (2026-09-29) ───────────────────────
    const auto bits_de = [](float v) {
        std::uint32_t b = 0;
        std::memcpy(&b, &v, 4);
        return static_cast<unsigned long long>(b);
    };
    const auto flottant_de = [](unsigned long long bits) {
        const std::uint32_t b = static_cast<std::uint32_t>(bits);
        float f = 0.0f;
        std::memcpy(&f, &b, 4);
        return f;
    };
    {
        const std::vector<std::string> statiques = objets_de("statiques");
        verifier(statiques.size() >= 3, "section `statiques` absente ou trop courte");
        for (const std::string& o : statiques) {
            unsigned long long graine = 0, entity_id = 0, attendu_cle = 0, attendu_f = 0;
            const bool complet = lire_entier(o, "graine", graine) && lire_entier(o, "entity_id", entity_id) &&
                                 lire_entier(o, "cle", attendu_cle) && lire_entier(o, "f_apparence", attendu_f);
            verifier(complet, "statique incomplet : " + o);
            if (!complet) {
                continue;
            }
            const std::uint64_t cle = cle_statique(graine, entity_id);
            verifier(cle == attendu_cle, "clé statique divergente pour l'entité " + std::to_string(entity_id));
            verifier(bits_de(fraction(cle, Selecteur::Apparence)) == attendu_f,
                     "apparence statique divergente pour l'entité " + std::to_string(entity_id));
            ++verifies;
        }
    }
    {
        const std::vector<std::string> garees = objets_de("places_garees");
        verifier(garees.size() >= 3, "section `places_garees` absente ou trop courte");
        for (const std::string& o : garees) {
            unsigned long long graine = 0, place = 0, t_ms = 0, cycle = 0, attendu_cle = 0, attendu_f = 0;
            const bool complet = lire_entier(o, "graine", graine) && lire_entier(o, "place", place) &&
                                 lire_entier(o, "t_ms", t_ms) && lire_entier(o, "cycle", cycle) &&
                                 lire_entier(o, "cle", attendu_cle) && lire_entier(o, "f_presence", attendu_f);
            verifier(complet, "place garée incomplète : " + o);
            if (!complet) {
                continue;
            }
            verifier(cycle_garee(t_ms) == cycle, "cycle garé divergent à t = " + std::to_string(t_ms));
            const std::uint64_t cle = cle_place_garee(graine, place, cycle);
            verifier(cle == attendu_cle, "clé de place divergente pour la place " + std::to_string(place));
            verifier(bits_de(fraction(cle, Selecteur::PresenceGaree)) == attendu_f,
                     "présence garée divergente pour la place " + std::to_string(place));
            ++verifies;
        }
    }
    {
        const std::vector<std::string> emetteurs = objets_de("emetteurs");
        verifier(emetteurs.size() >= 5, "section `emetteurs` absente ou trop courte");
        bool un_cas_sans_candidat = false;
        for (const std::string& o : emetteurs) {
            unsigned long long graine = 0, voie = 0, x1 = 0, x2 = 0, n = 0, vmin = 0, vmax = 0, t_ms = 0,
                               s0 = 0, s1 = 0, periode = 0;
            const bool complet = lire_entier(o, "graine", graine) && lire_entier(o, "voie", voie) &&
                                 lire_entier(o, "x1_bits", x1) && lire_entier(o, "x2_bits", x2) &&
                                 lire_entier(o, "n_bits", n) && lire_entier(o, "v_min_bits", vmin) &&
                                 lire_entier(o, "v_max_bits", vmax) && lire_entier(o, "t_ms", t_ms) &&
                                 lire_entier(o, "s0_bits", s0) && lire_entier(o, "s1_bits", s1) &&
                                 lire_entier(o, "periode_ms", periode);
            verifier(complet, "émetteur incomplet : " + o);
            if (!complet) {
                continue;
            }
            const Emetteur e{voie, flottant_de(x1), flottant_de(x2), flottant_de(n), flottant_de(vmin),
                             flottant_de(vmax)};
            std::uint64_t p = 0;
            verifier(periode_ms(e, p) && p == periode,
                     "période divergente pour la voie " + std::to_string(voie) + " : " + std::to_string(p) +
                         " au lieu de " + std::to_string(periode));
            const bool attendu_trouve = o.find("\"trouve\": true") != std::string::npos;
            Individu choix{};
            float abscisse = 0.0f;
            const bool trouve = choisir(e, graine, t_ms, flottant_de(s0), flottant_de(s1), nullptr, 0, choix, abscisse);
            verifier(trouve == attendu_trouve, "candidat trouvé/absent divergent : " + o);
            if (!attendu_trouve) {
                un_cas_sans_candidat = true;
            }
            if (trouve && attendu_trouve) {
                unsigned long long k = 0, cle = 0, vit = 0, abs = 0;
                const bool ok = lire_entier(o, "k", k) && lire_entier(o, "cle", cle) &&
                                lire_entier(o, "vitesse_bits", vit) && lire_entier(o, "abscisse_bits", abs);
                verifier(ok, "résultat d'émetteur incomplet : " + o);
                verifier(choix.k == k, "k divergent pour la voie " + std::to_string(voie));
                verifier(choix.cle == cle, "clé d'émetteur divergente pour la voie " + std::to_string(voie));
                verifier(bits_de(choix.vitesse_m_s) == vit, "vitesse divergente pour la voie " + std::to_string(voie));
                verifier(bits_de(abscisse) == abs, "abscisse divergente pour la voie " + std::to_string(voie));
            }
            ++verifies;
        }
        verifier(un_cas_sans_candidat, "le cas « aucun candidat » manque dans `emetteurs`");
    }

    if (echecs == 0) {
        std::printf("OK — %d vecteurs vérifiés, et les propriétés locales tiennent.\n", verifies);
        return 0;
    }
    std::fprintf(stderr, "%d ECHEC(S)\n", echecs);
    return 1;
}

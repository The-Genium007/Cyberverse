// =====================================================================================
// Auto-verification de la LOGIQUE PURE des hooks de foule derivee (monde partage, A' a H).
// Meme forme que `verif_identite_statique.cpp` : aucun framework, aucun type moteur.
//
//   client\red4ext\tests\verif_foule.cmd      (compile et lance ce test ET les 19 vecteurs)
//
// Ce que ce test prouve : la logique. Ce qu'il ne prouve PAS : qu'un seul detour s'attache, ni
// qu'il change quoi que ce soit en jeu (NON MESURE — bloc 4 ter de la campagne).
// =====================================================================================

#include "../src/FouleDerivee.hpp"

#include <cmath>
#include <cstdio>

using namespace tessera::foule;
namespace pop = tessera::population;

static int g_echecs = 0;
static int g_verifs = 0;

static void Verifier(bool condition, const char* quoi)
{
    ++g_verifs;
    if (!condition)
    {
        ++g_echecs;
        std::printf("  ECHEC : %s\n", quoi);
    }
}

int main()
{
    // ── 1. Imposer un tirage au generateur du moteur ────────────────────────────────────
    {
        bool tout = true;
        std::uint64_t etat = 0x0123456789abcdefULL;
        for (std::uint32_t i = 0; i < 200000; ++i)
        {
            etat = pcg_suivant(etat);
            const std::uint32_t voulu = static_cast<std::uint32_t>(pop::melanger(i) & 0x7fffffffu);
            if (pcg_sortie(pcg_etat_imposant(voulu, etat)) != voulu)
            {
                tout = false;
                break;
            }
        }
        Verifier(tout, "pcg_etat_imposant : la sortie du moteur vaut le nombre voulu (200 000 cas)");
        Verifier(pcg_sortie(pcg_etat_imposant(0, 0)) == 0, "imposer 0");
        Verifier(pcg_sortie(pcg_etat_imposant(0x7fffffffu, ~0ULL)) == 0x7fffffffu, "imposer 2^31-1");

        // La fraction imposee est relue AU BIT par la construction de flottant du moteur.
        bool fractions = true;
        for (std::uint64_t cle = 1; cle < 5000; ++cle)
        {
            const float f = pop::fraction(pop::melanger(cle), pop::Selecteur::PresenceGaree);
            const float relu = pcg_fraction(pcg_etat_imposant(mantisse_de(f), cle * 977));
            if (std::memcmp(&f, &relu, 4) != 0)
            {
                fractions = false;
                break;
            }
        }
        Verifier(fractions, "fraction imposee puis relue par le moteur : memes bits (5 000 cles)");

        // Les 27 bits bas de l'etat d'origine survivent (le flux d'apres reste disperse).
        Verifier((pcg_etat_imposant(42, 0x7ffffffULL) & 0x7ffffffULL) == 0x7ffffffULL,
                 "les 27 bits bas de l'etat d'origine sont conserves");
        // Temoin : la sortie n'est PAS la rotation canonique (F-PNJ-251).
        const std::uint64_t s = 0x9000000000000000ULL | (0xdeadbeefULL << 27);
        const std::uint32_t x = static_cast<std::uint32_t>(s >> 45) ^ static_cast<std::uint32_t>(s >> 27);
        const std::uint32_t r = static_cast<std::uint32_t>(s >> 59);
        Verifier(pcg_sortie(s) != ((x >> r) | (x << ((32u - r) & 31u))),
                 "la sortie du moteur differe de la rotation canonique (decalage 31 - r)");
    }

    // ── 2. Hors cle sans contexte ───────────────────────────────────────────────────────
    {
        TableMembres table;
        Contexte ctx;
        std::uint64_t cle = 0xdead;
        Verifier(resoudre_cle(ctx, table, cle) == Source::Aucune, "sans contexte : aucune cle");
        Verifier(cle == 0xdead, "sans contexte : la sortie n'est pas touchee");

        ctx.membre = 0x1000;
        Verifier(resoudre_cle(ctx, table, cle) == Source::Aucune,
                 "membre inconnu de la table : aucune cle");

        Fiche f;
        f.cle = 77;
        f.emetteur = 5;
        table.poser(0x1000, f);
        Verifier(resoudre_cle(ctx, table, cle) == Source::Membre && cle == 77,
                 "membre connu : sa cle");

        ctx.naissance = true;
        ctx.a_cle = true;
        ctx.cle = 99;
        Verifier(resoudre_cle(ctx, table, cle) == Source::Naissance && cle == 99,
                 "la naissance en cours l'emporte sur le membre");

        ctx.a_cle = false; // naissance « hors cle »
        ctx.membre = 0;
        Verifier(resoudre_cle(ctx, table, cle) == Source::Aucune,
                 "naissance hors cle : aucune cle, meme si la table n'est pas vide");

        ctx.naissance = false;
        ctx.garee = true;
        ctx.cle_garee = 1234;
        Verifier(resoudre_cle(ctx, table, cle) == Source::Garee && cle == 1234,
                 "porte des garees : la cle de la place");
    }

    // ── 3. Table videe au despawn, tentatives, eviction ─────────────────────────────────
    {
        TableMembres table;
        Fiche f;
        f.cle = 10;
        f.emetteur = 1;
        table.poser(0xA, f);
        f.cle = 11;
        table.poser(0xB, f);
        f.cle = 12;
        f.emetteur = 2;
        table.poser(0xC, f);

        std::vector<std::uint64_t> vivants;
        table.cles_de_l_emetteur(1, vivants);
        Verifier(vivants.size() == 2, "deux vivants sur l'emetteur 1");

        std::uint64_t cle = 0;
        std::uint32_t n = 99;
        Verifier(table.tentative_suivante(0xA, cle, n) && cle == 10 && n == 0, "1re tentative : 0");
        Verifier(table.tentative_suivante(0xA, cle, n) && n == 1, "2e tentative : 1");
        Verifier(!table.tentative_suivante(0xDEAD, cle, n), "membre inconnu : pas de tentative");

        Verifier(table.retirer(0xA), "despawn : la fiche sort");
        Verifier(!table.retirer(0xA), "despawn deux fois : la seconde ne retire rien");
        Fiche lue;
        Verifier(!table.trouver(0xA, lue), "apres despawn : introuvable");
        vivants.clear();
        table.cles_de_l_emetteur(1, vivants);
        Verifier(vivants.size() == 1 && vivants[0] == 11, "apres despawn : un seul vivant reste");

        // Adresse recyclee : une nouvelle naissance a la meme adresse repart de zero.
        f.cle = 500;
        table.poser(0xA, f);
        Verifier(table.tentative_suivante(0xA, cle, n) && cle == 500 && n == 0,
                 "adresse recyclee : nouvelle cle, tentatives remises a zero");

        // Les tentatives de trajet suivent le contexte : naissance d'abord, table ensuite.
        Contexte ctx;
        Verifier(!tentative_trajet(ctx, table, cle, n), "trajet sans contexte : hors cle");
        ctx.membre = 0xB;
        Verifier(tentative_trajet(ctx, table, cle, n) && cle == 11 && n == 0, "trajet d'un membre : sa fiche");
        ctx.naissance = true;
        Verifier(!tentative_trajet(ctx, table, cle, n),
                 "trajet pendant une naissance HORS CLE : hors cle, pas la fiche d'un autre");
        ctx.a_cle = true;
        ctx.cle = 900;
        Verifier(tentative_trajet(ctx, table, cle, n) && cle == 900 && n == 0 &&
                     tentative_trajet(ctx, table, cle, n) && n == 1 && ctx.prolongements == 2,
                 "trajet pendant une naissance : compte dans le contexte, par tentative");

        // Borne : 4096, la plus ancienne sort.
        TableMembres pleine;
        for (std::uint64_t i = 1; i <= TableMembres::CAPACITE + 3; ++i)
        {
            Fiche g;
            g.cle = i;
            pleine.poser(i, g);
        }
        Verifier(pleine.taille() == TableMembres::CAPACITE, "la table ne depasse pas 4096");
        Verifier(!pleine.trouver(1, lue) && !pleine.trouver(3, lue) && pleine.trouver(4, lue),
                 "ce sont les trois plus anciennes qui sont sorties");
        Verifier(pleine.evictions() == 3, "trois evictions comptees");
    }

    // ── 4. Les decisions : derivee = la reference, controle positif = valeur absurde ────
    {
        const float poids[5] = {0.5f, 0.2f, 0.0f, 0.2f, 0.1f};
        bool egal = true, controle = true;
        int autre_que_zero = 0;
        for (std::uint64_t cle = 1; cle <= 2000; ++cle)
        {
            const int d = decider_archetype(poids, 5, cle, 0);
            egal = egal && d == pop::tirage_pondere(poids, 5, cle, pop::Selecteur::Archetype);
            controle = controle && decider_archetype(poids, 5, cle, 'B') == 0;
            autre_que_zero += d != 0;
        }
        Verifier(egal, "B : la decision est tirage_pondere(…, Archetype)");
        Verifier(controle, "B controle : indice 0, toujours");
        Verifier(autre_que_zero > 500, "B : le controle n'est pas vide de sens (la derivee sort autre chose)");
        Verifier(decider_archetype(poids, 0, 1, 0) == -1, "B : liste vide → -1 (vanilla)");

        bool app = true;
        for (std::uint64_t cle = 1; cle <= 3000; ++cle)
        {
            const std::uint32_t s = sortie_apparence(cle, 0);
            for (std::size_t n = 1; n <= 22; ++n)
            {
                app = app && static_cast<int>(s % n) == pop::index_uniforme(n, cle, pop::Selecteur::Apparence);
            }
            app = app && s < 0x80000000u;
        }
        Verifier(app, "C : sortie imposee % N == index_uniforme(N) pour tout N <= 22");
        Verifier(sortie_apparence(123456, 'C') == 0, "C controle : apparence 0");

        Verifier(decider_vitesse(0.8f, 1.6f, 42, 'D') == 1.6f, "D controle : vitesse max");
        Verifier(decider_vitesse(0.8f, 1.6f, 42, 0) == pop::vitesse(0.8f, 1.6f, 42),
                 "D : la decision est vitesse(min, max, cle)");

        const float scores[4] = {1.0f, 3.0f, 0.5f, 2.0f};
        bool trajet = true;
        int differents = 0;
        for (std::uint64_t cle = 1; cle <= 2000; ++cle)
        {
            const int t0 = decider_trajet(scores, 4, cle, 0, 0);
            const int t1 = decider_trajet(scores, 4, cle, 1, 0);
            trajet = trajet && t0 == pop::tirage_pondere_normalise(scores, 4, pop::cle_prolongement(cle, 0),
                                                                   pop::Selecteur::Trajet);
            trajet = trajet && decider_trajet(scores, 4, cle, 0, 'E') == 0;
            differents += t0 != t1;
        }
        Verifier(trajet, "E : la decision est tirage_pondere_normalise(cle_prolongement(cle, n), Trajet)");
        Verifier(differents > 500, "E : deux tentatives successives ne tirent pas la meme voie");

        Verifier(fraction_garee(7, 99, 1000, 'P') == 0.0f, "A''' controle : fraction 0");
        Verifier(fraction_garee(7, 99, 1000, 0) == fraction_garee(7, 99, 29ULL * 60 * 1000, 0),
                 "A''' : meme fraction pendant tout le cycle de 30 min");
        Verifier(fraction_garee(7, 99, 1000, 0) != fraction_garee(7, 99, 31ULL * 60 * 1000, 0),
                 "A''' : la fraction change au cycle suivant");
        Verifier(fraction_reevaluation(5, 1000, 0) == fraction_reevaluation(5, 1099, 0),
                 "H : meme fraction dans un seau de 100 ms");
        Verifier(fraction_reevaluation(5, 1000, 0) != fraction_reevaluation(5, 1100, 0),
                 "H : la fraction change au seau suivant");
        Verifier(fraction_delai(5, 0, 0) != fraction_delai(5, 1, 0), "G : deux delais, deux selecteurs");
    }

    // ── 5. A' : la naissance derivee ────────────────────────────────────────────────────
    {
        std::uint8_t brut[0x30] = {};
        const std::uint64_t zone = 0x1122334455667788ULL;
        const std::uint16_t a = 3, b = 4;
        const float s0 = 20.0f, s1 = 60.0f, L = 200.0f, cible = 2.0f;
        const std::uint32_t effectif = 1;
        std::memcpy(brut + 0x00, &zone, 8);
        std::memcpy(brut + 0x08, &a, 2);
        std::memcpy(brut + 0x0a, &b, 2);
        brut[0x0c] = 1;
        std::memcpy(brut + 0x10, &s0, 4);
        std::memcpy(brut + 0x14, &s1, 4);
        std::memcpy(brut + 0x18, &L, 4);
        std::memcpy(brut + 0x1c, &cible, 4);
        std::memcpy(brut + 0x20, &effectif, 4);
        const Creneau k = lire_creneau(brut);
        Verifier(k.zone == zone && k.a == 3 && k.b == 4 && k.c == 1 && k.s0 == 20.0f && k.s1 == 60.0f &&
                     k.cible == 2.0f && k.effectif == 1,
                 "lire_creneau : les champs aux decalages de F-PNJ-187");

        const pop::Emetteur e = emetteur_du_creneau(k, 0.0f, 200.0f, 0.8f, 1.6f);
        Verifier(e.n_attendu == 10.0f, "emetteur : n = cible × longueur du fragment / fenetre");

        const std::vector<std::uint64_t> personne;
        const Naissance n1 = preparer_naissance(e, k, 0xABCDEF, 600000, personne);
        Verifier(n1.a_cle && n1.abscisse >= s0 && n1.abscisse <= s1, "une naissance dans la fenetre");
        const Naissance bis = preparer_naissance(e, k, 0xABCDEF, 600000, personne);
        Verifier(bis.cle == n1.cle && bis.abscisse == n1.abscisse, "meme entree, meme individu (deux clients)");

        // Le meme individu, 3 s plus tard, est 3·v metres plus loin.
        const Naissance tard = preparer_naissance(e, k, 0xABCDEF, 603000, personne);
        Verifier(tard.a_cle, "trois secondes plus tard : encore une naissance");
        if (tard.cle == n1.cle)
        {
            Verifier(std::fabs((tard.abscisse - n1.abscisse) - 3.0f * n1.vitesse) < 0.01f,
                     "client en retard de 3 s : meme individu, 3·v metres plus loin");
        }

        // Deja vivant : le suivant ; tous vivants ou suspendus : hors cle.
        std::vector<std::uint64_t> vivants = {n1.cle};
        const Naissance n2 = preparer_naissance(e, k, 0xABCDEF, 600000, vivants);
        Verifier(!n2.a_cle || n2.cle != n1.cle, "un individu deja vivant n'est pas choisi deux fois");
        Naissance n = n2;
        for (int garde = 0; n.a_cle && garde < 4096; ++garde)
        {
            vivants.push_back(n.cle);
            n = preparer_naissance(e, k, 0xABCDEF, 600000, vivants);
        }
        Verifier(!n.a_cle, "tous les candidats vivants : hors cle");

        Verifier(!preparer_naissance(e, k, 0xABCDEF, 0, personne).a_cle, "sans horloge serveur : hors cle");
        const Creneau vide = lire_creneau(std::vector<std::uint8_t>(0x30, 0).data());
        Verifier(!preparer_naissance(emetteur_du_creneau(vide, 0, 0, 0.8f, 1.6f), vide, 1, 600000, personne).a_cle,
                 "creneau nul : hors cle");
        Verifier(preparer_naissance(e, k, 0xABCDEF + 1, 600000, personne).cle != n1.cle,
                 "une autre graine donne un autre individu");
    }

    // ── 6. Interrupteurs ────────────────────────────────────────────────────────────────
    {
        const Interrupteurs rien = lire_interrupteurs(nullptr, nullptr, nullptr, nullptr, nullptr);
        Verifier(!rien.derivee && rien.controle == 0 && !rien.score_pur && !rien.journal &&
                     !rien.emetteur_creneau,
                 "sans variable : tout eteint");
        const Interrupteurs i = lire_interrupteurs("1", "e", "1", "creneau", nullptr);
        Verifier(i.derivee && i.controle == 'E' && i.score_pur && i.emetteur_creneau && i.journal,
                 "toutes les variables lues, lettre en majuscule, journal implicite");
        Verifier(lire_interrupteurs("0", "Z", "", "fragment", nullptr).controle == 0,
                 "lettre de controle inconnue : ignoree");
    }

    std::printf("%s — %d verifications, %d echec(s)\n", g_echecs == 0 ? "OK" : "ROUGE", g_verifs, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

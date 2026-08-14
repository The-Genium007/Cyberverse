// Vérification du scénario du joueur robot, SANS le jeu.
//
// Une trajectoire fausse serait indétectable en session : on n'a rien à quoi la comparer, et un
// robot qui téléporte discrètement produirait des « téléports » dans le verdict qu'on attribuerait
// au netcode. C'est donc l'instrument qu'il faut vérifier en premier.
//
// Compilation :
//   cl /nologo /std:c++20 /W4 /WX /EHsc /I ..\src verif_robot.cpp

#include "PlayerSync/Robot.h"

#include <cmath>
#include <cstdio>
#include <string>

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
} // namespace

int main()
{
    using namespace Tessera::Sync;

    std::printf("-- continuite : aucun saut de position dans tout le cycle --\n");
    // C'EST LA VERIFICATION QUI COMPTE. Un scenario discontinu produirait des teleports que le
    // verdict imputerait au reseau — l'instrument accuserait le code qu'il est cense mesurer.
    {
        double sautMax = 0.0;
        double tSaut = 0.0;
        PoseRobot precedente = PoseDuRobot(0.0);
        for (int i = 1; i <= 40000; ++i) // un cycle complet au pas de 1 ms
        {
            const double t = i * 0.001;
            const PoseRobot p = PoseDuRobot(t);
            const double d = std::sqrt(
                static_cast<double>(p.dx - precedente.dx) * (p.dx - precedente.dx)
                + static_cast<double>(p.dy - precedente.dy) * (p.dy - precedente.dy)
                + static_cast<double>(p.dz - precedente.dz) * (p.dz - precedente.dz));
            if (d > sautMax)
            {
                sautMax = d;
                tSaut = t;
            }
            precedente = p;
        }
        // A 4 m/s (la course, phase 5), un millieme de seconde vaut 4 mm. On tolere 1 cm.
        Verifie(sautMax < 0.01,
                "pas maximal " + std::to_string(sautMax * 100.0) + " cm a t=" + std::to_string(tSaut)
                    + " s (attendu < 1 cm)");
    }

    std::printf("\n-- la boucle se referme : la fin du cycle rejoint le debut --\n");
    {
        const PoseRobot debut = PoseDuRobot(0.0);
        const PoseRobot fin = PoseDuRobot(kDureeCycleS - 0.001);
        const double ecart = std::sqrt(
            static_cast<double>(fin.dx - debut.dx) * (fin.dx - debut.dx)
            + static_cast<double>(fin.dy - debut.dy) * (fin.dy - debut.dy)
            + static_cast<double>(fin.dz - debut.dz) * (fin.dz - debut.dz));
        // Le raccord doit etre INVISIBLE : sinon le verdict compterait un teleport par cycle,
        // et l'instrument accuserait le code qu'il mesure.
        Verifie(ecart < 0.05, "raccord de boucle continu : " + std::to_string(ecart * 100.0)
                                  + " cm (attendu < 5)");
    }

    std::printf("\n-- chaque phase est atteinte, et une seule a la fois --\n");
    {
        bool vues[10] = {};
        for (int i = 0; i < 40000; ++i)
        {
            const PoseRobot p = PoseDuRobot(i * 0.001);
            if (p.phase >= 1 && p.phase <= 9)
            {
                vues[p.phase] = true;
            }
        }
        bool toutes = true;
        for (int ph = 1; ph <= 9; ++ph)
        {
            if (!vues[ph])
            {
                toutes = false;
                std::printf("       phase %d jamais atteinte\n", ph);
            }
        }
        Verifie(toutes, "les 9 phases du scenario sont jouees");
    }

    std::printf("\n-- les phases d'arret sont vraiment immobiles --\n");
    {
        const PoseRobot a = PoseDuRobot(10.0);
        const PoseRobot b = PoseDuRobot(11.5);
        Verifie(a.locomotion == kRobotIdle && b.locomotion == kRobotIdle,
                "l'arret net (phase 3) annonce bien locomotion=0");
        Verifie(std::fabs(a.dy - b.dy) < 0.001f, "et la position n'y bouge pas d'un centimetre");
    }

    std::printf("\n-- le pas de cote garde le cap, et se deplace lateralement --\n");
    {
        const PoseRobot p = PoseDuRobot(22.0); // milieu de la phase 6
        Verifie(std::fabs(p.yaw) < 0.001f, "cap inchange pendant le pas de cote");
        Verifie(std::fabs(p.dx) > 1.0f, "deplacement lateral reel (dx = "
                                            + std::to_string(p.dx) + " m)");
    }

    std::printf("\n-- la marche arriere recule vraiment, cap inchange --\n");
    {
        const PoseRobot a = PoseDuRobot(26.5);
        const PoseRobot b = PoseDuRobot(28.5);
        Verifie(b.dy < a.dy - 1.0f, "la position recule (" + std::to_string(a.dy) + " -> "
                                        + std::to_string(b.dy) + " m)");
        Verifie(std::fabs(b.yaw) < 0.001f, "cap toujours a zero");
    }

    std::printf("\n-- le saut monte, redescend, et s'annonce en l'air --\n");
    {
        const PoseRobot sol = PoseDuRobot(31.0);
        const PoseRobot sommet = PoseDuRobot(31.5);
        const PoseRobot retour = PoseDuRobot(31.99);
        Verifie(sol.dz < 0.05f, "depart au sol");
        Verifie(sommet.dz > 1.3f && sommet.dz < 1.5f,
                "sommet a " + std::to_string(sommet.dz) + " m (attendu ~1,4)");
        Verifie(retour.dz < 0.1f, "retour au sol en fin d'arc");
        Verifie(sommet.locomotion == kRobotEnLair,
                "le sommet annonce locomotion=6 (InAir) — le seul signal qui decrive un saut");
    }

    std::printf("\n-- robustesse : un temps negatif ou enorme ne casse rien --\n");
    {
        const PoseRobot n = PoseDuRobot(-5.0);
        const PoseRobot g = PoseDuRobot(1.0e6);
        Verifie(n.phase >= 1 && n.phase <= 9, "temps negatif -> phase valide");
        Verifie(g.phase >= 1 && g.phase <= 9, "temps enorme -> phase valide");
    }

    std::printf("\n%d verifications, %d echec(s)\n", g_total, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

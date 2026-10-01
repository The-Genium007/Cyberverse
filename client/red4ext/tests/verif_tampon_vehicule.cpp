// Auto-verification du lissage des voitures conduites (ADR 0054 §4). Aucun framework.
// cl /std:c++20 /EHsc /W4 /nologo client\red4ext\tests\verif_tampon_vehicule.cpp /Fe:verif_veh.exe
#include "../src/PlayerSync/TamponVehicule.h"

#include <cmath>
#include <cstdio>

using namespace Tessera::Sync;
static int g_echecs = 0;
static void Proche(float o, float a, const char* q, float tol = 1e-3f)
{
    if (std::fabs(o - a) > tol) { ++g_echecs; std::printf("  ECHEC : %s (obtenu %.4f, attendu %.4f)\n", q, o, a); }
}
static void Verifier(bool c, const char* q)
{
    if (!c) { ++g_echecs; std::printf("  ECHEC : %s\n", q); }
}
static EchantillonVehicule E(double t, float x, float yawDeg = 0.0f, float vx = 0.0f)
{
    EchantillonVehicule e;
    e.tMs = t; e.pos[0] = x; e.vit[0] = vx;
    const float h = yawDeg * 3.14159265f / 360.0f;
    e.q[2] = std::sin(h); e.q[3] = std::cos(h);
    return e;
}

int main()
{
    PoseVehicule p;
    TamponVehicule v;
    Verifier(!v.Rendre(0, p), "tampon vide : rien a rendre");

    v.Empiler(E(1000, 0.0f, 0.0f));
    v.Empiler(E(1100, 10.0f, 90.0f));
    Verifier(v.Rendre(1050, p), "interpole");
    Proche(p.pos[0], 5.0f, "position au milieu");
    Proche(p.q[2], std::sin(45.0f * 3.14159265f / 360.0f), "slerp : z a 45 deg de cap");
    Proche(p.q[3], std::cos(45.0f * 3.14159265f / 360.0f), "slerp : w a 45 deg de cap");
    Verifier(!p.extrapolee, "dans l'intervalle : pas extrapole");

    v.Rendre(900, p);
    Proche(p.pos[0], 0.0f, "avant le premier : tenu au premier");

    // extrapolation par la vitesse : 10 m/s pendant 100 ms = 1 m
    TamponVehicule w;
    w.Empiler(E(1000, 0.0f, 0.0f, 10.0f));
    w.Rendre(1100, p);
    Proche(p.pos[0], 1.0f, "extrapole 100 ms a 10 m/s");
    Verifier(p.extrapolee, "marque extrapole");
    // plafond 250 ms puis gel : 2,5 m, pas 10 m
    w.Rendre(2000, p);
    Proche(p.pos[0], 2.5f, "gele a 250 ms d'extrapolation");

    // un echantillon plus ancien ou egal est ignore
    TamponVehicule x;
    x.Empiler(E(1000, 0.0f));
    x.Empiler(E(900, 99.0f));
    x.Empiler(E(1000, 99.0f));
    x.Rendre(1000, p);
    Proche(p.pos[0], 0.0f, "echantillon en retard ignore");

    // le plus court chemin : q et -q sont la meme rotation, pas de demi-tour
    TamponVehicule y;
    EchantillonVehicule a = E(0, 0.0f, 10.0f), b = E(100, 0.0f, 20.0f);
    for (float& c : b.q) c = -c;
    y.Empiler(a); y.Empiler(b);
    y.Rendre(50, p);
    const float cap = 2.0f * std::atan2(p.q[2], p.q[3]) * 180.0f / 3.14159265f;
    Proche(cap, 15.0f, "slerp par le plus court chemin", 0.05f);

    // debordement de profondeur : on garde les plus recents
    TamponVehicule z;
    for (int i = 0; i < 40; ++i) z.Empiler(E(i * 10.0, float(i)));
    z.Rendre(385, p);
    Proche(p.pos[0], 38.5f, "profondeur bornee, interpolation recente", 0.1f);

    std::printf(g_echecs ? "ECHEC : %d\n" : "OK\n", g_echecs);
    return g_echecs ? 1 : 0;
}

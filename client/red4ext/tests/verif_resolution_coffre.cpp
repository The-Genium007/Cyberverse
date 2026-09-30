// =====================================================================================
// Auto-verification de la regle de resolution de `Tessera_CoffreVehicule` (F-DEV-031).
// Meme forme que `verif_tampon_interpolation.cpp` : aucun framework, aucun type moteur.
//
// Le bug : `m_coffreVehicule` porte DEUX sortes d'identifiants (voir `protocol.fbs` sur
// `CoffreContenu.contenant`) — l'id RESEAU d'un vehicule (a traduire via une table de
// lookup), OU l'`EntityID` de JEU d'un appareil du monde (deja locale, deja utilisable).
// Avant le correctif, la fonction cherchait TOUJOURS dans la table des vehicules, meme
// pour un contenant du monde — qui n'y est jamais. Ce fichier rejoue la regle exacte du
// correctif (`m_coffreEstContenant ? id direct : lookup vehicule`) sur des identifiants
// bruts, sans dependre de `RED4ext::ent::EntityID` ni d'aucun type du moteur.
//
//   cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\
//            Build\vcvars64.bat" && cl /std:c++20 /EHsc /W4 /nologo
//            client\red4ext\tests\verif_resolution_coffre.cpp /Fe:verif_coffre.exe && verif_coffre.exe'
// =====================================================================================

#include <cstdint>
#include <cstdio>
#include <unordered_map>

namespace {

// Mini-jumeau de `RED4ext::ent::EntityID` : juste le hash et la notion de "defini".
struct IdLocal {
    std::uint64_t hash = 0;
    constexpr bool EstDefini() const noexcept { return hash != 0; }
    constexpr bool operator==(const IdLocal& autre) const noexcept { return hash == autre.hash; }
};

// Copie fidele de la regle posee dans `NetworkGameSystem.h::Tessera_CoffreVehicule` apres
// le correctif du 2026-09-30 : le genre de session (`estContenant`) dit quelle traduction
// appliquer, et c'est la SEULE information qui le dise.
IdLocal ResoudreCoffreVehicule(std::uint64_t brut, bool estContenant,
                                const std::unordered_map<std::uint64_t, IdLocal>& lookupVehicules)
{
    if (brut == 0) {
        return IdLocal{};
    }
    if (estContenant) {
        return IdLocal{brut};
    }
    const auto it = lookupVehicules.find(brut);
    return (it == lookupVehicules.end()) ? IdLocal{} : it->second;
}

int g_echecs = 0;
int g_verifs = 0;

void Verifier(bool condition, const char* quoi)
{
    ++g_verifs;
    if (!condition) {
        ++g_echecs;
        std::printf("  ECHEC : %s\n", quoi);
    }
}

} // namespace

int main()
{
    std::unordered_map<std::uint64_t, IdLocal> lookupVehicules;
    lookupVehicules[42] = IdLocal{4242};

    // ── LE CAS DU BUG, TEL QUE MESURE EN JEU (F-DEV-031) ──────────────────────────────
    // Caisse d'appartement, id local 3510019503, jamais dans la table des vehicules.
    // AVANT le correctif : la fonction cherchait quand meme dans `lookupVehicules`, ne la
    // trouvait pas, et rendait `vide` — c'est exactement le `IdLocal{}` qu'on obtiendrait
    // ici en appelant `ResoudreCoffreVehicule(3510019503, false, lookupVehicules)`.
    const auto caisse = ResoudreCoffreVehicule(3510019503ull, /*estContenant=*/true, lookupVehicules);
    Verifier(caisse.EstDefini(), "une caisse (contenant du monde) doit resoudre a un id DEFINI");
    Verifier(caisse == IdLocal{3510019503ull}, "l'id resolu doit etre EXACTEMENT l'id local recu, sans traduction");

    // Le meme id, mais lu comme si c'etait un coffre de vehicule (ancien chemin, non
    // corrige) : il n'est pas dans la table, donc `vide` — le bug reste reproduit ICI.
    const auto memeIdMaisVehicule = ResoudreCoffreVehicule(3510019503ull, /*estContenant=*/false, lookupVehicules);
    Verifier(!memeIdMaisVehicule.EstDefini(), "le meme id, pris pour un vehicule, doit rester introuvable (c'etait le bug)");

    // ── NON-REGRESSION : le coffre de vehicule continue de passer par la table ────────
    const auto vehiculeConnu = ResoudreCoffreVehicule(42ull, /*estContenant=*/false, lookupVehicules);
    Verifier(vehiculeConnu == IdLocal{4242ull}, "un vehicule reseau connu doit toujours resoudre via la table");

    const auto vehiculeInconnu = ResoudreCoffreVehicule(999ull, /*estContenant=*/false, lookupVehicules);
    Verifier(!vehiculeInconnu.EstDefini(), "un vehicule reseau absent de la table doit rester indefini");

    // ── CAS LIMITE : id brut nul, quel que soit le genre de session ───────────────────
    Verifier(!ResoudreCoffreVehicule(0ull, true, lookupVehicules).EstDefini(), "id brut nul (contenant) -> indefini");
    Verifier(!ResoudreCoffreVehicule(0ull, false, lookupVehicules).EstDefini(), "id brut nul (vehicule) -> indefini");

    std::printf("%d verification(s), %d echec(s)\n", g_verifs, g_echecs);
    return g_echecs == 0 ? 0 : 1;
}

// Les crochets de l'interface staff dans le netcode (lot H3, ADR 0055). La partie Windows :
// chargement PARESSEUX de `tessera_staff_hote.dll`, maillon de fenetre, crochet de presentation.
// La partie pure (JSON, regles) est `PontStaff.h`.
//
// Rien ici ne s'execute tant que `Droits` n'a pas recu un rang staff : un joueur sans rang ne
// charge ni l'hote ni `libcef.dll`, n'a ni crochet de presentation ni maillon de fenetre.
#pragma once

#include <string>
#include <vector>

namespace Tessera::Staff::Hote
{
/// `PermissionSync` recu (fil du reseau). Rang staff : demarre l'hote (une seule fois par
/// processus) et pousse les droits a la page. Sinon : la page est masquee, CEF reste (ADR §8).
void Droits(const std::vector<std::string>& noeuds);

/// La connexion au serveur est etablie / perdue (fil du reseau).
void Connexion(bool connecte);

/// Un message JSON deja forme (`PontStaff.h`) a donner a la page. Perdu, et compte, si la page
/// n'est pas prete : elle n'a alors rien demande.
void VersPage(std::string json);

/// Tire le prochain message de la page a relayer au serveur (`commande`, `abonner`,
/// `avertissement_vu`), deja valide. `false` = rien. Fil du reseau.
bool Tirer(std::string& json);

/// F2 a libere la souris pour la page : le script pose le contexte modal (`UiKitStaffSouris.reds`).
bool SourisLibre();

/// Dechargement du plugin : on se desarme, le maillon de fenetre reste (ADR §3 b).
void Arreter();
} // namespace Tessera::Staff::Hote

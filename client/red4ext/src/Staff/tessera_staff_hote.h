/* Interface C de l'hote CEF de l'interface staff (ADR 0055). Cinq fonctions, rien d'autre.
 *
 * Appelant : le netcode C++ (Cyberverse.Red4Ext.dll) aujourd'hui, le client Rust demain.
 * Ecrit a la main, a tenir egal a src/lib.rs (le test `l_en_tete_c_declare_les_cinq_fonctions`
 * le verifie).
 *
 * Fils : `demarrer`, `arreter`, `entree`, `message` s'appellent depuis le fil qui possede la
 * fenetre du jeu ; `image` depuis le fil de rendu, dans le crochet de presentation.
 * Tout texte est de l'UTF-8 avec sa longueur, jamais termine par zero.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Codes de retour communs. */
#define STAFF_HOTE_OK 0
#define STAFF_HOTE_SANS_CEF (-1)   /* CEF n'a pas demarre : sous-processus absent, cef_initialize refuse,
                                      second demarrage dans le meme processus, composition en echec */
#define STAFF_HOTE_ARRETE (-2)     /* appel avant `demarrer` ou apres `arreter` */
#define STAFF_HOTE_ARGUMENT (-3)   /* pointeur nul, taille nulle, version inconnue */

#define STAFF_HOTE_VERSION 1

/* Message PRIVE que l'hote poste a la fenetre quand la page a parle (fil de CEF -> fil de la
 * fenetre). L'appelant le passe a `staff_hote_entree` comme tout autre message ; c'est la que le
 * rappel est appele. WM_APP + 0x7E5. */
#define STAFF_HOTE_WM_PONT (0x8000 + 0x7E5)

/* Page -> appelant : un message JSON du pont. Appele sur le fil qui possede la fenetre, pendant
 * `staff_hote_entree`. L'hote y ajoute les siens : {"type":"souris","charge":{"libre":true|false}}
 * a chaque bascule de F2.
 * ETAT (lot H1) : la liste fermee des types n'est PAS encore filtree par l'hote (lot H2). */
typedef void (*staff_hote_rappel)(void* contexte, const char* json, size_t longueur);

typedef struct {
  uint32_t version;          /* STAFF_HOTE_VERSION */
  void* fenetre;             /* HWND du jeu (classe W2ViewportClass) */
  uint32_t largeur, hauteur; /* zone cliente, en pixels */
  const char* dossier;       /* dossier de l'hote, ABSOLU : libcef.dll, tessera-staff-rendu.exe,
                                ressources, ui/ (et sonde/ si outils_dev) */
  size_t dossier_longueur;
  const char* page;          /* "tessera://staff/index.html" ; "tessera://sonde/index.html" en sonde */
  size_t page_longueur;
  uint32_t outils_dev;       /* 1 seulement sous --tessera-dev : sert tessera://sonde/, ouvre le
                                port de debogage 9229 */
  staff_hote_rappel rappel;
  void* contexte;
} staff_hote_config;

/* Une entree de la procedure de fenetre du jeu, telle quelle. */
typedef struct {
  uint32_t message; /* WM_* */
  uint64_t wparam;
  int64_t lparam;
} staff_hote_entree_win;

/* Lance CEF (processus de rendu a part) et charge la page. Paresseux : jamais appele sans rang staff. */
int32_t staff_hote_demarrer(const staff_hote_config* config);

/* Ferme le navigateur, arrete CEF, relache ce qui est tenu sur le peripherique. Idempotent.
 * CEF ne se reinitialise PAS dans le meme processus : apres `arreter`, `demarrer` refuse. */
void staff_hote_arreter(void);

/* Composition : dessine la derniere image de la page par-dessus le tampon d'arriere-plan.
 * `chaine_d_echange` = IDXGISwapChain*, `file_de_commandes` = ID3D12CommandQueue* du jeu.
 * Rend le nombre d'images de page recues depuis le demarrage (>= 0), ou un code d'erreur. */
int64_t staff_hote_image(void* chaine_d_echange, void* file_de_commandes);

/* Entrees : rend 1 si l'hote a CONSOMME le message (le jeu ne doit pas le voir), 0 sinon,
 * ou un code d'erreur. F2 bascule « souris libre » ; souris au jeu -> 0, sauf F2 et
 * STAFF_HOTE_WM_PONT. Lui passer aussi WM_SIZE (rend 0) : la page suit la taille de la fenetre. */
int32_t staff_hote_entree(const staff_hote_entree_win* entree);

/* Pont, appelant -> page : un message JSON de la liste fermee (reponse, evenement, droits,
 * etat_connexion), donne a `tesseraPont.recevoir` de la page.
 * ETAT (lot H1) : le type n'est PAS encore filtre par l'hote (lot H2). */
int32_t staff_hote_message(const char* json, size_t longueur);

#ifdef __cplusplus
}
#endif

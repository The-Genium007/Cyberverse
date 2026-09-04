#include "CommandLine.h"
#include "TesseraSpawnEnrichi.h"

#include "TesseraEsthetiqueV.h"
// ⚠️ `Main.h` AVANT `Utils.h` : ce dernier journalise par `SDK`/`PLUGIN`, que seul `Main.h`
// definit. L'inverse produit « SDK : identificateur non declare » DANS Utils.h — une erreur
// qui designe le mauvais fichier, et qu'on cherche donc au mauvais endroit.
#include "Main.h"
#include "Utils.h"

#include <RED4ext/Scripting/Natives/Generated/game/TargetSearchFilter.hpp>

#include <windows.h>

#include <cmath>
#include <map>
#include <cstdio>
#include <cstring>

namespace Tessera::SpawnEnrichi
{
bool g_actif = false;

namespace
{
// ── LES CONSTANTES ADRESSEES AU BINAIRE — Cyberpunk 2077 2.31, et elle seule ─────────────────────
//
// Toutes viennent de mesures datees de la sonde `tools/re-probe`. Aucune n'est re-devinee ici.

/// La voie ENRICHIE du spawner du mode photo — celle qui accepte une charge de customisation.
/// `FUN_140662e08`, mesuree F-PLY-194.
constexpr std::uint64_t kRvaSpawnEnrichi = 0x662E08ull;

/// `DynamicBuffer::Reserve(tableau, capacite, tailleElement, alignement, rappel)` — F-PLY-267.
/// Identifiee par l'assertion de debug de CDPR elle-meme
/// (`redContainers/src/dynamicBuffer.cpp`, message nommant `capacity` et `elementSize`).
constexpr std::uint64_t kRvaReserve = 0x14B9D0ull;

/// Le spawner vit dans le systeme photomode, a cet offset (F-PLY-194).
constexpr std::size_t kOffsetSpawner = 0x390;

/// Taille de la requete de spawn, assemblee a la main.
constexpr std::size_t kTailleRequete = 0xF0;

/// Une paire de customisation : deux `u64`.
constexpr std::uint32_t kTaillePaire = 0x10;
constexpr std::uint32_t kAlignementPaire = 8;

using SpawnEnrichi_t = void* (*)(void* aSpawner, void* aSortie, void* aRequete, std::uint64_t aRecord,
                                 void* aCharge);
using Reserve_t = void (*)(void* aTableau, std::uint32_t aCapacite, std::uint32_t aTailleElement,
                           std::uint32_t aAlignement, void* aRappel);

/// ⭐ LE RECORD DES CORPS ENRICHIS — et il ne vient PAS du serveur.
///
/// Le serveur envoie un record de PASSANT (`Character.CitizenRichFemale`...), parce que c'est ce
/// qu'il fallait tant que le corps ne portait pas de visage. Le passer a la voie enrichie n'aurait
/// aucun sens : F-PLY-207 a mesure que vingt pantins de FOULE spawnes par cette voie sortaient
/// IDENTIQUES entre eux — la charge n'y produit rien d'observable.
///
/// La voie enrichie exige un record de la chaine PHOTOMODE, seule ascendance qui porte la
/// machinerie de customisation. `_Marche_` est celui que le controle positif de Lucas a valide le
/// 2026-08-23 (« je vois un personnage devant moi, exactement le meme que moi ») : entite du
/// photomode avec le graphe `humanoid.animgraph` et les six champs d'IA, donc un corps qui MARCHE.
///
/// ⚠️ **Un seul record, pas deux.** Aucun record genre n'est necessaire : le SEXE DU CORPS SUIT LA
/// CHARGE, jamais le nom du record — mesure a l'oeil du 2026-08-23, un record nomme `_Male` charge
/// d'une esthetique feminine rend un corps de femme (F-PLY-267).
///
/// ⚠️⚠️ **CE RECORD DOIT ETRE DANS LE MODSET LIVRE.** Il vit aujourd'hui dans `tools/re-probe`,
/// c'est-a-dire chez Lucas seulement (ADR 0038). Tant qu'il n'est pas promu, la voie enrichie
/// echouera chez un joueur — proprement, en retombant sur la voie sure, mais elle echouera.
constexpr const char* kRecordEnrichi = "Character.Tessera_Avatar_Marche_Male";

/// ⭐ LA VARIANTE FEMININE — meme record a un champ pres : elle pointe `avatar_distant_wa.ent`,
/// dont les trois composants de vetements portent des meshes `_wa_`.
///
/// ⚠️ ELLE N'EXISTE PAS POUR LE CORPS. Le corps suit la charge d'esthetique (F-PLY-267) : c'est
/// mesure, et l'entite masculine rend tres bien un V feminin. Elle existe parce que les VETEMENTS
/// sont des composants de l'entite (F-PLY-286) et sont donc figes a sa construction (F-PLY-191) —
/// il n'y a aucun moyen de les changer apres coup.
constexpr const char* kRecordEnrichiFeminin = "Character.Tessera_Avatar_Marche_Female";

/// L'ascendance de notre record : c'est elle que la sonde reconnait (`Player_Puppet`).
const std::uint64_t kRecordPhotomode = RED4ext::TweakDBID("Character.Player_Puppet_Photomode").value;
const std::uint64_t kRecordPhotomodeBase = RED4ext::TweakDBID("Character.Player_Puppet_Base").value;

/// Le TweakDBID de `kRecordEnrichi`, calcule une fois.
inline std::uint64_t recordAttendu()
{
    return RED4ext::TweakDBID(kRecordEnrichi).value;
}

/// Les six carnations du jeu, DANS L'ORDRE DE LEUR NOM — c'est cet ordre que `body_color` indexe.
///
/// ⛔ POURQUOI CETTE TABLE EXISTE. Notre `.app` figeait la couleur de peau du CORPS :
/// `02_ca_limestone` pour tout joueur masculin, `06_bl_dark` pour toute joueuse. Le VISAGE, lui,
/// est bati depuis la charge d'esthetique et etait donc juste — d'ou le symptome exact rapporte par
/// Lucas le 2026-09-03 : « la couleur des jambes et du tronc ne correspond pas ».
///
/// ⛔⛔ ET LA VOIE FACILE EST FERMEE, PAR MESURE (F-PLY-379). Ecrire `meshAppearance` a l'execution
/// sur les composants du corps est accepte, RELU, et sans aucun effet visuel — y compris sur notre
/// entite derivee, alors meme que `Toggle` y marche (F-PLY-377). La frontiere n'est pas « script
/// contre natif », elle est PRESENCE contre APPARENCE.
constexpr const char* kCarnations[] = {
    "01_ca_pale", "02_ca_limestone", "03_ca_senna",
    "04_ca_almond", "05_bl_espresso", "06_bl_dark",
};
constexpr std::size_t kNbCarnations = sizeof(kCarnations) / sizeof(kCarnations[0]);

/// Le record a spawner : la morphologie, et la CARNATION LUE DANS LA CHARGE.
///
/// ⭐⭐ ON NE DEVINE AUCUN INDEX, ON RECONNAIT UN NOM. Une premiere version cherchait une paire
/// `body_color` et prenait sa valeur pour un indice dans la table — c'etait faux sur les deux
/// points, et le resultat le disait : les DEUX avatars recevaient l'index 0, y compris REDDA dont
/// `body_color` vaut 6.
///
/// La vraie disposition est au registre, mesuree sur donnee vivante (F-PLY-196) : le second champ
/// d'une paire resout en `CName` et porte `<composant>__<meshAppearance>`. La carnation est donc
/// dans la charge SOUS SON NOM — `t0_000_pma_base__full__02_ca_limestone` — et pas sous un indice.
///
/// ⭐ Un `CName` ne se remonte pas, mais il se COMPARE : on hache les six candidats et on cherche
/// celui qui figure. C'est exact par construction, et ca supprime la seule inconnue qui restait
/// (« `body_color` est-il 0-base ? » n'a plus de raison d'etre posee).
///
/// ⚠️ Le defaut est la PREMIERE teinte, jamais un record inexistant : un record absent ferait
/// echouer le spawn en silence, et l'avatar retomberait sur un passant generique — ce qui se lit
/// comme une panne de reseau.
inline std::string RecordPourCarnation(bool aCorpsMasculin,
                                       const std::vector<std::uint64_t>& aPaires)
{
    const char* const corps = aCorpsMasculin ? "t0_000_pma_base__full__" : "t0_000_pwa_base__full__";
    for (std::size_t c = 0; c < kNbCarnations; ++c)
    {
        const std::uint64_t attendu = RED4ext::CName(
            (std::string(corps) + kCarnations[c]).c_str()).hash;
        for (std::size_t i = 0; i * 2 + 1 < aPaires.size(); ++i)
        {
            if (aPaires[i * 2 + 1] == attendu)
            {
                return std::string(aCorpsMasculin ? kRecordEnrichi : kRecordEnrichiFeminin) + "_"
                       + kCarnations[c];
            }
        }
    }
    return std::string(aCorpsMasculin ? kRecordEnrichi : kRecordEnrichiFeminin) + "_"
           + kCarnations[0];
}

/// Parmi `aNeufs`, celui qui est le plus proche de `aCible`. `0` si aucun n'est lisible.
///
/// Le discriminant est la POSITION VISEE : on vient de demander un corps la, le bon candidat est
/// celui qui s'y trouve. Ce n'est pas une heuristique de confort — c'est la seule information que
/// nous ayons fournie a l'appel, donc la seule qui nous appartienne.
inline std::uint64_t ChoisirLePlusProche(const std::vector<std::uint64_t>& aNeufs,
                                         const RED4ext::Vector4& aCible)
{
    if (aNeufs.empty()) { return 0; }

    // ⭐⭐ UN CANDIDAT UNIQUE SE PREND, SANS CONDITION — mesure du 2026-08-23, 17:48.
    //
    // Le journal rendait `candidats 0->1 (notre record 1)` ET « JAMAIS APPARU » dans la MEME ligne :
    // une entite portant notre record etait bien apparue, et cette fonction la jetait.
    //
    // La cause : chaque candidat etait resolu par `GetDynamicEntity`, qui interroge le
    // `DynamicEntitySystem` — lequel ne connait QUE les entites qu'il a lui-meme creees (Codeware).
    // Notre corps vient du spawner natif du photomode : il n'y est pas. La resolution echouait, le
    // `continue` l'ecartait, et la fonction rendait 0.
    //
    // ⚠️ LA REGLE : **un discriminant ne doit jamais pouvoir ECARTER le seul candidat qu'il est
    // cense departager.** Departager, c'est choisir entre plusieurs ; avec un seul, il n'y a rien a
    // choisir et le discriminant n'a pas voix au chapitre. C'est la troisieme fois aujourd'hui
    // qu'un mecanisme auxiliaire fait echouer la mesure qu'il devait servir — apres l'instrument
    // qui confondait « rien vu » et « pas pu regarder » (F-PLY-270) et l'attente deduite d'un
    // diagnostic vide.
    if (aNeufs.size() == 1) { return aNeufs[0]; }

    std::uint64_t meilleur = 0;
    float meilleureDistance = 1e9f;
    for (const auto id : aNeufs)
    {
        const auto e = Cyberverse::Utils::GetDynamicEntity(RED4ext::ent::EntityID{id});
        if (!e.has_value()) { continue; }
        const auto p = Cyberverse::Utils::Entity_GetWorldPosition(e.value());
        const float dx = p.X - aCible.X, dy = p.Y - aCible.Y, dz = p.Z - aCible.Z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < meilleureDistance) { meilleureDistance = d; meilleur = id; }
    }
    // ⚠️ ET SI AUCUN N'A PU ETRE RESOLU, ON PREND LE PREMIER plutot que de tout jeter. Ils portent
    // TOUS notre record — c'est deja une identification. Renoncer ici ferait retomber sur la voie
    // sure alors qu'on tient le corps.
    return meilleur != 0 ? meilleur : aNeufs[0];
}

/// Rayon d'enumeration pour retrouver le corps qu'on vient de fabriquer, en metres.
///
/// ⚠️⚠️ **12 m LE 2026-08-23 A MIDI, ET C'ETAIT UN FAUX NEGATIF — mesure en jeu le soir meme.**
///
/// Le journal rendait `0 entite(s) neuve(s) … 0 avant / 0 apres`, donc « le corps n'existe pas ».
/// Lucas, lui, VOYAIT les corps a l'ecran, et les distinguait par leur coiffure — donc ils
/// existaient ET portaient la bonne esthetique. L'enumeration cherchait simplement trop pres.
///
/// La cause est evidente une fois dite : **le corps naît a la position du VOISIN**, pas a celle du
/// joueur local. Un voisin peut etre a n'importe quelle distance jusqu'a la portee de visibilite du
/// serveur (100 m par defaut, `[runtime.aoi] visibility_radius`). Chercher dans 12 m, c'est ne
/// trouver que le cas ou les deux joueurs se touchent.
///
/// 150 m couvre l'AoI avec de la marge. Le cout est celui de deux enumerations, **au spawn
/// uniquement** — jamais par frame.
///
/// ⚠️ Et la lecon depasse ce chiffre : le commentaire d'origine DISAIT deja « un instrument trop
/// court ne rend pas rien, il rend un FAUX NEGATIF ». Je l'ai ecrit, et j'ai quand meme pose 12 m.
/// Ecrire la regle ne dispense pas de l'appliquer au nombre qu'on est en train de choisir.
constexpr float kRayonRecherche = 150.0f;

/// Resout un systeme du jeu par son nom RTTI.
///
/// ⚠️ **Pas de `CallStatic`, et c'est une lecon payee** (2026-08-15, systeme workspot) :
/// `CallStatic("ScriptGameInstance", "Get...")` a echoue avec CHAQUE type de sortie essaye, en
/// journalisant parfaitement ses ordres pendant que la couche visee n'etait jamais atteinte.
/// Resoudre par le TYPE dans le registre du moteur ne depend d'aucune signature de script.
RED4ext::IScriptable* SystemeParNom(const char* aNom)
{
    auto* rtti = RED4ext::CRTTISystem::Get();
    if (rtti == nullptr) { return nullptr; }
    auto* cls = rtti->GetClass(aNom);
    if (cls == nullptr) { return nullptr; }
    auto* engine = RED4ext::CGameEngine::Get();
    if (engine == nullptr || engine->framework == nullptr || engine->framework->gameInstance == nullptr)
    {
        return nullptr;
    }
    return engine->framework->gameInstance->GetSystem(cls);
}

/// Vrai si `aObjet` est bien de la classe `aClasse`.
///
/// ⚠️ On va calculer des adresses a offset fixe dans ces objets et appeler des fonctions natives.
/// Un objet inattendu ici, ce n'est pas une erreur : c'est un crash (F-PLY-225).
bool EstDeClasse(RED4ext::IScriptable* aObjet, const char* aClasse)
{
    if (aObjet == nullptr || aObjet->GetType() == nullptr) { return false; }
    const char* nom = aObjet->GetType()->name.ToString();
    return nom != nullptr && std::strcmp(nom, aClasse) == 0;
}

/// Les `EntityID` des corps portant `aRecord` autour du joueur.
///
/// ⭐ **C'est le seul chemin vers l'`EntityID` d'un corps enrichi**, et il n'est pas de confort : le
/// retour de la voie enrichie N'EST PAS un objet de script — le dereferencer a fait planter le jeu
/// le 2026-08-21 (F-PLY-225). On ne le touche donc jamais, et on retrouve le corps par le monde.
///
/// C'est la voie qu'emploie la sonde depuis toujours (`GetEntitiesAroundObject`, filtre par record),
/// et elle a deja enumere 200 corps enrichis a la fois (F-PLY-170) — donc elle les VOIT.
/// Ce qu'une enumeration a vu. ⚠️ **Les trois nombres existent parce que `0` ne suffisait pas.**
///
/// La premiere version ne rendait que le compte FILTRE. Le journal disait donc « 0 entite(s) »
/// aussi bien quand il n'y avait rien autour que quand il y avait cent entites dont aucune ne
/// passait le filtre. **Deux pannes opposees, un seul message** — et deux sessions de jeu pour
/// s'en apercevoir, alors que le chiffre manquant coutait une ligne.
struct Releve
{
    /// Tous les identifiants vus, sans aucun filtre. C'est LUI qui sert a la difference.
    std::vector<std::uint64_t> tous;
    /// Combien portaient le record attendu.
    std::uint32_t duRecord = 0;
    /// Combien portaient le record PHOTOMODE (leur ascendance). La sonde filtre sur celui-la.
    std::uint32_t duPhotomode = 0;
    /// Les identifiants de ceux qui portent l'un OU l'autre — les seuls candidats credibles.
    ///
    /// ⚠️ **C'EST CE QUI REMPLACE « LE PLUS PROCHE ».** Prendre la nouvelle entite la plus proche
    /// de la position visee n'IDENTIFIE rien : dans une rue peuplee, les entites entrent et sortent
    /// en permanence de la fenetre de 128, et un passant quelconque peut se trouver plus pres que
    /// notre corps. On enregistrerait alors le mauvais — ce qui donne exactement le symptome
    /// observe le 2026-08-23 : deux corps empiles dont aucun ne suit.
    std::vector<std::uint64_t> notres;
    /// L'enumeration elle-meme a-t-elle repondu ? `false` = l'appel a echoue, et tout le reste est
    /// sans valeur. Sans ce drapeau, « aucune entite » et « je n'ai pas pu regarder » se
    /// confondent — et c'est la confusion qui a coute le plus cher sur ce chantier.
    bool interrogeable = false;
    /// Quelle forme d'appel a mordu (`"2 params"`, `"1 param"`, `"AUCUNE"`). Journalisee : le jour
    /// ou l'une des deux cesse de marcher, on saura laquelle on employait.
    const char* forme = "?";
};

/// Releve les entites autour du joueur.
///
/// ⚠️ **PLUS AUCUN FILTRE PAR RECORD, et c'est le correctif du 2026-08-23 au soir.** La version
/// precedente ne gardait que les entites dont `GetRecordID()` egalait exactement notre record. Elle
/// rendait 0 alors que Lucas VOYAIT les corps a l'ecran, coiffures distinctes a l'appui.
///
/// Trois causes possibles, et le filtre les rendait indiscernables : l'entite rapporte peut-etre le
/// record de sa BASE (`Player_Puppet_Photomode`) plutot que le notre — c'est d'ailleurs sur
/// `Player_Puppet` que filtre la sonde qui, elle, les trouve ; ou `GetRecordID` ne se lie pas depuis
/// le C++ ; ou `GetEntitiesAroundObject` non plus.
///
/// On ne cherche donc plus a reconnaitre le corps : **on prend la DIFFERENCE avant/apres**. Une
/// difference ne suppose rien de ce qu'on cherche, et c'est precisement sa valeur ici.
void Relever(std::uint64_t aRecord, Releve& aOut)
{
    aOut.tous.clear();
    aOut.duRecord = 0;
    aOut.interrogeable = false;

    const auto joueur = Cyberverse::Utils::GetPlayer();
    if (joueur == nullptr) { return; }

    // ── LA SIGNATURE EXACTE, ET DEUX ERREURS QUE J'AI FAITES ENSEMBLE ───────────────────────
    //
    // Le script decompile de CDPR fait foi (`core/entity/gameObject.script:936`) :
    //
    //     public function GetEntitiesAroundObject( optional range : Float,
    //                                              optional searchFilter : TargetSearchFilter )
    //         : array< Entity >
    //
    // Je demandais `array<GameObject>` — c'est `Entity`. Et je ne passais qu'UN parametre alors
    // qu'il y en a deux. L'appel rendait donc `false`, que je lisais comme « aucune entite autour ».
    //
    // ⚠️ **C'est la meme faute que celle du dump RTTI, prise par l'autre bout** : j'ai suppose une
    // signature au lieu de la LIRE dans le script decompile, qui est la source de verite. Deux
    // correctifs (rayon 12->150 m, puis retrait du filtre par record) ont ete ecrits contre une
    // panne qui n'etait ni l'un ni l'autre — parce que mon message confondait « rien vu » et
    // « pas pu regarder ». Le drapeau `interrogeable` existe pour que ca n'arrive plus.
    //
    // LES DEUX FORMES SONT ESSAYEES, et le journal dit laquelle a mordu. Ce n'est pas de
    // l'indecision : chaque essai en jeu coute une fermeture, un deploiement et deux
    // rechargements. Quand un aller-retour est cher, on demande TOUT ce qu'on veut savoir d'un
    // coup — c'est la meme discipline que grouper les mesures qui exigent Lucas.
    RED4ext::DynArray<RED4ext::Handle<RED4ext::ent::Entity>> autour;
    RED4ext::game::TargetSearchFilter filtre{};

    // ⚠️ LA FORME SANS FILTRE D'ABORD, et ce n'est pas un detail de style.
    //
    // La sonde CET (`TesseraApparence`) appelle `p:GetEntitiesAroundObject(rayon)` avec le SEUL
    // rayon, et elle trouve ces corps depuis toujours — elle en a deja enumere 200 d'un coup
    // (F-PLY-170). C'est donc la forme PROUVEE sur ces entites precises.
    //
    // La forme a deux parametres se lie aussi (mesure du 2026-08-23 : elle rend 128 entites), mais
    // elle passe un `TargetSearchFilter` construit par defaut — dont on ne sait PAS ce qu'il exclut.
    // Un filtre inconnu qui rend beaucoup d'entites ressemble a un filtre inoffensif ; il peut
    // parfaitement ecarter la seule qui nous interesse.
    if (Red::CallVirtual(joueur, "GetEntitiesAroundObject", autour, kRayonRecherche))
    {
        aOut.forme = "1 param";
    }
    else if (Red::CallVirtual(joueur, "GetEntitiesAroundObject", autour, kRayonRecherche, filtre))
    {
        aOut.forme = "2 params";
    }
    else
    {
        aOut.forme = "AUCUNE";
        return;
    }
    aOut.interrogeable = true;
    // ⚠️ DEUX TYPES DE TABLEAU DANS CE MEME FICHIER, et ils n'ont pas la meme convention.
    // Celui du SDK RED4ext expose `size()` — une METHODE. Celui du MOTEUR, qu'on manipule par
    // offsets pour la charge de customisation, expose un CHAMP `size`. Les confondre donne une
    // erreur de conversion vers un pointeur de membre, qui ne ressemble pas du tout a sa cause.
    for (std::uint32_t i = 0; i < autour.size(); ++i)
    {
        auto& obj = autour[i];
        if (obj == nullptr) { continue; }
        aOut.tous.push_back(obj->entityID.hash);
        RED4ext::TweakDBID rec{};
        if (!Red::CallVirtual(obj, "GetRecordID", rec)) { continue; }
        if (rec.value == aRecord)
        {
            ++aOut.duRecord;
            aOut.notres.push_back(obj->entityID.hash);
        }
        // ⚠️ ET LE RECORD DE BASE, parce que la sonde filtre sur `Player_Puppet` — pas sur le
        // nôtre — et qu'elle TROUVE ces corps depuis toujours (200 énumérés d'un coup, F-PLY-170).
        // Si ces entités rapportent le record de leur ascendance plutôt que le nôtre, c'est ici
        // qu'on le verra. Un compteur qui vaut zéro des deux côtés dit autre chose qu'un compteur
        // qui vaut zéro d'un seul.
        else if (rec.value == kRecordPhotomode || rec.value == kRecordPhotomodeBase)
        {
            ++aOut.duPhotomode;
            aOut.notres.push_back(obj->entityID.hash);
        }
    }
}
}  // namespace

/// Ce qu'on retient d'un appel deja lance, en attendant que son corps apparaisse.
///
/// ⭐⭐ POURQUOI CETTE ATTENTE EXISTE — mesure du 2026-08-23, 17:20.
///
/// Le journal rendait `autour 128->128` : l'enumeration fonctionnait (128 entites vues), et
/// pourtant RIEN de neuf apres l'appel. Or Lucas voyait le corps a l'ecran. Les deux faits ne se
/// contredisent pas : **la creation d'entite est ASYNCHRONE**. On cherchait dans la meme frame que
/// l'appel, c'est-a-dire avant que le corps n'existe.
///
/// C'est d'ailleurs pour ca que la sonde, elle, les trouve : elle enumere par une commande
/// separee, des secondes plus tard.
///
/// ⚠️ ET L'APPEL NE DOIT PARTIR QU'UNE FOIS. L'appelant retente le spawn a CHAQUE snapshot tant
/// que l'entite manque — rappeler le spawner a chaque passage fabriquerait un corps par tick.
/// `appele` est ce qui l'empeche, et c'est le champ le plus important de cette structure.
struct EnAttente
{
    /// Les identifiants vus AVANT l'appel. La difference se fait contre eux, pas contre le passage
    /// precedent : un voisin qui arrive entre deux passages ne doit pas etre pris pour notre corps.
    std::vector<std::uint64_t> avant;
    RED4ext::Vector4 position{};
    std::uint32_t passages = 0;
    bool appele = false;
    /// ⭐ Le pointeur RENDU par l'appel enrichi. Décompilation du 2026-08-23 :
    /// `FUN_1406627d4` termine par `*param_2 = lVar1` — **le retour EST un objet**, pas une
    /// valeur jetable, et il reçoit un champ à `+0x110`.
    ///
    /// Or le premier relevé montrait `+0x48 = 0` — l'emplacement du `entityID` sur une
    /// `ent::Entity`. Zéro **au moment de l'appel**, ce qui est exactement ce qu'on attend d'une
    /// création asynchrone : le champ se remplit ensuite. D'où l'idée de le RELIRE.
    void* retour = nullptr;
};

/// Combien de passages on accorde au moteur pour faire naitre le corps.
///
/// L'appelant repasse a chaque snapshot (20-25 Hz), donc ~30 passages valent un peu plus d'une
/// seconde. Genereux exprès : un abandon trop tot retomberait sur la voie sure alors que le corps
/// est en route, et on aurait alors DEUX corps — exactement le defaut qu'on repare.
constexpr std::uint32_t kPassagesMax = 250;   // ~10 s : 30 passages ne faisaient que 1,2 s

/// Les appels en cours, par identifiant reseau.
static std::map<std::uint64_t, EnAttente> g_enAttente;

Resultat Tenter(std::uint64_t aNetworkId, const std::vector<std::uint8_t>& aBlob,
                const RED4ext::Vector4& aPosition, bool aCorpsMasculin)
{
    Resultat r;

    // ── GARDE 1 : L'INTERRUPTEUR ────────────────────────────────────────────────────────────────
    // Rien de ce qui suit ne s'execute tant qu'il est eteint. C'est le premier garde-fou decide
    // avec Lucas, et il rend l'A/B possible : un seul parametre change entre les deux moities.
    if (!g_actif)
    {
        return r;
    }
    // Sans fiche d'apparence, cette voie n'apporte rien que la voie sure ne fasse mieux.
    if (aBlob.empty())
    {
        return r;
    }
    r.tente = true;

    // ── UN APPEL DEJA PARTI POUR CE VOISIN ? ALORS ON CHERCHE, ON NE RAPPELLE PAS ───────────────
    //
    // L'appelant repasse ici a CHAQUE snapshot tant que l'entite manque. Sans ce rendez-vous, on
    // relancerait le spawner vingt fois par seconde — un corps par tick, tous invisibles a nos
    // tables, tous impossibles a effacer.
    {
        const auto attente = g_enAttente.find(aNetworkId);
        if (attente != g_enAttente.end() && attente->second.appele)
        {
            auto& a = attente->second;
            ++a.passages;

            // ── ⭐⭐ RELIRE L'OBJET RENDU — la voie la plus directe, et la moins chere ─────────
            //
            // Décompilation de `FUN_1406627d4` : le retour est un objet dont un champ est écrit à
            // `+0x110`. Le premier relevé donnait `+0x48 = 0` — la place du `entityID` sur une
            // `ent::Entity`. Zéro AU MOMENT DE L'APPEL est cohérent avec la création asynchrone
            // qu'on a mesurée par ailleurs : le champ se remplit après.
            //
            // ⚠️⚠️ ON LIT, ON N'APPELLE RIEN. Le crash du 2026-08-21 (F-PLY-225) ne venait pas
            // d'avoir lu ce pointeur : il venait d'avoir APPELÉ `GetFunction` dessus, sur un type
            // bidon. Une lecture gardée par `Lisible` vérifie que la page est mappée ; c'est tout
            // ce dont on a besoin.
            //
            // ⚠️ ET ON VALIDE AVANT D'UTILISER. Un `EntityID` plausible ici ressemble à ceux que la
            // voie sûre produit — de l'ordre de 10 000 000 à 11 000 000 sur cette session. Prendre
            // n'importe quel entier non nul ferait piloter n'importe quoi, et le défaut se
            // chercherait très loin d'ici.
            if (a.retour != nullptr
                && EsthetiqueV::Lisible(reinterpret_cast<std::uintptr_t>(a.retour), 0x120))
            {
                const auto* mots = reinterpret_cast<const std::uint64_t*>(a.retour);
                const std::uint64_t candidats[] = {mots[0x48 / 8], mots[0x110 / 8], mots[0x118 / 8]};
                const char* nomOffset[] = {"+0x48", "+0x110", "+0x118"};
                for (int k = 0; k < 3; ++k)
                {
                    const std::uint64_t v = candidats[k];
                    // Plage volontairement large mais FINIE : un identifiant, pas un pointeur.
                    if (v > 1000000ull && v < 0xFFFFFFFFull)
                    {
                        r.entite = RED4ext::ent::EntityID{v};
                        r.appelFait = true;
                        char b[300];
                        std::snprintf(b, sizeof(b),
                                      "corps enrichi TROUVE PAR LE RETOUR apres %u passage(s) — "
                                      "entite %llu lue en %s de l'objet rendu",
                                      a.passages, (unsigned long long)v, nomOffset[k]);
                        r.diag = b;
                        g_enAttente.erase(attente);
                        return r;
                    }
                }
                // Diagnostic espace : trois relectures suffisent a voir si ca se remplit.
                if (a.passages == 1 || a.passages == 30 || a.passages == 200)
                {
                    SDK->logger->InfoF(PLUGIN,
                                       "[retour relu, passage %u] +48=%llX +110=%llX +118=%llX",
                                       a.passages, (unsigned long long)candidats[0],
                                       (unsigned long long)candidats[1],
                                       (unsigned long long)candidats[2]);
                }
            }

            Releve maintenant;
            Relever(recordAttendu(), maintenant);
            // ⚠️ La difference porte sur les CANDIDATS CREDIBLES, pas sur tout le voisinage. Un
            // passant qui entre dans la fenetre n'est pas notre corps, meme s'il est plus proche.
            std::vector<std::uint64_t> neufs;
            for (const auto id : maintenant.notres)
            {
                bool connu = false;
                for (const auto v : a.avant)
                {
                    if (v == id) { connu = true; break; }
                }
                if (!connu) { neufs.push_back(id); }
            }

            const std::uint64_t trouve = ChoisirLePlusProche(neufs, a.position);
            if (trouve != 0)
            {
                r.entite = RED4ext::ent::EntityID{trouve};
                r.appelFait = true;
                char b[300];
                std::snprintf(b, sizeof(b),
                              "corps enrichi TROUVE apres %u passage(s) — entite %llu · "
                              "autour %u->%u · appel %s",
                              a.passages, static_cast<unsigned long long>(trouve),
                              static_cast<unsigned>(a.avant.size()),
                              static_cast<unsigned>(maintenant.tous.size()), maintenant.forme);
                r.diag = b;
                g_enAttente.erase(attente);
                return r;
            }

            if (a.passages < kPassagesMax)
            {
                // Aucun diagnostic : journaliser ici ecrirait trente lignes par voisin. C'est
                // `attente` qui porte la decision, pas le silence.
                r.appelFait = true;
                r.attente = true;
                return r;
            }

            char b[300];
            std::snprintf(b, sizeof(b),
                          "corps enrichi JAMAIS APPARU apres %u passage(s) — candidats %u->%u "
                          "(notre record %u, photomode %u, voisinage %u), appel %s. "
                          "Reprise par la voie sure.",
                          a.passages, static_cast<unsigned>(a.avant.size()),
                          static_cast<unsigned>(maintenant.notres.size()), maintenant.duRecord,
                          maintenant.duPhotomode, static_cast<unsigned>(maintenant.tous.size()),
                          maintenant.forme);
            r.diag = b;
            r.appelFait = true;
            g_enAttente.erase(attente);
            return r;
        }
    }

    // ── GARDE 2 : LA CHARGE, VALIDEE AVANT TOUT ─────────────────────────────────────────────────
    // Frontiere de confiance : ces octets viennent du reseau, et ils vont servir a calculer des
    // adresses d'ecriture dans le tas du jeu.
    std::uint32_t nHead = 0, nBody = 0, nArms = 0;
    std::vector<std::uint64_t> paires;
    std::string pourquoi;
    if (!EsthetiqueV::Decoder(aBlob, nHead, nBody, nArms, paires, pourquoi))
    {
        r.diag = "charge REFUSEE : " + pourquoi;
        return r;
    }
    std::uint32_t nInj = nHead + nBody + nArms;

    // ── SONDE `--tessera-charge-minimale` ───────────────────────────────────────────────────
    // Une seule paire injectee : tout ce qui apparait quand meme sur l'avatar vient d'AILLEURS que
    // de notre charge. C'est la mesure qui manque pour nommer la source du melange (F-PLY-295).
    const bool chargeMinimale = ChargeMinimaleDemandee(GetCommandLineA());
    if (chargeMinimale && nInj > 1)
    {
        nInj = 1;
    }

    // Le record est le NOTRE, jamais celui du serveur (voir `kRecordEnrichi`). Hache une seule fois
    // par appel : `TweakDBID` est un CRC32 du nom, plus la longueur dans les 32 bits hauts.
    //
    // ⭐ ET IL PORTE DESORMAIS LA CARNATION — voir `kCarnations` en tete de fichier.
    // ⛔⛔ SELECTION PAR CARNATION DESACTIVEE — 2026-09-03, apres mesure a l'ecran.
    //
    // Verdict de Lucas : « on a les deux textures qui cohabitent en parallele, ca fait un
    // clignotement — la texture foncee et la texture plus claire qui s'alternent tres rapidement ».
    // Un clignotement entre deux teintes, c'est DEUX CORPS AU MEME ENDROIT : notre `.app` impose
    // une carnation pendant que la charge d'esthetique en applique une autre. Les deux existent.
    //
    // ⭐ CE QUE CA APPREND, ET C'EST LE POINT IMPORTANT : la charge porte DEJA la carnation, sous
    // la forme `<composant>__<meshAppearance>` (F-PLY-196). Le `.app` n'avait donc pas a la
    // decider — il fallait le laisser NEUTRE et laisser la charge parler. C'est l'inverse de ce
    // que ce code faisait.
    //
    // Le record redevient celui de la morphologie seule, le temps de reprendre proprement.
    const std::string record = std::string(aCorpsMasculin ? kRecordEnrichi : kRecordEnrichiFeminin);
    (void)&RecordPourCarnation;
    const RED4ext::TweakDBID recordId(record.c_str());
    const std::uint64_t aRecord = recordId.value;

    // ── LES DEUX OBJETS DU JEU ──────────────────────────────────────────────────────────────────
    auto* pms = SystemeParNom("gamePhotoModeSystem");
    auto* ccs = SystemeParNom("gameuiCharacterCustomizationSystem");
    if (pms == nullptr || ccs == nullptr)
    {
        char b[200];
        std::snprintf(b, sizeof(b), "systeme(s) INJOIGNABLE(S) — photomode=%s customisation=%s",
                      pms != nullptr ? "ok" : "nul", ccs != nullptr ? "ok" : "nul");
        r.diag = b;
        return r;
    }
    RED4ext::Handle<RED4ext::IScriptable> etatHandle;
    if (!Red::CallVirtual(ccs, "GetState", etatHandle) || etatHandle == nullptr)
    {
        r.diag = "l'etat de customisation est INJOIGNABLE (GetState)";
        return r;
    }
    auto* etat = etatHandle.GetPtr();
    if (!EstDeClasse(pms, "gamePhotoModeSystem") || !EstDeClasse(etat, "gameuiCharacterCustomizationState"))
    {
        r.diag = "types inattendus — appel NON tente";
        return r;
    }

    // ── GARDE 3 : LES GARDES DU SPAWNER, RELUES A L'INSTANT ─────────────────────────────────────
    // Jamais supposees depuis la mesure : un spawner desinitialise rendrait 0 en silence, et on
    // chercherait l'erreur dans la requete.
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto spawner = reinterpret_cast<std::uintptr_t>(pms) + kOffsetSpawner;
    if (!EsthetiqueV::Lisible(spawner + 0x50, 0x10))
    {
        r.diag = "spawner ILLISIBLE — appel non tente";
        return r;
    }
    if (*reinterpret_cast<std::uint64_t*>(spawner + 0x50) == 0
        || *reinterpret_cast<std::uint64_t*>(spawner + 0x58) == 0)
    {
        r.diag = "garde du spawner NULLE — appel non tente";
        return r;
    }

    // ── LA RECOLTE, PUIS LA RESERVE ─────────────────────────────────────────────────────────────
    //
    // On recolte d'abord sur le V LOCAL — non pas pour son contenu, qu'on va ecraser, mais pour
    // obtenir un tableau VIVANT, alloue par le moteur, avec sa duree de vie. Fournir notre propre
    // tampon serait parier que le moteur ne le libere pas, et ce pari se paie en crash DIFFERE.
    EsthetiqueV::DynArray charge{reinterpret_cast<void*>(base + EsthetiqueV::RvaSentinelle()), 0, 0};
    std::uint32_t parSection[3] = {0, 0, 0};
    EsthetiqueV::Recolter(etat, base, charge, parSection);
    const std::uint32_t recolte = charge.size;

    // ── ⭐⭐⭐ LES SLOTS DU V LOCAL, COPIES AVANT D'ETRE ECRASES ────────────────────────────────
    //
    // C'est la matiere du correctif de F-PLY-366. L'appel natif construit la tete depuis l'etat de
    // customisation du JEU — celui du joueur local — et nos paires ne font que RECOUVRIR, slot par
    // slot. Tout slot qu'on ne couvre pas laisse donc transparaitre le visage de dessous.
    //
    // Mesure : recolte coupee sur les six groupes ET une seule paire injectee -> la tete du pantin
    // MASCULIN etait 100 % feminine, 6 composants sur 6. Ce n'est pas un residu de tableau, c'est
    // le visage de depart.
    //
    // ⚠️ ON COPIE MAINTENANT, et pas plus tard : l'injection ecrase les `nInj` premieres entrees,
    // donc lire les slots locaux apres coup ne rendrait que les notres.
    std::vector<std::uint64_t> slotsLocaux;
    const bool exhaustive = ChargeExhaustiveDemandee(GetCommandLineA());
    if (exhaustive && recolte > 0)
    {
        const auto src = reinterpret_cast<std::uintptr_t>(charge.entries);
        if (EsthetiqueV::Lisible(src, static_cast<std::size_t>(recolte) * kTaillePaire))
        {
            slotsLocaux.reserve(recolte);
            for (std::uint32_t i = 0; i < recolte; ++i)
            {
                slotsLocaux.push_back(
                    *reinterpret_cast<std::uint64_t*>(src + static_cast<std::size_t>(i) * kTaillePaire));
            }
        }
    }

    // ⭐ LA PARADE AU « MUR DE LA CAPACITE » (F-PLY-267).
    //
    // Sans elle, on ne pourrait injecter que ce qui tient dans la capacite recoltee sur le joueur
    // LOCAL — et un joueur masculin ne pourrait pas afficher une voisine (un V feminin porte des
    // paires `breast` qu'un V masculin n'a pas). Les producteurs reservent la valeur EXACTE, sans
    // marge : la capacite finit collee au nombre de paires recoltees. Compter sur du mou serait un
    // pari perdant.
    //
    // ⚠️ TROIS PRECAUTIONS, toutes lues dans le binaire :
    //   · `Reserve` agit des que la capacite demandee DIFFERE de la courante — donc il sait aussi
    //     RETRECIR. On ne l'appelle que pour agrandir.
    //   · Il REALLOUE : `entries` change, il faut le relire. Ecrire dans l'ancien pointeur, c'est
    //     ecrire dans un tampon libere.
    //   · Le 5e parametre est un rappel de deplacement/destruction ; les producteurs passent 0 pour
    //     ces paires POD, on passe 0 aussi.
    // ⚠️ La reserve doit tenir compte des paires de NEUTRALISATION qu'on ajoutera peut-etre : au
    // pire une par slot local. Reserver apres coup obligerait a une seconde reallocation, et
    // `Reserve` REALLOUE (le pointeur change) — deux fois, c'est deux occasions de se tromper.
    const std::uint32_t besoin = exhaustive
        ? nInj + static_cast<std::uint32_t>(slotsLocaux.size())
        : nInj;
    if (besoin > charge.capacity)
    {
        const auto reserver = reinterpret_cast<Reserve_t>(base + kRvaReserve);
        reserver(&charge, besoin, kTaillePaire, kAlignementPaire, nullptr);
        if (charge.capacity < nInj)
        {
            char b[220];
            std::snprintf(b, sizeof(b),
                          "Reserve N'A PAS mordu : %u demandees, capacite %u — appel NON tente",
                          nInj, charge.capacity);
            r.diag = b;
            return r;
        }
    }

    // ── L'INJECTION ─────────────────────────────────────────────────────────────────────────────
    // `entries` est relu MAINTENANT, apres la reserve : c'est tout l'objet de la precaution
    // ci-dessus, et l'oublier serait une ecriture dans de la memoire liberee.
    const auto ou = reinterpret_cast<std::uintptr_t>(charge.entries);
    if (!EsthetiqueV::Lisible(ou, static_cast<std::size_t>(nInj) * kTaillePaire))
    {
        r.diag = "le tableau n'est pas lisible sur toute la taille a injecter — appel NON tente";
        return r;
    }
    for (std::uint32_t i = 0; i < nInj; ++i)
    {
        *reinterpret_cast<std::uint64_t*>(ou + static_cast<std::size_t>(i) * kTaillePaire) = paires[i * 2];
        *reinterpret_cast<std::uint64_t*>(ou + static_cast<std::size_t>(i) * kTaillePaire + 8) =
            paires[i * 2 + 1];
    }
    charge.size = nInj;

    std::string diagResidu;

    // ── ⭐⭐⭐ LA NEUTRALISATION — le correctif de F-PLY-366 ──────────────────────────────────────
    //
    // Pour chaque slot que le V LOCAL occupe et que NOTRE charge ne couvre pas, on ecrit
    // explicitement une valeur neutre. Sans ca, le visage de dessous transparait exactement la —
    // c'est la coiffure et le maquillage de l'autre joueur sur le pantin, mesures le 2026-09-02.
    //
    // ⚠️ `0` COMME VALEUR NEUTRE EST L'HYPOTHESE DE CETTE SONDE, pas un fait. Si le moteur ne lit
    // pas `0` comme « rien », le relevé le dira au premier lancement : soit les composants
    // etrangers restent, soit d'autres disparaissent. Une question a un lancement.
    if (exhaustive && !slotsLocaux.empty())
    {
        std::uint32_t neutralisees = 0;
        for (const std::uint64_t slot : slotsLocaux)
        {
            // Deja couvert par nous ? Alors il n'y a rien a neutraliser — notre valeur gagne.
            bool couvert = false;
            for (std::uint32_t i = 0; i < nInj && !couvert; ++i)
            {
                couvert = (paires[i * 2] == slot);
            }
            if (couvert)
            {
                continue;
            }
            // ⚠️ La capacite a ete reservee pour ce cas (`besoin`), mais on REVERIFIE : une reserve
            // qui a echoue silencieusement ferait ecrire hors du tampon, et ce genre d'erreur ne se
            // manifeste pas ici — elle se manifeste ailleurs, plus tard, en crash inexplicable.
            if (charge.size >= charge.capacity)
            {
                break;
            }
            const auto d = ou + static_cast<std::size_t>(charge.size) * kTaillePaire;
            if (!EsthetiqueV::Lisible(d, kTaillePaire))
            {
                break;
            }
            *reinterpret_cast<std::uint64_t*>(d) = slot;
            *reinterpret_cast<std::uint64_t*>(d + 8) = 0ull;
            ++charge.size;
            ++neutralisees;
        }
        diagResidu += " [exhaustive : " + std::to_string(neutralisees) + " slot(s) neutralise(s) sur "
                    + std::to_string(slotsLocaux.size()) + " locaux, total " + std::to_string(charge.size) + "]";
    }

    // ── SONDE `--tessera-aligner-sections` ──────────────────────────────────────────────────
    //
    // ⭐ On vient d'ecrire nos `nInj` paires A LA FILE. Or le consommateur redecoupe
    // vraisemblablement aux bornes du joueur LOCAL (`parSection`), pas aux notres — d'ou un
    // melange de designs entre les deux personnages, decrit par Lucas le 2026-08-29 et predit
    // par le commentaire du diagnostic depuis le 2026-08-24.
    //
    // Cette sonde REECRIT le tableau en placant chaque section a l'offset LOCAL, et laisse a
    // zero ce qui depasse. Le total occupe devient celui du joueur local, pas le notre.
    if (AlignerSectionsDemande(GetCommandLineA()))
    {
        const std::uint32_t local = parSection[0] + parSection[1] + parSection[2];
        if (local <= charge.capacity && local > 0)
        {
            // On repart d'un tableau propre : les positions non ecrites doivent etre NULLES,
            // sinon on melange l'ancien desalignement au nouveau.
            const auto total = static_cast<std::size_t>(charge.capacity) * kTaillePaire;
            if (EsthetiqueV::Lisible(ou, total))
            {
                std::memset(reinterpret_cast<void*>(ou), 0, total);
                const std::uint32_t nOtre[3] = {nHead, nBody, nArms};
                std::uint32_t srcBase = 0, dstBase = 0;
                for (int s = 0; s < 3; ++s)
                {
                    const std::uint32_t n = nOtre[s] < parSection[s] ? nOtre[s] : parSection[s];
                    for (std::uint32_t k = 0; k < n; ++k)
                    {
                        const auto d = ou + static_cast<std::size_t>(dstBase + k) * kTaillePaire;
                        *reinterpret_cast<std::uint64_t*>(d) = paires[(srcBase + k) * 2];
                        *reinterpret_cast<std::uint64_t*>(d + 8) = paires[(srcBase + k) * 2 + 1];
                    }
                    srcBase += nOtre[s];
                    dstBase += parSection[s];
                }
                charge.size = local;
                diagResidu += " [sections alignees " + std::to_string(nHead) + "+"
                            + std::to_string(nBody) + "+" + std::to_string(nArms) + " -> "
                            + std::to_string(parSection[0]) + "+" + std::to_string(parSection[1])
                            + "+" + std::to_string(parSection[2]) + "]";
            }
            else
            {
                diagResidu += " [alignement REFUSE — tableau illisible]";
            }
        }
        else
        {
            diagResidu += " [alignement REFUSE — capacite " + std::to_string(charge.capacity)
                        + " < local " + std::to_string(local) + "]";
        }
    }

    // ── SONDE `--tessera-effacer-residu` ────────────────────────────────────────────────────
    //
    // ⚠️ CE QUE `size` NE GARANTIT PAS. La recolte a rempli le tableau avec l'esthetique du V
    // LOCAL ; on n'ecrase que les `nInj` premieres entrees. Au-dela, les paires du joueur local
    // SURVIVENT — mesure reelle : « 18 paire(s), recolte 23 » en laisse cinq derriere `size`.
    //
    // Poser `size` suppose que le moteur ne lit rien au-dela. F-PLY-348 a mesure qu'il reste
    // TROIS doublons meme avec `--tessera-sans-recolte` actif : quelque chose relit ce qu'on
    // croyait invisible. Cette sonde teste la lecture la plus simple — le residu lui-meme.
    //
    // ⭐ SONDE ET NON CORRECTIF, pour que l'A/B tienne dans une seule session : sans le drapeau
    // le comportement est inchange, avec lui le residu est efface. Un correctif pose d'office
    // rendrait la comparaison impossible.
    if (EffacerResiduDemande(GetCommandLineA()) && charge.capacity > nInj)
    {
        const auto residu = static_cast<std::size_t>(charge.capacity - nInj) * kTaillePaire;
        const auto debut = ou + static_cast<std::size_t>(nInj) * kTaillePaire;
        if (EsthetiqueV::Lisible(debut, residu))
        {
            std::memset(reinterpret_cast<void*>(debut), 0, residu);
            diagResidu = " [residu efface " + std::to_string(charge.capacity - nInj) + " paire(s)]";
        }
        else
        {
            diagResidu = " [residu NON efface — plage illisible]";
        }
    }

    // ── LA REQUETE ──────────────────────────────────────────────────────────────────────────────
    // Zeroee d'abord : les champs qu'on ne renseigne pas doivent etre nuls et non remplis de pile
    // sale — c'est ce qui rend un echec interpretable.
    alignas(16) std::uint8_t requete[kTailleRequete] = {};
    auto* f32 = reinterpret_cast<float*>(&requete[0]);
    f32[0x10 / 4] = aPosition.X;
    f32[0x14 / 4] = aPosition.Y;
    f32[0x18 / 4] = aPosition.Z;
    // ⚠️ Rotation NEUTRE : (0,0,0,1). Un quaternion (0,0,0,0) serait DEGENERE, pas neutre — c'est
    // l'erreur que F-PLY-195 a corrigee.
    f32[0x20 / 4] = 0.f;
    f32[0x24 / 4] = 0.f;
    f32[0x28 / 4] = 0.f;
    f32[0x2C / 4] = 1.f;
    requete[0xE9] = 1;
    requete[0xEA] = 1;

    // ── SONDE `--tessera-drapeaux=E8:1,EA:0` ────────────────────────────────────────────────
    // Ecrase des octets APRES les valeurs par defaut, pour les faire varier un par un sans
    // reconstruire. La garde de borne est ICI : le parseur ne connait pas la taille de la requete.
    std::string diagDrapeaux = diagResidu;
    for (const auto& [offset, valeur] : DrapeauxRequete(GetCommandLineA()))
    {
        if (offset >= kTailleRequete)
        {
            continue;
        }
        requete[offset] = valeur;
        char b[32];
        std::snprintf(b, sizeof(b), " %02zX=%u", offset, static_cast<unsigned>(valeur));
        diagDrapeaux += b;
    }

    // ── L'ETAT DU MONDE AVANT L'APPEL ───────────────────────────────────────────────────────────
    // On photographie les corps de ce record AVANT, pour retrouver le neuf par difference. Sans
    // cette photo, un corps deja present serait pris pour celui qu'on vient de faire naitre.
    Releve avant;
    Relever(aRecord, avant);

    // ── L'APPEL ─────────────────────────────────────────────────────────────────────────────────
    // ── ⭐ SONDE `--tessera-sans-recolte` : FAIRE TAIRE LE V LOCAL PENDANT L'APPEL ──────────
    //
    // Le spawner ne recoit aucun pointeur d'etat (F-PLY-317) : il lit le singleton, et c'est de la
    // que vient la contamination (F-PLY-296). On met donc a ZERO le compte de chaque groupe juste
    // avant l'appel, et on le restaure juste apres.
    //
    // ⚠️ La disposition a ete LUE AU DESASSEMBLEUR avant d'ecrire (ADR 0034) :
    //     desc = localisateur(etat, etat + conteneur, hashDuGroupe, 0)   // RVA 0x38582C
    //     *(uint*)(desc + 0x14) = le COMPTE      (entrees de 0x28 octets, nom en +0x00)
    //
    // ⚠️ RESTAURATION INCONDITIONNELLE. Elle vit dans un objet a duree de vie liee au bloc : si
    // l'appel natif jette ou sort par un chemin qu'on n'a pas prevu, le compte est remis. Laisser
    // un groupe a zero abimerait le V du joueur LOCAL, pas seulement l'avatar.
    struct RecolteEteinte
    {
        std::vector<std::pair<std::uint32_t*, std::uint32_t>> sauves;
        ~RecolteEteinte()
        {
            for (auto& [ou, valeur] : sauves)
            {
                *ou = valeur;
            }
        }
    } recolteEteinte;

    if (SansRecolteDemandee(GetCommandLineA()) && etat != nullptr)
    {
        using Localiser_t = std::uintptr_t (*)(std::uintptr_t, void*, std::uint64_t, std::uint64_t);
        const auto localiser = reinterpret_cast<Localiser_t>(base + 0x38582Cull);

        // ⚠️ TROIS COMPTEURS, PAS UN. Le premier jet n'imprimait que les succes — et « 0 groupe(s) »
        // recouvrait alors trois pannes sans rapport : table de groupes vide, localisateur qui rend
        // 0, ou garde de lisibilite qui rejette. Mesure le 2026-08-26 : la sonde a tire sur
        // 0 groupe(s), et le diagnostic ne permettait pas de dire laquelle des trois.
        std::size_t vus = 0, sansDesc = 0, illisibles = 0;
        for (const auto& g : EsthetiqueV::Groupes())
        {
            ++vus;
            auto* conteneur = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(etat) + g.conteneur);
            const auto desc = localiser(reinterpret_cast<std::uintptr_t>(etat), conteneur,
                                        RED4ext::CName(g.nom).hash, 0ull);
            if (desc == 0)
            {
                ++sansDesc;
                continue;
            }
            if (!EsthetiqueV::Lisible(desc + 0x14, sizeof(std::uint32_t)))
            {
                ++illisibles;
                continue;
            }
            auto* compte = reinterpret_cast<std::uint32_t*>(desc + 0x14);
            recolteEteinte.sauves.emplace_back(compte, *compte);
            *compte = 0;
        }
        diagDrapeaux += " [sans-recolte " + std::to_string(recolteEteinte.sauves.size())
                      + "/" + std::to_string(vus) + " groupe(s)"
                      + " sansDesc=" + std::to_string(sansDesc)
                      + " illisibles=" + std::to_string(illisibles) + "]";
    }

    const auto fn = reinterpret_cast<SpawnEnrichi_t>(base + kRvaSpawnEnrichi);
    void* sortie[2] = {nullptr, nullptr};
    fn(reinterpret_cast<void*>(spawner), &sortie[0], &requete[0], aRecord, &charge);
    r.appelFait = true;
    // ⚠️ `sortie[0]` N'EST PAS DEREFERENCE, ET CE N'EST PAS UNE PRECAUTION DE STYLE. Une version de
    // la sonde l'a traite comme un objet de script le 2026-08-21 : le type s'est lu `None` (un
    // `CName` de hachage nul) et le jeu est mort dans la foulee (F-PLY-225). « Lisible » n'est pas
    // « c'est un objet de la classe que je crois ».

    // ── ⭐⭐ LIRE CE QUE L'APPEL REND — PRUDEMMENT, ET SANS JAMAIS L'APPELER ────────────────────
    //
    // POURQUOI ON EN ARRIVE LA. L'enumeration par `GetEntitiesAroundObject` ne trouve jamais ce
    // corps : le journal rend invariablement `autour 128->128`, avant comme apres. Le script
    // decompile de CDPR explique pourquoi — cette fonction passe par le **systeme de ciblage**
    // (`GetTargetParts`) et ne rend que les entites porteuses d'un `TargetingComponent`. Le total
    // ne bouge pas d'un iota, ce qui ressemble fort a une **saturation** : dans une rue peuplee,
    // les 128 places sont prises avant que notre corps n'arrive.
    //
    // ⚠️⚠️ CE BLOC NE FAIT QUE LIRE, ET LA DISTINCTION EST VITALE. Le 2026-08-21, une version de la
    // sonde a traite ce meme pointeur comme un objet de script : elle a lu son type (`None`, un
    // `CName` de hachage nul), puis APPELE `GetFunction` dessus — et le jeu est mort (F-PLY-225).
    //
    // La lecon exacte de cet incident n'est pas « ne touche pas a ce pointeur », c'est **« lisible »
    // n'est pas « c'est un objet de la classe que je crois »**. Une lecture gardee par `Lisible`
    // est sure : elle verifie que la page est mappee. Ce qui a tue le jeu, c'est l'APPEL derriere.
    // On journalise donc des octets, on n'invoque rien, et on ne decide rien sur cette base.
    if (sortie[0] != nullptr && EsthetiqueV::Lisible(reinterpret_cast<std::uintptr_t>(sortie[0]), 0x60))
    {
        const auto* mots = reinterpret_cast<const std::uint64_t*>(sortie[0]);
        char d[400];
        std::snprintf(d, sizeof(d),
                      "[retour enrichi] +00=%llX +08=%llX +10=%llX +18=%llX +20=%llX "
                      "+28=%llX +30=%llX +38=%llX +40=%llX +48=%llX +50=%llX +58=%llX",
                      (unsigned long long)mots[0], (unsigned long long)mots[1],
                      (unsigned long long)mots[2], (unsigned long long)mots[3],
                      (unsigned long long)mots[4], (unsigned long long)mots[5],
                      (unsigned long long)mots[6], (unsigned long long)mots[7],
                      (unsigned long long)mots[8], (unsigned long long)mots[9],
                      (unsigned long long)mots[10], (unsigned long long)mots[11]);
        SDK->logger->InfoF(PLUGIN, "%s", d);

        // ── ⭐ DE QUELLE CLASSE EST CET OBJET ? LA VTABLE LE DIT ─────────────────────────────
        //
        // On tient un pointeur dont on sait deux choses : `+0x00` pointe dans l'executable (donc
        // c'est une table de methodes), et `+0x110` porte un EntityID exploitable. Ce n'est PAS une
        // `ent::Entity` — celle-ci porte son `entityID` en `+0x48`. C'est donc autre chose, et
        // savoir QUOI change tout : si c'est un objet de jeu, le teleport du moteur le prend
        // directement en HANDLE, sans jamais passer par une resolution d'identifiant.
        //
        // ⚠️ ON NE DEMANDE PAS SON TYPE AU JEU. `GetType()` sur un pointeur dont on ignore la
        // nature est precisement ce qui a mene au crash du 2026-08-21 (F-PLY-225) : le type s'est
        // lu `None`, et l'appel suivant a parcouru de la memoire arbitraire.
        //
        // On journalise donc l'ADRESSE de la vtable, en RVA — c'est-a-dire dans le repere de
        // Ghidra. La classe se resout alors HORS DU JEU, sans rien risquer, exactement comme
        // `Reserve` a ete identifie ce matin par l'assertion de CDPR.
        {
            const auto vt = *reinterpret_cast<const std::uintptr_t*>(sortie[0]);
            SDK->logger->InfoF(PLUGIN,
                               "[retour enrichi] vtable=0x%llX · base=0x%llX · RVA=0x%llX "
                               "(a resoudre dans Ghidra)",
                               (unsigned long long)vt, (unsigned long long)base,
                               (unsigned long long)(vt - base));
        }
        // ⚠️ ET LE TEMOIN, dans la meme trace. Sans lui, ces douze nombres ne se comparent a rien.
        // `sortie[1]` est le second mot du tampon de sortie : le gabarit natif ecrit parfois une
        // paire. On le dit meme s'il est nul — un zero OBSERVE vaut mieux qu'un silence.
        SDK->logger->InfoF(PLUGIN, "[retour enrichi] sortie[1]=%llX (temoin)",
                           reinterpret_cast<unsigned long long>(sortie[1]));
    }
    else if (sortie[0] != nullptr)
    {
        SDK->logger->InfoF(PLUGIN, "[retour enrichi] 0x%llX ILLISIBLE sur 0x60 octets — ce n'est pas "
                                   "un objet en memoire commitee",
                           reinterpret_cast<unsigned long long>(sortie[0]));
    }

    // ── ON NE CHERCHE PAS MAINTENANT : ON PREND RENDEZ-VOUS ─────────────────────────────────────
    //
    // ⭐⭐ Mesure du 2026-08-23, 17:20 : le journal rendait `autour 128->128`. L'enumeration voyait
    // 128 entites — elle fonctionnait donc parfaitement — et RIEN de neuf apres l'appel. Pendant ce
    // temps Lucas voyait le corps a l'ecran.
    //
    // Les deux faits ne se contredisent pas : **la creation d'entite est ASYNCHRONE**. Chercher
    // dans la meme frame que l'appel, c'est chercher un corps qui n'est pas encore ne. C'est aussi
    // pourquoi la sonde, elle, les trouve depuis toujours : elle enumere par une commande separee,
    // des secondes plus tard.
    //
    // On enregistre donc l'etat d'avant, et l'appelant — qui repasse a chaque snapshot tant que
    // l'entite manque — fera la difference aux passages suivants (bloc en tete de cette fonction).
    {
        EnAttente a;
        a.avant = std::move(avant.notres);
        a.position = aPosition;
        a.appele = true;
        a.passages = 0;
        a.retour = sortie[0];
        g_enAttente[aNetworkId] = std::move(a);
    }
    // ⭐ LA REPARTITION DU JOUEUR LOCAL EST DESORMAIS DITE, a cote de la notre.
    //
    // `parSection` etait calcule par `Recolter` puis JETE. Or c'est exactement l'instrument dont on
    // a besoin : la recolte concatene SIX groupes lus a des offsets fixes de l'etat (0x70 tete,
    // 0x80 corps, 0x90 bras), et on ecrase le tableau resultant par NOS paires — sans jamais dire
    // au moteur que la repartition a change.
    //
    // Hypothese (2026-08-24, NON MESUREE) : si le consommateur redecoupe par groupe en se fiant a
    // la repartition du joueur LOCAL, nos paires sont attribuees aux mauvaises sections — et on
    // obtient un melange entre deux personnages, ce que Lucas decrit : « la coiffure de REDDA PLUS
    // la sienne » sur LUCAS1.
    //
    // Ce que la ligne tranche : si `(nHead+nBody+nArms)` differe de `parSection`, la repartition
    // n'est pas celle qu'on croit. Deux nombres cote a cote, zero lancement de plus.
    char b[380];
    std::snprintf(b, sizeof(b),
                  "appel enrichi PARTI%s — %u paire(s) (%u+%u+%u), recolte %u (local %u+%u+%u), "
                  "capacite %u · %u entite(s) autour avant · retour 0x%llX. Recherche du corps aux "
                  "passages suivants (creation asynchrone).",
                  (std::string(aCorpsMasculin ? "" : " [feminin]")
                   + (chargeMinimale ? " [SONDE charge minimale]" : "")
                   + (diagDrapeaux.empty() ? "" : " [drapeaux" + diagDrapeaux + "]")).c_str(),
                  nInj, nHead, nBody, nArms, recolte,
                  parSection[0], parSection[1], parSection[2], charge.capacity,
                  static_cast<unsigned>(g_enAttente[aNetworkId].avant.size()),
                  reinterpret_cast<unsigned long long>(sortie[0]));
    r.diag = b;
    r.attente = true;   // l'appel est parti : on cherchera aux passages suivants
    return r;
}
}  // namespace Tessera::SpawnEnrichi

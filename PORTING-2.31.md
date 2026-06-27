# Port Cyberverse → Cyberpunk 2077 **2.31** (TesseraSynth)

Fork du framework Cyberverse (upstream `TDUniverse/Cyberverse`, MIT, ciblant le jeu **2.1**),
porté vers **2.31** pour le projet **TesseraSynth**. On ne réutilise que le **client** (plugin
RED4ext + redscript) et ses patterns d'intégration jeu ; le serveur et la sérialisation sont
remplacés par notre stack (serveur **Rust** + protocole **FlatBuffers**).

> Décision : porter (pas reconstruire), couplage version quasi nul (RTTI-par-nom, zéro offset).
> Branche de travail : `port/2.31`.

## Baseline de modding 2.31 (versions gelées)

RED4ext **1.30.0** · redscript **0.5.31** · Codeware **1.20.3** · CET **1.37.1**.
Jeu : CP2077 **2.31** (GOG + Phantom Liberty). Prérequis build : Visual C++ Redistributable 2022 (x64).

## Build sur Windows

```powershell
git clone --recursive git@github.com:The-Genium007/Cyberverse.git
cd Cyberverse
git checkout port/2.31
git submodule update --init --recursive
# Bumper RED4ext.SDK + red-lib vers leur version compatible 2.31 (submodules)
# puis :
cmake --preset <preset windows>   # vcpkg + Ninja, voir CMakePresets.json
cmake --build --preset <preset>
```

Déployer le `.dll` RED4ext + le module redscript sur l'install 2.31, lancer, **documenter ce qui
charge / casse**. Points à auditer (couplage version) :
- ~19 appels RTTI-par-nom (`client/red4ext/src/Utils.h`, `NetworkGameSystem.cpp`) ;
- hooks redscript (`client/RedscriptModule/src/Cyberverse.reds`) :
  `SingleplayerMenuGameController`, `PlayerPuppet::OnAction/OnItemEquipped`, `BaseProjectile::OnShoot` ;
- layouts d'event structs (`PlayerActionTracker.cpp`).

## Brancher notre transport/protocole (remplacer zpp_bits)

La sérialisation est isolée — point de couture unique :
- `EnqueueMessage(...)` → encoder un **`ClientEnvelope` FlatBuffers** au lieu du `MessageFrame` zpp.
- `PollIncomingMessages()` → décoder un **`ServerEnvelope`** et router `Snapshot`.
- **Conserver tel quel** : connexion **GNS** (`ConnectByIPAddress`), boucle dans le tick `IGameSystem`,
  helpers RTTI de `Utils.h`.

### Contrat fil (FlatBuffers, namespace `cyberpunk_rp.protocol`)

```fbs
struct Vec3 { x:float; y:float; z:float; }
table Join { display_name:string; }
table PositionUpdate { position:Vec3; yaw:float; }
table PlayerState { id:ulong; position:Vec3; yaw:float; }
table Snapshot { tick:ulong; players:[PlayerState]; }
union ClientMsg { Join, PositionUpdate }   // root ClientEnvelope{ msg }
union ServerMsg { Snapshot }               // root ServerEnvelope{ msg }
```

Générer les en-têtes C++ : `flatc --cpp protocol.fbs` (flatc **25.12.19**).

### Flux (tranche verticale 0-D)

1. Connexion GNS au serveur Rust (défaut `127.0.0.1:27020`, IP injectée en ligne de commande).
2. À la connexion : envoyer `ClientEnvelope(Join{display_name})`.
3. Chaque tick : envoyer `ClientEnvelope(PositionUpdate{position, yaw})` (position locale via `Utils.h`).
4. À réception d'un `Snapshot` (serveur à 20 Hz) : pour chaque `PlayerState` →
   spawn (id inconnu) / update-pose (id connu) / despawn (id disparu). Le serveur **exclut déjà
   le viewer** du snapshot → pas besoin de connaître son propre id pour ce slice.

**Réussite 0-D** : deux jeux connectés au serveur Rust, chacun voit l'avatar de l'autre bouger.

## Licence

Upstream MIT (voir `LICENSE`). Conserver l'attribution Cyberverse/TDUniverse dans les fichiers
réutilisés.

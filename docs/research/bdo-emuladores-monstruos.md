# Monstruos y NPCs en los emuladores y la documentación pública de BDO

Fecha: 2026-10-03. Los nombres y opcodes de Crimson Desert (CD) son de la build **1.0.0.2976**
(PE `0x6AB28F00`) y salen de `build/research/troc_table_session.txt`,
`troc_table_client.txt` y `docs/research/data/troc_messages_2976.csv`. Este informe no
escribe en el juego ni toca `D:\SteamLibrary`.

Leyenda: **[fuente]** = dicho en la fuente enlazada; **[inferido]** = deducción nuestra (de
nombres, de cómo encajan con CD o de varias fuentes juntas). Lo que no se encontró está en
la §8.

---

## 0. Resumen

1. **No hay ningún emulador de BDO abierto, legal y avanzado que implemente monstruos.**
   - Los emuladores originales y públicos (DesertProject en C#, la base .NET Core de Taiga,
     Illuminate en C++) llegan, como mucho, a entrar en el mundo. Según su propio hilo, el
     de C# no tenía "geodata, defined spawns, scripts" [fuente].
   - Los que sí tenían monstruos (el emulador Java "BDOEmu/OgreFest" y los "server files"
     oficiales Nova, Corsair, Guardian…) son **código filtrado**. No se han usado (§1.3).
   - Lo aprovechable son **nombres de mensajes**, enumeraciones, datos de aparición y
     descripciones de foros, más la documentación oficial de jugabilidad.
2. **El esquema de nombres es el mismo que en CD** [fuente + inferido]:
   - el cliente de BDO llama a los mensajes `Tr…Req`, `Tr…Ack`, `Tr…Acks` y `Tr…Nak`;
   - los emuladores los rebautizan `CM…` (= `Req`) y `SM…` (= `Ack`/`Acks`);
   - en CD son `TrocTr…Req/Ack/Nak`.

   Varios mensajes de monstruos coinciden casi letra por letra: `SMRemoveActor` ↔
   `RemoveActorAck`, `SMStartAction` ↔ `StartActionAck`, `SMApplyActionRestriction` ↔
   `ApplyActionRestrictionAck`, `CMHeartbeat 0x3E9` ↔ `HeartbeatReq`. Tabla en la §2.
3. **Diferencia clave: en BDO los monstruos los decide y los mueve el servidor; en CD, el
   cliente** [inferido, con apoyo fuerte en los nombres]:
   - BDO tiene `SMMoveNonPlayer` (S→C) y ningún `CM…` de movimiento de monstruos: solo
     `CMMovePlayer`;
   - las apariciones y reapariciones se gestionan con comandos de GM del lado servidor
     (`CMDelaySpawnMonster`, `CMSetImmediatelyRespawnMonster`, `CMDontSpawnForManager`,
     `CMChannelSpawnBlock`);
   - los datos del cliente de BDO **no contenían las posiciones de los monstruos**, solo las
     de los NPC fijos [fuente].

   En CD es al revés: el cliente pide los spawns (`ActivateSpawningPoolReq`…), corre la IA
   (`ChangeAIStateReq`) y mueve a los NPC (`MoveActorReq`). CD conserva el protocolo de BDO,
   pero con el modelo de "autoridad del controlador" que ya vimos en `combate-coop.md` §2.1.
4. **Lo que confirma la documentación oficial de BDO** sobre varios jugadores
   [fuente]:
   - límite de **10 monstruos con aggro por personaje** (antes 7);
   - las invocaciones tienen su propio límite;
   - en grupo hay cuatro modos de botín (libre, por orden, aleatorio, solo líder). En
     "libre", la tirada de botín usa los bonus de **quien mata**;
   - en los jefes del mundo, para optar al botín hay que hacer al menos un **0,01% del HP**
     del jefe.
5. **Para nuestra opción A (autoridad del host)**, BDO aporta tres patrones:
   - un **mensaje de alta por tipo de actor** (`SMAddMonsters`, `SMAddNpcs`,
     `SMAddDeadBodys`…), que en CD es un único `ActorLoadDataAck`;
   - **retirada con motivo** (`ERemoveActorType`: `OutOfRange`, `DespawnMonster`,
     `DespawnDeadBody`, `Teleport`, `Logout`…);
   - un **cadáver como entidad propia** con su botín.

---

## 1. Fuentes

### 1.1 Repositorios y ficheros útiles

| Fuente | Licencia | Fechas | Versión de BDO | Qué aporta aquí |
|---|---|---|---|---|
| **Illuminate** (CoolManBob), C++/POCO/MySQL: https://github.com/CoolManBob/Illuminate | **Sin licencia** (todos los derechos reservados: solo describir conceptos) | creado 2021-03-08, último push 2021-08-09 | No documentada | Servidores Login/World/Channel. Enumeraciones `ERemoveActorType`, `EAIState`, `EAIDestination`, `ESpawnType`. Ficheros de aparición en Lua **por celda** (`scripts/Channel/spawns/monster/0_0_-12_monster.lua`…) con registros `{posición, dirección, characterKey, cantidad, radio, índice de acción, índice de diálogo, función}`. Ver la nota de abajo. |
| **Yurdead/BDO_661** `packets.xml`: https://github.com/Yurdead/BDO_661 | GPL-3.0 | 2019-05-28 | cliente 661 (≈2017-18) | Lista de ~1519 nombres `CM*`/`SM*` con opcode. **Procedencia:** es la tabla de protocolo del emulador Java filtrado (§1.3). Solo usamos los **nombres**, que según el hilo de 2018 son los del cliente (§2.1) |
| **Yurdead/819-packets-xml-**: https://github.com/Yurdead/819-packets-xml- | GPL-3.0 | 2021-08-30 → 2021-10-31 | 819 "Nova/Hashashin" | ~110 nombres con opcode. Mismo origen y la misma reserva. Sirve para comparar opcodes entre versiones (§2.3) |
| **ychwu/bdo-toolkit**: https://github.com/ychwu/bdo-toolkit | MIT | 2026-07-07 → 2026-10-01 | BDO vivo NA/EU | Analizador pasivo. Confirma la cabecera `u16 longitud, u8 flags, u16 opcode`. Solo trata objetos y botín ("Mob Drop"), con opcodes calibrados por el usuario. **No nombra mensajes de actores** |
| iblazys/bdohook: https://github.com/iblazys/bdohook | sin licencia | 2019-03 | GamezBD 972 | Solo un DLL compilado y `opcodes.dat`, sin fuente. No aporta nada |

**Nota sobre Illuminate.** Revisé su estructura, los enums y una muestra de los datos de
aparición. No se leyó su código C++ del
servidor Channel. Por eso **no sé cómo implementa** la aparición, la difusión ni el combate:
solo sé lo que muestran los nombres de fichero y los datos. El código es de 2021 y está
incompleto (29 commits).

### 1.2 Hilos, notas y documentación

| Fuente | Fecha | Versión | Qué aporta |
|---|---|---|---|
| RaGEZONE "[Development] Black Desert Emulator" (archivado): http://web.archive.org/web/2023/https://forum.ragezone.com/f1000/black-desert-emulator-1085129/index5.html | 2015-12 | KR → EU | "Hay **solo** posiciones estáticas de NPC; las de los mobs no están en el fichero, así que hará falta un sniffer" [fuente] |
| RaGEZONE "Black Desert online SERVER Emulator Source" (archivado): http://web.archive.org/web/2023/https://forum.ragezone.com/f1000/black-desert-online-server-emulator-1001211/index16.html y `index17.html` | 2014 → 2017-09 | cliente "v97" (2014), DesertProject C# | El emulador "solo deja entrar y caminar". No hay NPC ni mobs porque "no hay geodata, spawns definidos, scripts ni bases de datos". Para tenerlos hay que hacer ingeniería inversa de "los paquetes de aparición de monstruos y NPC" [fuente] |
| RaGEZONE "OpCode and Func dump from kr2100 (OF) and 2179" (archivado): http://web.archive.org/web/2018/http://forum.ragezone.com:80/f1000/opcode-func-dump-kr2100-2179-a-1152798/ | 2018-08 | KR 2100 y 2179 | Tabla de mensajes del cliente. Nombres `Tr…Ack/Acks/Nak` y `eTr…Req`, y la equivalencia `CM = Req`, `SM = Ack/Acks`, `SMxxxNak = Nak`. Los parsers tienen la forma `process(ActorRoot*, u8* datos, u16 tamaño)`. Alguien comenta que los opcodes se barajan en cada versión [fuente] |
| RaGEZONE "BDO .NET Core Emulator Base" (Taiga, archivado): http://web.archive.org/web/2018/http://forum.ragezone.com:80/f1000/bdo-net-core-emulator-base-1146115/ | 2018-03 | cliente de OgreFest | Base original, "hard-coded", que llega a la lista de servidores. Nada de monstruos |
| BDO, notas del parche KR (Inven Global): https://www.invenglobal.com/articles/6795/bdo-kr-patch-notes-november-21st-changes-on-world-bosses-more-guild-skills-added | 2018-11-20 | KR | "El número máximo de monstruos que un personaje puede provocar pasa de 7 a 10". Los "objetos invocados" provocan 1 como máximo y las "criaturas invocadas", 4 [fuente] |
| Foro oficial NA/EU, opciones de botín de grupo: https://www.naeu.playblackdesert.com/en-us/Forum/ForumTopic/Detail?_topicNo=58247&_opinionNo=211641 | 2024-07-19 | NA/EU | En "libre", el botín se genera al morir el monstruo con los bonus de quien lo mató. En "por orden", se genera al recogerlo y se reparte por turnos [fuente] |
| Wiki oficial, World Bosses: https://www.naeu.playblackdesert.com/en-US/Wiki?wikiNo=83 | vivo | NA/EU | Hay que hacer un 0,01% del HP para optar al botín, con un "Damage pool" y un "Random pool" [fuente] |
| NDC 2021 "SPICA" (resumen): https://velog.io/@wsong0101/NDC-2021-정리-SPICA-UE4에서-대규모-몬스터가-나오는-PvE-게임을-위한-하이브리드-데디케이티드-서버 | 2021 | no es BDO | El servidor **delega actores en un cliente "host"**, que le devuelve su estado periódicamente. Si el host se va, los reasigna a otro, y sin host los procesa él mismo. Solo el servidor crea actores [fuente]. Es el mismo modelo que el "dueño/controlador" de CD |

### 1.3 Descartadas por ser código filtrado (no se abrieron)

- **Emulador Java "BDOEmu"/OgreFest** (hilo RaGEZONE 1142876; `freedDog/bdoemu` en GitHub): el
  primer mensaje dice que es "un repost" de ficheros de un servidor privado. Se descarta por
  ser código filtrado.
- **"Server files" oficiales de Pearl Abyss** (Nova 3205, Corsair 3506, Guardian, Archer,
  "release-bdo-official-server-files", "leaked commercial log"…): se descartan por ser código
  propietario filtrado.

---

## 2. Nombres de BDO y su equivalente en CD

### 2.1 Convención [fuente: hilo de 2018]

| Cliente BDO | Emuladores | CD (Troc) |
|---|---|---|
| `eTrXxxReq` (envío) | `CMXxx` | `TrocTrXxxReq` |
| `TrXxxAck`, `TrXxxAcks` (lote) | `SMXxx` | `TrocTrXxxAck` |
| `TrXxxNak` | `SMXxxNak` | `TrocTrXxxNak` |
| `TrHeartbeatReq` `0x3E9` | `CMHeartbeat` `0x03E9` | `HeartbeatReq` 1008 (`0x3F0`) |
| `SMNak` `0x3F3` (661) | — | `TrocTrNak` 1013 |

- **Registro de mensajes del cliente de BDO** [fuente]: una tabla de 0x110 bytes por entrada:
  `{vtable*, u16 opcode, u16 flags[6], wchar nombre[81], char nombre[88]}`. El primer método
  de la vtable es el parser.
- En CD el registro guarda `{u32 X, u32 opcode, prototipo*}` (`troc-bdo-y-runtime.md` §2.3).
  La idea es la misma, con otro formato [inferido].

### 2.2 Tabla de equivalencias de actores (661 → CD 2976)

Los nombres de BDO son de la lista 661 (§1.1). La correspondencia es **[inferido]** salvo que
el nombre coincida tal cual.

| Tema | BDO (661) | Dir. | CD 2976 | Comentario |
|---|---|---|---|---|
| Alta de actores | `SMAddPlayers` 0xBB9, **`SMAddMonsters` 0xBBA**, **`SMAddNpcs` 0xBBB**, `SMAddDeadBodys` 0xBBC, `SMAddGraves`, `SMAddVehicles`, `SMAddCollectInfo`, `SMAddTents`, `SMAddInstallations`, `SMAddGate`, `SMAddInstanceObject`, `SMAddAlteregos` | S→C | **`ActorLoadDataAck` 2005** (uno para todo) | BDO tiene un mensaje por **tipo**, en plural (lote, probablemente `…Acks`). CD tiene uno genérico y pesado (6-19 KB) |
| Baja | **`SMRemoveActor`** 0xBCA (+ `SMRemoveCollectInfo`) | S→C | **`RemoveActorAck` 2516**; `ManagementRemoveActorAck` 2923; `DeadActorHideAck` 2229 (ToWorld) | El nombre coincide. En BDO lleva un **motivo** (`ERemoveActorType`, §6) |
| Mover jugador | `CMMovePlayer` 0xBCD, `CMMovePlayerOnCarrier`, `CMPlayerDirection` | C→S | `MoveActorReq` 2556, `RotateActorReq` 2828 | En CD, `MoveActorReq` sirve también para los NPC controlados |
| Mover a otros | `SMMovePlayer` 0xBCE, **`SMMoveNonPlayer` 0xBD6**, `SMStartCharacterMoving`, `SMStartCharacterNoUseNaviMoving`, `SMActionRotate`, `SMNetworkPosition` | S→C | `MoveActorAck` 2697, `RotateActorAck` (ambos `ToNearExceptOneControl`, vaciados) | BDO separa jugador y no jugador y tiene movimiento "con navegación" (el servidor manda un destino) |
| Ritmo del movimiento | `CMChangeMovePacketSendTerm`/`SMChangeMovePacketSendTerm`, `CMChangeSendRotationThreshold`/`SMChangeSendRotationThreshold` | ambas | no encontrado | El servidor ajusta cada cuánto envía el cliente su posición y su rotación |
| Acciones | `CMStartAction` 0xD4F, `SMStartAction` 0xD50, `SMStartActionNak`, `SMActionRefresh`, `CMDoAction` 0xD57, `SMHideAction` | ambas | `StartActionReq` 2223, `StartActionAck` 2991, `StartSpawnActionAck` 2723, `DoFrameEventReq` 2380 | `CMDoAction` es candidato a antecesor de `DoFrameEventReq` [inferido, débil] |
| IA | `CMAiOrderToNPC`, `CMAiOrderToHorse`, `CMAiOrderToRideVehicle`, `CMSendAiHandlerToAllNpcInServer` (GM), `SMAiKillNotifier`, `SMAiWorldNotifier`, `CMAiDebugInfo` | ambas | `ChangeAIStateReq` 2734, `StartAIFunctionReq` 2291, `UpdateAIDestinationReq` 2405, `AiEventAck` 2218, **`ChangeAiControlAck` 2508**, `AiControlChangeCheatReq` 2442, `OnceTimer_UpdateServerAIChart` 2032 | BDO: el cliente da **órdenes** a una IA que vive en el servidor. CD: el cliente **corre** la IA e informa |
| Aggro | **`SMAggroInformation`** 0x102E (S→C), `CMSetMaxAggroCount` (GM) | S→C | `AggroRelationListReq` 3006 (**C→S**), `ChangeAggroRelationValidTickReq` 2695 | La dirección se invierte: en CD el cliente informa del aggro [inferido] |
| HP y estado | `SMMonsterActorStatus` 0x102C, `SMPlayerActorStatus` 0x102D, `CMActorStatus`, `SMHitPartHpSet`, `SMSendSelfHpRate`, `CMSetHpActor` (GM) | ambas | `VaryHPAck` 2147, `VaryStatByAccInputReq` 2181, `VaryStatCheatReq` 2703 | |
| Restricción | `SMApplyActionRestriction` | S→C | `ApplyActionRestrictionAck` 2162 | El nombre coincide |
| Buffs | `SMApplyActiveBuff`, `SMUnapplyActiveBuff`, `SMAddAppliedActiveBuffs` | S→C | `ApplyBuffAck` 2506, `UnapplyBuffAck` 3007 | |
| Muerte | `SMDieNonPlayer` 0xE0E, `SMDiePlayer` 0xE0D, `CMKillActor` (GM) | S→C | `NotifyHandleDeadAck` 2530, `NotifyHandleFatalAck` 2899, `KillCheatReq` 2279 | |
| Cadáver y botín | `SMAddDeadBodys`, `CMGetDroppedItemList`/`SMGetDroppedItems`, `CMPickupDroppedItem`/`SMPickupDroppedItem`, **`CMLootNearDeadBody`**, `SMQuestItemInDeadBody`, `SMDropItemPosition`, `CMChangeLooting`/`SMChangeLooting` | ambas | `LootingDropFromDeadBodyReq` 3052, `LootDropCharacterRewardListAck` 2897, `RemoveLootableAck` 2822, `ProcessLootingDeadDropOnceTimer` 2024, `VaryLootDropSubItemCountAck` 2653 | `ChangeLooting` = modo de botín del grupo (§5.4) |
| Aparición (GM) | `CMCreateMonster`, `CMDelaySpawnMonster`, `CMDelaySpawnSelectedMonster`, `CMSetImmediatelyRespawnMonster`, `CMDontSpawnForManager`/`SMListDontSpawnToManager`, `CMChannelSpawnBlock`, `CMEnableRandomSpawn`, `SMListSpawnedNonPcCount` | C→S | `SpawnCharacterCheatReq` 2115, `CharacterPresetSpawnNPCReq` 2440, `DestroyAllFieldCheatReq` 2540 | Son comandos de administrador sobre un sistema de aparición **del servidor** |
| Aparición normal | — (no hay Req) | — | **`ActivateSpawningPoolReq` 2424**, `DeactivatePoolSpawnReq` 2119, `AutoSpawnRoadReq` 2550, `MeshGroupAutoSpawnReq` 2809, `TerrainRegionVoxelSpawnReq` 2327, **`RegisterSpawnDistanceByExternalReq` 2817** / `Reset…` 3102 | **No existe en BDO**: aquí CD difiere |
| Spawn de NPC en cliente | `SMSetNpcActionForClientSpawn` 0xD60 | S→C | ¿`StartSpawnActionAck` 2723? | Sugiere NPC que aparecen solo en el cliente y cuya acción fija el servidor [inferido, débil] |
| Antitrampas | `CMCheckAttackTolerance`, `CMCheckMovementValue`, `CMCheckActionVerifyValue(T)`, `CMCheckAnimationSpeed`, `CMCheckValidActionChart` | C→S | validaciones dentro de `DoFrameEventReq` (`combate-coop.md` §1.4) | Parecen interruptores de GM de comprobaciones del servidor sobre lo que reporta el cliente [inferido] |
| Entrada al mundo | `CMEnterPlayerCharacterToField`, `SMEnterPlayerCharacterToField`/`…Complete`, `SMLoadField`, `SMLoadFieldComplete`, `CMSetReadyToPlay` (819) | ambas | `LoginCharacterToFieldServerReq` 3093, `MoveFieldAck` 2174, `MoveFieldCompleteReq` 2549, `MoveFieldCompleteAck` 2144, `LoadingCompleteReq`, `LoginCharacterToGameReq` | §6 |
| Grupo | `SMPartyMembers`, `SMPartyNewMember`, `CMInviteParty`… | ambas | `OpenPartyReq` 2453, `InvitePartyMemberReq` 2865, finder `ToNearAndParty` | |
| Mapa | `SMWorldmapMonsterPos` | S→C | no buscado | Posiciones de monstruos para el mapa (jefes) [inferido] |

### 2.3 Los opcodes cambian entre versiones [fuente]

- `SMAddMonsters` es `0xBBA` en la 661 y `0x0F6A` en la 819. `SMRemoveActor` es `0xBCA` y
  `0x0E8E`.
- En la 661 los opcodes de actor forman un bloque contiguo en orden de declaración
  (`0xBB9`… `SMAddPlayers`, `SMAddMonsters`, `SMAddNpcs`…). En la 819 ya están barajados.
- En CD 2976 también lo están (`ActivateSpawningPoolReq` 2424, `DeactivatePoolSpawnReq` 2119).
- Esto confirma la regla del proyecto: **resolver por nombre (RTTI o registro), nunca fijar
  opcodes**. El hilo de 2018 añade que los **formatos** cambian menos: entre versiones solo
  se añaden campos.

---

## 3. Aparición de monstruos y NPCs

**BDO:**
- El **servidor decide** [inferido]:
  - los datos del cliente solo traen las posiciones de los NPC fijos, no las de los mobs
    [fuente, 2015];
  - en los emuladores públicos, sin una tabla de spawns propia no hay monstruos [fuente, 2017];
  - todos los mensajes de control de aparición son de GM hacia el servidor (§2.2).
- **Formato de un punto de aparición** en Illuminate [fuente, datos]:
  - ficheros por celda del mundo, con nombres del tipo `0_0_<n>_monster.lua` y
    `…_vehicle.lua`;
  - cada registro tiene posición, dirección, `characterKey` (tipo de criatura), cantidad,
    `spawnRange` (radio de dispersión), índice de acción inicial, índice de diálogo y,
    opcionalmente, una función (comerciante, etc.; `ESpawnType` enumera esos roles de NPC).
- **Reaparición:** existen `CMSetImmediatelyRespawnMonster` y `CMDelaySpawnMonster` [fuente,
  nombres]. Los parches oficiales ajustan "tiempos de reaparición" por zona y la aparición
  de "grupos" tras matar al anterior (búsqueda en las notas NA/EU de 2025-2026; no se citan
  porque no aportan mecánica).
- **"Regiones":** hay mensajes de región (`SMSetRegionProductivity`,
  `SMInitializeRegionWeatherData`), pero ninguno de spawn por región. No encontré cómo divide
  BDO el mundo para las apariciones.

**CD (para comparar, de nuestras notas):**
- **El cliente pide** las apariciones al cargar escena: `ActivateSpawningPoolReq` con el id
  de escena (ver `interacciones-mundo.md`), `AutoSpawnRoadReq`, `MeshGroupAutoSpawnReq` y
  `TerrainRegionVoxelSpawnReq`. Hay además `RegisterSpawnDistanceByExternalReq` para fijar
  la distancia de aparición.
- El servidor las ejecuta y asigna las claves `0xB01…`.
- **Conclusión [inferido]:** en CD el "quién decide" es **el streaming del cliente**. Es un
  cambio respecto a BDO, coherente con un juego en el que el servidor vive junto al único
  cliente.

---

## 4. IA, movimiento y difusión a los cercanos

**BDO [inferido, a partir de los nombres]:**
- **La IA y el movimiento de los monstruos corren en el servidor.** No hay ningún `CM…` que
  mueva a un no jugador. El servidor emite `SMMoveNonPlayer` y `SMStartCharacterMoving`
  (con un destino que el cliente sigue con navegación). El cliente solo da órdenes
  (`CMAiOrderToNPC`, `CMAiOrderToHorse`).
- `EAIState` (Illuminate) da los estados de la IA: `Idle, Walk, Around, Chase, Trace, Turn,
  Action, KnockBack, Return, Recovery, Revive, Dead, TargetLostable, Movable` [fuente].
  `Return`/`Recovery` describen la "correa": el monstruo vuelve a su punto y se cura.
- **Área de interés:**
  - los spawns de Illuminate están **troceados por celda**;
  - existe el motivo de baja `OutOfRange` (§6);
  - el servidor regula la frecuencia de los paquetes de movimiento
    (`ChangeMovePacketSendTerm`, `ChangeSendRotationThreshold`).

  No encontré ninguna descripción pública del tamaño de sector de BDO ni de cómo calcula
  quién ve qué. El esquema genérico coreano (mundo dividido en sectores, se buscan solo los
  vecinos) sí está documentado en charlas NDC (p. ej. NDC 2013, "게임 서버 디자인 가이드",
  https://www.slideshare.net/slideshow/ndc2013-19986939/19986939) [fuente, general, no es
  de BDO].

**CD (de nuestras notas, para comparar):**
- Los sectores de `ServerField` mantienen la lista de observadores `{ServerUserActor*,
  refcount}`. `ToNear` usa esa lista.
- **La IA la corre el cliente que controla al actor** (`ChangeAIStateReq`,
  `CurrentTargetChangeReq`, `MoveActorReq` de NPC). Su difusión a los demás era
  `ToNearExceptOneControl`, que en esta build está **vaciado**.
- **Encaje [inferido]:** CD conservó la infraestructura de AOI del MMO (sectores,
  observadores, `ToNear`), pero pasó la IA del servidor (BDO) al "controlador" de cada NPC.
  `ChangeAiControlAck` (2508) es casi seguro el mensaje con el que el servidor asigna el
  controlador. Es el mismo patrón que SPICA (§1.2): el servidor delega actores en un cliente
  y los reasigna si ese cliente se va.

---

## 5. Daño, aggro, muerte, botín y HP

### 5.1 Autoridad del daño

- **No encontré ninguna descripción pública del flujo de golpe de BDO** (quién detecta el
  impacto y quién calcula el daño).
- Lo que hay son nombres:
  - `CMDoAction`;
  - interruptores de comprobación del servidor: `CMCheckAttackTolerance`,
    `CMCheckActionVerifyValue`, `CMCheckMovementValue`, `CMCheckAnimationSpeed`,
    `CMCheckValidActionChart`.
- Eso encaja con lo que medimos en CD: **el cliente informa del golpe** (`DoFrameEventReq`
  con objetivos), **el servidor valida la acción y calcula** (`combate-coop.md` §1.4). La
  "tolerancia de ataque" sería el margen de esa validación [inferido].

### 5.2 Aggro con varios jugadores [fuente]

- Cada personaje puede tener provocados **10 monstruos como máximo** (antes 7). Los objetos
  invocados provocan 1 y las criaturas invocadas, 4 (parche KR, 2018-11-20).
- Existe `CMSetMaxAggroCount` (GM), así que el límite vive en el servidor.
- El servidor envía `SMAggroInformation`.
- No hay documentación pública de la **elección de objetivo** (tabla de odio, daño, distancia).

### 5.3 Muerte y cadáver

- BDO trata el cadáver como **otro tipo de actor**: `SMAddDeadBodys`, el motivo de baja
  `DespawnDeadBody` y `SMSetDeadBodyRevive` [fuente, nombres].
- También hay `SMAddGraves` (tumbas de jugador).
- En CD el actor es el mismo: `NotifyHandleDeadAck` → cadáver → `RemoveActorAck` 15-75 s
  después, más `DeadActorHideAck` por `ToWorld`.

### 5.4 Botín [fuente]

- Modos de grupo: libre, por orden, aleatorio y solo el líder (`CMChangeLooting`).
- **Libre:** el botín se genera **al morir**, con los bonus de **quien mata**.
- **Por orden:** se genera **al recogerlo**, por turnos.
- Jefes del mundo: un mínimo del 0,01% del daño para optar al botín, con un reparto por daño
  y otro aleatorio.
- Recogida: `CMLootNearDeadBody` / `CMPickupDroppedItem`.

### 5.5 HP

- `SMMonsterActorStatus`, `SMPlayerActorStatus` y `SMHitPartHpSet` (HP por partes del
  cuerpo). Formatos no encontrados.
- En CD, `VaryHPAck` lleva HP×1000, HP máx×1000 y una **secuencia por actor** (`+0x40`).

---

## 6. Cómo recibe un jugador los monstruos que ya existen y cómo se quitan

**BDO:**
- **Orden de entrada** [inferido de la lista 819; el autor la parece ordenar según iba
  arreglando paquetes]: `CMEnterPlayerCharacterToField` → `SMEnterPlayerCharacterToField` →
  `SMLoadField` → `SMLoadFieldComplete` → `SMAddPlayers` → (estado del personaje) →
  `SMAddMonsters`, `SMAddVehicles`, `SMAddDeadBodys`, `SMAddNpcs` → `SMRemoveActor` /
  `SMStartAction` / `SMMovePlayer`… El cliente confirma con `CMSetReadyToPlay`.
- Los altas van **por lotes y por tipo** (nombres en plural).
- **Retirada:** `SMRemoveActor` con un **motivo** [fuente, `ERemoveActorType` de Illuminate]:

  | Valor | Motivo |
  |---|---|
  | −2 | `None` |
  | −1 | `Collect` |
  | 0 | `Logout` |
  | 2 | `DespawnDeadBody` |
  | 3 | `DespawnMonster` |
  | 4 | `Teleport` |
  | 5 | `DespawnEscort` |
  | 7 | `DespawnTent` |
  | 9 | **`OutOfRange`** |
  | 10 | `SealServant` |
  | 11 | `DespawnSummon` |
  | 14 | `DespawnTamedServant` |
  | 15 | `QuitPlayer` |

  `OutOfRange` es la baja por área de interés: un actor sale de la vista de un observador
  sin haber muerto.

**CD:**
- Entrada: `MoveFieldAck` → `MoveFieldCompleteReq` → ráfaga de `ActorLoadDataAck` →
  `LoadingCompleteReq`/`LoginCharacterToGameReq`.
- El diff de observadores de `0x2AF8AF0` llama a "aparecer" (`0x2B000C0`) y a "desaparecer"
  (`0x2B00380` → escritor de `RemoveActorAck`).
- Falta comprobar si `RemoveActorAck` de CD también lleva un motivo
  (`RemoveActorAck` 2516, handler del cliente). Si lo lleva, los valores de BDO son un buen
  punto de partida [inferido].

---

## 7. Consecuencias para la opción A (autoridad del host, `combate-coop.md` §4.3a)

1. **Altas y bajas explícitas, con motivo.**
   - El host debe decidir por sí mismo qué enemigos "ve" el invitado: un radio alrededor del
     clon o del invitado, con histéresis.
   - Entrar en el radio → `ActorLoadDataAck` traducido. Salir → `RemoveActorAck` traducido.
     Muerte → `NotifyHandleDeadAck`, y el cadáver sigue hasta su `RemoveActorAck` real.
   - Copiar la idea de BDO: distinguir en nuestro protocolo **"fuera de rango"**,
     **"despawn"** y **"muerto"**. Así el invitado no confunde una baja por distancia con una
     muerte, ni deja cadáveres huérfanos [inferido].
2. **Lotes por tipo y sin repetir.** BDO manda altas en lote (`SMAddMonsters`). Nosotros:
   - **agrupar** los `ActorLoadDataAck` del mismo tick;
   - **no reenviar** un alta a un invitado que ya tiene ese actor.

   Con altas de 6-19 KB, esto es lo que más ancho de banda ahorra [inferido].
3. **Suprimir los spawns propios del invitado.**
   - En BDO no hace falta porque el cliente no pide spawns. En CD sí: es la prueba T7
     (`ActivateSpawningPoolReq`, `AutoSpawnRoadReq`, `MeshGroupAutoSpawnReq`, y además
     `TerrainRegionVoxelSpawnReq` y `RegisterSpawnDistanceByExternalReq`, que no estaban en
     la lista de T7).
   - Cuidado: los NPC de misión, comerciantes y gimmicks podrían venir por los mismos
     caminos. Filtrar **solo enemigos** (por tipo de pool o por `characterKey`) [inferido].
4. **El HP lo manda el host, con secuencia.**
   - BDO no deja el HP en manos del cliente: hay mensajes de estado del servidor.
   - En el invitado, aplicar los `VaryHPAck` del host en orden de secuencia (`+0x40`) y
     **descartar** cualquier `VaryHPAck` que genere su propio servidor para esas claves
     (no deberían existir si se suprimen sus spawns).
5. **Aggro: limitarlo en el host.**
   - Si los enemigos del host llegan a atacar al clon (T6), copiar el límite de BDO: no más
     de ~10 enemigos con aggro sobre el invitado. Eso evita que un invitado lejano arrastre
     media zona.
   - Si el host no ataca al clon, no hay nada que limitar.
6. **Botín.** El modo "libre" de BDO (el botín es de quien mata) es el más fácil de copiar:
   cada jugador recoge en su mundo lo que **él** mata. Con la opción A, el invitado no tiene
   el monstruo en su servidor, así que su botín habría que **generarlo en el host** y
   entregarlo por otra vía. **No lo resuelve ninguna fuente**: queda abierto.

## 8. Lo que no se encontró

- Ningún código abierto, legal y funcional de **IA de monstruos, aparición, combate o botín**
  de BDO. Los únicos que lo tenían son filtraciones (§1.3).
- **Formatos de cable** de `SMAddMonsters`, `SMMoveNonPlayer`, `SMMonsterActorStatus`,
  `SMDieNonPlayer` o `SMAggroInformation`. Solo hay nombres y opcodes.
- Una **descripción pública del área de interés** de BDO (tamaño de sector o radio) y de cómo
  elige objetivo un monstruo entre varios jugadores.
- **Charlas técnicas de Pearl Abyss** sobre el servidor de BDO o de CD: las búsquedas en
  coreano e inglés solo dan entrevistas de producto y el motor gráfico BlackSpace.
- El fichero adjunto del hilo de 2018 (`opc+fn.7z`, lista completa de nombres del cliente
  KR 2100): solo está en el foro, y el foro no es accesible.
- El **significado exacto** de `SMSetNpcActionForClientSpawn`,
  `SMNotifyNonAllyActor`/`SMDisappearNonAllyActor` y `CMNotifyNonAllyActor`. Por el nombre
  pueden ser NPC que aparecen en el cliente y avisos de actores hostiles en el radar [inferido].
- Cómo implementa Illuminate su servidor Channel (§1.1, nota).

## 9. Acciones propuestas (de menor a mayor riesgo)

| # | Qué | Tipo | Decide |
|---|---|---|---|
| R1 | Leer el handler del cliente de `RemoveActorAck` (2516) y su escritor en el servidor: ¿lleva un byte de motivo? | estático | Si la opción A puede distinguir "fuera de rango", "despawn" y "muerte" de forma nativa |
| R2 | Escritor y destinatarios de `ChangeAiControlAck` (2508) y `OverriedOwnershipStateTypeyAck` (2168); `senders.py` | estático | Modelo de control de la IA |
| R3 | Log en vivo (solo lectura, con el sniffer) de 2508/2168 al entrar en zonas con enemigos | en vivo, lectura | Lo mismo, confirmado |
| R4 | Handler de `RegisterSpawnDistanceByExternalReq` (2817) y del resto de Req de spawn | estático | Radio de aparición; qué filtrar en T7 |
| R5 | Añadir `TerrainRegionVoxelSpawnReq` y `RegisterSpawnDistanceByExternalReq` a la prueba T7 | en vivo, copia de la partida | Supresión de los spawns del invitado |

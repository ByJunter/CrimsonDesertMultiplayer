# Troc: arte previo (BDO / comunidad) y mapa en vivo cliente↔servidor

Build: CrimsonDesert.exe 1.0.0.2976 (PE timestamp `0x6AB28F00`, base fija `0x140000000`,
parche público 2.03.02). Fecha: 2026-10-03. Partida cargada, jugador en el mundo.
Todo en **solo lectura**: `cdmp_probe.exe` contra el proceso y Python leyendo el fichero del exe.
Las direcciones de heap (`0x319…`) son de esta sesión y cambian en cada arranque; lo estable
son los `exe+RVA` y los offsets de campo. Scripts y volcados en `build/research/`
(`probe_util.py`, `walk_troc_table.py`, `client_actor_list.py`, `srv_keys.py`, `npc_pos.py`,
`cmp_stats.py`, `diff_snap.py`, `troc_table_session.txt`, `troc_table_client.txt`, …).

Leyenda: **[V]** verificado (en vivo, en el exe o en la fuente primaria), **[R]** fuente
secundaria no confirmada, **[S]** especulación.

---

## Resumen

1. **El canal entre las dos mitades es una "sesión de red virtual"**: `ServerUserActor+0x1F8`
   apunta a un `pa::NwVirtualAsyncSession` (con su propio `CryptogramAesGcm`). En el lado
   cliente hay un `NwVirtualSyncSession`. Es la capa de red del MMO con el socket sustituido
   por una cola en memoria (`VirtualServer`, `VirtualServerWaitAsync`). [V]
2. **Hay dos registros de mensajes en vivo** (opcode → instancia prototipo estática en `.data`):
   - el del **servidor**: 566 entradas, 481 `…Req`, 82 timers y 3 internos;
   - el del **cliente**: 370 entradas, 355 `…Ack` y 15 `…Nak`.
   Los opcodes empiezan en ~1000 (`0x3E9`), como en BDO. [V]
3. **El servidor corre en sus propios hilos**: `ServerRunThread`, `ServerWalkThread` y
   `ServerRepeatTimerThread`. El cliente corre en `ClientRunThread`. Son objetos
   `pa::NetworkThread`. [V]
4. **Cliente y servidor comparten la identidad del actor.** Ambos guardan una clave de actor
   u32 en `actor+0x60` y un número de serie en `actor+0x50`, con los mismos valores en los
   dos lados: jugador `0xA0100001`, usuario `0x90100000` y el resto `0xB0100000 | serie`.
   Nunca se identifican por puntero. [V]
5. **Las tablas de componentes son espejo** (`actor+0x68`, mismos slots): `Client*Component` ↔
   `Server*Component`. El servidor tiene su propio Status (HP), su propio
   `ServerTransformSyncActorComponent` con la posición y su propio inventario. [V]
6. **El servidor simula el mismo conjunto de actores que el cliente**, no el mundo entero. En
   una toma simultánea, los 91 `ServerNormalInGameActor` vivos estaban también en el
   cliente, y 125 de 128 actores del cliente estaban en el servidor. Ambos lados están
   acotados al radio de streaming. [V]
7. **Quedan restos claros del MMO** en el exe: servidores de autenticación y "field", un
   handshake cifrado, heartbeat, ODBC/SQL con 116 procedimientos `PaGamePublic.*`, Kafka, y
   party, guild y World Market. El juego **no tenía conexiones TCP abiertas** durante la
   prueba. [V]

---

## Parte 1 — Arte previo

### 1.1 Black Desert Online (BDO): emuladores e investigación de paquetes

- **Nombres de Pearl Abyss.** En 2015 el cliente coreano de BDO llamaba a un paquete
  `eTrHeartbeatReq`, opcode `0x3E9`, enviado cada 6 s. El investigador encontró unos 450
  paquetes manejados por un único switch. También sugirió que "el servidor es el cliente
  compilado de otra forma" (servidor y cliente público desde la misma base). [V]
  (RageZone, "[Development] Black Desert Emulator", archivado:
  http://web.archive.org/web/20230113091943/https://forum.ragezone.com/f1000/black-desert-emulator-1085129/index3.html)
  - **Qué se aplica:** el prefijo `Tr` + sufijo `Req` es el antecesor directo de
    `TrocTr…Req`. En Crimson Desert, `TrocTrHeartbeatReq` es el opcode **1008 (`0x3F0`)** y
    la numeración también empieza alrededor de 1000.
- **Illuminate** (CoolManBob, C++, 2021, incompleto): https://github.com/CoolManBob/Illuminate [V]
  - Servidores Login / World / Channel (game).
  - Cabecera de paquete: `u16 size`, `u8 isEncrypted` y `u16 opcode`; el opcode va cifrado.
  - Unos 670 opcodes `CM*` (cliente→servidor) y 790 `SM*` (servidor→cliente), en el rango
    `0x03E9`…`0x13xx`.
  - Las respuestas de error usan el sufijo **`Nak`**, que también existe en Crimson Desert
    (19 clases `TrocTr…Nak`; 15 están en el registro del cliente).
  - Cifrado del cliente moderno: Rabbit con una clave de sesión derivada vía AES en el
    handshake (en 2015-2016 era Rijndael/AES).
- **DesertProject** (C#, 2015-2016, LoginServer/WorldServer, llegó hasta el lobby; ahora
  bloqueado por DMCA):
  http://web.archive.org/web/20180611150923/https://github.com/blackdesert/DesertProject [V]
- **Simon Wörner, "Black Desert Online – Overview"** (2017): el exe iba empaquetado con
  Themida y el anticheat era XIGNCODE3.
  https://web.archive.org/web/20200812042033/https://blog.brn.li/post/bdo-01-overview/ [V]
- **ICE se usa para los ficheros**, no para la red: cifra los PAZ y los `.loc`.
  https://github.com/AMGarkin/BDOcrypt [V]
- **El RTTI de BDO usa el namespace `ge::`** (`ge::SelfPlayerActorProxy`); Crimson Desert usa
  `pa::`. https://secret.club/2019/01/24/reverse-engineering-bdo-2.html [V]
- Nadie ha publicado una explicación del nombre "Troc". [S] Es probablemente un prefijo de
  módulo o protocolo encima del `Tr` (transaction) de BDO.

### 1.2 Crimson Desert: trabajo de la comunidad sobre la división cliente/servidor

- **Trinity** (XeTrinityz, C++, 2026-08, builds 2.0x): https://github.com/XeTrinityz/Trinity [V]
  - Un **flag de "realm" por hilo** en el bloque TLS del motor: TLS[0]`+0x1F2`, u8, 0 =
    cliente y 1 = servidor. Trinity lo cambia para ejecutar funciones del motor "en el
    realm del servidor" y luego lo restaura. Advierte que dejar un hilo en el realm
    equivocado corrompe lo siguiente que toque.
  - Inventario, equipamiento y hora del mundo están duplicados, y **el servidor manda**:
    una reconciliación por frame de servidor a cliente revierte los cambios hechos solo en
    el cliente.
  - Para añadir un objeto hay que crearlo en los dos realms con el mismo id.
  - Habla de mensajes numerados servidor→cliente (2137, 2440). **Ojo:** en nuestra build,
    2137 es `TrocTrCallMercenaryCoolTimeCompleteReq` y 2440 es
    `TrocTrCharacterPresetSpawnNPCReq`. O los opcodes cambian entre parches, o Trinity numera
    otra cosa. No hay que fijar opcodes como constantes.
- **Glint-Spotter** (shin2344234, MIT, build 2.01.00): https://github.com/shin2344234/glint-spotter [V]
  - Usa pares nativos `TrocTrCreateOrChangePinMarkerReq/Ack`. El autor observa que pasar por
  la ruta de la `Req` hace los pines persistentes.
  - Resuelve clases desde los "deserializadores" de mensajes.
  - En su v1.1.27 da `exe+0x6DA67D8` como global del ServerActorManager, el mismo que
    usamos, y cuelga los registros de guardado del `ServerUserActor`.
- **CrimsonDesertTools PR #123**: "Self-heal the session to ServerUserActor offset by RTTI".
  Alguien más usa la relación sesión↔ServerUserActor.
  https://github.com/tkhquang/CrimsonDesertTools/pull/123 [V]
- **Post en Reddit (u/No_Variation_7024)**, resumido por crimsonupdates.com: dice haber
  reconstruido "el despachador interno de mensajes" y "el sistema de handlers TrocTr",
  controlar NPCs vía `ServerTransformSyncActorComponent` sin rebote, y retransmitir más de
  200 paquetes entre dos instancias. No publicó código.
  https://crimsonupdates.com/articles/crimson-desert-coop-mod-wip-position-sync-blackspace-engine/ [R]
- **CrimsonDesertCoop** (blizz3010) y **Crimson-Desert-Online-Modification** sincronizan
  memoria a mano con su propio protocolo y no usan la capa Troc. [V]
- **CrimsonDesertTelemetry** (ZappendusterFX, ahora en
  https://github.com/fabianviol/CrimsonDesertTelemetry) es telemetría **local** de
  cámara/iluminación en 127.0.0.1:27311. No tiene nada que ver con los servidores de
  Pearl Abyss. [V]

### 1.3 Contexto: de MMO a juego de un jugador

- Crimson Desert se anunció en la G-Star 2019 como MMORPG precuela de BDO. En diciembre de
  2020, los coproductores decían que se podría "cambiar entre un jugador y multijugador en
  cualquier momento" con progreso compartido.
  https://mmoculture.com/2020/12/crimson-desert-pearl-abyss-interview-clarifies-single-player-and-multi-player-questions-and-more/ [V]
- Después pasó a ser un juego de acción y aventura para un jugador. La mitad `Server*` dentro
  del proceso es casi seguro el servidor de juego del MMO compilado dentro del cliente. [S]
- Julio de 2026: el CEO dice que el cooperativo se estudia como "una tarea importante" que
  implica infraestructura de servidores.
  https://www.kitguru.net/gaming/matthew-wilson/pearl-abyss-still-exploring-mod-support-and-multiplayer-for-crimson-desert/ [V]

### 1.4 ¿Habla el juego con servidores remotos?

| Fuente | Qué se ve | Estado |
|---|---|---|
| Steam y FAQ de Pearl Abyss | Solo un jugador; Denuvo (internet para activar y para revalidar cada cierto tiempo); juega offline después; `pers.exe` sube informes de error bajo demanda | [V]/[R] |
| URLs en el exe | `https://open-pers.pearlapis.com/logstash` (con la clase `ElasticLogstashHTTPCallback`), `https://productmanager.pearlapis.com`, uno interno `http://krgco-lgspri1.pearldev.io:10001` y una página de `wiki.pearlabyss.com` | [V] |
| Imports del exe | WinHTTP (`WinHttpOpen/Connect/SendRequest…`, clases `WinHttpRequest` y `WinHttpTradeRequestData`) y Winsock (`WSASocketW`, `WSASend`, `WSAIoctl`, `connect`, `listen`, `accept`, `getaddrinfo`) | [V] |
| Kafka | `librdkafka.dll` y `librdkafkacpp.dll` en `bin64`; cadenas `Kafka Network Open`, `Kafka Message` y el error `eErrNoCantOpenKafkaNetwork` | [V] |
| `netstat` durante la sesión | El proceso solo tenía **un socket UDP en 127.0.0.1**, que es nuestro `cdmp.asi` (cargado en el juego). **Ninguna conexión TCP** | [V] |

**Conclusión:** el bucle de juego cliente↔servidor es local. Lo remoto es Steam/Denuvo,
informes de error y posibles logs HTTP o Kafka. No vimos ningún servidor de juego remoto. [V]

### 1.5 Restos del transporte MMO en el exe

- **Login completo de MMO** como mensajes Troc [V]:
  - `TrocTrConnectCryptogramToServerReq/Ack`; la cadena de log dice "Login step - Recieve
    TrocTrConnectCryptogramToServerAck".
  - `TrocTrLoginUserToAuthenticationServerReq/Ack`, `TrocTrLoginUserToFieldServerReq`,
    `TrocTrLoginCharacterToFieldServerReq`, `TrocTrLoginCharacterToGameReq`.
  - `TrocTrLogoutCharacterToFieldServerReq/Ack`, `TrocTrConnectToServerAck` (1002),
    `TrocTrOnConnectToServerReq` (1005), `TrocTrHeartbeatReq` (1008).
  - `TrocTrHeartbeatToParentRepeatTimer`: el "servidor padre" implica una topología de varios
    servidores.
- **Topología de servidores en los nombres:** AuthenticationServer, FieldServer, "parent
  server", `ServerInformationManager`, `ToSessionListSendTargetFinder` (difusión a una lista
  de sesiones), y `ConditionData_CheckLoginStep`. [V]
- **Base de datos** [V]:
  - Clases `OdbcFixedTvpType<…>`, `OdbcTvpDataType` y `SqlFetchResultSet`.
  - Ruta de fuente `…\BlackSpace\GamePlay\Network\GameServerFramework\Sql.cpp`.
  - 116 nombres de procedimientos `PaGamePublic.*`, por ejemplo `getOrGenerateServerNo`,
    `registerServerInformation`, `updateServerVisibility`, `loginCharacterToField` y
    `checkOdbcHeartbeat`.
  - Errores `eErrNoDbSessionCantOpen`, `eErrNoCantLoadFromDB`, etc.
  - Junto a la cadena `VirtualServer` hay una cadena con aspecto de credencial
    hard-codeada (no la copiamos).
- **Red y cifrado** [V]:
  - Clases `NwSession`, `NwSyncSession`, `NwAsyncSession`, `NwVirtualSyncSession`,
    `NwVirtualAsyncSession`, `NwSyncSessionPool`, `NetworkThread`, `NetworkActor`,
    `AsyncRequest`, `AsyncRequestSend`, `AsyncRequestEtc`, `PacketBuffer`, `PacketBufferSend`,
    `CryptogramAesGcm`, `CryptogramChaCha20` y `Cryptogram`.
  - Errores `eErrNoCantConnectServer`, `eErrNoCantNetworkListenOrAccept`,
    `eErrNoFailedToOpenCryptogramAes/Rsa` y `eErrNoCantFindTroc`.
- **Contenido MMO** [V]: party (`eErrNoFailToInvitePartyMember`), guild, World Market
  (`eErrNoFailConnectionWorldMarket`), `ServerLobbyActorComponent` y
  `TrocTrClearCharacterLobbyDataForSinglePlayAck` (limpieza explícita para un jugador).
- **Serialización:** existen `PacketBuffer`/`PacketBufferSend` y `AsyncRequestSend`, todas las
  sesiones llevan un `Cryptogram` y Glint-Spotter habla de "deserializadores de mensajes".
  Todo apunta a que cada `TrocTr*` sabe codificarse a un buffer de bytes. **No lo verificamos
  en código**; lo cubre el análisis estático del desensamblado.
- **Restos del transporte real** [V]:
  - La sesión servidor es un objeto `NwSession`, el mismo tipo base que una sesión real.
  - Hay un pool de 10 `NwSyncSession` **no virtuales** ya creados (`exe+0x57261D8`).
  - Hay `NwSyncClientFrameworkDesc`, `NwServerFrameworkParameter`, código de
    `listen`/`accept`/`connect` y handshake cifrado.
  - La identidad del actor es una clave numérica, no un puntero.
  - [S] El transporte real probablemente sigue compilado y la versión virtual lo sustituye
    sin más. El servidor actual solo gestiona **un** `ServerUserActor`.

---

## Parte 2 — Mapa en vivo

### 2.1 Gestores

| Objeto | Cómo llegar | vtable |
|---|---|---|
| ServerActorManager (SAM) | `[exe+0x6DA67D8]` | `exe+0x5B25988` |
| ClientActorManager (CAM) | `[[exe+0x6D69190]+0x30]` (también `exe+0x6DDA940` según Glint-Spotter) | `exe+0x55B62C8` |

**ServerActorManager**, objeto de menos de 0xE0 bytes; los bloques de 0x100 siguientes son
otros objetos:

| +off | Contenido |
|---|---|
| 0x08 | `CommonActorContainer*` (vtable `exe+0x58FF398`; cabecera tipo hashmap: `+0x18` buckets inicializados a `-1`, `+0x20` nodos) |
| 0x10 | `WindowsRWLock` embebido; 0x28: `WindowsConditionVariable` embebido |
| 0x40 | `ServerModuleFrameworkParameter*`: decenas de timeouts y límites en ms (30000, 600000, 10000, 5000, 60000…) y el valor `0x3C8D` (15501) en `+0x44`, de significado desconocido |
| 0x48 | **"ServerModule"** (sin RTTI): `+0x00 KernelEvent`, `+0x08 AsyncRequestEtc`, **`+0x18 NwVirtualAsyncSession*`**, `+0x28/+0x30` → interior del registro de mensajes del servidor, `+0x88 ServerModuleFrameworkParameter`, `+0x90 ServerModuleThreadStorage` |
| 0x50 | Tabla de `{u64 n, ServerActorManager*, u64 id}` (0x18 por entrada) a la que apunta `actor+0x88` |
| 0x68/0x80 | Segundo RWLock y CondVar |
| 0x9C (u32) | **Número de actores servidor**, incluidos los slots del pool (257 frente a 161 en el cliente; ver 2.5) |
| 0xA4, 0xC8… (u32) | Contadores monótonos que suben unas 30-40 por segundo |
| 0xA8 | Contenedor con el primer `ServerAiActor…` |

**ClientActorManager**: `+0x08 ClientActorContainer`, `+0x50` personaje del jugador,
`+0x58 ClientUserActor`, `+0xB8 PhaseSequencerManager`, `+0xD0 GamePlayDialogManager`,
`+0xE8 CameraManager` y muchos pares `(ptr, capacidad)` de pools.

**ClientActorContainer** (vtable `exe+0x557C690`):
- **`+0x58` array de `ClientActor*`** y **`+0x60` (u32) su número**, con la capacidad 600 en
  `+0x64`. Valió entre 84 y 223 según dónde estaba el jugador.
- `+0x28` y `+0x38`: listas con el `ClientUserActor`.
- `+0x48`: otro array de actores de capacidad 600.
- `+0x88`/`+0x90`: contadores que se mueven a la par que los del SAM.

### 2.2 Usuario, personaje y sesión

```
ClientUserActor (exe+0x55B59F0)          ServerUserActor (exe+0x5B257A8)   [1 sola instancia]
 +0x60 key = 0x90100000         ==        +0x60 key = 0x90100000
 +0xD0/+0xD8 -> ClientChildOnlyInGameActor +0xD0/+0xD8 -> ServerChildOnlyInGameActor
 +0x88 -> nodo de CAM                      +0x88 -> SAM+0x50 (tabla de ids)
 (sin sesión)                              +0x1F8 -> NwVirtualAsyncSession
```

**Personaje del jugador:**

| Campo | ClientChildOnlyInGameActor (`exe+0x5585C40`) | ServerChildOnlyInGameActor (`exe+0x5B1E2B0`) |
|---|---|---|
| +0x48 (u32) | `0x00010005` | `0x00010005` |
| +0x50 (u32) serie | 1 | 1 |
| +0x58 | `0x00100601_00000000` | `0x00100601_90100000` (clave del dueño) |
| **+0x60 (u32) clave** | **`0xA0100001`** | **`0xA0100001`** |
| +0x68 | tabla de componentes | tabla de componentes (mismos slots) |
| +0x80 | (0x30, 0x30) | (0x2D, 0x2D) |
| +0xA0 | `ClientUserActor*` | `ServerUserActor*` (dueño) |
| +0xD0 | puntero propio del cliente | `NwVirtualAsyncSession*` |
| tamaño | grande | **0x100** (pool contiguo) |

Para los NPCs, `+0x48 = 0x00010002`.

**Formato de la clave** [V en los valores, S en el significado]:
- Los 4 bits altos son el tipo: `0x9` usuario, `0xA` personaje controlable, `0xB` actor
  normal o companion.
- El bit `0x00100000` está activo en las entidades en uso; los slots libres del pool tienen
  `0xA00000xx`.
- Los 20 bits bajos son la serie (= `+0x50`).

**NwVirtualAsyncSession** (vtable `exe+0x57260C0`, instancia única, la sesión del lado
servidor):
- `+0x08` WindowsLock y `+0x90` **`ServerUserActor*`**.
- `+0x98` **`CryptogramAesGcm*`** (vtable `exe+0x570AE40`, buffer de clave de 32 bytes en
  `+0x10/+0x18`).
- **`+0xA0` → registro de mensajes del servidor** (ver 2.3).
- `+0xC8` `WindowsLockForOnlyEngineXXX`.
- `+0xE0…` varias sub-colas con la vtable `exe+0x5B30B88` y triples de índices.

**NwVirtualSyncSession** (vtable `exe+0x5726248`, instancia única, probablemente el lado
cliente):
- `+0x98` `CryptogramAesGcm`, `+0xA8 = 0x100`, `+0xC0 = 300.0f` y `+0xD0` → buffer de datos
  de alta entropía.
- La identificación como "lado cliente" es una inferencia.

Otros objetos de red vivos:
- **10 `NwSyncSession`** en pool (`exe+0x57261D8`, stride 0xE0, cada uno con su
  `CryptogramAesGcm`).
- `NwSyncSessionPool` (`exe+0x5573868`).
- **4 `NetworkThread`** (`exe+0x5725ED8`): nombre ASCII en `+0x18` y thread id en `+0x08`.
  Son `ServerRunThread`, `ServerWalkThread`, `ServerRepeatTimerThread` y `ClientRunThread`.
- `VirtualServerWaitAsync` (`exe+0x55C6680`) y `Troc` (base, `exe+0x5726410`) **no** tienen
  instancias en el heap: las únicas instancias Troc son prototipos estáticos.

### 2.3 Registros de mensajes (opcode → prototipo)

Cada entrada ocupa 16 bytes: `u32 X | u32 opcode` y luego un puntero a la **instancia
prototipo estática** del mensaje (en `.data`, `exe+0x6A5xxxx`–`0x6A8xxxx`, una por clase).

**Servidor** (`[NwVirtualAsyncSession+0xA0]`, entradas desde `+0x60`):
- **566 entradas**: 481 `…Req`, 82 `…Timer` (los timers se despachan por el mismo
  mecanismo) y 3 internos.
- Las primeras: 1005 `OnConnectToServerReq`, 1006 `OnDisconnectToServerReq`,
  1008 `HeartbeatReq`, 2001 `LogoutSessionToServerReq` y luego la jugabilidad
  (`StartActionReq` 2223, `MoveActorReq` 2556, `RotateActorReq` 2828…).
- La cabecera tiene en `+0x40` un `WindowsLockForOnlyEngineXXX` y en `+0x50` otro lock.
  En la segunda toma, ese lock tenía como dueño el thread id de **ServerWalkThread**: el
  hilo del servidor está usando el registro.
- Volcado completo en `build/research/troc_table_session.txt`.

**Cliente** (base `0x3190E6DE880` en esta sesión, entradas desde `+0x60`):
- Se encontró buscando en el heap quién apunta al prototipo de `TrocTrConnectToServerAck`.
- **370 entradas**: 355 `…Ack` y 15 `…Nak`. Rango de opcodes 1002…3100.
- Ejemplos: 1002 `ConnectToServerAck`, 1013 `TrocTrNak`, 2697 `MoveActorAck`,
  2987 `DoTeleportAck`, 2174 `MoveFieldAck`, 2509/2922/2846
  `PostProcessAttackResult_*Ack`.
- Volcado completo en `build/research/troc_table_client.txt`.
- Hay 525 clases `Ack` y solo 355 están en este registro. Las demás podrían estar en otro
  registro o no usarse.

Lo que significa la `X` de cada entrada (¿índice de handler? ¿hash?) está por confirmar.

**Prototipo de un mensaje** (ej. `TrocTrMoveActorReq` en `exe+0x6A6FBA0`, unos 0x90 bytes):

| +off | Contenido |
|---|---|
| 0x00 | vtable |
| 0x08 (u32) | Tipo o categoría: 4 en `MoveActorReq`, 1 en los Ack, `0x7FFFFFFE` en `OnConnectToServerReq` |
| 0x0C (u32) | **opcode** |
| 0x10 | Flags (los Ack tienen `0x02` en el byte `+0x17`) |
| 0x18 | Dos u16, por ejemplo `0x000B, 0x000B` o `0x0005, 0x7FC6` |
| 0x88 | 2 |

Los nombres de estos campos son trabajo del análisis estático.

### 2.4 Componentes espejo (`actor+0x68`)

Los slots coinciden uno a uno. En el lado servidor, unos pocos son `Common*`.

| Slot | Componente |
|---|---|
| 0x08 | UserLogin (solo cliente en el personaje) |
| 0x18 | ChildContainer (`Common` en el servidor) |
| 0x20 | **Status** |
| 0x28 | SubLevel |
| 0x38 | EquipSlot |
| 0x40 | CharacterControl |
| 0x48 | Vehicle |
| 0x50 | Detect |
| 0x58 | Ai |
| 0x60 | Effect (solo cliente) |
| 0x68 | FrameEvent |
| 0x70 | Catch |
| 0x78 | RemoteCatch |
| 0x80 | Attack |
| 0x88 | Projectile |
| 0x90 | Target |
| 0x98 | Interaction |
| 0xA0 | GameEventHandler |
| 0xA8 | Sequencer |
| 0xB0 | SequencerPlayData |
| 0xB8 | **Inventory** |
| 0xD0 | Store |
| 0xD8 | RepairItem |
| 0xE0 | ItemExchange |
| 0xF0 | Skill |
| 0xF8 | QuestDialog |
| 0x100 | Guild |
| 0x108 | Party |
| 0x110 | MercenaryClan |
| 0x120 | Wanted |
| 0x138 | Quest |
| 0x140 | Friendly |
| 0x148 | Journal |
| 0x150 | Knowledge |
| 0x158 | QuickTimeEvent |
| 0x160 | Minimap (solo cliente) |
| 0x168 | ContentsMisc |
| 0x170 | Docking |
| 0x178 | SpecialMode |
| 0x180 | ForceField |
| 0x188 | Faction |
| 0x190 | TargetFormation |
| 0x198 | GamePlayContents |
| **0x1A0** | **TransformSync** |
| 0x1A8 | Navigation |
| 0x1B0 | Input |
| 0x1B8 | Hierarchy |
| 0x1C0 | GamePlayState |

Comprobaciones en vivo:
- **HP en los dos lados** (cadena `[[Status+0x18]+0x58]+0x08`, int64 ×1000): HP 143/300 en
  los dos al mismo tiempo. Los stats 0, 22, 23, 24, 33, 34 y 52 coinciden. Los 17-20 son
  distintos entre cliente y servidor: probablemente valores locales o aleatorios de cada
  lado.
- **Posición en el servidor:** `ServerTransformSyncActorComponent+0xB4` es un vec3 local al
  trozo, igual al global `exe+0x6D77858` menos `exe+0x6D56500` (jugador:
  `(-365.206, 651.431, -802.030)`). Hay valores de rotación en `+0xA8`/`+0xB0`.
  - `ClientTransformSyncActorComponent` tiene **la misma posición en el mismo offset**.
    Comprobado en 8 NPCs emparejados por clave.
  - En uno de ellos, X difería exactamente en **1000** (`-957.43` frente a `42.57`). El
    trozo de referencia puede no coincidir entre las dos mitades.
- `ServerCharacterControlActorComponent` apunta en `+0xC8` a un `SceneObjectServer` y en
  `+0x1C0` al Status del servidor.

### 2.5 Recuento de actores y correspondencia cliente↔servidor

En una toma casi simultánea (escaneo del heap y lectura del contenedor del cliente unos
segundos después):

| Lado | Clase | Vivos |
|---|---|---|
| Cliente | ClientNormalInGameActor | 124–196 (varía con la zona) |
| Cliente | ClientChildOnlyInGameActor | 4: jugador + 3 companions (`B0100002`…`4`) |
| Servidor | ServerNormalInGameActor | 91 con vtable válida en la toma; **todos** con clave presente en el cliente |
| Servidor | ServerChildOnlyInGameActor | 100 slots: 4 en uso (jugador + 3 companions, `+0xA0` = ServerUserActor) y **96 slots libres preasignados** (clave `0xA00000xx`, sin dueño) |
| Servidor | ServerUserActor | 1 |

- Comparando claves: 125 de 128 actores del cliente estaban en el servidor. Los 3 restantes
  aparecieron entre las dos lecturas.
- Los contadores del SAM (`+0x9C`) y del contenedor del cliente (`+0x8C`) se diferencian
  justo en los 96 slots del pool: 257 = 161 + 96.
- **Conclusión:** el servidor **no** simula el mundo entero. Tiene el mismo conjunto de
  actores, acotado alrededor del único jugador.
- Hay que tomar el array del cliente y el escaneo del heap del servidor **a la vez**. Con
  minutos de diferencia la coincidencia baja al 15-30% por la rotación de actores, y el
  pool deja objetos liberados con la vtable intacta (48 "fantasmas" en la segunda toma).

### 2.6 Actividad del canal

Se compararon dos o tres volcados de los mismos objetos con 3-4 s de intervalo
(`diff_snap.py`):
- **Las sesiones no cambian:** las cabeceras de `NwVirtualAsyncSession` y
  `NwVirtualSyncSession`, sus sub-objetos y el pool `NwSyncSession` quedaron iguales. Los
  mensajes no dejan contadores en estos objetos. Probablemente se crean, se despachan y se
  liberan en el mismo tick (no hay instancias de `TrocTr*` en el heap).
- **Cambian:**
  - `AsyncRequestEtc+0x80` (dos u32 que suben, 0x1AED → 0x1B52).
  - El lock del registro del servidor (dueño = ServerWalkThread).
  - Los contadores de SAM (`+0x98`…`+0xD0`) y de `ClientActorContainer` (`+0x50`, `+0x60`,
    `+0x88`…`+0xD0`).
- **Para ver el tráfico hace falta un hook.** Los candidatos naturales son:
  - el despacho del registro, es decir la función que busca el opcode en
    `[session+0xA0]` (la localiza el análisis estático);
  - el envío de la sesión virtual (vtable `exe+0x57260C0` / `exe+0x5726248`).

### 2.7 Otros punteros útiles

- Hay nodos de "interés" o visibilidad de 0x20 bytes en una región `0x3195A1…`. Enlazan
  `ServerField`, `ServerSyncSceneObjectData` y `ServerNormalInGameActor`. Es probablemente el
  área de interés del servidor (qué ve cada sesión). [S]
- Los `ServerSyncSceneObjectData`, `ServerField` y `ServerSyncSceneObjectManager` son la
  parte del servidor que sincroniza objetos de escena (gimmicks).

---

## Qué aplicar al mod

1. **Identificar por clave de actor (`+0x60`), no por puntero.** Es la misma en cliente y
   servidor, y la asigna el servidor. Para sincronizar entre PCs, la clave del host es
   candidata natural de id de red. Falta comprobar que es estable tras recargar una zona.
2. **El servidor manda en los datos** (HP, inventario, posición en TransformSync). Escribir
   solo en el cliente se revierte, como dice Trinity. Para mover un actor de forma
   persistente hay que actuar en el servidor o enviar la `Req` correspondiente.
3. **Los opcodes no son estables entre parches** (según la comparación con Trinity). Hay que
   resolverlos en tiempo de ejecución, por RTTI o por el registro, nunca como constantes.

## Preguntas abiertas

1. Qué significa el campo `X` de las entradas del registro y el `0x3C8D` (15501) que aparece
   en `ServerModuleFrameworkParameter+0x44`, en el registro y en SAM+0xD0.
2. ¿Hay más registros de cliente? 355 de 525 `Ack` y ninguna clase `Noti` encontrada por
   nombre.
3. ¿Para qué sirven en un jugador los 10 `NwSyncSession` no virtuales? ¿Kafka o HTTP, o un
   resto del MMO?
4. Cómo viaja un mensaje: ¿se serializa a bytes y se cifra con el `CryptogramAesGcm` de la
   sesión, o se pasa el objeto? Mirar `PacketBuffer`/`AsyncRequestSend` en el análisis
   estático. Si se serializa, hay un punto natural para copiar el tráfico a otro PC.
5. El flag de realm de Trinity (TLS`+0x1F2`): falta confirmar el offset en 2.03.02 y su
   relación con `ServerRunThread`/`ClientRunThread`.
6. La diferencia de 1000 en X de un NPC: ¿un origen de trozo distinto por lado o un valor
   viejo?
7. Por qué la altura del global (`exe+0x6D77858`) coincide con TransformSync (651.431) y no
   con la física (651.481). ¿Sale del lado de sincronización?

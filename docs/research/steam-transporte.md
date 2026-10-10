# Transporte por Steam (Hito 5): investigación y diseño

Fecha: 2026-10-03 · Build del juego: `CrimsonDesert.exe` 1.0.0.2976, PE timestamp
`0x6AB28F00` (Steam build 25477059). Todo lo de este documento se obtuvo **leyendo
ficheros** (nunca se abrió ni se adjuntó el juego).

Scripts (solo lectura) y salidas en `build\research\steam\`:

| Fichero | Qué hace |
|---|---|
| `pe_steam_scan.py` | Parser PE propio (sin `pefile`): exportaciones de `steam_api64.dll`, versión, importaciones de `steam_api64.dll` en todos los PE de `bin64`, secciones del exe |
| `scan_output.txt` | Salida del anterior |
| `steam_api64_exports.txt` | Las 1.089 exportaciones (nombre, ordinal, RVA) |
| `exe_steam_strings.py` / `exe_strings_output.txt` | Cadenas de versión de interfaces Steam dentro del exe y su contexto |

Reproducir: `python build\research\steam\pe_steam_scan.py > build\research\steam\scan_output.txt`.

## Resultado en vivo (2026-10-03, build 1.0.0.2976)
`SteamTransport` implementado (`src/mod/net/steam_transport.*`) y probado en la pantalla de título
como host:
- las 21 exportaciones se resuelven;
- Steam está listo 0,5 s después de arrancar el mod (`GetHSteamUser`, `BLoggedOn`, SteamID64 y
  nombre de persona correctos);
- la red de relays pasa por los estados 2 → 3 → 100 (lista) en unos 14 s;
- ninguna excepción y el juego sigue normal.
Falta la prueba con un amigo real (una cuenta no puede conectarse consigo misma).

## Resumen

- `steam_api64.dll` del juego exporta **todo** lo necesario para
  `ISteamNetworkingMessages` con la API plana (accesor
  `SteamAPI_SteamNetworkingMessages_SteamAPI_v002`), además de amigos, usuario, utilidades y
  lobbies.
- El juego usa el despacho **clásico** de callbacks (`SteamAPI_RunCallbacks` +
  `SteamAPI_RegisterCallback`). No importa `SteamAPI_ManualDispatch_*`. **Nosotros no
  debemos llamar nunca a `RunCallbacks` ni a `ManualDispatch_Init`.**
- Recomendación: **`ISteamNetworkingMessages`** (datagramas por SteamID) y **no**
  `ISteamNetworkingSockets`. Con Messages se puede trabajar **sin ningún callback**:
  - El invitado envía al host.
  - El host acepta mediante sondeo, llamando a `AcceptSessionWithUser` para los amigos que están
    jugando a Crimson Desert.
  - El estado de la sesión se consulta con `GetSessionConnectionInfo(…, nullptr, nullptr)`.
- Descubrimiento: el invitado pone el SteamID64 del host en `cdmp.ini` o, si lo deja vacío,
  se elige automáticamente al único amigo que esté jugando a Crimson Desert. No hacen falta
  lobbies para dos jugadores.
- Con un solo PC, el transporte Steam **no se puede probar de extremo a extremo**. El UDP y
  `cdmp_peer` se quedan como transporte de desarrollo. Steam se valida con tests con un
  "Steam falso", con una sonda en el juego (SteamID, estado de los relays, amigos) y con la
  prueba real con un amigo.

## 1. `steam_api64.dll` del juego

`D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\steam_api64.dll` (no hay otra copia en
subcarpetas):

| Campo | Valor |
|---|---|
| Tamaño | 319.584 bytes |
| SHA-256 | `670d654aa3255c5061cf0236a1cdbb2f5076cbd5ae44f613b6f4921558e83e2d` |
| PE timestamp | `0x672A8EF7` (2024-11-05) |
| FileVersion | 9.31.86.4 ("Steam Client API", Valve, `steam_api.dll`) |
| Exportaciones | 1.089 |
| SDK aproximado | ≥ 1.60, porque exporta `SteamAPI_SteamTimeline_v004` |

Las cabeceras con las que se compiló el juego son anteriores: el bloque de versiones de
`SteamAPI_InitEx` que hay en el exe no incluye `STEAMTIMELINE`, así que corresponde a un SDK
de la 1.58 o la 1.59. Para nosotros no cambia nada.

### Exportaciones que necesitamos (todas presentes, nombres exactos)

Accesores de interfaces:
- `SteamAPI_SteamNetworkingMessages_SteamAPI_v002` (ord 1040). **Ojo:** el nombre lleva
  `_SteamAPI_`. La variante `SteamAPI_SteamNetworkingMessages_v002` es la de
  GameNetworkingSockets standalone y **no existe** aquí.
- `SteamAPI_SteamNetworkingSockets_SteamAPI_v012`, `SteamAPI_SteamNetworkingUtils_SteamAPI_v004`
- `SteamAPI_SteamFriends_v017`, `SteamAPI_SteamUser_v023`, `SteamAPI_SteamUtils_v010`,
  `SteamAPI_SteamMatchmaking_v009`, `SteamAPI_SteamApps_v008`
- `SteamAPI_SteamNetworking_v006` (la API P2P antigua, obsoleta)

ISteamNetworkingMessages:
- `SteamAPI_ISteamNetworkingMessages_SendMessageToUser`, `…_ReceiveMessagesOnChannel`,
  `…_AcceptSessionWithUser`, `…_CloseSessionWithUser`, `…_CloseChannelWithUser`,
  `…_GetSessionConnectionInfo`
- `SteamAPI_SteamNetworkingMessage_t_Release`
- `SteamAPI_SteamNetworkingIdentity_SetSteamID64` / `_GetSteamID64` / `_IsInvalid` / `_ToString` …

ISteamNetworkingUtils:
- `SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess`, `…_GetRelayNetworkStatus`
- `…_SetGlobalCallback_MessagesSessionRequest`, `…_SetGlobalCallback_MessagesSessionFailed`
  (en la nota sobre la recepción de `SessionRequest`, en la sección 2, se explica por qué no conviene usarlas)
- `…_SetConfigValue`, `…_SetGlobalConfigValueInt32`, `…_GetLocalTimestamp`

ISteamNetworkingSockets (alternativa, no recomendada):
- `…_ConnectP2P`, `…_CreateListenSocketP2P`, `…_AcceptConnection`, `…_GetConnectionInfo`,
  `…_ReceiveMessagesOnConnection`, `…_SendMessageToConnection`, `…_CloseConnection`,
  `…_CreatePollGroup`, `…_ReceiveMessagesOnPollGroup`, `…_CreateSocketPair`

Usuario, amigos y utilidades:
- `SteamAPI_ISteamUser_GetSteamID`, `SteamAPI_ISteamUser_BLoggedOn`
- `SteamAPI_ISteamFriends_GetPersonaName`, `…_GetFriendCount`, `…_GetFriendByIndex`,
  `…_GetFriendPersonaName`, `…_GetFriendGamePlayed`, `…_HasFriend`, `…_SetRichPresence`,
  `…_ClearRichPresence`, `…_GetFriendRichPresence`, `…_ActivateGameOverlayInviteDialog`,
  `…_ActivateGameOverlayInviteDialogConnectString`
- `SteamAPI_ISteamUtils_GetAppID`, `…_IsAPICallCompleted`, `…_GetAPICallResult`,
  `…_GetAPICallFailureReason`

Lobbies:
- `SteamAPI_ISteamMatchmaking_CreateLobby`, `…_JoinLobby`, `…_LeaveLobby`,
  `…_GetLobbyOwner`, `…_GetNumLobbyMembers`, `…_GetLobbyMemberByIndex`, `…_SetLobbyData`,
  `…_GetLobbyData`, `…_InviteUserToLobby`, `…_SetLobbyJoinable`, `…_SetLobbyType`,
  `…_RequestLobbyList`

Núcleo:
- `SteamAPI_GetHSteamUser`, `SteamAPI_GetHSteamPipe`, `SteamAPI_IsSteamRunning`,
  `SteamAPI_RunCallbacks`, `SteamAPI_RegisterCallback`, `SteamAPI_UnregisterCallback`,
  `SteamAPI_RegisterCallResult`, `SteamAPI_UnregisterCallResult`,
  `SteamAPI_ReleaseCurrentThreadMemory`, `SteamAPI_ManualDispatch_Init` / `_RunFrame` /
  `_GetNextCallback` / `_FreeLastCallback` / `_GetAPICallResult`, `SteamAPI_InitFlat`,
  `SteamInternal_FindOrCreateUserInterface`, `SteamInternal_ContextInit`

Cadenas de versión dentro de la DLL: `SteamNetworkingMessages002`,
`SteamNetworkingSockets012`, `SteamNetworkingUtils004`, `SteamFriends017`, `SteamUser023`,
`SteamUtils010`, `SteamMatchMaking009`…

## 2. Usar el Steam del juego desde la DLL inyectada

### Lo que hace el juego (tabla de importaciones de `CrimsonDesert.exe`)
`CrimsonDesert.exe` importa exactamente 9 funciones de `steam_api64.dll`:

```
SteamInternal_ContextInit, SteamInternal_FindOrCreateUserInterface,
SteamAPI_RegisterCallback, SteamAPI_UnregisterCallback, SteamInternal_SteamAPI_Init,
SteamAPI_RestartAppIfNecessary, SteamAPI_Shutdown, SteamAPI_RunCallbacks, SteamAPI_GetHSteamUser
```

- `SteamInternal_SteamAPI_Init` es lo que hay dentro del `SteamAPI_InitEx()` inline del SDK
  1.58 o posterior. El bloque de versiones que le pasa, en el offset de fichero `0x57D59C0`,
  incluye `SteamNetworkingMessages002`, `SteamNetworkingSockets012` y `SteamNetworkingUtils004`.
  Es decir, el juego **verifica al iniciar** que el cliente de Steam soporta esas versiones.
  Que aparezcan esas cadenas **no** quiere decir que el juego use la red de Steam.
- Usa el despacho **clásico**: `RegisterCallback` + `RunCallbacks`. No importa
  `SteamAPI_ManualDispatch_*` ni aparece esa cadena en el exe.
- Usa los accesores C++ (`FindOrCreateUserInterface`), no la API plana.
- Ningún otro PE de `bin64` importa `steam_api64.dll`.
- Cerca de las cadenas de idiomas, en la zona protegida, aparecen `SteamClient015`,
  `SteamUser017` y `STEAMAPPS_INTERFACE_VERSION006`. Lo más probable es que sea Denuvo
  hablando con `steamclient` por su cuenta, por un camino independiente del nuestro.
- No aparecen `SteamNetworkingMessagesSessionRequest`, `P2PSessionRequest`,
  `GameLobbyJoinRequested` ni `GameRichPresenceJoinRequested`. Para un juego de un jugador
  es lo esperable: no usa la red de Steam.

### Reglas para el mod
1. **Usar la DLL ya cargada.** `GetModuleHandleW(L"steam_api64.dll")` y `GetProcAddress`.
   Nunca `LoadLibrary` (para no cargar una segunda copia) ni `FreeLibrary`. El exe la importa
   estáticamente, así que ya está cargada cuando arranca nuestro hilo.
2. **No inicializar ni cerrar Steam.** Nada de `SteamAPI_Init*` ni de `SteamAPI_Shutdown`.
   Hay que esperar a que el juego lo haya hecho: sondear cada 500 ms hasta que
   `SteamAPI_GetHSteamUser() != 0`, `SteamAPI_SteamUser_v023() != nullptr` y
   `BLoggedOn() == true`, con un tiempo límite de unos 120 s. Si no se cumple, el transporte
   Steam queda desactivado con un mensaje en el log. En modo offline, `BLoggedOn` es falso y
   no hay red.
3. **No tocar el bucle de callbacks del juego:**
   - **Prohibido `SteamAPI_RunCallbacks` desde nuestro hilo:** ejecutaría los handlers del
     juego en nuestro hilo, con condiciones de carrera dentro del código del juego.
   - **Prohibido `SteamAPI_ManualDispatch_Init`:** cambia el pipe entero a despacho manual y
     el juego dejaría de recibir callbacks (logros, overlay, DLC…).
4. **Hilo worker.** `ISteamNetworkingMessages`, `ISteamFriends` e `ISteamUser` se pueden
   llamar desde nuestro hilo de red. Hay que llamar a
   `SteamAPI_ReleaseCurrentThreadMemory()` cada pocos segundos y al salir: la documentación
   dice que solo es automático en el hilo que llama a `RunCallbacks`. Ni en `DllMain` ni en el
   hilo del juego.
5. **Protección.** Las llamadas a Steam no son lecturas de memoria del juego, pero un crash
   igualmente lo recogería sentry. Hay que envolver cada llamada a la API plana en un *thunk*
   con `__try/__except`. A la primera excepción, el transporte se desactiva para siempre y se
   anota en el log.

### Recibir `SteamNetworkingMessagesSessionRequest_t` sin controlar el bucle de callbacks
Según la cabecera (`isteamnetworkingmessages.h`):
- *"Sending a message to a host will also implicitly accept any incoming connection from
  that host."*
- `AcceptSessionWithUser` devuelve `false` si no hay nada pendiente y `true` si hay una
  sesión pendiente o activa.
- Si el remitente sigue enviando, la petición se vuelve a publicar periódicamente.

Opciones, de la más robusta a la menos:

| Opción | Cómo | Valoración |
|---|---|---|
| **A. Sondeo sin callbacks** (recomendada) | Cada 500 ms, el host llama a `AcceptSessionWithUser` para cada amigo inmediato con `GetFriendGamePlayed().m_gameID == 3321460`, o solo para `peer_steamid` si está configurado. El invitado conoce al host y **le envía primero**, con lo que acepta implícitamente. | Sin vtables ni hilos ajenos. Solo acepta a amigos que estén jugando. Nuestro `Hello` se reintenta cada segundo, así que la latencia extra es como mucho de unos 0,5 s. |
| B. Ambos conocen el ID del otro | Los dos envían `Hello` al otro y la aceptación es implícita en ambos sentidos. | Trivial, pero obliga a configurar dos IDs. Sirve como plan de reserva. |
| C. `SteamAPI_RegisterCallback` con nuestro objeto | Declarar una clase C++ con **la misma declaración** que `CCallbackBase` del SDK: `virtual Run(void*)`, `virtual Run(void*, bool, uint64)`, `virtual int GetCallbackSizeBytes()`, `uint8 m_nCallbackFlags`, `int m_iCallback`. Como el compilador es MSVC, igual que el del juego, la vtable sale idéntica. Registrar `k_iCallback = 1251` con un tamaño de 136. El handler se ejecuta en el hilo del juego que llama a `RunCallbacks`: solo debe copiar el SteamID a un slot atómico. | Funciona según el precedente de abajo, pero: hay que desregistrarlo antes de descargar la DLL, el registro compite con `RunCallbacks` y es código que corre en el hilo del juego. Solo si A no basta. |
| D. `SetGlobalCallback_MessagesSessionRequest` | Puntero a función global. | **Evitar.** El mod de Wartales (UberMorgott/wartales-mp v0.1.2) informa de que, dentro de un juego con el cliente de Steam, el cliente **nunca invocó** estos callbacks aunque `SetGlobalCallback_*` devolviera `true`, y pasó a la opción C. |
| E. `ISteamNetworkingSockets` P2P | `ConnectP2P` + sondear `GetConnectionInfo` funciona en el lado que conecta. Pero el lado que escucha **necesita** `SteamNetConnectionStatusChangedCallback_t` (1221) para conocer el `HSteamNetConnection` entrante y llamar a `AcceptConnection`. Eso devuelve el problema a C o D. | Más complejo y sin ventaja: nuestra fiabilidad ya la da `ReliableChannel`. Hay que actualizar el ROADMAP, que dice Sockets. |

## 3. Cómo se encuentran los dos jugadores

| Opción | Pros | Contras |
|---|---|---|
| (a) SteamID64 del host en `cdmp.ini` del invitado | Lo más simple y determinista; ningún callback | Hay que copiar un número de 17 cifras. Lo mitigamos registrándolo en el log, mostrándolo en el overlay y con una tecla que lo copie al portapapeles. |
| (b) Lista de amigos | `GetFriendCount(k_EFriendFlagImmediate=0x04)` + `GetFriendByIndex` + `GetFriendGamePlayed`: amigos **jugando ahora** a la app 3321460. Si hay exactamente uno, se elige automáticamente. Sin callbacks. | Si hay varios amigos jugando, hay que elegir: por nombre (`peer_name=` en el ini, comparado con `GetFriendPersonaName`) o con una lista en el overlay. Buscar solo por nombre es ambiguo y el nombre se puede cambiar. |
| (b+) Rich presence | El host hace `SetRichPresence("cdmp_host","1")` y el invitado filtra por `GetFriendRichPresence(amigo,"cdmp_host")`, que funciona porque los dos están en el mismo juego. | Escribe en la presencia del juego. Hay que usar una clave propia y no tocar `steam_display`/`status`. Opcional. |
| (c) Lobby (`ISteamMatchmaking`) | Invitación desde el overlay | `CreateLobby` y `JoinLobby` son CallResults: se podrían sondear con `ISteamUtils::IsAPICallCompleted`/`GetAPICallResult`, pero no está claro que `RunCallbacks` del juego no consuma el resultado antes. Además, aceptar una invitación desde el overlay llega como callback (`GameLobbyJoinRequested_t`), que solo se recibe con la opción C. Para 2 jugadores es complejidad sin beneficio. |

**Recomendación:** (a) + (b) juntas:
- El invitado usa `peer_steamid=` si lo ha rellenado.
- Si lo deja vacío, se elige al **único amigo que esté jugando a Crimson Desert**.
- Si hay varios, el log y el overlay muestran la lista con nombre y SteamID para copiarlo en el ini.
- El host no configura nada (`role=host`) y solo acepta a amigos inmediatos que estén jugando,
  o solo a `peer_steamid` si lo ha puesto.

## 4. Diseño del transporte

### 4.1 Interfaz común (en `src/common/`, sin dependencias de Steam)

```cpp
namespace cdmp::net {
// Dirección de un jugador, independiente del transporte. Nunca es una dirección de memoria.
struct PeerAddr {
    enum class Kind : uint8_t { None, Udp, Steam };
    Kind kind = Kind::None;
    uint64_t value = 0;  // Udp: (ip_be << 16) | port   Steam: SteamID64
    bool valid() const { return kind != Kind::None; }
    bool operator==(const PeerAddr&) const = default;
    std::string str() const;  // "udp:127.0.0.1:37015" / "steam:<SteamID64>"
    static std::optional<PeerAddr> parse(std::string_view);  // mismo formato
};

class Transport {
public:
    virtual ~Transport() = default;
    // Datagrama ≤ proto::kMaxPacketSize (1200). No fiable, sin orden garantizado.
    virtual bool send(const PeerAddr& to, std::span<const uint8_t> data) = 0;
    // Espera hasta timeout_ms; devuelve el tamaño o nullopt.
    virtual std::optional<size_t> recv(PeerAddr& from, std::span<uint8_t> buf, int timeout_ms) = 0;
    // Mantenimiento periódico (aceptar sesiones, liberar memoria de hilo, estado).
    virtual void poll(uint32_t now_ms) {}
    virtual std::string status() const = 0;  // para el log y el HUD
};
}
```

- `UdpTransport`: adaptador de `UdpSocket`, sin cambiar su comportamiento. `from`/`to` se
  convierten a `Endpoint`.
- `SteamTransport` (en `src/mod/net/`, porque solo existe dentro del proceso del juego).
- `run_network` pasa a usar `Transport&` y `PeerAddr`. La comprobación `from == *peer` se
  queda, ahora con `PeerAddr`.

### 4.2 `SteamTransport` sobre `ISteamNetworkingMessages`
- **Envío:** `SendMessageToUser(id(to), data, n, flags, kChannel)` con
  `flags = k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession` (= 1 | 32 = 33).
  - Sin `NoNagle`, cada mensaje puede esperar hasta unos 5 ms para agruparse.
  - Sin `AutoRestartBrokenSession`, una sesión rota (por ejemplo, porque el amigo reinició el
    juego) devuelve `k_EResultNoConnection` (3) para siempre.
  - **No** usar `NoDelay`, que descarta el mensaje si no puede salir en ese momento.
  - Todo va por el canal no fiable: la fiabilidad de `Spawn` ya la pone nuestro `ReliableChannel`,
    así que el protocolo es idéntico en UDP y en Steam y no cambia `kVersion`.
- **Recepción:** `ReceiveMessagesOnChannel(kChannel, msgs, 16)`.
  - Por cada mensaje: se valida que `m_identityPeer.m_eType == 16` (SteamID) y que
    `m_cbSize ≤ 1200`, se copia a una cola interna y se llama a `SteamAPI_SteamNetworkingMessage_t_Release`.
  - `recv()` devuelve uno de la cola. Si está vacía, repite el sondeo con `Sleep(1)` hasta
    `timeout_ms`. El bucle actual ya usa 5 ms.
- **Canal:** `kChannel = 0x43` ('C'), configurable. El juego no usa Messages, pero no cuesta nada evitar el 0.
- **Tamaño:** los datagramas de hasta 1200 B son válidos (el máximo es 512 KiB), pero los que
  pasen de unos 1.150 B pueden fragmentarse en dos paquetes. Nuestro máximo real es de unos
  1.130 B (`kRelChunk` 1100 + cabeceras), así que no cambia nada.
- **`poll()` cada 500 ms:**
  - Host: aceptar sesiones (opción A).
  - `SteamAPI_ReleaseCurrentThreadMemory()`.
  - Consultar `GetSessionConnectionInfo(peer, nullptr, nullptr)` para el estado (1 Connecting,
    2 FindingRoute, 3 Connected, 4 ClosedByPeer, 5 ProblemDetectedLocally), que va al log y al HUD.
  - `GetRelayNetworkStatus(nullptr)` hasta llegar a `Current` (100).
- **Arranque:** `InitRelayNetworkAccess()`, que solo hace que la primera conexión sea más rápida.
- **Cierre:** `CloseSessionWithUser(peer)`.
- **Testeable sin Steam:** `SteamTransport` recibe una tabla de punteros a función
  (`SteamApi`). En el juego la rellena `GetProcAddress`; en `cdmp_tests`, un Steam falso en
  memoria con dos identidades que se entregan mensajes entre sí.

### 4.3 Host e invitado en lugar del relay
Hoy `ClientSession` siempre inicia la conexión (`Hello` hasta recibir `Welcome`) y
`cdmp_peer` responde y reenvía. Con dos jugadores no hace falta relay: el host **es** el otro
extremo.

- **Rol en `ClientSession`:** se añade `Role { Initiator, Responder }`.
  - `Initiator` (invitado, y también contra `cdmp_peer`): igual que ahora.
  - `Responder` (host): en `Connecting` no envía `Hello`. Al recibir `Hello`, responde
    `Welcome{player_name}`, pasa a `Connected` y guarda `peer_name`.
  - Si llega `Hello` estando ya en `Connected`, el otro extremo ha reiniciado su sesión
    (solo envía `Hello` en `Connecting`). Entonces el host hace `reset` (canal fiable y estado
    remoto) y responde `Welcome`, con lo que `CoopLink` vuelve a anunciar el `Spawn`.
  - `Ping`/`Pong`, el tiempo límite y los datos de juego funcionan igual en los dos roles: hoy
    `ClientSession` ya responde `Pong` a `Ping`.
- **Bloqueo del par (host):** `run_network` empieza sin `peer`. El primer datagrama que se
  decodifica como `Hello`, viene de una dirección aceptable (amigo jugando o `peer_steamid`) y
  trae el mismo `mod_build` fija `peer = from`. Si la sesión cae a `Connecting` por tiempo
  límite o por `Bye`, se libera `peer`. Con ese límite de dos jugadores, cualquier `Hello`
  de otro par mientras hay uno conectado se descarta.
- **Formato del cable:** no cambia. `kVersion` se queda en 3; solo cambia la semántica de quién
  responde. Hay que subir `kModBuild` para que el log distinga las versiones.
- **`cdmp_peer`** se queda para el desarrollo en un PC (eco con un jugador, relay con dos). El
  rol `Responder` también se puede probar en un PC **por UDP**: el juego como
  `role=host transport=udp` escuchando en `37015`, y `cdmp_host.exe` (datos sintéticos) como
  invitado contra `127.0.0.1:37015`. Así se prueba la lógica P2P sin relay.

### 4.4 `cdmp.ini` propuesto
```ini
[net]
transport=udp          ; udp | steam
role=guest             ; guest (inicia) | host (responde)
peer_host=127.0.0.1    ; solo UDP
peer_port=37015        ; UDP: destino (guest) o puerto de escucha (host)

[steam]
peer_steamid=          ; guest: SteamID64 del host; vacío = el único amigo jugando a CD
                       ; host: vacío = aceptar a cualquier amigo inmediato jugando a CD
peer_name=             ; alternativa a peer_steamid: nombre exacto del amigo
channel=67
rich_presence=0        ; 1 = el host anuncia cdmp_host=1 (descubrimiento b+)
```

### 4.5 Declaraciones propias (no hay Steamworks SDK en el repo)
Comprobado: ni `third_party/` ni `reference/` contienen cabeceras de Steamworks
(`steam_api*.h`, `isteamnetworking*.h`, `steamnetworkingtypes.h`). Se declara lo mínimo en
`src/mod/net/steam_flat.h`, con `static_assert` de tamaños y offsets. Todas las estructuras
de red usan `#pragma pack(push, 8)` en Windows (`VALVE_CALLBACK_PACK_LARGE`).

```cpp
#pragma pack(push, 8)
using HSteamUser = int32_t;
using HSteamNetConnection = uint32_t;
using SteamNetworkingMicroseconds = int64_t;

enum ESteamNetworkingIdentityType : int32_t { kIdentityInvalid = 0, kIdentitySteamID = 16 };

struct SteamNetworkingIdentity {          // 136 bytes
    int32_t  m_eType;                     // ESteamNetworkingIdentityType
    int32_t  m_cbSize;                    // 8 para SteamID
    union {
        uint64_t m_steamID64;
        uint32_t m_reserved[32];          // fija el tamaño de la unión en 128
    };
};
static_assert(sizeof(SteamNetworkingIdentity) == 136);

struct SteamNetworkingMessage_t;
struct SteamNetworkingMessage_t {         // 216 bytes; solo leemos los marcados con *
    void*    m_pData;                     //   0 *
    int32_t  m_cbSize;                    //   8 *
    HSteamNetConnection m_conn;           //  12
    SteamNetworkingIdentity m_identityPeer; // 16 *
    int64_t  m_nConnUserData;             // 152
    SteamNetworkingMicroseconds m_usecTimeReceived; // 160
    int64_t  m_nMessageNumber;            // 168
    void (*m_pfnFreeData)(SteamNetworkingMessage_t*); // 176
    void (*m_pfnRelease)(SteamNetworkingMessage_t*);  // 184
    int32_t  m_nChannel;                  // 192 *
    int32_t  m_nFlags;                    // 196
    int64_t  m_nUserData;                 // 200
    uint16_t m_idxLane;                   // 208
    uint16_t _pad1__;                     // 210
};
static_assert(offsetof(SteamNetworkingMessage_t, m_identityPeer) == 16);
static_assert(offsetof(SteamNetworkingMessage_t, m_nChannel) == 192);
static_assert(sizeof(SteamNetworkingMessage_t) == 216);

struct FriendGameInfo_t {                 // 24 bytes
    uint64_t m_gameID;                    // CGameID; app normal => == AppID (3321460)
    uint32_t m_unGameIP;
    uint16_t m_usGamePort, m_usQueryPort;
    uint64_t m_steamIDLobby;
};
static_assert(sizeof(FriendGameInfo_t) == 24);

struct SteamNetworkingMessagesSessionRequest_t { SteamNetworkingIdentity m_identityRemote; }; // 136, k_iCallback 1251
#pragma pack(pop)

// Constantes
constexpr int k_nSteamNetworkingSend_Unreliable = 0, k_nSteamNetworkingSend_NoNagle = 1,
              k_nSteamNetworkingSend_UnreliableNoNagle = 1, k_nSteamNetworkingSend_NoDelay = 4,
              k_nSteamNetworkingSend_UnreliableNoDelay = 5, k_nSteamNetworkingSend_Reliable = 8,
              k_nSteamNetworkingSend_ReliableNoNagle = 9, k_nSteamNetworkingSend_UseCurrentThread = 16,
              k_nSteamNetworkingSend_AutoRestartBrokenSession = 32;
constexpr int k_iSteamNetworkingMessagesCallbacks = 1250;      // SessionRequest 1251, SessionFailed 1252
constexpr int k_EFriendFlagImmediate = 0x04;
constexpr int k_EResultOK = 1, k_EResultFail = 2, k_EResultNoConnection = 3,
              k_EResultInvalidParam = 8, k_EResultLimitExceeded = 25;
// ESteamNetworkingConnectionState: 0 None, 1 Connecting, 2 FindingRoute, 3 Connected,
//   4 ClosedByPeer, 5 ProblemDetectedLocally
// ESteamNetworkingAvailability: -102 CannotTry, -101 Failed, -100 Previously, -10 Retrying,
//   0 Unknown, 1 NeverTried, 2 Waiting, 3 Attempting, 100 Current
```

Firmas de la API plana. Las referencias `const T&` del SDK se declaran como `const T*`, que en
x64 MSVC es el mismo ABI; solo hay una convención de llamada.

```cpp
struct ISteamNetworkingMessages; struct ISteamNetworkingUtils; struct ISteamUser; struct ISteamFriends;

using Fn_GetHSteamUser          = HSteamUser (*)();                                  // SteamAPI_GetHSteamUser
using Fn_ReleaseThreadMemory    = void (*)();                                        // SteamAPI_ReleaseCurrentThreadMemory
using Fn_Messages               = ISteamNetworkingMessages* (*)();                   // SteamAPI_SteamNetworkingMessages_SteamAPI_v002
using Fn_Utils                  = ISteamNetworkingUtils* (*)();                      // SteamAPI_SteamNetworkingUtils_SteamAPI_v004
using Fn_User                   = ISteamUser* (*)();                                 // SteamAPI_SteamUser_v023
using Fn_Friends                = ISteamFriends* (*)();                              // SteamAPI_SteamFriends_v017

using Fn_SendMessageToUser      = int32_t (*)(ISteamNetworkingMessages*, const SteamNetworkingIdentity*,
                                              const void* data, uint32_t size, int flags, int remote_channel);
using Fn_ReceiveMessagesOnChannel = int (*)(ISteamNetworkingMessages*, int local_channel,
                                            SteamNetworkingMessage_t** out, int max);
using Fn_AcceptSessionWithUser  = bool (*)(ISteamNetworkingMessages*, const SteamNetworkingIdentity*);
using Fn_CloseSessionWithUser   = bool (*)(ISteamNetworkingMessages*, const SteamNetworkingIdentity*);
using Fn_GetSessionConnectionInfo = int (*)(ISteamNetworkingMessages*, const SteamNetworkingIdentity*,
                                            void* info /*nullptr*/, void* status /*nullptr*/);
using Fn_MessageRelease         = void (*)(SteamNetworkingMessage_t*);               // SteamAPI_SteamNetworkingMessage_t_Release

using Fn_InitRelayNetworkAccess = void (*)(ISteamNetworkingUtils*);
using Fn_GetRelayNetworkStatus  = int (*)(ISteamNetworkingUtils*, void* details /*nullptr*/);

using Fn_GetSteamID             = uint64_t (*)(ISteamUser*);                         // SteamAPI_ISteamUser_GetSteamID
using Fn_BLoggedOn              = bool (*)(ISteamUser*);                             // SteamAPI_ISteamUser_BLoggedOn

using Fn_GetPersonaName         = const char* (*)(ISteamFriends*);                   // UTF-8
using Fn_GetFriendCount         = int (*)(ISteamFriends*, int flags);
using Fn_GetFriendByIndex       = uint64_t (*)(ISteamFriends*, int index, int flags);
using Fn_GetFriendPersonaName   = const char* (*)(ISteamFriends*, uint64_t steamid);
using Fn_GetFriendGamePlayed    = bool (*)(ISteamFriends*, uint64_t steamid, FriendGameInfo_t* out);
using Fn_HasFriend              = bool (*)(ISteamFriends*, uint64_t steamid, int flags);
using Fn_SetRichPresence        = bool (*)(ISteamFriends*, const char* key, const char* value);   // opcional
using Fn_GetFriendRichPresence  = const char* (*)(ISteamFriends*, uint64_t steamid, const char* key); // opcional

// Solo para la opción C:
//   void SteamAPI_RegisterCallback(CCallbackBase*, int iCallback);
//   void SteamAPI_UnregisterCallback(CCallbackBase*);
```

Para la identidad hay que poner a cero los 136 bytes y luego asignar `m_eType = 16`,
`m_cbSize = 8` y `m_steamID64 = id`. Es lo mismo que hace `SetSteamID64` inline en el SDK.
También se puede llamar a `SteamAPI_SteamNetworkingIdentity_SetSteamID64`.

## 5. Riesgos

| Riesgo | Valoración / mitigación |
|---|---|
| **Denuvo / antitrampas** | Crimson Desert no tiene antitrampas de kernel (es de un jugador). Denuvo habla con Steam por su cuenta (`SteamClient015`/`SteamUser017`). Nosotros **no ponemos hooks** en Steam: solo llamamos a exportaciones públicas, igual que cualquier overlay o herramienta. El riesgo es bajo. Nunca hay que parchear `steam_api64.dll` ni `steamclient64.dll`. |
| Romper los callbacks del juego | Solo pasa si llamamos a `RunCallbacks` o a `ManualDispatch_Init`, y ambas están prohibidas (sección 2). Con la opción A no registramos nada. |
| Crash dentro de Steam → informe a Pearl Abyss | Thunks con SEH; a la primera excepción, el transporte se desactiva. Antes del primer uso, la sonda del paso 0 solo lee y registra en el log. |
| La red de Steam a nombre de la app 3321460 | SDR para P2P está disponible para todos los juegos de Steam sin configuración ("all you need to do … is to use APIs such as … ConnectP2P"). Valve podría ver tráfico P2P en una app que no lo usaba; es inofensivo. |
| Límite de ancho de banda | Por defecto, `SendRate` es de 256 KB/s y el búfer de envío de 512 KB. Nuestro tráfico es de unos 1,2 KB/s de `PlayerState` más eventos (514/725 B, unos 10–30 Hz, ≤ 20 KB/s). Un `Spawn` de hasta 256 KB tarda alrededor de 1 s en ráfaga, aceptable. No hay que tocar la configuración **global**, que es compartida con el juego. Si `Send` devuelve `k_EResultLimitExceeded` (25), hay que contarlo y registrarlo. |
| Uso de red del propio juego | No usa la red de Steam (ver sección 2). Su "cliente/servidor" Troc es un loopback en memoria, sin relación con esto. |
| Cortafuegos de Windows | `steamclient` abre sockets UDP dentro de `CrimsonDesert.exe` y puede salir el aviso del cortafuegos para el exe del juego. Aunque se rechace, el relay SDR sigue funcionando, porque el tráfico es saliente. Hay que explicarlo en el `LEEME` del kit. |
| Privacidad de la IP | Con ICE, la conexión directa revela la IP al otro jugador. Steam aplica la preferencia del usuario ("permitir conexiones P2P directas"), y con el relay la IP queda oculta. |
| Steam offline o sin sesión | `BLoggedOn() == false`: se escribe un aviso en el log y no se conecta. Opcionalmente, se cae a UDP. |
| Sesión inactiva | Messages cierra las sesiones inactivas al cabo de unos minutos, sin callback. Nuestro `Ping` es de 1 Hz y `AutoRestartBrokenSession` lo cubre. |
| Desconexión del par | Nuestro tiempo límite de 5 s ya lo gestiona. `GetSessionConnectionInfo` da el motivo para el log. |

### Pruebas con un solo PC
- **Una cuenta no puede jugar contra sí misma.** No se puede tener dos clientes de Steam con
  sesión iniciada en el mismo PC. Y un segundo proceso con `SteamAPI_Init` (por ejemplo,
  `cdmp_peer` con un `steam_appid.txt`) tendría **el mismo SteamID**. No está comprobado si
  Messages permite enviarse mensajes a uno mismo, y es probable que no. Lo comprobará la sonda
  del paso 0 (enviar un mensaje a su propio SteamID y registrar el `EResult` y el estado de la
  sesión), aunque no hay que contar con ello.
- Las sesiones P2P van ligadas al AppID: una herramienta con la app 480 (Spacewar) no podría
  hablar con el juego.
- **Estrategia:**
  1. UDP + `cdmp_peer` sigue siendo el transporte de desarrollo.
  2. El rol host/invitado se prueba por UDP en un PC (juego + `cdmp_host.exe`, o dos
     `cdmp_host.exe` en `build\e2e\`).
  3. `SteamTransport` se prueba con un Steam falso en `cdmp_tests`.
  4. Sonda en el juego, solo de lectura (paso 0).
  5. Prueba real con un amigo, con el kit de `dist/`.

## 6. Plan de implementación paso a paso

0. **Sonda en el juego (solo lectura, sin enviar nada).** Con `[steam] probe=1`, el hilo de red:
   - resuelve las exportaciones de la sección 4.5 y registra en el log las que falten;
   - espera a `GetHSteamUser != 0` y a `BLoggedOn`, y registra el SteamID64, el nombre de
     persona y el AppID (`ISteamUtils::GetAppID` debe dar 3321460);
   - llama a `InitRelayNetworkAccess` y registra la evolución de `GetRelayNetworkStatus` hasta
     `Current`;
   - lista los amigos inmediatos que están jugando a la app 3321460;
   - llama a `ReleaseCurrentThreadMemory`.

   Opcionalmente: un único `SendMessageToUser` a su propio SteamID, registrando el `EResult` y
   `GetSessionConnectionInfo`. Criterio de éxito: ningún crash ni tirón, y el relay en `Current`.
   *(No escribe en la memoria del juego, pero se hace igualmente la copia de las partidas en
   `backups/`.)*
1. **`Transport` + `PeerAddr` + `UdpTransport`** en `src/common/`. `run_network` pasa a usar la
   interfaz, sin cambio de comportamiento. Tests de `PeerAddr::parse/str`. Hay que pasar
   `build.ps1` y el e2e actual con `cdmp_peer`.
2. **Rol `Responder` en `ClientSession`** + bloqueo del par en `run_network`. Tests unitarios:
   - handshake host/invitado;
   - pérdida de `Welcome` (el invitado reintenta y el host vuelve a responder);
   - `Hello` estando conectado, que reinicia el canal fiable;
   - un tercer par ignorado;
   - tiempo límite que libera el par.

   E2E en un PC: dos `cdmp_host.exe` (host y guest) por UDP loopback, sin `cdmp_peer`.
   Configuración: `[net] role`, y `peer_port` como puerto de escucha del host. Subir `kModBuild`.
3. **`steam_flat.h` + `SteamApi`** (tabla de punteros resuelta con `GetModuleHandle` y
   `GetProcAddress`, thunks con SEH, espera a que Steam esté listo) + **`SteamTransport`**
   (envío con flags 33, recepción y liberación, cola, canal). Tests con un Steam falso:
   - direccionamiento por SteamID;
   - descarte de identidades que no son SteamID y de tamaños mayores de 1200;
   - `Release` siempre llamado;
   - tiempo límite de `recv`.
4. **Descubrimiento y aceptación:**
   - Invitado: `peer_steamid` / `peer_name` / el único amigo que está jugando.
   - Host: sondeo de `AcceptSessionWithUser` cada 500 ms para amigos jugando (opción A),
     filtrado por `HasFriend(Immediate)`.
   - El overlay muestra el SteamID propio, el rol, el estado de la sesión
     (`GetSessionConnectionInfo`), el estado del relay y la lista de amigos jugando.
   - Una tecla copia el SteamID propio al portapapeles.
   - El transporte se elige en `cdmp.ini`, con `transport=udp` por defecto.
5. **Kit y documentación:** actualizar `kit/LEEME.txt` con cómo hacer de host y de invitado,
   el aviso del cortafuegos y qué ID copiar. Actualizar el `ROADMAP` (Messages en lugar de
   Sockets y lobby).
6. **Prueba real con un amigo:** los dos con `transport=steam`; un PC con `role=host` y el otro
   con `role=guest`. Medir:
   - el RTT de nuestro `Ping` y el estado de la sesión;
   - envíos con `LimitExceeded`;
   - la llegada del `Spawn` y la fluidez de los `GameEvent`.

   Revisar `cdmp.log` en ambos PCs.
7. **Opcional, solo si hace falta:**
   - opción C (`RegisterCallback` 1251) si el sondeo de aceptación resulta lento o poco fiable;
   - rich presence `cdmp_host` para el descubrimiento automático con varios amigos jugando;
   - invitación con `ActivateGameOverlayInviteDialogConnectString`;
   - lobbies solo para más de 2 jugadores.

## Fuentes
- Cabeceras de Valve (GameNetworkingSockets, la misma API que el cliente de Steam):
  - [steamnetworkingtypes.h](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/steamnetworkingtypes.h):
    estructuras, flags de envío, `pragma pack`, estados, valores por defecto de `SendRate`/`SendBufferSize`.
  - [isteamnetworkingmessages.h](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/isteamnetworkingmessages.h):
    aceptación implícita, `AcceptSessionWithUser`, `GetSessionConnectionInfo` con `nullptr`, tiempo límite de las sesiones.
  - [steamnetworkingsockets_flat.h](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/steamnetworkingsockets_flat.h):
    firmas planas.
- Documentación de Steamworks:
  - [Steam Datagram Relay](https://partner.steamgames.com/doc/features/multiplayer/steamdatagramrelay)
    (P2P disponible para todos los juegos de Steam);
  - [steam_api.h](https://partner.steamgames.com/doc/api/steam_api) (`RunCallbacks`, `ReleaseCurrentThreadMemory`).
- Precedente de un mod inyectado:
  [UberMorgott/Wartales-Mod-wartales-mp v0.1.2](https://github.com/UberMorgott/Wartales-Mod-wartales-mp/releases/tag/v0.1.2).
  Dentro del juego, los `SetGlobalCallback_MessagesSessionRequest` nunca se dispararon y lo
  solucionaron con `CCallbackBase` propios registrados con `SteamAPI_RegisterCallback`.
- Datos binarios propios: `build\research\steam\scan_output.txt` y `exe_strings_output.txt`.
- Tamaños de `FriendGameInfo_t`, `EResult` y `ESteamNetworkingAvailability`: proceden del
  SDK público de Steamworks (`isteamfriends.h`, `steamclientpublic.h`,
  `steamnetworkingtypes.h`) y no se han verificado contra un SDK local. Hay que confirmarlos
  con `static_assert` y con la sonda del paso 0.

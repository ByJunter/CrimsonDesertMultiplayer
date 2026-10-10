# Qué enviaba el servidor MMO a los demás jugadores sobre un jugador que se mueve

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`).
Análisis estático de solo lectura (capstone sobre el exe en disco) más las capturas
`captures/cdmp_packets_20261003-112627.bin`, `-125936.bin`, `-131004.bin` y `-105149.bin`.
El juego no se ejecutó. Complementa a [mirror-ack-formats.md](mirror-ack-formats.md); el
lado cliente (cómo se interpola `MoveActorAck`) se estudia aparte y aquí no se trata.

## Resumen

1. **El servidor no simula al jugador: solo valida y reenvía.** Los cuatro escritores de Ack
   (`MoveActorAck`, `StartActionAck`, `RotateActorAck`, `ChangeAIStateAck`) se llaman
   **solo** desde los handlers de las Req correspondientes (y, en StartAction, desde
   acciones que inicia el propio servidor). No hay ningún temporizador ni tick que difunda la
   posición propia del servidor. Los 85 temporizadores no incluyen nada de transform.
2. **El MMO era "controlador-autoritativo".** Cada actor tiene un cliente que lo controla (el
   jugador o, para NPCs, el cliente que corre su IA: `ChangeAIStateReq`, `AITeleportReq`… son
   C→S). El servidor reenvía lo que manda el controlador a `ToNearExceptOneControl` (todos los
   cercanos menos el que controla). Por eso en un jugador no se ve ningún `MoveActorAck`.
3. **Durante una acción con root motion, los demás jugadores solo recibían:**
   - `StartActionAck` (hash de la acción + MoveData del momento en que empieza);
   - `RotateActorAck` a ~33-40 Hz (quat + Path, con la **posición congelada** en el punto de
     agarre);
   - `UpdateWallUpDataAck` (2893) con la **trayectoria del "wall up"**: puntos `_up`,
     `_forward`, `_end` más un `WallPassPath`. **Pero el cliente solo envía la Req si un bool
     global (`exe+0x6D76D5C`) es distinto de 0, y en esta build nada lo escribe.** En las
     cuatro capturas aparece 0 veces;
   - al terminar, el primer `MoveActorReq` del controlador → `MoveActorAck` con la posición
     final (salto de 2-3 m).

   El cliente remoto reproducía el movimiento **ejecutando la misma acción** (su root motion)
   guiada por el WallUpPath. El servidor nunca calculó posiciones intermedias.
4. **La posición que guarda el servidor (`TransformSync+0xB4`) es simplemente la del último
   `MoveActorReq`/`StartActionReq`.** Durante la escalada se queda congelada en el punto de
   agarre. `RotateActorReq` solo cambia el quat y no integra el vec3 de `+0x48`. **No sirve
   como fuente para el clon.**
5. **Mensajes autoritativos de posición:** no existe un snapshot periódico. Los únicos
   mensajes "pon este actor aquí" son:
   - `MoveForceAck` (2496), un teletransporte que va a `ToNear`, **incluido el que
     controla**, junto con `ChangeMoveSessionIDAck`;
   - `PrivateActorMoveAck` (2447), una vez al hacer login;
   - `DoTeleportAck` (2987), dirigido a un único destinatario.
6. **En las capturas, los periodos "sin Move" son cortos y siempre de root motion:**
   - subidas a cornisa (acción `b0044755`): 1,2-1,5 s, +2,2-2,9 m de altura;
   - saltos de obstáculo (`a7d51c05` y otras): 0,3 s, 1-3 m;
   - un viaje de 32 s montado en un gimmick (vehículo), padre `0xB0100D4E`.

   **Escalar una pared sí envía `MoveActorReq`** (~40 Hz con vy ≈ 2,4 m/s).

**Recomendación principal:** para los huecos sin Move, alimentar al clon con `MoveActorAck`
**sintéticos** construidos con la posición real que lee `PlayerReader` (20 Hz), en lugar de
esperar algo del servidor. Opcionalmente, replicar `UpdateWallUpDataAck` para que la
animación de subida a cornisa del clon apunte al borde correcto (ver §7).

---

## 1. Escritores de Ack: quién los llama y cuándo

### 1.1 `MoveActorAck` — escritor `0x2974C00`

Cadena de llamadas (todo el código está en `.rsrc`, no en `.xtls`, salvo donde se indica):

```
MoveActorReq::vf2 0x2977E10
  └─ ServerTransformSync::ApplyMoveFromClient 0x2BC5420 (this=TS, res, &MoveData, sendAck=1)
       └─ ServerTransformSync::SetTransform 0x2BCCEB0 (this, res, &MoveData, sendAck, extra=0)
            └─ 0x2BCD8A0 (núcleo: mueve en el field y difunde)
                 ├─ extra==0 → 0x2974C00 con ToNearExceptOneControlSendTargetFinder   (0x2BCEB18)
                 └─ extra!=0 → 0x2974C00 con ToNearSendTargetFinder                   (0x2BCEAD9)
StartActionReq (0x2A78CA0) → 0x2BC5420 con sendAck=0 (aplica la MoveData de B, sin MoveActorAck)
```

`TS` = `ServerTransformSyncActorComponent` = `[actor+0x68]+0x1A0`.

**Condiciones, en orden:**

1. **Handler `0x2977E10`:**
   - busca el actor por clave (`0x2A827B0` sobre `[exe+0x6D69698]`);
   - exige que el `ServerUserActor` de la sesión sea el **dueño** (`vf170()` == `actor+0x90`);
   - guarda la cola de la Req (f32×3, la velocidad) en `TS+0x5A0`.
2. **`TS::CanMove` `0x21A1580`.** Si falla, la Req **se ignora** y solo se guarda el
   historial (10×28 B) mediante `0x28FA740`. Falla si:
   - `TS+0x28 == 0xFFFF` (sin field);
   - `byte TS+0x3DC != 0` (congelado);
   - para tipo 1, el componente Docking (`comps+0x170`) tiene `+0x18 != 0` (acoplado a algo);
   - hay un estado `0x15` (`0x17F2B60`);
   - el componente Catch (`comps+0x70`) tiene `+0x28 != 0` (lo tienen agarrado).
3. **`ApplyMoveFromClient` `0x2BC5420`:**
   - si `actor->vf(0x188)(5)` (el servidor controla el actor), descarta la Req y llama al tick
     propio `0x2BCBC00`;
   - si `byte TS+0x5B8 > 0` (**corrección pendiente**, ver §4), descarta;
   - si la distancia entre la posición del servidor y la nueva es **≤ 0,001 m**
     (`[0x5D72C5C]`), solo actualiza el quat (`TS+0x98`) y **no difunde**;
   - si no, llama a `SetTransform(sendAck)`.
4. **`0x2BCD8A0`:**
   - `0x2AF8090` mueve el actor en el field/sector; si falla, devuelve error y no hay Ack;
   - copia el Path (136 B) a `TS+0x308`;
   - estima la velocidad (`TS+0x1D8`, a partir de la posición previa y el tiempo en
     `TS+0x5C0`);
   - si `extra == 0` y es un movimiento normal (`arg5 == 0`), **construye el Ack** con
     `TS::GetMoveData` `0x21A1950`, que es estado del servidor:
     - posición = `TS+0xB4` (local al trozo) + sector `i16 TS+0xC0/+0xC2` × 1000;
     - quat compuesto de `TS+0xA4`/`+0xC8`/`+0xDC`/`+0xFC`;
     - u32 de `TS+0x408`;
     - Transform de `TS+0x40C`;
     - Path de `TS+0x308`;
   - sobrescribe la marca de tiempo con la de la Req y el byte `+0xE0` con 0;
   - la cola es la que se guardó de la Req (`TS+0x5A0`);
   - destino: `ToNearExceptOneControl`.
5. Con `arg5 == 1` (teletransporte, `0x2BCD120`) **sale antes** de difundir (`0x2BCE619`).
   El aviso se manda aparte con `MoveForceAck` (§4).

**Tasa:** una `MoveActorAck` por cada `MoveActorReq` que desplaza más de 1 mm. Es la misma
frecuencia del cliente: ~40 Hz al moverse; en la captura 1, 39.684 Req en 856 s. No hay
throttle. El temporizador de 3000 ms (`TS+0x5B0`) solo controla el bucle que arrastra a los
actores agarrados (si se alejan más de 64 m, `CatchNak` 2394).

**Fuente de datos:** la propia Req re-codificada a través del estado del servidor. Es la
posición del cliente, quizá cuantizada por el field.

**Efectos secundarios útiles para saber qué hace el servidor:**
- `TS+0x5C8` acumula la distancia → GameEvent tipo `0x3F` al pasar un umbral;
- `TS+0x5CC` acumula distancia → encola internamente el opcode 2068
  (`VaryFriendlyByFrameEventOnceTimer`);
- marca la casilla del mapa explorado en `[comps+0x168]+0x260` (bitset).

### 1.2 `StartActionAck` — escritor `0x2963FF0`

```
StartActionReq::vf2 0x296CAA0 → 0x2A78CA0
   ├─ 0x2BC5420(TS, &B.MoveData, sendAck=0)   (actualiza la posición del servidor, sin MoveActorAck)
   ├─ 0x28FA740(actor, B+0x138)                (historial)
   ├─ 0x2A7A8C0                                (arranca la acción en el servidor)
   └─ 0x2A797C0(..., flag=0) → 0x2A7A560 → 0x2963FF0
Acciones iniciadas por el servidor: 0x2A79420 → 0x2A797C0(..., flag=1)
```

`0x2A7A560` (difusión):
- **no envía nada** si el bit 31 de `TS+0x4F8` es 0, o si la definición del actor tiene
  `+0x10A & 1`;
- si alguna de las dos acciones tiene el flag `0x40` (`+0x51`/`+0xC9`), envía
  **`StartSpawnActionAck` (2723, escritor `0x29644A0`)** en lugar de StartActionAck;
- si no, envía StartActionAck:
  - `flag == 0` (relevo de una Req) → `ToNearExceptOneControl`;
  - `flag == 1` (acción forzada por el servidor) → `ToNear`, **incluido el que controla**.
- **El u8 que va en el Ack `+0x8E` es ese flag**: el servidor pone 0 en los relevos, no copia
  el u8 de la Req.
- El bloque B que se envía es **el de la Req** (`r13`), no se reconstruye.

**Tasa:** una por StartActionReq.

### 1.3 `RotateActorAck` — escritor `0x29750E0`

```
RotateActorReq::vf2 0x29784C0  → (mismo control de dueño que Move)
  └─ thunk 0x2BC6800 → 0x106F3120 (.xtls, ServerTransformSync::ApplyRotate)
        ├─ si byte TS+0x5B8 > 0 → no hace nada (corrección pendiente)
        ├─ GetMoveData del servidor, sustituye SOLO el quat por el de la Req
        ├─ SetTransform 0x2BCCEB0(sendAck=0, extra=1)   → la posición del servidor no cambia
        └─ si actor->vf(0x178)() → 0x29750E0 con ToNearExceptOneControl, u8 = 1
```

- El Ack lleva **el quat, el Path, la marca de tiempo y la cola de la Req** (el Path no se
  guarda en el servidor).
- **El vec3 de `+0x48` del payload (Path+0x34) no se usa** más que para reenviarlo.
- **Tasa:** una por RotateActorReq, sin umbral.

### 1.4 `ChangeAIStateAck` — escritor `0x297CF50`

Lo llama solo `0x2AC66D0` = `ServerAiActorComponent` vf68 (`+0x220`), con
`ToNearExceptOneControl`. Es el relevo de `ChangeAIStateReq`: el estado de IA lo produce el
cliente que controla al NPC.

### 1.5 Destinatarios (`To*SendTargetFinder`)

Las vtables están en `0x5B25418` (ToNear), `0x5B25358` (ExceptOneControl) y `0x5B25438`
(ToTarget). Hay 11 clases:

| Clase | Uso |
|---|---|
| ToNear | Destino de "Near" |
| ToNearExceptOneControl | Destino de "Near" sin el que controla |
| ToNearExceptOneSelf | |
| ToNearAndParty | |
| ToTarget | Un único destinatario |
| ToControlUser | |
| ToSessionList | |
| ToStage | |
| ToWorld | |
| ToField | |

"Near" se calcula con el nodo de interés del field en `TS+0x540` (`vf(0x60)` → lista de
sesiones, `0x4A2010`), pasando por el attacher de field `TS+0x4F0`. Es un área de interés
por sector, sin radio fijo en el código.

## 2. ¿Simula el servidor algo por su cuenta?

- `ServerTransformSyncActorComponent` vf42 (`+0x150`) = **`0x2BCBC00`**, el tick propio:
  - corre como mucho cada 100 ms (`TS+0x18`), salvo que lo fuercen;
  - **solo para los tipos de actor 3, 4, 5 y 6** (`byte [[actor+0x88]+1]`);
  - calcula un transform nuevo con `0x2BCBF50` (seguir al padre o un camino) y llama a
    `SetTransform(sendAck=0)`, así que **no hay Ack**.
  - El jugador es de **tipo 1** y queda excluido.
- Los demás que llaman a `SetTransform` son vfuncs de TransformSync (49/51/52/53:
  colocar/teletransportar desde lógica del servidor) y de `ServerGimmickActorComponent`
  vf275. **Todos con `sendAck=0`.**
- **No hay root motion en el servidor** para los jugadores. No se integran velocidades ni el
  vec3 del Path.
- Conclusión: si se lee `TS+0xB4` del servidor durante una subida a cornisa, solo se obtiene
  el punto de agarre.

## 3. Correcciones del servidor (lo más parecido a "autoridad")

- Si `ApplyMoveFromClient` falla con error (`MoveActorReq` y `StartActionReq`), se llama a
  **`0x2BCCC00`**:
  - envía `MoveForceNak` (2652, escritor `0x29746B0`) con la MoveData del servidor;
  - envía `ChangeMoveSessionIDAck` (3097, escritor `0x2973900`) al que controla;
  - **incrementa `TS+0x5B8`** (`TS+0x598` = id de sesión de movimiento).
- Mientras `TS+0x5B8 > 0`, Move y Rotate se descartan hasta que el cliente responde con
  `EchoMoveSessionIDReq` (2624).
- **Teletransporte del servidor** `0x2BCD120` → `0x1070FAD0` (.xtls) → `0x2BCD430`:
  - llama a `0x2BCD8A0` en modo forzado, sin MoveActorAck;
  - después, `0x2BCEEF0` envía **`MoveForceAck` (2496, escritor `0x2974200`) a `ToNear`**,
    incluido el que controla, y `ChangeMoveSessionIDAck`;
  - lo llaman 7 funciones (por ejemplo cuando `AITeleportReq` recoloca a los compañeros).
- En las capturas, `AITeleportReq` → `MoveForceAck` → `ChangeMoveSessionIDAck` →
  `EchoMoveSessionIDReq` aparece 24 veces en la captura 1. `MoveForceAck` es el **único**
  mensaje de posición autoritativa que el servidor manda también a los observadores.

## 4. Inventario de mensajes relacionados con el movimiento

**No existe ningún mensaje** con Climb, Glide, Swim, Parkour, Vault, Ledge, Hang, Fly, Rope o
Jump (salvo `UpdateAIJumpTargetData`, de IA). La escalada y el planeo son acciones normales
(`StartAction`) más Move y Rotate. Los mensajes específicos son:

| Mensaje (opcode) | Dir. | Formato del payload | Servidor (handler o escritor → destino) | Visto en capturas |
|---|---|---|---|---|
| **UpdateWallUpDataReq (2851)** / **Ack (2893)** | C→S / S→C | `u32 clave` + WallUpData (128 B, ver abajo) = **132 B** | Req vf2 `0x297A4A0`: busca el actor y **reenvía tal cual** con `0x2977130` → `ToNearExceptOneControl`. No guarda nada | **0** (bloqueado por el bool `exe+0x6D76D5C`) |
| UpdateSeatDataReq (2201) / Ack (2470) | C→S / S→C | `u32 clave, u8, u32, u32` (13 B) | Req `0x297A780` → relevo `0x2977550` → `ToNearExceptOneControl` | 0 |
| SyncActorDataReq (2526) / Ack (2455) | C→S / S→C | `u8, u32, u8…` | Req `0x298EB30` → `0x298B4D0` → `ToNearExceptOneControl` | 0 |
| ChangeVehiclePhysicsStateReq (2744) / Ack (2970) | C→S / S→C | `u32, u8…` | Req `0x2A6DE90` → `0x2A6C4A0` → `ToNearExceptOneSelf` | 0 |
| ProcessFallReq (2095) / RequestFallMustFailedAck (3045) | C→S / S→C | Req de 52 B / Ack de 8 B | `0x2A3C4A0` → `0x296C190` → `ToTarget` (vuelve al que la envió) | 1-2 |
| ChangeDrowningStateReq (2739) | C→S | `u32 clave, u8, u8` | `0x298E640`; sin relevo | 3-28 |
| WetnessReq (2089) | C→S | 6 B | `0x2A651C0` | 15-338 |
| MotionTypeChangeToDynamicReq (2707) | C→S | 24 B | `0x29C0370` | 5 |
| **MoveForceAck (2496)** | S→C | `u32 clave` + MoveData (218) + `u8, u32, u32, u8` = 232 B | `0x2974200` desde `0x2BCEEF0` → **ToNear** | 24 / 3 / 6 / 26 |
| MoveForceNak (2652) | S→C | (corrección) | `0x29746B0` desde `0x2BCCC00` | 0 |
| ChangeMoveSessionIDAck (3097) / EchoMoveSessionIDReq (2624) | S→C / C→S | 16 B `u32 clave, u32 clave del usuario, u64 id` / 12 B | `0x2973900` | se emparejan con MoveForceAck |
| PrivateActorMoveAck (2447) | S→C | 60 B: `u32 clave, vec3, vec3, u32, u32, u32, quat, u32` | `0x2975C10` desde `0x28F31F0` (login) | 1 (inicio de partida) |
| DoTeleportAck (2987) | S→C | `u32 clave, vec3, u64…` | `0x2973D80` desde `0x2BB3190` → `ToTarget` | 0 |
| StartSpawnActionAck (2723) | S→C | variante de StartActionAck (acciones con flag `0x40`) | `0x29644A0` | 0 |
| UpdateVisiblePlayerAck (2135) | S→C | — | No hay escritor con el opcode como inmediato: o se construye de otra forma o es código muerto | 0 |
| TransformSyncSceneObjectReq (2974) | C→S | 60 B | transform de objetos de escena (gimmicks), no de personajes | 584 (captura 1) |

Detalles del WallUpData (deserializador `0x2444490` y `0x14EAFB0`). Los nombres de campo
salen de los mensajes de error coreanos del propio exe
("WallUpPath의 _up를 읽어들이는데 실패했다", es decir, "no se pudo leer `_up` de WallUpPath").
La primera columna son offsets del **struct deserializado en memoria**, no del cable. En el
cable los campos van empaquetados; los offsets reales del payload están en §10.3. La última
columna es relativa al bloque `D` del §7:

| Struct (tras la clave) | Campo | En el cliente (`D` = ver §7) |
|---|---|---|
| `+0x00` u8 | ? (estado o tipo) | `D+0xEE0` |
| `+0x04` u32 | ? | `D+0xEE4` |
| `+0x08` vec3 | `WallUpPath._up` | `D+0x1170` |
| `+0x14` vec3 | `WallUpPath._forward` | `D+0x117C` |
| `+0x20` vec3 | `WallUpPath._end` | `D+0x1188` |
| `+0x2C` 43 B | `WallPassPath`: `_up`, `_forward`, `_end` (3 vec3), `_wallThickness` f32, `_isValid`, `_actionCondition_onlyWallPass`, `_canAutoWallPass` (3 u8) | `D+0x11C0..0x11EB` |
| `+0x58` u32 | ? | `D+0x1200` |
| `+0x5C` vec3 | ? | `D+0xF40` |
| `+0x68` vec3 | ? | `D+0xF4C` |
| `+0x74` vec3 | ? | `D+0x10DC` |
| `+0x80` f32 | ? | `D+0x1100` |

## 5. Lo que muestran las capturas (huecos de Move del jugador `0xA0100001`)

Script `build/research/movegaps.py`: tramos con 5 o más RotateActorReq y ninguna
MoveActorReq. "Salto" = distancia entre el Move de antes y el de después.

| Captura | t (s) | Duración | Rotates | Salto | Acción (StartAction) | Interpretación |
|---|---|---|---|---|---|---|
| 125936 | 141,03 | 1,23 s | 41 | 2,74 m (dy +2,53) | `96775acf→b0044755` | subida a cornisa (la escalada estudiada) |
| 131004 | 147,50 | 1,21 s | 43 | 3,07 m (dy +2,92) | `96775acf→b0044755` | subida a cornisa |
| 131004 | 155,79 | 1,54 s | 46 | 2,23 m (dy +2,16) | `b0044755` | subida a cornisa |
| 131004 / 105149 | 107,5; 446; 550; 580; 590 | 0,3 s | 7-10 | 1,8-3,3 m (dy +1-2) | `a7d51c05`, `df77a7ea`, `9a3031d3`… | salto de obstáculo o subida corta |
| 112627 | 731,67 | **32,7 s** | 1024 | **527 m** | `26107d3c/25dc7fb9` | **montado en un gimmick** (`0xB0100D4E`, un vehículo): Rotate con padre = gimmick y offset local fijo. El gimmick **no envía Move** (solo `UpdateGimmickReq` y `GimmickExecuteFrameEventReq` al principio) |

- Entre 140,58 y 141,03 s, **la escalada por la pared sí manda MoveActorReq** cada ~24 ms
  (y de 613,93 a 614,96).
- Solo el remate (de 141,15 a 142,26 s) es silencioso. La primera Move de después salta a
  `(-10092,70; 617,49; -3530,70)`.
- En el StartActionReq de la subida, los vec3 extra de B (Req `+0x170`/`+0x17C`) valen 0. **La
  acción no lleva el punto de destino**: ese dato solo viajaba en WallUpData.

## 6. ¿Hay un snapshot autoritativo para los actores remotos?

**No.** No existe ningún mensaje periódico S→C con el transform de los actores:
- los temporizadores del servidor no difunden transforms;
- el tick `0x2BCBC00` no envía nada;
- `ResolveDurtyFlagRepeatTimer` y `CheckChangeAnimationSpeedRepeatTimer` están mutados en
  `.xtls` y ninguna llamada desde `.xtls` lleva a los escritores de Move o Rotate, salvo
  ApplyRotate y ForceMove ya vistos.

Lo más parecido:
- **`MoveForceAck`**: un "snap" a una MoveData completa, que va también al que controla, así
  que el cliente debe aceptarlo sea quien sea el controlador. Lo aplica `0x9FED30`, que libera
  catch y docking y recoloca el actor. Útil para **correcciones duras o teletransportes del
  clon**, no para cada fotograma: cambia la sesión de movimiento.
- `PrivateActorMoveAck` y `DoTeleportAck`: puntuales y con un único destinatario.

## 7. Recomendaciones para mover a un jugador remoto (el clon)

1. **Rellenar los huecos con Acks sintéticos a partir de la posición real.** Durante un tramo
   sin `MoveActorReq` (Rotate sí, Move no, más de ~100 ms), generar `MoveActorAck` para el
   clon con la posición y el quat que lee `PlayerReader` (20 Hz). Entre dos PCs, usar lo que
   ya manda el ghost.
   - Rellenar el `MoveData` como en el espejo; la marca de tiempo en el reloj local.
   - Es exactamente lo que el servidor del MMO **no** hacía; el remoto lo resolvía con la
     animación. Aquí no depende de que el cliente aplique root motion al clon.
   - **Alternativa barata** con el retardo del espejo: si el retardo supera la duración del
     hueco (unos 1,5 s para una subida a cornisa), interpolar entre el último Move y el
     siguiente. Es peor que leer la posición real.
2. **Opcional, para que la animación encaje: replicar `UpdateWallUpDataAck` (2893).**
   - **Cabecera:** `u16 2893 | u8 0 | u16 132`.
   - **Payload:** `u32 clave del clon` + los 128 B del WallUpData, leídos de la memoria del
     jugador local mientras su controlador esté en el estado 5.
   - **Cadena de punteros (del handler `0xB64460`):**
     - `X = [[ [[actor+0x68]+0x40] + 0x118 ]] + 0xC8` (sale de CharacterControl);
     - `D = [[X+0x120]+0x18]+0xD8`;
     - campos según la tabla del §4.
   - **Lo que hace el emisor original** (`0x87DD00`): lee esos mismos campos si
     `byte [X+0xF0] == 5`.
   - **Puertas del receptor:**
     - `byte [[actor+0x88]+1] == 1`;
     - `byte [actor+0x96] == 0`, es decir, no lo controla este cliente. El emisor exige `!= 0`;
     - `X+0x120 != null`.
   - **Dónde aplicarlo:** desplazar `_up`/`_forward`/`_end` igual que el resto de posiciones si
     se usa `mirror_offset_m`. Mandarlo justo antes del `StartActionAck` de la acción
     `b0044755` (o en cada tick del estado 5, como el emisor).
   - **Pendiente de verificar en vivo:**
     - que esas coordenadas son de mundo y no relativas;
     - que el clon tiene `X+0x120`;
     - que su `+0x96 == 0`.
3. **No usar el TransformSync del servidor como fuente.** Para el jugador solo contiene el
   último Move o StartAction; durante la subida a cornisa está congelado y el servidor no
   integra nada.
4. **Padres o vehículos.**
   - Montado en un gimmick, el jugador solo manda Rotate con `Path.parentKey` = gimmick, un
     offset local fijo y `Path.WorldTransform` (Rotate `+0x5C`) **congelado**.
   - El movimiento del vehículo no viaja por la red.
   - Para el clon: o se traduce la clave del padre a un actor que exista y se mueva en el PC
     que recibe, o se pone `0xFFFFFFFFFFFFFFFF` en el padre y se mueve el clon con la posición
     leída (punto 1).
5. **Correcciones duras.** Si el clon se desvía mucho (más de unos metros) o tras un viaje
   rápido, un `MoveForceAck` (232 B: clave + MoveData + `u8 0, u32 0, u32 0, u8 1`, el
   valor visto en la captura) hace un snap sin interpolar.
   - No hace falta `ChangeMoveSessionIDAck`, porque eso es para el que controla.
   - Verificar antes en vivo que `0x9FED30` no tiene efectos no deseados en el clon:
     suelta catch y docking.
6. **El u8 del StartActionAck.** El servidor real manda **0** en los relevos (y 1 solo en
   las acciones que fuerza él). Si con 1 el clon se comporta bien, se puede mantener, pero 0
   es lo que vería un jugador remoto del MMO.
7. **El byte `+0xDD` del MoveActorAck.** El servidor lo pone a 0 cuando reenvía la marca de
   tiempo del cliente, y a 1 con su propio reloj (rama `extra`). La base de reloj la elige el
   byte TLS `+0x1EC`, probablemente el flag de realm que describe Trinity (en su build estaba
   en `+0x1F2`). Mantener 0.

## 8. Direcciones nuevas (build 2976)

| RVA | Qué es |
|---|---|
| `0x2BC5420` | `ServerTransformSync::ApplyMoveFromClient(this, res, MoveData*, bool sendAck)` |
| `0x2BCCEB0` | `ServerTransformSync::SetTransform(this, res, MoveData*, bool sendAck, u8 extra)` |
| `0x2BCD8A0` | núcleo de SetTransform: field, Path, velocidad, difusión de MoveActorAck |
| `0x21A1950` | `ServerTransformSync::GetMoveData(this, MoveData* out)` |
| `0x21A1580` | `ServerTransformSync::CanMove(this, res)` |
| `0x2BCCC00` | corrección: MoveForceNak + ChangeMoveSessionIDAck, `++TS+0x5B8` |
| `0x2BCD120` → `0x1070FAD0` → `0x2BCD430` / `0x2BCEEF0` | teletransporte del servidor + MoveForceAck |
| `0x106F3120` (.xtls, thunk `0x2BC6800`) | `ServerTransformSync::ApplyRotate` + RotateActorAck |
| `0x2BCBC00` | `ServerTransformSync` vf42 `+0x150`: tick propio (tipos 3-6, 100 ms, sin Ack) |
| `0x2A78CA0` | lógica de StartActionReq |
| `0x2A797C0` → `0x2A7A560` | difusión de StartAction / StartSpawnAction |
| `0x297A4A0` / `0x2977130` | UpdateWallUpDataReq (vf2) / escritor de UpdateWallUpDataAck |
| `0x87DD00` | emisor cliente de UpdateWallUpDataReq (puerta: `byte exe+0x6D76D5C`) |
| `0xB64460` | handler cliente de UpdateWallUpDataAck (escribe en `D+0xEE0…`) |
| `0x2444490` / `0x14EAFB0` | deserializadores de WallUpData / WallPassPath |
| `0x4F8220` | posición del TS = `+0xB4` + sector `i16 +0xC0/+0xC2` × 1000 |

Offsets de `ServerTransformSyncActorComponent` vistos:

| Offset | Contenido |
|---|---|
| `+0x18` | último tick |
| `+0x28` | field (`0xFFFF` = ninguno) |
| `+0x98` | quat |
| `+0xB4` | posición local |
| `+0xC0` / `+0xC2` | sector |
| `+0x188` / `+0x1D8` | velocidad estimada |
| `+0x308` | Path (136 B) |
| `+0x3DC` | congelado |
| `+0x408` / `+0x40C` | u32 / Transform |
| `+0x4F0` | attacher de field |
| `+0x4F8` | bit 31: difundir StartAction |
| `+0x540` | nodo de interés |
| `+0x598` | id de sesión de movimiento |
| `+0x5A0` | cola de la última Move |
| `+0x5B0` | temporizador de catch |
| `+0x5B8` | correcciones pendientes |
| `+0x5C0` | marca de tiempo de la posición previa |
| `+0x5C8` / `+0x5CC` | distancia acumulada |

## 9. Preguntas abiertas y qué verificar en vivo

- Quién escribe `exe+0x6D76D5C`. No hay referencias directas en código (búsqueda por fuerza
  bruta de RIP-relativas, incluido `.xtls`) ni punteros en datos. Puede escribirse de forma
  indirecta desde código mutado. Leerlo en vivo; si vale 0, confirma que WallUp nunca se
  envía.
- Si las coordenadas de WallUpPath son de mundo. Volcar `D+0x1170…` del jugador durante una
  subida a cornisa.
- Si el clon tiene la cadena `X+0x120` válida y `+0x96 == 0`.
- Qué es el vec3 `Path+0x34` (payload de Rotate `+0x48`). Vale (0,047; −0,023; −0,008) al
  colgar y (−0,48; 0,93; −0,02) al escalar. El servidor no lo usa; probablemente sea la
  aceleración o la velocidad angular para la extrapolación del cliente.
- `UpdateVisiblePlayerAck`: no se encontró su constructor en el servidor buscando el opcode
  como inmediato.

## Scripts nuevos (`build/research/`)

| Script | Qué hace |
|---|---|
| `callers.py` | llamadas y saltos a una RVA, con la función que los contiene |
| `callees.py` | llamadas directas que hace una función |
| `callctx.py` | contexto de instrucciones antes de cada llamada |
| `vtref.py` | punteros absolutos en datos → vtable y slot (RTTI) |
| `findop.py` | opcodes Troc usados como inmediatos en una función |
| `rawref.py` / `rawref2.py` | referencias RIP-relativas por fuerza bruta a una dirección o rango de datos (necesitan numpy, instalado en el venv) |
| `climbwin.py` | decodifica Move/Rotate/StartAction en una ventana de tiempo de una captura |
| `movegaps.py` | huecos sin Move del jugador |
| `keyhist.py` | histograma de mensajes por clave de actor |

Volcados: `srvmove_*.asm`, `s_*.asm`, `srv_rotreq_29784c0.asm`, `srv_movereq_2977e10.asm`.

---

## 10. Anexo: alcance del bool `exe+0x6D76D5C` y cómo replicar WallUp sin tocarlo

Objetivo (2026-10-03): saber qué afectaría poner `exe+0x6D76D5C = 1` en tiempo
de ejecución para que el cliente envíe `UpdateWallUpDataReq`. Análisis estático, solo
lectura, build 1.0.0.2976.

### 10.1 Quién lee y quién escribe `exe+0x6D76D5C`

**Cómo se buscó.** Con fuerza bruta (`rawref.py` y `rawref2.py`) se buscaron todos los
desplazamientos RIP-relativos hacia la dirección, en **todas** las secciones ejecutables
(`.rsrc`, `.xtext`, `.xtls`, `.text`), con 0, 1, 2 o 4 bytes de inmediato, y se verificaron con
capstone. También se buscaron punteros absolutos en datos (`vtref.py`) y referencias a bases
cercanas (`0x6D76C00–0x6D76D80`), por si se accede como `[base+off]`.

| Dirección | Función | Tipo | Qué controla |
|---|---|---|---|
| `0x87DD2F` (`cmp byte ptr [rip+…], r14b`) | `0x87DD00` (emisor de UpdateWallUpDataReq) | **lectura** | Si vale 0, la función retorna sin hacer nada |

- **No existe ningún otro lector ni ningún escritor.**
- Los vecinos (`+0x6D76D58`, `+0x6D76D60`, `+0x6D76D64`, `+0x6D76D70`) son contadores y
  estáticos de otras funciones (`0x85CB00`, `0x8B7B50`, `0x9139B0`) y no forman un struct con
  este byte.
- La dirección cae en la cola sin datos en disco de la sección de datos RW (nombrada
  `.rdata`, flags `0xC0000040`). Vale **0 al arrancar** y nada la cambia.

**Significado probable:** un interruptor **solo de WallUp**, un "sincronizar WallUp con los
demás jugadores" heredado del MMO y apagado en la versión de un jugador. No es un flag
global de modo online ni de depuración: nadie más lo consulta.

**Salvedad:** el código mutado por Denuvo (`.xtls`) podría calcular la dirección de forma
indirecta (patrones como `mov ecx, imm; sub ecx, [rip+…]`). No se puede excluir del todo
estáticamente, pero no hay ningún indicio.

### 10.2 Qué más cambiaría al ponerlo a 1

Solo el emisor `0x87DD00`. Su lógica completa:

```
CC = ClientCharacterControlActorComponent  = [[actor+0x68]+0x40]
CC::vf89 (+0x2C8) 0x8610A0                  (update del componente, cada frame)
  └─ X = [CC+0xC8]                          (controlador de movimiento; struct sin vtable)
     X::Update 0x87CBC0
       └─ si byte[[actor+0x88]+1] == 1 && byte[actor+0x96] != 0     (personaje controlado aquí)
            0x87DD00(X):
              if (!byte[exe+0x6D76D5C]) return;      ← el bool
              if (byte[X+0xF0] != 5)    return;      ← modo de movimiento 5 = WallUp
              m = [X+0x120]; if (!m)    return;      ← WallUpMovementMethod (vtable exe+0x55968B8)
              D = [[m+0x18]+0xD8]                     (m+0x18 = CC, así que D = [CC+0xD8])
              copia los campos de D (§10.3) → 0xB60CF0 → PostPacket(sesión 4, op 0xB23)
                (solo si byte[ClientNet+0x112] != 0, es decir, si la red está "conectada")
```

**Efectos de activarlo en una partida de un jugador:**

| Efecto | Detalle | Riesgo |
|---|---|---|
| El cliente envía `UpdateWallUpDataReq` | **una por update de CC (cada frame)** mientras `X+0xF0 == 5`, es decir, solo durante una subida a cornisa (~1,2-1,5 s → 70-110 paquetes de 137 B a 60-76 fps). No hay throttle ni comprobación de cambios | bajo |
| El servidor procesa la Req (`0x297A4A0`) | busca el actor por clave y **solo reenvía** (`0x2977130`) a `ToNearExceptOneControl`. Con una sola sesión, no hay destinatarios: no se envía nada | muy bajo |
| Estado del servidor, guardado, UI, otras Req | **nada**: el handler no escribe en ningún componente ni en datos de guardado. No hay ningún otro código detrás del bool | ninguno |
| El sniffer del mod | verá esas Req (opcode 2851) | — |
| Escribir el byte | sección de datos RW. Una escritura de 1 byte protegida con SEH | bajo |

**Valoración:** el alcance es mínimo, porque solo despierta este emisor. Aun así, existe la
alternativa del §10.4, que no escribe nada en el juego.

### 10.3 Formato completo y receta Req→Ack

| | Req | Ack |
|---|---|---|
| Nombre / opcode | `UpdateWallUpDataReq` **2851 (`0xB23`)**, máscara 4, flags `0x100000` | `UpdateWallUpDataAck` **2893 (`0xB4D`)**, máscara 1, flags `0x2000000` |
| vf2 | servidor `0x297A4A0` | cliente `0xB64460` |
| Tamaño del payload | **132 B** | **132 B** |
| Lectura | `u32 clave` + `0x2444490` | igual |
| Escritura | `0xB60CF0` (`SerializerFixedMemory` + `0x2444320`) | servidor `0x2977130` (`u32 clave` + `0x2444320`) |

**Receta:** `ack = req`, cambiando solo el opcode (2851 → 2893) y la clave (`req[0:4]` → clave
del clon). El layout es idéntico, sin u8 extra. Cabecera: `u16 0x0B4D | u8 0 | u16 132`.

Payload, con offsets **empaquetados** (incluyen la clave). Los nombres entre comillas vienen
de las cadenas de error del exe; el resto se deduce del código que los calcula (`0x6A8B50`,
`0x6A89A0`):

| Payload | Tipo | Campo | Origen (`D = [CC+0xD8]`) | Significado | ¿Desplazar? |
|---|---|---|---|---|---|
| `+0x00` | u32 | clave del actor | — | — | clave del clon |
| `+0x04` | u8 | fase | `D+0xEE0` | 0 = sin ruta (fuerza un recálculo), 1 y 2 son estados que usan los vfuncs de WallUp | no |
| `+0x05` | u32 | clase de altura | `D+0xEE4` | 0-3, según los escalones de altura entre los puntos de la navegación | no |
| `+0x09` | vec3 | "WallUpPath._up" | `D+0x1170` | punto del borde (y = altura del borde + constante) | **sí** |
| `+0x15` | vec3 | "WallUpPath._forward" | `D+0x117C` | punto tras avanzar sobre el borde | **sí** |
| `+0x21` | vec3 | "WallUpPath._end" | `D+0x1188` | punto final | **sí** |
| `+0x2D` | vec3 | "WallPassPath._up" | `D+0x11C0` | ruta de "atravesar muro" (saltar un obstáculo) | **sí** |
| `+0x39` | vec3 | "WallPassPath._forward" | `D+0x11CC` | | **sí** |
| `+0x45` | vec3 | "WallPassPath._end" | `D+0x11D8` | | **sí** |
| `+0x51` | f32 | "_wallThickness" | `D+0x11E4` | grosor del muro | no |
| `+0x55` | 3×u8 | "_isValid", "_actionCondition_onlyWallPass", "_canAutoWallPass" | `D+0x11E8..0x11EA` | flags | no |
| `+0x58` | u32 | id del objeto escalado | `D+0x1200` | u32 bajo de la clave del objeto de navegación (o 0) | traducir entre PCs |
| `+0x5C` | vec3 | punto de apoyo o resultado del raycast | `D+0xF40` | punto | **sí** |
| `+0x68` | vec3 | −dirección hacia delante (normal de la pared) | `D+0xF4C` | vector | no |
| `+0x74` | vec3 | dirección hacia delante (xz) | `D+0x10DC` | vector | no |
| `+0x80` | f32 | altura del muro | `D+0x1100` | metros | no |

Total: 4 + 1 + 4 + 36 + 43 + 4 + 36 + 4 = **132**. En memoria, el WallPassPath ocupa 44 B
(con relleno) y en el cable 43.

**Coordenadas.** `0x6A8B50` calcula `_up` y los demás puntos restando o sumando a los puntos
de la consulta de navegación (`comps+0x1A8`, `0x21B8C60`) la posición del actor que da
`TS::GetPos` (`0x4F8220` = local + sector × 1000). Por tanto están en el **mismo marco
"mundo" que la posición de `MoveActorReq`** (p. ej. x ≈ −10093), no en coordenadas locales al
trozo. Con `mirror_offset_m` hay que desplazar solo los **puntos** marcados en la tabla.

**Cuándo se envía.** En cada update del CC (cada frame) mientras `X+0xF0 == 5`. No hay
envío "al empezar" ni detección de cambios.

- **Modo 5** = `WallUpMovementMethod`. Índices de `X+0xF8 + i*8`, sacados del constructor
  `0x88C530` → `0x879B80(X, i, método)`:

  | Índice | Método |
  |---|---|
  | 0-3 | Normal |
  | 4 | AnimationAll |
  | **5** | **WallUp** |
  | 6 | RideOn |
  | 7 | RideOff |
  | 8 | LadderClimb |
  | 9 | (constructor `0x69ECB0`, probablemente Cover) |
  | 0xA | Vehicle |
  | 0xB | Docking |
  | 0xC | WallFinish |
  | 0xD | Sequencer |
  | 0xE | Approach |
  | 0xF | MiniGame |
  | 0x10 | AttackApproach |
  | 0x11 | RopeSwing |
  | **0x12** | **PointClimb** (escalada libre por la pared) |
  | 0x13 | (constructor `0x6A9300`, probablemente AiJump) |
  | 0x14 | Snowboard |
  | 0x15 | ActionFixedPosition |
  | 0x16 | ninguno |

- Encaja con las capturas: la escalada (PointClimb) **sí** manda Move, y el remate silencioso
  de 1,2-1,5 s sería el modo 5 (WallUp) o el 0xC (WallFinish).
- **Pendiente de confirmar en vivo** qué modo está activo en cada fase:
  - `byte [[[[jugador+0x68]+0x40]+0xC8]+0xF0]`;
  - y si los saltos de obstáculo cortos de 0,3 s también usan el modo 5, con WallPassPath.
- La ruta se recalcula (`0x6A89A0` → `0x6A8B50`) cuando `D+0xEE0 == 0` o cambia el objeto de
  navegación (`method+0x270`). Durante una subida normal es casi constante.

### 10.4 Alternativa más estrecha, sin tocar el bool

**Sí existe.** El handler del cliente `0xB64460` **no** consulta `exe+0x6D76D5C`: el bool solo
controla el emisor. El mod puede leer `D` del jugador local y fabricar el Ack él mismo:

```
CC_local = [[jugador+0x68] + 0x40]                 (ClientCharacterControlActorComponent)
X_local  = [CC_local + 0xC8]
activo   = byte [X_local + 0xF0] == 5  &&  [X_local+0x120] != 0 (vtable exe+0x55968B8)
D_local  = [CC_local + 0xD8]                       (= [[ [X_local+0x120] + 0x18 ] + 0xD8])
leer:  D+0xEE0 u8 · D+0xEE4 u32 · D+0x1170 3×vec3 · D+0x11C0 3×vec3 + f32 + 3×u8 ·
       D+0x1200 u32 · D+0xF40 vec3 · D+0xF4C vec3 · D+0x10DC vec3 · D+0x1100 f32
→ serializar en el orden de §10.3 con la clave del clon → inyectar como opcode 2893
```

- **Momento:** igual que el original, en cada tick mientras el modo 5 esté activo, con el
  mismo retardo que los Ack espejo. Debe llegar **antes o junto** al `StartActionAck` que mete
  al clon en la subida (la acción `b0044755` en las capturas) y seguir hasta que termine.
- **Puertas del receptor `0xB64460`** (comprobar en el clon antes de usarlo):
  - "no listo" `0x8B53C0` (igual que Move: `+0x5E == 0x10`, `+0xCF == 1`);
  - `byte [[clon+0x88]+1] == 1`;
  - `byte [clon+0x96] == 0`;
  - `X' = [[ [CC_clon+0x118] ]+0xC8]` con `[X'+0x120] != 0`;
  - `[X'+0x68]` de tipo 1 con `+0x96 == 0`;
  - escribe en `D' = [[ [X'+0x120]+0x18 ]+0xD8]`.
- **Desplazamientos:** aplicar `mirror_offset_m` solo a los puntos (§10.3). `D+0x1200` es el
  id del objeto escalado: en un solo PC sirve tal cual; entre dos PCs habría que traducirlo o
  ponerlo a 0.
- **Ventajas frente a poner el bool a 1:** no se escribe nada en el juego ni se generan Req
  hacia el servidor local, y funciona igual en el cooperativo real: el host lee su `D` y lo
  manda por la red.
- **No resuelve la posición por sí solo.** Esto solo da al clon la ruta que su
  `WallUpMovementMethod` necesita si su animación de subida aplica root motion. Si el cliente
  no mueve al clon durante esa acción (se estudia aparte), siguen siendo necesarios
  los `MoveActorAck` sintéticos del §7.1.

Scripts nuevos: `build/research/bytewr.py` (accesos `byte [reg+off]`) y `offacc.py` (accesos
a un offset en todo el código).

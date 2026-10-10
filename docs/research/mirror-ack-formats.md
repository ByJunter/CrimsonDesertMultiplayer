# Formato de los Ack para el "espejo" (Req del jugador → Ack del clon)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son
RVA (`exe+…`). Análisis estático de solo lectura (capstone sobre el exe en disco) y
validación contra la captura `captures/cdmp_packets_20261003-112627.bin`. El juego no se
ejecutó.

Scripts nuevos en `build/research/`:
- `readtrace.py <rva>`: lista en orden las lecturas `IDeserializer::vf1 Read(dst, n)` de una
  función (y de sus helpers), con los bucles.
- `mirror_check.py`: parsea las Req capturadas con los formatos de abajo, comprueba los
  tamaños y construye los Ack espejo.
- `opscan.py <opcode>`: busca el opcode como inmediato (así se encontraron los
  constructores de los Ack en el servidor).
- `c8scan.py`: busca escrituras de `byte [reg+0xC8]` (estado de control del actor).

## Resumen

| Par | Veredicto | Req | Ack | Diferencia |
|---|---|---|---|---|
| MoveActor 2556 → 2697 | **difiere** | 514 B | **235 B** | el Ack mete un `u8` en `+0xDE` y **no lleva** el array de historial de 10×28 B del final |
| StartAction 2223 → 2991 | **difiere (solo orden)** | 725 B | **725 B** | el `u8` final de la Req va en el Ack **antes** del bloque grande (`+0x8E`) |
| RotateActor 2828 → 2687 | **difiere** | 176 B | **177 B** | el Ack mete un `u8` en `+0xA4` |
| ChangeAIState 2734 → 2404 | **idéntico** | 672 B | 672 B | mismo deserializador compartido `0x2444E50` |

Los cuatro Ack empiezan por la **clave u32 del actor en `+0x00`**, igual que las Req. Los
sub-structs son los mismos en los dos lados: los handlers llaman a los **mismos
deserializadores** (`0x2444040`/`0x12AD8D0`/`0x141FB70`/`0x141FA90`, `0x2442610`,
`0x2442BF0`, `0x2444E50`). Lo único que cambia es la "envoltura" de cada mensaje.

**Confirmación por el otro lado:** los constructores de estos Ack en el servidor existen
(restos del MMO) y escriben exactamente estos formatos:

| Ack | Constructor del servidor | Quién lo llama |
|---|---|---|
| MoveActorAck | `0x2974C00` | `0x2BCEAD9` / `0x2BCEB18` |
| StartActionAck | `0x2963FF0` | `0x2A7A85A` / `0x2A7A891` |
| RotateActorAck | `0x29750E0` | `0x106F32C0` |
| ChangeAIStateAck | `0x297CF50` | `0x2AC6727` |

El llamador elige el destinatario según ese `u8` extra:
- `ToNearSendTargetFinder` si vale ≠0;
- `ToNearExceptOneControlSendTargetFinder` si vale 0.

Es decir, el MMO **sí** difundía `MoveActorAck` a los demás jugadores cercanos, salvo al
que controla al actor. En un jugador no hay otras sesiones y por eso nunca se ve.

## Reglas comunes de los handlers Ack (vf2)

1. **Validación de trama.** `p->size-5 == *(u16*)(pkt+3)`. El deserializador
   (`DeserializerMemory`, vtable `0x5721C60`, lectura `0x141F270`, cursor en `+0x1C`) tiene
   que consumir **exactamente** `payload_len`. Si sobra o falta un byte, el handler
   devuelve `[0x6CDB970]` y el paquete se descarta en silencio. Un Ack de tamaño erróneo
   no hace nada.
2. **Búsqueda del actor por la clave de `+0x00`.**
   - Gestor: `mgr = [[exe+0x6D69188]+0x38]`. `+0x08` es el contenedor y `+0x10` el lock,
     así que es muy probablemente el `ClientActorManager`. Conviene verificar en vivo que
     es el mismo objeto que `[[exe+0x6D69190]+0x30]`.
   - Búsqueda: `0x8A67E0` → `0x8B3C00` → `0x464290(contenedor, key)`.
   - Si no hay actor con esa clave, `TrocParams+0` se queda en null (`ClientNet::OnRecvPacket`
     pone `ctx+0x50 = 0`) y el Ack **no hace nada**.
   - **No existe ninguna comprobación "si key == mi jugador, return"** en estos handlers.
3. **Puerta "actor aún no listo"** (solo Move, Rotate y StartAction; ChangeAIState no la
   tiene): `0x8B53C0(mgr, key)`.
   - Se salta si `key == 0` o si `(key & 0x30000000) == 0x10000000` (claves de usuario
     `0x9…`).
   - Para `0xA…`/`0xB…`: si el actor existe y **no** cumple `u16[actor+0x5E] == 0x10 &&
     byte[actor+0xCF] != 0`, el handler devuelve `[0x6CDB9D0]`.
   - `ClientNet::ProcessRecvQueue` (`0x35D1BD`) trata ese código como "reintentar" y
     mete el paquete en la cola de reintentos `ClientNet+0xF0`.
   - `+0x5E` son flags de tipo (`0x10` = personaje).
   - `+0xCF` lo pone a 1 `0x8B7B50` (`exe+0x8B816B`) cuando el login del personaje termina
     (`+0xC0` → `+0xC1` → `+0xCF`; atacadores `UserOrCharacterValidLoginedScopeAttacher`).
   - En el jugador real (dump `client_child_actor.txt`) vale `+0x5E = 0x0010` y
     `+0xCF = 1`.
   - **Riesgo:** si el clon no completa el login, sus Ack se quedan reintentando sin fin.
4. **Tipo de actor:** `t = byte[[actor+0x88]+1]`.
   - Move, Rotate y StartAction solo aplican si `t ∈ {1,3,4,5,6,8,9}` (tablas de salto
     `0xB62748`, `0xB62C48`, `0xB57B9C`). Con `t == 7` (u otro) no hacen nada.
   - Antes, `0x466060(actor)` (tipos 1 y 3–9) llama a
     `[[actor+0x68]+0x1A0]->vf(0x150)(0)`.
5. **Puerta "lo controla este cliente"**, dentro de las funciones que aplican el mensaje
   (Move `0x48BC60`, Rotate `0x505DF0`). `Z = *([[actor+0x68]+0x40] /*CharacterControl*/ + 0x118)`;
   `Z+8` es el actor. **No se aplica nada** si se cumple una de estas dos condiciones:
   - **`actor->vf(0x188)(5)`** (`0x4FA620`) devuelve true. Es `byte[actor+0xC8] != 0`.
   - `byte[[Z+0x60]+0x10] & 1`. Su significado no está identificado; hay que verificarlo
     en vivo en el clon.

   `byte[actor+0xC8]` es el **estado de control** y lo calcula `0x4FA530`:
   - `3` si el tipo es 7;
   - `2` si el dueño `actor+0xA0` es el `ClientUserActor` local y `byte[actor+0x94] != 0`;
   - `1` en el resto de casos;
   - **pero se fuerza a `0`** si `u32[actor+0x90]` (clave del usuario dueño) no es la del
     `ClientUserActor` local (y el bit 30 de la clave del actor es 0, como en `0xA…` y
     `0xB…`).

   Por tanto:
   - **El jugador local** tiene `+0xC8 = 2` (dump: byte `+0xC8 = 02`). Su propio
     MoveActorAck se ignoraría, y por eso el servidor del MMO usa `ExceptOneControl`.
   - **Los NPC que simula el cliente** tienen 1 o 2, y se ignoran.
   - **El clon** tiene como dueño el usuario `0x9010F001`, distinto del local
     (`0x90100000`). Debería tener `+0xC8 = 0` y **aceptar** Move y Rotate. Verificar en
     vivo: `clon+0xC8 == 0`, `clon+0x90 == 0x9010F001`, `clon+0x5E == 0x10`,
     `clon+0xCF == 1`.

   El "u8 extra" de StartAction relaja la segunda condición: `0x48AD40` solo descarta si
   `bit0 && u8 == 0`.

Cabecera para inyectar: `u16 opcode | u8 slot = 0 | u16 payload_len | payload` (se inyecta
igual que `ActorLoadDataAck` en el experimento 4c). En la tabla Troc los cuatro Ack tienen
mask 1, flags `0x200`, el vf1 por defecto `0xB54130` y tamaño 5..32710.

---

## 1. MoveActorReq (2556) → MoveActorAck (2697): DIFIERE

- Req vf2 `0x2977E10`, Ack vf2 `0xB621F0`. Constructor del Ack en el servidor:
  `0x2974C00`, que usa el serializador `0x2443F40`, simétrico de `0x2444040`.
- Bloque común "MoveData" (218 B): son las lecturas en línea del vf2. El mismo bloque está
  empaquetado en `0x2444040`, que se usa dentro de StartAction.

| Offset | Tamaño | Campo | Helper | Observado en la captura (jugador) |
|---|---|---|---|---|
| `+0x00` | 4 | u32 clave del actor | Read 4 | `0xA0100001` |
| `+0x04` | 12 | vec3 posición de mundo | Read 12 | `(-10213.6, 626.3, -3436.2)` |
| `+0x10` | 16 | quat rotación (xyzw) | `0x141FA90` | `(0, 0.518, 0, 0.855)` |
| `+0x20` | 4 | u32 (a veces una clave `0xB01…`, 198 de 14.004) | Read 4 | casi siempre 0 |
| `+0x24` | 40 | Transform: pos(12), quat(16), escala(12) | `0x141FB70` | identidad (escala 1 en `+0x40`; de ahí los "4 floats a 1.0" en `+0x3C`) |
| `+0x4C` | 136 | "Path" `0x12AD8D0` (ver abajo) | | |
| `+0xD4` | 1 | u8 | Read 1 | 0 (312 veces vale 1) |
| `+0xD5` | 8 | u64 **marca de tiempo del cliente en ms** (+~28 por paquete) | Read 8 | `0x4AA5B`… |
| `+0xDD` | 1 | u8 | Read 1 | 0 |
| **Req** `+0xDE` | 12 | cola: f32, f32, f32 (el tercero = velocidad, igual a \|v\| de `+0x74`) | Read 8 + Read 4 | `(0, 0, 3.26)` |
| **Req** `+0xEA` | 280 | 10 × {u32 id, u64, u64, u64} historial | bucle `cmp esi,0xA` | `0x000F424E`, … |
| **Ack** `+0xDE` | 1 | **u8 extra** = flag del servidor "enviar también al que controla". **El cliente lo lee y lo descarta** (`[rbp+0x150]` no se usa) | Read 1 | — |
| **Ack** `+0xDF` | 8 | f32, f32 de la cola. Se guardan en `[Z+0xE8]+0x90` | Read 8 | |
| **Ack** `+0xE7` | 4 | f32/u32 de la cola (velocidad) | Read 4 | |

Total: Req `0xEA + 280 = 514`; Ack `0xEB = 235`.

"Path" `0x12AD8D0` (136 B, en el payload en `+0x4C`):

| Offset en el struct | Campo |
|---|---|
| `+0x00` | Transform local (40); en `+0x4C` del payload |
| `+0x28` | vec3 (velocidad); en `+0x74` |
| `+0x34` | vec3; en `+0x80` |
| `+0x40` | **u64 clave del padre o montura**; en `+0x8C` |
| `+0x48` | Transform de mundo (40); en `+0x94` |
| `+0x70` | vec3; en `+0xBC` |
| `+0x7C` | vec3; en `+0xC8` |

- A pie: la clave del padre vale `0xFFFFFFFFFFFFFFFF` y el Transform local es la
  posición de mundo.
- **Montado:** la clave del padre es `0xB0100003` (el caballo, 3.162 paquetes), el Transform
  local es un offset relativo y el de mundo (`+0x94`) es la posición real.

Cómo lo aplica el cliente (`0x48BC60(Z', &MoveData, &cola)`, con
`Z' = [[actor+0x68]+0x40]+0x118`):
- escribe la posición y el quat en `[[actor+0x68]+0x1A0]+0x534/+0x540`;
- luego llama a `0x87DB50` y `0x87D9C0`, que interpolan con la marca de tiempo de `+0xD5`.

## 2. StartActionReq (2223) → StartActionAck (2991): DIFIERE (mismo tamaño, otro orden)

- Req vf2 `0x296CAA0`, Ack vf2 `0xB57720`. Constructor del servidor `0x2963FF0`: escribe la
  clave, 2×A (`0x2442360`), el u8 y B (`0x2442A90`).
- **A** = `0x2442610`, 69 B:
  - u32 hash de acción, u32;
  - 4×u16, u8;
  - 2×{3×u16, u8};
  - u8, 4×f32;
  - `0x1AB3AA0` (8 B), 9×u8, u32.
- **B** = `0x2442BF0`, 582 B:
  - MoveData `0x2444040` (218 B, el mismo bloque que MoveActor `+0x04…+0xDD`);
  - u32, u32 (a veces una clave `0xB01…`);
  - vec3, vec3, u32;
  - 9×u32, 2×u8;
  - 10×{u32, u64, u64, u64} (280 B);
  - u64, u8, u8.

| Req | Ack | Contenido |
|---|---|---|
| `+0x000` u32 | `+0x000` u32 | clave del actor |
| `+0x004` 69 B | `+0x004` 69 B | A[0] |
| `+0x049` 69 B | `+0x049` 69 B | A[1] |
| `+0x08E` 582 B | **`+0x08E` u8** | Req: B / Ack: **flag u8** |
| `+0x2D4` u8 | `+0x08F` 582 B | Req: flag u8 (891 de 916 veces vale 1) / Ack: B |

Total: 725 en los dos.

Claves de actor dentro del payload (offsets de la **Req**; en el Ack súmale 1):
- `+0xAA`: el u32 de MoveData `+0x1C`;
- `+0x116`: la clave del padre del Path;
- `+0x16C`: el segundo u32 tras MoveData, que suele ser una clave `0xB01…` (¿objetivo?).

Cómo lo aplica: `0x48AD40(Z', &res, &A[2], u8, &B)`. El u8 actúa como "forzar": evita la
salida por `bit0` de la puerta 5, y `vf188(1) && u8`.

## 3. RotateActorReq (2828) → RotateActorAck (2687): DIFIERE

- Req vf2 `0x29784C0`, Ack vf2 `0xB62770`. Constructor del servidor `0x29750E0`: escribe la
  clave, el quat, el Path, el u64, el u8, 8 B y 4 B.

| Offset | Req | Ack |
|---|---|---|
| `+0x00` | u32 clave | igual |
| `+0x04` | quat (16) | igual |
| `+0x14` | Path 136 B (clave del padre en `+0x54`, 1.084 de 1.121 veces es una clave `0xB01…`) | igual |
| `+0x9C` | u64 marca de tiempo en ms | igual |
| `+0xA4` | cola 8 B + 4 B (hasta `+0xB0`) | **u8 extra (se descarta)** |
| `+0xA5` | — | cola 8 B + 4 B (hasta `+0xB1`) |

Total: Req 176, Ack 177. Cómo lo aplica:
`0x505DF0(actor, &quat, &Path, u64, &cola)`. Tiene las mismas puertas `+0xC8` y `bit0`,
escribe la cola en `[Z+0xE8]+0x90` y llama a `0x9FCE60` y `0x87DB50`.

## 4. ChangeAIStateReq (2734) → ChangeAIStateAck (2404): IDÉNTICO

- Req vf2 `0x29820B0`, Ack vf2 `0xB69700`. **Los dos llaman a `0x2444E50`.** Constructor
  del servidor `0x297CF50`, que escribe la clave, el AIState (`0x2444CD0`), u32 y u32.
- Payload (672 B):
  - `+0x00` u32 clave;
  - `+0x04` AIState (660 B):
    - 6 × sub-struct `0x181CE70` de 102 B (24×{u8,u8}, 5×u64, u8, u32, u8, u32, u32);
    - u32 (`+0x268` del payload);
    - u32 (`+0x26C`);
    - 4×u32 (`0x151F020`);
    - u32, u64, u32, u32;
    - 4×u8;
  - `+0x298` u32;
  - `+0x29C` u32 (en el jugador, `FFFFFFFF FFFFFFFF`).
- Diferencias del handler respecto a los otros tres:
  - **no tiene** la puerta "no listo" ni el filtro por tipo;
  - necesita el componente Ai `[actor+0x68]+0x58`; si falta, registra el error
    `[0x6CF6E64]`;
  - `0x9245B0` no hace nada si `[[ai+0x48]+8] == 0` (no hay "cerebro" de IA, cosa probable
    en un clon del jugador);
  - si el u32 en `+0x26C` vale 0, devuelve el error `[0x6CF70EC]`.

---

## Receta: construir el Ack a partir de una Req capturada

`K'` = clave del clon (p. ej. `0xA010F001`). `req` = payload de la Req (sin los 5 bytes de
cabecera). La cabecera es `u16 opAck | u8 0 | u16 len(ack)`.

```text
MoveActorAck (2697), 235 B:
  ack = K' (u32) + req[0x04:0xDE] + 0x00 + req[0xDE:0xEA]
  (se descartan req[0xEA:0x202], el historial 10x28)

StartActionAck (2991), 725 B:
  ack = K' + req[0x04:0x8E] + req[0x2D4:0x2D5] + req[0x8E:0x2D4]

RotateActorAck (2687), 177 B:
  ack = K' + req[0x04:0xA4] + 0x00 + req[0xA4:0xB0]

ChangeAIStateAck (2404), 672 B:
  ack = K' + req[0x04:]
```

(Implementado y comprobado contra las 39.684, 23.669, 8.412 y 12.712 Req de la captura 2
en `build/research/mirror_check.py`: todos los tamaños cuadran.)

Retoques recomendados además de cambiar la clave:
1. **Desplazar la posición** si el clon no debe solaparse con el jugador. Move:
   - `+0x04` (vec3 de mundo);
   - `+0x4C` (Transform local del Path, si el padre es `-1`);
   - `+0x94` (Transform de mundo del Path, si no es cero).

   StartAction: los mismos campos dentro de B, que en el **Ack** empieza en `+0x8F`
   (MoveData pos en `+0x8F`, Path en `+0xD7`).
2. **Claves embebidas:**
   - Move: padre/montura u64 en `+0x8C` y u32 en `+0x20`;
   - Rotate: padre u64 en `+0x54`;
   - StartAction (offsets del Ack): `+0xAB`, `+0x117`, `+0x16D`.

   Apuntan a actores del PC que envía. En la prueba de un solo PC, al montar el
   **mismo** caballo, el clon "montaría" el caballo del jugador. Para empezar, escribir
   `0xFFFFFFFFFFFFFFFF` en el padre cuando no se quiera vincular, o traducir la clave
   (caballo del invitado → su caballo en el host).
3. **Marca de tiempo** (Move `+0xD5`, Rotate `+0x9C`, dentro de B en StartAction): es el
   reloj en ms del cliente que envió. En loopback sirve tal cual. Entre dos PC habrá que
   rebasarla al reloj local, porque se usa para interpolar.
4. El u8 extra de Move y Rotate se puede poner a 0, porque el cliente lo ignora. En
   StartAction conviene copiar el de la Req (normalmente 1 = "forzar").

## Qué verificar en vivo antes de culpar al formato

Si un Ack bien formado "no hace nada", lee en el actor del clon:
- `+0x60` == `K'` (lo encuentra el gestor `[[exe+0x6D69188]+0x38]`);
- `u16 +0x5E == 0x10` y `byte +0xCF == 1` (si no, el paquete se reintenta sin fin);
- `byte +0xC8 == 0` (si vale ≠0, el cliente cree que lo simula él y descarta Move y
  Rotate);
- `byte [[actor+0x88]+1]` ∈ {1,3,4,5,6,8,9};
- `byte [[*([[actor+0x68]+0x40]+0x118) + 0x60] + 0x10] & 1 == 0`.

Un hook de solo lectura en el retorno de cada vf2 (`*res`): `0` = aplicado, `[0x6CDB970]` =
formato erróneo, `[0x6CDB9D0]` = reintentar.

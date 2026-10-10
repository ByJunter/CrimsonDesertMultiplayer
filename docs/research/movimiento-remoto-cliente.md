# Movimiento de un actor remoto en el cliente (Move/Rotate/StartAction Ack)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`). Análisis estático de solo lectura (capstone sobre el exe en disco), más medidas sobre
las capturas `cdmp_packets_20261003-125936.bin` y `-131004.bin`. El juego no se ejecutó.

Lo que está **verificado en el código** va sin marca. Lo marcado **(hipótesis)** es una deducción
que falta confirmar en vivo; para cada una se indica cómo comprobarla.

Scripts nuevos en `build/research/`:
- `gvscan.py`: lista los `pa::GlobalVariable{Int,Float,Bool}` estáticos.
- `riprefs.py <rva>…`: referencias RIP que `xref.py` no indexa (`movsxd`, `mov r32`, `vcomiss`…).
- `dispscan.py <disp>`: accesos `[reg+disp32]` y la vtable dueña de cada uno.
- `mr_methods.py`: vtables de todos los `*MovementMethod`.
- `mr_ts.py` y `mr_ts2.py`: análisis de las marcas de tiempo de las capturas.
- `mr/*.asm`: desensamblados usados en este informe.

---

## 0. Resumen ejecutivo

1. **Un actor remoto (modo 2) no simula nada.** Cada frame, el método de movimiento activo llama
   a `ICharacterMovementMethod::vf2 = 0xAB0540`, que hace solo esto:
   - posición = interpolación lineal de un búfer de muestras de red;
   - rotación = interpolación de otro búfer;
   - velocidad = Δposición / dt.

   No hay extrapolación, ni root motion, ni física. Usan este mismo `vf2` casi todos los
   métodos: Normal, AnimationAll, WallUp, LadderClimb, PointClimb, WallFinish, Cover, Vehicle,
   RideOn/Off, Docking, Approach, ActionFixedPosition, MiniGame y AttackApproach.
2. **El búfer se rige por la marca de tiempo del paquete**, comparada con el reloj local del
   cliente (ms desde el arranque). Se renderiza en `now − 200 ms`.

   Reglas al insertar una muestra:
   - se **descarta** si su marca de tiempo es ≤ la de la última muestra;
   - se **descarta** si se movió ≤ 0,1 mm respecto a la última;
   - si han pasado **más de 1000 ms** desde la última muestra, **se vacía el búfer** y se
     reinicia desde la posición que se ve en ese momento.

   Caben como máximo 30 muestras.
3. **Por qué el clon se queda quieto y luego salta en el remate de una escalada:**
   - las muestras congeladas (las nuestras sintetizadas y las de `StartAction`) **no entran
     al búfer**, por la regla de 0,1 mm;
   - por eso pasan más de 1 s sin muestras y el búfer se reinicia;
   - el salto de unos 3 m se recorre entonces en unos 200 ms (≈15 m/s) o menos, y eso se ve
     como un pop o teletransporte.

   La causa de que además **se desvanezca** no está cerrada; ver §4.4.
4. **Las marcas de tiempo de `StartActionAck` no se están desplazando.** En la captura
   131004 llegan unos 985 ms tarde con un retardo de espejo de 1 s. Además, `StartAction`
   alimenta **el mismo búfer de posición** que `MoveActor`. Hay que desplazar su marca de
   tiempo en **`Ack+0x160`**.
5. **De los Ack, el cliente remoto solo usa** posición, quaternion y marca de tiempo, más los
   2 floats de la cola de Move y Rotate.
   - Ignora la velocidad, los dos vec3 extra del Path, el Transform de `+0x24`, los u8 y el
     float de velocidad de la cola.
   - En el modo remoto también ignora el `Path` de `RotateActor` (`+0x14`, `+0x48` y `+0x5C`).
6. **Un clon en modo 2 no puede aplicar el root motion de una acción él solo**, con ningún flag
   de `StartActionAck`. Solo lo haría en **modo 3** (simulación local, como un NPC). Se puede
   forzar con el bit 1 de `byte[[CC+0x60]+0x10]` (experimento con escritura; ver §5).
7. **Hay parámetros ajustables:** variables globales del juego, todas escribibles. Ver la
   tabla de §6.

---

## 1. Flujo de datos

```
ClientNet::OnRecvPacket 0x35CA00 → cola → hilo principal 0x35D0C0 → vf2 del Ack
   │
   ├─ MoveActorAck vf2 0xB621F0 ──► 0x48BC60(Z, &MoveData, &cola)
   ├─ StartActionAck vf2 0xB57720 ─► 0x48AD40(Z, &res, &A[2], u8force, &B)   (B empieza por MoveData)
   └─ RotateActorAck vf2 0xB62770 ─► 0x505DF0(Z, &quat, &Path, ts, &cola)
          │  Z = [CC+0x118]   ([Z]+8 = actor, [Z+0xC8] = mc)
          │  CC = ClientCharacterControlActorComponent = [[actor+0x68]+0x40]
          │  mc = movement controller = [CC+0xC8]   (+0x60 = CC, +0x68 = actor)
          ▼
   puertas: actor->vf188(5) (byte actor+0xC8 != 0)  ó  byte[[CC+0x60]+0x10] & 1  → no aplica
          ▼
   TransformSync = [[actor+0x68]+0x1A0]  (ClientTransformSyncActorComponent)
        +0x534 vec3 / +0x540 quat ← "última posición/rotación de red" (Move y StartAction)
   modo = 0x88E8E0(mc)  (1 local, 2 remoto, 3 simulado localmente)
          ▼
   0x87D9C0(mc,&modo,&pos,&Path,ts) ──► mc+0x3D0 búfer de POSICIÓN
        (llama por puntero global [exe+0x6C8CC98] = setInterpolatePositionData 0x11B1FD0)
   0x87DB50(mc,&modo,&quat,&Path,ts) ─► mc+0x488 búfer de ROTACIÓN
        ([exe+0x6C8CC38] = setInterpolateRotateData 0x11B26E0)
   Rotate además fija la rotación ya: 0x9FCE60(TransformSync, quat, tipo 5)
   Cola de Move/Rotate: 8 bytes → [[CC+0xE8]+0x90]

Cada frame (hilo del juego), por actor:
   0x88EB70 → 0x88F4E0 (paso de movimiento)
      modo = 0x88E8E0(mc)
      modo 1 → control local (0x890E55)
      modo 2 → 0x87B760 → método[mc+0xF0]->vf2 (vt+0x10) = 0xAB0540 en casi todos
                 0xAB0540: 0x88E6A0(mc) → [exe+0x6C8CCA0] = updateInterpolatePositionData 0x11B2420
                            [exe+0x6C8CC40] = updateInterpolateRotateData 0x11B2AF0
                            out.pos, out.quat, out.vel = (pos − pos_prev)/dt
      modo 3 → 0x879C50 → métodos con lógica local (root motion, IA)
   0x891180 (aplicar) → TransformSync::SetPosition 0x9FCA60(tipo) / SetRotation 0x9FCE60
          → petición pendiente en TransformSync+0x590 (tipo en +0x5A0; 0xF = ninguna)
          → la consume 0x9FF500 (velocidades suavizadas, física)
```

Los métodos `setInterpolate*` y `updateInterpolate*` se registran por nombre como
`::pa::gameClientScript::…` en `0x891D410` y `0xC30407`. Son **código nativo** (`0x11B1FD0`,
`0x11B2420`, `0x11B26E0` y `0x11B2AF0`). El juego guarda el puntero en las globales
`exe+0x6C8CC38…CA0` y los llama con `call r8`.

### 1.1 Modo de control `0x88E8E0(mc)`
Las condiciones se evalúan en este orden (`actor = mc+0x68`):
1. `0x17F2A50(actor)` es cierto → **1** (control local). Para actores de tipo 1, 3…9 equivale
   al bit 2 de `byte[[CC+0x60]+0x10]`.
2. `byte[[CC+0x60]+0x10] & 2` → **3**.
3. `u32[actor+0x90] != 0` (tiene usuario dueño) → **2**. **Este es el caso del clon**, cuyo
   dueño es `0x9010F001`.
4. Si es tipo 1 y no es el actor local → **2**.
5. Si no, según el componente Vehicle (`[actor+0x68]+0x48`, `+0xEB`, `+0xC8`) y `[CC+0x1E9]` →
   **1**, **2** o **3**.

Así quedan las flags de `[[CC+0x60]+0x10]`:
- bit 0: ignorar Move y Rotate de red (puerta del Ack);
- bit 1: forzar el modo 3;
- bit 2: controlado localmente.

### 1.2 Índices de método de movimiento (`mc+0xF0`, tabla en `mc+0xF8 + 8·i`)
La tabla la registra `0x88C530` mediante `0x879B80`.

| Índice | Método | vf2, la que se usa en modo 2 |
|---|---|---|
| 0–3 | Normal | `0xAB0540` |
| 4 | AnimationAll | `0xAB0540` |
| 5 | WallUp | `0xAB0540` |
| 6 / 7 | RideOn / RideOff | `0xAB0540` |
| 8 | LadderClimb | `0xAB0540` |
| 9 | Cover | `0xAB0540` |
| 10 | Vehicle | `0xAB0540` |
| 11 | Docking | `0xAB0540` |
| 12 | WallFinish | `0xAB0540` |
| 13 | Sequencer | `0x69C400`, propia |
| 14 | Approach | `0xAB0540` |
| 15 | MiniGame | `0xAB0540` |
| 16 | AttackApproach | `0xAB0540` |
| 17 | RopeSwing | el modo 2 llama a `vf10` (`vt+0x50`) |
| 18 | PointClimb | `0xAB0540` |
| 19 | AiJump | el modo 2 llama a `vf10` |
| 20 | Snowboard | el modo 2 no llama a nada |
| 21 | ActionFixedPosition | `0xAB0540` |

---

## 2. El búfer de interpolación (algoritmo exacto)

Hay dos búferes con la misma estructura: posición en `mc+0x3D0` y rotación en `mc+0x488`.

| Offset | Campo |
|---|---|
| `+0x00` | u8 `done`: 1 cuando el búfer se vacía; un push lo pone a 0 |
| `+0x08` | puntero al array de muestras de **0xA0 bytes** |
| `+0x10` | u32 count |
| `+0x14` | u32 capacidad |
| `+0x18` | 0xA0 bytes: última muestra interpolada |

Formato de una muestra:
- posición: `vec3 pos (12) | pad | Path (136, en +0x10) | u64 ts (+0x98)`;
- rotación: `quat (16) | Path (136) | u64 ts (+0x98)`.

**Reloj.** `now = 0x1416B80() + [exe+0x6D69438]`. El sesgo solo se suma en el hilo cuyo byte
TLS `+0x1EC` vale 0.
- `0x1416B80` devuelve los ms desde el arranque: en caché por frame, o un reloj monotónico en ns
  dividido entre 1e6.
- Es el mismo reloj con el que el dueño firma sus Req. En la captura 131004, `RotateActorReq.ts`
  coincide con el momento de envío con un error de 0–5 ms.
- Ojo: es un reloj de **frame**. En una pausa larga (`t = 163,47 s` en la captura 131004) los
  `MoveActorReq` salen con `ts` 1,3 s más viejo.

**Insertar (`setInterpolatePositionData`, `0x11B1FD0`).**
1. Si `now ≤ delay`, sale.
2. Si `count > 0`:
   - Si `ts_nueva ≤ ts_última`, **se descarta**: no se aceptan muestras fuera de orden ni
     duplicadas.
   - Si `ts_nueva − ts_última > 1000 ms` (**fijo en el código**), hay **reinicio**:
     - `count = 0`;
     - se inserta una muestra sintética con la posición mostrada ahora (`+0x18`) y
       `ts = now − delay`.
   - Si `|pos_nueva − pos_última| ≤ 1e-4` (constante `0x5D72C34`), **se descarta**: una
     posición quieta no añade muestras.
3. `push`.
4. Mientras `count > maxSamples` (30), se elimina la muestra más antigua (desplazando el array).

En rotación (`0x11B26E0`) es igual, pero el umbral es el ángulo entre quaternions
≤ 1,19e-7 (`0x539B10`).

**Leer (`updateInterpolatePositionData`, `0x11B2420`), cada frame.**
- Si `count == 0`: `out = posición actual` y `done = 1`.
- `T = now − delay` (200 ms). Si `T ≤ ts[0]`, **no escribe** `out`: el actor se queda donde
  está.
- Busca `i` con `ts[i−1] ≤ T ≤ ts[i]` y calcula `out = lerp(pos[i−1], pos[i], clamp01((T −
  ts[i−1]) / (ts[i] − ts[i−1])))`.
- Si `T` ya pasó la última muestra: `out = pos[última]`. **No hay extrapolación**: el actor se
  para.
- La muestra sintética `+0x18` se reescribe con `{out, Path vacío, T}`.
- **El `Path` de las muestras no se usa nunca**. Solo cuentan `pos` y `ts` (o `quat` y `ts`).

Luego `0xAB0540`:
- convierte `out` a la representación de mundo (float + sector `i16` × 1000 m);
- calcula la velocidad como `(out − pos_frame_anterior) / dt`. Esa velocidad la usan las
  animaciones de locomoción y la física.

### Consecuencias, condiciones de "snap" verificadas en este búfer

| # | Situación | Resultado |
|---|---|---|
| S1 | Más de 1000 ms entre **muestras aceptadas** | Reinicio: la nueva posición se alcanza en `ts_nueva − (now−200)`. Si `ts ≈ now`, son **unos 200 ms**: el salto se ve como un pop. |
| S2 | `ts_nueva < now − 200` (muestra vieja) | Se aplica de golpe, sin lerp. |
| S3 | S1 con `ts_nueva < now − 200` | Tras el reinicio el búfer queda `[sintética(ts=now−200), nueva(ts menor)]`, sin orden. La búsqueda falla y pasa a `pos[última]`: **snap inmediato**. Es lo que provoca un `StartActionAck` con la `ts` sin desplazar tras una pausa. |
| S4 | Más de 30 muestras con `ts` en el futuro (> 1 s por delante) | Se pierden las más antiguas. Cuando `T` alcanza `ts[0]`, salta de la posición actual a `pos[0]`. |
| S5 | Muestras con posición constante (congeladas) | Se descartan, y no evitan S1. |
| S6 | Sin muestras nuevas | El actor se queda en la última posición; no hay extrapolación. |

---

## 3. Semántica campo a campo (lo que usa el cliente remoto)

### MoveActorAck (2697, 235 B), `0xB621F0` → `0x48BC60`
Los offsets son del **payload**.

| Offset | Campo | Uso en el cliente remoto |
|---|---|---|
| `+0x00` | u32 clave | Búsqueda del actor |
| `+0x04` | vec3 posición de mundo | **Muestra del búfer de posición**, y `TransformSync+0x534` |
| `+0x10` | quat | **Muestra del búfer de rotación**, y `TransformSync+0x540` |
| `+0x20` | u32 (a veces una clave `0xB01…`) | No se usa en el modo 2 |
| `+0x24` | Transform (40) | No se usa |
| `+0x4C` | Path (136) | Se guarda en la muestra pero **no se usa** (solo en el modo 3 o en teletransporte) |
| `+0xD4` | u8 | No se usa |
| `+0xD5` | **u64 ts (ms, reloj del cliente)** | **Clave**: decide orden, interpolación y reinicios |
| `+0xDD` | u8 | No se usa |
| `+0xDE` | u8 extra del servidor | Se lee y se descarta |
| `+0xDF` | 2×f32 | Van a `[[CC+0xE8]+0x90]` (entrada o parámetros de locomoción; semántica sin verificar) |
| `+0xE7` | f32 velocidad | Se lee y **no se usa** |

### StartActionAck (2991, 725 B), `0xB57720` → `0x48AD40`
- `A[0]` y `A[1]` (en `+0x04` y `+0x49`): se decodifican con `0x1EC9C00` y arrancan las
  acciones mediante `0x863530`. Esto pone la animación y el método de movimiento (`mc+0xF0`),
  pero **no** mueve el actor.
- `+0x8E` u8 "force": salta la puerta del bit 0 y la de `vf188`. **No activa el root motion.**
- `B` en `+0x8F`: empieza por un MoveData de 218 B.
  - pos en `Ack+0x8F`;
  - quat en `+0x9B`;
  - Path en `+0xD7`;
  - **ts en `Ack+0x160`**.

  Se escribe en `TransformSync+0x534/+0x540` y se **inserta en los dos búferes**, igual que un
  MoveActor. En la captura, `StartAck.ts` está unos 985 ms atrasado: **no se desplaza**.

### RotateActorAck (2687, 177 B), `0xB62770` → `0x505DF0`

| Offset | Campo | Uso |
|---|---|---|
| `+0x04` | quat | `SetRotation` inmediato (tipo 5) y muestra del búfer de rotación |
| `+0x14` | Path (136) | En el modo remoto **solo se guarda**: no mueve la posición |
| `+0x9C` | u64 ts | Marca de tiempo de la muestra de rotación |
| `+0xA4` | u8 extra | Se descarta |
| `+0xA5` | 8 B de cola | Van a `[[CC+0xE8]+0x90]` |
| `+0xAD` | 4 B de cola | No se usan |

Partes del Path de Rotate:
- `+0x14`: posición local (pos 12, quat 16, escala 12);
- `+0x3C`: vec3 velocidad;
- `+0x48`: segundo vec3 (el mismo campo que `+0x80` en Move). Durante la escalada vale unos
  `(−0,05, −0,04, −0,05)` y probablemente es un delta por tick; no se interpreta en el modo 2;
- `+0x54`: u64 clave del padre;
- `+0x5C`: Transform de **mundo** (pos 12…), válido cuando hay padre;
- `+0x84` y `+0x90`: dos vec3.

**Conclusión: RotateActorAck no mueve nunca al actor remoto.** Por mucho que lleve una posición
en `+0x14` o `+0x5C`, el cliente no la aplica.

---

## 4. Aplicación al transform y teletransportes

### 4.1 ClientTransformSyncActorComponent (`[[actor+0x68]+0x1A0]`)
Es el transform "lógico" del actor. Lo que ve el resto del juego sale de aquí y de la física.

| Offset | Campo |
|---|---|
| `+0xA4` | quat local |
| `+0xB4` | vec3 posición **local de sector** |
| `+0xC0` / `+0xC2` | i16 sector X y Z (×1000 m). **Mundo = (x + sx·1000, y, z + sz·1000)**, lectura `0x4F8220` |
| `+0xC8…+0x108` | Transform del padre (identidad si no hay) |
| `+0x330` | vec3 velocidad suavizada |
| `+0x534` / `+0x540` | última posición y quat recibidos por red |
| `+0x580` | vec3 auxiliar |
| `+0x590` | WorldPosition pendiente (16 B) |
| `+0x5A0` | u8 **tipo** de la petición: `0xF` = ninguna, `4` = teletransporte, `5` = rotación, `7` = forzado/anclado, `0xC` = movimiento de red normal |

`SetPosition` (`0x9FCA60(ts, &wpos, tipo, flag, …)`):
- descarta valores no finitos;
- en personajes, deja la petición pendiente **solo si el nuevo tipo es ≥ el pendiente**;
- la consume `0x9FF500`. Con tipo 4 no calcula la velocidad (`0xA001CB`).

### 4.2 Paso de aplicación `0x891180`
Elige el tipo según `mc+0x70` (modo de ámbito: 0/1 normal, 2/3 anclado) y unos flags de la
salida del método:
- `+0x10A`: teletransporte, que de los métodos solo lo pone AiJump;
- `+0x10D`: anclado.

En un remoto normal el tipo es `0xC`. Solo cuando el tipo de partida es 0 o 1 (Sequencer o
casos de anclaje) se evalúan los umbrales de teletransporte, que lo convierten en tipo 4:
- distancia actual→objetivo **≥ 20 m** (GlobalVariableFloat `exe+0x6CB9728`, por defecto
  20,0);
- salto de velocidad `|v_objetivo| − |v_actual|` **> 8 m/s** (`exe+0x6CB9688`, por defecto
  8,0);
- si `ctx+0x68 ≤ 0`, cualquier velocidad > 0,001;
- `ctx+0x68 > 1,0` (`exe+0x6CB96D8`) **y** el actor es local → tipo 4.

### 4.3 Suavizado de velocidad en TransformSync (`0x9FF500`)
Son globales float:

| Dirección | Valor por defecto |
|---|---|
| `exe+0x6CC2628` | 2,0 |
| `exe+0x6CC2678` | 8,0 |
| `exe+0x6CC26C8` | 1,0 (umbral de velocidad que elige entre 2 y 8 como constante de `exp(−k·dt)`) |
| `exe+0x6CC2718` | 0,01 (velocidad mínima) |
| `exe+0x6CC2768` | 30,0 (tope: velocidad física + 30) |

Solo suavizan la **velocidad** que se pasa a la animación; no causan teletransportes.

### 4.4 El desvanecimiento al final del remate
**Lo verificado:** con un hueco de 1,2 s sin muestras aceptadas:
1. Se cumple S1: el búfer se reinicia.
2. El salto de unos 3 m se recorre en unos 200 ms (≈15 m/s), o de golpe (S2/S3) si la `ts`
   está atrasada. Las muestras sintetizadas con la posición congelada no lo evitan (S5).

**(Hipótesis)** El fundido es el camino de **teletransporte (tipo 4)**:
- con el método Sequencer o anclado, el salto de > 8 m/s activa el umbral de §4.2;
- otra posibilidad es que el propio remate (una acción de tipo secuencia) lo pida.

En la ruta normal (tipo `0xC`) no encontré ningún umbral de distancia que oculte el actor.

Cómo verificarlo en vivo, solo con lectura:
- un hook en la entrada de `0x9FCA60` que registre `r8b` (tipo) cuando `rcx` sea el
  TransformSync del clon;
- leer `mc+0xF0` (el método de movimiento) durante el remate.

Si aparece el tipo 4 o el método 13, la hipótesis queda confirmada.

---

## 5. Root motion y actores remotos

- Los datos de animación tienen `_useRootMotion` ("mover la **CharacterScene** con el
  **MoveControl Bone**") y `_rootMotionMovingCorrection` ("corregir el desplazamiento por root
  motion hacia el Transform objetivo"). Ver `GameBodyAnimationEventData` y las cadenas cercanas
  a `0x55CE350`.
- **En el modo 2, ningún método aplica el root motion al transform del actor.**
  - `0xAB0540` sobrescribe posición, rotación y velocidad solo con los búferes de red.
  - El método que elige `StartActionAck` (PointClimb, WallUp, AnimationAll…) solo cambia el
    índice `mc+0xF0`. En el modo 2 casi todos vuelven a `0xAB0540`.
  - Por eso el clon sube "en el sitio": la animación avanza, pero el transform espera a la
    red. **No existe ningún campo de StartActionAck que lo cambie**; el u8 "force" solo abre
    puertas.
- **El modo 3 sí simula localmente.** `0x879C50` llama a la lógica local de cada método:
  - Normal → `vf3`;
  - AnimationAll → `0x681320`;
  - PointClimb → `0x691DB0` (su `vf1`);
  - LadderClimb → `0x68DB20` y `0x68EB20`;
  - WallUp → `vf10`.

  Lo usan los NPC: aplican el root motion de la acción.

  Pero en el modo 3 **el búfer de red no se consume**: `0x88E6A0` solo se llama desde
  `0xAB0540`. Las posiciones de Move se ignoran, salvo la ruta directa de `0x87D9C0`, cuando
  `[[actor+0x68]+0x1A8]->vf(0x1E0)() == 4`.
- **Experimento posible (escritura en memoria del juego; copia de seguridad antes):**
  1. Poner el bit 1 de `byte[[CC+0x60]+0x10]` del clon **solo durante** acciones de root motion
     (el remate de escalada), desde el `StartActionAck` correspondiente.
  2. Quitarlo cuando vuelvan los `MoveActorAck` con movimiento.

  Así el clon haría el remate por sí mismo. Riesgos:
  - comportamiento de IA o física local no probado en un actor sin cerebro;
  - un desfase al volver al modo 2 (el reinicio de > 1 s lo absorbe).
- **¿Qué transform sigue al root motion en el jugador local?** Según lo observado, la física
  (`+0x90`) y TransformSync están congeladas durante el remate. Lo único que se mueve es la
  CharacterScene, que construye `ClientCharacterControlActorComponent::BuildCharacterScene`.
  - `CharacterScene` no tiene vtable con RTTI y **no localicé estáticamente** el puntero
    exacto.
  - Candidatos para examinar en vivo (comparar volcados cada 30 ms durante un remate):
    1. `[CC+0x140]`: objeto de control de física de personaje.
       - `+0x2C0` es la física (posición en `+0x90`);
       - `+0x2ED` es un flag;
       - `+0x404` es un vec3 que `0x9FF500` suma a la velocidad cuando `+0x2ED == 1`.
         Probablemente es la **velocidad de root motion**.
       - `+0x2B8`, `+0x2E4`, `+0x490` y `+0x4A4` los escribe el movimiento.
    2. `[CC+0x130]`: objeto de escena o animación. `0x88F110` lo recorre hasta un nodo
       `+0x1E8` con estado `+0x220`.
    3. El hueso raíz del esqueleto. Hay que localizarlo desde el componente de animación; no
       lo resolví.
  - Si `[CC+0x140]+0x404` (con `+0x2ED == 1`) es una velocidad no nula durante el remate,
    **integrarla** da la trayectoria real, y sirve para sintetizar `MoveActorAck` suaves.

---

## 6. Ajustes del cliente (variables globales; escribir el valor actual)

Cada `GlobalVariable{Int,Float}` guarda el valor actual en `obj+0x38` y el de por defecto en
`obj+0x3C`. Ninguna tiene referencias por nombre en el exe (el nombre va hasheado). Las de
interpolación solo las leen las 4 funciones de interpolación.

| Dirección (valor) | Tipo | Por defecto | Efecto |
|---|---|---|---|
| `exe+0x6CB9548` | int | **200** | Retardo de interpolación en ms (`T = now − delay`) |
| `exe+0x6CB9598` | int | **30** | Máximo de muestras por búfer |
| `exe+0x6CB9728` | float | **20,0** | Distancia de teletransporte (camino Sequencer o anclado, §4.2) |
| `exe+0x6CB9688` | float | **8,0** | Salto de velocidad que provoca teletransporte (§4.2) |
| `exe+0x6CB96D8` | float | 1,0 | Umbral de `ctx+0x68` (solo para actores locales) |
| `exe+0x6CB9638` | float | 0,3 | Parámetro de física `[CC+0x140]+0x2B8` en ciertos estados |
| `exe+0x6CB95E8` | float | −1,0 | Si es > 0, sustituye ese mismo parámetro |
| `exe+0x6CC2628`…`0x6CC2768` | float | 2 / 8 / 1 / 0,01 / 30 | Suavizado de velocidad (§4.3) |
| *fijo* | — | 1000 ms | Reinicio del búfer |
| *fijo* | — | 1e-4 m y 1,19e-7 | Movimiento mínimo de posición y de rotación para aceptar una muestra |

En un jugador, el modo 2 solo lo usa nuestro clon (los NPC usan el 3 y el jugador el 1), así que
cambiar estas variables no afecta al resto del juego.

---

## 7. De clave a actor (para leer el estado real del clon)

Hay dos gestores con la misma estructura:
- `M1 = [[exe+0x6D69188]+0x38]`, el que usan los vf2 de los Ack;
- `M2 = [[exe+0x6D69190]+0x30]`, el `ClientActorManager`, que usa `0x9FF500` con
  `0x8ADFE0(M2, &out, key)`.

**Función** (convención MS x64): `0x8A67E0(mgr, ScopeOut* out /*0x20 B*/, u32 key)` → `out`.
- `out+0x08` = actor\* (o null);
- `out+0x10` = u8 válido.

Toma un RW lock de lectura en `mgr+0x10` y **hay que liberar el scope** con
`0x141434880(out)`. Desde el mod es preferible la lectura directa.

**Lectura directa (protegida con SEH; sin lock, puede leer un estado a medias):**
```
C = [mgr+0x08]
nb = u32[C+0x88]   (cubetas)      n = u32[C+0x8C]  (elementos; si 0 → no hay)
bucket = [C+0x98] + (key % nb) * 0x100
cnt = u32[bucket+0]
for i in 0..cnt-1:
    if u32[bucket + 8 + i*8] == key:
        idx  = u32[bucket + 0xC + i*8]
        node = [[C+0xA0] + idx*8]
        if u32[node+4] == key: actor = [node+8]
```
`0x464290` hace exactamente esto. `0x8ADFE0` además comprueba los flags `u16[actor+0x5E]`
(`0x10` en los personajes y `4` en las claves de usuario `0x9…`) y `actor->vf(0xC0)(0x10)`.

**Desde el actor del clon (`0xA010F001`):**
```
comp = [actor+0x68]
CC   = [comp+0x40]                 ClientCharacterControlActorComponent
TS   = [comp+0x1A0]                ClientTransformSyncActorComponent
mc   = [CC+0xC8]                   comprobar: [mc+0x60]==CC, [mc+0x68]==actor
flags= byte[[CC+0x60]+0x10]        bit0 ignorar red, bit1 forzar modo 3, bit2 local
pos real del clon (mundo) = TS+0xB4 (x,y,z) + (i16 TS+0xC0)*1000 en X, (i16 TS+0xC2)*1000 en Z
última pos de red        = TS+0x534 / quat TS+0x540
física                   = [[CC+0x140]+0x2C0]+0x90 (+ offset de trozo exe+0x6D56500, como en el jugador)
búfer de posición        = mc+0x3D0: count u32 @+0x10, array @+0x08 (0xA0 B/muestra, ts @+0x98)
método activo            = byte mc+0xF0 (tabla §1.2);  byte mc+0x70 (modo de ámbito)
```
Error de seguimiento = `TS_mundo(clon)` − `posición del jugador` en el instante
`ts_muestra + 200 ms`.

---

## 8. Recomendaciones para el espejo, por prioridad

1. **Desplazar también la `ts` de StartActionAck** (`u64` en `Ack+0x160`), igual que Move
   (`+0xD5`) y Rotate (`+0x9C`).
   - Hoy llega unos 1 s vieja. Tras una pausa provoca un snap inmediato (S3) y, si no, se
     descarta.
   - Comprobación: `mr_ts2.py` debe dar `lag ≈ 0` para MoveAck, StartAck y RotAck del clon.
2. **Generar la `ts` con el reloj del receptor en el momento de inyectar.** No basta con
   `ts_owner + delay`.
   - Calibrar `offset = ts_RotateReq − reloj_mod_ms`; el `RotateReq` sale con `ts ≈ now` y un
     error de ±5 ms.
   - Inyectar con `ts = reloj_mod_ms + offset`, conservando el espaciado original.
   - Forzar que crezca estrictamente **por actor, sumando Move y StartAction** (comparten
     búfer), y lo mismo para Rotate con StartAction.
   - Nunca una `ts` menor que `now − 200` (S2) ni mayor que `now + 500` (S4).
   - Esto también absorbe las pausas del reloj del dueño (los Req viejos de 1,3 s en `t = 163 s`).
3. **No dejar nunca más de 1000 ms entre muestras *aceptadas*.** Aceptada significa que se
   movió más de 0,1 mm. Las sintéticas congeladas **no cuentan** (S5).
   - Si hay que mantener vivo el búfer, mover cada muestra sintética al menos 1 mm (por
     ejemplo, alternar ±1 mm en Y) y enviarlas a 10–30 Hz.
4. **Remate de escalada (congelado de unos 1,2 s y salto de unos 3 m):** aprovechar el retardo
   del espejo.
   - Con un retardo D ≥ 1,3 s sobre la cola de envío, al ver el salto se pueden **reescribir
     las muestras congeladas pendientes** como una trayectoria: primero subir en Y y luego
     avanzar en XZ, repartida en todo el intervalo congelado.
   - El búfer interpola entonces unos 3 m en 1,2 s (≈2,5 m/s): sin reinicio, sin pop y sin
     teletransporte.
   - Mejor aún, si se confirma en vivo: integrar `[CC_jugador+0x140]+0x404` (velocidad de root
     motion, §5) y enviar esa trayectoria.
5. **Velocidad y Path no importan en el remoto.** La animación de locomoción usa la velocidad
   *derivada* de las posiciones interpoladas.
   - Para que no "patine", las muestras deben ser regulares (unos 30 Hz) y sin escalones.
   - Los campos `+0x74`, `+0x80` y `+0xE7` se pueden dejar como vienen.
6. **Ritmo de envío:** 25–40 Hz para Move; Rotate solo cuando cambie.
   - Con 200 ms de retardo bastan unos 6 muestras en la ventana; con 30 de capacidad hay margen
     para 1 s.
   - Si se quiere más tolerancia al jitter de Steam, subir `exe+0x6CB9548` a 300–400 ms: más
     colchón a cambio de más latencia.
7. **Opcional y experimental:** forzar el modo 3 (bit 1 de `[[CC+0x60]+0x10]`) solo durante el
   remate, para que el clon aplique el root motion él solo (§5). Hacer copia de seguridad antes.
8. **Medir:** leer `TS+0xB4/+0xC0` del clon frente a la posición del jugador retrasada
   `D + 200 ms`, y vigilar `count` en `mc+0x3D0+0x10`. Si cae a 1–2 y luego sube, hubo un
   reinicio (S1).

---

## 9. Pendiente o no resuelto
- Puntero exacto a la CharacterScene y al hueso raíz. Hay candidatos en `[CC+0x130]` y
  `[CC+0x140]`.
- Confirmar la semántica de `[CC+0x140]+0x404`.
- Confirmar el fundido con el tipo 4 (hook de lectura en `0x9FCA60`).
- Significado de los 2 floats de la cola (`[[CC+0xE8]+0x90]`) y del `vec3` en Path `+0x34`.
- Ruta del modo 1 (`0x890E55`): no analizada.

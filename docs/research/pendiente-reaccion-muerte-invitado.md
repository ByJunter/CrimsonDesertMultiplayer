# Pendiente: que el jugador del invitado reaccione a los golpes y pueda morir

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`) y valen solo para esta build.

Método:
- análisis estático de solo lectura (capstone sobre el exe en disco, siguiendo los saltos a `.xtls`);
- las 50 capturas (`bin64\cdmp_packets_20261004-*` y `D:\cdmp-grabaciones`). En dos de ellas
  (`080253` y `090008`) el jugador **murió de verdad** tres veces, con el eco de `cdmp_peer` activo;
- el código actual de `player_hits.cpp`, `guest_hits.cpp`, `local_capture.cpp`, `remote_puppet.cpp`
  y `aggro_test.cpp`.

El juego no se ejecutó.

Marcas:
- **[verificado]**: leído en el código;
- **[captura]**: medido en capturas;
- **[inferido]**: deducción pendiente de prueba en vivo.

Notación:
- **H**: anfitrión;
- **G**: invitado;
- **E**: enemigo de H;
- **E'**: su copia en G (`0xB01F0000+n`);
- jugador `0xA0100001`;
- marioneta `0xA010F001` (la copia del otro jugador);
- AAR = `ApplyActionRestrictionAck` (2162), ATA = `PostProcessAttackResult_AttackTargetAck` (2922),
  buff de golpe = `ApplyBuffAck` (2506) de `CC_Push…`.

Scripts y volcados en `build/research/p_guest_react/` (§9). Continúa
[pieza4-tambaleo.md](pieza4-tambaleo.md), [pieza4-dano-servidor.md](pieza4-dano-servidor.md) §8 y
[pieza4-botin-reaccion.md](pieza4-botin-reaccion.md) §8.

---

## 0. Resumen

1. **Un golpe real sobre el jugador llega igual que sobre un enemigo:** ATA, AAR y buff de golpe con
   el mismo id, y después daño. [captura, 615 golpes, 39 sesiones, 128 atacantes]
   - En los golpes con daño, **193 de 248** traen AAR, y los 193 traen detrás su buff de golpe
     (1104/1105/1106/1107/60008).
   - **192 de 193** hicieron reaccionar al jugador. Su propio cliente mandó `StartActionReq` con la
     reacción a los **19 ms** de mediana (p90: 25 ms), precedido de `EchoActionSessionIDReq` y
     `ChangeAIStateReq`×2.
   - Sin AAR casi nunca hay reacción. La receta de la pieza 4 vale tal cual, cambiando de lado:
     **objetivo = nuestro jugador; atacante o fuente = E'**.
2. **La reacción la lanza el cliente del propio G**, porque G controla a su jugador. El servidor de G
   solo recibe el `StartActionReq` de siempre. [verificado + captura]
   - La marioneta de G en H ya repite esa acción: `local_capture` la reenvía.
   - En `080253`, la marioneta repitió la reacción `096435EA→C1AB6A49` del jugador. También repitió
     la muerte `889616A9→33E568ED`.
   - **H no necesita nada nuevo para ver el tambaleo de G.**
3. **Para morir de forma nativa, `KillCheatReq(nuestro jugador, opción 0)`.** [verificado + captura]
   - **`0x17AC040` solo mata del todo si el registro de daño trae `+0x4F = 1`.** Con 0, deja al
     actor "fatal" (`NotifyHandleFatalAck`).
     - `KillCheatReq` pone `+0x4F = 1` cuando la opción es 0 (`0x2C40EBA`–`0x2C40ECB`);
     - `VaryStatCheatReq` lo deja siempre en 0 (`0x2C3FF67`).

     Eso explica V2 ("fatal" y muerte 5 s después) frente a L1 (muerte inmediata).
   - El estado fatal **en un jugador** (tipo 1) lanza otra acción global (`0x1797BB0`). No hay
     ninguna captura de ese estado en un jugador, así que podría quedarse "vivo a 0". **No usar
     `VaryStatCheatReq` ni `VaryStatByAccInputReq` para llegar a 0 HP.**
   - El permiso de trucos ya pasó sobre nuestro propio jugador: V1, `114648`, `VaryStatCheatReq` →
     `VaryHPAck` con fuente `A0100001`. [captura]
4. **La muerte real de un jugador y la reaparición son nativas y ya se ven en la copia.**
   [captura, 3 muertes]
   1. En el mismo lote del golpe letal llega `NotifyHandleDeadAck(jugador, asesino, 8)`.
   2. El cliente lanza la acción de muerte en 17-25 ms.
   3. Unos **10 s** después: `ReviveLoadingReq(2)` → `LoadingFieldDataAck` (punto de reaparición) →
      `ReviveReq(2)` → `VaryHPAck` (lleno) + `NotifyHandleReviveAck` + `ReviveAck` → `LoadingCompleteAck`.

   `NotifyHandleDeadAck`, `NotifyHandleReviveAck` y `ReviveAck` ya están en `kRelayedAcks`. En las dos
   sesiones, **la marioneta murió y revivió** 0,4-0,6 s después.
5. **Carencias que encontré por el camino** [captura + inferido]:
   - **17 %** de los golpes al jugador (106 de 615) salen de un `DoFrameEventReq` **sin lista de
     objetivos**: es el servidor el que barre. En H, el servidor no conoce la marioneta, así que esos
     golpes a G **se pierden** en la pieza 2.
   - En las flechas (accessor de tipo 1) el "hash" es un contador, así que `player_hits` no aprende su
     daño.
   - Con `aggro_test` forzado, `copy_state_normal()` no mira si la marioneta está muerta
     (`Status+0x273`). Los enemigos de H podrían seguir pegando al cadáver de G.

**Recomendación (§6):** opción **A + B**:
- **reacción:** el trío ATA + AAR + buff inyectado en el cliente de G, desde E';
- **plantillas:** las aprende H con los golpes que recibe su jugador y viajan en el `PlayerHit`;
- **daño no letal:** como ahora (`VaryStatByAccInputReq`, mínimo 1 HP);
- **golpe letal:** `KillCheatReq(propio, 0)`;
- **G muerto:** ignorar `PlayerHit`;
- **H:** dejar de forzar el objetivo sobre una marioneta muerta.

---

## 1. Cómo llega un golpe real al jugador [captura]

`hits_on_player.py` → `hits_on_player.csv` (todas las capturas; ATA con objetivo `A0100001`).

### 1.1 El lote del servidor (≈9 ms después del `DoFrameEventReq` del enemigo)

Ejemplo: `080253`, t = 117,773 (flecha de `B01003EA`, golpe 1105):

```
S>C ATA                  010010a0 ea0310b0 f7 … 07 … (hash) … +0x81=1 +0x82=1
S>C ApplyActionRestriction 010010a0 … +0x1C=1 +0x24=0x800000A1 +0x34=67AE8750 … +0x49=ea0310b0
S>C ApplyBuffAck         010010a0 51040000(1105) … a1000080(id = AAR) ea0310b0 010010a0 27 … (pos. atacante)
S>C VaryHPAck            010010a0 40420f00 hp … fuente ea0310b0
S>C ApplyBuffAck ×3      40011 (+0x10 = 0,1,2), fuente = el propio jugador
S>C DataDefinedRegenerateValueAck ×2
S>C ApplyBuffAck         65418
S>C OnPostBuffDamagedAck 010010a0 ea0310b0 ea0310b0 60ea0000 (60 HP) …
S>C ApplyBuffAck         1021 (Damage_Percent)          ← el buff de daño
S>C VarySharpnessAck ×3  slots 3/4/6, −1                 ← desgaste del equipo
S>C AttackerAck          (al enemigo)
C>S EchoActionSessionIDReq 010010a0 01000000 00000000   ← eco de AAR +0x1C
C>S ChangeAIStateReq ×2
C>S StartActionReq       010010a0 096435ea → c1ab6a49   ← la reacción (11 ms)
```

### 1.2 Estadística

| Golpes al jugador | n | Reacción (`StartActionReq` ≤120 ms) |
|---|---|---|
| `F7` con AAR (todos con daño y buff de golpe) | 193 | **192** (mediana 19 ms) |
| `F7`/`E7` con daño, sin AAR | 55 | 23 (otras acciones, no de tambaleo) |
| Sin daño (guardia, esquiva…; buffs 51495, 24, 1025, 51401…) | 367 | casi ninguna |

- **AAR ↔ buff:** 193 de 193, con el mismo id. Los campos son los mismos que sobre enemigos:
  - `+0x14 = 0`, `+0x1C = 1`, `+0x44 = 0`, `+0x48 = 1`;
  - ATA con `+0x81 = +0x82 = 1` y `+0x10 = 7` (564 de 615).
- **Claves y reacciones del jugador** (`StartActionReq` nuevo):

  | Clave AAR | Buff | n | Reacciones más frecuentes |
  |---|---|---|---|
  | `79156D1B` | 1104 `CC_PushLight` | 75 | `0AE7FCF4`, `DE394E73`, `496AABC1`, `F966B53B` |
  | `67AE8750` | 1105 `CC_PushNormal` | 83 | `53F5FA58`, `38931230`, `F966B53B`, `496AABC1` |
  | `F931B81A` | 1106 `CC_PushHard` | 29 | `47E122BD` (16), `496AABC1` |
  | `67AE8750` | 60008 | 4 | `496AABC1`… |
  | `E55D252F` | 1107 `CC_KnockDown` | 2 | `CC630CF0` |

- **P(AAR) por ataque** (hash de ATA, solo golpes con daño): de 64 hashes, 56 siempre, 4 nunca y 4
  mixtos (`A8DF9560` 19/20, `607CAA10` 12/14, `3C159924` 9/10, `0BF80BB7` 1/2). Es casi binaria, como
  en los enemigos.
- **Accessor → ATA:** cada accessor (hash de acción + índice de frame event) del enemigo da **un
  solo** hash de ATA (`acc_map.py`). Una tabla `(variante, accessor) → trío` es estable.
- **Vida de los buffs** (`buff_life.py`):
  - 1104/1105/1106/1107, 40011, 65418, 1021 y 1023 **nunca** reciben `UnapplyBuffAck`. Son
    instantáneos, como en los enemigos;
  - el 24 dura unos 1,8 s y el 60008, unos 1,6 s.

### 1.3 Origen del golpe (`sweep_hits.py`)

| `DoFrameEventReq` del atacante, justo antes | Golpes |
|---|---|
| Su lista de objetivos nombra al jugador (accessor tipo 0: 420; tipo 1, flechas: 89) | 509 |
| **Lista vacía: el servidor barre él mismo** | **106 (17 %)** |

La pieza 2 solo ve el primer caso (`targets_of`). En H, el servidor no conoce la marioneta y no la
encuentra al barrer. **Uno de cada seis golpes a G no se transmite** [inferido de lo anterior].

---

## 2. Qué hace el cliente con cada paquete, si el objetivo es nuestro jugador

| Paquete | Handler | Efecto en el jugador local | ¿Inyectable en G con E'? |
|---|---|---|---|
| ATA | `0xB59280` → `0x451020` | Busca al atacante; si no existe, no hace nada más. Empuje físico si flags `&4` (`0x45C8A0` → `0x503E80`). Efectos y sonido del impacto **solo si `+0x81 = 0`** (`0x45B700`; en los reales vale 1). Evento de IA y cambio de objetivo (`0x45D800`) [verificado, `pieza4-botin-reaccion.md` §3.1] | **Sí**: E' existe en el cliente de G. Sin E', ATA no |
| AAR | `0xB5AEE0` → `0x48B400` | Escribe la restricción y lanza la acción global `E69AB342` (prioridad 6). `+0x1C` → `0x8B4790`, que es el id de sesión de acción del jugador local: de ahí el `EchoActionSessionIDReq(…, 1)`. Si el objetivo está muerto, nada [verificado] | **Sí**. La fuente es opcional (11 AAR reales con fuente 0 reaccionaron) |
| Buff de golpe | `0xC13D70` → `0xA16F60` → `CommonDamagedActionRestrictionBuffProcessor` | Vuelve a aplicar la restricción con duración. Eventos `0x63`/`0x64` [verificado, `pieza4-tambaleo.md` §2]. Sin él, R1 se saltó el tambaleo | **Sí**, con el mismo id que el AAR |
| `OnPostBuffDamagedAck` | `0xB70890` → `0x632EC0` → `.xtls 0x901D160` | Status `vf(0x198)` del dañado. Si el atacante es el jugador local, número de daño (cola `[[0x6D69190]+0x80]+0x240`) [verificado]. Para nosotros, "estado dañado" o destello [inferido] | Cosmético y opcional (P3) |
| `VaryHPAck` | — | Lo manda el servidor de G al aplicar el daño (fuente 0 o el propio jugador) | No hace falta |
| `NotifyHandleDeadAck` | `0xB764A0` → `0x634420` → `.xtls 0x902F790` → `0x1797850` → `.xtls 0xC76CF30` | Marca muerto (`+0x272 = 0x100`, hora `+0x268`) y llama `vf(0x150)`. Con el asesino encontrado: `vf(0x300)` y `vf(0x308)`, con el motivo. El asesino es **opcional**: nulo si no se encuentra. El id de sesión va a `0x8B4790` [verificado] | **No inyectar**: lo manda el servidor de G (§3) |

---

## 3. Morir de forma nativa

### 3.1 La rama de muerte de `0x17AC040` [verificado, `pieza4/s_17ac040_varyhp.asm`]

Con delta < 0 y el HP nuevo ≤ 0 (`0x17AC381`–`0x17AC47B`):
- **registro (`vf53`) `+0x4F == 0` y HP máx > 0** → **fatal**:
  - si no lo era ya: `+0x272 = 1`;
  - `vf(0x268)(0x13)`: `NotifyHandleFatalAck`.
- **si no** (`+0x4F != 0`) → **muerte**:
  - `+0x268 = ahora`;
  - `vf(0x150)`;
  - `+0x272 = 0x100` (`+0x273 = 1`);
  - `+0x274` si el HP máx coincide.

Quién rellena `+0x4F`:

| Req | Estructura del registro | `+0x4F` | Resultado |
|---|---|---|---|
| `KillCheatReq(clave, opción)` | `[rbp-0x80]`; `0x2C40EBA`: `+0x4F = (opción == 0)` | **1 con opción 0** | Muerte inmediata (L1, motivo `0x10`) [captura] |
| `VaryStatCheatReq` | `[rbp-0x40]`; `0x2C3FF67`: `dword +0x4F = 0` | 0 | Fatal (V2) [captura] |
| `VaryStatByAccInputReq` | No pasa por `0x17AC040` (`0x17AE4B0`) | — | `OnBattle_HandleKnockOut`, sin muerte (53 enemigos "zombis") [captura] |
| Golpe nativo | Lo rellena la cadena de daño | 1 en los 3 golpes letales a jugadores de las capturas | Muerte directa, sin fatal (0 de 49 capturas con fatal real) |

### 3.2 ¿Y "fatal" en un jugador?

`HandleFatal` `0x17979F0`:
- `+0x272 = 1`, `vf(0x268)`, `vf(0x168)` y `vf(0x160)`;
- tipo 1 (personaje jugador; `cmp byte [[a+0x88]+1], 1`) → lanza la acción global
  `[0x6A6C650]`/`[0x6A6C64C]` con `0x207AA10` (`0x1797BB0`–`0x1797BF6`);
- tipos 3-9: tabla de saltos.

[verificado]

- No se sabe qué acción es, ni si el jugador sale solo. En los enemigos fue su cliente el que mandó
  `DieByRunOutFatalReq` 5 s después. El remitente está virtualizado: hay 14 inmediatos `0x911`, casi
  todos en código ofuscado de `.xtls`.
- Hay cadenas `_delayDeadFatal` y `_delayKnockOut` de `ActionRestrictionOrderInfo`,
  `OnBattle_HandleFatal`, `OnBattle_HandleKnockOut` y `OnBattle_HandleRevive`.
- **Conclusión:** "fatal" es un estado intermedio sin probar en el jugador [inferido]. Puede valer la
  pena explorarlo (¿derribado y reanimable?, prueba F1), pero no como camino principal.

### 3.3 Lo que hace el cliente tras la muerte (las 3 muertes reales) [captura, `death_tl.py`, `scan_death.py`]

| Δt desde la muerte | Dirección | Mensaje |
|---|---|---|
| 0 | S>C | (lote del golpe) `VaryHPAck hp 0` → `OnPostBuffDamagedAck` → **`NotifyHandleDeadAck(jugador, asesino, 8, 0, sesión 1, 0x0A)`** → `ClearAllyGroupDataAck` → `ElementalMaterialFuelAck` |
| +17-25 ms | C>S | `EchoActionSessionIDReq`×2, `ChangeAIStateReq`…, `ChangeAggroRelationValidTickReq`, `ChangeBattleStateReq(0)`, **`StartActionReq` → acción de muerte** (`33E568ED`, `0859C754`, `5E81B5B3`), `Gimmick*Req` |
| +1,1 s | C>S | `UpdateSequencerGamePlayDataEventReq(0x198, 0)` (pantalla de muerte) |
| +9,75-10,0 s | C>S | `UpdateSequencerGamePlayDataEventReq(0x198, 1)`, **`ReviveLoadingReq(02 00)`** |
| | S>C | `ReviveLoadingAck(jugador, 2)`, `DisbandMercenaryAck`, **`LoadingFieldDataAck`** (posición del punto de reaparición; a unos 160 m en `080253`), retiradas de secuenciadores |
| +11,5 s | C>S | **`ReviveReq(jugador, tipo 2, 0, 0, 0)`** |
| | S>C | `VaryHPAck` (HP lleno), **`NotifyHandleReviveAck`**, **`ReviveAck(jugador, 2, …, 0x00019010)`** |
| +13,7 s | S>C | `LoadingCompleteAck` |

- `ReviveReq` en el servidor (`0x298C070`): `u32 clave, u8 tipo (< 9), u8, u8, u8` → `0x1794000`
  (`Status`). Existe el enum `ReviveType` y datos `FieldReviveInfo`, `_reviveInPlaceHardDifficulty` y
  `_giveUpFieldReviveList` [verificado: cadenas]. Las 3 muertes usaron el tipo 2.
- Si los ~10 s son automáticos o los pulsó el usuario, no se sabe.

### 3.4 Lo que ve H (la copia de G) [captura, `puppet_death.py`]

| Sesión | Muerte del jugador | Marioneta |
|---|---|---|
| 080253 | 119,719; acción `33E568ED` a 119,744 | `NotifyHandleDeadAck` relevado a 120,099 (asesino → la propia marioneta); `StartActionAck 889616A9→33E568ED` a 120,165; `NotifyHandleReviveAck`/`ReviveAck` a 131,662 |
| 090008 | 149,108 / 204,808 | Muerte relevada a 149,596 / 205,418; acción de muerte a 149,710 / 205,418; reviven a 160,751 / 216,581 |

- Todo esto ya lo hace el código actual: `kRelayedAcks` + reenvío de `StartActionReq`.
- La teleportación al punto de reaparición llega como movimiento con salto > 15 m
  (`kGapMaxJump`).
- Si G desaparece durante la carga, `coop.cpp` vuelve a cargar la copia.
- **Los enemigos de H frente a una marioneta muerta: sin datos.** En esas sesiones ningún enemigo
  apuntaba a la marioneta.
  - `IsTargetable` (`0x22141E0`) **no mira `Status+0x273`** (`companeros-mercenarios.md` §3.3).
  - `aggro_test::copy_state_normal()` tampoco, así que con Ctrl+F11 una marioneta muerta seguiría
    siendo "fijable" [verificado en el código del mod].
  - Si la IA del cliente descarta objetivos muertos por otra vía, está sin comprobar [inferido].

---

## 4. Plantillas: de dónde sale el trío en G

En G, el ataque solo se conoce por el `PlayerHit`: copia E', hash del accessor y daño. Hace falta el
ATA, el AAR y el buff de ese ataque sobre un **jugador**.

| Fuente | Cobertura | Calidad |
|---|---|---|
| **H aprende de los golpes a su propio jugador**: `DoFrameEventReq(E → propio)`, ≤60 ms ATA(propio, E), AAR(propio, fuente E) y el buff con el mismo id. Clave `(variante, hash del accessor, índice de frame)` | Los ataques que ya golpearon a H. Cada variante suele tener pocos | **Exacta**: es el mismo enemigo, el mismo ataque y la misma especie de objetivo (Kliff) |
| G aprende de los golpes de sus propios enemigos (el mismo código en los dos lados) | Baja si la supresión 3g quita los hostiles de G | Exacta |
| Plantilla genérica: el último trío 1104 visto | Total | Aproximada: reacción ligera para todo |

Como en la pieza 4: P(AAR) por ataque ≥ 0,8 → con tambaleo; si no, solo el ATA.

**Formato propuesto** (`PlayerHit` v2; **sube `kVersion`**):

```
u32 E' | u32 hash accessor | i64 daño×1000 | u32 índice frame | u8 tipo accessor | u8 banderas
[ u16 n_ata | ATA | AAR(77) | buff(77) ]
```

- Banderas: bit 0 = trae plantillas; bit 1 = con tambaleo.
- Unos 400 B, por debajo de `kMaxEventPayload = 1024`.

---

## 5. Opciones

| | Qué | Fidelidad | Riesgo | Esfuerzo |
|---|---|---|---|---|
| **A** | **Reacción inyectada en G**: trío ATA + AAR + buff con objetivo = jugador propio y atacante o fuente = E', en una pasada (`puppet_inject_s2c_batch`, como `guest_hits`). Daño como hoy (`VaryStatByAccInputReq`, mínimo 1 HP) | Tambaleo, empuje y dirección nativos: los ejecuta el cliente de G con sus gráficos. Sin desgaste del equipo (`VarySharpness`) | **Bajo**: es la misma vía que la pieza 4 (R1b). Buffs instantáneos, nada pendiente en el servidor. Peor caso, el jugador hace el `A73D6EC4` y no tambalea | Bajo: reutiliza `guest_hits` |
| **B** | **Muerte por `KillCheatReq(propio, 0)`** cuando el daño ≥ HP propio (de `VaryHPAck`) | Muerte, pantalla, reaparición y relevo a H nativos. Asesino = el propio jugador, motivo `0x10` | **Bajo-medio**: permiso probado sobre el jugador (V1) y muerte directa probada (L1). Falta ver si el motivo `0x10` o "asesino = yo" cambian la pantalla o la penalización [inferido] | Muy bajo |
| B' | Como B, reescribiendo en `on_recv_packet` el S>C `NotifyHandleDeadAck` propio: asesino → E' y motivo → 8 | "Te mató X" correcto | Medio: hay que **modificar** un paquete real en el hook (hoy solo se copia). Solo si B enseña algo raro | Bajo-medio |
| C | "Fatal" (`VaryStatCheatReq` hasta 0) como estado de derribo, rematado con `DieByRunOutFatalReq` o `KillCheatReq` | ¿Derribo nativo? | **Alto/desconocido**: jamás capturado en un jugador. Puede quedarse vivo a 0 | Bajo (solo prueba F1) |
| D | Golpe "sombra" nativo en el servidor de G: forjar `StartActionReq` + `DoFrameEventReq` de un NPC real del mundo de G de la misma variante contra el jugador | **Total**: daño con la defensa de G; ATA, AAR, buffs, `VarySharpness` y muerte con el asesino real | **Alto**: hace falta un gemelo vivo y cerca en el servidor de G (los spawns no son deterministas, `enemigos-del-anfitrion.md` §4.2 D). Validaciones de la acción y del frame (§1.2 de `pieza4-dano-servidor.md`). Mueve NPC reales | Alto |

---

## 6. Plan recomendado (A + B)

**En G (receptor del `PlayerHit`)**, en este orden:
1. **Si nuestro jugador está muerto**, se ignora el golpe:
   - se marca al ver `NotifyHandleDeadAck(propio)` y se desmarca con `ReviveAck(propio)`;
   - también entre `ReviveLoadingAck` y `LoadingCompleteAck`.
2. Daño = el del evento, con el tope de la mitad del máximo que ya existe. `letal = daño ≥ HP propio`
   (último `VaryHPAck`).
3. **Reacción, no letal y con E' en el cliente:** `puppet_inject_s2c_batch`, en una pasada:
   - **ATA** de la plantilla: `+0x00` = jugador propio; `+0x04` = E' (si E' no existe, sin ATA);
   - **AAR** si la plantilla tambalea: `+0x00` = propio; `+0x24` = `0x8000E000 + (n & 0xFFF)`, un
     rango propio distinto del `0x8000F000` de la pieza 4; `+0x49` = E' (o 0). El resto, de la
     plantilla (`+0x14 = 0`, `+0x1C = 1`, `+0x44 = 0`, `+0x48 = 1`);
   - **buff de golpe** con el mismo id:
     - `+0x00`/`+0x1C` = propio; `+0x18` = E' (o 0);
     - `+0x29` = posición de E' en G (de `actor_world_position`), que da la dirección de la reacción;
     - `+0x39`/`+0x41` = último reloj de servidor visto más el tiempo transcurrido, como en
       `guest_hits`.
4. **Daño, después de inyectar:**
   - no letal → `VaryStatByAccInputReq(propio, HP, −daño)` con el mínimo de 1 HP actual. Nunca llegar
     a 0 por esta vía;
   - letal → **`KillCheatReq(propio, 0)`**. Con un golpe letal se puede inyectar antes el ATA, para el
     empuje. El AAR y el buff no hacen falta: el cliente ignora la restricción sobre un muerto, y la
     acción de muerte manda.

**En H (emisor):**
- En `hits_observe_c2s` y `hits_observe_s2c`, aprender las plantillas de los golpes a nuestro jugador
  (§4), y mandarlas en el `PlayerHit`.
- **No mandar `PlayerHit` si la marioneta está muerta.** Se marca con el `NotifyHandleDeadAck`
  relevado y se desmarca con el `ReviveAck`.
- `aggro_test::copy_state_normal()`: devolver falso si `Status+0x273 != 0`.

**Ini:** `[coop] player_hit_react=1` y `player_hit_lethal=1`, desactivados por defecto, como
`guest_hit_stagger`.

**Fuera de alcance, pero apuntado:**
- los golpes "de barrido" (17 %);
- el aprendizaje de flechas (tipo 1: usar una clave fija, como `kProjectileAct` en `guest_hits`).

---

## 7. Pruebas en vivo (un PC, `cdmp_peer --mode relay` en eco)

En eco, "G" y "H" son el mismo PC:
- un enemigo forzado con Ctrl+F11 a pegar a la marioneta genera un `PlayerHit` que vuelve a
  nosotros;
- las copias E' (`0xB01F…`) existen porque el relevo de enemigos también hace eco.

Antes de empezar:
- **copia de la partida** en `backups/`;
- salir **sin guardar**.

| # | Qué | Predicción (`cdmp.log` + captura) | Si falla |
|---|---|---|---|
| **P1** | `player_hit_react=1`; dejar que E pegue a la marioneta con un ataque que tambalea (P(AAR) ≥ 0,8 en la tabla de H) | En la captura, el trío inyectado sobre `A0100001` con fuente `B01F00xx`. En ≤30 ms, C>S `EchoActionSessionIDReq(A0100001, 1)`, `ChangeAIStateReq`×2 y **`StartActionReq prev → reacción`** de la familia de la clave (1104 → `0AE7FCF4`/`DE394E73`/`496AABC1`/`F966B53B`; 1105 → `53F5FA58`/`38931230`/`F966B53B`; 1106 → `47E122BD`). En pantalla, nuestro personaje tambalea. La marioneta repite la acción ~delay después | Si sale `A73D6EC4` o nada: P1b (AAR y buff con fuente 0, sin ATA). Si tampoco: P1c, control con un trío real reciente del jugador, solo cambiando el id |
| P2 | Igual, pero solo el ATA | Sin `StartActionReq` de tambaleo (como los 340 `F7` sin AAR). Quizá un empujón (flags `&4`) | — |
| P3 | ATA con `+0x81 = 0` y, aparte, `OnPostBuffDamagedAck(propio, E', E', daño)` | Efecto y sonido del impacto (`0x45B700`) o destello de daño. Verificar con captura de pantalla (`CopyFromScreen`) | Dejarlos fuera: es cosmético |
| **P4** | `player_hit_lethal=1`; bajar el HP con enemigos normales hasta que un `PlayerHit` sea letal | C>S `KillCheatReq(A0100001, 0)` → mismo lote `VaryHPAck hp 0 fuente A0100001` + **`NotifyHandleDeadAck(A0100001, A0100001, 0x10)`**. En ≤30 ms, `StartActionReq` → acción de muerte y `ChangeBattleStateReq(0)`. Pantalla de muerte. A los ~10 s, `ReviveLoadingReq(2)`, `ReviveReq(2)` y `ReviveAck`. La marioneta muere y revive (relevo) | Sin pantalla o atascado: anotar el estado, quizá por el motivo `0x10` (→ B'). Salir con `Stop-Process`; la partida no se guardó |
| P5 | Tras P4, mirar en H si algún enemigo sigue mandando `DoFrameEventReq` con la marioneta muerta como objetivo, con Ctrl+F11 encendido y apagado | Con el arreglo de `copy_state_normal`, ninguno. Los `PlayerHit` mientras estemos muertos se descartan (log) | Si siguen: forzar el cambio de objetivo (`CurrentTargetChangedAck`) a otro |
| F1 (opcional) | `VaryStatCheatReq(A0100001, HP, −HP)` | `NotifyHandleFatalAck(A0100001)` y la acción global de "fatal". Qué hace el jugador y si llega solo `DieByRunOutFatalReq` | Rematar con `KillCheatReq(propio, 0)` |

---

## 8. Preguntas abiertas

1. ¿El motivo `0x10` o un asesino igual a uno mismo cambian algo en la pantalla de muerte, en la
   penalización o en las misiones? (P4)
2. Guardia, esquiva y súper armadura:
   - H no sabe si G estaba bloqueando, y G aplica daño y tambaleo igualmente;
   - en las capturas, los golpes sin daño coinciden con acciones concretas del jugador (`EB340761`
     ×83, `3AB993E6` ×38, `5F42B65E`, `E858D9A2`, `9134A0C8`…; `nodmg_hits.py`);
   - posible mejora: que G no aplique nada si su acción en curso está en el conjunto "sin daño"
     aprendido.
3. ¿La IA de los enemigos de H suelta un objetivo muerto por sí sola? (P5)
4. ¿La reaparición puede ser "en el sitio" (`ReviveType` ≠ 2, `_reviveInPlaceHardDifficulty`)?
   Así G no acabaría lejos de H. Haría falta otro `ReviveReq`; no se ha probado.
5. El daño viene del jugador de H, con su defensa. Habría que escalarlo con la defensa de G, que se
   aprende igual que en `guest_hits`.
6. ¿Conviene seguir relevando a la marioneta de H los buffs de golpe del jugador?
   - La marioneta recibe `ApplyBuffAck` 1105 (relevo) y puede relanzar `E69AB342` por su cuenta
     (`0x182C4A0`, sin AAR previo) [inferido].
   - Ya pasa hoy en eco, sin problemas conocidos.
7. Los golpes de barrido (17 %) y las flechas en `player_hits`.

---

## 9. Scripts, volcados y direcciones

### Scripts (`build/research/p_guest_react/`, con `build/research/venv/Scripts/python.exe`)

| Script / fichero | Qué hace |
|---|---|
| `scan_death.py` | Mensajes de muerte y reaparición por sesión (las del jugador están en `080253` y `090008`) |
| `death_tl.py <sesión> [pre] [post]` → `tl_080253.txt` | Línea de tiempo alrededor de cada muerte del jugador |
| `hits_on_player.py` → `hits_on_player.csv` | Los 615 ATA al jugador: lote (AAR, buff, daño) y respuesta C>S en 120 ms |
| `acc_map.py` | Estabilidad accessor → (hash de ATA, AAR, buff) |
| `sweep_hits.py`, `proj_hits.py` | Golpes por lista de objetivos y por barrido; proyectiles |
| `buff_life.py` | Vida (`UnapplyBuffAck`) de los buffs que acompañan a los golpes |
| `nodmg_hits.py` | Acción del jugador en los golpes sin daño |
| `puppet_death.py` | Marioneta y enemigos alrededor de las muertes |
| `immscan.py`, `win.py`, `ripwrite.py` | Inmediatos de opcode, ventana de desensamblado, accesos RIP sin REX |
| `c_*.asm` / `x_*.asm` / `s_*.asm` | Cliente `.rsrc` / `.xtls` / servidor (lista abajo) |

### Direcciones nuevas o reutilizadas (build 2976)

| RVA | Qué es |
|---|---|
| `0x17AC3AE`–`0x17AC3F2` | `0x17AC040`: rama fatal (`registro+0x4F == 0` y HP máx > 0 → `+0x272 = 1`, `vf(0x268)(0x13)`) |
| `0x17AC3F7`–`0x17AC47B` | `0x17AC040`: rama de muerte (`+0x272 = 0x100`, `vf(0x150)`) |
| `0x2C40EBA`–`0x2C40ECB` | `KillCheatReq`: `registro+0x4F = (opción == 0)` (opción en `r9`) |
| `0x2C3FF67` | `VaryStatCheatReq`: `registro+0x4F = 0` |
| `0xB764A0` | Cliente: handler de `NotifyHandleDeadAck` (`u32` muerto, `u32` asesino, `u8` motivo, `u8`, `u64` sesión, `u8`) |
| `0x634420` → `.xtls 0x902F790` | Status del cliente: muerte (asesino opcional; sesión → `0x8B4790`) |
| `0x1797850` → `.xtls 0xC76CF30` | Muerte común: `+0x272 = 0x100`, `vf(0x150)`, `vf(0x300)`/`vf(0x308)` (asesino y motivo) |
| `0xB76900` → `0x17979F0` | `NotifyHandleFatalAck` → `HandleFatal`; tipo 1 → acción global `[0x6A6C650]`/`[0x6A6C64C]` (`0x1797BB0`) |
| `0xB76D40` / `0xB6F590` / `0xB6FA60` | Cliente: `NotifyHandleReviveAck` / `ReviveAck` / `ReviveLoadingAck` |
| `0x298C070` → `0x1794000` | Servidor: `ReviveReq` (`u32`, `u8` tipo < 9, `u8`, `u8`, `u8`) |
| `0x298C490` / `0x29947E0` | Servidor: `ReviveLoadingReq` / `DieByRunOutFatalReq` |
| `0xB70890` → `0x632EC0` → `.xtls 0x901D160` | Cliente: `OnPostBuffDamagedAck` (Status `vf(0x198)`; números de daño si el atacante es local) |
| `0x8B4790` (→ `.xtls 0x98CD370`) | Id de sesión de acción del jugador local (AAR `+0x1C`, muerte `+0x0A`) → `EchoActionSessionIDReq` |
| `0x45B700` | ATA: efectos y sonido del impacto si `+0x81 == 0` (sin abrir del todo) |
| `0x22141E0` | `IsTargetable`: no comprueba `Status+0x273` |

Opcodes:

| Opcode | Mensaje |
|---|---|
| 2530 | `NotifyHandleDeadAck` |
| 2899 | `NotifyHandleFatalAck` |
| 3053 | `NotifyHandleReviveAck` |
| 2475 | `ReviveAck` |
| 2993 | `ReviveLoadingAck` |
| 2988 | `ReviveReq` |
| 2362 | `ReviveLoadingReq` |
| 2321 | `DieByRunOutFatalReq` |
| 2279 | `KillCheatReq` |
| 2703 | `VaryStatCheatReq` |
| 2181 | `VaryStatByAccInputReq` |
| 2681 | `OnPostBuffDamagedAck` |
| 2350 | `EchoActionSessionIDReq` |

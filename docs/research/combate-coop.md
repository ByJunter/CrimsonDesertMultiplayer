# Combate en cooperativo: mensajes, relevos del MMO y plan por etapas

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`) y valen solo para esta build. Análisis estático de solo lectura (capstone sobre el
exe en disco) más las capturas `captures/cdmp_packets_20261003-112627.bin` (captura 2, sin
truncar) y `-105149.bin` (captura 1, truncada a 1 KB). El juego no se ejecutó.

Lo marcado **(hipótesis)** falta confirmarlo en vivo; la §6 dice qué prueba lo decide.
Scripts y volcados en `build/research/combat/` (lista en la §8).

---

## 0. Resumen

1. **El golpe lo detecta el cliente, el daño lo calcula el servidor.** Cuando un ataque
   conecta, el cliente envía **`DoFrameEventReq` (2380)** con el atacante, el frame event de
   la acción en curso y una lista de **objetivos** (u32 clave + transform + punto de impacto
   + hueso). El servidor valida, calcula y responde en ~5 ms con
   `PostProcessAttackResult_AttackTargetAck`/`_AttackerAck`, `VaryHPAck`,
   `OnPostBuffDamagedAck` (daño), `ApplyActionRestrictionAck` (aturdimiento),
   `ApplyBuffAck`, `VarySharpnessAck` y, si muere, `NotifyHandleDeadAck`.
   `AttackByCollisionReq` (2815) casi no se usa (6 veces en la captura 1).
2. **Los enemigos los simula el cliente.** Su movimiento, su IA (`ChangeAIStateReq`), su
   objetivo (`CurrentTargetChangeReq`), sus acciones y sus golpes contra el jugador
   (`DoFrameEventReq` con atacante `0xB01…`) salen del cliente. La reacción a un golpe
   también: unos 20 ms después de recibir `AttackTargetAck`, el cliente envía
   `EchoActionSessionIDReq` + `ChangeAIStateReq` + `StartActionReq` del enemigo. El
   servidor solo lleva HP, buffs, muerte y botín.
3. **En el MMO, el jugador B recibía casi todo por `ToNear`**, es decir, todos los cercanos
   incluido el atacante: `AttackTargetAck`, `AttackerAck`, `VaryHPAck`, `NotifyHandleDeadAck`,
   `OnPostBuffDamagedAck`, `ApplyActionRestrictionAck`, `ApplyBuff/UnapplyBuffAck`,
   `CreateProjectileAck`, `VarySharpnessAck`. Las reacciones del monstruo le llegaban como
   relevo (`StartActionAck`…) del cliente que controlaba a ese monstruo.
4. **Etapa A (solo visual): falta el relevo de "frame events" del servidor.** Sacar y
   guardar el arma no es una acción aparte: es el frame event `ChangeDockingSocket`, que en el
   MMO el servidor reenviaba a los demás con **`ChangeDockingSocketByFrameEventAck` (2747,
   `ToNearExceptOneSelf`)**. En un jugador nunca se emite (no hay destinatarios) y nuestro
   clon no lo recibe **(hipótesis: el arma del clon no cambia de sitio)**. Lo mismo pasa con
   los cambios de equipo (`OtherEquipItemAck` 2757). `ChangeBattleStateReq` **no** se reenvía
   a nadie, así que no hace falta.
5. **Sí podemos inyectar Req en el servidor local**: `ClientNet::PostPacket` (`0x35C670`) copia
   el paquete y lo encola, y ya lo tenemos enganchado. Pero `DoFrameEventReq` exige que el
   **servidor** tenga al atacante ejecutando **la misma acción** (hash, id y nº de frame event).
   El clon no existe en el servidor, así que reenviar sus golpes tal cual no funciona.
6. **Hay dos Req de daño sin esa validación**, candidatas para la etapa B:
   - **`VaryStatByAccInputReq` (2181)**: `clave, u32 stat, i64 delta`. Sin comprobar dueño
     ni lista blanca de stats en el handler. HP es el stat `0x000F4240`.
   - **`AttackByCollisionReq` (2815)**: `víctima, atacante, …, hash de ataque`.

   Las dos están **sin probar**.
7. **Recomendación:**
   - etapa A con relevos capturados en el emisor (docking, buffs, proyectiles, equipo);
   - etapa B en dos pasos:
     1. **daño espejo** (cada PC conserva sus enemigos y se reenvía el daño ya calculado,
        aplicado con `VaryStatByAccInputReq` si la prueba T2 sale bien);
     2. **enemigos del host** retransmitidos al invitado como NPC de "otro dueño" (como en el
        MMO).

---

## 1. Catálogo de mensajes de combate (capturas)

Captura 2: 856 s, ~10 combates (a caballo con arco y a pie con espada). "Pico" = máximo en una
ventana de 1 s. "P" = clave del jugador `0xA0100001`; "W" = actor del mundo `0xB01…`. Script
`combat_cat.py` (salidas en `cat_112627.txt` y `cat_105149.txt`) y `peak.py`.

### 1.1 Cliente → servidor

| Mensaje (op) | Tamaño | Total / pico/s (jugador) | Claves (offset del payload) | Qué es |
|---|---|---|---|---|
| **DoFrameEventReq (2380)** | 517 + n·665 (hasta 3302) | 1311 / 32 (307 / 10) | `+0x01` atacante; objetivo *i* en `len − (n−i)·665` | Frame event de servidor de la acción en curso. Con n>0 objetivos es **el golpe** (§1.4) |
| StartActionReq (2223) | 725 | 23669 / 172 (916 / 7) | `+0x00` actor, `+0x16C` **objetivo actual** (lock-on), `+0x116` padre | Cada ataque, bloqueo, esquiva o reacción es una acción. Ya se reenvía al clon |
| ProcessAfterBeginFrameEventReq (2189) | 4 | 1:1 con StartActionReq | `+0x00` actor | Sigue a cada StartActionReq |
| CurrentTargetChangeReq (2984) | 18 | 404 / 12 (326 / 11) | `+0x00` actor, `+0x04` **nuevo objetivo**, `+0x08` u32 motivo | Mayoritariamente enemigos que pasan a apuntar al jugador |
| AggroRelationListReq (3006) | 25 + 16·k | 64 | `+0x01` actor (tras el tag u8 `04`), lista de `{u32 hash, u32, u32, u32 clave}` | Relaciones de agresividad (cliente) |
| ChangeBattleStateReq (2257) | 5 | 33 (4) | `+0x00` actor, `+0x04` u8 estado | El servidor solo guarda el byte (`comp+0x164`) y ejecuta `0x22218F0`. **No se reenvía** |
| CreateProjectileFromFrameEventReq (2387) | 142/154 | 44 (20) | `+0x00` dueño, `+0x84` objetivo | Flechas |
| OnHitProjectileReq (2999) | 28 | 31 | `+0x00` dueño, `+0x08` dueño, `+0x0C` **objetivo**, `+0x10` vec3 | Impacto de flecha (seguido de un DoFrameEventReq con objetivo) |
| RemoveProjectileReq (2399) | 77 | 52 | `+0x00` dueño | |
| EchoActionSessionIDReq (2350) | 12 | 60 | `+0x00` actor | El cliente confirma la nueva sesión de acción de un enemigo golpeado |
| AttackResultBackgroundOnlyReq (2577) | 6 | 4 (26 en C1) | `+0x00` jugador, u8, u8 | |
| AddAttackImpulseCountReq (2705) | 8 | 3 | `+0x00` W | |
| AttackByCollisionReq (2815) | 44 | 0 (6 en C1) | `+0x00` víctima, `+0x04` atacante (0 en las muestras), `+0x0C` vec3, `+0x18` vec3, `+0x24` hash (`0xF2EE2678`) | Daño por colisión física (§4.3) |
| VaryStatByAccInputReq (2181) | 16 | 56 | `+0x00` actor, `+0x04` u32 stat, `+0x08` i64 delta | En las capturas: estamina del caballo (`0x000F425A`, −8000) |
| OnDemandEquipDockingGimmickSpawnReq / Unspawn (2249/2451) | 7-17 | 37/10 | `+0x01` actor | Casi todo enemigos (sacar su arma "on demand") |

### 1.2 Servidor → cliente

| Mensaje (op) | Tamaño | Total / pico/s | Campos conocidos |
|---|---|---|---|
| **PostProcessAttackResult_AttackTargetAck (2922)** | 199 (211 montado/proyectil) | 107 / 8 | `+0x00` **objetivo**, `+0x04` atacante, `+0x08` u32 flags de resultado, `+0x24` u32 hash del ataque, `+0x2C…` u64 ids de ataque, `+0x69` vec3 posición del objetivo, `+0x83` vec3 punto de impacto, quats, **FrameEventAccessor al final (12 B)** |
| **PostProcessAttackResult_AttackerAck (2509)** | 160 (172) | 98 / 5 | `+0x00` **atacante**, `+0x04` objetivo, mismo hash en `+0x24`, accessor en `+0x60` |
| PostProcessAttackResult_AttackTarget_SkillAck (2846) | — | 0 | variante de habilidad |
| **VaryHPAck (2147)** | 92 | 150 / 7 | `+0x00` actor, `+0x04` stat `0x000F4240` (HP), **`+0x08` i64 HP×1000**, **`+0x18` i64 HP máx×1000**, `+0x30` u64 id de ataque, **`+0x40` u32 secuencia por actor**, `+0x54` fuente |
| **OnPostBuffDamagedAck (2681)** | 53 | 67 / 7 | `+0x00` objetivo, `+0x04` atacante, `+0x08` fuente, **`+0x0C` u32 daño×1000** (13000 = 13 HP, cuadra con VaryHP), `+0x2C` u64 id de ataque |
| ApplyActionRestrictionAck (2162) | 77 | 46 | `+0x00` objetivo, `+0x49` fuente (aturdir o derribar) |
| VarySharpnessAck (2532) | 8 | 60 | `+0x00` jugador, u16, u16 (filo del arma) |
| ApplyBuffAck / UnapplyBuffAck (2506/3007) | 77 | 1884/5212 (jugador 379/242, pico 22/17) | `+0x00` portador, `+0x04` id de buff, `+0x14` instancia, `+0x18` fuente, `+0x1C` portador, `+0x29` vec3 |
| NotifyHandleDeadAck (2530) | 19 | 13 | `+0x00` **muerto**, `+0x04` asesino, `+0x08` u8 motivo (8 = matado, 7 = por sí mismo), … |
| CreateProjectileAck / RemoveProjectileAck (2250/2751) | 127-289 / 81 | 54/52 | `+0x00` dueño, `+0x39` jinete o jugador, `+0x3D` objetivo |
| CatchActionResultAck (2156), ReleaseCatchResultAck (2512) | 84/245 | 8/1 | `+0x00` jugador, `+0x04` agarrado |
| AiEventAck (2218) | 100 | 104 | `ToTarget` (solo al que controla la IA) |
| LootDropCharacterRewardListAck (2897), RemoveLootableAck (2822) | | 9/2 | `+0x01` muerto; `+0x00` W y `+0x04` jugador |

`StartActionAck` (2991) y `ChangeAIStateAck` (2404) no aparecen nunca: son relevos
`ExceptOneControl` (ver [broadcast-servidor-movimiento.md](broadcast-servidor-movimiento.md)).

### 1.3 Secuencia de un golpe (captura 2, espada a pie, t = 487,2 s)

```
486.907 C>S StartActionReq        A0100001  acción f104f0b7, objetivo B01006D8
487.203 C>S DoFrameEventReq       1307 B: atacante A0100001, accessor {0,0,5a2ea805,0,4}, 1 objetivo B01006D8
487.209 S>C AttackTargetAck       B01006D8 <- A0100001, hash 3bac94eb, accessor {…5a2ea805…,4}
487.209 S>C ApplyBuffAck ×3       A0100001 (efectos del golpe sobre el atacante)
487.209 S>C OnPostBuffDamagedAck  B01006D8 daño 13000
487.209 S>C AttackerAck           A0100001 -> B01006D8
487.209 S>C VaryHPAck             B01006D8 hp 30000 -> 17000 (máx 70000)
487.209 S>C VarySharpnessAck      A0100001
(~20 ms) C>S EchoActionSessionIDReq / ChangeAIStateReq / StartActionReq del enemigo (su reacción)
```

Un enemigo de 70 HP muere en 4-8 golpes. `NotifyHandleDeadAck` llega en el mismo lote que el
último `VaryHPAck` (hp=0) y el `RemoveActorAck` del cadáver, de 15 a 75 s después
(`hpseq.py`).

### 1.4 Formatos (offsets del payload, empaquetado sin alineación)

**DoFrameEventReq (2380)**, handler del servidor `0x296D130`:

| Offset | Contenido |
|---|---|
| `+0x000` | u8 `04` (tag de "clave de actor"; el mismo prefijo aparece en AggroRelationList y otros) |
| `+0x001` | MoveData del atacante (222 B, igual que MoveActorReq `+0x00…+0xDD`): clave, pos, quat, …, u64 marca de tiempo en `+0x0D6` |
| `+0x0DF` | historial 10 × 28 B (como MoveActorReq) |
| **`+0x1F7`** | **FrameEventAccessor, 12 B**: `u8 tipo` (0 = acción, 1 = otra vía; 3 = inválido) · `u8 slot` · `u32 hash de la acción` · `u16 id` · `u32 índice del frame event` |
| `+0x203` | u16 etiqueta de variante (0-6 con datos; 7 = vacía) + datos. Variante 0 (125 B) = volumen del ataque: vec3, quat, vec3, quat, vec3, … |
| `len − n·665` | **n entradas de objetivo, 665 B cada una** (`0x1F78430`): `+0x000` u32 clave del objetivo; `+0x004` MoveData del objetivo (218 B); `+0x0DE` u32; **`+0x0E2` vec3 punto de impacto**; `+0x0EE`/`+0x0FA` vec3; **`+0x106` nombre del hueso** (p. ej. `"Bip01 Spine2"`); … |

Comprobaciones del servidor (`0x296D130` → `0x2A896E0`):
- el atacante existe y lo controla la sesión (`vf160`/`vf170` == `actor+0x90`; error `0x6CF7A04`);
- posición distinta de (0,0,0); accessor de tipo distinto de 3;
- n ≤ 30, claves de objetivo ≠ 0, con el bit 30 a 0 y **en orden creciente**;
- **la acción en curso del atacante en el servidor** (`[[comp+0x88+slot·8]+0x18]`) existe
  (error `0x6CF6C60`), su hash es el del accessor, su u16 (`[x+0x38]+0x5E`) coincide y el
  índice es menor que el nº de frame events; si no, se descarta en silencio;
- para cada objetivo con el **mismo dueño** que el atacante, aplica su MoveData al
  TransformSync del servidor (`0x2BC5420`, sin Ack): el cliente informa de dónde estaba el
  enemigo al recibir el golpe.

Después llama al procesador de frame events (tipo 0 → `[actor+0x68]+0x40` → `0x2A896E0`;
tipo 1 → `+0x88` → `0x2BE0D90`). La clase la decide el **dato de la acción** del servidor,
no el paquete. Tabla tipo → clase de la fábrica `0x28F0210` en `fetype_map.txt`: 0 Attack,
1 AttackThrow, 2 AttackCatch, 7 Projectile, 26 DetachDockingGimmick,
**27 ChangeDockingSocket**, 50 AttachDockingGimmick…

`FrameEventAccessor` se (de)serializa en `0x1F78F20`/`0x1F78DE0`, **virtualizadas por Denuvo**
(saltan a `.xtls`). El orden del cable sale de las capturas.

**VaryStatByAccInputReq (2181)**, handler `0x2972720`: `u32 clave, u32 stat, i64 delta`
(16 B). Busca el actor por clave en el gestor del servidor (`[exe+0x6D69698]`, `0x2A827B0`),
traduce el id de stat con la tabla `[exe+0x6D69AE8]` y llama a `0x17AE4B0(stats, índice,
ahora, delta)`. **No comprueba dueño ni qué stat es.**

**AttackByCollisionReq (2815)**, `0x2A3D430` → `0x2C3D7D0`:
- formato: `u32 víctima, u32 atacante, u32, vec3, vec3, u32 hash, u32`;
- la víctima debe tener el mismo dueño que la sesión;
- el atacante es opcional (si existe, debe estar en el mismo field);
- aplica `0x2BDA2F0` sobre la víctima, sin validar ninguna acción.

### 1.5 ¿Quién lleva la clave del OBJETIVO?

- **C→S:**
  - `DoFrameEventReq`: cada entrada de objetivo, más su transform;
  - `StartActionReq` `+0x16C`;
  - `CurrentTargetChangeReq` `+0x04`;
  - `AggroRelationListReq` (lista);
  - `OnHitProjectileReq` `+0x0C`;
  - `CreateProjectileFromFrameEventReq` `+0x84`;
  - `AttackByCollisionReq` `+0x00` (víctima).
- **S→C:**
  - `AttackTargetAck` `+0x00`;
  - `AttackerAck` `+0x04`;
  - `VaryHPAck` `+0x00` (fuente en `+0x54`);
  - `OnPostBuffDamagedAck` `+0x00`;
  - `ApplyActionRestrictionAck` `+0x00`;
  - `NotifyHandleDeadAck` `+0x00`;
  - `ApplyBuffAck` `+0x00`;
  - `CreateProjectileAck` `+0x3D`;
  - `CatchActionResultAck` `+0x04`.

---

## 2. Qué recibía el jugador B cuando A golpeaba a un monstruo

### 2.1 Escritores del servidor y destinatarios

Script `senders.py` (salida en `senders_out.txt`) y desensamblado de los llamadores en `.xtls`.

| Ack | Escritor (servidor) | Llamador | Destinatarios | Handler del cliente (vf2) |
|---|---|---|---|---|
| AttackTargetAck 2922 | `0x2965BF0` | `0x28B0F30` | **ToNear** | `0xB59280` |
| AttackerAck 2509 | `0x29657D0` | `0x28B0150` | **ToNear** | `0xB58F50` |
| AttackTarget_SkillAck 2846 | `0x2966010` | `0x28B21D0` | ToNear | `0xB595E0` |
| VaryHPAck 2147 | `0x2991440` | `.xtls 0x10431D40` | **ToNear** | `0xB75020` |
| NotifyHandleDeadAck 2530 | `0x29927E0` | `.xtls 0x10433BD0` | **ToNear** | `0xB764A0` |
| NotifyHandleFatalAck 2899 | `0x2992D10` | `0x29444F0` | ToNear | `0xB76900` |
| OnPostBuffDamagedAck 2681 | `0x2989490` | `.xtls 0x1043CEB0` | ToNear | `0xB70890` |
| ApplyActionRestrictionAck 2162 | `0x2967810` | `.xtls 0x10502340` | ToNear | `0xB5AEE0` |
| VarySharpnessAck 2532 | `0x2A02040` | `0x2AE03B0` | ToNear | `0xBD2D00` |
| ApplyBuffAck / UnapplyBuffAck | `0x2A622B0` / `0x2A626D0` | varios | ToNear | `0xC13D70` / `0xC14140` |
| CreateProjectileAck 2250 | `0x2A389A0` | `.xtls 0x1074D9E0` | ToNear | `0xBF3D50` |
| OnHitEventSuccessAck 2650 | `0x296C590` | `0x2A7C780` | ToNear | `0xB5F430` |
| AttackGameEventAck 2553 / AiEventAck 2218 | `0x2A571B0` / `0x297C710` | `.xtls` | ToTarget | |
| DeadActorHideAck 2229 | `0x29B36A0` | `0x2C37340` | ToWorld | `0xB9E770` |
| **ChangeDockingSocketByFrameEventAck 2747** | `0x2969BC0` | `0x27DDE40` (`ServerFrameEventChangeDockingSocket::vf1`) | **ToNearExceptOneSelf** | `0xB5D2E0` |
| **OtherEquipItemAck 2757** / OtherUnequipItemAck 3073 | `0x29ED1F0` / `0x29ED760` | `0x2AD5CC0` / `0x2ADA5A0` (equipar) | **ToNearExceptOneSelf** | `0xBBF8D0` / `0xBBFE90` |
| AddVisualEquipItemAck 2265 | `0x2A01110` | `0x28D8EE0` | ToNear | `0xBD2050` |

Por tanto, cuando A golpeaba a un monstruo M, B (cerca) recibía:
- el resultado: `AttackTargetAck` (atacante = A), `AttackerAck`, `VaryHPAck` de M,
  `OnPostBuffDamagedAck` (número de daño), `ApplyActionRestrictionAck`, buffs y, al final,
  `NotifyHandleDeadAck`;
- **la animación de A** por `StartActionAck`;
- **la reacción de M** como `StartActionAck`/`MoveActorAck`/`ChangeAIStateAck` relevados desde
  el cliente que controlaba a M.

El MMO era "autoridad del controlador": cada NPC tiene un dueño (`actor+0x90`, la clave de
usuario del cliente que corre su IA). Que en un jugador el dueño es el usuario local lo
demuestra que el servidor acepta `MoveActorReq` de 3.393 NPCs, y ese handler exige dueño.
Existe además `OverriedOwnershipStateTypeyAck` (2168).

### 2.2 Lo que hace el cliente con un golpe

- **Objetivo controlado por este cliente (los enemigos en un jugador):** al recibir
  `AttackTargetAck` (+ `ApplyActionRestrictionAck`), el cliente lanza en ~20 ms la reacción
  del enemigo y la comunica:
  `EchoActionSessionIDReq` → `StartAIFunctionReq` → `ChangeAIStateReq` →
  `CurrentTargetChangeReq` → `StartActionReq`.
- **Objetivo remoto (lo que veía B):** recibía la misma `AttackTargetAck` (efectos de impacto,
  **(hipótesis)** sangre o chispas y número de daño) y la reacción llegaba aparte como relevo.

---

## 3. Etapa A: que los ataques del clon se vean bien

`StartAction` ya cubre las animaciones de ataque, bloqueo, esquiva, golpe recibido y
derribo: todas son acciones del controlador. Lo que el MMO enviaba **aparte** a los demás:

| # | Qué falta | Mensaje para el receptor | Cómo obtenerlo en el emisor | Receta |
|---|---|---|---|---|
| A1 | **Arma a la mano o a la espalda** al sacar o guardar, cambio de arma en combate | `ChangeDockingSocketByFrameEventAck` 2747 = `u32 clave` + FrameEventAccessor (12 B) | El servidor del emisor ejecuta `ServerFrameEventChangeDockingSocket::vf1` `0x27DDE40` y llama al escritor `0x2969BC0(ServerActor* rcx, finder rdx, FrameEventAccessor* r8)`. **Con una sola sesión el escritor sale antes de serializar** (lista de destinatarios vacía, `0x2969C20`), así que hay que **engancharlo en la entrada** y leer la clave del actor (`ServerActor+0x60`, **(hipótesis)** como en el cliente) y el accessor en memoria (`+0x08` tipo, `+0x10` slot, `+0x14` hash, `+0x18` u16, `+0x1C` índice) | `ack = clave_clon ‖ u8 tipo ‖ u8 slot ‖ u32 hash ‖ u16 id ‖ u32 índice`. Cabecera `u16 2747 \| u8 0xFF \| u16 16`. El handler `0xB5D2E0` resuelve el frame event con `0x1F78A20` sobre la acción **que el clon está ejecutando** y llama a Docking (`[actor+0x68]+0x170`, `0x97A320`). Enviarlo después del `StartActionAck` de esa acción |
| A1b | Ganchos, linternas y otros objetos acoplados | `AttachDockingChildActorAck` 2293 / `DetachFromDockingParentActorAck` 2887 | Frame events 50/26 (mismo patrón) | por estudiar |
| A2 | Efectos de buffs (brillo elemental, ardiendo, etc.) **(hipótesis)** | `ApplyBuffAck`/`UnapplyBuffAck` (77 B) | El emisor los **recibe** (S→C, `ToNear`) con su clave en `+0x00`, `+0x18` y `+0x1C` | Copiar el payload cambiando su clave por la del clon en esos tres offsets; descartar los que tengan otro portador. Hasta 22/s en combate: filtrar por id de buff si molesta |
| A3 | Flechas del clon | `CreateProjectileAck`/`RemoveProjectileAck` | S→C del emisor (dueño = jugador o caballo) | Cambiar dueño y jinete (`+0x00`, `+0x1D`, `+0x35`, `+0x39`) por el clon o su caballo; objetivo `+0x3D` → 0. **(hipótesis)** el id de proyectil puede chocar con los locales |
| A4 | Cambio de equipo | `OtherEquipItemAck` 2757 | Escritor `0x29ED1F0` (probablemente la misma salida temprana) | Formato pendiente (lector `0xBBF8D0`: u8, u32 clave, u32, u32, u16 ranura + struct del objeto). **Alternativa ya disponible:** reaparecer el clon con la plantilla nueva de `ActorLoadDataAck` tras `EquipItemAck` |
| A5 | Filo, battle state, aggro | — | No hacen falta: `VarySharpness` es UI propia y `ChangeBattleState` no se reenvía | — |

No se puede replicar en la etapa A el impacto sobre el objetivo (sangre o reacción): el
objetivo no existe en el otro mundo.

---

## 4. Etapa B: enemigos compartidos

### 4.1 Restricciones

- Cada PC tiene su servidor, su mundo y sus claves (`0xB01xxxxx` distintas). El servidor
  reutiliza claves al recargar.
- El clon es **solo de cliente**: no hay `ServerActor` para él.
- Los Ack de resultado llevan la secuencia de HP (`VaryHPAck +0x40`) y los ids de ataque del
  servidor que los generó.
- Inyectar en el cliente un `VaryHPAck` o `NotifyHandleDeadAck` sin que el servidor lo sepa
  **desincroniza**: el siguiente `VaryHPAck` real (regeneración) lo pisa y el enemigo sigue
  vivo para el servidor.

### 4.2 ¿Podemos inyectar Req C→S en el servidor local? Sí

`ClientNet::PostPacket(ClientNet* [exe+0x6D69170], int* res, u32 sesión=4, const u8* pkt,
u16 tamaño, u8 canal, u16 flags=0x10)` (`0x35C670`):
- copia el paquete a un búfer propio de `0x8030` y lo encola en `ClientNet+0xE0`;
- después el NetworkThread lo envía por `NwVirtualSyncSession::Send` `0x14380B0` →
  `ServerNet::OnRecvPacket` `0x2773C20`.

Ya tenemos un mid-hook en su entrada (`troc_sniffer.cpp`). Se puede inyectar igual que en
`on_recv_packet`: llamar a la función original desde el propio hook, en el hilo del juego que
envía, con `t_injecting` para no entrar en bucle.

Requisitos:
- `slot` = 0 (todas las Req capturadas);
- el **canal** debe coincidir con `Troc+0x14` de ese opcode. Antes de inyectar, registrar
  `[rsp+0x30]` (canal) y `[rsp+0x38]` (flags) de las llamadas reales;
- cifrado apagado; sin número de secuencia.

Lo que filtra es cada handler (dueño y acción en curso).

### 4.3 Opciones

| Opción | Qué hace | Qué tenemos | Viabilidad | Riesgos |
|---|---|---|---|---|
| **(a) Autoridad del host** | Solo el mundo del host simula enemigos. El invitado recibe sus `ActorLoadDataAck` (claves traducidas a un rango libre y **dueño = usuario ajeno**, p. ej. `0x9010F001`, para que el cliente los trate como remotos: control `+0xC8 = 0`) y sus Move/StartAction/Rotate/ChangeAIState como Ack (recetas de `mirror-ack-formats.md`), más los resultados (`VaryHP`, `AttackTarget`, `Dead`, `RemoveActor`) con claves traducidas. Los enemigos propios del invitado se suprimen | Todo el lado de recepción: sniffer bidireccional, inyección S→C, recetas. **Supresión (hipótesis):** las spawns las pide el cliente (`ActivateSpawningPoolReq` 2424, `AutoSpawnRoadReq` 2550, `MeshGroupAutoSpawnReq` 2809); filtrarlas en el invitado podría evitar que su servidor cree enemigos | Media-alta para **ver** los mismos enemigos. Baja para el daño del invitado (ver c) | Volumen alto (los `ActorLoadDataAck` pesan 6-19 KB). Misiones y botín del invitado no avanzan (los enemigos no existen en su servidor). **No se sabe si la IA del host ataca al clon** (T6). Los golpes de un enemigo del host al clon llegan al servidor del host con un objetivo desconocido y se ignoran: el daño al invitado hay que aplicarlo en su PC (`VaryStatByAccInputReq` sobre su propio jugador) |
| **(b) Mundos propios + daño espejo** | Cada PC simula sus enemigos. Por cada golpe del emisor se envía `{datos del personaje objetivo, posición, daño (OnPostBuffDamaged +0x0C o Δ de VaryHP), hash, muerte}` y el receptor lo aplica a **su** enemigo equivalente (mismo tipo, el más cercano a menos de ~5 m) | Daño ya calculado por el servidor del emisor. Las posiciones de los enemigos locales salen de sus `MoveActorReq` (las envía nuestro cliente) | **Alta y barata**, si `VaryStatByAccInputReq` aplica HP y muerte (T2) | Los enemigos divergen (cada IA persigue a su jugador): el clon "golpea al aire". Emparejamiento erróneo con enemigos amontonados. Doble botín y doble experiencia (cada mundo da lo suyo; aceptable) |
| **(c1) Atacante suplente** | En el receptor: `StartActionReq` del **jugador local** con la acción del clon (y MoveData = su posición real) + `ProcessAfterBeginFrameEventReq` + `DoFrameEventReq` con el accessor del clon y objetivos traducidos (ordenados, con su MoveData local); luego reinyectar el último `StartActionReq` real | PostPacket. Formatos completos | Media. Pasa las validaciones solo si el u16 del accessor coincide con el que guarda el servidor **(hipótesis: lo copia de la Req)** | Efectos secundarios en el jugador local: costes de estamina y buffs al empezar la acción (`ApplyBuff`/`VaryDataDefinedStaticStat` tras cada StartActionReq), su posición en el servidor, `ChangeActionSessionIDAck`. El daño usa las stats del jugador local. Frágil |
| **(c2) `VaryStatByAccInputReq` de HP** | `clave del enemigo local, 0x000F4240, −daño×1000` | PostPacket | **Alta mecánicamente** (no hay comprobaciones en el handler) | **(hipótesis)** puede no disparar muerte, botín, `AttackTargetAck` ni reacción (es un cambio de stat, no un ataque). Combinarlo con (c4) para lo visual. Probar el clamp en 0 |
| **(c3) `AttackByCollisionReq`** | `víctima = enemigo local, atacante = jugador local, pos, dir, hash` | PostPacket | Media | **(hipótesis)** el hash define el daño (¿se acepta el `+0x24` del `AttackerAck` del emisor?). Puede producir `AttackTargetAck`, reacción y muerte de forma nativa |
| **(c4) Cosmético en el cliente** | Inyectar en el receptor un `AttackTargetAck`/`ApplyActionRestrictionAck` (objetivo = enemigo local, atacante = clon) | Inyección S→C | Alta | Provoca la reacción del enemigo (su cliente la lanza) sin daño real. `EchoActionSessionIDReq` hacia un servidor que no cambió la sesión **(hipótesis: inofensivo)** |
### 4.4 Recomendación

- **Etapa B1 (rápida): opción (b) con (c2) + (c4).** El daño es el que calculó el servidor del
  atacante, aplicado al enemigo equivalente; la reacción se fuerza en local. Útil para luchar
  juntos contra el mismo grupo, sabiendo que las posiciones de los enemigos divergen.
- **Etapa B2: opción (a) para lo visual + (c2) para el daño en ambos sentidos.**
  - Los enemigos del host aparecen en el invitado como NPC de otro dueño.
  - El daño del invitado se aplica en el host con (c2). Ningún servidor puede calcularlo,
    porque el del invitado no conoce al enemigo y el del host no conoce al invitado. Hay que
    estimarlo: con una tabla `hash de ataque → daño` aprendida de los golpes del host, o
    con (c3) pasando el hash, si T3 funciona.

---

## 5. Plan priorizado (los pasos más pequeños primero)

1. **P0, instrumentación (solo lectura):**
   - el sniffer guarda canal y flags de PostPacket;
   - `analyze_capture` decodifica DoFrameEventReq (accessor, variante, objetivos) y
     AttackTarget/VaryHP/Dead;
   - mid-hook de solo log en la entrada de `0x2969BC0` (escritor de
     ChangeDockingSocketByFrameEventAck) y de `0x29ED1F0` (OtherEquipItemAck), para ver cuándo
     se llaman al sacar o guardar el arma y al equipar. Comprobar la firma de entrada con
     `mksig.py`.
2. **A1, arma en mano** (2747): emitir `{accessor}` desde el hook anterior → evento de red nuevo
   (subir `kVersion`) → `u32 clon + accessor` en el receptor, con el retardo del espejo.
   - **Verificación:** sacar y guardar la espada; el arma del clon cambia de sitio.
3. **A2, buffs:** reenviar los `ApplyBuff`/`UnapplyBuffAck` cuyo portador es el jugador,
   reescritos para el clon.
4. **A3, flechas:** `CreateProjectileAck`/`RemoveProjectileAck` del jugador y su caballo.
5. **A4, equipo:** reaparecer el clon cuando llegue `EquipItemAck`/`UnequipItemAck` (o decodificar
   `OtherEquipItemAck`).
6. **B0, pruebas de inyección C→S** (§6: T1-T5), cada una sobre una copia de la partida en
   `backups/`.
7. **B1, daño espejo:**
   - el emisor manda por golpe `{clave del personaje del objetivo (de su ActorLoadData),
     pos (hit entry +0x004), daño (OnPostBuffDamaged +0x0C), hash (AttackTarget +0x24),
     muerto}`;
   - el receptor empareja y aplica (c2) y (c4).
8. **B2, enemigos del host en el invitado:** relevo de `ActorLoadDataAck`, Move, StartAction,
   Rotate, ChangeAIState y resultados con traducción de claves y dueño ajeno; supresión de
   spawns del invitado (T7).

## 6. Pruebas en vivo que deciden las hipótesis

Todas con `cdmp_peer` por loopback, salvo que se indique otra cosa. Antes de cualquier
escritura, copia de la partida.

| # | Prueba | Decide |
|---|---|---|
| T0 | Con el espejo activo, sacar y guardar el arma y mirar el clon (captura de pantalla) | Si de verdad falta A1 |
| T1 | Inyectar por PostPacket una Req inocua (p. ej. reenviar un `CurrentTargetChangeReq` real del jugador) y comprobar con el sniffer que no hay Nak (1013) | Que la vía C→S funciona y el canal correcto |
| T2 | `VaryStatByAccInputReq(enemigo, 0x000F4240, −5000)` sobre un enemigo cercano. ¿Llega `VaryHPAck` (con +0x40 incrementado)? Repetir hasta 0: ¿`NotifyHandleDeadAck`, botín, `RemoveActorAck`? | Viabilidad de (c2) |
| T3 | `AttackByCollisionReq(enemigo, 0xA0100001, 0, pos, dir, 0xF2EE2678, 0)`, y luego con el hash de un `AttackTargetAck` real | Viabilidad de (c3) |
| T4 | Inyectar en el cliente un `AttackTargetAck` capturado, reescrito con objetivo = enemigo local y atacante = clon | Si (c4) da reacción visual y si el `EchoActionSessionIDReq` posterior provoca Nak |
| T5 | Antes de (c1): hookear (solo lectura) la entrada de `0x2A896E0` y loguear el u16 `[x+0x38]+0x5E` frente al u16 del accessor y de la StartActionReq | De dónde sale el id del accessor |
| T6 | Llevar el clon (con `mirror_offset_m` grande) cerca de enemigos sin el jugador: ¿lo atacan? ¿Salen `CurrentTargetChangeReq`/`DoFrameEventReq` con objetivo `0xA010F001`? | Si los enemigos del host pueden luchar contra el invitado |
| T7 | Descartar en el cliente `ActivateSpawningPoolReq`/`AutoSpawnRoadReq` en una zona con enemigos | Si se pueden suprimir los enemigos del invitado |
| T8 | Inyectar `ApplyBuffAck` reescrito para el clon (por ejemplo, el de un ataque elemental) | Si A2 produce efectos visibles |

## 7. Direcciones nuevas (build 2976)

| RVA | Qué es |
|---|---|
| `0x296D130` | `DoFrameEventReq::vf2` (servidor): deserializa, valida y despacha |
| `0x2A896E0` | ejecución del frame event (tipo 0): valida la acción en curso del atacante |
| `0x2BE0D90` | ejecución del frame event (tipo 1, componente `+0x88`) |
| `0x28F0210` | fábrica `ServerFrameEvent*` por tipo (switch: tablas `0x28F22A0`/`0x28F21C0`) |
| `0x27D8550` | `ServerFrameEventAttack::vf1` |
| `0x27DDE40` | `ServerFrameEventChangeDockingSocket::vf1` (aplica en el servidor y difunde) |
| `0x2969BC0` | escritor de `ChangeDockingSocketByFrameEventAck` (sale si no hay destinatarios) |
| `0xB5D2E0` | handler del cliente de `ChangeDockingSocketByFrameEventAck` |
| `0x1F78430` | deserializador de una entrada de objetivo (665 B en el cable) |
| `0x1F79A30` | deserializador de la variante del frame event (u16 etiqueta 0-6) |
| `0x1F78F20` / `0x1F78DE0` | (de)serializador de FrameEventAccessor (**virtualizado**, salta a `.xtls`) |
| `0x1F78A20` | cliente: resuelve el frame event desde el accessor y el actor |
| `0x2972720` | `VaryStatByAccInputReq::vf2` (sin comprobación de dueño); `0x17AE4B0` cambia el stat |
| `0x2A3D430` → `0x2C3D7D0` → `0x2BDA2F0` | `AttackByCollisionReq` |
| `0x29814A0` | `ChangeBattleStateReq::vf2` (`comp+0x164`, `0x22218F0`; sin relevo) |
| `0x2965BF0` / `0x29657D0` / `0x2966010` | escritores de AttackTarget / Attacker / AttackTarget_Skill |
| `0x2991440` / `0x29927E0` / `0x2989490` / `0x2967810` | escritores de VaryHP / NotifyHandleDead / OnPostBuffDamaged / ApplyActionRestriction |
| `0x29ED1F0` / `0x29ED760` | escritores de OtherEquipItemAck / OtherUnequipItemAck (`ToNearExceptOneSelf`) |
| `0xB59280`, `0xB58F50`, `0xB75020`, `0xB764A0`, `0xB70890`, `0xB5AEE0`, `0xC13D70`, `0xC14140`, `0xBBF8D0` | handlers del cliente (§2.1) |
| `[exe+0x6D69698]` / `[exe+0x6D696A0]+0x48` | gestores de actores del servidor usados para buscar por clave (`0x2A82370`, `0x2A827B0`) |
| `[exe+0x6D69AE8]` | tabla de definiciones de stats (id → índice) |

## 8. Scripts (`build/research/combat/`)

| Script | Qué hace |
|---|---|
| `combat_cat.py <bin> cat\|timeline a b\|dump op` | catálogo de mensajes de combate con histograma de claves por offset; línea de tiempo |
| `dumpat.py <bin> <op> t0 t1 [clave]` | volcado hex de un opcode en una ventana |
| `player_tl.py <bin> t0 t1 [clave]` | línea de tiempo de un actor (acción y objetivo de StartAction, accessor de DoFrameEvent) |
| `fetypes.py <bin> [clave]` | DoFrameEventReq agrupados por variante, con objetivos y respuestas |
| `pdfe.py`, `peak.py`, `hpseq.py` | resumen de DoFrameEvent y docking; picos por segundo; secuencias de HP y muerte |
| `senders.py <rva>…` | escritor de Ack → llamadores → `To*SendTargetFinder` |
| `ackwriters.py build\|reach` | mapa función → opcodes de Ack escritos (`ackwriters.pkl`); qué Acks alcanza un handler |
| `sfe.py`, `fetype_map.py` | clases `ServerFrameEvent*` y la tabla tipo → clase de la fábrica |
| `*.asm`, `*.txt` | volcados citados en este informe |

## Anexo: FrameEventAccessor en memoria

Build 1.0.0.2976. El layout no sale de `0x1F78F20`/`0x1F78DE0` (virtualizadas), sino de tres
funciones no virtualizadas que lo construyen o lo leen.

**1. Constructor en la pila (`DoFrameEventReq::vf2` `0x296D130`).** Objeto en `rbp+0x10`:

```
0x296D262  lea rax, [vtable FrameEventAccessor 0x557C480]
0x296D269  mov [rbp+0x10], rax              ; +0x00 vtable
0x296D26D  mov byte ptr [rbp+0x18], 3       ; +0x08 u8 tipo (3 = inválido)
0x296D275  vmovups ymmword ptr [rbp+0x20], ymm0   ; +0x10..+0x2F a cero
```

Usos del tipo:
- `0x296D7CF cmp byte ptr [rbp+0x18], 3`;
- `0x296D937`/`0x296DA96 movzx eax, byte ptr [rbp+0x18]`, y luego `0x296DAF7 cmp al, 1`.

Se pasa como `r8` a `0x2A896E0` (`0x296DADB lea r8, [rbp+0x10]`).

**2. Lector del servidor (`0x2A896E0`, r8 = accessor):**

| Campo | Lectura | Uso |
|---|---|---|
| slot | `0x2A89710 movzx esi, byte ptr [r8+0x10]` | índice: `0x2A89796 mov rdi, [r13+rsi*8+0x88]` |
| hash | `0x2A8971A mov r12d, [r8+0x14]` | `0x2A8986C cmp [rax+0x18], r12d` |
| id | `0x2A8971E movzx r15d, word ptr [r8+0x18]` | `0x2A8987A cmp [rax+0x5E], r15w` |
| índice | `0x2A89723 mov ebx, [r8+0x1C]` | `0x2A8989E cmp eax, ebx` / `0x2A898CE cmp ebx, -1` |

**3. Resolvedor del cliente (`0x1F78A20`, rcx = accessor, desde `0xB5D2E0`):**
- `0x1F78A40 movzx eax, byte ptr [rcx+8]` y, si vale 0, la rama de acción;
- `0x1F78A8D movzx edx, byte ptr [rsi+0x10]` y `0x1F78A89 mov r8d, [rsi+0x14]` →
  `0x232F6D0` (busca la acción);
- `0x1F78ABE movzx edx, word ptr [rsi+0x18]`;
- `0x1F78B58 mov r8d, [rsi+0x1C]`.

**Ojo:** otras ramas (tipo ≠ 0) leen los mismos bytes con otros anchos (`dword [+0x10]`,
`byte [+0x14]`, `dword [+0x18]`, `byte [+0x1C]`). Es una unión según el tipo, y el layout de
arriba vale solo para el tipo 0. Además, los anchos son de lectura: el relleno exacto no está
probado. Encaja con el cable de 12 B: `u8 tipo, u8 slot, u32 hash, u16 id, u32 índice`.

**Cadena hasta el escritor y clave del actor:**
- `0x2A89E61 mov r9, [rbp+0x460]`: el puntero al accessor, que entró en `r8` y se guarda en
  `rsp+0x18`;
- `0x2A89E76 call [rax+0x20]` sobre el sistema `[[exe+0x6D694D8 o 0x6D694E0]+0x48]`, con el
  contexto `[rbp+0x90]` (`+0` actor). Esta llamada es virtual: **el salto hasta el vf1 no está
  verificado**;
- en `0x27DDE40`: `0x27DDE52 mov rbp, r9`, `0x27DDF42 mov r8, rbp`,
  `0x27DDF4A mov rcx, [rsi]` (con `rsi = [rsp+0x80]`, el 5º argumento: el contexto),
  `0x27DDF4D call 0x2969BC0`. Es el **único** llamador de `0x2969BC0`;
- en el escritor:
  - `0x2969BE2 mov rsi, rcx`;
  - `0x2969D98 mov eax, [rsi+0x60]`, que se serializa como el primer u32 del Ack
    (`0x2969DC4`), y el cliente lo trata como clave de actor (`0xB5D3C0` → `0x8A67E0`). **Por
    tanto, `+0x60` es la clave del actor**;
  - `0x2969BDC mov r12, r8` → `0x2969DD5 call 0x1F78DE0` (serializa el accessor).
- El mid-hook en la entrada de `0x2969BC0` salta **antes** de la salida temprana por lista
  vacía (`0x2969C20`).

**Lo que no está probado:** que el cliente envíe un `DoFrameEventReq` para cada vez que se
saca o guarda el arma en un jugador. Es probable, porque el servidor también aplica el socket
(`0x27DDED5 mov [rbx+0x1C], eax`, con `rbx = [[actor+0x68]+0x170]`, `0x27DDEAA`), pero hay que
comprobarlo en vivo (T0b): log-only en `0x2969BC0` mientras se saca y guarda la espada.

# Interacciones con el mundo en cooperativo: árbol con cuerda y gallina agarrada

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Las direcciones son RVA (`exe+…`) y
valen solo para esta build. Análisis estático de solo lectura más la captura
`captures/cdmp_packets_20261003-200328.bin` (31 min, un PC, marioneta en eco con `cdmp_peer`
relay) y su log `cdmp.log` (copiado en `build/research/interactions/`). El juego no se ejecutó.

Hora de pared = **20:03:42,71 + t** (calibrado con los `ActorLoadDataAck` inyectados de la
marioneta: t 1276,573 ↔ 20:24:59,285; t 1462,295 ↔ 20:28:05,007; t 1730,534 ↔ 20:32:33,246).

Lo marcado **(hipótesis)** no está comprobado. Scripts y volcados en
`build/research/interactions/` (§7).

---

## 0. Resumen

1. **El "árbol" es un objeto de escena del nivel convertido en gimmick bajo demanda.** Tiene un
   id de nivel de 16 bytes (`{u64 0xC58D002EB21193B9, u64 1}` y `{0xC58D002EB2117D4A, 1}` para los
   dos árboles usados), estable entre recargas. Cuando el jugador se acerca, el cliente pide
   `SceneLevelOnDemandGimmickSpawnReq` y el servidor carga un actor "Game Mechanic" (registro 7,
   claves `0xB01010E1` y `0xB01010BB`). Esa clave **no aparece en ningún mensaje** durante el tirón
   ni el lanzamiento.
2. **El enganche viaja como "remote catch"** (frame event `ServerFrameEventRemoteCatch`). El
   cliente manda `DoFrameEventReq` (636 B, variante 6) con el id de escena del árbol y el punto
   local del gancho (≈7,5–8 m de altura en el tronco). El servidor responde
   **`RemoteCatchResultAck` (2763) por `ToNear`**: en el MMO, eso es lo que recibían los demás.
   El doblado, el muelle y el lanzamiento no tienen mensaje propio. Son física del cliente, con
   las acciones del jugador. El vuelo va en los `MoveActorReq` (47 m/s, +11 m) y no hay mensaje de
   impulso.
3. **La "gallina" es una perdiz** (`Partridge`, registro 6, variante `0x000F454C`, clave
   `0xB01012F7`). El agarre es un frame event de ataque de tipo *catch*. El servidor responde
   **`CatchActionResultAck` (2156, `ToNear`)**: `+0` quien agarra, `+4` el agarrado, socket
   `"Hand_Socket"` y hueso `"B_EnemyCatch_00"`. Al soltarla responde
   **`ReleaseCatchResultAck` (2512, `ToNear`)**, con el transform y la velocidad de la suelta.
   Mientras va en la mano, el cliente manda `StartActionReq` de la perdiz con **padre del Path =
   jugador** y no envía `MoveActorReq`.
4. **Los tres Ack son `ToNear`**, y ese relevo sigue vivo en esta build: el propio emisor los
   recibe, así que los bytes son los que recibía cualquier jugador cercano. **Ninguno está en
   `kRelayedAcks`**, y por eso el otro jugador no ve nada. Los relevos que sí están muertos
   (`ExceptOneControl`/`ExceptOneSelf`) aquí solo afectan a las acciones de la perdiz y a
   `BreakSceneObjectAck`.
5. **Recetas (§4)**, por valor y esfuerzo:
   - **R1, árbol:** reenviar `RemoteCatchResultAck` cambiando `+0` por la clave de la marioneta.
     El árbol se identifica solo, porque el id de escena es del nivel.
   - **R2, perdiz:** reenviar `CatchActionResultAck`/`ReleaseCatchResultAck` traduciendo `+4` a
     la perdiz equivalente del receptor (variante y punto de aparición), y filtrar en el receptor
     las Req con padre = marioneta.
   - **R3:** arreglar ya el objetivo `+0x16D` de los `StartActionAck` de la marioneta, que hoy
     lleva claves del mundo del emisor.

---

## 1. Segmentación de la captura

Claves: P = jugador `0xA0100001`. Herramienta (gimmick del jugador, "Game Mechanic" cargado al
entrar, `t` 1276,34) = `0xB01000AB`. SA = `StartActionReq` del jugador (hash A[0] `+0x04` / A[1]
`+0x49`). GEFE = `GimmickExecuteFrameEventReq`. Se ignora el tráfico de la marioneta (`…F001`).

Hay dos recargas (`t` 1446–1457 y 1783–1793). El periodo útil va de 1457 a 1783.

### 1.1 Árbol (dos intentos; el segundo lanza al jugador)

**Preparación.** Los árboles se convierten en gimmick al acercarse:

| t | Dir | Mensaje (op, tamaño) | Campos |
|---|---|---|---|
| 1619,541 / 1620,765 | C>S | SceneLevelOnDemandGimmickSpawnReq (2928, 25) | `04 \| u32 1 \| u32 0x7546F840 \| id escena 16 B` |
| 1619,548 / 1620,770 | S>C | SyncSceneObjectAck (2665, 615, ToField) + **ActorLoadDataAck** (6163, reg. 7 "Game Mechanic", dueño `0x90100000`) + UpdateOnDemandGimmickAck (2145, 22, **ToNear**: `04 \| P \| u8 1 \| id escena`) | árbol 2 `0xB01010BB` en (−10808,63, 577,51, −4116,91), escala 0,872; árbol 1 `0xB01010E1` en (−10804,64, 574,63, −4137,04) |
| 1619,88 / 1621,06 | C>S | RegionInOutReq ×6, GimmickBranchStateReq, GEFE ×8, UpdateGimmickReq | inicialización por parte del cliente |
| varios | C>S | GamePlayTriggerInOutReq (3068, 161) | entrar o salir del volumen del árbol: `+0x00` u64 id del trigger (el mismo que hay en la carga del árbol), `+0x20` transform del árbol, `+0x48` clave del árbol, `+0x70` transform del jugador, `+0x98` P |

**Intento 1** (árbol `b21193b9` / `0xB01010E1`; jugador en (−10811, 576,5, −4127)):

| t | Dir | Mensaje | Detalle |
|---|---|---|---|
| 1658,155 | C>S | SA `26107d3c` + GEFE/UpdateGimmickReq de la herramienta (`003ecc59`/`6ba22087`) | sacar la herramienta (hipótesis) |
| 1659,666 | C>S | SA `3975eabf` | lanzar el gancho |
| 1660,446 | C>S | SA `fa732c46` + 6× DoFrameEventReq (517) | |
| **1660,549** | C>S | **DoFrameEventReq (636)** | accessor `{tipo 0, slot 0, 9a368460, id 0, fe 20}`, **variante 6**: `00 \| 02 \| u32 0 \| id escena b21193b9… \| u64 0x8000000002001D60 \| vec3 local (−0,23, 7,53, −0,10) \| quat (−0,007, −0,500, −0,004, 0,866)` |
| **1660,564** | S>C | **RemoteCatchResultAck (97)** | ver §2.1 |
| 1660,577 / 1660,840 | C>S | IsRemoteCatchEffectStartStateReq (2108, 2 B) `00 01` / `00 00` | |
| 1660,599–1660,866 | C>S | UpdateGimmickReq/GEFE de la herramienta + SA `9a368460`, `2821f20a` | tirón |
| 1661,165–1664,156 | C>S | SA `a747bf2f`, `7cc69d61` ×2, `d9c333c4`, `76d8c2d8`, `6b4ee3ca` + GEFE de la herramienta | |
| 1665,6–1678,3 | C>S | SA **`563271fe` cada 1,5 s** | sujetar y tirar: el jugador retrocede a ~1 m/s de (−10811, −4122) a (−10817, −4111) |
| **1675,156** | C>S | SceneLevelOnDemandGimmickUnspawnReq `0xB01010E1` → UpdateFieldGimmickSaveDataKeyAck + RemoveActorAck | **el gimmick del árbol se descarga mientras el jugador sigue tirando** |
| 1678,266 | C>S | GEFE de la herramienta (`92786a19`/`76b28076`) | soltar |
| 1678,402 | C>S | DoFrameEventReq (521) accessor `1f4ef471` fe 5, variante 3 (4 B a cero) | |
| 1678,559 / 1678,590 | C>S | SA `1f4ef471`, `be0b7eed` | **no hay lanzamiento**: el árbol ya no estaba cargado |

**Intento 2** (árbol `b2117d4a` / `0xB01010BB`). El lanzamiento ocurre a las ≈20:32:03:

| t | Dir | Mensaje | Detalle |
|---|---|---|---|
| 1686,694–1688,6 | C>S | SA `3975eabf`, `fa732c46`, `9a368460`, `2821f20a` + IsRemoteCatch `00 01` | primer lanzamiento fallido (sin Ack) |
| 1689,574 | C>S | SA `3975eabf` | |
| **1689,679** | C>S | DoFrameEventReq (636), variante 6 | id `b2117d4a…`, u64 `0x8000000002001D68`, vec3 local (0,29, 8,12, −0,17) |
| **1689,690** | S>C | **RemoteCatchResultAck (97)** | |
| 1689,707 / 1690,036 | C>S | IsRemoteCatchEffectStartStateReq `00 01` / `00 00` | |
| 1689,757–1692,725 | C>S | SA `9a368460`, `2821f20a`, `a747bf2f`, `7cc69d61`, `d9c333c4`, `76d8c2d8`, `6b4ee3ca` + GEFE de la herramienta | igual que en el intento 1 |
| 1694,2–1701,0 | C>S | SA `563271fe` ×6 | tirón: de (−10820, −4105,8) a (−10827,9, −4106,4) |
| **1701,011** | C>S | GEFE de la herramienta `92786a19`/`76b28076` + UpdateGimmickReq | soltar |
| 1701,127 | C>S | DoFrameEventReq `1f4ef471` fe 5, variante 3 | sin vector |
| **1701,155** | C>S | MoveActorReq | **lanzamiento**: de (−10827,8, 577,7) a (−10825,1, 581,8) en 0,2 s (47 m/s), pico y = 589,07 a 1702,5, unos 25 m de recorrido |
| 1701,279 / 1701,529 | C>S | SA `1f4ef471`, `be0b7eed` + DoFrameEventReq `eb340761` (var. 7) y `269521ce` (var. 0) | vuelo |
| 1701,530 | S>C | AttackTargetAck/AttackerAck P→P | autogolpe, propio |
| 1702,289 / 1704,328 | C>S | SA `269521ce`, `eb340761` | planeo o caída (hipótesis) |
| 1709,274 | C>S | SceneLevelOnDemandGimmickUnspawnReq `0xB01010BB` | |

Antes, en 1621,0, el jugador golpeó el árbol 2 con el arma: `AttackResultBackgroundOnlyReq` y
3× `RegisterLoad/UnregisterLoadGimmickFromActorImpulseReq`. Eso activó la reserva de aparición
del árbol (`ActivateSpawningPoolReq` con su id de escena) y cayó un `Rhinoceros Beetle`
(`0xB01010F5`). No tiene relación con la cuerda.

### 1.2 Perdiz (tres agarres, a partir de las ≈20:32:25)

La perdiz `0xB01012F7` la carga en 1705,685 una reserva de aparición (`ActivateSpawningPoolReq`
en 1705,631). Aparece en (−10746,00, 565,31, −4131,05). Desde 1719 huye del jugador con su IA, que
corre en nuestro cliente.

| t | Dir | Mensaje (op, tamaño) | Campos |
|---|---|---|---|
| 1722,679 | C>S | InteractionPreemptionStepReq (2671, 25) | `+0` P, `+4` perdiz, `+8` u32 interacción `0x000F42CD` |
| 1722,695 | S>C | InteractionPreemptionStepSuccessAck (2663, 29) | ToTarget |
| 1722,708 / 1722,711 | C>S / S>C | InteractionDoStepReq (2720, 16) / DoStepSuccessAck (2916, ToTarget) | |
| 1722,710 / 1722,740 | C>S | SA `3e6dbf81`, `26107d3c` con **objetivo `+0x16C` = perdiz** | |
| 1722,888 | C>S | DoFrameEventReq (1307): accessor `4bccf3cb` fe 2, **1 objetivo = perdiz** (entrada de 665 B) | frame event de catch |
| **1722,894** | S>C | **CatchActionResultAck (82)** + AttackTargetAck/AttackerAck (hash `a80bb127`) + SyncChallengeAck | |
| 1722,917 | C>S | StartActionReq **de la perdiz** `6a929941`, **padre `+0x116` = P**, u8 `+0x2D4` = 0 | agarrada |
| 1724,47–1739,6 | C>S | SA `4bccf3cb`, …; perdiz `24d43354`, `8e9cc0d0` (padre P) | **la perdiz no envía ningún MoveActorReq mientras va en la mano** |
| 1739,630 | C>S | SA `bff2eda3` | soltar |
| 1740,274 | C>S | DoFrameEventReq (1307) `feed9fb2` fe 0, objetivo = perdiz | |
| **1740,285** | S>C | **ReleaseCatchResultAck (245)** + AttackTargetAck/AttackerAck (hash `e6d438dd`) | |
| 1740,297 | C>S | perdiz `2e48b0df`, padre `FFFFFFFF`, u8 = 1, + MoveActorReq | libre otra vez |
| 1744,288–1749,848 | | segundo agarre y suelta | interacción `0x000F42CD` |
| 1750,597–1760,756 | | tercer agarre (interacción `0x000F42BE`) y suelta | |

Para la marioneta, el mod inyectó en 1723,10, 1723,12, 1744,74 y 1751,04 `StartActionAck` con
`+0x16D = 0xB01012F7` (§4.3).

### 1.3 Otros (después)

En 1815–1824 el jugador coge un objeto del mundo y lo lanza:
1. `InteractionPreemptionStep`/`DoStep` sobre `0xB0101BE3` (Game Mechanic).
2. `CatchActionResultAck` (84 B, sockets `"Bip01_Weapon_L"` y `"Basic_ChildSocket"`).
3. `GimmickCaughtByFocusActorReq` (28 B: `u32 1 | id escena 16 B | P`).
4. `GimmickCatcherUpdateReq`/`Ack` (ToNear).
5. `CreateProjectileFromFrameEventReq`/`CreateProjectileAck`.
6. `BreakSceneObjectReq` (42 B, mismo id de escena).
7. `RemoveProjectileReq`/`Ack`.

`ChangeDrowningStateReq` (nadar) no tiene que ver.

---

## 2. Qué son el árbol y la perdiz, y qué mensajes los mueven

### 2.1 El árbol

- **Identidad estable:** id de escena de 16 B, `u64 0xC58D002EB2xxxxxx` + `u64 1`. Aparece en
  `SyncSceneObjectAck` (ToField) en 1276, 1489 y 1597, antes y después de la recarga de 1451, con
  los mismos bytes. **(Hipótesis)** es el mismo en otro PC, porque sale de los datos del nivel.
- **Actor temporal:** `ActorLoadDataAck` de registro 7 "Game Mechanic". La carga incluye
  `u32 0x000F4A5D` y el id de escena en `+0x1204`/`+0x120C` (los dos árboles tienen `0xF4A5D`;
  **(hipótesis)** es el tipo de gimmick). `+0x18` vale `0x00011171` en todos los gimmicks, así que
  no sirve como tipo. La posición está en la marca de transform del final de la carga y en
  `GamePlayTriggerInOutReq +0x20`. La clave `0xB010xxxx` cambia en cada carga.
- **`RemoteCatchResultAck` (97 B), lector `0xB5B710`:**

  | Offset | Contenido |
  |---|---|
  | `+0x00` | u32 clave de quien agarra |
  | `+0x04` | 5× u8 (`00 00 01 01 00`) |
  | `+0x09` | u8 clase de objetivo (2 = objeto de escena) **(hipótesis)** |
  | `+0x0A` | u32 clave de actor objetivo (0 aquí) |
  | `+0x0E` | **id de escena (16 B)** |
  | `+0x1E` | u64 id de pieza |
  | `+0x26` | **vec3 punto local del gancho** |
  | `+0x32` | quat |
  | después | 2× u32, u8 y una cadena (u32 longitud ≤ 0x40) |

  El lector:
  - busca a quien agarra (`0x8A67E0`);
  - llama a `[[actor+0x68]+0x1A0]->vf(0x150)(0)`;
  - aplica en el componente `[actor+0x68]+0x78` con `0x592340(comp, …, objetivo, …)`.

  **Actúa sobre el actor que agarra**, que puede ser cualquiera: el mensaje está pensado para
  observadores.
- **Doblado, muelle y lanzamiento:** después del Ack no hay más mensajes del árbol. Ni la clave
  del árbol ni su id de escena aparecen entre 1660 y 1701. Solo hay:
  - acciones del jugador (`563271fe` en bucle mientras tira);
  - `UpdateGimmickReq`/GEFE de **su herramienta** `0xB01000AB`, que no se reenvían a nadie (los
    handlers `0x29DE610` → `0x2AC0790` y `0x29E0970` → `0x2AC1270` no llegan a ningún escritor de
    Ack);
  - `MoveActorReq`.
- **`ReleaseRemoteCatchResultAck` (2471, ToNear) nunca aparece**, ni al soltar ni al lanzar. Por
  tanto, en el MMO los observadores tampoco lo recibían: la suelta salía de reproducir las
  acciones.
- **Lanzamiento:** solo `MoveActorReq`. El frame event de la suelta (`1f4ef471`, variante 3) no
  lleva vector. **No hay mensaje de impulso.**

### 2.2 La perdiz

- `ActorLoadDataAck` de **registro 6** (no 3), variante `+0x18 = 0x000F454C`, nombre
  `"Partridge"`. El punto de aparición está en la marca de transform del final (como en los NPC),
  y coincide con su primera posición.
- **Agarre (`CatchActionResultAck`, 82 B), lector `0xB582B0`:**

  | Offset | Contenido |
  |---|---|
  | `+0x00` | quien agarra |
  | `+0x04` | **agarrado** |
  | `+0x08` | u8 |
  | `+0x09` | cadena `"Hand_Socket"` (u32 longitud 12, incluye el NUL) |
  | `+0x19` | cadena `"B_EnemyCatch_00"` (u32 longitud 15) |
  | después | u8, u64 (`+0x3A`, varía), u8 final (varía) |

  El lector:
  - busca a quien agarra;
  - llama a `0x935DD0(comp = [actor+0x68]+0x70, …)`;
  - `0x935DD0` busca al agarrado por clave (`0x8AE940`, `0x935FA2`) y, si no existe, sale con el
    error `0x6CF705C` (`0x937090`). **(Hipótesis)** sin crash.
- **En la mano:** `StartActionReq` de la perdiz (la envía nuestro cliente, que la controla) con
  padre `+0x116` = quien la agarra y u8 = 0, y ningún `MoveActorReq`. Va pegada al socket.
- **Suelta (`ReleaseCatchResultAck`, 245 B):**
  - `+0x00` quien agarra, `+0x04` agarrado;
  - `+0x08` transform (pos, quat, escala) del punto de suelta, y `+0x50` el mismo;
  - `+0x78` vec3 (≈ velocidad de lanzamiento, (−0,26, 0, 1,47)) **(hipótesis)**;
  - más u64/u32.
- Los `AttackTargetAck`/`AttackerAck` que los acompañan (hash `a80bb127` al agarrar y `e6d438dd`
  al soltar) son la reacción del agarrado y no hacen falta para verlo.

---

## 3. Qué recibían los demás en el MMO

Fuente: `appearance/broadcast_scan.txt` más los llamadores.

| Ack | Escritor ← llamador | Destino | ¿Lo recibe el emisor hoy? | Lector del cliente |
|---|---|---|---|---|
| **RemoteCatchResultAck 2763** | `0x2968330` ← `0x2902DC0` ← `ServerFrameEventRemoteCatch::vf1` `0x27D8EB0` (`0x27D90C9`) | **ToNear** (`0x2902E88`) | **sí** | `0xB5B710` |
| ReleaseRemoteCatchResultAck 2471 | `0x29688C0` ← `0x2902DC0`, `0x2903950` | ToNear | no se emitió | `0xB5BD60` |
| **CatchActionResultAck 2156** | `0x2964C00` ← `.xtls 0x108A9730` ← `0x2C4C970` | **ToNear** | **sí** | `0xB582B0` |
| **ReleaseCatchResultAck 2512** | `0x2966920` ← `.xtls 0x108AFF30` ← `0x2C4CCF0` | **ToNear** | **sí** | `0xB5A170` |
| ForceReleaseCatchResultAck 3050 | `0x296AFA0` | ToNear | — | `0xB5E2D0` |
| GimmickCatcherUpdateAck 2356 | `0x29DC5B0` ← `0x29E47E0` | ToNear | sí | `0xBB0960` |
| UpdateOnDemandGimmickAck 2145 | `0x29A39F0` | ToNear | sí | — |
| CreateProjectileAck 2250 | `0x2A389A0` | ToNear | sí | `0xBF3D50` |
| BreakSceneObjectAck 2746 | `0x299BB80` ← `.xtls 0x108784A0` | **ToNearExceptOneSelf (vacío)** | no | `0xB897E0` |
| StartActionAck/MoveActorAck de la perdiz agarrada | relevo | **ExceptOneControl (vacío)** | no | |
| Interaction*SuccessAck, SyncChallengeAck, UpdateGimmickAck | | ToTarget | solo el emisor | |

Así que, en el MMO, B veía:
- **para el árbol:** el `RemoteCatchResultAck` de A, más sus acciones y movimientos;
- **para la perdiz:** `CatchActionResultAck` y `ReleaseCatchResultAck`, más las acciones de la
  perdiz relevadas desde el cliente que la controlaba.

Los tres Ack clave usan `ToNear`, que funciona. La difusión ToNear serializa una sola vez, así que
el emisor ya tiene los bytes exactos.

---

## 4. Recetas para reproducirlo en el mundo del otro jugador

### R1. Árbol: relevo de `RemoteCatchResultAck` (valor alto, esfuerzo bajo)

- **Emisor:** en `capture_observe_s2c`, si el opcode es 2763 y `+0 == jugador`, emitir
  `Relay(u16 2763 + payload)`. Es fiable: un único mensaje por enganche.
- **Receptor:**
  - comprobar 97 B (o leer la cadena final);
  - `+0x00` = clave de la marioneta;
  - si `+0x0A != 0` (el objetivo es un actor y no un objeto de escena), descartar o traducir;
  - **no tocar el id de escena**: identifica el mismo árbol;
  - inyectar con el retardo del espejo, **después** del `StartActionAck` de `9a368460`, porque
    llega 15 ms después del DoFrameEventReq. Encolarlo con el mismo retardo basta.
- **Lo demás ya llega:** las acciones (`3975eabf`…`563271fe`…`1f4ef471`) y el vuelo (Moves).
- **Requisito:** que el gimmick del árbol esté cargado en el receptor. Su cliente lo carga solo
  cuando su jugador está cerca. Si no lo está, se puede inyectar en el servidor del receptor el
  mismo `SceneLevelOnDemandGimmickSpawnReq` (25 B) que mandó el emisor **(hipótesis; riesgo
  medio)**.
- **Riesgos:**
  1. **(Hipótesis)** el doblado depende de que el cliente receptor simule la cuerda entre la
     marioneta y el árbol a partir de este Ack y de las acciones. Es lo que hacía el MMO, pero no
     está probado.
  2. La marioneta puede no tener la herramienta `0xB01000AB`: es un gimmick propio que se carga al
     entrar. La petición `OnDemandEquipDockingGimmickSpawnReq` de la marioneta (1276,86,
     1462,74…) no recibe respuesta.
  3. El receptor podría mandar `IsRemoteCatchEffectStartStateReq` (2 B, sin clave), que su
     servidor aplicaría **a su propio jugador** (`0x2971BD0`: flag en `comp+0xF0+slot`).
     Inofensivo, pero se puede filtrar.
  4. En la prueba con un solo PC, el árbol es el mismo objeto que el jugador ya tiene enganchado.

### R2. Perdiz: relevo de `CatchActionResultAck` y `ReleaseCatchResultAck` con traducción (valor alto, esfuerzo medio)

- **Emisor:** reenviar 2156 y 2512 si `+0 == jugador`. Añadir los datos del agarrado:
  - variante (`+0x18` de su carga);
  - punto de aparición;
  - posición actual, sacada de su último Move/StartAction.

  Es lo mismo que el evento `Damage`, pero sin exigir el registro 3.
- **Receptor:**
  - emparejar con su propia perdiz por variante y por cercanía al punto de aparición o a la
    posición actual (radio de unos 25 m);
  - **`local_capture` hoy solo guarda el registro 3** (`payload[2] == 3`): hay que añadir el 6
    (animales);
  - `Catch`: `+0` = marioneta, `+4` = perdiz local. `Release`: lo mismo, y desplazar las posiciones
    `+0x08`/`+0x50` si `mirror_offset_m != 0`;
  - si no hay pareja, no enviar nada.
- **Filtro C→S necesario (hipótesis de riesgo):** la perdiz local la controla el cliente receptor,
  que enviará a **su** servidor `StartActionReq`/`MoveActorReq` con padre = `0xA010F001`. Hay
  precedente de que el servidor ignora claves de marioneta: en esta captura recibió
  `MoveActorReq`, `UpdateGimmickReq`, `RegionInOutReq` y `OnDemandEquipDockingGimmickSpawnReq`
  con `0xB010F001`/`0xA010F001` sin problemas. Aun así, conviene que el hook de `PostPacket`
  cambie el padre de la marioneta por `FFFFFFFF` (`+0x116` en StartAction, `+0x8C` en Move).
- **Riesgos:**
  - emparejar mal con perdices amontonadas (salto visible hasta la mano);
  - el jugador local agarrando la misma perdiz;
  - **(hipótesis)** las reservas de aparición pueden colocar los animales en sitios distintos en
    cada mundo; el id de la reserva no está en la carga.
- **Alternativa:** cargar una copia de la perdiz del emisor con dueño ajeno. Es arriesgado: el
  caballo con dueño no aparecía y sin dueño cerraba el juego.

### R3. Objetivos de `StartActionAck` (corrección pequeña, urgente)

`start_action_ack` solo limpia el padre (`+0x117`). El objetivo `+0x16D` sale con la clave del
mundo del emisor: en esta sesión, `0xB01012F7` en 4 Ack. Con dos PC esa clave puede ser **otro
actor** del receptor, porque las claves se asignan en secuencia en cada mundo. Hay que ponerlo a
0, o traducirlo con el mismo emparejamiento de R2.

### R4. Objeto lanzado (prioridad baja)

`GimmickCatcherUpdateAck` y `CreateProjectileAck` (ToNear). `BreakSceneObjectAck` necesitaría un
hook en su escritor `0x299BB80`, como el de equipo. El gimmick se identifica por su id de escena,
que va en `GimmickCaughtByFocusActorReq`/`BreakSceneObjectReq`.

### Lo que NO hay que hacer

- Inyectar en el servidor del receptor una Req de catch o de remote catch (`DoFrameEventReq`): la
  marioneta no existe allí y el handler valida al atacante y su acción.
- Mandar Req con claves de gimmick u objeto a handlers de stats: el crash con
  `VaryStatByAccInputReq` sobre un gimmick lo demuestra.
- R1 y R2 son **solo S→C**.

### Prioridad

R3 (minutos) > **R1** (unas horas, un mensaje) > **R2** (1-2 días: emparejar animales y filtrar
C→S) > R4.

---

## 5. Pruebas (un PC, con `cdmp_peer` relay; copia de la partida antes)

| # | Prueba | Decide |
|---|---|---|
| I1 | R1 con un retardo grande del espejo (≥ 5 s) o una tecla que reinyecte el último `RemoteCatchResultAck` + acciones cuando el jugador ya soltó | Si la marioneta engancha y dobla el árbol |
| I2 | Mirar si el cliente manda `IsRemoteCatchEffectStartStateReq` tras el Ack inyectado | Si hace falta filtrarlo |
| I3 | R2 contra **otra** perdiz cercana (no la que tiene el jugador) | Socket, suelta, y Req con padre = marioneta |
| I4 | Inyectar `CatchActionResultAck` con un agarrado inexistente | Que `0x935DD0` falla sin crash |

---

## 6. Direcciones nuevas (build 2976)

| RVA | Qué es |
|---|---|
| `0x27D8EB0` | `ServerFrameEventRemoteCatch::vf1` (salto a `0x27D90C9` → `0x2902DC0`) |
| `0x2902DC0` | Lógica de remote catch del servidor: escribe RemoteCatch y ReleaseRemoteCatch (ToNear) |
| `0x2968330` / `0x29688C0` | Escritores de RemoteCatchResultAck / ReleaseRemoteCatchResultAck |
| `0x2964C00` / `0x2966920` | Escritores de CatchActionResultAck / ReleaseCatchResultAck (llamados desde `.xtls`) |
| `0xB5B710` → `0x592340` | Cliente: RemoteCatchResultAck → componente `[actor+0x68]+0x78` |
| `0xB582B0` → `0x935DD0` | Cliente: CatchActionResultAck → componente `[actor+0x68]+0x70` |
| `0xB5A170` | Cliente: ReleaseCatchResultAck |
| `0x8B53C0` | Cliente: comprueba si un actor está en estado pendiente (lo usan los lectores de Ack) |
| `0x2971BD0` | Servidor: IsRemoteCatchEffectStartStateReq (sin clave; aplica al jugador de la sesión) |
| `0x29DE610` → `0x2AC0790` | Servidor: UpdateGimmickReq (no relevado) |
| `0x29E0970` → `0x2AC1270` | Servidor: GimmickExecuteFrameEventReq (no relevado) |
| Variante 6 de `DoFrameEventReq` (`+0x203` = 6) | Objetivo del remote catch: `u8 0, u8 clase, u32 actor, id escena 16 B, u64 pieza, vec3, quat` |

## 7. Scripts (`build/research/interactions/`)

| Script | Qué hace |
|---|---|
| `cap.py` | Carga la captura una vez y la guarda en `cap_200328.pkl` |
| `pkeyops.py` | Opcodes que contienen la clave del jugador, con offsets |
| `tl.py`, `seg.py <t0> <t1> [claves]` | Línea de tiempo (completa / compacta) |
| `pactions.py` | Acciones del jugador marcando las raras |
| `ppos.py`, `flights.py` | Trayectoria del jugador y búsqueda de lanzamientos |
| `dump.py <op> t0 t1` | Volcado hex con vista float |
| `findbytes.py <hex>` | Paquetes que contienen unos bytes |
| `*.asm` | Desensamblados citados |
| `seg_1676.txt`, `seg_1718.txt` | Segmentos del árbol y de la perdiz |

---

## 8. Prueba con Shadow y en un PC (6 de octubre)

- **Shadow:** los dos `RemoteCatchResultAck` del anfitrión llegaron al invitado y se descartaron
  allí. La comprobación de claves recorría todos los bytes, y el id de escena y el quat contenían
  `0xB3000000` y `0x92000000`. Todas las acciones del enganche llegaron a la copia. El árbol tiene
  **el mismo id de escena en los dos PCs** y el invitado lo tenía cargado (`UpdateOnDemandGimmickAck`
  2145: `04 | u32 jugador | u8 1 cargado / 0 descargado | id escena`).
- **Un PC, tras el arreglo:** el primer enganche de la copia funciona (marca azul en el árbol); los
  siguientes fallan con `"모종의 이유로 클라에서 RemoteCatch에 실패했습니다…"` desde `0x592536`
  (`0x594AD0` rechaza el enganche) y `"RemoteCatchResultAck처리에 실패했습니다."` desde
  `0xB5BBC9`. La copia nunca soltó el primero: la marca se quedó en ese árbol.
- **Suelta:** `ReleaseRemoteCatchResultAck` (2471), lector `0xB5BD60`: `u32 quien agarra | u8
  ranura | u8 motivo` (6 B), que llama a `0x5A7A10(comp [actor+0x68]+0x78, ranura, motivo, 0, 1)`.
  Las sueltas del propio juego usan motivo 2. La ranura es el `+0x04` del `RemoteCatchResultAck`
  (0 en todas las capturas). El servidor la escribe (`0x29688C0`, ToNear) solo desde su lógica de
  remote catch, y al que se suelta nunca le llega, así que el mod la fabrica para la copia. Lo
  hace en el `StartAction` del emisor que pasa de sujetar (`+4 0x563271FE`) a soltar
  (`+8 0x1F4EF471`), con el que empiezan los `Move` del lanzamiento, y también antes de un
  enganche nuevo si la suelta no llegó.
- Desensamblados: `build/research/interactions/c_b5bd60_releaseremotecatch_ack.asm`,
  `c_5a7a10_release_apply.asm`, `s_29688c0_releaseremotecatch_writer.asm`.

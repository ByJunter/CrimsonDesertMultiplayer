# Apariencia y estado visible de la marioneta (equipo, HP, estado)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`).
Análisis estático de solo lectura (capstone sobre el exe en disco) y capturas
`captures/cdmp_packets_20261003-112627.bin` (captura 2), `-125936`, `-131648` y `-134008`.
El juego no se ejecutó. Todo lo marcado **(suposición)** está sin verificar.

Scripts nuevos en `build/research/appearance/` (ejecutar con `build\research\venv\Scripts\python.exe`):

| Script | Qué hace |
|---|---|
| `equip_timeline.py <bin> [s]` | línea de tiempo sin ruido alrededor de cada `EquipItemReq`/`UnequipItemReq` (guarda la captura ya parseada en `build/research/appearance/cache/`, unos 270 MB en total y regenerables) |
| `ctx.py <bin> <Mensaje> [s] [n]` | lo mismo alrededor de cualquier mensaje |
| `player_loads.py <bin> [hex…]` | cargas del jugador (paquete de usuario + cuerpo) y búsqueda de valores u32 |
| `diff_loads.py capA i capB j [user\|body]` | diff byte a byte de dos cargas del jugador |
| `key_offsets.py <bin> [clave]` | por tipo de mensaje: cuántos llevan la clave del jugador y en qué offset |
| `puppet_reqs.py <bin> [v]` | paquetes con claves de la marioneta y respuestas del servidor |
| `broadcast_scan.py [out]` | **todas** las difusiones del servidor: clase `To*SendTargetFinder` → escritor → opcode (salida en `broadcast_scan.txt`) |
| `reach.py <desde> <hasta>` | ¿se llega de una función a otra por llamadas directas? |

Volcados: `srv_equip_2ad5cc0.asm`, `srv_unequip_2ada5a0.asm`, `w_otherequip_29ed1f0.asm`,
`w_otherunequip_29ed760.asm`, `w_equipack_29eef90.asm`, `cli_otherequip_bbf8d0.asm`,
`cli_otherunequip_bbfe90.asm`, `cli_equipapply_99f500.asm`, `cli_equipapply2_980a50.asm`,
`cli_fe_changedocking_8010c0.asm`, `srv_equipreq_xtls_104a9120.asm`.

## Resumen

1. **El MMO sí difundía los cambios de equipo**: `OtherEquipItemAck` (2757) y
   `OtherUnequipItemAck` (3073) a `ToNearExceptOneSelf`. En un jugador no hay más sesiones y
   el escritor sale antes de serializar, así que **nunca aparecen en las capturas**.
2. El handler del cliente (`0xBBF8D0`) busca el actor por clave y aplica la lista
   `{u32 itemKey, u16 ranura}` a su `ClientEquipSlotActorComponent` (`[actor+0x68]+0x38`).
   **No hay ninguna puerta "solo el actor local"**: debería funcionar con la marioneta (modo 2).
3. Problema: el **itemKey no viaja en ningún paquete del jugador** (`EquipItemReq`/`Ack` solo
   llevan índices de inventario). La forma más limpia de obtenerlo es un **hook inline en la
   entrada del escritor del servidor** `0x29ED1F0` (función completa en `.rsrc`). Para quitar
   equipo basta con el `UnequipItemAck` que ya se captura.
4. Muchos mensajes "para los demás" van a `ToNear`, **que incluye al propio jugador**: el
   cliente del emisor ya los recibe con su clave (`VaryHPAck`, buffs, elemental, muerte…).
   Para la marioneta basta con **reenviarlos cambiando la clave**: son exactamente los bytes
   que el MMO mandaba a los demás.
5. Los "150 bytes" que cambian entre cargas son: un **tamaño anidado** en `+0x0B` (no un
   contador), el **Transform** al final (posición, cuaternión y escala), valores actuales de
   stats y **marcas de tiempo en ms del reloj del servidor**. Además, el paquete del cuerpo es
   una cabecera fija más **una copia exacta** del paquete de usuario.

---

## 1. Equipo

### 1.1 Lo que recibe el emisor (captura 2, t≈805–822 s)

| t (s) | Dir. | Mensaje | Payload |
|---|---|---|---|
| 805.770 | C>S | `UnequipItemReq` (2120) | `05 00` = u16 ranura de equipo |
| 805.781 | S>C | `VaryDataDefinedStaticStatAck` ×1-2 | stats recalculados |
| 805.781 | S>C | `UnequipItemAck` (2280), 12 B | `01 00 10 a0` · `05 00` · `63 00` · `0c 24 00 00` |
| 815.657 | C>S | `EquipItemReq` (2157), 6 B | `02 00` · `3e 00` · `10 00` = tipo de inventario, hueco, ranura |
| 815.658 | S>C | `ApplyBuffAck`/`UpdateChangeBuffLevelAck`/`VaryDataDefinedStaticStatAck` | efectos del objeto (ToNear) |
| 815.659 | S>C | `EquipItemAck` (3098), 17 B | `04` · `01 00 10 a0` · `0f 24 00 00` · `00` · `10 00 02 00 3e 00 00` |
| 820.248 | S>C | `EquipItemAck`, 24 B (cambio) | `04` · clave · `10 24 00 00` · `00` · `10 00 02 00 73 00 00` · `10 00 02 00 3e 00 01` |
| 822.194 | C>S | `OnDemandEquipDockingGimmickSpawnReq` | `04` · clave · `10 00` → el servidor carga un actor gimmick "Game Mechanic" |

Formatos (por los escritores del servidor):

- **`EquipItemAck`** (escritor `0x29EEF90`): `u8 4 | u32 clave | u32 serie | u8 | N × {u16 ranura,
  u16 tipoInv, u16 huecoInv, u8 retirado}`. La entrada con `retirado = 1` es la pieza vieja que
  vuelve al inventario.
- **`UnequipItemAck`** (escritor `0x29F0AC0`): `u32 clave | u16 ranura | u16 huecoInv | u32 serie`.
- La **serie** (`0x240C`, `0x240D`…) es un contador por usuario que comparten las dos Ack
  (`[rsi+0x18]` en el servidor).
- El primer byte `04` de muchos payloads es el **marcador de fragmento** del serializador
  (`4` = paquete completo; 1/2/3 = primero/intermedio/último). El cliente lo lee y no lo comprueba.

### 1.2 Lo que el servidor manda a los demás

`broadcast_scan.py` encuentra los dos únicos relevos de equipo:

| Lógica del servidor | Escritor | Destino | Opcode |
|---|---|---|---|
| equipar `0x2AD5CC0` (llamada en `0x2AD685A`) | `0x29ED1F0` | `ToNearExceptOneSelf` | `OtherEquipItemAck` 2757 (`0xAC5`) |
| quitar `0x2ADA5A0` (llamada en `0x2ADAD0B`) | `0x29ED760` | `ToNearExceptOneSelf` | `OtherUnequipItemAck` 3073 (`0xC01`) |

- `0x2AD5CC0` se alcanza desde `EquipItemReq` (`0x2A03730` → `.xtls 0x104A9120` → `0x2C449A0` →
  `.xtls 0x108860D0` → `0x2AD34C0` → `.xtls 0x1061F800` → `0x2AE7400` → `0x2AD3860`), desde
  `ReserveToChangeEquipFromQuickSlotReq` (`0x2A03D90`), desde el cambio por ranura rápida
  (`ServerFrameEventChangeEquipByQuickSlot` `0x27DDB80` → `0x2AD5600`), desde
  `SwapEquipSlotToOtherSlotReq` y desde secuencias. **Un hook en el escritor cubre todos los caminos.**
- `UnequipItemOnlyAck`/`EquipItemOnlyAck` (ToNear) aparecen solo con claves `0xB01…` (NPC
  desarmados). No afectan al jugador.
- `UnequipItemListByItemDestroyAck` (2179, ToNear): objeto destruido → se recibe con la clave
  propia y basta con reenviarlo (§2).

**Formato `OtherEquipItemAck` (2757)** (escritor `0x29ED1F0`, lector `0xBBF8D0`):

| Offset | Tipo | Campo |
|---|---|---|
| `+0x00` | u8 | `4` (marcador de fragmento) |
| `+0x01` | u32 | clave del actor |
| `+0x05` | u32 | serie (la misma que `EquipItemAck+0x05`) |
| `+0x09` | N × 6 B | `{u32 itemKey, u16 ranura}`; `itemKey = 0` → quitar esa ranura |

Tamaño `9 + 6·N`. El lector calcula `N = (len − 9) / 6` y descarta el paquete si sobra un byte.
El servidor mete primero las piezas puestas (`{clave, ranura}`) y luego las retiradas
(`{0, ranura}`). En el cambio de 820 s sería `[{nueva, 0x10}, {0, 0x10}]`.

**Formato `OtherUnequipItemAck` (3073)** (escritor `0x29ED760`, lector `0xBBFE90`), 10 B, sin
byte de fragmento: `u32 clave | u16 ranura | u32 serie`.

**itemKey**: el servidor lo obtiene con `0x38AB60(item+8)`, que indexa la tabla estática de
objetos (`[exe+0x6D69AA8]+0x58`) por un u16 y devuelve la ficha; el u32 es el primer campo de la
ficha. Es un **id de datos estáticos**, igual en todos los PCs. El cliente lo convierte con
`0x38AAC0` (stub de Denuvo).

### 1.3 Qué hace el cliente que lo recibe

`0xBBF8D0` (`OtherEquipItemAck`):
1. Valida la trama y la puerta "no listo" `0x8B53C0` (igual que Move: si la marioneta aún no
   terminó el login, el paquete va a la cola de reintentos).
2. Busca el actor con `0x8A67E0` (gestor `[[exe+0x6D69188]+0x38]`).
3. Toma `[[actor+0x68]+0x38]` (`ClientEquipSlotActorComponent`) y llama a `0x99F500(comp, serie, lista)`.

`0x99F500` separa las entradas en "poner" (itemKey ≠ 0) y "quitar" y llama a
`0x980A50(comp, &serie, poner, quitar)`. **Puerta de serie**:

```
if (serie > comp+0x18) { comp+0x18 = serie; aplicar }
else if (dword [exe+0x6CF74F0] == 0) aplicar      // sin actualizar la serie
else descartar
```

No he encontrado ninguna comprobación de "actor controlado localmente" (`+0xC8`, `+0x96`,
`vf188`) en `0x99F500`, `0x980A50` ni sus llamadas principales. Las referencias a
`WorldSystem+0xD8` que aparecen son búsquedas de recursos. Por tanto, **debería aplicarse a la
marioneta** (suposición fuerte; verificar en el juego).

`0xBBFE90` (`OtherUnequipItemAck`) llama a `0x980500(comp, out, ranura, 1)` y **no comprueba la serie**.

### 1.4 Receta

**Emisor (solo lectura de argumentos, en el hilo del servidor):**

- Hook inline (safetyhook) en la **entrada** de `0x29ED1F0`. Cabecera esperada:
  `48 89 5C 24 08 4C 89 4C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56`.
  - Argumentos: `rcx` = actor del servidor (clave en `[rcx+0x60]`), `rdx` = target finder,
    `r8` = `u32*` serie, `r9` = vector `{ptr datos, u32 cuenta en +8}` con elementos de 8 B
    `{u32 itemKey, u16 ranura, 2 B relleno}`.
  - Copiar (con SEH) solo si `[rcx+0x60]` es la clave del jugador (`0xA01…`), encolar sin
    bloquear y llamar al original (que sale enseguida porque no hay destinatarios).
- Quitar equipo: o bien hook igual en `0x29ED760` (`48 89 5C 24 08 4C 89 44 24 18 55 56 57 41 54 …`;
  `rcx` = actor, `r8` = `u16*` ranura, `r9` = `u32*` serie), o **sin hook**, desde el
  `UnequipItemAck` (2280) que ya ve el sniffer: `ranura = ack[4:6]`, `serie = ack[8:12]`.
- Mantener un mapa `ranura → itemKey` (0 = vacía) con lo visto desde la última carga. Sirve para
  reponer el estado tras un respawn (§1.5).

**Receptor:**

```text
OtherEquipItemAck (2757):   cabecera C5 0A | FF | u16 len
  payload = 04 | u32 0xA010F001 | u32 serie | N × (u32 itemKey, u16 ranura)

OtherUnequipItemAck (3073): cabecera 01 0C | FF | 0A 00
  payload = u32 0xA010F001 | u16 ranura | u32 serie
```

- La serie tiene que ser **mayor** que la que guarda la marioneta (`comp+0x18`; la comparación
  es con signo). La del emisor solo crece dentro de una sesión. Usar siempre la última vista.
- Si el emisor cambia de partida, la serie puede bajar: en ese caso habrá una plantilla nueva
  y un respawn, así que la marioneta empieza con la serie nueva.
- El sniffer del receptor verá los paquetes inyectados: ignorar los que lleven claves de la marioneta.

**Efecto secundario esperado:** al cambiar el equipo, el cliente del receptor enviará
`OnDemandEquipDockingGimmickSpawnReq` con la clave de la marioneta (como ya hace al aparecer:
ranuras `0,1,2,0xF,0x14,0x17`). El servidor no conoce ese actor y no carga nada. En el primer
clon las armas sí se veían (captura en `experimento-4c.md`), así que estos "gimmicks de
acoplamiento" parecen ser solo objetos especiales (en la captura, la ranura `0x10` carga un
actor "Game Mechanic"). **(Suposición)** Algún objeto de ese tipo no se verá en la marioneta.

### 1.5 Plan B y respawn: ¿se puede obtener un `ActorLoadDataAck` nuevo?

- El servidor **no** reenvía el `ActorLoadDataAck` del jugador al cambiar de equipo ni en el
  viaje rápido. En las 5 capturas solo llega al cargar la partida.
- El escritor de `ActorLoadDataAck` no se encuentra con el opcode como inmediato (seguramente
  está en `.xtls`). Forzar al servidor a generar uno para nuestra propia sesión obligaría a
  descartarlo antes de que lo procese nuestro cliente: **no recomendado**.
- Parchear el equipo dentro de la plantilla exige decodificar el deserializador de
  `ActorLoadDataAck`, recalcular el tamaño anidado (`+0x0B`, §3) y la copia del cuerpo.
  **No hace falta**, porque `OtherEquipItemAck` acepta una lista completa.
- **Receta de respawn:** plantilla antigua + justo después un `OtherEquipItemAck` con
  **todas** las ranuras cambiadas desde la carga (mapa del emisor) y la última serie. Un único
  mensaje fiable, idempotente.

Limitación: el tinte, el encantamiento y la durabilidad no viajan en `OtherEquipItemAck` (solo
el itemKey). Los mensajes de tinte (`DyeItem*Ack`) solo van al propio jugador. **(Suposición)**
La marioneta mostrará el objeto con el tinte por defecto hasta el siguiente respawn con plantilla nueva.

---

## 2. Otro estado visible

### 2.1 Lo que el MMO enviaba a los demás

`broadcast_scan.py` resuelve 731 sitios de envío. Además de `ToTarget` (536, respuestas
individuales), los relevos que tocan al personaje son:

| Mensaje (op) | Destino | ¿Lo recibe ya el emisor? | Cómo conseguirlo |
|---|---|---|---|
| MoveActorAck / RotateActorAck / StartActionAck / ChangeAIStateAck / UpdateWallUpDataAck | ExceptOneControl | no | desde las Req (ya hecho) |
| **OtherEquipItemAck / OtherUnequipItemAck** | ExceptOneSelf | no | §1 |
| **ChangeDockingSocketByFrameEventAck** (2747) | ExceptOneSelf | no | §2.2 |
| ChangeVehiclePhysicsStateAck (2970), BreakSceneObjectAck, ClearCombinationChildAck | ExceptOneSelf | no | no prioritario |
| **VaryHPAck** (2147) | ToNear | **sí** | reenviar |
| DataDefinedRegenerateValueAck (2586), VaryDataDefinedStaticStatAck (2434), CharacterDataDefined*StatAck | ToNear | sí | reenviar |
| ApplyBuffAck (2506) / UnapplyBuffAck (3007) / UpdateChangeBuffLevelAck (2252) | ToNear | sí | reenviar |
| UpdateElementalAck (2642) (mojado, ardiendo…) | ToNear | sí (≈15/s) | reenviar si se ve útil |
| NotifyHandleDeadAck (2530), NotifyHandleFatalAck (2899), NotifyHandleReviveAck (3053), ReviveAck (2475) | ToNear | sí | reenviar (fiable) |
| CharacterCustomizationAck (2622) | ToNear | sí (al entrar) | reenviar (fiable) |
| ChangeCharacterNameAck (2484) | ToNear | sí | reenviar |
| UnequipItemListByItemDestroyAck (2179) | ToNear | sí | reenviar |
| VarySharpnessAck, VaryEquipItemEnduranceAck, UpdateEquipItemCoolTimeAck | ToNear | sí | no visible |
| AttachLinkVehicleAck / DetachLinkVehicleAck / RideOffByServerAck | ToNear | sí | §2.4 |
| PostProcessAttackResult_* | ToNear | sí | combate, fuera de alcance |

Como `ToNear` serializa una sola vez para todos, **los bytes que recibe el emisor son
exactamente los que recibía cualquier jugador cercano**. Receta genérica:

1. En `capture_observe_s2c`, si el opcode está en la lista blanca y el payload contiene la clave
   del jugador, enviarlo tal cual: `u16 opcode + payload`.
2. En el receptor, comprobar el tamaño esperado, sustituir **todas** las apariciones de la clave
   del jugador y de la de usuario por las de la marioneta (como `replace_u32` en
   `puppet_event`) e inyectar con slot `0xFF`.

Offsets de clave observados en la captura 2 (`key_offsets.py`):

| Mensaje | Tamaño | Clave del jugador en |
|---|---|---|
| VaryHPAck | 92 | `+0x00`; en `+0x54` (origen del cambio; regeneración = la propia clave) |
| DataDefinedRegenerateValueAck | 90 | `+0x00` (`+0x54` en 7 casos) |
| VaryDataDefinedStaticStatAck | 41 | `+0x00` |
| ApplyBuffAck / UnapplyBuffAck | 77 | `+0x00` y `+0x18`/`+0x1C` (lanzador y objetivo) |
| UpdateChangeBuffLevelAck | 42 | `+0x00` |
| UpdateElementalAck | 118 | `+0x00` |
| NotifyHandleDeadAck | 19 | `+0x04` |
| CharacterCustomizationAck | 279 | `+0x00` |

Claves de otros actores dentro de esos mensajes (atacante NPC `0xB01…`) apuntan a actores del
PC emisor: dejarlas como están (no existen en el receptor) o ponerlas a 0. **(Suposición)** Los
handlers toleran claves desconocidas en esos campos secundarios.

### 2.2 Arma desenvainada o enfundada

- No hay un mensaje de estado. Desenvainar es una **acción** (`StartAction`, ya replicada) cuyos
  *frame events* mueven el arma de un "socket" a otro.
- El cliente tiene `ClientFrameEventChangeDockingSocket` (vtable `0x55844C8`, ejecución
  `0x8010C0`). **No tiene puerta de control local**: usa el componente de acoplamiento
  `[comps+0x170]` del actor que hace la acción.
- El servidor tiene `ServerFrameEventChangeDockingSocket` (`0x27DDE40`), que reenvía
  `ChangeDockingSocketByFrameEventAck` (2747, escritor `0x2969BC0`) a `ToNearExceptOneSelf`.
  Ese relevo indica que el MMO **no confiaba** en que los demás clientes ejecutaran ese evento
  por su cuenta.
- **(Suposición)** Si el sistema de acciones ejecuta los frame events también en actores
  remotos, el arma de la marioneta ya cambia de mano con el espejo actual. Si no, hay que
  construir `2747` a partir del `DoFrameEventReq` (2380, C>S, 517–3302 B, clave en `+0x01`).
  Su formato no está decodificado: lector del cliente `0xB5D2E0` = `u32 clave` + struct
  `0x1F78F20`; deserializador del servidor en `0x296D130`.
- `ChangeBattleStateReq` (2257, `u32 clave | u8 0/1`, entrar o salir de combate) **no se
  reenviaba** (no hay Ack ni relevo en el servidor). No hace falta.

### 2.3 Capucha o casco

No existe mensaje. Las cadenas del exe muestran que depende del objeto y de las acciones:
`_showHelmOnBattleStance` (propiedad del objeto), `ClientFrameEventSetVisualEquipItem` (frame
event) y `_visualEquipItemInfo`. `AddVisualEquipItemAck` (2265) solo lo usan los minijuegos
(`SpawnMiniGameCharacterReq`/`ApplyMiniGameReq`). **(Suposición)** Con el equipo (§1) y las
acciones sincronizadas, la capucha seguirá al emisor. Verificarlo en el juego.

### 2.4 Montura

- Subir al caballo es una interacción: `InteractionDoStepReq` (C>S,
  `01 00 10 a0 | 03 00 10 b0 | 90 01 00 00 | 02 00 00 00`, es decir, jugador, caballo,
  interacción `0x190` y paso 2) → `InteractionDoStepSuccessAck` solo al jugador.
- Montado, los Move/Rotate llevan como padre la clave del caballo (`0xB0100003`); el espejo
  actual la borra.
- Para verlo bien hace falta **una segunda marioneta (el caballo)**: plantilla =
  `ActorLoadDataAck` del caballo que llega tras `CallVehicleMercenaryAck`, con clave nueva, y
  traducir la clave del padre en Move/Rotate/StartAction. Es trabajo aparte. Hasta entonces, la
  marioneta "monta" sin caballo.

### 2.5 Emotes

Son acciones: `StartActionAck` (ya replicado). No hay mensajes sociales específicos.

### 2.6 HP

`VaryHPAck` (2147, 92 B, ToNear). Disposición observada **(parcialmente suposición)**:

| Offset | Campo | Ejemplo (t=15 s) |
|---|---|---|
| `+0x00` | u32 clave | `0xA0100001` |
| `+0x04` | u32 id de stat | `0x000F4240` (HP) |
| `+0x08` | i64 actual ×1000 | 278000 = 278 HP |
| `+0x18` | i64 máximo ×1000 | 300000 |
| `+0x30` | u64 marca de tiempo (ms del reloj del servidor) | 38487 |
| `+0x38` | u64 | 1000 |
| `+0x40` | u32 contador | 1, 2, 3… |
| `+0x54` | u32 clave del origen | la propia (regeneración) o la del atacante |

El handler `0xB75020` pasa la puerta "no listo", busca el actor y actualiza su stat
(`0x62FA00`). Con eso la marioneta tendría el HP correcto internamente. **(Suposición)** El juego
no dibuja barra de vida para un "jugador" que no es el local, así que la barra visible es mejor
pintarla en el overlay con `PlayerState.health/health_max`, que ya viajan.

### 2.7 Nombre

El nombre va en el paquete de usuario como cadena con longitud (`+0x2B` u32 = 5, `+0x2F` "Kliff"
en UTF-16). Siempre es "Kliff". `UpdateVisiblePlayerAck` (2135, sin escritor en el servidor)
lee una lista de claves y actúa sobre su visibilidad (`0x17FA6C0`, `0xA9DD70`), no sobre un
nombre. **(Suposición)** No hay etiqueta nativa de nombre para otros jugadores. Usar el overlay
con el nombre del peer (`Hello.name`).

---

## 3. Qué cambia entre cargas del paquete del jugador

Comparaciones hechas con `diff_loads.py`:
- carga [0] contra carga [1] de una misma sesión;
- carga [1] de sesiones distintas con la misma partida (112627 contra 131648);
- carga [1] de partidas distintas (112627 contra 134008).

**Estructura descubierta:**
- La carga [0] de cada sesión (12.803 B) es un personaje **por defecto** en `(0, 1000, 0)`. La
  [1] es la partida real. `local_capture` se queda con la última, pero si se pidiera un spawn
  entre ambas usaría la [0]: conviene **ignorar plantillas en `(0, 1000, 0)`**.
- **El paquete del cuerpo = `u16 L` + L bytes de cabecera propia (`0x1820` en la partida real,
  con byte `0x07` y la clave del cuerpo) + una copia exacta del paquete de usuario** (0 bytes
  distintos en todas las capturas). Todo lo que cambia en el usuario cambia igual en el cuerpo,
  desplazado `+0x1822`.

**Campos que cambian:**

| Dónde (paquete de usuario) | Qué es | ¿Importa al reinyectar tarde? |
|---|---|---|
| `+0x0B` u16 | **tamaño anidado = longitud del payload − 13** (`0x31F6` = 12803−13, `0x31FA` = 12807−13, `0x316D` = 12666−13). Corrige `captura-2.md`, donde aparecía como "contador" | Solo si se cambia la longitud del paquete: hay que actualizarlo |
| final − 154: pos (12) + quat xyzw (16) + escala (12) | Transform de mundo; quat solo de guiñada (`0, y, 0, w`) | **Sí**: ya se escribe la posición; conviene escribir también el **cuaternión** (final − 142), o la marioneta mira hacia donde miraba al cargar hasta el primer Move |
| p. ej. `+0x3B9` (i64 ×1000) | valores actuales de stats: HP 278000 frente a 300000, etc. (los offsets dependen del contenido de la partida) | HP o aguante antiguos en la marioneta; se corrige con `VaryHPAck` si se reenvía |
| 2 bytes bajos de u64 en entradas cada `0x80` (`+0x3E1`, `+0x461`…) y en listas de buffs/habilidades | **marcas de tiempo en ms del reloj del servidor del emisor**: `0x9657` = 38.487 ms, el mismo valor que `VaryHPAck+0x30` en ese momento | **(Suposición)** En el receptor se comparan con su propio reloj: la regeneración puede parecer congelada o saltar, y los buffs durar de más o de menos. Es cosmético |
| `+0x25A3` 4 B (`b1 66 2e f4`) | valor aleatorio o hash por sesión | **(Suposición)** inofensivo |
| listas de bytes `01` (`+0x28FC…`), entradas de buffs | contenido de la partida (conocimientos, buffs activos) | solo entre partidas distintas |

Las claves (usuario `+0x03/+0x12/+0x1F`, jugador ×8 desde `+0x0E`) ya las reescribe `puppet_spawn`.

---

## 4. Plan de implementación por prioridad

Todos los cambios de formato suben `proto::kVersion` (3 → 4).

1. **Equipo en vivo: hook en el escritor del servidor más inyección.**
   - Emisor:
     - hook en `0x29ED1F0`, comprobando la cabecera de bytes y la build;
     - opcional, hook en `0x29ED760`, o derivarlo de `UnequipItemAck`;
     - nuevo `RelKind::Equip = 3` con payload `u32 serie | u8 n | n × (u32 itemKey, u16 ranura)`;
     - quitar equipo como el mismo mensaje con `itemKey = 0`, o `RelKind::Unequip = 4` con `u16 ranura | u32 serie`.
   - Receptor: construir `OtherEquipItemAck` o `OtherUnequipItemAck` (§1.4) con la clave de la
     marioneta e inyectarlo por la cola de `remote_puppet`, sin retardo de movimiento.
   - **Verificar (un PC):**
     - F9 para crear el clon y cambiar una pieza visible (pecho o capa);
     - en `cdmp.log`, el hook con N entradas y el código de retorno del handler
       (0 = aplicado, `[0x6CDB970]` = formato erróneo, `[0x6CDB9D0]` = reintentar);
     - captura de pantalla: el clon cambia;
     - repetir quitando una pieza y con un cambio de arma por ranura rápida.
2. **Estado de equipo tras un respawn.**
   - El emisor guarda el mapa `ranura → itemKey` desde la última plantilla y lo manda (un único
     `RelKind::Equip` con todas las ranuras y la última serie) justo después de cada `Spawn`.
   - **Verificar:** cambiar de equipo, F10 y F9: el clon aparece ya con el equipo nuevo.
3. **Mejoras del spawn.**
   - Escribir el cuaternión actual en final − 142 en los dos paquetes.
   - Ignorar plantillas en `(0, 1000, 0)`.
   - **Verificar:** el clon aparece mirando hacia donde mira el jugador.
4. **Relevos `ToNear`.**
   - Nuevo `EventKind::Relay = 5` (no fiable) y `RelKind::Relay = 5` (fiable), con payload `u16 opcode + payload`.
   - Lista blanca:
     - frecuentes (no fiables): `VaryHPAck`, `DataDefinedRegenerateValueAck`;
     - raros (fiables): `NotifyHandleDead/Fatal/ReviveAck`, `ReviveAck`, `CharacterCustomizationAck`,
       `UnequipItemListByItemDestroyAck`, `ApplyBuff/UnapplyBuff/UpdateChangeBuffLevelAck`;
     - opcional: `UpdateElementalAck`.
   - Receptor: tamaño exacto, `replace_u32` de las claves de usuario y jugador, e inyectar.
   - **Verificar:**
     - recibir daño y ver en el log los `VaryHPAck` inyectados con resultado 0;
     - leer el HP de la marioneta con el lector de stats (como `PlayerReader`, buscando el actor
       por clave) y comprobar que coincide;
     - morir, para ver si el clon reproduce la muerte.
5. **Overlay sobre la marioneta.** Nombre del peer y barra de HP (de `PlayerState`) anclados a la
   posición real del actor de la marioneta (buscarlo por clave `0xA010F001` en el gestor
   `[[exe+0x6D69188]+0x38]` con `0x8A67E0`, o leer su transform), no a la del ghost.
   **Verificar** con captura de pantalla.
6. **Arma desenvainada.**
   - Primero solo observar: desenvainar y enfundar con el clon presente y mirar si su arma cambia
     de mano.
   - Si no cambia: investigar el formato de `DoFrameEventReq` y construir
     `ChangeDockingSocketByFrameEventAck` (2747).
7. **Montura (más adelante).** Marioneta del caballo con su propia plantilla y traducción de la
   clave del padre.

Riesgos y reglas:
- El hook del paso 1 corre en el **hilo del servidor**. Solo debe copiar los argumentos (con
  SEH), sin locks bloqueantes, y llamar siempre al original.
- Como en el experimento 4c, el cliente del receptor mandará Req con la clave de la marioneta
  (`OnDemandEquipDockingGimmickSpawnReq`…). **No guardar la partida con la marioneta presente.**

---

## 5. Prueba en vivo 1: el equipo de la marioneta afecta a la UI local

Build **1.0.0.2976**. Datos: captura `captures/cdmp_packets_20261003-160139.bin` y log
`captures/cdmp_160126_test_v4.log`. Análisis estático de solo lectura. El juego no se ejecutó.
Lo marcado **(suposición)** está sin verificar.

Scripts en `build/research/equip2/` (con `build\research\venv\Scripts\python.exe`):

| Script | Qué hace |
|---|---|
| `tl.py <bin>` | línea de tiempo sin ruido alrededor de todo mensaje con "Equip" (salida en `tl.txt`) |
| `loads.py <bin> [hex…]` | `ActorLoadDataAck` del jugador y de la marioneta, y dónde aparece cada u32 |
| `charno.py <bin>` | offsets del characterNo, de la serie de equipo y de las cargas |
| `dump.py <bin> <op> <i> [desde] [len]` | volcado hex de un paquete |
| `errname.py <rva>…` | nombre `eErrNo…` de una variable global de código de error |

Volcados: `d_bc1760.asm` (EquipItemAck), `d_bc3010.asm` (UnequipItemAck), `d_98c9c0.asm` y
`d_98e5e0.asm` (aplicación local), `d_97fe40.asm` (deserializador del componente),
`d_aa90d0.asm`, `d_aa8e40.asm` y `d_b6c3f0.asm` (caché `cacheEquipSlotList`), y otros.

### 5.1 Secuencia real (t = segundos desde el inicio de la captura; t 115,03 = 16:03:51,64)

| t | Dir. | Mensaje | Payload / nota |
|---|---|---|---|
| 45,972 | S>C (inyectado) | `ActorLoadDataAck` de la marioneta | usuario `0x9010F001`, jugador `0xA010F001`, cuerpo `0xB010F001` |
| **45,995** | **C>S** | **`ClientCacheReq`** (2674) | `"cacheEquipSlotList"` + u64 `0x7FFFFFFFFFFFFFFF`. **Solo aparece aquí**: no lo provoca la carga propia (15,6 s y 44,4 s), sino la de la marioneta |
| 46,009 | S>C | `ClientCacheAck` (2323), 3605 B | clave **`0xA0100001`** (la LOCAL), serie `0x2400` y la lista de equipo del jugador |
| 46,336 | C>S | `OnDemandEquipDockingGimmickSpawnReq` | clave de la marioneta, ranuras `0,1,2,0xF,0x10,0x14,0x17` (ya conocido) |
| 98,913 / 98,924 | C>S / S>C | `UnequipItemReq` `04 00` → `UnequipItemAck` | ranura 4 → hueco 0x79, serie `0x2401` |
| 100,488 | S>C (inyectado) | `OtherEquipItemAck` | `04 01f010a0 01240000 \| 00000000 0400` = {0, ranura 4} |
| 102,977 / 102,981 | C>S / S>C | `UnequipItemReq` `10 00` → `UnequipItemAck` | ranura 0x10 → hueco 0x7A, serie `0x2402` |
| 105,113 | S>C (inyectado) | `OtherEquipItemAck` | {0, ranura 0x10}, serie `0x2402` |
| 115,025 / 115,033 | C>S / S>C | `EquipItemReq` `02 00 12 00 04 00` → `EquipItemAck` | hueco 0x12 → ranura 4, serie `0x2403` |
| 115,989 | S>C (inyectado) | `OtherEquipItemAck` | {itemKey `0x1B23D`, ranura 4}, serie `0x2403` |
| **127,456 / 127,470** | C>S / S>C | **el mismo** `EquipItemReq` `02 00 12 00 04 00` → **`FailEquipItemAck`** | el servidor ya lo tenía puesto: **el cliente local no había aplicado el `EquipItemAck` de 115 s** |
| 129,182 / 129,187 | C>S / S>C | `EquipItemReq` hueco 0x7F → `EquipItemAck` (cambio) | pone 0x7F y retira 0x12, serie `0x2404` |
| 132,786 | S>C (inyectado) | `OtherEquipItemAck` | [{`0x238E`, 4}, {0, 4}], serie `0x2404` |
| 133,192 / 133,195 | C>S / S>C | `EquipItemReq` hueco 0x26 → ranura 3 | serie `0x2405` |
| 134,413 | S>C (inyectado) | `OtherEquipItemAck` | {`0x196F0`, 3}, serie `0x2405` |

Conclusiones:
- **Los 5 `OtherEquipItemAck` inyectados son correctos**: clave `0xA010F001`, la serie real,
  itemKeys estáticos y ranura correcta. En el cambio de 129 s el formato `[{nuevo, r}, {0, r}]` es
  el del servidor: `0x980A50` procesa primero las retiradas y luego las puestas.
- **Tras ninguna inyección envía el cliente un Req** (ni `OnDemandEquipDocking…` ni nada con la
  clave local). Los únicos C>S con claves de la marioneta son `RegionInOutReq` ×4,
  `UpdateGimmickReq` ×1 y `OnDemandEquipDockingGimmickSpawnReq` ×1, todos al aparecer.
- **Los dos fallos locales ocurren ANTES de la inyección correspondiente**: el `UnequipItemAck`
  de 98,92 s llega 1,56 s antes que el 2757 de la marioneta, y el `EquipItemAck` de 115,03 s
  0,96 s antes. Por tanto, **el 2757 no es la causa directa**. El estado ya estaba corrompido
  desde antes, probablemente desde el spawn (§5.3).
- Retardo real de la marioneta: entre 0,96 y **3,6 s**, no 0,35 s. El log muestra que el evento
  llega al receptor unos 60 ms después. El retardo lo añade la cola `movement=true` de
  `remote_puppet`.
- En los mismos lotes también se reenvían `ApplyBuffAck`/`UnapplyBuffAck` del equipo, con origen
  `22 | clave | ranura`, y `VaryDataDefinedStaticStatAck`, con la clave cambiada.

### 5.2 Estática del camino `OtherEquipItemAck`: no comparte estado

- `0xBBF8D0` busca el actor por clave (`0x8A67E0`) y llama a `0x99F500([[actor+0x68]+0x38], serie, lista)`.
- `0x99F500` **crea objetos de ítem nuevos a partir de los datos estáticos**: `0x24097A0`, luego
  `0x38AAC0(itemKey)` y `0x2409970(info, 1)`, con `+0x20 = 0` y `+0x30 = 0`. **No llevan número de
  instancia** ni se registran en ninguna tabla global: el constructor y la asignación
  (`0x240B070`) no tocan variables globales.
- La serie va **por componente** (`comp+0x18`). `[exe+0x6CF74F0]` **no es un contador**: es el
  código de error **`eErrNoInvalidCachedSequenceNo`** (registrado en `0x2202C09`). La puerta es:

  ```
  if (serie > comp+0x18) aplicar
  else devolver eErrNoInvalidCachedSequenceNo
  ```

  La serie es el **"CachedSequenceNo" de la caché de la lista de equipo**.
- `0x980A50`: si una ranura que hay que quitar no existe en `comp+0x90`, devuelve
  `eErrNoItemNotExist` (`0x6CF7D30`) y **aborta todo el mensaje**. Las operaciones por ranura
  (vf `+0x140` para poner, `0x980500` para quitar y `0x97C4B0` para el acoplamiento) están
  **virtualizadas por Denuvo** (saltan a `.xtls`). No se puede descartar del todo que dentro
  haya estado compartido.
- El notificador de UI `0x9A1DF0` (`[[[exe+0x6D69190]+0x80]+0x338]`, recibe la clave del actor) solo
  lo llaman los caminos **locales** `0x98C9C0` (EquipItemAck) y `0x98E5E0` (UnequipItemAck), no
  `0x980A50`. **El 2757 no refresca la UI.**

### 5.3 Lo que sí comparte la marioneta: es "el mismo personaje"

1. **characterNo**: el paquete de usuario lleva el u64 `0x7FFFFFFFFFFFFFFF` en **`+0x17` y
   `+0x43`**, y el cuerpo en `+0x1839`/`+0x1865` (desplazados `+0x1822`). `puppet_spawn` no los
   reescribe. Es el mismo valor que en `LoginCharacterToFieldServerReq`.
2. **Números de instancia de los objetos**: la lista de equipo de la plantilla (serie u32 en
   **`+0x1763`**, que vale `0x23FC`, y 16 entradas desde `+0x1773`) contiene `{u64 itemNo, u32 itemKey,
   u64 cantidad…}`. Por ejemplo `0x3DD/0x310DA` y `0x3E6/0x310D2`: **los mismos itemNo que el
   inventario local** (`EquipmentListOnLoginAck`).
3. **Caché global `EquipSlotListCache`** (`pa::IClientCache`, vtable `0x55C9F00`, gestor
   `[exe+0x6D691F0]`, nombre `"cacheEquipSlotList"`):
   - El deserializador del componente `0x97FE40` llama a la caché para cualquier actor con
     `(clave & 0x30000000) == 0x20000000` (`0xA01…`, también la marioneta).
   - Lo hace con `0xAA90D0(caché, out, actor, &id, characterNo, serie)`.
   - La entrada se identifica por **(characterNo, serie)** (`0xAA8E40`) y **no por la clave del actor**.
   - La serie de la plantilla (`0x23FC`) estaba atrasada, así que la caché:
     - marca la entrada como inválida;
     - **borra el fichero de caché en disco** (`0x1328D10`);
     - manda el `ClientCacheReq` de 45,995 s.
   - El servidor contesta con la lista **del jugador local** (`0xA0100001`, `0x2400`).
4. **Aplicación de la caché**:
   - `ClientCacheAck` (`0xB6C3F0`) → `0xAA99C0`/`0xAA9960` → `EquipSlotListCacheData::Apply` (`0xAA7F20`).
   - `Apply` busca el actor con **`0x8AEB30([[exe+0x6D69190]+0x30], characterNo)`**: recorre la lista de
     actores "UserOrCharacterValidLogined" y devuelve **el primero** con
     `[[[actor+0x68]+0x20]+0x70] == characterNo`.
   - Después **sobrescribe directamente el componente de equipo** de ese actor: `+0x18` (serie),
     `+0x1C`, `+0x20`, ranuras `+0x90`, `+0x1C8`…
   - Con dos actores con el mismo characterNo, la caché (y cualquier búsqueda "mi personaje" por
     characterNo) puede resolver **al actor equivocado**, o reescribir el componente local con
     una instantánea.

**Hipótesis (suposición fuerte; encaja con los dos síntomas y con el orden temporal):**
- El spawn de la marioneta mezcla la identidad del personaje local:
  - el mismo characterNo;
  - los mismos itemNo;
  - la caché reaplicada a 46 s y su fichero borrado.
- Desde entonces el estado local de equipo y lo que pinta la UI dejan de coincidir:
  - **Al quitar (98,9 s)**: el objeto desaparece visualmente, pero la ranura 4 sigue "ocupada" en
    la UI. El 2757 posterior no refresca la UI (§5.2).
  - **Al poner (115 s)**: el `EquipItemAck` local no se aplica, o la UI mira otro componente. La
    marioneta sí lo aplica, porque su componente parte de un estado limpio.
  - **Al reintentar (127 s)**: el reintento confirma que la UI local seguía viendo el objeto en la bolsa.
  - **Se arregla solo** con el siguiente cambio que retira esa ranura (129 s), como describió el usuario.
- Mecanismo alternativo **(suposición)**: tablas indexadas por itemNo dentro de las funciones
  virtualizadas (`+0x140`, `0x980500`, `0x980890`).

### 5.4 Arreglo recomendado (por orden)

1. **Experimento A/B (antes de tocar nada, un PC):**
   - **(a)** co-op activo, sin marioneta;
   - **(b)** marioneta creada con `puppet_set_equip_enabled(false)`;
   - **(c)** como en esta prueba.
   - En cada caso, quitar y poner la ranura 4 y comprobar la UI.
   - Si (b) ya falla, la culpa es del spawn (§5.3) y no del 2757.
2. **Separar la identidad del personaje en la plantilla** (`puppet_spawn`, en el usuario y en el
   cuerpo):
   - comprobar que hay exactamente 2 apariciones de `0x7FFFFFFFFFFFFFFF` en cada paquete
     (usuario `+0x17`/`+0x43`, cuerpo `+0x1839`/`+0x1865`);
   - sustituirlas por un valor propio, p. ej. `0x7FFFFFFFFFFFF001`;
   - el tamaño no cambia, así que no hay que tocar `+0x0B`.
   - Riesgos **(suposición)**: la caché pedirá `cacheEquipSlotList` para un characterNo
     desconocido (posible `Nak`/error del servidor local) y algún sistema puede exigir que sea
     el del usuario. Validar con `cdmp_peer` antes que en el juego.
3. **Igualar la serie de la plantilla** (`+0x1763` y cuerpo `+0x2F85`) con la última serie vista
   (`g_equip_serial` o la del `EquipItemAck`/`UnequipItemAck`). Así la caché no se invalida, no
   se borra el fichero y no hay `ClientCacheReq`.
4. **Plan B/defensa:** no dejar salir un `ClientCacheReq` con `"cacheEquipSlotList"` en los 2 s
   siguientes a un spawn de la marioneta. Si no se puede bloquear un C>S, descartar el
   `ClientCacheAck` correspondiente antes de que lo procese el cliente.
5. **Opcional (suposición):** reescribir los itemNo de la lista de la plantilla a valores únicos
   (`0x7F00000000000000 | i`). Hace falta decodificar del todo el registro de ítem, porque su
   tamaño es variable.
6. **No hace falta cambiar el 2757**: no lleva números de instancia. Sí conviene:
   - sacarlo de la cola de movimiento (retardo de hasta 3,6 s);
   - usar `OtherUnequipItemAck` (3073) para quitar, como hacía el servidor.

**Verificación en vivo:**
- Antes de nada, copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save` en `backups/`.
- Leer con SEH, para el actor local y para la marioneta (`0x8A67E0` por clave):
  - `[[[a+0x68]+0x20]+0x70]` → u64 (characterNo);
  - `[[a+0x68]+0x38]+0x18` (serie);
  - el tamaño de `+0x90`.
- Hacerlo antes y después del spawn y de cada cambio.
- Con el arreglo 2 o 3:
  - no debe aparecer `ClientCacheReq` tras el spawn;
  - la serie local debe seguir al `EquipItemAck`;
  - quitar o poner la ranura 4 debe refrescar la UI al momento;
  - reintentar ya no debe dar `FailEquipItemAck`.

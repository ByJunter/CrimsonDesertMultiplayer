# Botín para el invitado

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA (`exe+…`)
y valen solo para esta build.

Método:
- análisis estático de solo lectura (capstone, `cddis.py`, RTTI);
- capturas del anfitrión (`bin64\cdmp_packets_*.bin`, sobre todo `20261006-172237`, con tres
  registros de cadáver completos) y del invitado en Shadow (logs del invitado del 5 y el 6 oct);
- el código actual de `enemy_relay.cpp` y `guest_hits.cpp`.

El juego no se ejecutó y no se tocó `src/`. Continúa
[pieza4-botin-reaccion.md](pieza4-botin-reaccion.md) §1,
[pendiente-recompensas-progreso.md](pendiente-recompensas-progreso.md) §4 (R6) y
[pendiente-3g-suprimir-enemigos-invitado.md](pendiente-3g-suprimir-enemigos-invitado.md).

Marcas:
- **[verificado]**: leído en el código;
- **[captura]**: medido en capturas (se da el script);
- **[inferido]**: deducción pendiente de prueba en vivo (§7).

Notación: **H** anfitrión, **G** invitado, **E** enemigo de H, **copia** = la copia de E en G
(`0xB01F0000+n`, solo existe en el cliente de G), **gemelo** = un actor del **servidor de G** que
hace de E para el botín. Jugador `0xA0100001` (en cada PC, el suyo).

Scripts en `build/research/p_botin_muerte/` (§9).

---

## 0. Resumen

1. **El botín vive en el cadáver y lo entrega el servidor que lo mató.** [captura + verificado]
   - Al morir, el servidor tira la lista de la variante (máscara 2) y la manda en el lote de la muerte
     (`LootDropCharacterRewardListAck`, `04 | u32 muerto | n × 37 B`).
   - Al **registrar** el cadáver (el jugador o su mascota), el cliente manda
     `InteractionPreemptionStepReq` + `InteractionDoStepReq` con la clave de interacción `0x000F428C`.
     El servidor responde en el mismo milisegundo con `AddItemToInventoryAck` (los objetos),
     `LootDropCharacterRewardListAck` vacío (5 B) y `RemoveLootableAck(muerto, jugador)`.
   - Todo depende de que el cadáver **exista en el servidor de quien registra**.
2. **Por qué la copia ofrece botín y no da nada:** la copia solo existe en el cliente de G. Su cliente
   la trata como cualquier cadáver y muestra el aviso, pero la petición nombra una clave que el
   servidor de G no tiene y no pasa nada [verificado el camino; inferido el silencio, como el resto de
   peticiones con claves de copia].
3. **Quitar el aviso falso, ya:** cuando muere una copia, inyectar en el cliente de G lo mismo que el
   servidor manda tras vaciar un cadáver:
   - `LootDropCharacterRewardListAck` = `04 | u32 copia` (5 B);
   - `RemoveLootableAck` = `u32 copia | u32 jugador propio` (8 B);

   y descartar las `Interaction*Req` C→S que nombren una copia. El handler de `RemoveLootableAck`
   (`0xBC4950 → 0xCB1410`) borra al jugador del conjunto de "quién puede registrar" del cadáver y
   refresca la interacción [verificado]. Que eso apague el aviso de la copia está por probar (L1).
   Reserva segura: retirar la copia (`RemoveActorAck`, como `puppet_remove_actor`) unos segundos
   después de terminar la animación de muerte.
4. **Botín de verdad para G: un "gemelo de botín" en el servidor de G** (recomendado):
   - cuando muere E y G participó, G pide a **su** servidor un actor de **la misma variante** en el
     sitio del cadáver de la copia (`SpawnCharacterCheatReq`, 21 B), lo quita de su cliente al
     llegar su carga (`RemoveActorAck` local) y lo mata en el acto (`KillCheatReq(gemelo, 0)`);
   - el servidor de G tira el botín de la variante **con sus propias reglas** y deja el cadáver
     "registrable" para G;
   - el mod hace de **alias**: la lista de botín y las respuestas que nombran al gemelo se reescriben a
     la copia (S→C), y la interacción de G con la copia se reescribe al gemelo (C→S). G registra "la
     copia" y recibe objetos nativos en su partida.
   - Con 3g ("ocultar y congelar" los nativos), el gemelo puede ser el **nativo equivalente** en vez
     de uno creado (R6). Así hasta se puede rematar por colisión y G recibe también la experiencia.
5. **Seguridad:** el invitado nunca recibe una lista de objetos del anfitrión. El anfitrión solo
   puede decir "la copia N ha muerto"; G decide, con datos propios, si participó y qué variante es
   aceptable, y **su** servidor tira el botín. Lo peor que puede hacer un anfitrión malicioso es elegir
   la variante de una copia: se acota con una lista permitida y con límites de ritmo (§6).
6. **Descartado o último recurso:**
   - inyectar en G el `LootDrop…` o el `AddItemToInventoryAck` de H: solo cambia la interfaz; la
     partida vive en el servidor de G;
   - `SpawnItemToGroundByCheatReq` / `CreateItemFromTrItemValueCheatReq` con la lista de H: formatos
     sin decodificar, riesgo de objetos inválidos en la partida, y obligan a validar cada objeto contra
     la tabla de botín de la variante (`dropsetinfo`);
   - `LootingDropFromDeadBodyReq` (3052) sobre la copia: el servidor de G no la tiene.

---

## 1. El botín de punta a punta [captura + verificado]

### 1.1 Al morir (servidor que mata)

Repaso de `pieza4-botin-reaccion.md` §1.2 [verificado]:

```
Status vf97 0x2942350 → 0x2942730 (tipo de actor, motivo ≠ 0, byte global 0x6D06E98)
   → 0x2AE6210 (suelta el equipo; [actor+0xA0] == 0)
   → LootDrop [actor+0x68]+0xC8 → 0x2B85E40 (modo 2, o 3 si motivo == 3)
   → 0x2B83960 (lista CharacterInfo+0x280, entradas {clave, máscara, 1}; destinatario = usuario del field)
   → 0x2B84E20 → escritor 0x29F2CD0 = LootDropCharacterRewardListAck (ToNear)
```

- Las variantes **con máscara 2** mandan la lista en el lote de la muerte (72 de 72 en
  `pieza4-botin-reaccion.md`). Ejemplo: `172237` t = 630,169, `B010107B`, 153 B.
- Las demás no mandan lista al morir, pero el cadáver **sí se puede registrar** (máscara 1, se tira
  al registrar): `172237` t = 376,992, `B0100AD7`, sin lista previa, da 381 B de
  `AddItemToInventoryAck`.

### 1.2 Al registrar el cadáver

`lt.py` (recuento) y `lt2.py 20261006-172237` (líneas de tiempo):

```
t = 653,100 (172237), cadáver B0100B59, muerto 4,28 s antes por el jugador (motivo 8, +10 XP)
 −1,479 C>S InteractionPreemptionStepReq    040010b0 590b10b0 8c420f00 00000000 00000000 01 00000000
 −1,476 S>C InteractionPreemptionStepSuccessAck 040010b0 | (los mismos 25 B)
 −0,089 C>S InteractionDoStepReq            040010b0 590b10b0 8c420f00 00000000
 −0,081 S>C InteractionDoStepSuccessAck     010010a0 | (los mismos 16 B)
 −0,001 S>C ApplyBuffAck                    590b10b0 13000000 … fuente 040010b0 …   (buff 19 en el cadáver)
 −0,001 S>C AddItemToInventoryAck           04 010010a0 …  (750 B: los objetos)
 −0,000 S>C LootDropCharacterRewardListAck  04 590b10b0     (5 B: lista vacía)
 +0,000 S>C RemoveLootableAck               590b10b0 010010a0
```

Formatos [captura]:

| Mensaje | Tamaño | Campos |
|---|---|---|
| `InteractionPreemptionStepReq` (2671) | 25 B | `u32 quien interactúa | u32 objetivo | u32 clave de interacción | u32 0 | u32 0 | u8 1 | u32 0` |
| `InteractionPreemptionStepSuccessAck` (2663) | 29 B | `u32 quien | (los 25 B)` |
| `InteractionDoStepReq` (2720) | 16 B | `u32 quien | u32 objetivo | u32 clave | u32 0` |
| `InteractionDoStepSuccessAck` (2916) | 20 B | `u32 jugador | (los 16 B)` |
| `LootDropCharacterRewardListAck` (2897) | 5 + 37·n B | `u8 4 | u32 muerto | n × {u8, u64, u64, u64, u64, u32}` (handler `0xBC4C20`) |
| `RemoveLootableAck` (2822) | 8 B | `u32 muerto | u32 jugador` (handler `0xBC4950`) |

- **Quién interactúa:** en las tres de `172237` es `0xB0100004`, la **mascota** del jugador ("Brown
  Dog", tipo 6, dueño el usuario; `k4.py`). Al montar a caballo, es el propio jugador `0xA0100001`.
  En Shadow (`20261005-202422` t = 1191,162) también registra la mascota de G.
- **Clave de interacción:** `0x000F428C` = registrar un cadáver (las tres de H y la de G). En G
  aparece también `0x000F4274` sobre otro actor (`B01009BA`).
- **Recuento** en todas las capturas: `InteractionDoStepReq` 229, `AddItemToInventoryAck` 431,
  `LootDropCharacterRewardListAck` 165, `RemoveLootableAck` 30. **`LootingDropFromDeadBodyReq`: 0**
  (el juego registra por interacción, no con esa petición).
- **Quién decide:** el servidor del que registra. El cliente solo pide la interacción.

### 1.3 Lo que guarda el cliente [verificado]

`ClientLootDropActorComponent` (vtable `0x55BE5D8`, mismo layout que `CommonLootDropActorComponent`
`0x5915380`), en `[actor+0x68]+0xC8`:

| Campo | Qué es | Quién lo escribe |
|---|---|---|
| `+0x18` | Conjunto de claves de jugador (entradas de 32 B; clave en `+8`, puntero en `+0x18`) | La carga del actor (vf11 `0x2145330`: `u32 n`, `n × u32`); `RemoveLootableAck` lo borra (`0xCB1410` → `0x411310`) |
| `+0x38` / `+0x40` | Lista de entradas de `0xC8` B | La carga |
| `+0x48` / `+0x50` | Recuentos por objeto (`0x2145B00`: clave, u16, i64) | Carga y `VaryLootDropSubItemCountAck` |
| `+0x58` / `+0x60` | **Lista de botín** | La carga (`0x2145FE0`) y `LootDropCharacterRewardListAck` (`0x9DA2B0`) |
| `+0x6C` | u8 "lista recibida" | `0x9DA2B0` pone 1 |

- Ambos handlers llaman después a `0x50A260(actor)` (refresco de la interacción y del icono de mapa
  `MapIcon_ActorDeadBodyLootable`).
- Las condiciones de la interacción (`ConditionData_IsLootable`, `ConditionData_HasLootItem`,
  `ConditionData_IsPetLooting`; vtables `0x5950858`, `0x59506E8`…) evalúan en `.xtls` (virtualizado):
  no se pudo ver qué campo miran [inferido: el conjunto `+0x18` y la lista `+0x58`].

### 1.4 Servidor: registrar por petición directa [verificado]

`LootingDropFromDeadBodyReq` (3052, 6 B: `u32 muerto | u8 | u8`): `0x2A116B0` busca al muerto con
`0x2A827B0` sobre `[exe+0x6D69698]` y llama a `0x2B81410(LootDrop del muerto, …, actor de la sesión,
u8, u8)`. Acepta como "quien registra" una mascota o montura y sube a su dueño (`[actor+0x88]+1` ∈
{4, 5, 6}). Nunca aparece en las capturas: no se sabe qué son los dos `u8`. **Exige un cadáver en ese
servidor.**

---

## 2. Qué pasa hoy con la copia en G

- La copia la carga `enemy_relay` con la `ActorLoadDataAck` de H traducida. Su muerte llega como
  `NotifyHandleDeadAck` inyectado y su animación con los `StartActionReq` de H.
- El servidor de G **no tiene** ese actor. Ni la lista de botín de H ni sus `RemoveLootableAck` se
  relevan (`kRelayedAcks` no los incluye) [verificado en `enemy_relay.cpp`].
- El cliente de G ve un cadáver humano normal y ofrece "registrar". La interacción sale hacia el
  servidor de G con la clave de la copia y no ocurre nada [inferido].
- **Las capturas de Shadow no tienen ningún intento** sobre una copia (`gi.py` sobre las tres: 0
  `Interaction*` con claves `0xB01F…`), así que el detalle de la respuesta (Nak, nada o
  `InteractionPreemptionStepFailAck`) no está medido.
- La **mascota de G** también puede intentar registrar copias por su cuenta (`IsPetLooting`).

---

## 3. Quitar el aviso falso (inmediato)

En G, en el mismo punto donde `enemy_on_state` inyecta el `NotifyHandleDeadAck` de una copia, y
**solo si no hay gemelo de botín para ella** (§4):

1. ~1 s después de la muerte (que la animación ya haya empezado), `puppet_inject_s2c` de:
   ```
   LootDropCharacterRewardListAck (2897), 5 B:  04 | u32 copia
   RemoveLootableAck             (2822), 8 B:  u32 copia | u32 jugador propio
   ```
   Es exactamente lo que recibe H al vaciar un cadáver (§1.2).
2. Descartar en C→S cualquier `InteractionPreemptionStepReq`/`InteractionDoStepReq` cuyo objetivo
   (`+4`) sea una copia (`is_enemy_copy_key`). Hoy van al servidor de G y no hacen nada, pero dejan al
   jugador o a la mascota esperando una respuesta.
3. **Si el aviso sigue** (L1 negativa): retirar la copia muerta del cliente de G con `RemoveActorAck`
   (`puppet_remove_actor` + `note_removed`) a los ~4 s de la muerte, cuando la animación ha terminado.
   Es lo que hace el juego con los cadáveres lejanos; el cadáver de H sigue en su mundo.

Riesgo: bajo. Los dos Ack llevan claves que solo existen en el cliente de G. El handler de
`RemoveLootableAck` busca el actor (`0x8A67E0`) y `0xCB1410` no hace nada si no hay actor o si no
tiene componente LootDrop (`0xCB1422`, `0xCB1436`) [verificado].

---

## 4. Opciones para que G reciba botín de verdad

| | Qué | Botín | XP / crédito para G | Depende de | Riesgo para la partida | Esfuerzo |
|---|---|---|---|---|---|---|
| **G1 (recomendada)** | **Gemelo de botín creado**: `SpawnCharacterCheatReq(variante, pos)` en el servidor de G → quitarlo del cliente → `KillCheatReq(gemelo, 0)` → alias copia ↔ gemelo en la interacción y en los Ack de botín | **Nativo**: el del servidor de G para esa variante (máscara 2 al morir, máscara 1 al registrar) | No (motivo 16) | El formato de `SpawnCharacterCheatReq` (21 B, sin capturas) | **Bajo**: objetos creados por el propio servidor; el actor es dinámico | Medio |
| G2 | **Gemelo nativo (R6)**: el nativo de G de la misma variante y punto de aparición, oculto y congelado por 3g; se mueve al sitio y se remata | Nativo | **Sí** si se remata por colisión (necesita el actor en el cliente; con 3g no está) | 3g y el emparejamiento (70-96 % por origen; 2 % en nodos de facción) | Bajo; mata un nativo de verdad (cuenta para su reaparición) | Alto |
| G3 | `LootingDropFromDeadBodyReq(gemelo, a, b)` directo en vez del alias de la interacción | Nativo | — | Un gemelo (G1 o G2) y descifrar `a`, `b` | Bajo | Bajo, pero con pruebas |
| G4 | Objetos por truco: `SpawnItemToGroundByCheatReq` (2433, `0x2A03090 → 0x2C445D0`) o `CreateItemFromTrItemValueCheatReq` (2583) con la lista de H, validada contra `dropsetinfo` | Solo lo que H dice (validado) | No | Decodificar los formatos sin capturas | **Alto**: un `TrItemValue` mal hecho queda en la partida | Alto |
| ✗ | Inyectar en G el `LootDrop…`/`AddItemToInventoryAck` de H | Ninguno (solo interfaz) | No | — | Desincroniza la interfaz | — |

### 4.1 G1 paso a paso

**H** (sin cambios grandes): el `NotifyHandleDeadAck` relevado ya identifica la copia. Añadir al
evento de muerte la posición final de E (ya se envía su posición por `EnemyPos`).

**G**, al recibir la muerte de la copia `C` (variante `V` = `u32` en `+0x18` de su carga, la misma
que usa `check_spawn`):

1. **Elegible** (§6): G participó, `V` está en la lista permitida, una sola vez por `C`, dentro de los
   límites de ritmo. Si no, §3 (quitar el aviso) y fin.
2. **Crear el gemelo:** C→S `SpawnCharacterCheatReq` (2115), 21 B:
   ```
   u32 V | u32 ? | f32×3 posición del cadáver de C en G | u8 ?
   ```
   - Deserializador `0x29BE8C0` (`u32`, `u32`, 12 B, `u8`), núcleo `0x2C3ED50` [verificado el orden de
     lectura].
   - `V == 0` → Nak `0x3F5`. Mismo permiso que `KillCheatReq`: `[[actor+0x68]+0x130]` vf `0x140(0)`,
     que ya pasa (X4, L1).
   - `V` se busca en la tabla estática de `[exe+0x6D69A48]` (CharacterInfo); si no está, Nak.
   - El significado del segundo `u32` y del `u8` no está leído (¿número, motivo de aparición?)
     [inferido; prueba L2].
3. **Ocultarlo:** la primera `ActorLoadDataAck` de tipo 3 con variante `V` a menos de 3 m de esa
   posición en ≤ 2 s es el gemelo `T`. Guardar `T ↔ C` y, en el siguiente tick, `RemoveActorAck(T)`
   inyectado en el cliente de G. Para el servidor de G queda "oculto y congelado" (3g §0.3: la IA la
   corre el cliente). Mejor aún, con el filtro de cargas de 3g, no dejarlo entrar.
4. **Matarlo:** C→S `KillCheatReq` = `u32 T | u32 0` (opción 0: muerte inmediata, `+0x4F = 1`). El
   servidor de G tira el botín de `V` (motivo 16 ≠ 0/3 da el modo 2) y lo manda ToNear.
5. **Alias S→C** (gancho de recepción, sustituir la clave antes de que el juego lo procese):

   | Mensaje | Campo con `T` → `C` |
   |---|---|
   | `LootDropCharacterRewardListAck` | `+1` |
   | `RemoveLootableAck` | `+0` |
   | `InteractionPreemptionStepSuccessAck` | `+8` |
   | `InteractionDoStepSuccessAck` | `+8` |
   | `ApplyBuffAck` (buff 19 del registro) | `+0`, `+0x1C` (o descartarlo) |
   | `NotifyHandleDeadAck`, `VaryHPAck` de `T` | descartar (la copia ya está muerta) |

6. **Alias C→S:** `InteractionPreemptionStepReq` e `InteractionDoStepReq` con objetivo `+4 = C` →
   `+4 = T`. Como el gancho de envío solo observa, se descarta la original y se manda la reescrita con
   `sniffer_inject_c2s`.
7. **Limpieza:** al registrar (`RemoveLootableAck(T)`) o al retirarse la copia, olvidar el alias.

Resultado esperado: G registra la copia, ve la lista de su servidor y recibe
`AddItemToInventoryAck` nativo. La distancia que valida el servidor es la del registrador al gemelo,
que está donde está la copia.

### 4.2 G2 (cuando exista 3g)

Igual que G1, cambiando los pasos 2-4 por el gemelo nativo que 3g haya ocultado
(`pendiente-recompensas-progreso.md` §4.A y `pendiente-3g…` §0.5):
- moverlo con `MoveActorReq` a la posición del cadáver;
- `VaryStatCheatReq` a 1 HP y `AttackByCollisionReq` con el jugador de G como dueño (con la flecha de
  `muerte-realista.md` §3.1), si el gemelo está en el cliente; si no, `KillCheatReq`.

Da además la experiencia y el `KillActor` a G solo si el remate por colisión impacta, y eso exige
que el gemelo exista en el cliente de G (con 3g no existe). Por eso G1 es más sencilla para el botín.

---

## 5. No romper partidas

- **Los objetos los crea el servidor de G** con sus tablas, igual que al registrar cualquier cadáver:
  nada de `TrItemValue` hechos a mano.
- **El gemelo es un actor dinámico** creado por truco y muerto en el acto. Lo normal es que no se
  guarde y que el recolector (`ActorGarbageCollectionRepeatTimer`) se lleve el cadáver [inferido;
  prueba L5: guardar, salir y cargar].
- **Objetos de misión:** la lista puede traer `Quest_Paper_*` (`pendiente-recompensas-progreso.md`
  §1.1). El servidor de G los da con sus reglas, igual que si G matara un bandido de esa familia.
- **Telemetría:** el motivo 16 (`DevCheat`) solo se envía cuando muere **el jugador**
  (`pendiente-recompensas-progreso.md` §2.6), no con enemigos.
- **No se mata a ningún nativo de G** en G1: no cambian ni contadores de campamento ni reapariciones.
- **H no cambia:** su botín sigue saliendo para él, en su mundo. Cada jugador tiene el suyo, como en
  un cooperativo con botín por jugador.

---

## 6. Seguridad: botín solo por muertes reales en las que G participó

Todo lo decide G con datos propios. H solo puede afirmar "la copia `C` murió".

| Regla | Dato (propio de G) |
|---|---|
| La copia existe y está muerta en G | `g_copies` y el `NotifyHandleDeadAck` relevado |
| **G participó** | Al menos un golpe propio sobre `C` en los últimos 30 s: un `DoFrameEventReq` C→S de nuestro jugador con `C` en su lista (`guest_hits_observe_c2s` ya lo detecta para mandar el `GuestHit`) |
| G estaba cerca | Nuestro jugador a ≤ 30 m del cadáver de `C` al morir |
| Variante aceptable | `V` hostil según la tabla de facciones de 3g (§2.1), sin jefes, élites, únicos ni actores de escenario o misión (los mismos criterios de supresión de 3g); y `V` debe ser la de la carga de `C` (H no puede cambiarla después) |
| Una vez por copia | Marca por `C` (y por su `net_id`/`gen`) |
| Ritmo | ≤ 1 gemelo cada 2 s, ≤ 30 cada 5 min, ≤ 16 alias vivos |
| Sin participación | Solo §3 (quitar el aviso) |

- Lo que H **no** puede hacer: elegir objetos, cantidades ni rarezas (los tira el servidor de G), ni
  provocar botín sin que G haya golpeado.
- Lo que H **sí** podría hacer, siendo malicioso: mandar copias de una variante con buen botín y
  dejar que G las mate. Lo acota la lista permitida (`V` normal de mundo) y el ritmo; es equivalente a
  que G mate esos enemigos en su propio mundo.
- El gemelo se crea en la posición de **la copia en G** (leída de memoria en G), no en una posición
  que mande H.

---

## 7. Pruebas en vivo

Un PC, `cdmp_peer` en eco (las copias existen en eco). Antes, **copia de la partida** en `backups/`;
en las pruebas que crean objetos (L2-L5), salir **sin guardar** salvo en L5.

| # | Prueba | Predicción | Decide |
|---|---|---|---|
| **L1** | Matar una copia (eco) y, ~1 s después, inyectar `LootDrop…` vacío + `RemoveLootableAck(copia, propio)`. Captura de pantalla con `CopyFromScreen` del aviso antes y después | El aviso "registrar" desaparece de la copia | §3 (si no: retirar la copia) |
| L1b | Registrar una copia sin L1 | C→S `Interaction*Req` con la clave de la copia; anotar la respuesta del servidor (¿`InteractionPreemptionStepFailAck`, Nak, nada?) | §2 |
| **L2** | `SpawnCharacterCheatReq(54121, 1, pos a 5 m delante, 0)` lejos de otros enemigos | `ActorLoadDataAck` de tipo 3, variante 54121, en esa posición; sin Nak. Probar `u32 = 0/1` y `u8 = 0/1` si no sale | Formato de G1 paso 2 |
| **L3** | L2 + `RemoveActorAck` local + `KillCheatReq(T, 0)` | `NotifyHandleDeadAck(T, A0100001, 0x10)` y `LootDropCharacterRewardListAck(T, …)` (54121 tiene máscara 2); nada visible | G1 pasos 3-4 |
| **L4** | L3 + alias: registrar la copia muerta | C→S reescritos con `T`; S→C `AddItemToInventoryAck` con objetos de 54121, `RemoveLootableAck(T→C)`; el inventario sube | G1 completo |
| L5 | L4, guardar, salir y cargar | Los objetos siguen; no aparece ningún cadáver ni bandido nuevo en ese sitio | Partida sana |
| L6 | Dos PC: G golpea y H remata; luego G sin golpear | Botín en G solo en el primer caso | Reglas de §6 |

---

## 8. Preguntas abiertas

1. ¿Qué mira exactamente el cliente para ofrecer "registrar" (`+0x18`, `+0x58`, `+0x6C` o solo
   "muerto + `_isLootableDeadBody`")? Lo decide L1.
2. ¿Qué son el segundo `u32` y el `u8` de `SpawnCharacterCheatReq`? (L2)
3. ¿El servidor guarda en la partida los actores creados con `SpawnCharacterCheatReq`? (L5)
4. ¿Qué son los dos `u8` de `LootingDropFromDeadBodyReq`? Solo para G3.
5. La mascota de G: ¿intenta registrar copias por su cuenta? Si lo hace, el descarte de §3.2 lo
   cubre, y con el alias de G1 la mascota registraría el gemelo como hace con cualquier cadáver.

---

## 9. Scripts, direcciones y opcodes (build 2976)

Scripts en `build/research/p_botin_muerte/` (con `build/research/venv/Scripts/python.exe`; usan
`build/research/pieza4/pk.py`):

| Script | Qué hace |
|---|---|
| `lt.py` | Recuento de los mensajes de botín e interacción en todas las capturas |
| `lt2.py <sesión> [antes] [después]` | Línea de tiempo alrededor de cada `RemoveLootableAck` |
| `gi.py <captura>` | Interacciones y botín de una captura cualquiera (las de Shadow), marcando claves de copia |
| `k4.py <sesión> <clave>` | Registro de carga de un actor (tipo, dueño, variante, nombre) |
| `ld.py <sesión>` | Contextos de la clave del jugador dentro de las cargas de NPC |
| `vtl.py <texto> [n]` | Vtables RTTI que contienen un nombre, con sus funciones |

| RVA | Qué es |
|---|---|
| `0xBC4950` → `0xCB1410` | Cliente: `RemoveLootableAck` → borra al jugador del conjunto `+0x18` del LootDrop y refresca (`0x50A260`) |
| `0xBC4C20` → `0x9DA2B0` | Cliente: `LootDropCharacterRewardListAck` → lista `+0x58`/`+0x60`, `+0x6C = 1`, refresco |
| `0x55BE5D8` / `0x5915380` / `0x5B2BFA8` | Vtables `ClientLootDropActorComponent` / `CommonLootDropActorComponent` / `ServerLootDropActorComponent` |
| `0x2145330` | LootDrop vf11: deserializa la parte del componente en la carga del actor |
| `0x2251EE0`, `0x2251FD0` / `0x2251C70`, `0x2251DA0` | `ConditionData_IsLootable` / `ConditionData_HasLootItem` (evaluación en `.xtls`) |
| `0x2A116B0` → `0x2B81410` | Servidor: `LootingDropFromDeadBodyReq` (`u32 muerto | u8 | u8`), acepta mascota o montura |
| `0x29E8FA0` / `0x29E9490` | Servidor: `InteractionPreemptionStepReq` / `InteractionDoStepReq` |
| `0x29BE8C0` → `0x2C3ED50` | Servidor: `SpawnCharacterCheatReq` (`u32 | u32 | vec3 | u8`), permiso vf `0x140(0)`, tabla `[0x6D69A48]` |
| `0x2A33FA0` → `0x2BA2260` | `FireAndPushCharacterRewardToInventoryReq` (`u64 | u8 | u8`, componente `+0x110` del jugador): de mercenarios, no sirve |
| `0x2B85E40` / `0x2B83960` / `0x29F2CD0` | Servidor: botín al morir / lista por máscara / escritor de la lista |

| Opcode | Mensaje |
|---|---|
| 2897 | `LootDropCharacterRewardListAck` |
| 2822 | `RemoveLootableAck` |
| 2962 | `AddItemToInventoryAck` |
| 2671 / 2663 | `InteractionPreemptionStepReq` / `…SuccessAck` (3000 = `…FailAck`) |
| 2720 / 2916 | `InteractionDoStepReq` / `…SuccessAck` |
| 3052 | `LootingDropFromDeadBodyReq` |
| 2115 | `SpawnCharacterCheatReq` |
| 2279 | `KillCheatReq` |
| 2516 | `RemoveActorAck` |

---

## Formato de SpawnCharacterCheatReq y alternativas

Build **1.0.0.2976**. Análisis estático (capstone) y capturas ya grabadas; el juego no se ejecutó. Volcados y
scripts nuevos en `build/research/p_botin_muerte/` (lista en §H). Marcas como en el resto del informe.

### A. `SpawnCharacterCheatReq` (2115), 21 B [verificado]

```
+0x00 u32   variante (clave de characterinfo, p. ej. 54121)
+0x04 u32   SE LEE Y SE DESCARTA (no llega al núcleo): poner 0
+0x08 f32×3 posición en el mundo (la del cadáver de la copia en G)
+0x14 u8    motivo de aparición (eSpawnReason, desc+0xA): usar 33 (0x21) = "Cheat"
```

- **Deserializador `0x29BE8C0`** (`s_29be8c0.asm`): lee `u32 → [rbp+0x7F]`, `u32 → [rbp-0x39]`,
  12 B → `[rbp-0x11]`, `u8 → [rbp+0x77]`. Exige haber consumido exactamente la longitud de la cabecera
  (`0x29BEA30`); si no, error `[0x6CDB970]` sin procesar. Llama a
  `0x2C3ED50(handler, paquete, &variante, r9 = basura, &pos, &u8)`: el segundo `u32` **no se pasa**
  (`[rbp-0x39]` no vuelve a leerse). World Builder lo llama "unused": coincide.
- **El `u8` es el motivo de aparición.** `0x2C3F106`: `r8b = u8; if (u8 == 0x28) r8b = 0x21;` y
  `0x278EA70` (ctor de `ICreateServerActorDesc`) lo guarda en `desc+0xA`. Enum sacado de su registro de
  reflexión (`0x15BA3F0`, `enum_spawnreason.txt`; capacidad 0x29):

  | Valor | Nombre | Valor | Nombre |
  |---|---|---|---|
  | 0 | Level | 20 | PortalGimmick |
  | 1 | Sequencer | 21 | DropItem |
  | 2-11 | AutoSpawn_Bird, _SidWalk, _Wagon, _TerrainRegion, _SpawningPool_Near, _SpawningPool_Socket, _SpawningPool_Point, _MeshGroup, _FactionPatrol, _FactionPosition | 22 | DiscardItemFromInventory |
  | 12 | NPCSchedule | 23 | Housing |
  | 13 | Summon | 24 | Craft |
  | 14 | SummonMercenary | 25 | ThrowEquip |
  | 15 | SummonGimmick | 26 | DropFromDeadBody |
  | 16 | CombinationGimmick | 27 | DropFromDeadBodyVehicle |
  | 17 | EquipDockingGimmick | 28-32 | Transmutation, Inspect, GlobalGameActor, SceneCollectSpawn, Summon_LinkedVehicle |
  | 18 | InstallationGimmick | **33** | **Cheat** |
  | 19 | GuideEffectGimmick | 34-39 | Editor_GimmickEventExecutable, Editor_EquipDockingGimmickEventExecutable, Editor_AttackDebugAttackTarget, Editor_AttackDebugAttacker, AutoSpawn_SpawningPool_ActionPoint, DailyRoutine |

  - 40 (0x28) es el centinela "número de valores": el servidor lo convierte en 33 (Cheat). Es, con toda
    probabilidad, lo que manda la interfaz de trucos de desarrollo [inferido]. **Valores válidos: 0-39;
    usar 33.**
  - `SpawnCharacterForActionTestReq` (3058) fija 37 (`Editor_AttackDebugAttacker`). Los demás creadores
    del juego usan 1, 10, 12, 13, 14, 16, 22, 30, 31, 32 y 39 (`reasons.txt`).
  - No se encontró código que trate el 33 de forma especial (lecturas de `desc+0xA` en `0x2AF9F40` y sus
    llamadas a 3 niveles, `xgrep.py`). Su efecto real (filtros de `ServerField::vf17`, guardado) queda
    [inferido].
- **Enum de motivos de muerte** (de paso; `0x15BA200`, `enum_deadreason.txt`): 0 SpawnAsDead,
  1 SummonerDead, 2 InstantFallDeath, 3 Freeze, 4 Lava, 5 Swamp, 6 Drowning, 7 Falling, **8 BuffDamage**,
  9 Barricade, 10 HitByObject, 11 Fire, 12 Shock, 13 Sequencer, 14 KillSelf, **15 Fatal**,
  **16 DevCheat**, 17 Dune, 18 Pressed.
  - Corrige las etiquetas de §1.2 y de `pieza4-dano-servidor.md` §1.4: el 8 de una muerte normal es
    "daño por buff" (el daño de los ataques es un buff), no "matado por otro"; 6 y 7 son ahogado y caída.

### B. Núcleo `0x2C3ED50` paso a paso [verificado, `s_2c3ed50.asm`]

1. `variante == 0` → Nak con error `[0x6CDB978]`.
2. Personaje de la sesión = `ServerChildOnlyInGameActor::vf44` (`0x50E6B0`: `[actor+0xA0]`, el mismo que
   usa `KillCheatReq`). `0x206AE30` lo engancha a su field (`FieldAttacher`); si no está en un field →
   Nak `[0x6CF6EBC]`.
3. **Permiso:** `[[personaje+0x68]+0x130] → vf(0x140)(0)` (tipo 0; `KillCheatReq` usa el 2 y
   `VaryStatCheatReq` el 3). Es el mismo componente y la misma función virtual que ya pasan en vivo (X4).
   La implementación conocida, `ServerContentsMiscActorComponent::vf40` = `0x3600C0` (`mov al, 1; ret`),
   ignora el tipo → **pasa también con el 0** [inferido alto]. Si falla → Nak `[0x6CF7A04]`.
4. Variante en la tabla de `CharacterInfo` (`[0x6D69A48]`, hash en `+0x68/+0x78/+0x80`, índice u16 en
   `+8`); si no está → Nak `[0x6CF6BC8]`.
5. `desc` en la pila (0x4E8 B):
   - motivo en `+0xA`, índice en `+8`;
   - posición con `0x2797080(desc, &pos, cuaternión identidad 0x6A65908)`;
   - field en `+0x3C0` (el del personaje);
   - `+0x3B8` = actor de la sesión si es de tipo 1. Es el mismo patrón que todos los creadores del juego
     ("jugador que provocó la aparición", `descfields.py`);
   - si `CharacterInfo+0x68 == 4` (tipo mascota/montura), `desc+0x40` = personaje del jugador → **dueño**.
     Los bandidos son tipo 3: sin dueño. **No pedir nunca una variante que no sea de tipo 3**: se crearía
     una mascota o montura del jugador, que sí podría ir a la partida.
6. `desc.vf4` (`0x278F3A0 → .xtls 0x100E00F0`, `x_278f3a0.txt`): solo comprueba índice ≠ 0xFFFF y el
   transform. **No hay límite de distancia** al jugador ni comprobación de terreno.
7. `ServerField::vf17` (`[field]+0x88` = `0x2AF9AE0`) con una lista de un desc → `0x2AF9F40`
   (`.xtls 0x10649310`): crea el actor **en la misma llamada** (`0x2AFA110`, `0x2793000`, `0x2AFE200`…).
   Si hay error → Nak con ese código (y retira lo creado, `0x2AF9DCB`).
8. **Éxito: no hay Ack.** Lo único que ve el cliente es la `ActorLoadDataAck` del actor nuevo, cuando el
   sistema de cercanía se la manda. World Builder la vio llegar **5-7 s después** [M]. La clave es dinámica
   (`0xB010xxxx`, la siguiente del contador del servidor) y solo se conoce por esa carga.

**Nak** (1013), 17 B **sin cabecera de longitud** [captura, 7 en `D:\cdmp-grabaciones`]:
`F5 03 | FF | u32 código de error | u16 opcode de la Req (0x0843 para 2115) | u64 0`.
- Que se mande depende de la tabla de errores (`0x14276D0`/`0x1427C10`: tipo 1 = Nak, tipo 2 = silencio).
- Los códigos son globales que se rellenan al arrancar (no se pueden leer en disco).

### C. IA, guardado y botín

- **IA:** la de un NPC la ejecuta el cliente que lo controla (3g §0.3; `ChangeAIStateReq`/`StartActionReq`
  son C→S). Si el cliente de G **nunca recibe la carga** del gemelo (descartar la `ActorLoadDataAck` en el
  mid-hook de `0x35CA00`, como en la fase 5), el gemelo queda quieto, sin objetivo y sin atacar. Tampoco lo
  ven los NPC de G, porque su IA también corre en el cliente [inferido alto].
- **Guardado:** los NPC se guardan en listas propias de cada sistema (`FieldNPCSaveData` con
  `_fieldNpcSaveDataKey`, `FieldGimmickSaveData_AutoSpawnChildList` del gimmick dueño,
  `FactionPatrolCharacterSaveData`, `NPCScheduleCharacterSaveData`…).
  - Un actor de motivo Cheat no tiene clave de guardado ni dueño de aparición; además muere en el acto y
    lo retira el recolector.
  - La advertencia de World Builder ("lo creado puede autoguardarse") era sobre **gimmicks** con UUID de
    escena.
  - Riesgo bajo [inferido; lo decide L5].
- **Muerte y botín con `KillCheatReq(T, 0)`** [verificado + captura]:
  - `0x2C40EBA`-`0x2C40ECB`: `registro+0x4F = (opción == 0)`. **Opción 0 = muerte inmediata.** Con otra
    opción queda "fatal" (0 HP) esperando un `DieByRunOutFatalReq` que manda **el cliente**; con el gemelo
    descargado no llegaría nunca → **usar siempre 0**.
  - Asesino = personaje de la sesión, motivo 16 (DevCheat), tipo 0xA.
  - Las 14 `KillCheatReq` de las capturas (`kc.py`): `VaryHPAck` + `NotifyHandleDeadAck(víctima, A0100001,
    0x10)` en 1-17 ms. Con variantes de máscara 2, `LootDropCharacterRewardListAck` en el mismo lote
    (`173434` t = 187,5 `B010016B`, 153 B; `114648` t = 404,9 `B01001A7`, 42 B). A veces, una carga de
    tipo 7 (el arma que suelta, variante `0x11171`).
  - Ninguna de esas víctimas se registró después: que un cadáver de `KillCheatReq` sea registrable está
    [inferido]. El destinatario del botín es "el usuario del field", no el asesino (§1.1). Lo deciden L3/L4.
  - Sin experiencia ni crédito de misión (`pendiente-recompensas-progreso.md`); aquí es lo deseable.
- **Alternativas peores:**
  - `VaryStatCheatReq` a 0 → "fatal" → `DieByRunOutFatalReq` (motivo 15): da botín, pero son dos pasos y
    el segundo lo tendríamos que inyectar nosotros;
  - `AttackByCollisionReq` daría experiencia, pero exige el gemelo cargado en el cliente.

### D. ¿Botín sin gemelo? No [verificado, `up.py`]

Todo lo C→S que llega a las funciones de botín:

| Función | Llegan desde |
|---|---|
| `0x2B85E40` (tirada al morir) | Solo la muerte (`0x2AE6210`) |
| `0x2B83960` (lista por máscara) | La muerte y `ServerFrameEventDrop::vf1` `0x27DB1E0` (frame event "Drop" de una acción de un actor que existe) |
| `0x2B81410` (registrar) | `LootingDropFromDeadBodyReq` (3052), `UseItemReq` (2855), `UseItemReserveSlotReq` (2776), `DiscardItemReq` (2725) y los temporizadores `ProcessLootingDeadDropOnceTimer` (2024) y `PushCharacterToInventoryOnceTimer` (2048) |
| `0x2B84E20` (enviar lista) | Además `RewardAfterRelationalDialogReq` (2415), `MiniGameResultReq` (2134), `PushRewardToPlayerOnceTimer` y `StealItemByFrameEventOnceTimer` |

Todas exigen un actor en el servidor (cadáver, objeto o diálogo), o usan tablas de recompensa que no son la
de la variante. **No hay ninguna Req que tire la tabla de una variante en un punto: el gemelo es
necesario.**

### E. Interacción y Ack de botín [verificado + captura]

`InteractionPreemptionStepReq` (2671): `0x29E8FA0` lee `+0 u32 quien`, `+4 u32 objetivo`, `+8 u32 clave`,
`+0xC u32`, `+0x10 u32`, `+0x14 u8`, `+0x15 u32` y llama a `0x2B36860` (`.xtls 0x1067B660`) sobre el
componente de interacción de **quien** (`[actor+0x68]+0x98`):
- busca el field de quien y la `InteractionInfo` de la clave (`[0x6D69AB8]`);
- busca **el objetivo en ese field** (`0x2A827B0`). **Si no existe → `InteractionPreemptionStepFailAck`
  (3000, escritor `0x29E7D10`) con error 0**;
- si existe → `0x2B36EF0` (condiciones comunes `0x2124060`, `0x211E2A0`…; la distancia, si se mira, está
  ahí o en `.xtls`, sin decodificar) → `SuccessAck` o `FailAck` con el código.

`InteractionDoStepReq` (2720): `0x29E9490 → .xtls 0x104A8D90 → 0x2C44080`: busca a quien y llama a
`0x2B36B30`; si hay error → **Nak** (no `FailAck`). El registro llega después por el temporizador
`ProcessLootingDeadDropOnceTimer` (`0x2C31430 → 0x2B81410`), que **no mira distancias** y sube de mascota o
montura a su dueño.

**Hallazgo en capturas** (`ifail.py`): hay **44 `FailAck`**, todas de la **mascota de H** (`B0100004`)
intentando registrar **copias** (`B01F0002/3/7`) cada ~2,8 s, sin fin (`20261004-090931`,
`20261005-071235`, `20261006-172237` t = 443-477).
- Responde a L1b y a §8.5: el servidor contesta `FailAck` y el cliente reintenta.
- El descarte de §3.2 hace falta ya, también en H con copias en eco.

Formatos (las claves de actor, en negrita):

| Mensaje | B | Formato |
|---|---|---|
| `InteractionPreemptionStepReq` 2671 C→S | 25 | **`u32 quien`** · **`u32 objetivo`** · `u32 clave` (`0x000F428C` = registrar) · `u32 0` · `u32 0` · `u8 1` · `u32 0` |
| `…SuccessAck` 2663 S→C | 29 | **`u32 quien`** · los 25 B (objetivo en **+8**) |
| `…FailAck` 3000 S→C | 23 | **`u32 quien`** · **`u32 objetivo`** · `u32 clave` · `u32 error` (0 = no existe) · `u32 0xFFFFFFFF` · `u8 0` · `u8 1` · `u8 0` |
| `InteractionDoStepReq` 2720 C→S | 16 | **`u32 quien`** · **`u32 objetivo`** · `u32 clave` · `u32 0` |
| `InteractionDoStepSuccessAck` 2916 S→C | 20 | **`u32 jugador`** (aunque registre la mascota) · los 16 B (objetivo en **+8**) |
| `LootDropCharacterRewardListAck` 2897 S→C | 5 + 37·n | `u8 4` · **`u32 muerto`** · n × 37 B (`u8 4 · u64 objeto · u64 cantidad · u64 · u64 · u32`). Sin contador (n sale de la longitud). **Sin claves de actor** en las entradas (165 de 165) |
| `RemoveLootableAck` 2822 S→C | 8 | **`u32 muerto`** · **`u32 jugador`** |
| `KillCheatReq` 2279 C→S | 8 | **`u32 víctima`** · `u32 opción` (0) |
| `SpawnCharacterCheatReq` 2115 C→S | 21 | `u32 variante` · `u32 0` · `f32 x, y, z` · `u8 33` |

Distancia: si el servidor de G la mide, es entre la posición **en su servidor** de quien registra (el
jugador o la mascota de G) y el gemelo. Con el gemelo en el punto del cadáver de la copia, es la misma
distancia que ve el jugador [inferido; las condiciones están en `.xtls`].

### F. Secuencia recomendada (G1 corregida)

1. Muerte de la copia `C` relevada y elegible (§6). Variante `V` = `u32` en `+0x18` de la carga de `C`;
   **comprobar `load[2] == 3`** (tipo enemigo).
2. C→S `SpawnCharacterCheatReq(V, 0, pos_C, 33)` con `sniffer_inject_c2s`. Apuntar `(V, pos_C, t)`.
3. S→C: la primera `ActorLoadDataAck` con `load[2] == 3`, `+0x18 == V`, `+7 == 0`, clave no vista, a ≤ 3 m
   de `pos_C` y en ≤ 15 s es el gemelo `T`.
   - Posición: 28 B antes del marcador `1.0, 1.0, 1.0, FF×8`, como en `load_transform_ok`.
   - **Descartarla** (no entregarla al cliente).
   - Si llega un Nak 1013 con `u16 0x0843`, abortar y aplicar §3.
4. En ese mismo momento, C→S `KillCheatReq(T, 0)`.
5. S→C:
   - descartar todo lo que nombre a `T` como actor principal (`VaryHPAck`, `NotifyHandleDeadAck`,
     `UnapplyBuffAck`, `ClearAllyGroupDataAck`, `ElementalMaterialFuelAck`, `RemoveActorAck`,
     movimiento…);
   - salvo estos, reescritos `T → C`: `LootDropCharacterRewardListAck` (+1) y, más tarde,
     `RemoveLootableAck` (+0), `InteractionPreemptionStepSuccessAck` (+8), `…FailAck` (+4) e
     `InteractionDoStepSuccessAck` (+8);
   - el `ApplyBuffAck` 20001 del jugador (buff de muerte) puede pasar.
6. C→S: `InteractionPreemptionStepReq`/`InteractionDoStepReq` con objetivo `+4 == C` → `+4 = T`, dejando
   "quien" intacto (jugador o mascota). Hasta que exista `T`, descartarlas o dejar el `FailAck` (el cliente
   reintenta).
7. Olvidar el alias al llegar `RemoveLootableAck(T)` o al retirar la copia.

### G. Riesgos

| Riesgo | Nivel | Mitigación |
|---|---|---|
| Actor equivocado, o variante de mascota/montura (tipo 4 → dueño = jugador) | Medio | Solo `load[2] == 3`, lista permitida, nunca una `V` de otro tipo |
| La carga llega a los 5-7 s y G intenta registrar antes | Bajo | Descartar o dejar fallar hasta tener `T` |
| Paquetes S→C con una clave que el cliente no tiene | Bajo-medio | Descartarlos todos (paso 5); es lo mismo que ya hace la fase 5 con las cargas descartadas |
| Gemelo persistente en la partida | Bajo [inferido] | Muere en el acto, motivo Cheat, sin clave de guardado. L5 con copia de `save` |
| Cierre del juego | Bajo | Nada escribe en la memoria del juego: solo Req nativas y descartes en el gancho de recepción que ya se usa |
| Cadáver de `KillCheatReq` no registrable | Medio [inferido] | L3/L4. Reserva: `LootingDropFromDeadBodyReq(T, ?, ?)` (G3) |
| Arma soltada en el suelo (carga de tipo 7) | Bajo | Es un objeto real del servidor de G: dejarla |

### H. Scripts y volcados nuevos (`build/research/p_botin_muerte/`)

| Fichero | Qué es |
|---|---|
| `s_29be8c0.asm`, `s_2c3ed50.asm` | Deserializador y núcleo de `SpawnCharacterCheatReq` |
| `s_278ea70.asm`, `x_278f3a0.txt`, `s_2af9ae0.asm`, `x_2af9f40.txt`, `x_2afa110.txt`, `x_2793000.txt` | Desc, validación y creación en el field |
| `s_2c40c00.asm` | Núcleo de `KillCheatReq` |
| `s_29e8fa0.asm`, `x_2b36860.txt`, `x_2b36ef0.txt`, `s_29e7d10.asm`, `x_29e9490.txt`, `x_2c44080.txt`, `s_2b81410.asm` | Interacción, `FailAck` y registro |
| `s_enum_spawnreason.asm`, `enum_spawnreason.txt`, `s_enum_deadreason.asm`, `enum_deadreason.txt` | Enums de motivo de aparición y de muerte |
| `reasons.py` (`reasons.txt`), `descfields.py`, `enumdump.py`, `enumtab.py`, `vt.py`, `xgrep.py`, `up.py` | Motivo por creador, campos del desc, volcado de enums, vtables, búsqueda en código mutado, subida hasta los handlers |
| `cheatops.py`, `kc.py`, `ifail.py` | Capturas: recuento de trucos, línea de tiempo tras `KillCheatReq`, `FailAck` de interacción |

Quedan para las pruebas en vivo:
- L2: que llegue la carga del gemelo y cuánto tarda; buscar en ella un byte 33 para identificarlo sin
  ambigüedad;
- L3/L4: que el cadáver de `KillCheatReq` se pueda registrar;
- L5: guardado.

---

## Por qué el gemelo no dio botín (prueba del 7 de octubre)

Build **1.0.0.2976**. Análisis estático y de capturas; el juego no se ejecutó y no se tocó `src/`.
Captura del invitado (Shadow): `build/research/shadow-20261007/cdmp_packets_20261007-040239.bin`. El reloj
de la captura va ~29 s por detrás del log: log `04:11:24,980` ≈ captura `497,194`. Scripts nuevos con
prefijo `g7_` en `build/research/p_botin_muerte/` (lista en P.6).

### P.0 Resumen

1. **Causa decisiva: el registro no lo dispara la interacción, sino un `DoFrameEventReq` del que
   registra, y el mod no lo reescribe.** [captura]
   - En los **31 de 31** registros reales de todas las capturas (`g7_lfe.py`), el cliente manda un
     `DoFrameEventReq` justo antes de la respuesta del servidor:
     - atacante = quien registra (jugador `A0100001` o mascota `B0100004`);
     - **el cadáver como objetivo**, en `+642`. En los paquetes de 1307 B de esta build es `n − 665`, la
       entrada de objetivo de siempre.
   - El servidor responde en 5-15 ms con `ApplyBuffAck` (buff 19 en el cadáver),
     `AddItemToInventoryAck`, `LootDrop…` vacío y `RemoveLootableAck`.
   - En Shadow, la interacción reescrita al gemelo funcionó (`…PreemptionStepSuccessAck` e
     `InteractionDoStepSuccessAck` con `B0100D8E`).
   - Pero el `DoFrameEventReq` siguiente seguía nombrando **la copia** `B01F0022` (t = 562,503,
     566,665, 568,688…). El servidor de G no tiene ese actor: no hay buff 19 ni registro. La mascota
     reintenta cada ~3 s durante un minuto.
2. **La tirada al morir funcionó bien.** [captura + verificado]
   - Sí llegó una `LootDropCharacterRewardListAck` de un gemelo: `B01011B7` (variante 52671), t = 748,853,
     42 B, 1 entrada (objeto `0xF49B6`, de `DropSet_Arrow_Drop` `0x27167`).
   - 52671 es la **única** de las cinco variantes con una entrada de máscara 2 (`g7_cii.py`, `l_mask2.py`).
     50002, 50007, 50038 y 52665 solo tienen máscara 1 (se tira al registrar).
   - Esas variantes tampoco mandan lista al morir cuando las mata un jugador (`deaths_var.csv`: 50002 y
     50007, 0 listas). Es el comportamiento normal.
   - Nada en la tirada al morir ni en la de registro lee el motivo de aparición (33), el motivo de muerte
     (16), el asesino ni el byte "cargado en el cliente" (`+0xCF`).
3. **`0x2569C661` = `eErrNoInteractionTargetConditionCheckFail`** [verificado]. Falló una condición de
   la interacción `Dead_Loot_Simple` (`0xF4249`) sobre el gemelo `B0100D76` (variante 50002). Es otro
   problema, menor (P.4).
4. **Arreglo recomendado (mínimo)** (P.5):
   - reescribir también la entrada de objetivo de `DoFrameEventReq` (copia → gemelo);
   - reescribir `PostProcessAttackResult_AttackerAck` (gemelo → copia);
   - crear el gemelo en la posición **asentada** de la copia: al primer intento de registro, no al morir.

### P.1 Línea de tiempo comparada [captura]

Registro real con la mascota (`20261006-172237`, cadáver `B0100B59`; `g7_kw.py … 650.7 654 B0100B59`):

```
651,621 C>S CurrentTargetChangeReq        040010b0 590b10b0 …
651,621 C>S InteractionPreemptionStepReq  040010b0 590b10b0 8c420f00 …
651,624 S>C InteractionPreemptionStepSuccessAck
651,653 C>S StartActionReq (mascota; objetivo en +364)
653,011 C>S InteractionDoStepReq          040010b0 590b10b0 8c420f00 00000000
653,019 S>C InteractionDoStepSuccessAck
653,027 C>S StartActionReq (mascota, acción 0x6C7F519B → 0x071B4F34; objetivo en +364)
653,086 C>S DoFrameEventReq               04 040010b0 … | +642: 590b10b0 + MoveData del cadáver   ← dispara el registro
653,099 S>C ApplyBuffAck                  590b10b0 13000000 … fuente 040010b0   (buff 19 en el cadáver)
653,099 S>C PostProcessAttackResult_AttackerAck  040010b0 590b10b0 81 …  (ataque 0x92A93847)
653,099 S>C AddItemToInventoryAck         (750 B)
653,100 S>C LootDropCharacterRewardListAck 04 590b10b0
653,100 S>C RemoveLootableAck              590b10b0 010010a0
```

Con el jugador (`Dead_Loot_Simple`, `0xF4249`) es igual:
- Preemption → DoStep → `StartActionReq` → `DoFrameEventReq` (atacante `A0100001`) ~0,2 s después;
- después, buff 19 y `AddItemToInventoryAck`;
- 8 casos en `20261003-105149` y `20261004-181940` (`g7_k49.py F4249`).

Shadow, mascota sobre la copia `B01F0022` (gemelo `B0100D8E`; `g7_kw.py … 559 570 B01F0022 B0100D8E`):

```
559,529 C>S InteractionPreemptionStepReq  040010b0 22001fb0 …   (el mod la reescribe a 8e0d10b0)
559,532 S>C InteractionPreemptionStepSuccessAck  … 8e0d10b0 …   (el servidor de G ve al gemelo)
562,420 C>S InteractionDoStepReq          040010b0 22001fb0 …   (reescrita)
562,421 S>C InteractionDoStepSuccessAck   … 8e0d10b0 …
562,437 C>S StartActionReq (mascota, +364 = 22001fb0)
562,503 C>S DoFrameEventReq               04 040010b0 … | +642: 22001fb0   ← SIN reescribir: objetivo inexistente
            (nada: ni buff 19, ni AttackerAck, ni AddItemToInventoryAck)
565,377 … se repite cada ~3 s hasta 612,9
```

El jugador hizo lo mismo a los 568,506: `Dead_Loot_Simple` sobre `B01F0022` → `B0100D8E`, Success, DoStep
Success, `DoFrameEventReq` con `22001fb0` en `+642` y ningún registro.

**C→S que nombran el cadáver entre la Preemption y el `AddItemToInventoryAck`**:

| Opcode | Mensaje | Dónde va el cadáver | ¿Hay que reescribirlo? |
|---|---|---|---|
| 2984 | `CurrentTargetChangeReq` | `+4` (`u32 quien · u32 objetivo · …`, 18 B) | No: el servidor aceptó la Preemption sin él [inferido] |
| 2671 | `InteractionPreemptionStepReq` | `+4` | Sí (ya se hace) |
| 2223 | `StartActionReq` del que registra (2 veces) | `+364` (objetivo de la acción) | Según las capturas no hace falta: el servidor no responde a él. Reescribirlo solo si la prueba falla [inferido] |
| 2720 | `InteractionDoStepReq` | `+4` | Sí (ya se hace) |
| **2380** | **`DoFrameEventReq` del que registra** | **Entrada de objetivo: clave en `n − 665` (= `+642` con 1 objetivo, 1307 B). MoveData del objetivo en `+4` y `+0x44` de la entrada** | **Sí: es el que dispara el registro** |

El cliente sigue mandando `MoveActorReq`, `UpdateGimmickReq` y `SetQuestDialogReq` de la copia, pero no
intervienen.

### P.2 Las condiciones del servidor, una a una [verificado salvo lo marcado]

**Al morir** (`KillCheatReq(T, 0)`), `Status vf97 0x2942350 → 0x2942730`:
- tipo de actor ∈ {1, 3, 4, 5, 6, 8, 9}: el gemelo es tipo 3 → pasa;
- motivo `Status+0x270` ≠ 0: es 16 → pasa (16 da el modo 2, como el 8);
- byte global `0x6D06E98` ≠ 0: pasa (si no, 52671 no habría mandado lista);
- `0x2AE6210`, `[actor+0xA0] == 0`: pasa (suelta el arma: `UnequipItemOnlyAck` en `7B` y `8E`);
- `0x2B85E40` modo 2 → `0x2B83960`: entradas de `CharacterInfo+0x280` con la máscara de "al morir". Solo
  52671 la tiene, y solo `B01011B7` mandó lista.

**Ninguna condición falló al morir.**

**Al registrar:**

| Paso | Función | Condición | Gemelo |
|---|---|---|---|
| Preemption | `0x29E8FA0 → 0x2B36860` (`.xtls`) | El objetivo está en el field de quien interactúa (`0x2A827B0`; si no, `FailAck` con error 0) | Pasa |
| | `0x2B36EF0` + `.xtls` | Condiciones de la `InteractionInfo` y del objetivo (detalle debajo) | Pasa en `8E`; **falla en `D76`** (P.4) |
| DoStep | `0x29E9490 → 0x2C44080 → 0x2B36B30` | Estado de la interacción | Pasa |
| **Golpe de registro** | `DoFrameEventReq 0x296D130 → 0x2A896E0 → ServerFrameEventAttack` | El **objetivo de la entrada debe existir** en el servidor. Validaciones del atacante (`pieza4-dano-servidor.md` §1.2). Volumen y alcance del ataque (`0x1564440`, sin decodificar) | **Nunca llegó: nombraba la copia** |
| Registro | buff 19 → `ProcessLootingDeadDropOnceTimer 0x2C31430 → 0x2B81410(LootDrop, &err, quien, 0, 0)` [inferido el enlace buff → temporizador; mismo milisegundo en capturas] | El cadáver tiene LootDrop (`eErrNoInvalidActorComponent`). Tipo del cadáver. Mascota o montura → su dueño (`UserOrCharacterValidLoginedScopeAttacher`: el que debe estar conectado es **el dueño**, no el cadáver) | Pasaría |
| | `0x2B85870` | Cadáver en un field (`eErrNoInvalidField`). Si `LootDrop+0x6C == 0`, tira la máscara `1 << 0 = 1` | Pasaría |
| | `0x2B829A0` | Field del que registra (`eErrNoInvalidField`) y su byte "se puede registrar" `+0x6C` (`eErrNoNotLootableField`). Alguna entrada de `+0x280` con máscara 1 (si no hay, éxito sin nada). Después, los objetos van al inventario | Pasaría |
| | `0x2B81410` (final) | `CharacterInfo+0x14E` (`LogoutAtLooted`) o el `u8` del 4.º argumento → retira el cadáver (`0x2068340`, motivo `0x56`). Si no, refresco `ToTarget` (`0x29F2890`) | — |

Condiciones de la `InteractionInfo` sobre quien interactúa:
- `Dead_Loot_Simple`: `0xF7176 InteractionDefault_Always` =
  `!checkActionAttribute(Fall‖Ride‖Catch‖Climb‖Jump‖Fly‖RemoteCatch‖WallUp‖MoveLv4)`;
- `Dead_Loot_Pet`: `0xF4241 InteractionDefault`.

Otras comprobaciones:
- `xgrep` sobre `0x2B81410`, `0x2B85870`, `0x2B829A0`, `0x2B36860`, `0x2B36EF0` y `0x2C31430`: ninguna
  lectura de `+0xCF` ni de `desc+0xA`. Nada comprueba el motivo de aparición, el motivo de muerte, el
  asesino ni que el cliente haya cargado el cadáver.
- **Ningún cadáver de motivo 16 se ha registrado nunca en las capturas** (`g7_dl.py`):
  - los 31 registros reales son de motivo 8;
  - el asesino era el jugador u otro NPC;
  - se registraron entre 0,7 s y 305 s después de morir.

  Que funcione con motivo 16 sigue [inferido], pero nada en el código leído lo impide.
- **Retirada del cadáver:** el recolector retiró `D76`, `7B` y `8E` a los ~150-157 s
  (`RemoveActorAck 04 | u32 0 | u8 0x19 | u32 clave`, t = 654,355).
  - No es "demasiado pronto": los intentos fueron a los 55-110 s.
  - El mod debe olvidar el alias al verlo. Hoy no lo detecta, porque busca la clave en `+0` y
    `RemoveActorAck` la lleva en `+6`.

### P.3 El error `0x2569C661` [verificado]

- Los códigos de error son el `hashlittle` de Bob Jenkins (lookup3) del nombre, con `initval = 0xC5EDE`:
  - hash en `0x1364780`;
  - lo llama el registro de errores `0x1427790`, desde `0x21FA720` y `0x1428140`;
  - con eso se traducen todos: `g7_errhash.py <código>`; tabla global → nombre con `g7_errmap.py`
    (1.564 nombres).
- `0x2569C661` = **`eErrNoInteractionTargetConditionCheckFail`**. Lo devuelve la Preemption cuando no se
  cumple una condición de la interacción evaluada sobre el objetivo.
- Otros de esta investigación:

  | Global | Error |
  |---|---|
  | `0x6CF6B68` | `eErrNoActorNotExist` |
  | `0x6CF6CF4` | `eErrNoNotLootableField` |
  | `0x6CF7844` | `eErrNoLogoutAtLooted` |
  | `0x6CF7D8C` | `eErrNoRewardNotExist` |
  | `0x6CF6B54` | `eErrNoInvalidField` |

### P.4 Por qué falló `D76` y no `8E` (sin resolver) [inferido]

- Mismo jugador y misma interacción (`0xF4249`): 6 intentos entre 579,3 y 581,5, a **0,3 m** del gemelo
  (posiciones con `g7_pos.py`). Con `8E`, la misma interacción pasó a los 568,5.
- Los dos gemelos son iguales en lo que mira el código leído:
  - tipo 3, motivo de aparición 33;
  - muertos con `KillCheatReq`, motivo de muerte 16;
  - nunca cargados en el cliente.
- No hay límite de tiempo: hay registros reales a los 305 s.
- Diferencias:
  - `D76` es `NHM_Bandit_OneHandDagger_50002`, con interacciones de atar, montar y proponer (`Npc_Bind`,
    `Enemy_Mount`, `Propose`), y no soltó arma al morir;
  - `8E` es `NTM_Bandit_TwoHandAxe_50038`.
- Hipótesis, por orden:
  1. **Interacción a medias.** A los 568,5 el jugador empezó `Dead_Loot_Simple` sobre `8E` y nunca la
     terminó (el golpe de registro fue a la copia). El servidor puede seguir considerándolo "en
     interacción" y rechazar otra. El arreglo de P.5 lo elimina.
  2. Una condición de objetivo de la variante 50002 que el gemelo no cumple (se evalúa en `.xtls`).
- Ninguna captura natural tiene este error (`g7_fails.py`: los únicos 6 son estos).
- Se decide en la prueba en vivo con el arreglo: si vuelve a salir, anotar la variante y el estado del
  jugador.

### P.5 Arreglo recomendado

**Mínimo (lo decisivo), en `loot_rewrite_c2s` y `loot_filter_s2c`:**

1. **C→S `DoFrameEventReq` (2380).** Reescribir solo si se cumple todo esto:
   - `p[0] == 4`;
   - el atacante (`p+1`) es nuestro jugador o una mascota o montura nuestra (`0xB010xxxx`);
   - la entrada de objetivo en `n − 665` es una copia con alias (`g_twin_of`);
   - hay **una sola** entrada de objetivo (`n == 0x282 + 665`, los 1307 B del registro). Con varias, el
     servidor exige claves en orden creciente (`pieza4-dano-servidor.md` §M2) y habría que reordenar.

   Entonces, escribir la clave del gemelo en `+0` de esa entrada.
   - No tocar el atacante ni el MoveData.
   - Los observadores (`guest_hits_observe_c2s`, `loot_observe_c2s`) leen la copia capturada **antes** de
     la reescritura y verán un "golpe" a una copia muerta. Conviene que ignoren los objetivos muertos.
2. **S→C `PostProcessAttackResult_AttackerAck` (2509):** `u32 atacante · u32 objetivo · …` (160 B).
   - Si `+4` es un gemelo con copia, reescribirlo a la copia (o descartar el mensaje).
   - Hoy pasa con la clave del gemelo, que el cliente no tiene.
3. **S→C `RemoveActorAck` (2516):** `04 · u32 0 · u8 motivo · n × u32 clave`. Si alguna clave es un
   gemelo:
   - descartar el mensaje (el cliente nunca lo tuvo);
   - olvidar el alias;
   - si no se registró, `no_loot_here(copia)`.
4. Lo demás ya está:
   - el `ApplyBuffAck` del buff 19 (`+0` = gemelo) se descarta;
   - `LootDrop…` (`+1`) y `RemoveLootableAck` (`+0`) se reescriben a la copia;
   - `AddItemToInventoryAck` no lleva claves de actor.

**Posición del gemelo (muy recomendable):**
- Crearlo **al primer intento de registro** de la copia: el primer `InteractionPreemptionStepReq` con
  objetivo = copia muerta y elegible.
- Usar la posición que tenga la copia **en ese momento** (`game::actor_world_position(copia)`).
- Descartar ese primer intento: el cliente reintenta a los ~3 s, y crear y matar el gemelo tardó 50-70 ms
  en esta captura.
- Motivo: la copia se sigue moviendo después del aviso de muerte.
  - El gemelo `8E` se creó en (−9647,8; 575,7; −4902,6).
  - El `DoFrameEventReq` de las 568,688 trae la copia en (−9650,7; 575,3; −4899,4): **4,6 m**.
  - El golpe de registro pasa por el volumen y alcance del ataque (`0x1564440`), y el MoveData solo se
    aplica al objetivo si es "del mismo dueño" (`0x2BC5420`) [inferido].
- Además, así solo se crean gemelos de cadáveres que G intenta registrar de verdad.

**Orden completo:**

```
copia C muerta y elegible (§6)
G intenta registrar C  → descartar ese primer Interaction*Req
C→S SpawnCharacterCheatReq (2115): u32 V | u32 0 | f32×3 posición actual de C | u8 33
S→C ActorLoadDataAck(T) → descartada;   C→S KillCheatReq (2279): u32 T | u32 0
S→C VaryHPAck/NotifyHandleDeadAck/… de T → descartados;  LootDrop…(T) si V tiene máscara 2 → a C
G reintenta:
C→S InteractionPreemptionStepReq   +4  C→T     S→C …SuccessAck +8 T→C  (…FailAck +4 T→C)
C→S InteractionDoStepReq           +4  C→T     S→C …SuccessAck +8 T→C
C→S StartActionReq (+364 = C)      sin tocar
C→S DoFrameEventReq   entrada n−665: C→T       ← NUEVO
S→C ApplyBuffAck buff 19 (T)       descartar
S→C PostProcessAttackResult_AttackerAck +4 T→C ← NUEVO
S→C AddItemToInventoryAck          pasa (los objetos de G)
S→C LootDropCharacterRewardListAck 04|T → 04|C
S→C RemoveLootableAck  T|jugador → C|jugador; olvidar el alias
```

**Alternativas evaluadas:**

| Opción | Veredicto |
|---|---|
| Otro motivo de aparición | No hace falta: nadie lo lee en la tirada ni en el registro. Seguir con 33 (sin guardado ni listas de dueño; nunca pedir una variante de tipo 4) |
| `VaryStatCheatReq` (2703) a 0 + `AttackByCollisionReq` (2815) con la flecha `0xF653A168` del jugador | No hace falta para el botín: la muerte ya tira bien. Da XP (motivo 8) y exige el gemelo cargado en el cliente. Descartado |
| `DieByRunOutFatalReq` (2321) | Tampoco cambia nada del registro. Descartado |
| Dejar que el cliente cargue el gemelo y retirarlo después | No hace falta: el registro no mira si el cliente lo cargó |
| **`LootingDropFromDeadBodyReq` (3052)** como reserva | Formato y riesgos debajo. Probar solo si P.5 falla, con copia de la partida |

`LootingDropFromDeadBodyReq` (3052), 6 B: `u32 muerto · u8 a · u8 b`:
- `0x2A116B0` lee `a → [rbp+0x68]` = 4.º argumento de `0x2B81410`. **Si ≠ 0, retira el cadáver tras
  registrarlo** (`0x2068340`, motivo `0x56`).
- Lee `b → [rbp+0x60]` = 5.º argumento. **Si ≠ 0, además manda la lista al que registra**
  (`0x29FCFF0`).
- El temporizador natural usa `(0, 0)`.
- No necesita interacción, golpe ni distancia.
- Riesgo: "quien registra" es `[sesión+0]`, el actor de la sesión, no el personaje (`KillCheatReq` lo
  traduce con vf44). Podría fallar o no llevar los objetos al inventario.
- Si hay error: Nak `0x3F5` con `u16 0x0BEC`.

**Riesgos:**
- Partida: los mismos que en §5 (objetos creados por el servidor de G con sus tablas).
- Sin XP ni crédito de misión por la muerte (motivo 16): el registro solo da objetos.
- Un `DoFrameEventReq` reescrito con un gemelo que ya no existe da un error silencioso, como hoy.

**Confianza:**
- **Alta** en la causa: los 31 registros reales usan ese `DoFrameEventReq`; en Shadow nombraba la copia y
  no pasó nada.
- **Media** en que, arreglado, salgan objetos: nunca se ha registrado un cadáver de motivo 16, y el
  alcance del golpe no está decodificado.
- **Baja** en la causa de `D76`.

### P.6 Scripts nuevos (`build/research/p_botin_muerte/`, ejecutar desde `build/research`)

| Script | Qué hace |
|---|---|
| `g7_kw.py <captura> <t0> <t1> <clave>…` | Todos los paquetes de un intervalo que contienen alguna clave (con su posición) |
| `g7_k49.py <clave de interacción>` | Cada `InteractionPreemptionStepReq` con esa clave y lo que sigue en 2,5 s, en todas las capturas |
| `g7_lfe.py` | Para cada `RemoveLootableAck`, el `DoFrameEventReq` que lo provocó (tamaño, posición de la clave, atacante) |
| `g7_fails.py` | Todas las `InteractionPreemptionStepFailAck` por (clave de interacción, error) y recuento de claves |
| `g7_dl.py` | Muerte → registro de los 31 registros reales: asesino, motivo y retraso |
| `g7_pos.py <captura> <t0> <t1> <clave>…` | Posiciones por `MoveActorReq`/`MoveActorAck` |
| `g7_dfe.py <captura> <t> <clave>` | Volcado de un `DoFrameEventReq` (ocurrencias de la clave y entrada de objetivo) |
| `g7_errhash.py <código>…` | Nombre de un código de error (lookup3, `initval 0xC5EDE`) |
| `g7_errmap.py <global>…` | Global de error → nombre |
| `g7_da.py <rva>` | Desensamblado anotado con los nombres de error |
| `g7_ii.py <tabla> <clave>…` | Registros de `interactioninfo`, `conditioninfo` o `dropsetinfo` |
| `g7_cii.py <variante>…` | Interacciones y condiciones dentro de un `CharacterInfo` |
| `s_2b829a0.asm` | Núcleo de "dar el botín" (`0x2B829A0`) |

---

## Prueba del 7 oct (noche)

Build del juego **1.0.0.2976**; mod **228287A2** (+ cambios locales, protocolo 9) en los dos PC, con el arreglo
del golpe de saqueo de 1307 B (`aa4ddb5`). Solo análisis de logs y capturas; no se ejecutó el juego ni se tocó
`src/`.

Ficheros:
- Invitado (Shadow): el log del invitado (`cdmp.log`, 7 oct, noche) y
  `cdmp_packets_20261007-191633.{bin,log}` (105 MB).
- Anfitrión: `bin64\cdmp.1.log` y
  `cdmp_packets_20261007-201826.{bin,log}`.

Relojes:
- Captura del invitado → log del invitado: **hora = t + 19:17:02,57**. Se ajustó con el `SpawnCharacterCheatReq`
  inyectado (log `19:25:06.271` "opcode 2115 posted" = captura t = 483,701).
- Log del anfitrión ≈ log del invitado **+ 1 h** (±1 s). Ejemplo: el anfitrión quita la copia `B01F0021` a las
  `20:25:08.101`; el invitado recibe el `RemoveActorAck` en t = 485,03 (`19:25:07.60`).
- La captura C→S guarda el paquete **antes** de `loot_rewrite_c2s`: muestra la clave de la copia aunque se
  envió la del gemelo. La S→C lo guarda **antes** de `loot_filter_s2c`: muestra la clave real del gemelo. Que la
  reescritura funcionó se ve en lo que contesta el servidor.

### N.0 Resumen

1. **El botín funcionó 3 veces: el servidor del invitado dio objetos reales** [captura].
   - Las 3 veces que la `Preemption` pasó, la cadena entera funcionó: `DoStep`, `DoFrameEventReq` de 1307 B
     (reescrito al gemelo), buff 19 en el gemelo, `AttackerAck`, `AddItemToInventoryAck`, `LootDrop…` y
     `RemoveLootableAck`. El arreglo `aa4ddb5` **funciona**.
   - Objetos (de `iteminfo`):
     - mascota sobre `B01F0053` a las 19:27:27: `Very_Small_Copper_Pack` ×3;
     - mascota sobre `B01F003D` a las 19:27:44: `IronStone` ×1 y `CopperOre` ×1;
     - jugador sobre `B01F0063` a las 19:29:26: `Clout` ×3, `IronStone` ×1 y `CopperOre` ×2.
   - El `AddItemToInventoryAck` es idéntico al de un saqueo propio: misma cabecera
     `04 010010a0 050b 0001 0100 00` y 12 + 369·n B (n = 1, 2 y 3).
2. **Por qué en la prueba no se vio botín** [captura + logs]:
   - **3 de las 11 copias desaparecieron antes de poder saquearlas.** El anfitrión murió dos veces (hp 0 a las
     20:24:56 y 20:26:52) y reapareció a ~150 m, en (−10172,7; 674,4; −2156,9), a las 20:25:08 y
     20:27:58. Su barrido de zona quitó las copias, también las muertas ("is not a zone enemy; its copy …
     removed"):
     - `B01F0021`, 1,3 s después de morir;
     - `B01F003C` (127 s) y `B01F003E` (75 s), en el lote de t = 651,42 (19:27:54).
   - **2 de los 5 saqueos intentados los rechazó el servidor del invitado** con `InteractionPreemptionStepFailAck`
     error `0x2569C661` (`eErrNoInteractionTargetConditionCheckFail`):
     - `B01F0064` (gemelo `B01008DD`): la mascota lo pidió 22 veces en 62 s;
     - `B01F006A` (gemelo `B0100937`): el jugador lo pidió 6 veces.
   - **La mascota se quedó atascada** reintentando `B01F0064` desde las 19:28:35 hasta que se cerró el juego
     (19:29:46). Por eso nunca saqueó `B01F0065`, `B01F0069` ni `B01F0067`, que tenían gemelo muerto y
     estaban a 2-10 m.
   - Lo que sí salió era poco y casi todo lo saqueó la mascota sola. El único saqueo a mano que funcionó fue
     20 s antes de cerrar.
   - **El mod no escribe nada en el log ni al saquear ni cuando el servidor rechaza.** Además, "Enemy relay:
     enemy copy … died (no loot here)" sale **siempre**, también cuando se pidió gemelo (`enemy_relay.cpp:1875`).
     Leyendo el log parece que nunca hubo botín.
3. **El rechazo `0x2569C661` es del gemelo, no del que saquea** [captura + verificado]:
   - no depende de la distancia, la altura, el tiempo desde la muerte ni el estado del jugador o la mascota
     (N.4);
   - es una condición de "bloqueo" evaluada **sobre el objetivo** en `0x24051B0` (N.4.2);
   - qué condición exacta es sigue sin resolver: las candidatas, por orden, están en N.4.3.
4. **Arreglo** (N.5):
   - (a) ante ese `FailAck` de un gemelo, saquearlo por `LootingDropFromDeadBodyReq` (3052) una vez, y si
     tampoco sale, `no_loot_here(copia)` para que la mascota deje de reintentar;
   - (b) no quitar una copia muerta con gemelo pendiente cuando el anfitrión la retira;
   - (c) escribir en el log los saqueos y los rechazos.

### N.1 Las 11 copias, una a una [captura]

Todos los gemelos murieron de verdad:
- el `ActorLoadDataAck` del gemelo llegó 5-15 ms después del `SpawnCharacterCheatReq` (21 B, motivo 33), con
  la posición pedida exacta (`n7_ld.py`: diferencia 0,00 m en los 11);
- el `KillCheatReq` (8 B) salió 8-27 ms después;
- el `NotifyHandleDeadAck` (19 B, `u32 gemelo · u32 A0100001 · u8 0x10`, motivo 16, asesino el jugador)
  llegó 6-67 ms después del `KillCheatReq`;
- ningún `Loot: twin … not dead yet` en el log;
- en esta captura (hasta t = 755, 271 s después del primero) el servidor **no** retiró ningún gemelo
  (`RemoveActorAck`). Por la mañana los retiró a los ~150 s.

| Copia | Variante | Gemelo | Muerte de la copia (log) | Spawn / Kill / Dead del gemelo (t) | Lista al morir | Saqueo | Resultado |
|---|---|---|---|---|---|---|---|
| `B01F0021` | 1001982 `NHM_Drunk_BlackBears_OneHandSword_2` | `B010084A` | 19:25:06.266 | 483,701 / 483,721 / 483,726 | — | ninguno | La copia se quitó en t = 485,03 (anfitrión reaparecido) |
| `B01F003C` | 54033 `…OneHandBow_54033` | `B0100854` | 19:25:46.344 | 523,778 / 523,793 / 523,860 | 153 B | ninguno | Quitada en t = 651,42 |
| `B01F003D` | 1000331 `…OneHandSword_1` | `B010086F` | 19:26:06.381 | 543,824 / 543,841 / 543,853 | — | mascota, t = 632,369 → 641,451 | **Objetos** (750 B) |
| `B01F003E` | 336434581 `NHM_Brimstone_Drunk_BlackBears_OneHandSword_1` | `B0100885` | 19:26:34.653 | 572,087 / 572,121 / 572,132 | — | ninguno | Quitada en t = 651,42 |
| `B01F0053` | 1000452 `NGM_Wolf_Pursuer_OneHandMusket_3` | `B010088C` | 19:27:00.264 | 597,706 / 597,724 / 597,736 | 79 B | mascota, t = 623,197 → 624,583 | **Objetos** (381 B) |
| `B01F0064` | 54006 `…OneHandAxe_54006` | `B01008DD` | 19:28:34.043 | 691,476 / 691,508 / 691,522 | — | mascota ×22, t = 692,568-753,801 | **`FailAck 0x2569C661` ×22** |
| `B01F0065` | 54034 `…OneHandBow_54034` | `B01008FF` | 19:28:43.125 | 700,564 / 700,582 / 700,593 | 153 B | ninguno | Gemelo muerto sin saquear |
| `B01F0069` | 54015 `…OneHandBomb_54015` | `B0100909` | 19:28:55.126 | 712,561 / 712,578 / 712,591 | — | ninguno | Gemelo muerto sin saquear |
| `B01F0067` | 54033 `…OneHandBow_54033` | `B010090E` | 19:28:56.940 | 714,374 / 714,397 / 714,404 | 264 B | ninguno | Gemelo muerto sin saquear |
| `B01F0063` | 1000331 `…OneHandSword_1` | `B0100931` | 19:29:11.460 | 728,891 / 728,907 / 728,917 | — | jugador, t = 743,629 → 743,82 | **Objetos** (1119 B) |
| `B01F006A` | 1000456 `NGM_Wolf_Pursuer_OneHandAxe_5` | `B0100937` | 19:29:14.524 | 731,967 / 731,982 / 731,992 | — | jugador ×6, t = 736,715-739,272 | **`FailAck 0x2569C661` ×6** |

Notas:
- Las tres sin gemelo (`B01F0049`, `B01F0040`, `B01F0043`: "we did not hit it lately") recibieron el
  `LootDrop` vacío y el `RemoveLootableAck` inyectados en t = 582,06. Correcto.
- "Lista al morir": `LootDropCharacterRewardListAck` del gemelo en el lote de la muerte. Solo las variantes con
  máscara 2 (`DropSet` `0x27167`/`0x27168`), como se esperaba (`l_mask2.drops`). El filtro la reescribe a la
  copia.
- `B01008FF` soltó el arma al morir (`UnequipItemOnlyAck` + `CreateProjectileAck`). Ninguno de los tres que
  dieron objetos la soltó: el arma no influye (la hipótesis de la mañana sobre `D76` queda descartada).

### N.2 Un saqueo que funcionó, paso a paso [captura]

Mascota `B0100004` sobre la copia `B01F003D` (gemelo `B010086F`); `g7_kw.py … 632 642 B01F003D B010086F`:

```
632,369 C>S CurrentTargetChangeReq        040010b0 3d001fb0 …                  (18 B, sin reescribir)
632,369 C>S InteractionPreemptionStepReq  040010b0 3d001fb0 8c420f00 …        (25 B; enviado con 6f0810b0)
632,376 S>C InteractionPreemptionStepSuccessAck 040010b0 040010b0 6f0810b0 8c420f00 …  (29 B, +8 = gemelo)
632,371-640,594 C>S StartActionReq ×8 de la mascota (+364 = 3d001fb0, sin reescribir; acción 9B517F6C)
641,278 C>S InteractionDoStepReq          040010b0 3d001fb0 8c420f00 00000000  (16 B; enviado con el gemelo)
641,284 S>C InteractionDoStepSuccessAck   010010a0 040010b0 6f0810b0 8c420f00 00000000
641,295 C>S StartActionReq                9B517F6C → 071B4F34 (+364 = copia)
641,451 C>S DoFrameEventReq               1307 B, 04 040010b0 …, +642 = n−665: 3d001fb0 (enviado con 6f0810b0)
641,459 S>C ApplyBuffAck                  6f0810b0 13000000 … fuente 040010b0   (77 B, buff 19 en el gemelo)
641,459 S>C PostProcessAttackResult_AttackerAck 040010b0 6f0810b0 81 …     (160 B)
641,470 S>C AddItemToInventoryAck         04 010010a0 050b…                (750 B: IronStone ×1, CopperOre ×1)
641,472 S>C LootDropCharacterRewardListAck 04 6f0810b0 04 b64c0f00 …       (301 B → copia)
641,472 S>C RemoveLootableAck             6f0810b0 010010a0                 (8 B → copia; alias borrado)
```

- La `Preemption` se aceptó con la mascota a **10,8 m** del gemelo (`n7_dist.py`). Luego la mascota se acercó
  hasta 5,7 m mientras repetía la acción `9B517F6C`.
- `B01F0053`: igual, con la mascota a 0,4 m; del `DoStep` al `AddItem` pasaron 81 ms.
- `B01F0063` (jugador, `Dead_Loot_Simple` `0xF4249`): `Preemption` y `DoStep` seguidos (18 ms); el
  `DoFrameEventReq` 0,17 s después; los objetos a los 3 ms.
- **Comparación con un saqueo propio:** en esta captura no hay ninguno; todos los `RemoveLootableAck` son de
  gemelos o inyectados. El de referencia es el de `20261006-172237`, t = 653,1 (`B0100B59`, §1.2): la misma
  secuencia y los mismos tamaños (`AddItem` 750 B, misma cabecera).
  - Única diferencia: tras el saqueo, el servidor manda la lista que queda (42, 301 y 116 B, solo entradas
    con primer byte 4, p. ej. `0xF4525`), no la de 5 B vacía. [inferido] Son entradas que no se dan al
    registrar (condicionadas). El cliente la recibe con la clave de la copia, justo antes del
    `RemoveLootableAck`.
- Recuento de 31 saqueos reales en todas las capturas (`n7_nat.py`): incluye la variante 50002 (`B0100921`,
  `20261003-105149`, jugador). Ninguna variante es "insaqueable" por sí misma.

### N.3 Los dos rechazos [captura]

`B01F0064` → `B01008DD` (54006), mascota, `Dead_Loot_Pet` `0xF428C`:

```
684,345 C>S StartActionReq mascota 866C7489 → ABFCB379, +364 = B01F0064  (la mascota ataca la copia VIVA)
691,476 C>S SpawnCharacterCheatReq f6d20000 00000000 (−10270,17; 674,72; −2280,77) 21
691,500 S>C ActorLoadDataAck  B01008DD (8116 B)    691,508 C>S KillCheatReq   691,522 S>C NotifyHandleDeadAck
692,568 C>S InteractionPreemptionStepReq 040010b0 64001fb0 8c420f00 …  (1,05 s después de la muerte)
692,580 S>C InteractionPreemptionStepFailAck 040010b0 dd0810b0 8c420f00 61c66925 00000000 000100   (23 B)
… 21 veces más, cada ~2,8 s, hasta 753,801/753,803 (19:29:36). Entre medias, la mascota repite 866C7489
  (+364 = copia) y no se mueve: (−10268,1; 674,7; −2285,6), a 5,2-5,3 m del gemelo.
```

`B01F006A` → `B0100937` (1000456), jugador, `Dead_Loot_Simple` `0xF4249`:

```
731,967 Spawn (−10247,11; 674,94; −2287,11)   731,974 Load (7974 B)   731,982 Kill   731,992 Dead
736,715 C>S InteractionPreemptionStepReq 010010a0 6a001fb0 49420f00 …   (4,7 s después de la muerte)
736,719 S>C …FailAck 010010a0 370910b0 49420f00 61c66925 …
… ×6 hasta 739,281, con el jugador a ~1,5 m del gemelo. A las 743,629 el mismo jugador saquea B01F0063 sin
  problema.
```

- Formato del `FailAck` (3000, 23 B): `u32 quien · u32 objetivo · u32 clave de interacción · u32 error · u32 0
  · u8 0 · u8 1 · u8 0`. El filtro lo reescribe (`+4` gemelo → copia). El cliente reintenta cada ~3 s sin
  límite (la mascota, por su IA).
- No hubo ningún otro error ni `Nak` sobre gemelos.

### N.4 El error `0x2569C661`

#### N.4.1 Lo que no es [captura]

| Factor | Rechazados | Aceptados | Conclusión |
|---|---|---|---|
| Distancia de quien saquea al gemelo | `DD` 5,2 m (mascota); `37` ~1,5 m (jugador); `D76` (mañana) 0,3 m | `6F` **10,8 m** (mascota); `8C` 0,4 m; `31` ~2 m | No es la distancia |
| Tiempo desde la muerte | `DD` 1,05 s… 62 s; `37` 4,7 s; `D76` 82 s | `8C` 25,5 s; `6F` 88,5 s; `31` 14,7 s; `8E` 55 s | No es el tiempo (`DD` falla igual a los 62 s) |
| Gemelo en el aire | `DD`: la copia está a 674,72-674,76 al crear el gemelo y acaba en 674,73; `37`: 674,94 → 674,9 | `31` se creó 0,3 m **por debajo** de donde acabó la copia | No (`n7_y.py`) |
| Posición del gemelo | Exacta a la pedida | Exacta | — |
| Estado de quien saquea | El jugador falla con `37` a las 739,3 y acierta con `31` a las 743,6. La mascota había terminado bien `6F` a las 641,5 | — | No es quien saquea (además daría `…OwnerConditionCheckFail`, `0x6CF6D24`) |
| Interacción a medias (hipótesis 1 de la mañana) | `DD` falla ya en el primer intento | — | Descartada |
| Tiempos de creación y muerte | Load +8 ms, Kill +10 ms (`DD`); `37` igual | Iguales | No |
| Variante | 54006, 1000456 y 50002 (una vez cada una) | 1000331 (×2), 1000452 y 50038 | Única diferencia sistemática que queda; ningún gemelo de las variantes fallidas se probó dos veces |

#### N.4.2 Dónde sale [verificado, estático]

- Solo `0x24054FA` lee el global del error (`0x6CF6D2C`), dentro de **`0x24051B0`**. `0x21FD4D1` y `0x220B846`
  son de la tabla y del registro de errores.
- **`0x24051B0(entrada, quien, objetivo, r9b)`** evalúa una entrada de condición de 32 B (volcado
  `s_24051b0.asm`):
  - `[+0]` condición A, que se evalúa con **quien saquea** como sujeto;
  - `[+8]` condición B, con **el objetivo** como sujeto;
  - `[+0x10]` tipo (da el byte de resultado 0/1/3/4);
  - `[+0x18]` desactivada (si ≠ 0, la entrada pasa);
  - `[+0x19]` modo.
    - Si **≠ 0** (modo "permitir"): pasa si A o B se cumplen; si no, `OwnerConditionCheckFail`.
    - Si **= 0** (modo "bloquear"): si `quien->vf(0x178)` y A se cumple → `OwnerConditionCheckFail`; **si B
      se cumple → `TargetConditionCheckFail`**. Si ninguna, pasa.
  - Así que el rechazo es una condición de **bloqueo** que **se cumple en el gemelo**.
- **`0x21206C0`** es el único que llama a `0x24051B0`, y desde dos bucles (lo llama `0x211FCB0`). `rdx` es el
  objetivo; `[this+8]`, quien saquea; `r8`, la clave de la interacción.
  1. **Entradas propias del objetivo.**
     - Si el tipo de actor `[[obj+0x88]+1]` es 1, busca en un dato dinámico del actor:
       `0x15EE200(…, [[[obj+0x68]+0x58]+0x90]+8)`, tabla de entradas de `0x140` B, lista en `+0x20`/`+0x28`.
     - Si no encuentra nada, usa la lista de su `CharacterInfo`: `0x389570([[obj+0x68]+0x20]+0x30)`,
       `+0x338`/`+0x340`.
     - Si el tipo es 7, usa la lista del gimmick (`[obj+0x68]+0x30`, `+0x48`).
     - En las listas busca la interacción por su clave (`u16` en `+0`) y evalúa sus condiciones (`+0x50`,
       n = `+0x58`, 32 B cada una).
  2. **Condiciones de la `InteractionInfo`:** `0x3888B0(clave)`, `+0x48`/`+0x50`.
     - `Dead_Loot_Pet`: solo `0xF4241 InteractionDefault` = `!checkActionAttribute(SwimUnderWater ‖ SwimMove ‖
       Fall ‖ Ride ‖ Catch ‖ Climb ‖ Jump ‖ Fly ‖ RemoteCatch ‖ WallUp ‖ MoveLv4)`.
     - `Dead_Loot_Simple`: `0xF7176` (lo mismo sin nadar) y `0xF4312`.
  - Además, §1.3 ya cita las condiciones de datos `ConditionData_IsLootable`, `ConditionData_HasLootItem` e
    `ConditionData_IsPetLooting`. Evalúan en `.xtls`.
- Los dos rechazos dan **el mismo error con dos interacciones distintas** (mascota `0xF428C` y jugador
  `0xF4249`). Eso apunta a algo del **cadáver**, común a las dos, y no a la condición de una interacción.

#### N.4.3 Hipótesis, por orden [inferido]

1. **Estado del gemelo en el servidor**, distinto según la variante. Muere 10-30 ms después de nacer, quizá
   durante su acción de aparición. Su acción actual en el servidor tendría un atributo de la lista de bloqueo
   (`Fall`/`Jump`/`Catch`…). También podría estar en un nodo del action chart cuyo dato dinámico (rama 1 de
   `0x21206C0`) bloquea `Dead_Loot_*`.
   - A favor: el fallo es permanente para ese gemelo y no depende de quien saquea ni de la distancia.
   - Ningún cliente lo simula: el servidor no mandó ni un `StartActionAck` de un gemelo en toda la captura. Así
     que su estado se queda fijo.
2. **`ConditionData_IsLootable` / `HasLootItem` sobre el cadáver.** El conjunto de jugadores con derecho
   (`LootDrop+0x18`) o la lista del cadáver, distintos en `DD`/`37`/`D76`.
   - En contra: todos los gemelos murieron igual (asesino `A0100001`, motivo 16).
   - Y las variantes que solo tienen máscara 1 (`6F`, `31`) se saquearon sin lista previa.
3. Una condición de la lista propia de la variante en su `CharacterInfo`.
   - En contra: 50002 se saquea sin problema cuando muere de forma natural (`B0100921`).

**Cómo decidirlo:** el servidor del invitado está en el mismo proceso. Al recibir el `FailAck` de un gemelo, el
mod puede volcar en solo lectura (SEH):
- el actor del servidor (`0x2A827B0` sobre `[exe+0x6D69698]`, como `LootingDropFromDeadBodyReq`);
- su `LootDrop` (`[actor+0x68]+0xC8`: `+0x18`, `+0x58`, `+0x6C`);
- su acción actual.

Si se compara con un gemelo que sí se saqueó, se sabe cuál de las tres es.

### N.5 Arreglo recomendado

**(a) Rescate y desbloqueo ante el rechazo:** en `loot_filter_s2c`, rama de `kOpInteractionPreemptionStepFailAck`
(`src/mod/net/loot_twin.cpp`).
- Hoy solo reescribe `+4` a la copia. Pasaría a hacer esto:
  1. Leer `err = u32 en +12`. Si `+4` es un gemelo con copia y `err != 0`, escribir en el log una vez por
     gemelo: `Loot: twin T of copy C refused the loot (interaction K, error E)`.
  2. Si `err == 0x2569C661` y es el primer rechazo de ese gemelo, encolar `LootingDropFromDeadBodyReq` (3052)
     con `sniffer_inject_c2s(make_packet(3052, {u32 T, u8 0, u8 0}))`.
     - `(0, 0)` es lo que usa el temporizador natural: no retira el cadáver ni manda la lista aparte.
     - Va directo a `0x2B81410` y **se salta todas las condiciones de interacción**, la distancia y el golpe
       (§1.4, §P.5).
  3. Guardar en `Twin` `refused_ms` y `rescued`.
     - Si llega su `RemoveLootableAck`, la rama que ya existe lo reescribe a la copia y borra el alias.
     - El `AddItemToInventoryAck` pasa sin tocar.
  4. En `loot_tick`: si a los 3 s el gemelo sigue con copia (sin `RemoveLootableAck`), o llega otro
     `FailAck` después del rescate:
     - `no_loot_here(copia)`;
     - borrar el alias y `t.copy = 0`;
     - log `Loot: no loot for copy C (its twin T refused it)`.

     El cliente borra el aviso de saqueo y **la mascota deja de reintentar**: puede saquear los demás
     cadáveres.
  5. Si el rescate falla, descartar los `FailAck` siguientes de ese gemelo (`return true`), para no
     alimentar más reintentos.
- Riesgos:
  - "Quien registra" en 3052 es el actor de la sesión (`[sesión+0]`), no el personaje.
    - El handler sube de mascota o montura a dueño, pero no está probado con el actor de la sesión.
    - Puede responder `Nak 0x3F5` (`u16 0x0BEC`) o no dar nada. En ese caso queda el desbloqueo del paso 4,
      que ya es una mejora.
  - Objetos creados por las tablas del servidor del invitado: los mismos riesgos para la partida que ya se
    aceptaron en §5. Copia de la partida antes de probar.
  - Un solo intento por gemelo. Los gemelos ya están limitados a 30 cada 5 min.
  - Paquete nuevo: comprobar en la captura que sale con 6 B y que el servidor contesta. Si no contesta nada,
    el paso 4 lo cubre.

**(b) Que una copia muerta con gemelo pendiente no desaparezca:** `enemy_on_remove`
(`src/mod/net/enemy_relay.cpp:1942`).
- Hoy el anfitrión la quita cuando él muere o reaparece ("is not a zone enemy"), y el invitado la borra al
  momento (`loot_forget_copy` + `puppet_remove_actor`).
- Propuesta:
  - nueva función `bool loot_keeps_copy(uint32_t copy)`: verdadera si la copia tiene alias a un gemelo muerto
    con copia, sin saquear y con `now − kill_ms < 120 s`;
  - si lo es, `enemy_on_remove` aplaza la retirada a una lista nueva (`g_deferred_remove`);
  - la retirada se completa en `enemy_tick` cuando `loot_keeps_copy` pasa a falso: saqueada, rechazada,
    caducada, o `loot_reset`.
- Alternativa en el anfitrión: no quitar en el barrido de zona las copias de enemigos **muertos** durante
  120 s.
  - Es más sencillo, pero afecta a todas las copias.
  - Es preferible decidirlo en el invitado, que es quien tiene el gemelo.
- Riesgos:
  - un cadáver que el anfitrión ya no tiene se ve en el invitado hasta 120 s más (es solo del cliente;
    inofensivo);
  - si el anfitrión se reinicia y reutiliza claves de copia, `loot_reset` ya borra los alias y libera los
    aplazados.

**(c) Diagnóstico en el log** (`loot_twin.cpp`, `enemy_relay.cpp`):
- `RemoveLootableAck` de un gemelo con copia: `Loot: copy C looted through twin T`. Si el
  `AddItemToInventoryAck` anterior era de quien saquea, añadir el número de objetos, `(n − 12) / 369`.
- Corregir `enemy_relay.cpp:1875`: escribir "(no loot here)" solo cuando `!twin`.

**(d) Lo que ya funciona y no hay que tocar:**
- la reescritura del `DoFrameEventReq` de 1307 B;
- la `Preemption` y el `DoStep`;
- los `…SuccessAck` (`+8`), `AttackerAck` (`+4`), `LootDrop…` (`+1`) y `RemoveLootableAck` (`+0`).

**Prueba siguiente (con copia de la partida):**
- el anfitrión no debe morir mientras haya cadáveres por saquear (o probar con (b));
- saquear a mano con el jugador (no solo la mascota) al menos 5 copias de variantes distintas;
- buscar en el log del invitado `refused`, `looted through` y `no loot for copy`;
- en la captura, cada `LootingDropFromDeadBodyReq` y lo que contesta.

### N.6 Scripts nuevos (`build/research/p_botin_muerte/`, ejecutar con `build/research/venv`)

| Script | Qué hace |
|---|---|
| `n7_tl.py <captura>` | Línea de tiempo de truco, muerte, interacción, botín y `AddItem` |
| `n7_tw.py <captura> <gemelo,…> [t0]` | Todos los paquetes que nombran a un gemelo (posición de la clave) |
| `n7_cp.py <captura>` | Por copia muerta: qué mensajes C→S y S→C la nombran después de morir (claves y muertes fijas de esta prueba) |
| `n7_ld.py <captura>` | Carga de cada gemelo frente a la posición pedida en el `SpawnCharacterCheatReq` |
| `n7_items.py <captura> <t,…>` | Objetos y cantidades de un `AddItemToInventoryAck` (12 + 369·n B; clave en `+8` de la entrada) |
| `n7_sa.py <captura> <t0> <t1> <clave>` | Acciones (`StartActionReq`, objetivo en `+364`) e interacciones de un actor |
| `n7_dist.py <captura> <t0> <t1> <clave> <x,y,z>` | Posiciones de un actor y distancia a un punto |
| `n7_y.py <captura> "<nombre copia t y>;…"` | Altura de la copia alrededor de la creación del gemelo |
| `n7_nat.py` | Los saqueos reales de todas las capturas, con la variante del cadáver y el primer intento |
| `s_24051b0.asm`, `s_21206c0.asm` | Evaluación de las condiciones de interacción y de dónde salen las entradas |

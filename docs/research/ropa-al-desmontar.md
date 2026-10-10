# La copia recupera la ropa de la conexión tras desmontar (o tras cualquier recarga)

Build **1.0.0.2976** (PE `0x6AB28F00`). Prueba real con dos PC del 5 de octubre de 2026:
anfitrión = PC propio e invitado = Shadow, con la **misma partida** en los dos (el atuendo
lleva una cabeza de conejo). Es análisis de solo lectura: no se ha cambiado código ni se ha
ejecutado el juego. Lo marcado **(suposición)** está sin verificar.

## 0. Resumen

> **Arreglado y probado (5 oct, 23:13, un PC con `cdmp_peer`; commit `649cf29`, protocolo 7).**
> Se aplicó §5.1-§5.4. En 3 desmontes, la copia reapareció con la ropa actual. La lectura
> del componente de equipo confirma que el juego lo aplicó: la serie es la del último cambio y las
> ranuras 3 y 4 se recolocaron. Durante ~1 s se ve la ropa de la conexión (las piezas se aplican
> 1 s después de cargar la copia). Se considera aceptable. Se podría adelantar a
> ~300 ms (§5.2, opcional).

- **Síntoma.** Cuando un jugador desmonta, el otro PC recarga la copia y la ve con la ropa que
  llevaba **al conectar**. Los cambios en vivo anteriores sí se habían visto (A5 ✅). Pasa en los
  dos sentidos: la copia del invitado en el anfitrión (MARK 2) y la del anfitrión en el invitado
  (MARK 4).
- **El emisor no reenvía su personaje.** El receptor recarga desde `last_spawn_`, que guarda las
  plantillas de la conexión (siempre "generation 1"/"2"). Encima añade el mapa de ranuras
  cambiadas y lo reinyecta un segundo después con un único `OtherEquipItemAck` (2757).
- **Ese 2757 llega al cliente.** Sale en las dos capturas, con la clave de la copia, las ranuras
  correctas y la última serie. Aun así, **no se aplica**.
- **Causa principal (muy probable).** El mapa por ranura guarda solo el **estado final** de cada
  ranura y olvida que esa ranura **ya estaba ocupada en la plantilla**. Por eso el 2757 de
  después del spawn intenta **poner** una pieza en una ranura ocupada sin el `{0, ranura}` que
  el servidor siempre manda en un cambio. Así ocurre, por ejemplo, con `[{F42E4,4}]` en lugar de
  `[{F42E4,4},{0,4}]`.
  - `0x980A50` aborta **todo** el mensaje si falla una sola puesta. La retirada de la cabeza
    (`{0,3}`), que va en el mismo mensaje, tampoco se aplica.
  - La copia queda igual que la plantilla, es decir, con la ropa de la conexión.
- **Causa secundaria posible.** La serie del 2757 de después del spawn es **igual** a la del
  último cambio en vivo, no mayor. Si el componente de la copia nueva heredara esa serie (caché
  `cacheEquipSlotList`, que se indexa por characterNo), la puerta `serie > comp+0x18` también
  lo descartaría. **(Suposición: no la descartan los datos, pero tampoco la demuestran.)**
- **Arreglo.** El mapa tiene que recordar, por ranura, si la plantilla tenía algo. Tras el spawn
  se manda lo mismo que mandaría el servidor:
  - primero un `OtherUnequipItemAck` (3073, que no mira la serie) por cada ranura que la
    plantilla tenía ocupada;
  - después un 2757 solo con las puestas;
  - todo con una serie que **crezca siempre** en el receptor;
  - y fuera de la cola de movimiento.
  - Además, un diagnóstico que lea el componente de equipo de la copia para confirmar si se
    aplicó.

## 1. Qué hace el código hoy

### 1.1 Emisor (`src/mod/net/local_capture.cpp`)

- `g_equip_slots` (`:166`) es un `map<ranura, itemKey>` con "0 = quitada". Contiene los cambios
  desde la última plantilla. `note_equip` (`:260-279`):
  - lo vacía si cambió la generación de plantillas (`:263-267`);
  - aplica primero las retiradas y después las puestas (`:269-274`);
  - guarda la última serie (`:275`);
  - emite el cambio tal cual (`EventKind::Equip`).
- Las retiradas sueltas llegan desde `UnequipItemAck` (2280), que ve el sniffer (`:464-473`), y
  se convierten en `{0, ranura}`.
- `capture_spawn_info` (`:763-791`) adjunta el mapa a `SpawnInfo.equip` como un 2757 con **una
  entrada por ranura** (`:785-790`). Una ranura quitada sale como `{0,r}`; una cambiada, como
  `{nuevo,r}`, **sin** `{0,r}`.
- El emisor **solo vuelve a enviar su personaje cuando cambia la generación de plantillas**
  (`coop.cpp:228-243`), es decir, cuando llega un `ActorLoadDataAck` propio (`:622-638`). Desmontar,
  cambiarse de ropa o quedarse atascado no generan plantillas nuevas. El servidor nunca reenvía
  `ActorLoadDataAck` por un cambio de equipo (`apariencia-estado-coop.md` §1.5).

### 1.2 Receptor (`src/mod/net/coop.cpp`, `src/mod/net/remote_puppet.cpp`)

- Cada `RelKind::Equip` que llega en orden pasa por `deliver_ordered` (`coop.cpp:327-337`):
  - se **funde** en `last_spawn_` y `remote_spawn_` con `merge_equip` (`:31-67`). Es el mismo
    mapa por ranura del emisor, **con las retiradas** (`0`), pero tampoco recuerda la ocupación
    de la plantilla;
  - se inyecta en vivo con `puppet_event`:
    - una retirada suelta de 15 B se convierte en `OtherUnequipItemAck` 3073
      (`remote_puppet.cpp:1223-1231`);
    - todo lo demás va como 2757 tal cual llega (`:1233`).
- **Recarga.** Cualquier recarga reutiliza `last_spawn_`:
  - por desmontar: `remote_puppet.cpp:1152-1173` → `coop.cpp:203-207`;
  - por copia atascada: `:1325`;
  - por copia desaparecida: `coop.cpp:217-222`;
  - por vuelta tras un silencio: `:194-199`.
- `puppet_spawn` (`remote_puppet.cpp:749-756`) encola las plantillas y, a `now + 1000` ms, **un
  único 2757** con todo el mapa y la serie del último cambio. Va con `movement=true`, así que
  `puppet_remove` lo purga y se descarta si llega más de 2 s tarde (`:166`, `:1383`).

Respuesta a las preguntas abiertas:

| Pregunta | Respuesta |
|---|---|
| ¿Qué guarda el receptor? | Un mapa por ranura (no solo el último cambio), con las retiradas como `0`. Se funde en `last_spawn_`. Le falta saber qué ranuras tenía ocupadas la plantilla. |
| ¿El emisor reenvía su personaje? | No. Solo cuando carga plantillas nuevas (partida o pantalla de carga). Todas las recargas de la prueba usan la plantilla de la conexión: "generation 1" en el anfitrión y "generation 2" en el invitado. |
| ¿"applied after spawn" llega al juego? | Llega: se ve inyectado en las capturas, 1 s después del `ActorLoadDataAck` y 0,85 s después de que la copia ya exista (el `OnDemandEquipDockingGimmickSpawnReq` de la copia sale a los 0,155 s). **No se aplica** (§3). |
| ¿Cubre la cabeza de conejo? | Sí. Es equipo normal de la **ranura 3**. Casi seguro es el itemKey `0x196F0`: está en la lista de equipo de la plantilla (`+0x1763`…) y el invitado se lo pone y se lo quita en la ranura 3 a las 21:45:17 y a las 21:56:13. No cubre tinte, encantamiento ni las opciones de "ocultar casco" (§6). |

## 2. Pruebas: logs y capturas

Correspondencia de tiempos entre capturas y logs, obtenida cruzando inyecciones conocidas:
- captura del anfitrión `cdmp_packets_20261005-204149`: t = 0 ≈ **20:42:02,7** en la hora del
  anfitrión;
- captura del invitado `cdmp_packets_20261005-202422`: t = 0 ≈ **20:24:46,8** en la hora de
  Shadow, que va una hora por detrás (**21:24:46,8** en la del anfitrión).

Todos los paquetes de la tabla son S>C inyectados en la copia `0xA010F001`. La serie va en
hexadecimal. Los payloads de 2757 tienen la forma `04 | clave | serie | {item, ranura}…`.

### 2.1 Copia del invitado en el anfitrión (MARK 2)

| t (s) | Hora del anfitrión | Mensaje | Contenido | Serie |
|---|---|---|---|---|
| 2653,994 | 21:26:16 | `ActorLoadDataAck` de la copia | plantilla del invitado: serie de equipo `+0x1763` = **`0x2526`**, contiene `0x196F0` | — |
| 2886,044 | 21:30:08,7 | 3073 | quitar ranura **3** (la cabeza) | 252B |
| 2888,783 | 21:30:11,5 | 2757 | `[{F42E4,4},{0,4}]` (cambio: la 4 **estaba ocupada**) | 252C |
| 2891,350 | 21:30:14,1 | 2757 | `[{1B23D,4},{0,4}]` | 252D |
| 2894,121 | 21:30:16,9 | 2757 | `[{F42E4,4},{0,4}]` | 252E |
| 3359,595 | 21:38:02,4 | `ActorLoadDataAck` | recarga por desmontar: misma plantilla (serie `0x2526`) | — |
| **3360,596** | 21:38:03,4 | **2757 "applied after spawn"** | **`[{0,3},{F42E4,4}]`**: falta el `{0,4}` | **252E** (= la del último cambio) |
| 3380,081 | 21:38:22,9 | 2757 tras la recarga de las 21:38:20 | igual | 252E |
| 3468,528 | 21:39:51,3 | 2757 tras la recarga por atasco de las 21:39:48 | igual | 252E |
| 4365,522 | 21:54:48,3 | 2757 tras desmontar | `[{0,3},{F42E4,4},{1D8B5,5}]` | 2539 (= la del último cambio, 3795,366) |
| 4655,988 / 4671,043 | 21:59:38 / 21:59:53 | 2757 tras desmontar | `[{0,3},{F42E4,4},{1D8B5,5}]` | 254E (= la del último cambio, 4453,042) |

En el log del anfitrión salen las mismas líneas:
- 21:30:08–21:30:16: `equipment removed (slot 3)` y `equipment change (2 slot(s))` tres veces;
- 21:38:02,405, 21:38:21,893 y 21:39:50,080: `Puppet: 2 equipment slot(s) changed since the sender's
  load, applied after spawn`;
- 21:38:44: MARK 2.

### 2.2 Copia del anfitrión en el invitado (MARK 4)

| t (s) | Hora del anfitrión | Mensaje | Contenido | Serie |
|---|---|---|---|---|
| 1361,773 | 21:47:28,6 | 3073 | quitar ranura **4** (estaba ocupada) | 253B |
| 1362,362 | 21:47:29,2 | 3073 | quitar ranura **3** (estaba ocupada) | 253C |
| 1364,867 | 21:47:31,7 | 2757 | `[{F45B2,4}]` (ranura vacía: puesta suelta) | 253D |
| 1366,681 | 21:47:33,5 | 2757 | `[{238E,4},{0,4}]` | 253E |
| 1369,465 | 21:47:36,3 | 2757 | `[{1B23D,4},{0,4}]` | 253F |
| 1374,181 | 21:47:41,0 | 2757 | `[{F4543,3}]` | 2540 |
| **1509,636** | 21:49:56,5 | **2757 "applied after spawn"** (recarga de las 20:49:55 en Shadow) | **`[{F4543,3},{1B23D,4}]`**: no lleva ninguna retirada | **2540** (= la del último cambio) |
| 1645,009 | 21:52:11,8 | 2757 tras la recarga de las 20:52:10 en Shadow | igual | 2540 |

MARK 4 (21:50:10 del anfitrión) cae 14 s después de la primera recarga. Este caso es la prueba
más útil:
- el mensaje **no contiene ninguna retirada**, así que no puede fallar por quitar una ranura
  inexistente;
- las dos puestas van a ranuras que **la plantilla tenía ocupadas**: el anfitrión empezó
  quitando la 4 y la 3 con 3073;
- y aun así la copia sale con la ropa de la conexión.

### 2.3 Lo que descartan los datos

- **No es que no llegue.** Los 2757 de después del spawn están en las dos capturas, con la
  clave, el tamaño (9 + 6n) y la serie correctos.
- **No es un problema de tiempos.** La copia ya existe cuando llega: su
  `OnDemandEquipDockingGimmickSpawnReq` se ve a 3359,750 (+0,155 s) y el 2757 a 3360,596. Aunque
  la copia no hubiera terminado el login, `0xBBF8D0` lo mete en la cola de reintentos (§1.3 de
  `apariencia-estado-coop.md`).
- **No es la serie de la plantilla.** La plantilla tiene `0x2526` y los 2757 de después del
  spawn llevan `0x252E`, `0x2539`, `0x254E` y `0x2540`, todas mayores.
- **No se pierde por la cola de movimiento.** El paquete está en la captura. El
  `movement=true` es un riesgo aparte (§5.3).
- **No es la caché pidiendo una lista ajena.** Solo hay un `ClientCacheReq` en toda la sesión,
  en el primer spawn (2654,006). Es para el characterNo `0x7FFFFFFFFFFFF001` de la copia, y la
  respuesta es vacía: serie `0xFFFFFFFF` y 0 entradas. En las recargas no hay ninguno.

## 3. Por qué no se aplica

### 3.1 Causa principal: puesta en una ranura ocupada sin su retirada

Así es como el cliente aplica un 2757 (`apariencia-estado-coop.md` §1.3 y §5.2, y el volcado
`build/research/appearance/cli_equipapply2_980a50.asm`):

1. `0x99F500` divide la lista en *poner* (itemKey ≠ 0) y *quitar*, y llama a
   `0x980A50(comp, &serie, poner, quitar)`.
2. `0x980A50` recorre las retiradas (`0x980AF1`-`0x980C90`) y las pasa por
   `0x980500(comp, out, ranura, 0)`, que **no confirma todavía** (4.º argumento 0; el camino 3073
   pasa 1). Si una falla, sale con el error (`0x980CB7`).
3. Después recorre las puestas (`0x980DB0`-`0x980ED6`) con la vfunc `+0x140`, virtualizada por
   Denuvo. **Si una falla, salta a `0x980FA2` y sale** sin llamar a `0x981290`, que aplica las dos
   listas, ni al refresco del acoplamiento y lo visual (`0x97C4B0`, `0x982810` y `0x9841C0`). Se
   pierde el mensaje entero, retiradas incluidas.

El servidor **siempre** acompaña una puesta en una ranura ocupada con `{0, ranura}`. Se ve en
todos los cambios de la tabla, por ejemplo `[{F42E4,4},{0,4}]`, y en `EquipItemAck`, que "pone
0x7F y retira 0x12" (§5.1 del mismo documento). Las puestas sueltas solo aparecen en ranuras
vacías, por ejemplo `{F45B2,4}` justo después de quitar la 4. Que `+0x140` falle con la ranura
ocupada es **(suposición fuerte)**, porque la función está virtualizada. Encaja con los dos
casos:

- **Invitado → anfitrión:**
  - se manda `[{0,3},{F42E4,4}]`;
  - la ranura 4 de la plantilla está ocupada y no lleva `{0,4}`, así que la puesta falla y se
    aborta todo;
  - la cabeza (ranura 3) **no** se quita y la ranura 4 conserva su pieza original.
  - Resultado: la ropa de la conexión.
- **Anfitrión → invitado:**
  - se manda `[{F4543,3},{1B23D,4}]`, dos puestas en ranuras ocupadas;
  - se aborta.
  - Resultado: la ropa de la conexión.
- **En vivo funciona** porque cada mensaje es el del servidor, que lleva su `{0,r}`, y la copia
  está en el mismo estado que el emisor.

El fallo está en cómo se resume el historial. Se pliega en un "estado final por ranura"
(`local_capture.cpp:269-274` y `coop.cpp:38-51`) y se vuelve a emitir como **una entrada por
ranura** (`local_capture.cpp:786-789` y `coop.cpp:59-65`). Con eso se pierde la retirada
necesaria. Ese resumen solo vale si la copia parte de un estado en el que esas ranuras están
vacías, y una copia recién cargada parte de la plantilla.

### 3.2 Causa secundaria posible: la serie no es mayor

La puerta de `0x99F500` es `serie > comp+0x18`, con signo. Además, `comp+0x18` se actualiza
antes de aplicar. Los 2757 de después del spawn reutilizan **exactamente** la serie del último
cambio en vivo, en los cuatro casos.
- Si `comp+0x18` de la copia nueva sale de la plantilla (`0x2526`), pasa.
- Si sale de la entrada de `EquipSlotListCache` (indexada por characterNo `…F001` y viva
  entre recargas) y algún camino de aplicación la actualizó, la serie es igual y se
  descarta con `eErrNoInvalidCachedSequenceNo`.

Que no haya `ClientCacheReq` en las recargas indica que la caché da por válida su entrada, pero
no sabemos qué serie guarda **(suposición)**. El arreglo de §5 cubre los dos casos, y el
diagnóstico de §5.4 dirá cuál de los dos era.

## 4. Alternativas valoradas

| Opción | Valoración |
|---|---|
| Que el emisor mande plantillas nuevas al desmontar o cada cierto tiempo | **No es posible** sin forzar al servidor a generar un `ActorLoadDataAck` propio, que nuestro cliente procesaría (§1.5 de `apariencia-estado-coop.md`, "no recomendado"). Las plantillas solo cambian al cargar. |
| Parchear la lista de equipo dentro de la plantilla (`+0x1763` serie, 15 entradas de tamaño variable desde `+0x1773`) | Funcionaría con cualquier recarga, pero hay que decodificar el registro de ítem y recalcular el tamaño anidado. Es mucho más frágil que reinyectar. |
| No recargar al jinete al desmontar | Reduce cuántas veces pasa, pero **no lo arregla**. Las recargas por atasco (21:39:48), por copia desaparecida, por vuelta tras un silencio y por Ctrl+F9 usan el mismo `last_spawn_` y fallan igual. La recarga al desmontar existe porque `RideOff::Enter` falla en la copia (`remote_puppet.cpp:148-150`). Es otro frente. |
| Mandar el mapa completo con sus retiradas, una serie que crezca y verificación | **Recomendada** (§5). Usa el mismo formato de mensaje, vale para todas las recargas y es idempotente. |

## 5. Arreglo propuesto

### 5.1 El mapa recuerda si la plantilla tenía la ranura ocupada

La lógica es la misma en el emisor y en el receptor. Conviene sacarla a un módulo puro que se
pueda probar, por ejemplo `src/common/equip_state.{h,cpp}` sin Windows, y usarlo desde
`note_equip`, `capture_spawn_info` y `merge_equip`.

```text
struct SlotState { uint32_t item; bool had_at_load; };   // item 0 = vacía ahora
map<u16 ranura, SlotState>

apply(change = 04|key|serial|n×{item,ranura}):
  para cada ranura r tocada por el cambio:
    si r no está en el mapa:
      had_at_load = el cambio contiene {0, r}
      // primera operación sobre r: con retirada → había algo en la carga
  retiradas primero (item = 0) y luego puestas (item = x)   // como ahora
  serial = max(serial, change.serial)

snapshot() -> payload 2757 (mismo formato 9 + 6n):
  para cada r: si item != 0  → {item, r}
               si had_at_load → {0, r}    // la misma ranura puede salir dos veces, como en el servidor
  (item == 0 && !had_at_load → nada: estaba vacía y sigue vacía)
```

- Al leer un snapshot (`merge_equip`, que parte de `info.equip`), un `{0,r}` marca
  `had_at_load = true`. El snapshot se puede volver a leer sin perder nada.
- Emisor:
  - `g_equip_slots` pasa a ser `map<u16, SlotState>` (`local_capture.cpp:166`);
  - `note_equip` (`:260-279`) usa `apply`;
  - `capture_spawn_info` (`:785-790`) usa `snapshot()`.
- Receptor: `merge_equip` (`coop.cpp:31-67`) usa `parse(snapshot)`, luego `apply(change)` y luego
  `snapshot()`.
- El formato de `SpawnInfo.equip` no cambia de tamaño ni de estructura. Aun así conviene
  **subir `kVersion`** (`protocol.h:28`): un PC con la versión vieja seguiría plegando la
  retirada y la pareja se vería con el fallo.

Lo que debería salir con los datos de la prueba:
- invitado (§2.1): `had` = {3, 4}, final {3: 0, 4: F42E4} → `[{F42E4,4},{0,3},{0,4}]`;
- anfitrión (§2.2): `had` = {3, 4}, final {3: F4543, 4: 1B23D} → `[{F4543,3},{1B23D,4},{0,3},{0,4}]`.

### 5.2 Inyección tras el spawn: retiradas por 3073 y puestas por 2757

En `puppet_spawn` (`remote_puppet.cpp:749-756`), en lugar de un único 2757:

1. Por cada `{0, r}` del snapshot, un **`OtherUnequipItemAck` (3073)** (`u32 clave | u16 r | u32
   serie`). Llama a `0x980500(…, 1)`, confirma al momento y **no mira la serie** (§1.3). Si la
   ranura ya estuviera vacía, falla solo ese mensaje.
2. Detrás, con la misma hora (`enqueue_at` es estable para horas iguales), **un 2757 solo con las
   puestas**. Para entonces esas ranuras ya están vacías, que es el mismo estado en el que
   funcionan las puestas sueltas en vivo.
3. Las dos cosas con **`movement=false`**, para que `purge_movement` no las purgue y no las tire
   el descarte de más de 2 s (`:1383`). También conviene adelantarlas: la copia existe a los
   0,155 s, así que basta con `now + 300` en lugar de `+1000` (opcional).
4. Hay que cambiar el log: `Puppet: after spawn: N removal(s) (3073), M item(s) put (2757, serial …)`.

Opcional y conservador: un 2757 por ranura en lugar de uno con todas, para que una puesta mala
no arrastre a las demás. Pero cada mensaje necesita una serie estrictamente mayor que la
anterior (§5.3), así que solo merece la pena si el diagnóstico demuestra que hace falta.

### 5.3 Serie de las inyecciones: siempre creciente en el receptor

En `remote_puppet.cpp`, para **todo** 2757 que se inyecte, tanto en vivo (`:1233`) como tras el
spawn:

```text
g_equip_serial_out = max(g_equip_serial_out + 1, serie_del_emisor)
reescribir payload[5..9] = g_equip_serial_out
```

- Solo se reinicia al cambiar de pareja (`forget_partner`) o con una plantilla nueva.
- Así cada mensaje supera al anterior **en la misma copia** y en cualquier copia que heredara la
  serie por la caché. Esto cubre la causa secundaria de §3.2.
- Nunca baja de la serie real del emisor, así que la plantilla (`0x2526`) siempre queda por debajo.
- El 3073 no la necesita, pero puede llevar la misma por coherencia.
- Riesgo **(suposición)**: ninguno conocido. `comp+0x18` solo se compara con nuestras propias
  inyecciones.

También hay que pasar a `movement=false` las inyecciones de equipo en vivo (`:1229` y `:1233`).
Hoy, una pausa de más de 2 s en un menú las tira y la copia se queda con la ropa vieja hasta la
siguiente recarga.

### 5.4 Diagnóstico para confirmar (de solo lectura y con SEH)

Unos 1,5 s después de cada inyección de equipo, y siempre tras el spawn, hay que leer la copia
con `game::find_actor(kPuppetPlayer)`, con lecturas seguras:

- `comp = [[actor+0x68]+0x38]` (`ClientEquipSlotActorComponent`, vtable `0x055BCC38`);
- `serie = [comp+0x18]`;
- `lista = [comp+0x90]`: `datos = [lista+8]`, `n = [lista+0x10]`, paso `0xD0`, ranura u16 en
  `+0xC8`. Esa estructura es la que recorre `0x980A50` en `0x980AF1`-`0x980B28`.
- Hay que escribirlo en el log así: `Puppet: copy equip component: serial 0x…, slots [..]`.

Con eso se distinguen las dos causas:

| Lectura tras el spawn (antes del arreglo) | Conclusión |
|---|---|
| serie = la del 2757 y ranuras = las de la plantilla (sigue la 3, etc.) | Se pasó la puerta de serie pero `0x980A50` abortó: **causa principal** |
| serie anterior (≥ la del 2757) y ranuras de la plantilla | Lo rechazó la puerta de serie: **causa secundaria** |
| ranuras correctas pero el aspecto viejo | Problema visual o de acoplamiento: otra línea de investigación |

## 6. Apariencia que no es "equipo"

- **La cabeza de conejo sí es equipo.** Va en la ranura 3 y la mueven `EquipItemReq` y
  `UnequipItemReq`, así que la cubren el hook del escritor de 2757 y el camino 2280→3073. Que
  aparezca en la recarga no es un hueco de cobertura: es la retirada perdida de §3.1.
- **No viajan:**
  - tinte, encantamiento y durabilidad (2757 solo lleva el itemKey; los `DyeItem*Ack` solo van
    al propio jugador, §1.5 del documento de apariencia);
  - el interruptor de "mostrar casco" (`_showHelmOnBattleStance`);
  - los eventos `ClientFrameEventSetVisualEquipItem` y `_visualEquipItemInfo` (§2 del mismo
    documento).
  - Tras una recarga, esas cosas salen como en la plantilla. Es una limitación aparte y conocida.

## 7. Plan de pruebas

### 7.1 Tests unitarios (`tests/tests.cpp`, con el módulo puro de §5.1)

- **Secuencia del invitado:**
  - entrada: 3073 r3, luego `[{F42E4,4},{0,4}]`, luego `[{1B23D,4},{0,4}]`, luego
    `[{F42E4,4},{0,4}]`;
  - snapshot esperado: `{F42E4,4}`, `{0,3}`, `{0,4}`.
- **Secuencia del anfitrión:**
  - entrada: r4 quitada, r3 quitada, luego `{F45B2,4}`, luego `[{238E,4},{0,4}]`, luego
    `[{1B23D,4},{0,4}]`, luego `{F4543,3}`;
  - snapshot esperado: `{F4543,3}`, `{1B23D,4}`, `{0,3}`, `{0,4}`.
- **Ranura vacía en la carga:** `{196F0,3}` y luego `{0,3}` dan un snapshot vacío.
- **Ranura vacía en la carga que acaba ocupada:** `{1D8B5,5}` da `{1D8B5,5}` sin retirada.
- **Fusión en el receptor:** `merge(snapshot del emisor, cambio)` da lo mismo que `apply` sobre
  todo el historial. Volver a leer un snapshot debe ser idempotente.
- **Serie de salida:** con entradas `0x2540, 0x2540, 0x2541` salen `0x2540, 0x2541, 0x2542`.
- Compilar y pasar los tests con `.\scripts\build.ps1`.

### 7.2 Un PC, contra `cdmp_peer` por loopback

1. Antes, copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save` en `backups/`, porque se inyecta en
   memoria.
2. Arrancar `cdmp_peer`, el juego y la partida. Esperar a la copia.
3. Quitarse la cabeza (r3), cambiar dos veces la pieza de la r4 y ponerse algo en una ranura vacía.
4. Forzar recargas:
   - montar y desmontar el caballo;
   - o, con `[dev] minimap_cycle=1`, pulsar Ctrl+F9, que llama a `puppet_request_reload`.
5. Comprobar en `bin64\cdmp.log`:
   - `after spawn: N removal(s)…`;
   - `copy equip component: serial … slots …`: sin la r3 y con la r4 nueva;
   - en la captura: 3073 con la r3 y la r4 y luego un 2757 solo con puestas.
6. Verificación visual con `CopyFromScreen` (2560×1440), recortando la copia: sin cabeza de
   conejo y con la pieza de la r4 actual.
7. Hacer el diagnóstico de §5.4 también **antes** de aplicar el arreglo (con el código actual)
   para saber cuál de las dos causas era.

### 7.3 Dos PC: anfitrión y Shadow (`docs/PRUEBA-DOS-PC.md`)

- Repetir A5 y A6 en los dos sentidos. Cada jugador cambia la cabeza y la ropa, monta y
  desmonta. El otro pulsa Ctrl+F12 tras cada recarga.
- Añadir B1: viaje rápido y vuelta, para la recarga por desaparición.
- Esperado en los dos logs:
  - `after spawn: …` con retiradas;
  - componente de la copia con las ranuras actuales;
  - en pantalla, la ropa actual y no la de la conexión.
- Recoger los logs de Shadow desde la carpeta compartida de logs (Shadow va una hora por detrás).

## 8. Otra observación (no investigada)

En la recarga de las 21:38:02 (anfitrión), aunque salió `the horse copy stays while its rider
reloads`, no se reutilizó el caballo. Salió `horse prepared …` y se inyectó otro `ActorLoadDataAck`
del caballo (`0xB010F002`) a las 21:38:05,5, con el anterior aún en el mundo. Pasó lo mismo a las
21:38:21, 21:39:50 y 21:54:47. `find_actor(kPuppetHorse)` debió de fallar en
`puppet_spawn` (`remote_puppet.cpp:732`). Hay que revisarlo aparte: puede crear caballos
duplicados o un "Actor creation failed".

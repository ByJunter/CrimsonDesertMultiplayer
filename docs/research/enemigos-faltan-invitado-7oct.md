# Enemigos que faltan en el invitado (prueba del 7 oct, tarde)

Build del juego **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Mod 0.1.0, build **`6B865217`**
(commit `6b86521`), protocolo 9, en los dos PC. Entre `6b86521` y `HEAD` (`aa4ddb5`) solo cambian
`calm_pose_locked`, el doblado del árbol, el minimapa y `loot_twin`. Nada de lo que se analiza aquí
cambió.

- Anfitrión = PC principal (`[dev] enemy_relay=1`, `[coop] enemy_auto=1`).
- Invitado = Shadow (`[coop] suppress=1`, zona de 150 m que se apaga a 170 m, `enemy relay false`).
- Campamento: Hills of No Return (Hernandian Territory), alrededor de x ≈ −10 300…−10 220,
  z ≈ −2 380…−2 290.

Etiquetas: **[hecho]** = está en un log o en una captura (cito la hora o la línea). **[inferido]** =
sale de juntar varios hechos. **[hipótesis]** = falta comprobarlo.

Este documento se centra en **cuántos** enemigos faltan y **por qué**. Lo que ocurrió tras la muerte
del anfitrión (18:54:17), los nativos que se devolvieron y la regla de "holding a weapon" ya está en
`zona-tras-muerte-anfitrion.md`. Aquí no se repite.

---

## 0. Resumen

| # | Causa raíz | Enemigos que explica | Tipo |
|---|---|---|---|
| 1 | **Tope `kZoneMax = 24`** en el anfitrión, sin sustituir copias. El invitado, en cambio, oculta **todos** sus nativos de la zona. | **19** (de 18:49:50 a 18:53:45, 43 enemigos de zona frente a 24 copias), **30** a las 18:54:20 (54 frente a 24) y **16** en la sesión 3 (40 frente a 24) | Permanente mientras dura la zona |
| 2 | **Marcas de tiempo de las acciones iniciales** sin ajustar o con desbordamiento. Las copias se cargaron antes de que el invitado tuviera fiable su reloj de juego. | **8 de 24** copias a ~10 km (invisibles) hasta las 17:50:15–20; **1** (`0xB01F002C`) en (0, 0, 0) **para siempre** | Pasajero (7) y permanente (1) |
| 3 | **Carrera** entre `enemy_reset_partner` y `auto_tick` tras el corte de las 18:46:41. 24 copias se reenviaron con claves nuevas a un invitado que todavía no estaba. Se entregaron al volver a conectar, durante la pantalla de carga y a 1 200 m. | Es el origen de la causa 2 y de los "reenvíos" (pista 1). Además ocupó 18 de las 24 plazas con una selección antigua. | Disparador |
| 4 | **Las copias no se mueven hasta que se mueve el propio anfitrión**: `puppet_enemy_move` necesita `g_last_move`. | 0 enemigos menos, pero 18 copias congeladas durante 113 s y 6 durante 26 s ("NO move") | Pasajero |

Lo que **no** es causa (está en §7): el filtro de facción y origen (todos los Black Bear y Wolf
Tracker del campamento cumplen el predicado), el radio, las variantes excluidas, la caché de cargas
(máximo 167 entradas y ninguna carga con varios registros), el límite de 48 copias del invitado y los
civiles (no se copian ni se ocultan, a propósito).

**Lo que más pesa en la práctica.** En la sesión principal (de 17:49:48 a 17:54:36 en el reloj del
invitado) el anfitrión tenía **43** enemigos de zona a menos de 150 m (54 al final). El invitado ocultó
**33, luego 47** de sus nativos y solo recibió **24** copias. Esas 24 eran 18 copias viejas de la
carrera más 6 nuevas. De ellas, 8 estuvieron fuera de sitio durante el primer medio minuto y 1 durante
toda la sesión. Así que el invitado vio unos **23** enemigos de unos 43–54, más 2 Wolf Tracker nativos.

**Prueba de que esos enemigos existían y peleaban** [hecho]: entre las 18:50 y las 18:54, el anfitrión
registra **35 golpes** de sus enemigos a la copia del invitado. **31** de esos golpes son de **6 enemigos
sin copia**: `0xB0100714`, `0715`, `0769`, `0771`, `0779` y `07FF`. En el invitado aparecen como
`"the other player's enemy 0x00000000 hit us … ignored, its attacker is not a copy of their enemies
here"` (20 líneas, de 17:53:23 a 17:53:57). Por ejemplo, anfitrión 18:53:24.343 `our enemy 0xB01007FF
hit the other player's copy (attack 0xB265E87A)` corresponde a invitado 17:53:23.671 `enemy
0x00000000 hit us (attack 0xB265E87A …) ignored`. Es decir, al invitado lo atacaban enemigos invisibles.

---

## 1. Fuentes y relojes

| Qué | Ruta |
|---|---|
| Log del invitado | `cdmp.log` del PC del invitado, 7 oct, tarde (1 829 líneas, de 17:46:53 a 17:56:40) |
| Captura del invitado | `cdmp_packets_20261007-174707.bin` (354 325 paquetes, 544,6 s) |
| Log del anfitrión | `D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\cdmp.log` (1 874 líneas, de 18:42:47 a 18:56:24) |
| Captura del anfitrión | `…\bin64\cdmp_packets_20261007-184300.bin` (382 742 paquetes, 783,9 s) |

**El desfase entre relojes no es de 55 min 54 s. Es de ≈ +1 h 00 min 00,6 s (anfitrión = invitado
+ 1 h)** [hecho]. Coincide con `zona-tras-muerte-anfitrion.md`. La pareja que se usó al principio
(invitado 17:49:48 "zone ON" con anfitrión 18:45:42 "zone ON") no encaja. La "request 1" de las
18:45:42 es de una **sesión anterior del invitado** que no tiene log (ver §6). Las parejas buenas son:

| Invitado | Anfitrión | Qué |
|---|---|---|
| 17:47:18.487 `Session: connected to peer 'Jugador'` | 18:47:19.183 `Session: connected to peer 'Shadow'` | reconexión |
| 17:48:24.019 `sent our character (… spawn 1)` | 18:48:24.969 `received the other player's character (… spawn 1)` | personaje |
| 17:49:48.726 `the host answers request 1` | 18:49:49.417 `the guest's enemy zone ON (request 1)` | petición 1 de esta sesión |
| 17:49:49.376 `copy 0xB01F0031 queued` | 18:49:49.898 `sending enemy 0xB0100761 … as copy 0xB01F0031` | copia (la red tarda unos 50 ms) |
| 17:55:51.536 `the host answers request 2` | 18:55:52.186 `zone ON (request 2)` | petición 2 |

**Hora de las capturas** (`t` = segundos desde el primer paquete) [hecho]:
- Anfitrión: t0 = **18:43:17.133**. Ancla: los `RemoveActorAck` inyectados de la marioneta en
  t = 204,247 corresponden a `Inject: delivered opcode 2516` de las 18:46:41.380.
- Invitado: t0 = **17:47:31.862**. Ancla: la carga de `0xB01F0019` en t = 50,554 corresponde a
  `queued` de las 17:48:22.416, y la de `0xB01F0031` en t = 137,524 a la de las 17:49:49.376.

Los paquetes que inyecta el mod (cargas, acciones y movimientos de las copias) **sí** salen en la
captura, ya con la marca de tiempo final que recibe el cliente.

---

## 2. Línea de tiempo (las horas del invitado van entre paréntesis)

| Anfitrión | Qué pasa | Pruebas |
|---|---|---|
| 18:43:49 | Conecta la **sesión 1** del invitado. Su log no está. | log del anfitrión, línea 113 |
| 18:45:42.684 | La sesión 1 pide la zona: `zone ON (request 1, 150 m)`. El invitado se queda quieto en (−10 021,6, 715,6, −2 446,8), a unos 250 m del campamento. El anfitrión camina hacia el campamento. | líneas 304 y 352 y siguientes ("last remote" no cambia desde 18:45:52) |
| 18:46:33.563–18:46:40.853 | Se envían 24 copias, de `0x01` a `0x18`. Las 16 primeras van con **"0 recent actions"**. | líneas 418–449 |
| 18:46:37.582 | `reliable pending 64`, `pending 5865 B`: el invitado deja de confirmar mensajes. | línea 443 |
| 18:46:41.348 | `Session: peer timed out after 5000 ms`, seguido de `other side changed: 24 enemies to copy again` | líneas 450–454 |
| 18:46:41.427–41.428 | **Carrera**: se envían otras 24 copias, de `0x19` a `0x30`, con motivo "zone" y **sin nadie conectado** | líneas 458–481 |
| (17:46:53) | Arranca el juego del invitado otra vez (el log empieza de cero) | log del invitado, línea 1 |
| 18:47:19 (17:47:18) | Se reconecta. Se entregan las 24 copias que esperaban en la cola. El invitado no está en el mundo y las guarda. | log del invitado, líneas 75 y 106 |
| 18:47:22 / 18:48:02–04 | El anfitrión, ya fuera del modo zona, quita 6 copias con `is far` (a más de 80 m). Se borran de la espera del invitado. | líneas 532–535, 580 y 586 |
| (17:48:22.395) | `back in the world: loading 18 held enemy copies`. El reloj de juego del invitado aún no es fiable: `waiting for our own game clock` a las 17:48:22.867. | log del invitado, líneas 139 y 160 |
| (17:48:24–29) | Informes "exists": 8 de las 18 están mal colocadas, a 1 200 m o a 10 km | líneas 233–353 |
| 18:47:53–18:50:15 | El anfitrión **no se mueve** (ningún `MoveActorReq` propio) | captura del anfitrión |
| 18:49:49.417 (17:49:48) | La **sesión 2** pide la zona. El anfitrión ya tiene 18 copias en `g_sent` y solo añade 6 (`0x31`–`0x36`) hasta llegar a 24. | líneas 985–998 |
| (17:49:48.756) | `zone ON (host 5 m away, 25 natives to hide)`. El invitado retiene 33 nativos y luego 35, 39, 46 y 47. | log del invitado, líneas 725, 802 y 1146 |
| (17:50:15.5) | Llega el **primer** `MoveActorAck` de la copia del anfitrión y, en el mismo momento, el primero de cada copia de enemigo | captura del invitado, t = 163,676 |
| 18:49:49–18:54:37 | **No se envía ni una copia más** aunque haya entre 43 y 54 enemigos de zona | log del anfitrión: no hay líneas "sending" entre la 998 y la 1806 |
| 18:54:17 | Muere el anfitrión. La zona se apaga a las 17:54:36. | `zona-tras-muerte-anfitrion.md` |
| 18:55:52–55 (17:55:51) | **Sesión 3** de la zona: se envían 24 copias de golpe (`0x37`–`0x4E`). El invitado retiene 31 nativos y deja 8. | líneas 1798–1829 |

---

## 3. Causa 1: tope de 24 copias y sin sustitución (la que más pesa)

### 3.1 El código

`src/mod/net/enemy_relay.cpp`:
- l. 733: `constexpr size_t kZoneMax = 24;`
- `zone_tick` (l. 805–849): solo **añade** mientras `count < kZoneMax`. Una copia se quita cuando su
  enemigo deja de ser de zona, desaparece o se aleja más de 170 m. **Nunca se cambia** una copia lejana
  por un enemigo más cercano que no tiene copia.
- `zone_candidates` (l. 777–803) ordena así: primero los que pelean, después por cercanía a la copia del
  invitado (`d_guest`).

En el invitado, `native_suppress.cpp` oculta **todos** los nativos de zona a menos de `g_radius`
(150 m) del anfitrión (l. 237: `if (d > g_radius) return false`), sin límite comparable (`kMaxHeld`
es 512). Así se rompe la "regla de oro" de `enemy_load.h`: el anfitrión releva menos de lo que oculta
el invitado y el invitado ve el campamento vacío.

### 3.2 Recuento en el anfitrión (captura 184300) [hecho, reconstruido]

Simulé `g_loads` con la captura (`build/research/p_faltan7oct/hostzone.py`): cargas de un solo registro
de tipo 3 o 6, sin dueño y con clave del mundo, y las borré con cada `RemoveActorAck`. Las posiciones
salen de `MoveActorReq`/`MoveActorAck` y, si no hay, de la última Transform de la carga. Después apliqué
`zone_check`, portado tal cual de `enemy_load.cpp` con las tablas reales de `enemy_tables_2976.h`
(55 facciones, 957 variantes, 585 orígenes permitidos y 1 170 de precaución). Distancia horizontal a
`0xA0100001`, radio 150 m.

| Hora (anfitrión) | Tipo 3 a ≤ 150 m | **De zona** | Copias en `g_sent` | Enemigos de zona **sin copia** |
|---|---|---|---|---|
| 18:46:40 (sesión 1) | 44 | 23 | 24 | (la cola de envíos aún iba por la mitad) |
| 18:50:00 | 67 | **43** (32 Black Bear, 11 Wolf Tracker) | 24 | **19** |
| 18:51:30 | 68 | 43 | 24 | 19 |
| 18:53:00 | 67 | 43 | 24 | 19 |
| 18:54:20 | 76 | **54** (43 Black Bear, 11 Wolf Tracker) | 24 | **30** |
| 18:56:10 (sesión 3) | 69 | **40** | 24 | **16** |

- En 17 de las 18 instantáneas tomadas cada 15 s entre las 18:49:50 y las 18:54:15 salen 43 enemigos
  de zona, y 54 en la última. En total aparecen 54 claves distintas.
- Las 24 copias de la sesión 2 **sí** son de zona y están dentro del radio en todas las instantáneas.
  No sobra ninguna: simplemente no caben más.
- Los que faltan **no están lejos**. A las 18:53:00 hay enemigos sin copia a 28, 29, 30, 36, 42 y
  45 m del anfitrión (`0xB0100801`, `07FF`, `0800`, `0779`/`077A`/`077B`, `0771`, `0714`…). A las
  18:54:20, a 2, 5, 7, 9 y 10 m. El invitado estaba a unos 5–10 m del anfitrión.
- Los 6 que golpearon a la copia del invitado (§0) están en esa lista.

### 3.3 Lado del invitado [hecho]

- `Suppress: zone on | held 33 … swept 33` a las 17:49:53.884. Luego `held 35, dropped 2` a las
  17:50:03, `held 39` a las 17:53:14, `held 46` a las 17:53:24 y `held 47, dropped 16` a las 17:53:54.
- Si se simula la captura del invitado igual que la del anfitrión (los nativos retirados por la
  supresión desaparecen con su `RemoveActorAck`), entre las 17:50 y las 17:53 solo quedan **2** nativos
  de zona a la vista, dos Wolf Tracker.
- Total a la vista en la sesión 2: **24 copias, de las que 23 están bien colocadas** desde las 17:50:20
  (§4), más **2 nativos**. El anfitrión tenía entre 43 y 54. Eso es "muchos menos".

### 3.4 Arreglo propuesto

1. **Subir el tope y repartir los envíos** (`enemy_relay.cpp`):
   - `kZoneMax` de 24 a **48** (o 64), y `kMaxCopiesHere` del invitado (l. 1323) de 48 a
     `kZoneMax + 16`. Si no, el invitado rechazaría las de más (`"48 copies here already"`).
   - En `zone_tick`, como mucho **6 envíos por tick** (500 ms, unos 12 por segundo), del más cercano al
     más lejano. Hoy se envían los 24 de golpe en el mismo milisegundo (18:55:55.106–108).
   - **Sustitución con histéresis**: si se ha llegado al tope y hay un candidato a una distancia
     `d_guest` al menos 15 m menor que la copia más lejana durante 3 s seguidos, se quita esa copia
     (`remove_locked`) y se envía el candidato.
2. **Mantener la regla de oro aunque se llegue al tope.** El mensaje `EnemyZoneMsg` ya lleva `radius_m`
   en la respuesta. Si hay más de `kZoneMax` enemigos de zona, el anfitrión responde con un radio
   `r = distancia (a sí mismo) del enemigo número kZoneMax − 2 m`, con un mínimo de 50 m (lo exige
   `decode_enemy_zone`). El invitado usa ese radio en `suppress_on_answer` (`g_radius`). Para que los dos
   lados coincidan, el anfitrión tiene que ordenar y recortar por distancia a **sí mismo** (`d_me`, el
   centro de la zona del invitado), no por `d_guest`. `enemy_zone_update` ya vuelve a responder cuando
   algo cambia: hay que añadir "el radio cambió más de 10 m, como mucho una vez cada 5 s".
3. **Comprimir `EnemySpawn`.** Las 48 cargas de copias de la captura del invitado ocupan de media
   **8 311 B, y 1 111 B con zlib nivel 6 (13,4 %)** [hecho]. Con 54 copias se pasaría de unos 450 KB a
   unos 60 KB. Cambia el formato, así que hay que subir `kVersion`. Antes hay que comprobar qué
   compresor está ya en las dependencias (miniz o LZ4).

**Riesgos:**
- Ancho de banda: con 24 copias a la vez el anfitrión mostró `pending 190809 B` y una RTT de 321 ms
  (18:55:58), y la cola tardó unos 5 s en vaciarse. Con 54 sin comprimir serían unos 10 s de retraso
  en el canal fiable, que también lleva el movimiento del jugador.
- Más carga para el cliente del invitado. Además se multiplica el problema lateral de §8.1
  (`SetQuestDialogReq`).
- La sustitución hace aparecer y desaparecer copias en el borde. El radio dinámico devuelve y oculta
  nativos en el borde.
- Un radio de 50 m como mínimo puede seguir teniendo más de 48 enemigos en campamentos muy densos
  (a las 18:54:20 había unos 25 a menos de 55 m). Entonces hay que combinarlo con el punto 1.

---

## 4. Causa 2: copias en (0, 0, 0) o a 10 km por las marcas de tiempo (pista 3)

### 4.1 Qué se ve [hecho]

Informes "exists" de las 18 copias guardadas, cargadas entre las 17:48:22.4 y las 17:48:27.5 y
consultadas unos 2 s después (log del invitado, líneas 233–353):

| Copia | Enemigo del anfitrión | Acciones iniciales (marca en Ack+0x160) | "queued at" | "exists: at" | Proporción |
|---|---|---|---|---|---|
| `0x1B` | `0xB010070C` Black Bear | 198185, 202261 | (−10 267,7, 676,9, −2 353,8) | (−110,5, 7,3, −25,3) | 0,0108 |
| `0x20` | `0xB0100701` Wolf Tracker | 197713, 197746, 200551, 203333 | (−10 294,2, …) | (−112,6, 7,4, −25,8) | 0,0109 |
| `0x21` | `0xB01006F9` | 199283 … 202646 | (−10 284,1, …) | (−132,5, 8,7, −30,2) | 0,0129 |
| `0x22` | `0xB01006F8` | 199283 … 202614 | (−10 283,1, …) | (−148,2, 9,8, −33,8) | 0,0144 |
| `0x23` | `0xB01006F7` | 197134 … 202614 | (−10 283,3, …) | (−163,7, 10,8, −37,3) | 0,0159 |
| `0x24` | `0xB0100702` | 199283 … 202614 | (−10 294,0, …) | (−159,7, 10,5, −36,6) | 0,0155 |
| `0x25` | `0xB0100703` | 199283 … 202646 | (−10 296,4, …) | (−152,3, 10,0, −34,9) | 0,0148 |
| `0x2C` | `0xB010071C` | **18446744073709533702, …536584** (negativas, desbordadas) | (−10 282,3, 675,1, −2 328,8) | **(0,0, 0,0, 0,0)** | 0 |
| las otras 10 | | una acción (o dos separadas 35 ms) en 197002–203603, o desbordadas (`0x29`) | | en su sitio, a unos 1 200 m del invitado | 1 |

- Las acciones en directo que llegan después llevan marcas del reloj del invitado, ya ajustadas: 93 513
  en t = 56,5. Es decir, el reloj de juego del invitado iba por **unos 87 700 ms** al cargar las
  copias. Las marcas iniciales de 197 000–203 600 ms estaban, por tanto, **110–116 s en el futuro**.
  Las desbordadas (`0x29` y `0x2C`) están a unos 1,8 × 10¹⁹ ms: en la práctica, **nunca**.
- `0x2C` sigue en el origen 5 minutos después: el invitado envía `CreateProjectileFromFrameEventReq`
  de `0xB01F002C` con la posición **(0, 0, 0)** en t = 353,8 y 377,6 (17:53:25 y 17:53:49). En cambio
  `0x22` y `0x23`, que antes estaban fuera de sitio, disparan desde el campamento (−10 273, 677, −2 330)
  a partir de t = 361. **Las 7 que no desbordaron se corrigieron solas y `0x2C` nunca.**
- En las copias que se cargaron después ya no pasa: `0x31`–`0x36` (17:49:49) tienen sus 4 acciones
  iniciales con la **misma** marca (164355…164527), recortada a "ahora" por `InjectClock`, y quedaron
  a 54–59 m, en su sitio. `0x37`–`0x4E` (17:55:54) igual (516787) y además con "placed by a Move".

### 4.2 Por qué [inferido]

1. `coop.cpp` l. 90–96: al volver al mundo, `local_in_world_` pasa a verdadero y se cargan **en ese
   mismo momento** todas las copias guardadas (17:48:22.395). Pero el reloj de juego de este cliente
   todavía no es fiable: 472 ms después aparece `Co-op: waiting for our own game clock (move once)`
   (17:48:22.867) y `Time sync: waiting for the game clock` (17:48:24.280).
2. `remote_puppet.cpp`, `puppet_take_injection` (l. 2140): `game_now = capture_game_clock_trusted() ?
   … : nullopt`. Sin `game_now` **no se pasa por `InjectClock::stamp`** y la marca va tal cual.
3. `puppet_spawn_enemy` (l. 1244–1250) convierte las acciones guardadas con `start_action_ack`, que
   llama a `rebase_timestamp(ack, 0x160)` con el `g_ts_shift` **global**: vale lo que dejó el último
   evento procesado.
   - En las 16 primeras era **0**, porque aún no se había procesado ningún evento: el desfase del reloj
     se conoce a las 17:48:28.8 (`clock offset -217525 ms`). Por eso la marca es la del anfitrión, de
     hacía unos 100 s.
   - En `0x29` y `0x2C` ya valía unos −217 500 ms. Como las acciones grabadas son anteriores al arranque
     del juego del invitado, `198185 − 217525 < 0`, y el `uint64` **da la vuelta**.
4. `inject_clock.h` explica lo que hace el cliente: guarda un búfer de posiciones por actor, lo
   reproduce 200 ms por detrás, **descarta las muestras que no son más nuevas que la última** y nunca
   extrapola. Si la primera muestra está 110 s en el futuro, todas las demás se descartan hasta que el
   reloj del cliente llega a ella. Mientras tanto la copia interpola desde (0, 0, 0) hacia esa muestra.
   Eso explica proporciones de alrededor del 1 % a los 2 s [hipótesis sobre por qué la interpolación
   parte del origen: puede que sea la posición inicial del búfer del actor].
   - Por qué solo afecta a las copias con **varias acciones repartidas en más de 1 s** (`0x1B`: 4,1 s;
     `0x20`–`0x25`: 3–5,6 s) y no a las que tienen una sola acción o dos casi juntas (`0x1E`: 35 ms) es
     una [hipótesis]: con una sola muestra el cliente coloca el actor en esa muestra, y con dos
     interpola entre el origen del búfer y ellas.

### 4.3 Arreglo propuesto

1. **No cargar copias hasta que el reloj sea fiable.** En `enemy_relay.cpp`, `load_waiting()`
   (l. 1532): salir si `!capture_game_clock_trusted()`. Eso las deja esperando en `g_waiting`. Lo mismo
   para las guardadas en `coop.cpp` l. 90: exigir también que `capture_game_clock_trusted()` sea cierto
   y que `clock_.offset()` exista. Riesgo bajo: solo retrasa la carga unos segundos, y
   `kMaxCopiesHere` ya limita lo que puede esperar.
2. **Marcas de las acciones iniciales en "ahora".** En `puppet_spawn_enemy`, cada `StartActionAck`
   inicial tiene que llevar en Ack+0x160 el reloj de juego del cliente (`game::client_game_clock_ms()`),
   o el mismo valor para todas, igual que hace ya `InjectClock` cuando el reloj es fiable. Son acciones
   para la pose, no movimiento. Riesgo bajo.
3. **`rebase_timestamp` con saturación** (l. 390–396): calcular en `int64` y, si el resultado es menor
   que 1, poner 1. Así un valor negativo nunca acaba en 1,8 × 10¹⁹. Riesgo nulo.
4. **Red de seguridad**: `report()` ya ve el problema 2 s después ("10405 m from you"). Si una copia
   está a más de 1 km de la posición de su carga, se puede quitar y volver a cargar
   (`puppet_remove_actor` + reencolar), como hace `puppet_tick` con la copia del jugador. Riesgo medio:
   carga doble.

---

## 5. Causa 3: la carrera tras el corte de las 18:46:41 (pistas 1 y 4)

### 5.1 Por qué se reenvió `0xB010070E` (`0x11` y luego `0x19`) [hecho + inferido]

- 18:46:41.348 `Session: peer timed out` → `forget_partner` → `enemy_reset_partner()` (l. 1683–1711).
  Esa función vacía `g_sent`, pone en cuarentena los ids 1–0x18, vacía `g_outgoing` y deja
  `g_partner_zone = {}` (zona apagada).
- 79 ms después (18:46:41.427), el hilo de `enemy_test_loop` (T24720) envía **24 copias "zone"**
  (`0x19`–`0x30`) [hecho]. `auto_tick` (l. 875–893) había copiado `zone` (encendida) **antes** del
  reinicio. Luego `zone_tick` copió `g_sent` (ya vacío) y `zone_candidates(… all=false)` le devolvió
  los 24 de nuevo [inferido por el orden del código y los tiempos]. Las claves son nuevas porque
  `alloc_copy_key_locked` no reutiliza ids en cuarentena.
- Esas 24 se quedan en `g_outgoing` (que ya se había vaciado) y salen hacia el **siguiente**
  compañero al reconectar a las 18:47:19, porque `coop.cpp` l. 138–144 las vacía en cuanto hay
  conexión.
- Con la zona ya apagada, `auto_tick` sigue en el modo antiguo (copias "fighting you") y quita las que
  están a más de 80 m (`kAutoDropRadius`): `0x2A`, `0x2B`, `0x2D` y `0x2E` a las 18:47:22, y `0x30` y
  `0x2F` a las 18:48:02/04. Las 18 restantes se quedan en `g_sent` **como si fueran de zona**.
- A las 18:49:49 llega la petición real de la sesión 2. `zone_tick` ve 18 en `g_sent` y añade solo 6
  más.

### 5.2 ¿Quedaron duplicados o fantasmas en el invitado? [hecho]

- **No hay dos copias del mismo enemigo.** En la captura del invitado cada clave de `0x19` a `0x4E` se
  carga **una sola vez**, y ninguna de `0x01`–`0x18` llega a este proceso. Esas fueron a la sesión 1
  del invitado, que se cerró (§6).
- **Sí hubo dobles cortos**: entre la carga de las 18 copias (17:48:22) y la zona encendida
  (17:49:48.756) el invitado tenía su supresión apagada. Al llegar al campamento, a las 17:49:47 tenía
  **33 nativos de zona y 18 copias** de los mismos sitios. Duró hasta el barrido de las 17:49:48–53.
- **Fantasma permanente**: `0xB01F002C`, en el origen y disparando desde (0, 0, 0), hasta que se quitó
  a las 17:54:31.836.

### 5.3 Copias cargadas a 1 203 m y en la pantalla de carga (pista 4) [hecho]

- El invitado volvió al mundo en (−9 907, 550, −3 488). Las 18 copias se cargaron a 1 180–1 230 m de
  él (`1203.5 m from you`, por ejemplo). Ese invitado **no había pedido ninguna zona**: su primera
  petición es de las 17:49:48. Las recibió porque `enemy_on_spawn` / `coop.cpp` l. 514–519 aceptan
  cualquier `EnemySpawn`, esté donde esté el invitado y aunque no tenga la zona encendida.
- El invitado fue a caballo hasta el anfitrión (1,2 km en unos 77 s) y a las 17:49:47 ya estaba allí.

### 5.4 Arreglo propuesto

1. **Época del compañero** (`enemy_relay.cpp`): un `uint32_t g_partner_epoch` protegido por `g_mutex`
   que se incrementa en `enemy_reset_partner`. `auto_tick` lee `zone` y la época bajo el mismo bloqueo.
   `send_enemy(key, why, epoch)` comprueba, bajo el bloqueo donde mete la entrada en `g_sent`, que
   `epoch == g_partner_epoch`, y que `g_partner_zone.on` sigue igual si el motivo es "zone". Si no,
   aborta. Además, cada entrada de `g_outgoing` lleva su época y `enemy_take_outgoing` tira las de
   épocas anteriores. Riesgo bajo.
2. **El anfitrión no copia por zona hasta que la sesión actual lo pide**: ya es la intención de
   `g_partner_zone = {}`. Con el punto 1 se cumple.
3. **Invitado: no cargar copias lejanas.** En `load_waiting`, dejar esperando, en vez de cargar, una
   copia cuya Transform de carga esté a más de, por ejemplo, 300 m de nosotros. Se carga cuando nos
   acercamos o se descarta si llega su `EnemyRemove`. Riesgo: una copia de "fighting you" siempre está
   cerca, así que no le afecta. La espera ya tiene límite (`kMaxCopiesHere`).

---

## 6. La sesión 1 y el reinicio del juego del invitado (pregunta abierta)

- [hecho] El invitado deja de confirmar mensajes entre las 18:46:32 (`reliable pending 0`) y las
  18:46:37 (`reliable pending 64`). Las primeras copias salieron a las 18:46:33.563. El log del invitado
  empieza de cero a las 17:46:53 (el anfitrión iba por las 18:46:54), así que **el proceso del juego
  del invitado terminó a las 18:46:3x y se volvió a abrir**.
- [hipótesis] Pudo ser un **cuelgue o cierre provocado por las copias**. Las 16 primeras iban **sin
  ninguna acción** y llegaron a un invitado a 250 m. En otras pruebas las copias sin acción cargaron
  bien (`copias-enemigos-faltan-pose-minimapa.md`), así que no está demostrado. También pudo ser que el
  juego se cerrara a mano.
- **Falta comprobar** si el juego de Shadow se cerró solo, y traer de Shadow el log de esa
  sesión (`bin64\cdmp.1.log`, si allí también rota) y el informe de crashpad si existe.

---

## 7. Lo que se descartó (pista 2)

| Sospecha | Resultado | Pruebas |
|---|---|---|
| Facción hostil o "apuntó en 120 s" | En la zona solo cuenta la facción (`zone_check`). `enemy_hostile_in_combat` (120 s) se usa para el combate y no interviene. Black Bear = facción 1000148 y Wolf Tracker = 1000018: los dos están en `kHostileFactions`. | simulación |
| Orígenes no permitidos | Los 6 hashes de horario de los Black Bear (`0xE3CC5DC8`, `0xBE62E27F`, `0xA0F3AE60`, `0x174A520B`, `0x50D848D5`, `0x26D69BD3`) **están** en `kAllowedOrigins`. Todos los Black Bear y Wolf Tracker a ≤ 150 m dan "zona". | simulación (al principio parecían no permitidos por un error en mi lector de la tabla en hexadecimal, ya corregido) |
| Variantes excluidas (jefes) | Ninguna en el campamento | simulación |
| Radio | Los que faltan estaban a 2–117 m | §3.2 |
| Caché de cargas | Como mucho **167** entradas (`g_loads` se vacía a partir de 1 500) y ningún vaciado. **No hay ninguna** `ActorLoadDataAck` con varios registros de tipo 3 (esas no se guardan). | simulación |
| Riders o torres ("NOT copied") | Ninguna línea "NOT copied" en el log del anfitrión | grep |
| Límite del invitado (`kMaxCopiesHere` = 48) | No se llegó: 24 copias | log |
| Límite de ritmo de carga (8 de golpe y luego 2 por segundo) | Solo retrasa: las 24 de la sesión 3 tardaron unos 8 s, de 17:55:54.5 a 17:56:02.5 | log |
| Civiles y animales | Pailune Refugee (unos 10), Female Deer y Deer (que son tipo 3), Wandering Freesword, Haldin y Hernandian Guard (facción 1000131, no hostil) no se copian ni se ocultan, **a propósito**. El invitado ve los suyos. | simulación de los dos lados |
| Prueba de la mañana (`cdmp.1.log`, 07:19) | 16 copias (6 Blinding Arrow y 10 Hernandian Guard), por debajo del tope. Allí no pasaba esto. | log del anfitrión de la mañana |

**Causa 4 (movimiento) [hecho].** `remote_puppet.cpp` l. 1289: `puppet_enemy_move` termina sin hacer
nada si `g_last_move` (el último Move de la **copia del anfitrión**) está vacío, y `puppet_spawn_enemy`
avisa con "NO move". El anfitrión no envió ningún `MoveActorReq` propio entre las 18:47:32.966 y las
18:50:15.838. En el invitado, el primer `MoveActorAck` de la copia del anfitrión y el primero de
**todas** las copias de enemigo llegan a la vez (t = 163,7, 17:50:15.5). Hasta entonces las 18 copias
guardadas estuvieron **113 s** congeladas y `0x31`–`0x36`, **26 s**. **Arreglo:** usar como plantilla
el último `MoveActorReq` (514 B) de **nuestro** propio jugador cuando `g_last_move` esté vacío. Hace
falta una función nueva en `local_capture` (por ejemplo `capture_own_move_template()`). Los campos que
dependen del actor ya se reescriben (+0x00, +0x04, +0x10, +0x20, +0x4C, +0x58, +0x68, +0x8C, +0x94 y
+0xD5). Riesgo medio: hay que probarlo antes con `cdmp_peer`.

**Sesión 3 (después de la muerte)** [hecho, con dudas]. A los 2 s de cargar, 5 copias tenían `state
0x8, login 0` (`0x37`, `0x39`, `0x3A`, `0x3C` y `0x46`), `0x38` tenía `Status+390 0x2` (bandera de
oculta) y 5 aún no estaban en la lista de actores (`0x41`–`0x45`). La lista de actores no tenía la
carga porque se **entregó tarde**: por ejemplo, `0x41` se encoló a las 17:55:56.031 y llegó a las
17:56:00.511. Si esos estados eran solo pasajeros es una [hipótesis] que no se puede cerrar, porque la
captura termina a las 17:56:07. `0x37` y `0x38` estaban peleando con el anfitrión y murieron a las
17:56:19 y 17:56:15.

---

## 8. Hallazgos laterales

1. **El cliente del invitado envía a su servidor peticiones sobre las copias** [hecho]. En la captura
   del invitado hay **20 642** `SetQuestDialogReq` (2857, 4 B = clave), y **20 500** son de claves
   `0xB01F00xx`: unas 900 por cada copia de `0x19`–`0x36`, unas 2,5 por segundo y copia. En el
   anfitrión hay 184 en toda la sesión, todas de nativos. Las copias de `0x37`–`0x4E` no las provocan
   (en los 40 s que se ven). Es tráfico hacia un servidor de Pearl Abyss que nombra actores que ese
   servidor no tiene. Hay que investigarlo aparte: qué campo de la carga lo provoca y por qué solo en
   unas copias.
2. **`0x2C` disparaba desde (0, 0, 0)**: el cliente del invitado envió `CreateProjectileFromFrameEventReq`
   con esa posición a su servidor.
3. El desfase de 55:54 que se usó al principio era erróneo (§1). Conviene escribir siempre una pareja
   de anclas de verdad en `PRUEBA-DOS-PC.md`.

---

## 9. Arreglos por orden de impacto

| Orden | Cambio | Dónde | Enemigos que recupera | Riesgo |
|---|---|---|---|---|
| 1 | Tope 48–64, 6 envíos por tick, sustitución con histéresis y radio dinámico en la respuesta, para cumplir la regla de oro | `enemy_relay.cpp` `kZoneMax`, `zone_tick`, `zone_candidates` (ordenar por `d_me`), `zone_answer_locked`/`enemy_zone_update`; `native_suppress.cpp` `suppress_on_answer` (`g_radius`); invitado `kMaxCopiesHere` | 16–30 por campamento | Ancho de banda (comprimir: 13 %, sube `kVersion`), carga del cliente, apariciones en el borde |
| 2 | Cargar copias solo con el reloj fiable, marcas iniciales en "ahora" y `rebase_timestamp` con saturación | `enemy_relay.cpp` `load_waiting`; `coop.cpp` l. 90; `remote_puppet.cpp` `puppet_spawn_enemy`, `rebase_timestamp` | 1 permanente y 7 durante unos 2 min tras una pantalla de carga | Bajo |
| 3 | Época del compañero para cortar la carrera y tirar los mensajes de una época anterior | `enemy_relay.cpp` `enemy_reset_partner`, `auto_tick`, `send_enemy`, `enemy_take_outgoing` | Evita las 18 copias "viejas" y la causa 2 | Bajo |
| 4 | No cargar copias a más de 300 m de nosotros | `enemy_relay.cpp` `load_waiting` | Evita cargas a 1 200 m | Bajo |
| 5 | Plantilla de Move de nuestro propio jugador si `g_last_move` está vacío | `remote_puppet.cpp` `puppet_spawn_enemy`/`puppet_enemy_move`; `local_capture` (función nueva) | Copias que se mueven aunque el anfitrión esté quieto | Medio |

**Cómo comprobarlo en la próxima prueba:**
1. En el anfitrión, registrar cada 10 s, con la zona encendida, `zone: N zone enemies within R m, M
   copied` (hoy no hay ninguna línea que lo diga cuando se llega al tope).
2. En el invitado, comparar `held` con las copias cargadas.
3. Repetir el corte de conexión con `cdmp_peer` (Ctrl+C a mitad de un envío) y confirmar que no sale
   ninguna línea "sending enemy … (zone" sin una petición de la sesión nueva.

---

## 10. Scripts

En `build/research/p_faltan7oct/` (Python + `tools/scripts/analyze_capture.py`):
- `capx.py`: lector con caché de las dos capturas.
- `zone.py`: `zone_check` portado, con las tablas leídas de `enemy_tables_2976.h`.
- `hostzone.py <h|g> HH:MM:SS,...`: instantáneas de los enemigos de zona a ≤ 150 m.
- `keyops.py <h|g>`: paquetes por clave de copia.
- `track.py <h|g> <clave> [n]`: carga, acciones y movimientos de una copia.
- `firstmove.py`: marcas de las acciones iniciales y primer movimiento de cada copia.

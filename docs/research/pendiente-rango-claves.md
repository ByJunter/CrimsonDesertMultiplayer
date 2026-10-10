# Rangos de claves seguros para las copias inyectadas

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA de esta
build. Investigación de solo lectura: análisis estático con capstone, 50 capturas (`D:\cdmp-grabaciones`
y `bin64`, del 2026-10-03 y 04) y el volcado vivo `build/research/sam_dump1.txt`.
Scripts en `build/research/p_keys/`.

Etiquetas: **[verificado]** = leído en el código; **[captura]** = medido en capturas;
**[inferido]** = deducción.

---

## Resumen

- **La clave de actor no es un contador que crece sin fin.** Es un *handle* con este formato
  **[verificado]**:

  | Bits | Campo | Valores reales |
  |---|---|---|
  | 31 | válido | siempre 1 |
  | 30 | clave **local del cliente** | 0 en todo lo que manda el servidor |
  | 28-29 | clase | 1 = usuario (`0x9…`), 2 = jugador (`0xA…`), 3 = mundo (`0xB…`) |
  | 20-27 | **generación** (8 bits) | 1 en la primera vuelta |
  | 0-19 | **índice** | `0 … 0x3C8C` |

- **El servidor reparte los índices de una cola FIFO de 15.501 (`0x3C8D`) huecos**, compartida por
  usuarios, jugadores y actores del mundo (`ServerActorManager+0xC8`). El índice **nunca pasa de
  `0x3C8C`**. Cuando la cola da la vuelta, el índice se **reutiliza con la generación + 1**:
  `0xB02xxxxx`, `0xB03xxxxx`… **[verificado]** + **[inferido]** (la +1 la hace una vfunc virtualizada).
- **Consecuencia 1: nuestras claves actuales no chocan nunca** con las del servidor de esta build,
  dure lo que dure la sesión. Sus índices (`0xF001-0xF003`, `0xF0001-0xFEFFF`) quedan fuera del
  pool. Los avisos anteriores ("`0xB010F001` se alcanza en 2-4 h", `caballo-coop.md` §6 y
  `enemigos-del-anfitrion.md` R8) **eran falsos**: suponían un contador lineal.
- **Consecuencia 2: hay un fallo real, y no está en las claves inventadas.** Tras unas
  **15.500 altas** (de **32 a 60 min** de juego activo, según las capturas) las claves reales pasan a
  `0xB02…`/`0xA02…`. **Las 18 comprobaciones `key >> 20 == 0xB01/0xA01/0x901` del mod dejan de
  reconocerlas**: se acaban el relevo de enemigos, los golpes, el aprendizaje de NPC y la limpieza
  de objetivos de la marioneta.
- El servidor **busca actores solo por el índice** (`key & 0xFFFFF`) **[verificado]**. Le llegan
  cientos de `Req` con nuestras claves (por ejemplo, 5.882 `SetQuestDialogReq` de copias de
  enemigos) **[captura]**. Hoy no encuentra nada y no responde, pero **un índice dentro del pool
  haría que esas `Req` actuaran sobre un actor real**, y ese estado podría acabar en la partida
  guardada.
  - Regla de oro: **índice ≥ `0x3C8D`** (con margen, ≥ `0x8000`).
  - **No** usar otra generación con un índice bajo.
- **Recomendación:**
  - mantener los valores actuales;
  - arreglar los predicados de clase para que acepten cualquier generación;
  - acotar el `net_id` de las copias a `1..0xEFFF`;
  - unificar el contador de ids de instancia;
  - añadir un "canario" que desactive las copias si alguna vez aparece una clave real fuera del
    pool.

---

## 1. Cómo reparte claves el servidor

### 1.1 Alta (`0x2A82BF0`, crear `ServerNormalInGameActor`) [verificado]

```
0x2A82D57  if (SAM.cola.count == 0) → error [0x6CDB9E4] eErrNoInvalidActorKey   (no hay clave libre)
0x2A82DC6  v = cola.data[cola.head++ % cola.cap]; cola.count--                  (pop FIFO)
0x2A82E02  actor.vf5(v & 0xFFFFF)              → actor+0x50 = índice            (vf5 = 0x50E6D0)
0x2A82E11  actor+0x60 = (actor+0x60 & 0xF00FFFFF) | ((v>>20)&0xFF)<<20          (generación de la cola)
0x2A82E49  actor.vf8(…, SAM+0x50[tipo])        (clase en bits 28-29; ver 0x2A7DAF9: byte tipo << 28)
0x2A82E8E  actor.vf4(((actor+0x60 & 0xFFF00000) + 0x100000) >> 20, 1)           → generación + 1
0x2A82EEE  inserta en el mapa SAM+0x98 con clave = actor+0x60 & 0xFFFFF        (¡solo el índice!)
```

- El mismo patrón "+0x100000" aparece en las otras dos rutas de alta: `0x2A83608`
  (usuario/jugador) y `0x2A7DB0E` (pool por usuario).
- `vf4` está virtualizada (`0x28F24F0` → `.xtls 0x103732F0`). Que sume 1 a la generación se deduce
  porque la cola se llena con generación 0 (§1.3) y todas las claves vistas tienen generación 1
  **[inferido, confianza alta]**.

### 1.2 Baja (`0x2A846B0`) [verificado]

```
v = 0x80000000 | (actor+0x60 & 0x0FF00000) | (actor+0x50 & 0xFFFFF)   ; misma generación, sin clase
actor+0x60 = 0
cola.data[cola.tail++ % cola.cap] = v; cola.count++                    ; push al FINAL (FIFO)
```

### 1.3 Llenado inicial (`0x2A7DEA0`, `ServerActorManager::Initialize`) [verificado]

- Rellena la tabla `SAM+0x50`: 8 entradas de 0x18 B con `{u8 clase, u8 tipo de registro, …, u32 cupo}`:

  | Clase | Tipo | Cupo |
  |---|---|---|
  | 1 | 0 (usuario) | N |
  | 2 | 1 (jugador) | 100·N |
  | 1 | 2 | N |
  | 3 | 3 | `param+0x80` |
  | 3 | 4 | `param+0x88` |
  | 3 | 5 | `param+0x98` |
  | 3 | 6 | `param+0x88` |
  | 3 | 7 | `param+0x94` |

  `param` es el `ServerModuleFrameworkParameter` de `SAM+0x40`.
- `param+0x44` = suma de los cupos.
- Bucle `0x2A7E100`: `for i in 0..total-1: push(0x80000000 | (i & 0xFFFFF))`.
- Valor vivo: `param+0x44` = **`0x3C8D` = 15.501**. El volcado del SAM (`sam_dump1.txt`) da:

  | Campo | Valor |
  |---|---|
  | `+0xC8` head | `0x2E5` |
  | `+0xCC` tail | `0x3EBB` |
  | `+0xD0` count | `0x3BD6` |
  | `+0xD4` cap | `0x3C8D` |

  Cuadra: `tail − head = count`, y 741 altas con 558 bajas **[captura/vivo]**.
- Esto resuelve la pregunta abierta 1 de `troc-bdo-y-runtime.md` (qué es `0x3C8D`).
- Tabla tipo→clase del cliente en `0x5545178`: `01 02 03 03 03 03 03 03 03 03 FF 01 01 FE 02 03`
  **[verificado]**.

### 1.4 Búsqueda en el servidor (`0x2A81900` Find, `0x2A81AA0` FindPlayer) [verificado]

- `índice = key & 0xFFFFF` → mapa hash `SAM+0x98` (cubetas de 0x100 B y 31 entradas; rehash al
  llenarse, `0x2A84F00` / `0x391680`).
- **No compara ni la generación ni la clase completa.**
- `FindPlayer` solo exige clase 2, o el bit 30.

### 1.5 Claves locales del cliente (bit 30) [verificado]

- `0x8AD590` (scope `ContentsCharacterLoginingScopeAttacher`) crea actores **solo del cliente** con
  su propio contador en `mgr+0x48`:
  - `índice+1` (20 bits, da la vuelta);
  - clase = tabla `0x5545178[tipo]` en los bits 28-29;
  - `| 0xC0000000`.
- La generación se incrementa al crear el gestor: `0x8A83F4`.
- Prefijos `0xC…`, `0xD…`, `0xE…` y `0xF…`. **Nunca coinciden con los nuestros**, que tienen el
  bit 30 a 0. No conviene usar ese espacio: lo gestiona el propio cliente.

---

## 2. Evidencia en capturas

### 2.1 Claves reales por clase (`k1.py`, `k4.py`, `keygen_scan.py`) [captura]

- **Las 50 capturas:**
  - 71.788 registros de `ActorLoadDataAck` de mundo, más 518 de usuario y 200 de jugador;
  - prefijos solo `0x901`, `0xA01` y `0xB01`: **generación 1 siempre**;
  - **ningún índice ≥ `0x3C8D`** fuera de nuestras claves.
- **Contador único para las tres clases:** en `183530` hubo un relogin a 688 s, y el usuario y el
  jugador nuevos salieron con `0x9010173F` y `0xA0101740` (índices consecutivos del mismo pool que
  el mundo).
  - **No se reinicia al recargar partida**, solo al reiniciar el proceso: todas las capturas
    empiezan en `0x90100000` / `0xA0100001` / `0xB0100002`.
  - Lo de que "el caballo siempre es `0xB0100003`" solo vale en el **primer** login de cada
    proceso.
- **Reutilización tras `RemoveActorAck`:** **0 casos** en todas las capturas.
  - Ninguna sesión llegó a vaciar el pool. El máximo fue `0x32E2` = 13.026 altas, en `183530`.
- **Huecos:** de los 13.025 índices de `183530`, solo 194 no llegaron al cliente (actores solo del
  servidor o fuera de rango).

### 2.2 Ritmo de altas → cuándo llega la generación 2 [captura]

| Captura | Duración | Índice máx. | Altas/min | Vuelta (15.501) en |
|---|---|---|---|---|
| `20261003-183530` | 1.606 s | `0x32E2` | 487 | **32 min** |
| `20261004-080253` | 445 s | `0x0DE5` | 480 | 32 min |
| `20261004-084001` | 527 s | `0x0D45` | 387 | 40 min |
| `20261003-200328` | 1.846 s | `0x1E48` | 252 | 62 min |
| `20261003-141056` | 907 s | `0x03E2` | 66 | 236 min |

- Con juego activo (viajes y combate) son de **250 a 490 altas/min**: la generación 2 llega entre
  **los 30 y los 60 min**.
- El contador de 8 bits de la generación da la vuelta tras 255 ciclos, unos 4 millones de altas
  (días de juego). Es irrelevante.

### 2.3 El cliente manda al servidor `Req` con nuestras claves (`k9.py`, `k10.py`, `k11.py`) [captura]

- **Con nuestra clave en `+0`:**

  | `Req` | Veces |
  |---|---|
  | `SetQuestDialogReq` (copias de enemigos, ~1/s cada una) | 5.882 |
  | `RegionInOutReq` | 576 |
  | `UpdateGimmickReq` | 144 |
  | `EchoActionSessionIDReq` | 126 |
  | `MoveActorReq` | 111 |
  | `CreateProjectileFromFrameEventReq` | 83 |
  | `ChangeDrowningStateReq` | 8 |
  | `AttackByCollisionReq` | 7 |
  | `WetnessReq` | 2 |

- **Con nuestra clave dentro:** `StartActionReq`, `UpdateGimmickReq`, `GamePlayTriggerInOutReq`,
  `AggroRelationListReq`, `CurrentTargetChangeReq`, `DoFrameEventReq`…
- El servidor **no responde** con nuestra clave: 0 `SetQuestDialogAck` con clave de copia frente a
  683 con clave real. La búsqueda por índice falla.
  - Hoy es inocuo **porque el índice está fuera del pool**.
  - Con un índice ≤ `0x3C8C`, `0x2A81900` encontraría un actor **real** y le aplicaría la `Req`
    (diálogo de misión, región, humedad, colisión…).

### 2.4 Otros espacios de ids [captura] + [verificado]

| Espacio | Real | Nuestro | ¿Choque? |
|---|---|---|---|
| Instancia de buff/restricción (`ApplyBuffAck +0x14`, AAR `+0x24`) | **Por portador**, empieza en `0x80000001`. Máximo en todas las capturas: `0x8000035C` (la copia `0xA010F001`, con los ids reales del emisor); jugador `0x80000247` en 6 min de combate; enemigos ≤ `0x9C` | `0x8000F000 + (n & 0xFFF)` | No. Solo se compara por igualdad dentro de las ranuras del mismo actor (`pieza4-tambaleo.md`). El jugador necesitaría unas 10 h de combate continuo para llegar a `0xF000`, y nunca inyectamos sobre el jugador |
| Proyectil (`CreateProjectileAck` / `RemoveProjectileAck`) | `{clave del dueño, u32 n}` con `n` pequeño (`RemoveProjectileAck +0` dueño, `+4` dueño, `+8` = 1) | El mod aún no inyecta | Por dueño: los de la copia viven en el espacio de su clave |
| Pin del mapa | Servidor: índice del primer bit libre (0-499) o `(byte<<32)\|índice` (`mapa-minimapa.md` §1.4) | `0x7FDD00000000F001` | No. El cliente solo compara por igualdad |
| Nº de personaje (caché de equipo) | `0x7FFFFFFFFFFFFFFF` | `0x7FFFFFFFFFFFF001` | No (ya probado en vivo) |
| Nº de grupo (`Party`) | `0x201_0000xxxx`, serie por mundo | ver `copias-grupo-invisibles.md` | Fuera de este tema |

### 2.5 Almacenamiento en el cliente [verificado]

- `ClientActorContainer`: mapa hash con cubeta = `key % nb` (0x100 B, 31 entradas, rehash).
  - Compara la **clave completa** de 32 bits (`0x464290`, `actor_lookup.cpp`).
  - El alta (`0x465410`) está virtualizada, pero la fábrica `0x462FD0` la llama con la clave
    entera.
- **No hay arrays indexados por los bits bajos.**
- Los usos de `key & 0xFFFFF` en el cliente son:
  - **reparto del update por frames:** `(índice % 30) == ranura` en `0x94FA37`, más `0x61C155`,
    `0x94D67D` y `0x8B7D0E`. Sirve cualquier índice;
  - **almacenaje del índice:** `0x46C317`, `0xA08680`;
  - **centinela `0xFFFFE`** en dos contenedores de handles: `0x488035` y `0x12C7EF8`.
    **Hay que evitar los índices `0xFFFFE` y `0xFFFFF`.**
- **Una clave alta no rompe nada:** las copias `0xB01F00xx` y `0xB010F00x` se crean y se ven en vivo.

### 2.6 ¿Pueden ir a la partida guardada? [inferido, confianza alta]

- **Lo que se guarda lo escribe el servidor** desde sus componentes (por ejemplo
  `ServerContentsMiscActorComponent` slot10 `0x28B7360`, `mapa-minimapa.md` §1.5). El servidor
  **no tiene `ServerActor`** para ninguna copia, y sus búsquedas por índice fallan (§2.3).
- **Las claves son de tiempo de ejecución:** empiezan de 0 en cada proceso (§2.1). La partida
  guardada no puede referirse a actores por clave.
- Los `save.save` están cifrados (magia `SAVE`, entropía 8,0): no aparece ni `0xA0100001`. Leerlos
  no sirve para comprobarlo.
- **Riesgo residual:** solo si un índice nuestro cayera dentro del pool (§2.3). Por eso la regla del
  índice es la que de verdad protege la partida.

---

## 3. Dónde se rompe el mod con la generación 2 [verificado en el código]

`key >> 20 == 0xB01 / 0xA01 / 0x901` solo acepta la generación 1:

| Fichero:línea | Efecto cuando las claves pasen a `0xB02…` |
|---|---|
| `src/mod/net/enemy_relay.cpp:258-259` `is_key_shape` | No reconoce listas de claves (`Party`, cabeceras) |
| `enemy_relay.cpp:716` | **Deja de relevar enemigos** (filtra `key>>20 != 0xB01`) |
| `enemy_relay.cpp:960-961` | Validación de claves conocidas |
| `src/mod/net/guest_hits.cpp:95-98` `is_key_shape`, `is_real_enemy` | Golpes del invitado ignorados |
| `src/mod/net/player_hits.cpp:46-47` | Golpes a la marioneta ignorados |
| `src/mod/net/local_capture.cpp:34-36, 500, 565, 595, 601, 603, 622, 663` | HP de enemigos, aprendizaje de NPC, montura y cuerpo tras un relogin con generación ≥ 2 |
| `src/mod/net/remote_puppet.cpp:80-82` `is_actor_key`, `:606` | Las claves reales `0xB02…` en los paquetes del emisor **no se limpian ni se traducen**: objetivos (`+0x16D`) que apuntan a actores al azar del receptor |
| `src/mod/net/piece4_test.cpp:61, 167`, `aggro_test.cpp:274` | Pruebas |
| `aggro_test.cpp:101/274` y `mod.cpp:482` | Además, `0xA0100001` fijo: falla tras un relogin |
| `build/research/pieza4/pk.py` `is_key` | Los scripts de investigación tampoco verían la generación 2 |

**Otro desborde latente:**
- `enemy_relay.cpp:494` `copy = 0xB01F0000 + g_next_net_id++` crece sin límite.
- Tras 65.535 copias da `0xB0200000+`. Eso es el espacio **real** de la generación 2 (índices
  0…) y además sale de `is_enemy_copy_key`.
- Pasando por `0xB01FFFFE/F` toca el centinela.
- Muy improbable (horas), pero gratis de evitar.

---

## 4. Enfoques

| # | Enfoque | Cambios | Riesgo | Esfuerzo |
|---|---|---|---|---|
| **A** | **Mantener las claves y arreglar los predicados** (recomendado) | `keys.h` con `key_class(k)=(k>>28)&3`, `key_index(k)=k&0xFFFFF`, `is_server_key(k)` = bit31 ∧ ¬bit30 ∧ índice < `0x8000`, `is_mod_key(k)`; sustituir las 18 comprobaciones; `net_id` en `1..0xEFFF` con reutilización diferida | Muy bajo. Sin cambio de formato: no hace falta subir `kVersion` | ~1 tarde |
| B | Mover todas nuestras claves a un bloque único de índices, uno por clave (`0xF000-0xF0FF` marionetas, una por par; `0xF0001-0xFEFFF` enemigos) | Además de A, cambiar constantes (`remote_puppet`, `local_capture`, `enemy_relay`, `horse_diag`, `puppet_minimap`, `aggro_test`, `pk.py`) | Bajo, pero cambia lo que ve el otro PC (traducción G/H): **subir `kVersion`**. Hoy el usuario, el jugador y el cuerpo comparten el índice `0xF001` sin problemas en vivo | Medio |
| C | Usar otra generación (`0xB0F0xxxx`) con índice bajo | — | **Peligroso**: el servidor busca solo por índice y las `Req` filtradas acabarían en actores reales (§2.3) | Descartado |
| D | Usar claves locales del cliente (bit 30) | — | Choca con el contador propio del cliente (`0x8AD590`); `FindPlayer` trata el bit 30 aparte | Descartado |

**También descartado:** `0xB01FFFF0+` (propuesto en `enemigos-del-anfitrion.md` R8). Incluye el
centinela `0xFFFFE/F` y se solapa con `is_enemy_copy_key`.

---

## 5. Plan recomendado (A)

1. **`src/mod/net/keys.h`**: una sola fuente de verdad.

   | Uso | Clave | Índice |
   |---|---|---|
   | Usuario de la marioneta | `0x9010F001` | `0xF001` |
   | Jugador | `0xA010F001` | `0xF001` |
   | Cuerpo | `0xB010F001` | `0xF001` |
   | Caballo | `0xB010F002` | `0xF002` |
   | Grupo del caballo | `0xB010F003` | `0xF003` |
   | Reserva para más pares | `0xF000 + 0x10·par + {1,2,3}` (`par` = 0…15) | `0xF001…0xF0FF` |
   | Copias de enemigos | `0xB01F0000 + net_id`, `net_id` = `1…0xEFFF` | `0xF0001…0xFEFFF` |
   | **Prohibido** | — | `0xFFFFE` y `0xFFFFF` |
   | **Prohibido** | — | cualquier índice < `0x8000` |
   | Instancias AAR/buff | `0x8000F000 + (n & 0xFFF)`, **un solo contador** compartido por `guest_hits` y `piece4_test` | — |
   | Pin | `0x7FDD00000000F001` (sin cambios) | — |
   | Nº de personaje | `0x7FFFFFFFFFFFF001` (sin cambios) | — |

   - Generación de nuestras claves: **1**, como las reales. El servidor no la mira y el cliente
     compara la clave entera.
   - Predicados (`is_server_key`, `key_class`…): **cualquier generación 1…255**.
2. **Sustituir las comprobaciones de §3** por `key_class` / `is_server_key` / `is_mod_key`.
   - `0xB01` → clase 3; `0xA01` → clase 2; `0x901` → clase 1.
3. **`net_id`**:
   - dar la vuelta dentro de `1..0xEFFF`;
   - no reutilizar uno antes de 10 s tras su `RemoveActorAck` (lo que ya proponía
     `enemigos-del-anfitrion.md` §2);
   - si se agotan, no crear la copia y registrarlo.
4. **Canario** (en `capture_observe_s2c`, al ver un `ActorLoadDataAck` real): si una clave no
   nuestra tiene índice ≥ `0x8000`, el bit 30 o la generación 0, registrar un error y desactivar las
   copias (`enemy_relay` y `remote_puppet`).
   - Opcional: al arrancar, leer **con VirtualQuery/SEH** `u32 [[exe+0x6DA67D8]+0xD4]` (cap del pool)
     y exigir que sea < `0x8000`.
5. **Tests unitarios** (`tests/`):
   - predicados con generaciones 1, 2 y 255;
   - las tres clases;
   - el límite `0x8000`;
   - el centinela;
   - la vuelta de `net_id`.
6. **Investigación:** cambiar `is_key` en `pk.py` a "clase ∈ {2,3} y generación cualquiera".
   Corregir `caballo-coop.md` §6.1 y `enemigos-del-anfitrion.md` R8 / §1.2.

**Riesgos de cambiar rangos** (solo para el enfoque B):
- `puppet_minimap` registra el icono por `ActorKey`;
- `horse_diag` vigila claves fijas;
- el otro PC traduce H/G con las mismas constantes, así que hay que subir `kVersion`;
- no hay datos guardados, misiones ni minimapa persistente que dependan de nuestras claves (§2.6).

---

## 6. Pruebas en vivo (un PC, `cdmp_peer`)

| # | Prueba | Cómo | Predicción |
|---|---|---|---|
| **P1** | Tamaño del pool (solo lectura) | Partida cargada, sin co-op. `cdmp_probe read <[exe+0x6DA67D8]+0xC8> --type u32 --count 8` | `+0xD4` = `0x3C8D`; `+0xC8` (head) ≈ índice máximo visto en la captura + 1; `+0xD0` = `0x3C8D` − actores vivos (~150-300) |
| **P2** | Vuelta de generación | Sesión de 45-60 min con sniffer, muchos viajes rápidos y combate; leer `+0xC8` cada 10 min con P1. Después `keygen_scan.py <captura>` | Cuando `head` pase de 15.501, `first_gen2` ≠ None con claves `0xB02xxxxx` de índice ≤ `0x3C8C`; `out_of_pool` vacío |
| **P3** | El fallo actual | P2 con `[coop] enabled=1` (eco) y relevo de enemigos activo, sin el arreglo | Tras la vuelta, `cdmp.log` deja de mostrar enemigos relevados y golpes (filtro `0xB01`); los nuevos enemigos no tienen copia |
| **P4** | El arreglo | P3 con el plan A | Copias y golpes siguen tras la vuelta; el canario no salta |
| **P5** | `Req` con nuestras claves | Cualquier captura con copias: `k10.py <captura>` | 0 `Ack` con clave de copia; las `Req` reales sí tienen `Ack` |
| **P6** | Relogin | Volver al título y cargar otra partida en el mismo proceso, con sniffer | Usuario y jugador con índices nuevos (`0x9010xxxx` / `0xA010xxxx` con `xxxx` > 2); el cuerpo y el caballo dejan de ser `0xB0100002-4`. Los `0xA0100001` fijos de `aggro_test`/`mod.cpp` fallan |

**Copia de seguridad:**
- P1, P2, P5 y P6 no escriben en memoria.
- P3 y P4 usan el co-op, que inyecta: haz antes una copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save`
  en `backups/`.

---

## 7. Preguntas abiertas

1. **¿De dónde salen los cupos?** `param+0x80/0x88/0x94/0x98` (15.501 en total) se ponen a 0 en el
   constructor (`0x27BFBE5`) y se rellenan después: ¿datos del juego o un ajuste? Si un parche los
   sube por encima de `0x8000`, el canario lo detecta. Margen actual: ×4 para `0xF001` y ×63 para
   `0xF0000`.
2. **`vf4` virtualizada:** la +1 de la generación es una deducción. La confirma P2.
3. **Centinela `0xFFFFE`** (`0x487FF0`, `0x12C7900`): ¿qué contenedor es? Basta con evitarlo.
4. **Pool por usuario** (`0x2A7D9F0`): reutiliza un objeto con su índice y sube la generación. No se
   ha visto en ninguna captura (ninguna clave de generación 2). ¿Qué tipos lo usan?
5. **Claves locales del cliente** (`0x8AD590`, bit 30): ¿qué actores las usan (secuencias,
   cinemáticas)? ¿Aparecen en paquetes que el mod parsea? Los predicados nuevos las excluyen.

---

## Apéndice A: direcciones (build 2976)

| RVA | Qué es |
|---|---|
| `0x2A82BF0` | Servidor: crear `ServerNormalInGameActor`; pop de la cola (`0x2A82DC6`); generación +1 (`0x2A82E8E`); inserción por índice (`0x2A82EEE`) |
| `0x2A83310` / `0x2A7D9F0` | Otras altas (usuario y jugador / pool por usuario) con el mismo +1 (`0x2A83608`, `0x2A7DB0E`) |
| `0x2A846B0` | Servidor: liberar clave → push al final de la cola |
| `0x2A7DEA0` | `ServerActorManager::Initialize`: tabla `SAM+0x50` y llenado de la cola (`0x2A7E100`) |
| `0x2A7DD60` / `0x27BEB50` | Destructor y constructor del SAM (cola en `+0xC8`: `0x141244E70`) |
| `0x2A81900` / `0x2A81AA0` | SAM Find / FindPlayer por índice (`key & 0xFFFFF`) |
| `0x2A84F00` | Inserción en el mapa hash (31 por cubeta, rehash `0x391680`) |
| `[exe+0x6DA67D8]` | `ServerActorManager*`. `+0x40` param (`+0x44` = 15.501); cola `+0xC8` head, `+0xCC` tail, `+0xD0` count, `+0xD4` cap, `+0xE0` datos |
| `0x6CDB9E4` | Código `eErrNoInvalidActorKey` (cola vacía) |
| `0x462FD0` | Cliente: fábrica de `ClientUserActor`. `vf5(key&0xFFFFF)`, bit 30 → `vf8`, `vf4(key>>20)`, inserción con la clave completa (`0x465410`) |
| `0x50E6D0` | `vf5` común: `actor+0x50 = índice` |
| `0x206AC90` / `0x28F24F0` / `0x2A7D230` | `vf4` de cliente, servidor y usuario de servidor (virtualizadas) |
| `0x8AD590` | Cliente: claves locales (bit 30) desde el contador `mgr+0x48`; `0x8A83F4` sube su generación |
| `0x5545178` | Tabla tipo de registro → clase (`01 02 03 03 03 03 03 03 03 03 FF 01 01 FE 02 03`) |
| `0x94FA37`, `0x61C155`, `0x94D67D`, `0x8B7D0E` | Reparto del update por índice (módulo) |
| `0x488035`, `0x12C7EF8` | Centinela de índice `0xFFFFE` |

## Apéndice B: scripts (`build/research/p_keys/`)

| Script | Qué hace |
|---|---|
| `k1.py` (`k1.out`) | Claves por clase en `ActorLoadDataAck` y `RemoveActorAck` de todas las capturas; reutilización tras la baja |
| `k4.py` | Línea de tiempo del primer índice visto (monotonía, huecos, relogin) |
| `k2.py` / `k3.py` | Búsqueda de constantes de clave (`0xB0000000`, `0xFFFFF`, `shl 0x1C`…) en el exe |
| `k7.py` / `k8.py` | Ids de instancia de buff/AAR: rango y alcance por portador |
| `k9.py` / `k10.py` / `k11.py` | `Req` C>S con nuestras claves y si el servidor responde |
| `k12.py` | Campo de id de proyectil (Create/Remove) |
| `k13.py` / `k14.py` | Claves de índice bajo (usuario, cuerpo, caballo) por captura |
| **`keygen_scan.py`** | Escaneo con generaciones: generaciones vistas, índice máximo, primera clave de generación 2 e índices fuera del pool (para P2) |
| `f_*.asm` | Desensamblados: `f_2a82bf0.asm` (alta), `f_2a7dea0_init.asm` (llenado), `f_sam_ctor.asm`, `f_462fd0.asm`, `f_8ad590.asm`, `sam_region.asm` |

# Copias de enemigos agachados que salen de pie en pose A (prueba del 7 oct por la tarde)

Build del juego **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Los hashes de acción y las
tablas de `actionchart/*.paac` solo valen para esta build. Investigación de solo lectura (7 oct 2026):
no se ejecutó el juego ni se tocó `src/`, `kit/`, `scripts/` ni ninguna configuración.

Marcas: **[verificado]** = comprobado en log, captura, datos del juego o código; **(hipótesis)** = deducido
y pendiente de confirmar en el juego.

Datos usados:
- anfitrión (H): `D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\cdmp.log` y
  `cdmp_packets_20261007-184300.bin`;
- invitado (G, Shadow): el log del invitado (`cdmp.log`, 7 oct, tarde) y
  `cdmp_packets_20261007-174707.bin`;
- datos del juego ya extraídos antes: `build\research\pieza4\react\paac\` (558 `actionchart/*.paac`,
  sacados del PAZ por `r_paac.py`), `build\research\arrow\index.tsv` y `build\research\ids\out\gd\`.

Scripts: `build\research\p_agachados\` (lista en el §9).

---

## 0. Resumen

| # | Pregunta | Respuesta | Confianza |
|---|---|---|---|
| 1 | ¿Qué hacen en H los enemigos agachados? | Son bandidos "Black Bear" con origen de **schedule** (`0xBE62E27F`). Su acción de reposo es **`0x40094A9C`**, un bucle de ~27 s. El ciclo es entrar `0xBFCED713` → bucle `0x40094A9C` → salir `0x5BD8B2C2` → `wait_default` → volver a entrar. **`0x40094A9C`, `0xBFCED713` y `0x0C5E50CE` solo existen en `actionchart/schedule_action_nhm.paac`** | alta [verificado] |
| 1b | ¿Están acoplados a algo? | **No.** No tienen gimmicks acoplados (n = 0), ni padre, ni grupo, ni hueco de campamento. Al cargar no hay ningún "Game Mechanic" a menos de 2 m que no sea un activador de montar. La postura sale **solo del schedule** | alta [verificado] |
| 2 | ¿Qué recibe G? | La carga real, sin cambios salvo las claves y la **identidad del schedule** (hash en `T0+42` y en el bloque de secuencia, elemento/serie en `T0+46..51`). Primero recibe `wait_default` (la primera acción, calmada) y, **6–24 s después, `0x40094A9C>0x40094A9C` sin tocar**, repetido cada ~27 s hasta la pelea | alta [verificado] |
| 2b | ¿Por qué pose A? | En G, los nativos del mismo schedule (con schedule) reproducen `0x40094A9C`. La copia, igual a ellos salvo el schedule, no lo anima. Las copias con acciones de los charts comunes (`common_defaultupper/lower`) se animan bien. Lo más probable: una acción del sub-chart de schedule (`9_schedule` → `schedule_action_nhm`, marcado `isNPCScheduleActionChart`) no tiene animación en un actor sin schedule | media-alta (hipótesis con datos) |
| 3 | ¿Por qué ahora solo estos? | La versión probada **no es `aa4ddb5`**: es **`6b86521`** (`build 6B865217`; el `.asi` contiene una cadena que `aa4ddb5` borró). Su `calm_pose` solo cambia la **primera** acción (`+4 == 0`). Las repeticiones de la pose pasan. Esta zona tiene pocos enemigos con pose de schedule (7 de 57), y la de la prueba anterior tenía muchos | alta [verificado] |
| 4 | ¿Lo arregla `aa4ddb5`? | **Solo en parte.** Repitiendo la sesión con su lógica, siguen 3 copias y **186 s** de acción de schedule visible (antes, 5 copias y 1354 s). Falla en dos casos: (a) `BFCED713>40094A9C` (vuelve a agacharse, `prev` ≠ pose) y (b) copias enviadas cuando la primera acción ya salió de las 4 guardadas (la pose no se aprende) | alta [verificado por simulación] |
| 5 | Arreglo | En el anfitrión, **cambiar por `wait_default` toda acción que solo exista en `schedule_action_*.paac`** (tabla fija de 13 738 hashes de la build 2976, generada por script), en `+0x04` y `+0x08`. Se aplica con o sin pelea y sin tener que aprender la pose. Simulación: **0 s**. Ninguna de esas acciones aparece en batalla (0 de 2486 en H y 0 de 764 en G) | alta (el arreglo) |
| 6 | Que se agache como en H | Solo con un experimento: **mantener el schedule** en la carga de la copia (interruptor de desarrollo, §7.2). Decide entre B1 y B2 | por probar |

---

## 1. Relojes y versión probada

### 1.1 Reloj [verificado]

- **H ≈ G + 1 h 00 min 00 s** (zona horaria), con 0,2–0,5 s de latencia del relevo. **No** son 55 min
  54 s: esa es la diferencia entre las horas de inicio de las dos capturas (18:43:00 frente a 17:47:07).
- Anclas:
  - muerte de `0xB01006FD`: `NotifyHandleDeadAck` en H a las 18:54:19.034, el de su copia `0xB01F001C`
    en G a las 17:54:18.777;
  - envío de `0x31`–`0x36`: H a las 18:49:49.898, G las encola a las 17:49:49.376–.554 (+1 h).
- Las horas de captura se pasaron a reloj de pared con dos anclas:
  - en H, la carga inyectada del jugador de G (`delivered opcode 2005 (12673 bytes)`, 18:44:49.229);
  - en G, la carga de la copia `0xB01F0019` (17:48:22.416).

### 1.2 La versión que se probó es `6b86521`, no `aa4ddb5` [verificado]

- Los dos logs dicen `=== Crimson Desert Multiplayer 0.1.0 (build 6B865217, protocol 9) ===`. Es
  `git rev-parse 6b86521` = `6b865217d830…`.
- El `.asi` es el mismo en `bin64\cdmp.asi` y en el que recibió el invitado: 4 098 560 B, del
  7 oct a las 06:02:10. `aa4ddb5` se registró a las 06:05:36.
- El `.asi` **contiene** la cadena `"another twin was just asked for"` (offset de fichero 2 903 592).
  `aa4ddb5` la borró de `loot_twin.cpp`. Así que el binario no incluye `aa4ddb5`, ni siquiera sin
  registrar.
- La captura lo confirma: G recibió `0x40094A9C>0x40094A9C` sin cambiar (§3.2). Con la lógica de
  `aa4ddb5` esas repeticiones habrían salido como `wait_default`.

`calm_pose_locked` en `6b86521` (de `cac4136`), según el diff de `aa4ddb5`:

```cpp
if (req.size() < 12 || !g_schedule_spawned.count(key) || u32_at(req.data() + 4) != 0 ||
    u32_at(req.data() + 8) == 0) return;
if (engaged...) return;
// +4 y +8 = wait_default
```

Es decir, solo cambia la acción "que no sigue a ninguna" (`+4 == 0`), que es la primera.

---

## 2. Formato que importa: dos capas de acción en `StartActionReq` [verificado]

`StartActionReq` (2223, 725 B) / `StartActionAck` (2991, 730 B; igual hasta `+0x8E`) llevan **dos
pares**:

| Campo | Qué es | Chart donde están los hashes |
|---|---|---|
| `+0x04` / `+0x08` | acción anterior / nueva de la **parte de arriba** | `common_defaultupper`, `schedule_action_nhm`… |
| `+0x49` / `+0x4D` | acción anterior / nueva de la **parte de abajo** | `common_defaultlower`… |

- Un paquete con `+0x04 = +0x08 = 0` cambia solo la capa de abajo. Hay muchos así (los
  `L0 0 > 0` de `layers_*.txt`).
- Los charts lo confirman: `nhm_upper.paac` incluye `common_defaultupper`, `common_action_npc`,
  `sequencer_action`, `8_endbehavior`/`endbehavior_action`, **`9_schedule`/`schedule_action_nhm`**,
  `10_actionpoint`/`actionpoint_branchset` y `actionpoint_nhm`, y `common_gimmick_action_npc`.
- `nhm_lower.paac` incluye `common_defaultlower`, `ride_selector_lower` y `common_lower_hitaction`.
  **Ningún chart de abajo incluye uno de schedule** (búsqueda de `schedule_action_` en los 558
  `.paac`).
- `calm_pose_locked()` actual solo mira la capa de arriba (`+4`/`+8`). Es la correcta.

---

## 3. Pregunta 1: qué hacen en H los enemigos agachados

### 3.1 Quiénes son [verificado]

Repasando los 78 envíos de la sesión (`acts.py`, `layers.py`), la acción de arriba en reposo, antes
de la pelea, es `wait_default` (`0x26107D3C`) para casi todos:
- 63 de campamento (origen F), con la capa de abajo en `0x1DF97512`;
- 57 de schedule (S), con la capa de abajo en `wait_default`.

Las excepciones (`classify.py`):

| Acción de arriba | Veces | NPC | Origen | Charts que la definen |
|---|---|---|---|---|
| **`0x40094A9C`** | 90 | **7** | todos S | **solo `schedule_action_nhm`** |
| **`0xBFCED713`** | 6 | 4 | S | **solo `schedule_action_nhm`** |
| **`0x0C5E50CE`** | 4 | 2 | S | **solo `schedule_action_nhm`** |
| `0x5C8B242E` (abajo `0xAE59880A`) | 160 | 9 | todos F | `common_defaultupper/lower`, `actionpoint_branchset`, `common_gimmick_action`… |
| `0xFD6A846A` | 33 | 3 | F | `common_defaultlower/upper`… |
| `0x5BD8B2C2` (salida) | 7 | 4 | S | `common_defaultupper` |
| `0x0182884E`/`0x1B37BABA`, `0xB486DDA9`/`0x41C91124`… | pocas | — | S y F | `common_defaultupper` |

Enemigos con la pose de schedule, todos "Black Bear" del schedule `0xBE62E27F`, salvo `A1F`:

| Real (H) | Variante | Posición | Copias | ¿Las vio G antes de pelear? |
|---|---|---|---|---|
| `0xB010070F` | 1002107 | (-10257.6, 678.3, -2338.0) | `0x12`, **`0x1A`** | sí, 17:48:22 → pelea 17:53:37 |
| `0xB01006FC` | 54020 | (-10274.9, 677.5, -2361.3) | `0x0A`, **`0x1D`** | sí, no llegó a pelear |
| `0xB0100742` | 54022 | (-10264.8, 675.4, -2320.6) | `0x18`, **`0x26`** | sí, 17:48:25 → pelea 17:53:15 |
| `0xB01009DA` | 1000934 | (-10264.8, 675.4, -2320.6) | **`0x3E`** | 17:55:56–17:56:36 (fin de captura) |
| `0xB0100A31` | 54040 | (-10284.7, 678.3, -2363.7) | **`0x4A`** | 17:56:01–17:56:36 |
| `0xB0100A1F` | 54013 | (-10260.3, 678.2, -2344.8), schedule `0xE3CC5DC8` | `0x3A` | no: `0x0C5E50CE` antes de la pelea (18:55:36), enviada después |

**(hipótesis fuerte)** Estos son los "agachados":
- es la única pose distinta de `wait_default` entre las copias que vio G;
- es un bucle largo, de 26,9 s (`0xB01006FC` la repite a las 18:46:57.4, 18:47:24.3, 18:47:51.3…);
- tiene una entrada y una salida propias: se agacha y se levanta;
- sale de un chart de schedule, igual que las poses que daban pose A en la prueba de la mañana
  (§3.4).

No se puede leer el nombre: el `.paac` guarda las acciones por hash, y los nombres de animación (5985
rutas `.paa` en `schedule_action_nhm`) no están ligados al hash de forma legible sin el formato del
chart (§8).

### 3.2 El ciclo en H [verificado, `tl.py h`]

```
18:46:34.738 B01006FC  L0 0        > 40094A9C   L1 0 > 26107D3C     (carga: empieza agachado)
18:46:57.430 B01006FC  L0 40094A9C > 40094A9C   (repetición cada ~26,9 s, 16 veces)
18:53:28.022 B01006FC  L0 40094A9C > 5BD8B2C2   (se levanta: alerta por la pelea cercana)
18:53:28.924 B01006FC  L0 5BD8B2C2 > 26107D3C
18:53:32.339 B01006FC  L0 26107D3C > 0182884E   (mira alrededor, chart común)
18:53:50.388 B01006FC  L0 26107D3C > BFCED713   (se agacha otra vez: entrada, chart de schedule)
18:53:51.614 B01006FC  L0 BFCED713 > 40094A9C   (bucle)
```

- `0xB010070F` repite "levantarse → wait → agacharse" 4 veces entre las 18:53:18 y las 18:53:31,
  antes de su `ChangeBattleStateReq 1` (18:53:37.128).
- `0xB0100742` entra en batalla a las 18:53:15.516, se levanta a las 18:53:16.035 y pelea.
- **Ninguna acción de solo-schedule aparece en batalla.** En H: 0 de 1212 acciones de arriba de
  hostiles en batalla, y 0 en la capa de abajo. En G: 0 de 366 (`fp.py`).

### 3.3 ¿Acoplados a un objeto? [verificado]

- Sus cargas tienen la lista de gimmicks acoplados vacía: `03 04 FF FF FF FF | 00 00 00 00`.
- Tampoco tienen padre, hijos, grupo ni hueco de campamento: su `T0+390` es 0, y su origen es S.
- Los "Game Mechanic" (tipo 7) a menos de 4 m (`near.py`):
  - `0xB0100780` y `0xB0100A5A` son `RideLimitTrigger` (activadores de montar);
  - `0xB0100889`/`88A`/`8B4`/`8F7`/`901` se cargan **después** de las 18:53:38 y llevan
    `PickUpTrigger`/`ReturnTrigger`/`LHand_Socket`: son las armas que sueltan los muertos (`gim.py`).
- Ningún mensaje de `Interaction*`, `Docking*` o `Attach*` nombra a estos enemigos.
- La diferencia entre la carga real y la que recibe G (`ldiff.py B010070F B01F001A`) es solo esta:
  - las claves (`0xB010070F` → `0xB01F001A`, 25 sitios);
  - el usuario dueño (`+0x14`);
  - **el hash del schedule `0xBE62E27F`, en `T0+42` y en `+7175`, y el elemento/serie de `T0+46..51`**.

  Es lo que hace `strip_spawn_identity()`.

**Conclusión:** la postura no depende de ningún objeto del mundo ni de un campo de grupo o de
campamento. Depende del **schedule**: su hash y su elemento son lo único de la carga que la copia no
tiene.

### 3.4 Coincide con la prueba de la mañana [verificado]

Las poses de schedule que daban pose A en `copias-enemigos-faltan-pose-minimapa.md` §2.1–2.2 también
son de solo-schedule (`where.py`):
- `0xF35F9FEE`, `0x6A703B9F`, `0x83332911`, `0x0FEF2E31`, `0xBA97CAC7` y `0xAA8595D3` solo están en
  `schedule_action_nhm`;
- `0xC45DA4D3` solo está en `schedule_action_ntm`.

En cambio, `0xC2B2D85B`, la acción de pelea que sí se animaba, no está en ningún chart de schedule.

---

## 4. Pregunta 2: qué recibe G y por qué no se anima

### 4.1 Lo que llega [verificado, `bundle.py`, `tl.py g`]

| Copia | Carga | Lote inicial (arriba) | Después |
|---|---|---|---|
| `0x1A` (`70F`) | 17:48:22.444 | `26107D3C>26107D3C` (la primera, calmada) | 17:48:46.132 **`40094A9C>40094A9C`**, luego cada ~27 s; ciclos `BFCED713`… a las 17:53:18–20 |
| `0x1D` (`6FC`) | 17:48:22.934 | `26107D3C>26107D3C` | 17:48:46.132 **`40094A9C>40094A9C`** y repeticiones |
| `0x26` (`742`) | 17:48:25.525 | `26107D3C>26107D3C` | 17:48:28.343 **`40094A9C>40094A9C`** (a los 2,8 s) |
| `0x3E` (`9DA`) | 17:55:56.509 | `40094A9C>5BD8B2C2`, `5BD8B2C2>26107D3C`, `26107D3C>BFCED713`, **`BFCED713>40094A9C`** | 17:56:06.508 `40094A9C>40094A9C` |
| `0x4A` (`A31`) | 17:56:01.508 | `26107D3C>26107D3C`, **`40094A9C>40094A9C`** | 17:56:22.711 ídem |

- En la capa de abajo reciben siempre `wait_default`, igual que el real.
- Todas las copias existen y terminan de entrar ("exists … state +5E 0x10 login +CF 1").
- `HorseDiag` las marca con acción en curso (`cur(+10)=true play(+18)=true`) desde el primer segundo.

Así que **la copia sí tiene una acción** y **no es un problema de orden**: la pose llega con la copia
ya cargada (`0x1A` existe desde las 17:48:24 y la pose llega a las 17:48:46). Enviar la pose después
de que exista no la arreglaría.

### 4.2 Por qué no se anima

**Hechos [verificado]:**
- En G, los **nativos** del mismo schedule (`0xBE62E27F`, con schedule) mandan esa misma acción
  (`gnat.py`):
  - `0xB010088C`, `0xB01008B8`, `0xB01008C7`, `0xB01008F7` y `0xB01008FA` → `0>40094A9C` a las
    17:49:38–43 y de nuevo al volver, a las 17:54:37;
  - `0xB01008F8`, `0xB01009D3`, `0xB0100B21`… → `0x0C5E50CE`.

  `0xB01008B8` está en el mismo sitio que la copia `0x26` (-10264.8, 675.4, -2320.6). El juego de G
  puede animar esa acción en un actor con schedule.
- La carga de la copia es la del real **menos el schedule**, y su usuario es otro (modo de control 0).
- Las copias de esta misma sesión con acciones de charts comunes se ven bien (en la prueba se vieron de pie
  en reposo y en grupo):
  - las de campamento: arriba `wait_default`, abajo `0x1DF97512`;
  - las de schedule con reposo `wait_default`.

  También están en modo de control 0. **El modo de control no impide animar.**
- `nhm_upper.paac` **incluye** el sub-chart `schedule_action_nhm` (ranura `9_schedule`): la acción
  forma parte del chart de cualquier humano.
- El exe tiene un campo `ActionChartPackage::isNPCScheduleActionChart` (cadena
  `" : isNPCScheduleActionChart: %s. "` en la RVA `0x5903A70`), además de
  `AIActionChart_NPCScheduleAction`, `ValidScheduleActionInfo/Key` y
  `eErrNoNotExistCurrentScheduleAction`.
- `gamedata/validscheduleaction` (2,5 KB) lista condiciones con objetos: `pickaxe`, `shovel`,
  `hammer`, `heavybucket`, `headpot`, `audience2`… Ninguno de los hashes de §3.1 está en ella.

**(hipótesis B1, principal)** Las acciones de un chart marcado `isNPCScheduleActionChart` solo se
resuelven o cargan su animación cuando el actor tiene schedule o elemento. Con el schedule quitado, la
acción entra (la copia "tiene acción"), pero sin clip. El esqueleto se queda en su pose de referencia,
que para estos humanos es la **pose A** (no la T). Al empezar la pelea llegan acciones de charts
comunes y la copia se anima.

**(hipótesis B2)** La acción necesita además que el schedule **corra**: el secuenciador y el punto de
acción. En ese caso no bastaría con dejarle el schedule a la copia, porque una copia en modo 0 no
ejecuta su IA.

El experimento del §7.2 decide entre B1 y B2. **El arreglo del §6 no depende de cuál sea.**

No se encontró en el exe el código que lee `isNPCScheduleActionChart`: la cadena de depuración no se
referencia directamente, y el resto pasa por `.xtls`. Ver §8.

---

## 5. Pregunta 3a: qué hace `aa4ddb5` con esto [verificado por simulación]

`sim.py` repite la sesión de H con sus `StartActionReq`, sus envíos y sus `ChangeBattleStateReq` a
través de tres versiones de `calm_pose_locked`:
- simula las 4 acciones guardadas (`kKeptActions`), que cuentan también los paquetes que solo tocan la
  capa de abajo;
- mide cuánto tiempo la acción de arriba en curso de cada copia es de solo-schedule, antes de que su
  real entre en batalla.

| Versión | Copias afectadas | Tiempo total |
|---|---|---|
| `6b86521`, la probada (solo la primera acción) | 5 (`0x1A` 389 s, `0x1D` 541 s, `0x26` 372 s, `0x3E` 26 s, `0x4A` 26 s) | **1354 s** |
| `aa4ddb5`, HEAD (pose aprendida y sus repeticiones) | 3 (`0x1A` 10 s, `0x1D` 151 s, `0x3E` 26 s) | **186 s** |
| Propuesta §6 (tabla de solo-schedule) | 0 | **0 s** |

Los dos agujeros de `aa4ddb5`:
1. **Se vuelve a agachar.** `26107D3C>BFCED713` (la entrada) no es la pose. Después,
   `BFCED713>40094A9C` tiene `next == pose`, pero `prev = BFCED713`, que no es 0 ni la pose: la
   condición `(prev != 0 && prev != pose->second)` lo deja pasar.

   La copia queda en pose A hasta la siguiente repetición (~27 s). Si el real ya no repite, queda así
   para siempre: `0x1D` desde las 18:53:51, 151 s.
2. **La pose no se aprende.** `g_pose_of` solo se aprende si la acción `0>pose` está en `g_actions`
   al enviar. Solo se guardan 4 acciones, contando los paquetes de la capa de abajo (`+4 = +8 = 0`).

   `0xB01009DA` se cargó a las 18:55:24.8 y se envió a las 18:55:55 con
   `40094A9C>5BD8B2C2 … BFCED713>40094A9C` (log: "4 recent actions"). La `0>40094A9C` ya no estaba.
   `0x3E` no se calma nunca.

---

## 6. Pregunta 3b: arreglo propuesto (anfitrión, `src/mod/net/enemy_relay.cpp`)

### 6.1 Idea

Una copia **nunca** tiene schedule. Toda acción de arriba que **solo exista en un chart de schedule**
no puede animarse en ella. Hay que cambiarla por `wait_default`:
- siempre, sin aprender poses y sin depender de la pelea;
- en `+0x04` y en `+0x08`.

Es un cambio solo del lado que envía:
- G no cambia y el protocolo tampoco (`kVersion` igual);
- basta con instalar el nuevo `.asi` en el anfitrión.

### 6.2 Datos: tabla de solo-schedule de la build 2976 [verificado]

`sched_only.py` saca las acciones de los 558 `.paac`:
- toma las cabeceras de acción de 276 B, con el patrón `hash | 00×4 | FF×4 | 00×28`. Salen 7944 en
  `schedule_action_nhm`, el número de acciones de ese chart;
- reúne las de los 59 `schedule_action_*.paac`: **13 738 hashes**;
- **ninguno** está definido en otro chart. Eso incluye `nhm` 7944, `nhw` 1381 y `ntm` 864.

Comprobaciones:
- están `0x40094A9C`, `0xBFCED713`, `0x0C5E50CE` y las 7 poses de la mañana;
- **no** están `0x26107D3C` (`wait_default`), `0x5BD8B2C2`, `0x5C8B242E` ni `0xFD6A846A`;
- 0 apariciones en batalla, en H y en G (§3.2).

`emit_table.py` ya genera `build\research\p_agachados\schedule_only_actions_2976.inc`: el array C++
ordenado, 187 KB de texto y ~55 KB en el binario.

### 6.3 Cambio concreto

1. **Nuevo fichero** `src/mod/net/schedule_actions_2976.inc`. Es una copia del `.inc` generado, con
   la build en la cabecera. En `enemy_relay.cpp`:

   ```cpp
   constexpr uint32_t kScheduleOnlyActions[] = {
   #include "mod/net/schedule_actions_2976.inc"
   };
   bool is_schedule_only_action(uint32_t a) {
       return a && std::binary_search(std::begin(kScheduleOnlyActions), std::end(kScheduleOnlyActions), a);
   }
   ```

   Usar la tabla solo si la build del juego es la 2976 (el lector ya comprueba las offsets de esta
   build). Con otra build, queda solo la regla aprendida (punto 3).
2. **`calm_pose_locked(Bytes& req, uint32_t key)`** (hoy en la línea ~565):
   - Condición de entrada: humano de tipo 3 (`g_loads[key][2] == 3`). Para la tabla no hace falta
     `g_schedule_spawned`: ninguna copia tiene schedule. Para la regla aprendida, mantenerlo.
   - `next = +0x08`. Si es 0, es un paquete solo de la capa de abajo: no se toca.
   - Si `is_schedule_only_action(next)`, poner `next = wait_default`.
   - Si `is_schedule_only_action(prev)`, poner `prev = wait_default`. La copia nunca la estuvo
     haciendo, porque se cambió antes.
   - Opcional, suavizado: si `prev` era de solo-schedule y la copia **no** está peleando (`g_engaged`
     < 20 s), poner también `next = wait_default`.

     Así se evita la animación de levantarse (`0x5BD8B2C2`) en una copia que ya está de pie. En
     pelea no se toca: `0xB0100742` entra en batalla (18:53:15.5) antes de levantarse (18:53:16.0).
   - Escribir `+0x04`/`+0x08` solo si algo cambió.
3. **Regla aprendida de `aa4ddb5`**, como respaldo para otra build:
   - quitar `(prev != 0 && prev != pose->second)`: basta con `next == pose`, y `prev == pose` → wait;
   - aprender la pose en **`enemy_observe_c2s()`**, cuando llega una `StartActionReq` de un tipo 3 con
     `+4 == 0` y sin pelea, y no en el envío. Así no depende de las 4 acciones guardadas.

     Para eso, `g_schedule_spawned` debe salir de la carga en `enemy_observe_s2c()`, con una variante
     de solo lectura de `strip_spawn_identity()`, y no en `send_enemy()`.
4. **Registro.** La primera vez por copia:

   ```
   Enemy relay: copy 0x… action 0x40094A9C (schedule only) -> wait_default
   ```

   Y un contador en el resumen periódico. Permite comprobar en la próxima prueba qué copias se
   calmaron.

### 6.4 Riesgos

| Riesgo | Qué hacer |
|---|---|
| La tabla vale solo para la build 2976. Una actualización puede añadir o renumerar acciones | Usarla solo con la build 2976, conservar la regla aprendida y regenerarla con `sched_only.py` + `emit_table.py` (`r_paac.py` vuelve a extraer los `.paac`) |
| La cabecera de 276 B se reconoce por un patrón. Puede faltar alguna acción o colarse un registro que no es acción | Las 10 poses conocidas están y ninguna acción común. Una que falte deja el síntoma como hoy en ese enemigo; un registro de más solo añade un hash que nunca llega |
| Se pierde la postura: la copia queda **de pie** en `wait_default`, no agachada | Es lo que se pidió como mínimo. La postura real es el §7.2 |
| La animación de salida (`0x5BD8B2C2`) en una copia de pie | Suavizado opcional del punto 2 |
| Civiles (refugiados) también usan acciones de solo-schedule (`0xBDBCE29D`, `0x22771FC3`…) | Hoy no se copian. Si algún día se copian, la misma regla les sirve |
| Copias de origen "secuencia" (Q): el sub-chart `sequencer_action` podría tener el mismo problema | No hubo ninguna en esta sesión. Si sale una en pose A, ampliar la tabla con `sequencer_action.paac` |
| Coste: una búsqueda binaria en 13 738 entradas por `StartActionReq` relevada (~1,4/s por enemigo) | Despreciable |

---

## 7. Pruebas

### 7.1 Confirmar el arreglo (barato)

- Mismo campamento "Black Bear" (≈ (-10265, 676, -2330)), con G a menos de 60 m antes de pelear.
- Captura de pantalla de cada lado (`CopyFromScreen`) de los enemigos de `0xBE62E27F`.
- En H, el registro nuevo debería decir `40094A9C`/`BFCED713 -> wait_default` para sus copias.
- En la captura de G no debería llegar a ninguna copia una `StartActionAck` con `+0x08` de la tabla.
  Se comprueba con `fp.py` sobre la nueva captura: debería dar 0 en `B01F…`.
- Esperado: las copias de pie en reposo, nunca en pose A.

### 7.2 Que se agache como en H (experimento; decide entre B1 y B2)

- Interruptor `[dev] enemy_keep_schedule=1`:
  - `strip_spawn_identity()` deja el hash del schedule en `T0+42` y en el bloque de secuencia, y el
    elemento en `T0+46`;
  - pone a cero solo la serie (`T0+50`);
  - la tabla no se aplica a esa copia.
- Primero en un solo PC con `cdmp_peer` y una copia manual (Ctrl+F6) de un bandido agachado.
- Si la copia se agacha, es B1: dejar el schedule en las copias en reposo (con los riesgos de abajo) o
  seguir con la tabla.
- Si sigue en pose A, es B2: la tabla es la solución definitiva.
- Riesgos:
  - el cliente de G puede tratar la copia como un NPC de schedule y pedir cosas a su servidor, que no
    conoce la clave (como el `SetQuestDialogReq` que ya se escapa, `copias-enemigos-faltan-pose-
    minimapa.md` §2.3);
  - puede haber choque con el nativo del mismo elemento si no está oculto.
- Hacer copia de `save` antes, como siempre.

### 7.3 Otra alternativa sin tabla (más trabajo de RE)

- La clave global de una acción lleva a su índice por `0x206C220`/`0x206BC20` (tabla `0x6F40488`,
  `pieza4-tambaleo.md` §6).
- Desde el índice se podría llegar al `ActionChartPackage` dueño y leer su `isNPCScheduleActionChart`
  en el anfitrión, con lectura protegida.
- Así no haría falta una tabla por build, pero hay que localizar el campo, y parte del código está en
  `.xtls`.

---

## 8. Lo que no se pudo cerrar

- **El nombre y el aspecto de `0x40094A9C`.**
  - No sale por fuerza bruta: se probaron ~4 millones de nombres de `index.tsv` y un vocabulario de
    posturas (`sit`, `crouch`, `squat`, `bonfire`, `work`…) con `hashlittle(·, 0xC5EDE)`. Solo
    salieron `wait` y `wait_default`.
  - El `.paac` liga cada acción a sus animaciones por un formato que no está descifrado. La tabla de
    5985 rutas `.paa` de `schedule_action_nhm` está en `0x201604` (`u16 n | n × (u8 len | ruta\0)`).
    Incluye, por ejemplo, `cd_ss00047_37_bonfire_00_1act_phm_*` y `cd_phm_adult_00_00_sit000_*`.
  - Que sea "agachado" se deduce del síntoma visto en la prueba y del ciclo de entrar, bucle y salir. Una
    captura de pantalla en H del enemigo `0xB01006FC` (o del mismo elemento) lo confirmaría.
- **El mecanismo exacto en el motor** (B1 frente a B2), §4.2 y §7.2.
- **Los de campamento con `0x5C8B242E`/`0xAE59880A`** (`0xB0100707` y `0xB01009B9`, ranuras de
  campamento 3 y 1). Su acción está en charts comunes, así que no deberían salir en pose A, aunque
  quizá se vean sentados en el aire sin su punto de campamento. G solo tuvo una copia de ellos
  (`0x41`) en los últimos 36 s de la captura. Mirarlos en la próxima prueba.

---

## 9. Scripts (`build\research\p_agachados\`)

Ejecutar con el `python` del sistema. `capx.py` cachea las dos capturas en `h.pkl`/`g.pkl` (~220 MB,
bajo `build/`, ignorados por git).

| Script | Directorio | Qué hace |
|---|---|---|
| `capx.py`, `prep.py` | `p_agachados` | Lectura, caché y anclas de reloj de las dos capturas |
| `sent.txt` | — | Envíos de H (hora, real, copia, origen), sacados del log |
| `acts.py` | `p_agachados` | Por enemigo enviado: origen, gimmicks acoplados, posición, batalla, primeras acciones |
| `layers.py` → `layers_h.txt`, `layers_g.txt` | `p_agachados` | Pares de arriba y abajo más frecuentes por enemigo y copia |
| `dumpact.py`, `tl.py` | `p_agachados` | Volcado y cronología de `StartActionReq/Ack` de unas claves |
| `off.py` | `p_agachados` | Reloj: `NotifyHandleDeadAck` real frente a copia |
| `bundle.py` | `p_agachados` | Lo que recibió G con cada copia y la siguiente acción de arriba |
| `gnat.py` | `p_agachados` | Nativos de G con las acciones de solo-schedule |
| `near.py`, `gim.py` | `p_agachados` | Gimmicks y actores a menos de 4 m, y cadenas de los gimmicks |
| `ldiff.py <real> <copia>` | `p_agachados` | Diferencias entre la carga real (H) y la recibida (G) |
| `sim.py` | `p_agachados` | Simula `6b86521`, `aa4ddb5` y la propuesta (§5) |
| `fp.py` | `p_agachados` | Acciones de solo-schedule por capa, hostilidad y batalla (falsos positivos) |
| `paac_find.py`, `where.py`, `classify.py` | `build\research` | En qué `.paac` está cada hash |
| `sched_only.py` → `sched_only.pkl` | `build\research` | Las 13 738 acciones de solo-schedule |
| `emit_table.py` → `schedule_only_actions_2976.inc` | `build\research` | La tabla en C++ para el arreglo |
| `paac_anims.py`, `paac_rec.py` | `build\research` | Tabla de rutas `.paa` y cabeceras de acción de `schedule_action_nhm` |
| `brute.py`, `names_from_index.py` | `build\research` | Intentos de nombrar los hashes (sin resultado) |

---

## 10. Resumen

1. Los enemigos agachados son bandidos de **schedule** (`0xBE62E27F`) cuya pose de reposo es
   `0x40094A9C`, un bucle de ~27 s con entrada `0xBFCED713` y salida `0x5BD8B2C2`. Además está
   `0x0C5E50CE`.
2. Esas acciones **solo existen en `schedule_action_nhm.paac`**, como las poses que daban pose A por
   la mañana. No están acoplados a ningún objeto (gimmicks n = 0, sin padre, grupo ni campamento).
3. G recibe la carga real menos la **identidad del schedule** y las acciones tal cual: primero
   `wait_default` y después `0x40094A9C` repetida. La copia existe y tiene acción, pero no se anima:
   pose A.
4. Los nativos de G con el mismo schedule sí la animan, y las copias con acciones de charts comunes
   también. **Causa (media-alta):** una acción del sub-chart de schedule no tiene animación en un
   actor sin schedule.
5. Se probó `6b86521`, no `aa4ddb5` (build `6B865217`; el `.asi` conserva una cadena que `aa4ddb5`
   borró). Su `calm_pose` solo cambiaba la primera acción.
6. `aa4ddb5` mejora mucho (1354 s → 186 s simulados) pero falla al volver a agacharse
   (`BFCED713>pose`) y cuando la acción `0>pose` ya no está entre las 4 guardadas.
7. **Arreglo:** en `calm_pose_locked()` del anfitrión, cambiar por `wait_default` (`+0x04`/`+0x08`)
   toda acción de la tabla de solo-schedule de la build 2976. Son 13 738 hashes, generados y en
   `p_agachados\schedule_only_actions_2976.inc`.
8. El arreglo funciona siempre, sin aprender poses ni depender de la pelea. Simulado: 0 s de pose A.
   Las acciones de la tabla salen 0 veces en batalla. Basta con actualizar el anfitrión; el
   protocolo no cambia.
9. Respaldo: aprender la pose en `enemy_observe_c2s()` y aceptar `next == pose` con cualquier `prev`.
10. Para que se agachen como en H: experimento `[dev] enemy_keep_schedule` en loopback (decide B1/B2).

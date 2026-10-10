# Árbol con cuerda (4): por qué la cuerda de la copia no se ve entera

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`), válidas
solo para esta build. Análisis estático de solo lectura más los logs que ya había; el juego no se
ejecutó. Continúa `arbol-cuerda-3.md`. Lo marcado **(hipótesis)** no está comprobado.

Volcados y scripts nuevos en `build/research/interactions/rope4/` (§9).

Notación: `comp = [[actor+0x68]+0x78]` (`ClientRemoteCatchActorComponent`, vtable `0x55890D8`).
Por ranura `r` (0–1):
- bloque de la cuerda `S = comp + 0x928 + r·0x1A0`;
- estado de control `E = comp + 0x168 + r·0x3E0`.

Doy los offsets absolutos para la ranura 0.

---

## 0. Resumen

1. **La cuerda visible es una malla de spline** (un objeto de escena con un componente
   renderizable) que está en **`S+0x78` = `comp+0x9A0`**. Cada fotograma pasa esto:
   - el tick por ranura (`0x59D470`, que corre **también en la copia**) recalcula los puntos con
     `0x59CAF0`: origen `comp+0x950`, la estela `comp+0xAA8` y el extremo `comp+0x958`;
   - los carga en la malla con `0x589750` → `0x30E1750`;
   - le pone los parámetros del shader desde `RemoteCatchFunctions::vf16` = **`0x11930A0`**, que
     también corre en la copia. Son `_tension`, `_ropeForce`, `_particleChangeRatio`,
     `_targetColorLerpRatio`, `_darkhandBeginOffset`, `_initScaleFromIndexRatio`,
     `_endScaleFromIndexRatio` y `_sourcePosition`.

   La longitud no se guarda en ningún sitio: los puntos salen de los nodos vivos en cada fotograma.
2. **Descartado: el vuelo de la copia sí llega a 1.** Las tres sondas de Shadow (7 oct, 04:06–04:07)
   dan `phase 3`, `flight 0.46/0.44`, `0.42/0.40` y `0.47/0.45`. Es decir, el tiempo transcurrido
   llega a la duración, la copia pasa por la fase 2 y engancha (fase 3), y tiene los cuatro punteros
   de la cuerda. Además, la copia envió ella misma los `IsRemoteCatchEffectStartStateReq` 2108
   `00 01`/`00 00`, y esos solo los mandan `0x5A12A0` (inicio del vuelo) y `0x5A1530` (enganche).
3. **Diferencia confirmada por la sonda: `+AC0 = 0` en la copia en la fase 3.**
   - `comp+0xAC0` (`S+0x198`) es el **estado del efecto de la cuerda**. Solo lo escribe
     **`0x595FE0(comp, on)`**, que pone en las dos ranuras `+0xAC0 = on` y `+0xABC = on ? 1.0 : 0.0`.
   - A `0x595FE0` solo la llaman el frame event **`ClientFrameEventRemoteCatchEffectState::vf1`
     (`0x6D2090`)** y `vf40` (`0x58B380`, que solo apaga, en la fase 4). Los frame events "locales"
     no corren en la copia.
   - `vf16` lleva `comp+0xAB8` (proporción actual) hacia `comp+0xABC` (objetivo), y de esa
     proporción salen cuatro parámetros de la malla: `_targetColorLerpRatio = x`,
     `_darkhandBeginOffset = 1 − 0,25·x`, y `_initScaleFromIndexRatio` y `_endScaleFromIndexRatio`
     (interpolados entre constantes de `.data`).
   - En la copia `x` se queda en 0. **(Hipótesis, confianza media)** Los dos `…ScaleFromIndexRatio`
     escalan los segmentos según su índice a lo largo de la cuerda. Con `x = 0` la cuerda se dibuja
     solo en parte. Es justo el síntoma.
4. **Otras cosas que solo se calculan para el jugador local** (todas por la puerta `0x86A557`, o
   porque son frame events):
   - **(a)** `E+0x38C` (`comp+0x4F4`), `E+0x378` y `E+0x37C` (flexión y su velocidad), que solo
     escribe `0x1190FD0`. Con ellos, `vf16` pone **`_tension = 1.0`** y calcula `_ropeForce` y
     `_particleChangeRatio`. En la copia, `_tension` se queda en 0,9 (el valor del cambio de fase) y
     `_particleChangeRatio` sale de otros campos.
   - **(b)** El frame event `ClientFrameEventRemoteCatchDeliverEffect` (`0x6D2040` → `0x5AA140`),
     que crea más efectos de la configuración en `S+0xE8`/`S+0xF8`.
   - **(c)** `RemoteCatchRopeBody` (`comp+0xCC0`): es solo una prueba de colisión (*shape cast*) a lo
     largo de la cuerda, no se dibuja.
   - **(d)** `0x17A6210([[[actor+0x68]+0x20]+0x20], matriz del objetivo)`, al final de `0x5956A0`.
     Mueve el pivote de los objetos acoplados (**(hipótesis)** el de la herramienta
     `RemoteCatchPivot`, que la copia ni siquiera tiene).
5. **Arreglo propuesto (solo visual, sin cambiar el protocolo):** en el mid-hook que ya existe en
   **`0x86A576`** (`on_actor_update`, en el hilo de actualización de la copia), mientras la copia
   está enganchada a un árbol, hacer lo mismo que `0x595FE0(comp, 1)`:
   - `byte comp+0xAC0+r·0x1A0 = 1`;
   - `f32 comp+0xABC+r·0x1A0 = 1.0`.

   Al soltar, poner 0. Escribir directamente, sin llamar a la función: así no se toma el cerrojo del
   actor. Opcional (R2): mientras la copia tira, `comp+0x4F4 = 1` y `comp+0x4E0` = la fuerza que ya
   calcula `bend_tree`, para que `_tension`, `_ropeForce` y `_particleChangeRatio` coincidan. Antes
   de nada, ampliar la sonda (R0, §6.1) para comparar con el jugador local.

---

## 1. Cómo se dibuja la cuerda

### 1.1 Las piezas del bloque `S`

Lo crea `0x5ABBC0` (inicio, fase 1). Los efectos salen de
`[[0x6D69190]+0xE0]+0xF38 → 0x3B57AE0`, y los nodos de origen y extremo de `0x5AB7A0`, con la
configuración `0x85FB13C7` (`[[0x6D69190]+0xB0] → 0x2456860`). Todos son asas: `[h+8]` es el
objeto, `[h+8]+0x15 == 0` significa vivo y `nodo = [h+8] − 0x28`.

| Campo | Absoluto (r 0) | Qué es | Quién lo coloca o lo usa por fotograma | ¿Corre en la copia? |
|---|---|---|---|---|
| `S+0x00` | `comp+0x928` | efecto de la punta | `0x5AEAA0` lo pone en la transformación del objetivo (`0x5A40E0`) | sí |
| `S+0x08` | `comp+0x930` | efecto del inicio del vuelo | `0x59E150` lo usa como punto de salida; en la fase 3, solo en árboles, `0x5AEAA0` lo escala según la velocidad (`E+0x2E4`, global `0x6C9A3F8`) | sí |
| `S+0x10` | `comp+0x938` | cabeza de la cuerda | `0x598F80`: fase 2, interpolada entre el inicio y el objetivo con el progreso suavizado (`0x1325800`); fase 3, en el objetivo (`0x59D6B6`) | sí |
| `S+0x28` | `comp+0x950` | **nodo A, origen de la spline** | lo lee `0x59CAF0` y da `_sourcePosition` en `0x588E40` | sí |
| `S+0x30` | `comp+0x958` | **nodo B, extremo de la spline** | lo leen `0x59CAF0` y `0x59C660` (estela) | sí |
| `S+0x40` | `comp+0x968` | efecto con `_externalFadeRatio` | `0x59E150` (fase 2) le da el progreso suavizado del vuelo | sí |
| `S+0x50`/`S+0x58` | `comp+0x978`/`0x980` | efectos | `vf52` `0x5970C0` los enciende o apaga según `+0xF6 == 5`; la llama `0x5A1530` (`0x5A299B`) al enganchar | sí |
| `S+0x68` | `comp+0x990` | nodo entre las dos manos | `0x5AEAA0` (`0x5AF522`…): punto medio de dos huesos, orientado hacia el socket | sí |
| **`S+0x78`** | **`comp+0x9A0`** | **malla de la cuerda (spline)** | puntos: `0x589750`; parámetros: `0x5889E0`, `0x588BF0`, `0x588E40`, `0x589540` | sí, con los datos de §2 |
| `S+0x94` | `comp+0x9BC` | fase | | |
| `S+0x170` | `comp+0xA98` | u8: actualizar los puntos de la spline | lo pone `0x5ABBC0` (`0x5ABF17`) | |
| `S+0x174` | `comp+0xA9C` | f32: interpolación del extremo en la fase 5 | `0x5A0390` | |
| `S+0x178` | `comp+0xAA0` | f32: distancia acumulada de la estela | `0x59C660` | |
| `S+0x180` | `comp+0xAA8` | estela: vector `{ptr, u32 n (+0xAB0), u32 cap (+0xAB4)}` de vec3 | `0x59C660` (solo en la fase 3) | |
| **`S+0x190`** | **`comp+0xAB8`** | **f32 `x`: proporción actual del efecto** | `vf16` `0x11930A0` | sí |
| **`S+0x194`** | **`comp+0xABC`** | **f32: objetivo de `x` (0 o 1)** | **solo `0x595FE0`** | **no** (frame event) |
| **`S+0x198`** | **`comp+0xAC0`** | **u8: efecto encendido** | **solo `0x595FE0`** | **no** (frame event) |

Validez de la malla (`0x588780`, y la misma comprobación en `0x5889E0`, `0x588BF0`, `0x588E40`,
`0x589750` y `0x11930A0`):
- `h = [S+0x78]`, `o = [h+8]`, `byte o+0x15 == 0` y `byte o+0x30 ≥ 5`;
- `c = 0x41F6C0(o − 0x28)` (componente renderizable), con `byte c+0x5C != 0`;
- `rend = [c+0x250]`, con `dword rend+0x88 & 0x400 == 0`.

### 1.2 Los puntos: `0x59CAF0(comp, r, &out)`, cada fotograma, desde `0x59D470`

Se llama en `0x59D83B` si `comp+0xA98 != 0` y la malla es válida. Antes, `0x59C660(comp, r)`
actualiza la estela.

1. `out` = copia de la estela `comp+0xAA8` (`0x3A1960`).
2. Si la fase es 3:
   - busca en el componente `[[actor+0x68]+0xF0]` dos listas de actores con las claves de
     `"portalgate0"`/`"portalgate1"` (`0x5913628`/`0x5913638`);
   - si hay un actor enlazado, usa su hueso `"InteractionPivot"` (`0x5589CE0`, con `0x17F8070`) como
     último o primer punto. Es el caso de los portales, que no se da con los árboles.
3. Si la fase es 5, el último punto se interpola hacia el penúltimo con `comp+0xA9C`.
4. Si la fase es 3 y no hubo portal, **añade al final la posición del nodo B** (`comp+0x958`).
5. **Pone al principio la posición del nodo A** (`comp+0x950`), salvo si hay portal. Antes quita
   los puntos que estén a menos de 2 m de A (`0x5B2470`, constante `0x5D732C0 = 2.0`).

`0x59C660` (estela, solo en la fase 3):
- añade la posición del nodo B cuando se ha movido más de `[0x6C99AA8]` y deja de ser colineal (el
  coseno se compara con `cos([0x6C99AF8]·π/180)`);
- `comp+0xAA0` acumula la distancia;
- la estela tiene como máximo `[0x6C99B48]` puntos.

Con un árbol quieto, B no se mueve y la spline es A → B.

**Consecuencia:** la forma de la cuerda sale de dos nodos vivos y la recalcula el tick de la copia
igual que el del jugador local. No hay ninguna longitud guardada en el lanzamiento que pueda dejar la
cuerda corta (§3.2).

### 1.3 Los parámetros del shader: `vf16` `0x11930A0`, cada fotograma, desde `0x59D470`

Llamada: `[comp+0xC68]->vf(0x80)(actor, ranura, dt, &entrada, E, S)` en `0x59D7F3`.
`RemoteCatchFunctions@gameClientScript`, vtable `0x56FFB48`, vf16 = `0x11930A0` (comprobado). Es el
mismo tick que ya corre en la copia (`arbol-cuerda-3.md` §2).

1. `0x11926D0(…)`:
   - fuera de la fase 3: `_ropeForce = 0` (`0x588BF0`);
   - en la fase 3: `_ropeForce` (vec3 + w) a partir de las posiciones de A y B y de `E+0x37C`
     **si `E+0x38C != 0`** (flexión), o de otros campos de control de `E` (`+0x98`, `+0xA0`, `+0xF4`,
     `+0x100`, `+0x26x`…`+0x2Fx`) si no;
   - al final, `_particleChangeRatio` (`0x589540`) a partir de `|E+0x378|` (la fuerza de la
     flexión) si `E+0x38C != 0`, suavizado en `E+0x2F8`.
2. **`_tension`** (`0x5889E0`):
   - si `E+0x38C != 0` (tirando, **solo en el jugador local**), vale 1,0 en cada fotograma;
   - si no, y si el efecto está apagado (`S+0x198 == 0`) y la fase acaba de cambiar
     (`S+0xD0 = comp+0x9F8`), depende de la fase nueva (`S+0xD1 = comp+0x9F9`): fase 2 → 0;
     fases 3 y 4 → **0,9**; fase 5 → 0,5.
3. **Proporción del efecto:**
   - `x = S+0x190`, que se acerca a `S+0x194` a `[0x6C995A8]` por segundo (`0x1193201`…`0x119322A`);
   - después, `0x588E40(S, x, 1 − 0,25·x, lerp([0x6C995F8], [0x6C99648], x), lerp([0x6C99698], [0x6C996E8], x))`.
4. `0x588E40` escribe, en este orden (comprobado con los offsets de la pila):

| Parámetro (hash de) | Cadena | Valor |
|---|---|---|
| `0x6DD555C` | `_targetColorLerpRatio` (`0x5582B68`) | `x` |
| `0x6DD5564` | `_darkhandBeginOffset` (`0x55897E8`) | `1 − 0,25·x` |
| `0x6DD556C` | `_initScaleFromIndexRatio` (`0x5589800`) | `lerp(P5F8, P648, x)` |
| `0x6DD5574` | `_endScaleFromIndexRatio` (`0x5589820`) | `lerp(P698, P6E8, x)` |
| `0x6DD557C` | `_sourcePosition` (`0x5589838`) | posición de mundo del nodo A (`S+0x28`) |

- Todos van por `objeto->vf(0x308)(hash, valor)` sobre el objeto de `S+0x78`.
- Las constantes `P… = [0x6C99xxx]` están en una zona sin inicializar del ejecutable. Las rellena
  al arrancar código ofuscado (`0xD50FCF0`), así que **solo se pueden leer en vivo**.
- Constantes de `.xdata`: `0,25` (`0x5D72E50`), `0,9` (`0x5D73058`), `0,5` (`0x5D72F34`) y `1,0`
  (`0x5D730B4`).

### 1.4 El estado del efecto: `0x595FE0`

```
0x595FE0(rcx = comp, dl = on):
  toma el cerrojo del actor ([comp+8], 0x393650 con r8b = 1)
  para r en 0..1:
    si byte [comp+0xAC0+r*0x1A0] != on:
      byte [comp+0xAC0+r*0x1A0] = on
      f32  [comp+0xABC+r*0x1A0] = on ? 1.0 : 0.0
  suelta el cerrojo
```

Sus únicos llamadores son:
- `ClientFrameEventRemoteCatchEffectState::vf1` = `0x6D2090` (vtable `0x5584568`): el byte de datos
  `+8` vale 1 (encender), 0 (apagar) o 2 (nada);
- `vf40` `0x58B380` (`0x58B70C`), que apaga en la fase 4 al cambiar de acción.

El reinicio de la suelta (`0x5A7BC0` → `0x589820`) limpia el estado `E`, incluido `E+0x38C`, pero no
toca `S+0x198`. Otros lectores de `+0xAC0`/`+0xABC`/`+0xAB8`: solo `0x11930A0`
(`rope4/offs_rope_nodes.txt` y el rastreo de `+0x198`/`+0x194`/`+0x190`).

**Es decir, todo lo que hace `S+0x198` es visual.**

### 1.5 Lo que se actualiza solo para el jugador local (`0x5956A0`, puerta `0x86A557`)

Repaso completo de `0x5956A0` (`rope3/c_5956a0.asm`):

| Paso | Dirección | Qué hace | ¿Visual? |
|---|---|---|---|
| Script `vf2` | `0x595CAF` → `0x117F7E0` → `0x1190FD0` | flexión del árbol (`SetBend`); escribe `E+0x370`…`E+0x38D` (**`E+0x38C`, `E+0x378`, `E+0x37C`, `E+0x380`**) | indirectamente: `vf16` usa `E+0x38C`/`+0x378`/`+0x37C` para `_tension`, `_ropeForce` y `_particleChangeRatio` |
| `comp+0xCA0` `vf(0x20)` | `0x595B96` | runtime de `allocateRemoteCatchFishingPullRuntime` (pesca y tirones) | no en árboles |
| **RopeBody** `comp+0xCC0` `vf2` | `0x595DA6` → `0x1197640` | *shape cast* (`0x3A30A20`) del objetivo al actor con la cápsula creada en `vf1` `0x11972F0` (longitud = `clamp(distancia − 1, [0x6C99878], [0x6C998C8])`, medida al empezar en `0x594F76`). Guarda el punto de choque en `+0x18`/`+9` | **no** (prueba de obstáculos) |
| Pivote | `0x595E57`…`0x595F37` → `0x17A6210([[[actor+0x68]+0x20]+0x20], matriz)` | copia la matriz del objetivo de la ranura 0 en `+0x80`…`+0xBF` de los objetos acoplados que cumplen una regla de tipo 1 y tienen `+0x132 & 0x20` | **(hipótesis)** el pivote `RemoteCatchPivot` de la herramienta (`0xB010xxxx`); la copia no la tiene (`arbol-cuerda-2.md` §3.2) |

La RopeBody se crea en `0x594AD0` (`0x594E3F`…`0x594F76`, también en la copia, si el global
`0x6C999B8 != 0`) con la fábrica `createRemoteCatchRopeBody` `0x1197A90`, pero en la copia
**nunca se actualiza**. No influye en el dibujo.

Frame events con efectos de la cuerda que la copia no ejecuta (son de animación y no pasan
`0x4F42B0`, `arbol-cuerda-3.md` §2):
- `ClientFrameEventRemoteCatchEffectState` (`0x6D2090` → `0x595FE0`): §1.4;
- `ClientFrameEventRemoteCatchDeliverEffect` (`0x6D2040` → `0x5AA140`): vuelve a montar las listas
  de efectos `S+0xE8`/`S+0xF8` (`0x5A9D30`/`0x5B2650`) desde la configuración `0x85FB13C7`, con la
  ranura en `datos+0x14`. **(Hipótesis)** son los efectos de "entrega" a lo largo de la cuerda o en
  el objetivo.

---

## 2. Qué valores tiene la copia (logs)

Hay sondas de la cuerda en un solo log:
`build/research/shadow-20261007/cdmp.log` (Shadow como receptor, 7 oct). Revisé todos los
`bin64/cdmp*.log` y los logs del invitado. Ni la tarde del 7 de octubre
(`20261007-tarde`, `bin64/cdmp.log` de las 18:42) ni `cdmp.1.log` (07:14) tienen ningún enganche
retransmitido. En `071430` solo engancha el jugador local, y entonces la sonda no se dispara.

```
04:06:09.355 rope probe | copy: type 0 piece 0x800000000100171B phase 3 flight 0.46/0.44 rope 5A32CC29590 5A32CC26210 5A32CC21520 5A32CC201E0 +AC0 0 +CA8 1 +D00 0x0 | ours: type 6 … phase 7 …
04:06:44.104 rope probe | copy: type 0 piece 0x800000000100171B phase 3 flight 0.42/0.40 rope … +AC0 0 +CA8 1 +D00 0x0 | ours: (sin enganche)
04:07:45.525 rope probe | copy: type 0 piece 0x800000000100171B phase 3 flight 0.47/0.45 rope … +AC0 0 +CA8 1 +D00 0x0 | ours: (sin enganche)
```

| Campo | Copia | Lectura |
|---|---|---|
| `+0x9BC` | 3 | enganchada |
| `+0xA90/+0xA94` | 0,46/0,44 · 0,42/0,40 · 0,47/0,45 | **el vuelo terminó** (el transcurrido llega a la duración; ~0,4 s = ~19 m a 45 m/s) |
| `+0x928/+0x930/+0x968/+0x970` | no nulos | los efectos existen |
| **`+0xAC0`** | **0** | **efecto apagado**: no llegó el frame event `EffectState` |
| `+0xCA8` | 1 | `0x594AD0` terminó bien |
| `+0xD00` | 0 | sin pendiente del servidor (normal en la copia) |
| "ours" | `type 6`, fase 7 | **sin referencia**: el jugador local no estaba enganchado |

**Lo que falta saber:** el valor de `+0xAC0`/`+0xAB8` en el **jugador local** mientras está enganchado
a un árbol. La sonda de ahora no lo muestra (§6.1).

---

## 3. Por qué la cuerda de la copia se queda a medias

### 3.1 ¿El vuelo no llega a 1 porque se salta la fase 2? — **No**

- La sonda da fase 3 con `+0xA90 ≥ +0xA94` en los tres enganches.
- La copia no se salta la fase 2. El 2763 inyectado entra por `0x594AD0` → `0x5ABBC0`, que pone la
  fase 1 y llama a `0x5A12A0` (fase 2). Su tick (`0x59E150`) hace avanzar `+0xA90` y llama a
  `0x5A1530` (fase 3).
- Confirmación externa: los 2108 `00 01`/`00 00` que envió Shadow los mandan `0x5A12A0`
  (`0x5A14FF`) y `0x5A1530` (`0x5A29BD`), las dos únicas llamadas al escritor `0xB56B00`
  (`mov ecx, 0x83C`).
- La llamada de vuelta del efecto (`0x58A1C0`) también funciona con la copia: `0x8AF170` exige
  `byte [actor+0x5E] & 0x18 != 0`, y la copia tiene `0x10`. La nota de `arbol-cuerda-2.md` §3, paso
  2, no implicaba que fallara.
- `_externalFadeRatio` de `comp+0x968` recibe el progreso suavizado en cada fotograma de la fase 2,
  también en la copia.

### 3.2 ¿La longitud o los segmentos se fijan con la distancia del lanzamiento? — **No en lo que se dibuja**

- Los puntos de la malla se recalculan en cada fotograma a partir de los nodos A y B (§1.2).
- La única longitud fija es la cápsula de la RopeBody (§1.5), que solo sirve para colisiones y que
  en la copia ni se actualiza.

### 3.3 ¿Falta un frame event? — **Sí: `ClientFrameEventRemoteCatchEffectState`**, y seguramente `DeliverEffect`

Cadena del fallo (hechos):
1. En el emisor, la animación del enganche lleva un frame event `RemoteCatchEffectState` que llama a
   `0x595FE0(comp, 1)`. **(Hipótesis sobre el momento exacto:** durante `9a368460`/`2821f20a`; no
   viene en ninguna captura porque no es de servidor).
2. En la copia no corre: `+0xAC0` se queda en 0 (sonda) y `+0xABC` en 0.
3. `vf16` (que sí corre en la copia) lleva `x = +0xAB8` hacia 0. Desde la creación (`0x589820` la
   deja a 0) nunca sube.
4. La malla recibe los parámetros "apagados":
   - `_targetColorLerpRatio = 0`;
   - `_darkhandBeginOffset = 1,0` (encendido: 0,75);
   - `_initScaleFromIndexRatio = [0x6C995F8]` (encendido: `[0x6C99648]`);
   - `_endScaleFromIndexRatio = [0x6C99698]` (encendido: `[0x6C996E8]`).

**(Hipótesis, confianza media)** `_initScaleFromIndexRatio`/`_endScaleFromIndexRatio` son el tramo,
como fracción del índice del segmento, en el que la malla pasa de escala 0 a escala completa, y
`_darkhandBeginOffset` es dónde empieza la parte "Darkhand" (la mano oscura de la punta). Con los
valores "apagados" solo se vería una parte de la cuerda. Encaja con lo que se vio en la prueba (la
cuerda no se veía completa, solo una parte), y con que antes de arreglar la pieza se quedaba en el
brazo: entonces el extremo estaba en el punto de reposo.

**(Hipótesis, confianza baja-media)** Además, en la copia faltan los efectos de
`ClientFrameEventRemoteCatchDeliverEffect` (§1.5) y el pivote de la herramienta (`0x17A6210`, más
la herramienta misma). Si una parte visible de la cuerda del jugador local sale de ahí, el estado
del efecto por sí solo no lo arregla del todo.

### 3.4 Lo que no explica el síntoma pero también es distinto

- **`_tension`:** el jugador local la tiene a 1,0 mientras tira (`E+0x38C`). La copia la deja en 0,9
  desde que entra en la fase 3. **(Hipótesis)** cambia la comba, no la longitud.
- **`_ropeForce`/`_particleChangeRatio`:** en el jugador local salen de la flexión (`E+0x37C`,
  `E+0x378`). En la copia, de campos de control que no escribe nadie. El mod (R1 de
  `arbol-cuerda-3.md`) llama a `SetBend` directamente y no toca `E`.

---

## 4. Hechos e hipótesis

| Afirmación | Tipo | Evidencia |
|---|---|---|
| La malla de la cuerda está en `comp+0x9A0` y sus puntos son A (`+0x950`), la estela (`+0xAA8`) y B (`+0x958`), recalculados en cada fotograma por `0x59CAF0` | hecho | `0x59D7F9`…`0x59D85B`, `0x59CAF0`, `0x589750` → `0x30E1750` |
| Los parámetros del shader los pone `vf16` `0x11930A0`, que corre en la copia | hecho | `0x59D7F3` en el tick por ranura; vtable `0x56FFB48` vf16 |
| `+0xAC0`/`+0xABC` solo los escribe `0x595FE0`, que solo llaman `0x6D2090` (frame event) y `0x58B380` (apagar) | hecho | `callers.py`; rastreo de offsets en `0x400000`…`0x1400000` |
| En la copia `+0xAC0 = 0` en la fase 3 | hecho | 3 sondas de Shadow |
| El vuelo de la copia termina y pasa a la fase 3 | hecho | sondas y 2108 de la copia |
| En el jugador local `+0xAC0 = 1` mientras está enganchado a un árbol | **hipótesis** (no hay sonda) | el frame event existe para eso |
| Con `x = 0`, los parámetros `…ScaleFromIndexRatio`/`_darkhandBeginOffset` dibujan solo una parte | **hipótesis** | nombres y fórmulas; constantes solo en vivo |
| `_tension = 1,0` y `_ropeForce` dependen de `E+0x38C`, que solo escribe `0x1190FD0` | hecho | `0x1193176`, `0x119282C`, `0x1192F6A`, `0x11912BC` |
| RopeBody es solo colisión y solo se actualiza en el jugador local | hecho | `0x11972F0`/`0x1197640` (`0x3A19A90`/`0x3A30A20`), su llamador es solo `0x5956A0` |
| `0x17A6210` mueve el pivote de la herramienta | hipótesis | campo `+0x132 & 0x20`, matriz en `+0x80`; la herramienta lleva `RemoteCatchPivot` |

---

## 5. Propuesta de arreglo

### R0 — Ampliar la sonda (solo lectura, riesgo nulo) — **lo primero**

Añadir a `rope_slot_text` (ranura 0, con `game::read`, que ya va protegida con `VirtualQuery`/SEH):

| Campo | Offset | Esperado (local / copia sin arreglo) |
|---|---|---|
| efecto encendido | `u8 comp+0xAC0` | 1 / 0 |
| objetivo de `x` | `f32 comp+0xABC` | 1,0 / 0,0 |
| `x` | `f32 comp+0xAB8` | → 1 / 0 |
| actualizar spline | `u8 comp+0xA98` | ≠ 0 en los dos |
| estela | `u32 comp+0xAB0` (n), `f32 comp+0xAA0` | parecidos |
| fase vista por el tick | `u8 comp+0x9F9`, `u8 comp+0x9FA` | 3 / 3 |
| flexión | `u8 comp+0x4F4`, `f32 comp+0x4E0`, `f32 comp+0x4E4` | tirando: 1, > 0 / 0, 0 |
| malla viva | `h = [comp+0x9A0]`, `o = [h+8]`, `u8 o+0x15`, `u8 o+0x30` | `o ≠ 0`, 0, ≥ 5 |
| nodos A/B vivos | `[comp+0x950]`, `[comp+0x958]` → `[h+8] ≠ 0` | sí / sí |

Y que también **se dispare con el enganche del jugador local**. Hoy solo se programa al relevar un
2763 de la copia (`remote_puppet.cpp:1691`). Hay que programarla igual cuando el 2763 S→C es del
jugador local, o, más sencillo, cada 1 s mientras el `comp+0x9BC` local esté en 2 o 3.

Prueba T0: en un PC con `cdmp_peer`, engancharse a un árbol con el jugador local y, aparte, recibir
un enganche de la copia. Con eso se sabe si `+0xAC0` del jugador local es 1. Si fuera 0, el estado
del efecto no es la causa y hay que mirar `DeliverEffect` y la herramienta (§3.3).

### R1 — Encender el estado del efecto en la copia (confianza media, solo visual) — **el arreglo**

**Dónde:** el mid-hook que ya existe, `exe+0x86A576` (`on_actor_update`).
- Corre en el hilo de actualización de la copia, justo antes de `0x8A0FC0`, que registra el tick por
  ranura que lee estos campos.
- Hoy la función sale enseguida si la copia no está tirando. Hay que separar esta parte para que
  corra siempre que `[rsi+8] == g_copy_actor` y `g_copy_hooked`.
- Conviene ponerlo detrás de su propia opción `[dev]` (por ejemplo `rope_effect = 1`) para poder
  comparar.

**Qué (escritura directa, lo mismo que `0x595FE0`):**

```
copia = g_copy_actor; si [rsi+8] != copia: salir
__try {
  comp = [[copia+0x68]+0x78]; si !comp: salir
  fase0 = u8 [comp+0x9BC]; tipo0 = u8 [comp+0x20]
  on = g_copy_hooked && tipo0 == 0 && (fase0 == 2 || fase0 == 3)
  si on != g_effect_on:                       // solo al cambiar, como hace 0x595FE0
     para r en 0..1:
        u8  [comp+0xAC0+r*0x1A0] = on
        f32 [comp+0xABC+r*0x1A0] = on ? 1.0f : 0.0f
     g_effect_on = on
} __except { desactivar para toda la sesión y registrarlo (como bend_tree) }
```

- **Por qué escribir directamente y no llamar a `0x595FE0`.** `0x595FE0` toma el cerrojo de ámbito
  del actor (`0x393650` con `r8b = 1`), y aquí ya estamos dentro de la actualización de ese mismo
  actor. No se sabe si el cerrojo es reentrante. Son un byte y un float alineados, y el juego solo
  los lee en `0x11930A0`, así que no hay condición de carrera que importe.
- **Momento.** Empezar con la fase 2 da el mismo resultado que el emisor salvo unas décimas: la
  rampa de `x` (`[0x6C995A8]`/s) suaviza la diferencia. Si R0 enseña que en el jugador local se
  enciende más tarde (por ejemplo, solo en la fase 3), usar solo la fase 3.
- **Apagado.** Cuando `g_copy_hooked` pasa a falso (al inyectar el 2471) o la fase sale de {2, 3}.
  Hay que dejarlo así, porque la suelta (`0x5A7BC0`) no lo reinicia (§1.4).
- **Al recargar la copia:** el componente es nuevo y ya empieza a 0. Poner `g_effect_on = false`.

**Riesgos:**
- Visual: si `+0xAC0 = 1` significa otra cosa en el jugador local (R0), el color y la escala de la
  cuerda de la copia podrían no coincidir. Se nota a simple vista y la opción `[dev]` lo revierte.
- Excepción: todo dentro de `__try`, y se apaga a la primera, como `bend_tree`. No se llama a código
  del juego, así que no queda ningún cerrojo tomado.
- Concurrencia: son escrituras escalares en un componente que solo es de la copia, en el hilo de su
  propia actualización.

### R2 — Flexión visible en la cuerda de la copia (confianza media, solo visual, opcional)

Dentro de `bend_tree`, cuando se llama a `SetBend` con `strength` y `dir`:

```
f32 [comp+0x4E0] = strength                  // E+0x378: fuerza
f32 [comp+0x4E4] = kBendPerSecond o 0         // E+0x37C: velocidad (para _ropeForce)
f32 [comp+0x4E8] = dir.x; [comp+0x4EC] = 0; [comp+0x4F0] = dir.z   // E+0x380
u8  [comp+0x4F4] = 1                          // E+0x38C: flexión en curso
```

Y `u8 [comp+0x4F4] = 0` cuando la copia deja de tirar (en la rama de `on_actor_update` que hoy pone
`g_pull_seconds = 0`) o al soltar. Con eso, `vf16` da `_tension = 1,0`, y `_ropeForce` y
`_particleChangeRatio` salen de la fuerza, como en el jugador local.

- Lectores de `E+0x38C`: `0x1190FD0` (solo local, no corre en la copia), `0x11926D0` y `0x11930A0`
  (visuales), `0x5A87B0` (lo copia) y los reinicios `0x589820`/`0x587B70`. Riesgo bajo.
- No hace falta para el síntoma "cuerda a medias".

### R3 — Si R1 no basta: efectos de `DeliverEffect` y de la herramienta (confianza baja)

- `DeliverEffect`: llamar a `0x5AA140(comp, &resultado, &datos_fe)` hace falta un bloque de datos de
  frame event (`+0x14` ranura y más campos sin decodificar), y esa función crea y destruye efectos.
  **No lo recomiendo** sin decodificar antes `0x5AA140` entero.
- Herramienta: que la copia tenga la herramienta (`OnDemandEquipDockingGimmickSpawnReq` sin
  respuesta) es otro trabajo, y `0x17A6210` también es solo local. Antes de dedicarle tiempo, R0
  debería confirmar con una captura (`CopyFromScreen`) si la parte que falta coincide con la
  herramienta.

### Descartado

| Opción | Por qué no |
|---|---|
| Forzar el progreso del vuelo (`+0xA90`) | El vuelo ya termina (§3.1) |
| Ajustar una longitud de la cuerda | No hay ninguna: los puntos son de los nodos vivos (§3.2) |
| Llamar a `0x5956A0` o a la RopeBody en la copia | Mete la entrada del receptor (`arbol-cuerda-3.md` §2) y la RopeBody no se dibuja |
| Poner `[CC+0x1E9] = 1` para que corran los frame events | Enciende todos los frame events locales de la copia (golpes, `DoFrameEventReq`…) |
| Llamar a `0x595FE0` desde el hilo de red | Toma el cerrojo del actor fuera de su actualización; además no hace falta |

---

## 6. Pruebas

| # | Prueba | Decide |
|---|---|---|
| T0 | R0 en un PC (`cdmp_peer`): enganche del jugador local y de la copia | si el jugador local tiene `+0xAC0 = 1`/`x → 1` (base de R1) |
| T1 | R1 en un PC, con captura de pantalla de las dos cuerdas | si la cuerda de la copia sale entera |
| T2 | R1 en dos PC (Shadow como receptor) | lo mismo con una pieza distinta en cada PC |
| T3 | R1 + R2 tirando de un árbol | que la cuerda de la copia se tensa como la del jugador local |
| T4 | Enganchar, soltar y volver a enganchar | que `+0xAC0` vuelve a 0 entre enganches y no hay excepciones |

### 6.1 Formato sugerido de la sonda

```
rope probe | copy: … phase 3 flight a/b | fx +AC0 1 +ABC 1.00 +AB8 0.73 | spline +A98 1 trail n 0 d 0.00 | bend +4F4 0 f 0.00 | mesh ok A ok B ok
```

---

## 7. Conclusiones

1. **Cómo se dibuja.**
   - Es una spline (`comp+0x9A0`) del nodo A (`comp+0x950`, el "source", unido a un socket del actor
     por `0x5AB7A0`) al nodo B (`comp+0x958`), pasando por la estela.
   - La punta (`comp+0x928`) va al punto de enganche (la transformación del objetivo `0x5A40E0`, es
     decir, el `+0x26`/`+0x3E` del 2763 compuesto con la pieza o el objeto de escena).
   - El progreso del vuelo (`+0xA90/+0xA94`) solo mueve la cabeza (`comp+0x938`) y
     `_externalFadeRatio` (`comp+0x968`) en la fase 2.
   - Solo en el jugador local: el estado del efecto (frame event), la flexión (`E+0x38C`/`+0x378`), la
     RopeBody, el pivote `0x17A6210` y `DeliverEffect`.
2. **Por qué se queda a medias:**
   - no es el vuelo (termina);
   - no es una longitud fija (no la hay);
   - es, con la mayor probabilidad, el estado del efecto, que se queda apagado
     (`+0xAC0 = 0` medido) porque falta el frame event `RemoteCatchEffectState`.
3. **Logs:** solo hay 3 sondas, las de Shadow del 7 oct: `flight 0.46/0.44`, `0.42/0.40` y
   `0.47/0.45`, con `+AC0 0`. No hay valores del jugador local enganchado.
4. **Arreglo:**
   - R1: `+0xAC0 = 1` y `+0xABC = 1.0` en las dos ranuras desde el mid-hook `0x86A576`, con
     escritura directa dentro de `__try`, y apagarlo al soltar;
   - opcional, R2: `E+0x38C`/`+0x378` al doblar;
   - antes, R0 para confirmarlo.

---

## 8. Confianza

| Afirmación | Confianza |
|---|---|
| Mecanismo de la spline (puntos A–estela–B y `vf16` para los parámetros) | alta (código) |
| El vuelo de la copia termina | alta (sondas y 2108) |
| `+0xAC0 = 0` en la copia por el frame event que falta | alta (sonda + único escritor) |
| Que eso sea lo que deja la cuerda a medias | **media** (depende de las constantes que solo se ven en vivo y de cómo las use el shader) |
| R1 no tiene efectos fuera de lo visual | alta (únicos lectores: `0x11930A0`) |
| `DeliverEffect`/herramienta como causa adicional | baja |

---

## 9. Archivos nuevos (`build/research/interactions/rope4/`)

| Archivo | Contenido |
|---|---|
| `vt.py` | vtables y vfuncs de una clase por su nombre RTTI |
| `c_59caf0.asm` | puntos de la spline (A, estela, B; portales) |
| `c_59c660.asm` | estela de B en la fase 3 |
| `c_589750.asm` | carga de los puntos en la malla (`0x30E1750`) |
| `c_588780.asm` | comprobación de la malla válida |
| `c_5889e0.asm` | `_tension` |
| `c_588bf0.asm` | `_ropeForce` |
| `c_588e40.asm` | `_targetColorLerpRatio`, `_darkhandBeginOffset`, `_init`/`_endScaleFromIndexRatio`, `_sourcePosition` |
| `c_589540.asm` | `_particleChangeRatio` |
| `c_11926d0.asm` | fuerza y partículas de la cuerda (usa `E+0x38C`/`+0x378`/`+0x37C`) |
| `c_595fe0.asm` | estado del efecto (`+0xAC0`/`+0xABC`) |
| `c_589820_slot_reset.asm` | reinicio del estado `E` (desde la suelta `0x5A7BC0`) |
| `c_5ab7a0.asm` | creación de los nodos A/B y `S+0x18`/`S+0x20` |
| `c_5aa140_deliver_effect.asm` | `ClientFrameEventRemoteCatchDeliverEffect` → efectos `S+0xE8`/`S+0xF8` |
| `c_1197a90_ropebody_factory.asm` | `createRemoteCatchRopeBody` |
| `c_17a6210_pivot_matrix.asm` | matriz del objetivo en los objetos acoplados (solo local) |
| `c_26d3840_node_world_xf.asm` | transformación de mundo de un nodo (`+0x1CC`…`+0x1F6` si no tiene padre) |
| `offs_rope_nodes.txt` | quién usa `+0x950/+0x958/+0x9A0/+0xA98/+0xAA0/+0xAA8/+0xAB8/+0xABC/+0xAC0` |
| `vf52_calls.txt` | llamadas virtuales a `+0x1A0` (busca de `vf52`) |

Ya volcados antes y usados aquí: `rope2/c_11972f0.asm` y `c_1197640.asm` (RopeBody `vf1`/`vf2`),
`rope2/c_59d470.asm`, `c_59e150.asm`, `c_5aeaa0_slot_post.asm`, `c_58a1c0_phase1to2.asm`,
`c_6d2090.asm`, `c_6d2040.asm` y `rope3/c_11930a0_script_tick.asm`, `c_5956a0.asm`, `c_598f80.asm`.

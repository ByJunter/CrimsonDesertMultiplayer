# Árbol con cuerda (3): qué dobla el árbol y cómo doblarlo en la copia

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`), válidas
solo para esta build. Análisis estático de solo lectura y capturas; el juego no se ejecutó.
Continúa `arbol-cuerda-2.md`. Lo marcado **(hipótesis)** no está comprobado.

Capturas y logs usados:

| Archivo | Qué es |
|---|---|
| `bin64/cdmp_packets_20261007-050207.bin` | anfitrión (emisor), 7 oct, enganches a 223,66 / 258,40 / 319,82 s |
| `build/research/shadow-20261007/cdmp_packets_20261007-040239.bin` + `cdmp.log` | Shadow (receptor), mismos enganches (reloj de Shadow = anfitrión − 1 h) |

Volcados y scripts nuevos en `build/research/interactions/rope3/` (§8).

---

## 0. Resumen

1. **La fase 4 no dobla el árbol.** En los enganches a árboles no hay fase 4: `vf50` (`0x5A0770`)
   sale de un frame event de "recoger la cuerda". Si se ejecutara, el cliente enviaría un
   `DoFrameEventReq` de variante 6 (`0x593B50` → `0x4EC430`), y en los 15 enganches de las
   capturas no aparece ninguno durante el tirón. `vf51` (`0x5A68D0`) solo sirve con un **actor**
   como objetivo, para atraer enemigos.
2. **El árbol se dobla por una flexión procedural del `TreeComponent`** (componente del nodo de
   escena del árbol), no por física. Cada fotograma se llama a
   **`TreeComponent::SetBend` = `0x349EA10(tree, const vec3* dir, float fuerza)`**. Si un
   fotograma no llega la llamada, el propio `TreeComponent` vuelve a la posición recta con una
   oscilación amortiguada (`0x349D46C`…`0x349D7AD`).
3. **Quién llama a `SetBend`:**
   - **(A) principal, durante el tirón:** `0x86A310` (actualización del CharacterControl) →
     `0x5956A0` (actualización local del componente de la cuerda) → `RemoteCatchFunctions::vf2`
     `0x117F7E0` → **`0x1190FD0`** → `SetBend`.
   - **(B) secundaria:** `0x8A30C0`, dentro de la actualización local del movement controller
     (`0x87A700`), durante un tiempo después del frame event `ClientFrameEventTreePulling`
     `0x6D46D0` **(hipótesis: cuando el actor está a la altura de la copa, en el lanzamiento)**.
4. **Por qué la copia no lo hace:**
   - (A) tiene una puerta en `0x86A557`: `actor->vf(0x178)` = `0x4FE260`, que exige
     `[actor+0xA0] ==` usuario local **y** `byte [actor+0x94] != 0`. Es decir, solo el personaje
     del jugador local. La copia no la pasa, así que nunca ejecuta `0x5956A0`.
   - (B) solo corre en el modo de control 1 (`0x88F4E0`: modo 1 → `0x87A700`, modo 2 →
     `0x87B760`). La copia está en el modo 2.
   - Los frame events con la bandera `+6 & 4` solo corren si `byte [CC+0x1E9] != 0`
     (`ClientFrameEventProcessor::vf4` `0x4F42B0`).
   - Además, en el emisor la flexión depende de **su entrada**: las teclas
     `"Key_RemoteCatchTreePulling"` y `"RemoteCatch_ControlPosition_Pull"`, leídas en `0x1190FD0`.
5. **No hay ningún mensaje de red de la flexión.** En las capturas no viaja nada con el id de escena
   del árbol durante el tirón, y ningún Ack S→C llega a `SetBend` (§4).
6. **Arreglo recomendado (receptor, solo visual):** hacer en la copia lo que hace `0x1190FD0`.
   Cada fotograma, mientras la copia está tirando, llamar a `SetBend(tree, dir, fuerza)` desde un
   mid-hook en **`0x86A576`** (la misma función y el mismo hilo en que el juego lo hace para el
   jugador local). `tree` sale de `0x491AC0(0x5B1600(comp_copia, 0))`. Para que la amplitud sea
   fiel, el emisor puede enviar su propia flexión (`comp+0x4E0` y `comp+0x4E8`, §5 R3).

---

## 1. Qué dobla el árbol en el jugador local

### 1.1 `TreeComponent` (componente del nodo de escena del árbol)

Se obtiene con `0x491AC0(nodo)`, que recorre los componentes del nodo (`nodo+0x210`, con
`nodo+0x218` componentes) buscando el tipo de `0x48D680` (`ReflectMetaObjectBind<TreeComponent>`,
`0x5580F88`). Campos que se usan:

| Campo | Tipo | Contenido |
|---|---|---|
| `+0x59` | u8 | bit 3 (`& 8`): registrado en la lista de actualización del dueño |
| `+0x60` | handle | dueño: `[[tree+0x60]+8] − 0x28` = `SceneObjectClient` (vtable `0x557E120`). `vf 0x4A8` lo registra; `vf 0x400`, en la actualización, aplica los parámetros al dibujo |
| `+0x218` / `+0x220` | ptr | objeto que crea el "agarre" (`0x349A5D0`, desde `0x5B1800`); lo libera `0x349A780` |
| `+0x278` | u8 | árbol agarrado (lo ponen a 1 `0x5B1800` y a 0 la suelta `0x5A826C` y el lanzamiento `0x5B2021`) |
| `+0x29C` | f32 | amplitud de la vuelta (`k · fuerza`, con `k = [0x6D26288]`) |
| `+0x2A0` | f32 | fase de la vuelta (π/2 al refrescarse) |
| **`+0x2A4`** | f32 | **fuerza de la flexión** (de −1 al tope; la usa el dibujo, `0x349ECD0`) |
| **`+0x2A8`** | vec3 | **dirección de la flexión** (horizontal: `y = 0`, normalizada) |
| **`+0x2B4`** | u8 | refrescado en este fotograma (lo borra la actualización) |

**`SetBend` `0x349EA10(rcx = tree, rdx = const float dir[3], xmm2 = fuerza)`:**
1. copia `dir` en `+0x2A8`, pone `+0x2AC = 0` y normaliza (`0x393960`);
2. `+0x2A4 = fuerza`, `+0x2B4 = 1`;
3. si `+0x59 & 8 == 0`, llama a `dueño->vf(0x4A8)(tree)`, y luego `+0x59 |= 8`.

No hace nada más: no toca física ni red.

**Actualización (`0x349D46C`…`0x349D7AD`, sin llamadores directos; la llama el dueño):**
- con `+0x2B4 == 1`: `+0x2A0 = π/2`, `+0x29C = k · +0x2A4` y `+0x2B4 = 0`. La flexión se queda
  como está.
- sin refrescar: oscila con amortiguación desde la fuerza actual hasta 0 (`+0x29C` baja con `dt`
  y `+0x2A0` avanza). Al final `+0x29C = 0` y se da de baja.
- En los dos casos pasa los parámetros al dibujo (`dueño->vf(0x400)` → `0x3A6F560`).

Es una **flexión visual procedural** (la "fuerza" del dibujo del árbol). La física de la cuerda
(fases 2 y 3) no la produce. **(Hipótesis)** el cuerpo de la pieza del tronco no se mueve con la
flexión. Si se moviera, el extremo de la cuerda de la copia la seguiría solo, porque ya usa su
propia pieza (`fill_rope_piece`).

### 1.2 Fuente A: `0x1190FD0` (la flexión del tirón)

Cadena, cada fotograma, **solo para el personaje local**:

```
0x86A310 (CharacterControl, rsi = CC, xmm6 = dt; hilo de actualización de actores)
  0x86A550  rcx = [CC+8] (actor); call [vt+0x178]   -> 0x4FE260 / 0x8A67D0
  0x86A55F  je 0x86A576                             ; la copia salta aquí
  0x86A571  0x5956A0(comp = [[actor+0x68]+0x78], dt)
              por ranura con entrada de tipo 0 (árbol) o 4, y sin actor objetivo:
              0x595CAF  [comp+0xC68]->vf2(dt·escala, actor, ranura, &comp+0x168+r·0x3E0, &comp+0x928+r·0x1A0)
                        = RemoteCatchFunctions::vf2 0x117F7E0 (vtable 0x56FFB48)
                          r12b = byte [[[CC+0x88]+0x10]+0xF6]   ; propiedad del nodo de la acción actual (capa 0)
                          activo = (r12b == 1)
                          tree = 0x491AC0(0x5B1600(comp, ranura))
                          si hay tree y activo: 0x1190FD0(this, dt, 1, actor, estado)
                          si no: 0x1180EA0 (puede soltar, 0x5A7A10) / 0x117FDA0; luego 0x1190FD0(this, dt, activo, actor, estado)
  0x86A576  mov rcx,[rsi+0xC8]  ; aquí llega todo actor, también la copia
  0x86A57D  0x8A0FC0(mc)        ; registra el tick por ranura 0x59D470 (por eso la copia sí llega a la fase 3)
```

`escala = 0x4FFFC0(actor)`: la escala de tiempo global (`[[0x6D691B8]+0x10]+0xCE4`) si el actor es
de tipo 1 y `[CC+0x1E9] != 0`; si no, **(hipótesis)** 1.

**`0x1190FD0(rcx = this, xmm1 = dt, r8b = activo, r9 = actor, [rsp+0x28] = estado)`**, con
`estado = comp + 0x168 + ranura·0x3E0`. Para la ranura 0:

| Campo del estado | Absoluto (ranura 0) | Contenido |
|---|---|---|
| `+0x370` f32 | `comp+0x4D8` | temporizador de la tecla de eje (0,1 s) |
| `+0x374` f32 | `comp+0x4DC` | signo del eje (±1) |
| **`+0x378` f32** | **`comp+0x4E0`** | **fuerza de la flexión** |
| `+0x37C` f32 | `comp+0x4E4` | velocidad de la flexión |
| **`+0x380` vec3** | **`comp+0x4E8`** | **dirección** (`y = 0`) |
| **`+0x38C` u8** | **`comp+0x4F4`** | **flexión en curso** (1 desde el primer fotograma activo; 0 en cuanto deja de estar activo) |
| `+0x38D` u8 | `comp+0x4F5` | agarre hecho (`0x5B1800` una vez por tirón) |

Algoritmo:
1. Si `activo == 0`: `+0x38C = 0`, `+0x370 = +0x374 = 0` y sale **sin llamar a `SetBend`** (el
   árbol vuelve solo).
2. `nodo = 0x5B1600(comp, 0)`: entrada de la ranura 0 (`0x18082A0`) → id de escena →
   `0x1851010` sobre `[[[0x6D69190]+0xC8]]+0x80` → `obj` → `nodo = [obj+0x58] − 0x28`. Si el id de
   escena es 0 (punto en coordenadas de mundo), devuelve 0. Después, `tree = 0x491AC0(nodo)`.
3. Que se pueda doblar: `p = [nodo+0x108]->vf(0x328)()`, con `p != 0` y `byte [p+0x18] != 0`.
4. Punto del gancho: `0x5A4760(comp, 0, &out)`. Salida de 0x28 B, con la posición de mundo en
   `+0x1C/+0x20/+0x24`. Con `h = out.y − base.y` (base = `0x26D3840(nodo)+0x20`) y `H = f32 nodo+0xB0`:
   - si `h < H·[0x6C99738]` sale (gancho demasiado bajo);
   - `ratio = clamp((h − H·k) / (H − H·k), 0, 1)`;
   - grosor: si `√(nodo.AC² + nodo.B4²) · (1 − h/H) > [0x6CB8828]` sale (tronco demasiado grueso a
     esa altura);
   - **tope** = `clamp(powf(ratio, 0.3) · 1.5, 0, 1)`.
5. `dir = (actor.x − árbol.x, 0, actor.z − árbol.z)` normalizada: **el árbol se dobla hacia quien
   tira**. El actor sale de `0x4F8220([[actor+0x68]+0x1A0])`: `x = f32 tc+0xB4 + i16 tc+0xC0·1000`,
   `y = tc+0xB8`, `z = tc+0xBC + i16 tc+0xC2·1000`. El árbol, de `0x26D3840(nodo)`:
   `x = +0x1C + i16 +0x28·1000`, `z = +0x24 + i16 +0x2A·1000`.
6. Una vez por tirón (`+0x38D == 0`): `0x5B1800(comp)` (agarre: `0x349A5D0`, `tree+0x278 = 1`,
   consulta de física `vf 0x268`).
7. Primer fotograma (`+0x38C == 0`): `fuerza = tree+0x2A4`, `dir0 = tree+0x2A8` (o `dir` si la
   fuerza es 0), y `+0x38C = 1`.
8. **Entrada:** `e = (i8) [[actor+0x68]+0x1B0]+0xB9` **(componente sin identificar)**. Si `e == 0`:
   - eje `"Key_RemoteCatchTreePulling"` (`0x56FF8E8`, sistema de entrada global `[0x6D69180]+0x20`,
     `vf 0x48`): si es distinto de 0, `+0x370 = 0,1` y `+0x374 = −signo(eje)`; mientras
     `+0x370 > 0`, `e = +0x374`.
   - si sigue en 0 y está pulsado `"RemoteCatch_ControlPosition_Pull"` (`0x56FF858`, `vf 0x40`):
     velocidad objetivo `0,15` y `tope = max(fuerza, 0,2·tope)`.
   - si no: velocidad objetivo `e · 0,15`.
9. Dinámica: `|Δvel| ≤ (0,05·f32 [[actor+0x68]+0x1B0]+0xC8 + 0,5)·dt`, `fuerza += vel·dt`, acotada
   a `[−1, tope]`. Si la fuerza es negativa, va hacia 0,05 con `1 − e^(−5dt)`. `dir` se interpola
   hacia el `dir` del paso 5 (`0x1373040`, factor `dt·[0x6C99788]`).
10. **`SetBend(tree, &estado+0x380, estado+0x378)`**.

Constantes de `.xdata`: 0,15 · 0,2 · −1 · 0,05 · 0,5 · −5 · 0,3 · 1,5. Las de `.data` (`0x6C99738`,
`0x6CB8828`, `0x6C99788`, `0x6D26288`, `0x6D26238`, `0x6CB8788`, `0x6CB87D8`) se rellenan al
arrancar y **solo se pueden leer en vivo**.

**Consecuencia:** en el emisor, la flexión sube unos 0,15/s mientras el jugador tira con la
entrada (unos 6–7 s hasta el tope si el tope es 1), se queda quieta si no hay entrada, y vuelve
sola en cuanto la acción deja de tener `+0xF6 == 1`.

### 1.3 Fuente B: `0x8A30C0` (`ClientFrameEventTreePulling`)

- `ClientFrameEventTreePulling::vf1` `0x6D46D0` escribe `f32 [[[actor+0x68]+0x40]+0xC8]+0x63C = 0`
  (`mc+0x63C`; −1 = inactivo).
- `0x8A30C0(mc, xmm1)`: si `0 ≤ mc+0x63C ≤ [0x6CB8788]`, `mc+0x63C += xmm1`. El nodo sale de
  `[[mc+0x188]+0x50]` (sin identificar). La fuerza es
  `(1 − clamp(radio·(1 − (actor.y − base.y)/H) / [0x6CB8828])) · [0x6CB87D8] · xmm1`. La dirección es
  `−[[[0x6D691B8]]+0x50]+0xD4` (**(hipótesis)** la de la cámara) con `y = 0`. Escribe en el
  `TreeComponent` lo mismo que `SetBend` (copia en línea) y `mc+0x638`.
- Solo la llama `0x87A700` (`0x87B2C6`). Hace falta el modo de control 1 y `byte mc+0x5E9 == 2`
  (estado normal; 1 = vuelo del lanzamiento `0x5B1F10`).
- **(Hipótesis)** sirve para la sacudida de la copa cuando el actor está arriba (lanzamiento). No
  hace falta para la flexión del tirón.

---

## 2. Por qué la copia no dobla el árbol

| Puerta | Dónde | Jugador local | Copia (`0xA010F001`) |
|---|---|---|---|
| `actor->vf(0x178)` = "personaje del jugador local" | `0x86A557` (`0x4FE260`: `[a+0xA0] == [[[0x6D69190]+0x30]+0x58]` y `byte [a+0x94]`; `0x8A67D0` para `ClientUserActor`) | sí → `0x5956A0` | **no** → nunca corre `0x5956A0`, ni `vf2`, ni `0x1190FD0` |
| Modo de control (`0x88E8E0`) | `0x890A82`: 1 → `0x87A700`; 2 → `0x87B760`; 3 → `0x879C50` | 1 → `0x8A30C0` | **2** → nunca corre `0x8A30C0` |
| Frame events "solo locales" | `0x4F42B0`: `byte [datos+6] & 4` → tipo 1 y tipos 3–6, 8 y 9 exigen `byte [CC+0x1E9] != 0`; tipos 2 y 7, nunca. `& 0x10`: no si `Status+0x390 | +0x394 ≥ 0x80` | corren | **(hipótesis)** no corren: los de la cuerda (`RemoteCatch`, `TreePulling`, `ReleaseRemoteCatch`) llevan el bit 4. Es coherente con la suelta (`arbol-cuerda-2.md` §2) y con la ausencia de fase 4 |
| Entrada del tirón | `0x1190FD0`: teclas globales | la del jugador | sería **la del jugador del receptor**: inútil y engañosa |

Lo que sí corre en la copia es el tick por ranura (`0x8A0FC0` → `0x59D470`). Por eso llega a la
fase 3 con la pieza y la cuerda correctas (sonda del 7 oct). La fase 3 por fotograma solo coloca
la cuerda (`0x5A40E0`, `0x598F80`, `0x5AEAA0`); no dobla nada.

**No conviene** forzar ninguna de las puertas:
- `vf 0x178` y `[CC+0x1E9]` significan "personaje controlado por este PC". Cambiarlos activa en la
  copia todos los frame events locales (golpes con `DoFrameEventReq`, proyectiles, sueltas,
  cámara…) y otras rutas.
- Ejecutar `0x5956A0` en la copia añadiría la entrada del receptor y sus efectos (`comp+0xCC0`,
  `comp+0xCA0`, `vf1`/`vf5`).

---

## 3. `vf50`, `vf51` y `vf52`, decodificadas (para descartarlas)

Vtable de `ClientRemoteCatchActorComponent` `0x55890D8`: vf42 `0x594AD0` (inicio), vf43 `0x5A7A10`
(suelta), vf45 `0x5A40E0`, vf47 `0x596BF0`, **vf50 `0x5A0770`**, **vf51 `0x5A68D0`**,
**vf52 `0x5970C0`**.

### 3.1 `vf50` `0x5A0770` — pasa a la fase 4 ("recoger la cuerda")

- Firma: `(comp, i8 ranura, f32 duración, f32 velocidad, u8 soltar_antes, u8 a6)`. La llama solo
  `0x593B50` (`0x593C84`), cuando el frame event `ClientFrameEventRemoteCatch` (`0x7F88B0`, datos
  `[fe+8]`) tiene `+0x57 != 0`. Los argumentos salen de los datos: `+0x54` ranura, `+0x10` duración,
  `+0x14` velocidad, `+0x5A` soltar_antes y `+0x58` a6.
- Solo funciona en las fases 2, 3, 5 y 6 (tabla `0x5A0C40`) y con los objetos de la cuerda
  `+0x928/+0x930/+0x938` vivos.
- Si `soltar_antes`, llama primero a `0x5A7BC0(comp, r, 2, 0, 1, 0)`.
- Duración: `duración` si es > 0; si no, `distancia(punta, mano 0x59DD60) / velocidad`.
- Escribe:
  - `+0x9C0` = fase anterior y `+0x9C4 = 0` (progreso);
  - `+0x9C8` = duración;
  - `+0x9CC` = transformación de la punta;
  - `+0x9F4 = 1`, `+0x9F5 = !soltar_antes`, `+0x9F6 = a6`;
  - efectos `+0x980`/`+0x998` (`0x5A9D30`);
  - **`+0x9BC = 4`**.
- Después, `0x593B50` envía un frame event de servidor de tipo 6 (`0x4EC430`), que sale como
  `DoFrameEventReq` de variante 6.
- Por fotograma, `0x59F760` (fase 4):
  - `progreso += dt / duración`;
  - si la fase anterior era 3 y `+0x9F5`, el objetivo sigue enganchado (`0x5A40E0`, `0x59F5F0`);
  - la punta se interpola desde `+0x9CC` hasta la mano (`0x59DD60`) y se coloca con `0x598F80`;
  - al llegar a 1, `0x2D2C900([CC+0x130], 0x25, cfg+0x94)` (evento de efecto) o fase 7.
- **Es recoger la cuerda o atraer el objetivo hacia la mano.** En los árboles no se usa: en los
  enganches de `050207` (y en los 12 de `arbol-cuerda-2.md`) no hay ningún `DoFrameEventReq` de
  variante 6 aparte del del lanzamiento (fe 20). Llamarla en la copia despegaría la punta del
  tronco.

### 3.2 `vf51` `0x5A68D0` — `ClientFrameEventRemoteCatchPull`

- La llama `0x807C80` con `rdx = [datos_fe+8]`. Si devuelve verdadero, se envía el frame event de
  servidor de tipo 7.
- Usa la ranura actual `i8 comp+0xD0` (`0xFF` = ninguna) y exige un **actor objetivo**
  (`0x1808330` → `ScopeAttacher<ClientActor>`). Con un árbol (`+0x2C = 0`) devuelve falso en
  `0x5A69F2`.
- Lee `f32 datos+0x1C` y `u8 datos+0x25` (modos 0/1/2); el resto es la cinemática de atraer al
  actor. **No sirve para árboles.**

### 3.3 `vf52` `0x5970C0`

En la fase 3, enciende o apaga la visibilidad de los efectos `+0x978`/`+0x980` según
`byte [[[CC+0x88]+0x10]+0xF6] == 5`. Si el objetivo es un gimmick de tipo 7, también hace
`física->vf(0x3D8)`. No es una fuerza.

---

## 4. Red: qué llega y qué envía el emisor

### 4.1 Nada S→C lleva el tirón

- Opcodes de la cuerda (`troc_messages_2976.csv`): 2763 (inicio), 2471 (suelta), 2474
  (transmutación), 3050 (suelta forzada), 2156/2512 (agarre normal), 2356 `GimmickCatcherUpdateAck`.
  Ninguno tiene que ver con tirar o doblar, y ninguno llega a `SetBend`/`TreeComponent` (los únicos
  que escriben `+0x2A4` son `SetBend`, `0x8A30C0` y la propia actualización).
- `rope3/scene_refs.py` sobre `050207`: en los 21 s que siguen a cada enganche, el id de escena del
  árbol (`82fc9066de0000090ffe0001…`) **solo** aparece en el `DoFrameEventReq` C→S del lanzamiento.
  Los S→C de esa ventana son los habituales del mundo (`UpdateElementalAck`, `SyncSceneObjectAck`…).

### 4.2 Lo que envía el emisor mientras tira (`rope3/tl_050207.txt`)

Enganche 1 (anfitrión, t en s de la captura):

| t | Acción principal (`StartActionReq +8`) | Herramienta (GEFE/UGR `+4 | +8`) |
|---|---|---|
| 223,56 | `9a368460` (lanzar; DFE fe 14–20) | — |
| 223,73–224,47 | `2821f20a`, `a747bf2f`, `7cc69d61` (enganchado, quieto) | `da667cc9`/`a55a59ff | 85fb13c7`, `92786a19 | 6ba22087` |
| **227,13** | **`d9c333c4`** → `76d8c2d8` → `6b4ee3ca` (empieza a tirar; capa superior `26107d3c`) | `003ecc59 | 080bfa8f`, `92786a19 | 080bfa8f`, **`003ecc59 | 76b28076`** |
| 228,07–237,07 | **`563271fe` cada ~1,5 s** (sujeta tirando) | — |
| **237,44** | `563271fe → 7cc69d61` (deja de tirar) | **`92786a19 | 76b28076`** |
| 238,19 | `d20295ad` (suelta) | `550d1ed6 | 85fb13c7` |

- Durante el tirón **no hay `DoFrameEventReq`** ni `IsRemoteCatchEffectStartStateReq` (este último
  solo sale al enganchar: `00 01` a +0,02 s y `00 00` a +0,47 s).
- La copia de Shadow reproduce la misma cadena con los `StartActionAck` inyectados
  (`rope3/copy_actions_040239.txt`: `d9c333c4` en 183,58, `563271fe` ×7 y `→ 7cc69d61` en 193,90).
- Conjuntos (**(hipótesis)**: corresponden a `+0xF6 == 1`):
  - **tirando**: `P = {d9c333c4, 76d8c2d8, 6b4ee3ca, 563271fe}`;
  - enganchado sin tirar: `{9a368460, 2821f20a, a747bf2f, 7cc69d61}`.
- La herramienta (`003ecc59/92786a19 | 76b28076` = empieza/acaba el tirón) es un `UpdateGimmickReq`
  que el servidor no reenvía, y el componente de la cuerda no la lee.
- **La cantidad de flexión no viaja**: depende de la entrada del emisor (§1.2 paso 8).

### 4.3 ¿Hay que sincronizar la flexión?

**No para el juego:**
- es cosmética y local de cada PC;
- el lanzamiento del emisor (`0x5B1F10`) usa su propio árbol y la trayectoria llega a la copia por
  `MoveActor`;
- la suelta inyectada ya reinicia el árbol del receptor (`0x5A7BC0` → `0x349A780`,
  `tree+0x278 = 0`).

**Sí para que se vea igual**: la amplitud depende de cuánto tiró el emisor. Basta con aproximarla
(R1) o enviarla (R3).

---

## 5. Recomendación (ordenada por confianza)

### R0 — Sonda de solo lectura antes de nada (confianza alta, riesgo nulo)

Ampliar la sonda actual para leer, cada 250 ms durante un enganche, **en la copia y en el jugador
local** (todo con `VirtualQuery`/SEH):
- `comp = [[a+0x68]+0x78]`: `byte +0x20`, `+0x9BC` (la fase; se espera 3 también al tirar, nunca 4),
  `f32 +0x4E0`, `vec3 +0x4E8`, `byte +0x4F4`, `byte +0x4F5`;
- `node = [[[[a+0x68]+0x40]+0x88]+0x10]`: `byte node+0xF6` (se espera 1 en P y otro valor en
  `7cc69d61`);
- `[[a+0x68]+0x1B0]` (no nulo), `i8 +0xB9`, `f32 +0xC8`;
- las variables `f32` de `.data`: `0x6C99738`, `0x6CB8828`, `0x6C99788`, `0x6D26288`, `0x6D26238`,
  `0x6CB8788`, `0x6CB87D8`.

Con eso se confirma `+0xF6`, el tope y la escala, y que el jugador local dobla con
`+0x4F4 == 1` y `+0x4E0 > 0`.

### R1 — `SetBend` en la copia desde su propia actualización (confianza media-alta, solo visual)

**Dónde:** `safetyhook::create_mid` en **`exe+0x86A576`**.
- Bytes esperados: `48 8B 8E C8 00 00 00` (`mov rcx,[rsi+0xC8]`), seguidos de `E8 3E 6A 03 00`.
  Es el destino del `je` de `0x86A55F` y la instrucción entera tiene 7 B, así que el hook no corta
  nada.
- En ese punto, `rsi = CC`, `[rsi+8] = actor` y `xmm6 = dt`.
- Es la función (y el hilo de tareas de actualización de actores) desde donde el juego llama a
  `SetBend` para el jugador local (`0x86A571`).
- Se llama para **cada actor en cada fotograma**, así que lo primero es una comparación barata.

**Lógica** (`ctx.rsi`, `ctx.xmm6`):

```
copia = g_copy_actor (atómico; lo refresca puppet_tick con game::find_actor(kPuppetPlayer) cada ~250 ms)
si [rsi+8] != copia o !g_copy_hooked: salir
comp = [[copia+0x68]+0x78]
si byte comp+0x20 != 0 (no es árbol) o byte comp+0x9BC != 3: tirando = false; salir
tirando = (byte [[[rsi+0x88]+0x10]+0xF6] == 1)       ; si R0 lo confirma; si no, acción de la capa 0 ∈ P
si !tirando: t_tiron = 0; salir                       ; sin SetBend el árbol vuelve solo, como en el emisor
nodo = 0x5B1600(comp, 0)                             ; rcx comp, dl 0 -> nodo o 0
si !nodo: salir
tree = 0x491AC0(nodo)                                ; rcx nodo -> TreeComponent* o 0
si !tree: salir
(opcional, como el juego) p = [nodo+0x108]->vf(0x328)(); si !p o byte p+0x18 == 0: salir
out[0x28] = {}; si !0x5A4760(comp, 0, out): salir    ; punto del gancho: x = f32 out+0x1C, z = f32 out+0x24
dir = (g_told_pos.x − out.x, 0, g_told_pos.z − out.z)  ; posición de la copia (ya la conoce el mod)
si |dir| < 0,01: salir
t_tiron += dt;  fuerza = min(tope, 0,15·t_tiron)       ; tope: fórmula del §1.2 paso 4, o 0,8 fijo
0x349EA10(tree, dir, fuerza)                          ; void(__fastcall*)(void*, const float*, float): fuerza en xmm2
```

Todas las llamadas al juego van dentro de `__try/__except`. Ante la primera excepción, se apaga
para toda la sesión, como `rope_piece_of`.

- **Opcional, `0x5B1800(comp)` una vez al empezar el tirón** (agarre: `tree+0x278 = 1`, objeto
  `+0x220`, consulta de física). El juego lo hace, y la suelta inyectada ya lo deshace
  (`0x349A780`). **(Hipótesis)** cambia el dibujo o la física del árbol agarrado. Hace más trabajo
  (memoria y física), así que conviene dejarlo para una segunda prueba.

**Riesgos:**
- **Hilos.** Las tareas de actualización de actores corren en paralelo (`caballo-coop.md` §9). Si
  el jugador local tira del mismo árbol a la vez, dos hilos llamarían a `SetBend` (y quizá a
  `vf 0x4A8`, la primera vez) sobre el mismo `TreeComponent`. Para evitarlo, no doblar con la copia
  si la ranura 0 del jugador local tiene el mismo id de escena y `+0x4F4 == 1`. `vf 0x4A8` de
  `SceneObjectClient` (`0x3611F70`) está ofuscada y no se ha visto si toma un cerrojo.
- `0x5B1600` toma el cerrojo del gestor de objetos de escena (`0x1851010`). El juego la llama desde
  el mismo sitio, y el mod ya la usa a través de `rope_piece_of`.
- Una excepción dentro de una función que tiene un cerrojo puede dejarlo tomado (bloqueo). Por eso
  se apaga a la primera.
- La amplitud es aproximada: no se conoce la entrada del emisor.

### R2 — Ajustar R1 con el comienzo y el final exactos del emisor (confianza media)

- Inicio del tirón: la primera acción de P.
- Fin: la acción `7cc69d61` tras `563271fe`, o cualquier acción fuera de P, o la suelta.

Con `+0xF6` (R0) no hace falta ninguna lista: la copia ejecuta las mismas acciones a su ritmo
(retardo de reproducción incluido), así que la flexión empieza y acaba sincronizada con su
animación.

### R3 — El emisor envía su flexión real (confianza media-alta, fiel; cambia el protocolo)

- **Emisor:** el mismo mid-hook en `0x86A576`, con `[rsi+8] ==` el personaje local (después de que
  `0x5956A0` actualice el estado en ese fotograma). Cada 50–100 ms, o cuando la fuerza cambie más
  de 0,01:
  - leer `comp = [[jugador+0x68]+0x78]` y comprobar `byte comp+0x20 == 0` y `comp+0x9BC == 3`;
  - leer `activo = byte comp+0x4F4`, `fuerza = f32 comp+0x4E0` y `dir = f32 comp+0x4E8, +0x4F0`
    (x, z; `y = 0`);
  - enviar el mensaje nuevo `RopeBend {u8 activo, f32 fuerza, f32 dir_x, f32 dir_z}`, y uno final
    con `activo = 0`.
- **Receptor:** R1 sin la rampa, con la fuerza y la dirección del último `RopeBend` (interpoladas)
  mientras `activo == 1` y hayan llegado hace menos de 500 ms. La dirección es de mundo y vale
  igual en los dos PC.
- Hay que serializarlo campo a campo en `protocol.cpp` y **subir `kVersion` de 9 a 10**.
- Como la fuerza también se reproduce con el retardo de la copia, conviene ponerle la marca de
  tiempo del emisor y aplicarla con el mismo retardo que los `StartActionAck`.

### Descartado

| Opción | Por qué no |
|---|---|
| `vf50` / fase 4 en la copia | Es recoger la cuerda: la punta se iría hacia la mano. El juego no la usa con árboles (§3.1) |
| `vf51` | Exige un actor objetivo (§3.2) |
| Un mensaje S→C del servidor | No existe (§4.1) |
| Ejecutar los frame events de la copia (`[CC+0x1E9] = 1`) | Activa todos los frame events locales y cambia el modo de control y la escala de tiempo. Además no basta: la flexión no es un frame event, sino `0x5956A0` con entrada |
| Forzar `vf 0x178` o saltar la puerta de `0x86A55F` para la copia | Ejecuta `0x5956A0` entero: entrada del receptor, efectos y sueltas por script (`0x1180EA0`) |
| Llamar a `0x1190FD0` en la copia | Lee la entrada **del receptor**; sin entrada, la flexión no crece |

---

## 6. Confianza

| Afirmación | Confianza |
|---|---|
| La flexión es `TreeComponent` (`SetBend` `0x349EA10`, `+0x2A4/+0x2A8/+0x2B4`) y vuelve sola si no se refresca | alta (código) |
| Las dos únicas fuentes son `0x1190FD0` (vía `0x5956A0`/`vf2`) y `0x8A30C0` | alta (únicos escritores de `+0x2A4` fuera del propio componente) |
| La copia no pasa por `0x5956A0` (puerta `vf 0x178`) ni por `0x8A30C0` (modo 2) | alta |
| `0x86A310` corre para la copia | media-alta (`0x8A0FC0`, único registrador del tick que sí corre en la copia, se llama justo después) |
| En los árboles no hay fase 4 (`vf50`) y `vf51` no aplica | alta (capturas: sin DFE de variante 6 al tirar; `vf51` exige actor) |
| `+0xF6 == 1` exactamente en las acciones de P | media (hipótesis; R0 lo decide) |
| La flexión del emisor depende de su entrada | alta (código: teclas globales en `0x1190FD0`) |
| El cuerpo de la pieza no sigue la flexión | baja (hipótesis) |
| Llamar a `SetBend` desde `0x86A576` para la copia es seguro | media (mismo sitio que el juego; queda el riesgo de hilos del §5) |

---

## 7. Pruebas propuestas

| # | Prueba | Decide |
|---|---|---|
| T1 | R0 en un PC: tirar de un árbol con el jugador local y ver la sonda | `+0xF6`, fase 3 durante el tirón, `+0x4E0` que crece y el tope |
| T2 | R0 en dos PC (Shadow como receptor) | que `+0xF6` de la copia sigue a P y que la copia tiene `tree` (`0x5B1600`/`0x491AC0`) |
| T3 | R1 primero con `cdmp_peer` (un PC) y luego en dos PC | que el árbol se dobla hacia la copia y vuelve al soltar o al dejar de tirar |
| T4 | R1 con el jugador local tirando de **otro** árbol a la vez | sin cuelgues por concurrencia |
| T5 | R3 | que la amplitud coincide con la del emisor |

---

## 8. Archivos nuevos (`build/research/interactions/rope3/`)

| Archivo | Contenido |
|---|---|
| `c_349ea10_tree_setbend.asm` | `TreeComponent::SetBend` |
| `c_349d6d8_tree_update.asm` | actualización y vuelta del `TreeComponent` |
| `c_349ecd0_tree_apply.asm` | paso de los parámetros al dibujo |
| `c_349a780_tree_release.asm` | reinicio del árbol (suelta y lanzamiento) |
| `c_5b1600_slot_target_node.asm` | nodo de escena del objetivo de la ranura |
| `c_5b1f10_tree_launch.asm` | lanzamiento desde el árbol (`mc+0x5E9 = 1`) |
| `c_5956a0.asm` | actualización local del componente (despacha `vf1`/`vf2`/`vf5` del script) |
| `c_11930a0_script_tick.asm` | `RemoteCatchFunctions::vf16` (parámetros de la cuerda por fotograma) |
| `c_86a310_cc_update.asm` | actualización del CC: puerta `vf 0x178` y punto del hook `0x86A576` |
| `c_88f4e0_mc_tick.asm`, `c_87a700_mc_update.asm`, `c_886f30_mode.asm` | movement controller: modo 1/2/3 y `mc+0x5E9` |
| `c_8a30c0_treepull_timer.asm`, `c_8a31bf_treepull_tail.asm`, `c_6d46d0_fe_treepulling.asm` | fuente B |
| `c_4f41c0_cfep.asm`, `c_4f42b0_cfep.asm` | `ClientFrameEventProcessor` vf3/vf4 (puerta `+6 & 4` y `CC+0x1E9`) |
| `c_59f5f0_phase3_update.asm`, `c_598f80.asm` | por fotograma, fase 3 (solo actores y colocación de la punta) |
| `c_58bce0_vf33.asm`, `c_8e5fd90_comp_ctor.asm`, `c_3611f70_sceneobj_register.asm`, `c_460be0_cc1e9.asm`, `c_1f63f70_layer_advance.asm`, `c_a6e030_unrelated.asm` | contexto (los dos últimos, ofuscado o sin relación) |
| `lin.py` | desensamblado lineal (no se para en los trozos de `.pdata`) |
| `vcall_scan.py` | llamadas virtuales `call [reg+disp]` cerca de una carga de `+0x78` |
| `physcalls.py` | llamadas al mundo físico `[[0x6D69190]+0xE8]` |
| `glob_use.py` | usos de `[[global]]+0x50]+campo` |
| `scene_refs.py`, `scene_refs_050207.txt` | paquetes con el id de escena del árbol durante cada enganche |
| `copy_actions.py`, `copy_actions_040239.txt` | acciones inyectadas a la copia en Shadow |
| `tl_050207.txt` | línea de tiempo del emisor (`rope2/rope_tl.py`) |

`rope2/c_1190fd0.asm` (motor de la flexión) y `rope2/c_117f7e0.asm` (`vf2`) ya estaban volcados.

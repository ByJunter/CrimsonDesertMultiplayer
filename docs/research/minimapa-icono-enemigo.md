# El icono de enemigo de las copias en el minimapa: cuadrado rojo (jefe) contra punto rojo

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`) y solo valen para esta build. Investigación de solo lectura (7 oct 2026): desensamblado
con capstone del exe en disco, datos ya extraídos de los PAZ (`build/research/arrow/out/`) y el log
del invitado (`cdmp.log`, 7 oct, tarde). No se ejecutó el
juego ni se tocó `src/`, `kit/` ni `scripts/`.

Marcas:
- **[E]** verificado estáticamente (desensamblado o datos del juego);
- **[L]** visto en el log;
- **[I]** inferido o hipótesis; hay que confirmarlo en vivo.

Scripts de trabajo (no forman parte del repo):
`strrefs.py` (referencias `lea` a cadenas), `iconenum.py` (vuelca el enum de `0x15CE070`) y los
desensamblados `d8d10.txt`, `d89c0.txt`, `apply.txt`, `cfgfill.txt`, `rel.txt`. Se reutilizan
`cddis.py` y `cdpe.py` de `build/research/` y la tabla `build/research/arrow/uimt_resolved.txt`
(de `flecha-color.md`).

---

## 0. Resumen

1. **El icono 22 que damos hoy es el de JEFE.** `cfg+0x112` = `Actor_Enemy_Boss` = clave 22 =
   componente `MapIcon_ActorEnemyBoss`, textura `cd_icon_map_boss` teñida de rojo. Ese es el
   cuadrado o rombo rojo de la foto **[E]**.
2. **El punto rojo es `cfg+0x10A` = `Actor_Enemy` = clave 27** (`MapIcon_ActorEnemy`, textura
   `cd_icon_map_enemy` de 16 px, rojo `#da1f1f`) **[E]**. Es el que más reciben los NPC del mundo en
   el log del invitado (`27x6302`) **[L]**.
3. **He descifrado la tabla `cfg` entera**: `cfg + 0xA0 + 2·i` guarda la `UIMapIconKey` del valor
   `i` de un enum cuyos nombres registra `0x15CE070`. Cuadra con todos los valores ya conocidos:
   `+0x110` = 32, `+0x112` = 22, `+0x114` = 28, `+0x124` = 23 y `+0x126` = 0 **[E]+[L]** (§1).
4. **El juego no saca el punto rojo de `0x9E8D10`, sino del post-proceso de `0x9E89C0`.** Cuando la
   relación del NPC con el jugador local es hostil (`0x2215750` = 0), el post-proceso pone un
   subicono de la "familia enemigo": `+0x10A` normal, `+0x10C` élite y `+0x10E` aggro. Si el icono
   principal quedó vacío, lo promociona a principal: `+0x10C` si es élite y `+0x10A` en otro caso.
   `+0x10E` nunca pasa a principal **[E]** (§2.3).
5. **El 27 no es "en combate"**: es el enemigo hostil normal. El de combate o aggro es el 30
   (`Actor_Enemy_Aggro`), y solo va como **subicono**, cuando el NPC tiene al jugador local en su
   lista de odio (`0x18004D0`) **[E]**. El 24 del histograma es `Actor_Enemy_Elite` **[E]**.
6. **Arreglo inmediato, sin compilar**: en el `cdmp.ini` del **invitado**, sección `[coop]`, poner
   `enemy_minimap_icon=0x10A` y reiniciar el juego. La validación actual
   (`0xEA ≤ offset ≤ 0x16C`, par) lo acepta. Después se cambia el valor por defecto en `config.h`
   (`0x112` → `0x10A`) (§6).
7. Mejora opcional: imitar al juego también para élites y jefes leyendo `CharacterInfo` de la
   copia. Élite: `+0x217 == 4` → `+0x10C`. Jefe: `+0x1E5 ∈ {1, 2}` → `+0x112` (§6.3).

---

## 1. La tabla `cfg` de iconos, descifrada

### 1.1 De dónde sale [E]

- `cfg = [[[[exe+0x6D69528]]+8]+0x270]`, o `exe+0x6D69530` con el flag TLS `+0x1EC`. Ya está en
  `mapa-minimapa.md` §2.2 y en `offsets.h` (`kMinimapIconRoot`/`Alt`).
- `0x15CE070` es un inicializador estático que llama 104 veces a
  `0x44D8B0(&enum@0x6A6AE00, índice, nombre)`. Registra los nombres de un enum de 0x6A valores.
  Las cadenas están en `0x57B3…`/`0x57B4…`, por ejemplo `"Actor_Enemy"` en `0x57B43D8` y
  `"Actor_Enemy_Boss"` en `0x57B4378`.
- El valor del enum `i` vive en **`cfg + 0xA0 + 2·i`**. Cada entrada es la `UIMapIconKey` (índice de
  `gamedata/uimaptextureinfo`) **cuyo nombre coincide con el del enum**.

Comprobación con todo lo ya conocido:

| `cfg+` | `i` | Nombre del enum | Clave esperada (`uimt_resolved.txt`) | Visto |
|---|---|---|---|---|
| `0xEA` | 37 | `Pin_Marker` | 1 | Es la clave del pin del grupo 0 (`mapa-minimapa.md` §1.2) **[E]** |
| `0x110` | 56 | `Actor_Npc_Witness` | 32 | `table +0x110 32` **[L]** |
| `0x112` | 57 | `Actor_Enemy_Boss` | 22 | `table +0x112 22` **[L]** |
| `0x114` | 58 | `Actor_Enemy_Police` | 28 | `table +0x114 28` **[L]** |
| `0x124` | 66 | `Actor_Focus_InBush` | 23 | `other player's icon = table +0x124 (key 23)` **[L]** |
| `0x126` | 67 | `Actor_Focus` | 0 | La flecha propia es la clave 0 (`flecha-color.md` §1.1) **[E]** |
| `0x16C` | 102 | `Pin_Marker_Trace` | 2 | Es la clave del pin "trace" del grupo 1 **[E]** |

Siete de siete. La regla vale.

### 1.2 La tabla útil para actores (claves por nombre; las de 0x10A–0x10E aún no se han leído en vivo)

| `cfg+` | Nombre | Clave | Uso en el código |
|---|---|---|---|
| `0xFA` | `Actor_DeadBody_Default` | 33 | — |
| `0xFC` | `Actor_DeadBody_Lootable` | 34 | Cadáver con botín (`0x9E96E0`) |
| `0xFE` | `Actor_DeadBody_Lootable_Sub` | 35 | Cadáver, rama sin botín especial (`0x9E96C0`) |
| `0x100` | `Actor_Character` | 102 | Mercenario o personaje del clan local |
| `0x104` | `Actor_Party_Default` | 104 | `0x9E9469` |
| `0x106` | `Actor_Player_Other` | 105 | — |
| `0x108` | `Actor_Neutral` | 107 | Subicono si la relación es 1 (neutral) |
| **`0x10A`** | **`Actor_Enemy`** | **27** | **Enemigo hostil: el punto rojo** |
| `0x10C` | `Actor_Enemy_Elite` | 24 | Hostil élite (`CharacterInfo+0x217 == 4`) |
| `0x10E` | `Actor_Enemy_Aggro` | 30 | Hostil con el jugador en su lista de odio (solo subicono) |
| `0x110` | `Actor_Npc_Witness` | 32 | NPC testigo (rama `+0x120`/`+0x128`) |
| `0x112` | `Actor_Enemy_Boss` | 22 | Jefe (`CharacterInfo+0x1E5 ∈ {1,2}`) |
| `0x114` | `Actor_Enemy_Police` | 28 | Guardia o policía |
| `0x116` | `Actor_Enemy_Target` | 25 | — |
| `0x118` | `Actor_Enemy_BountyHunter` | 29 | — |
| `0x124` | `Actor_Focus_InBush` | 23 | Hoy, el compañero |
| `0x126` | `Actor_Focus` | 0 | La flecha propia |
| `0x14E` | `Faction_OperationCharacterActor` | 42 | Mercenario (estado 3 de `0x214D130`) |

La tabla completa (`0xEA`–`0x16C`) está en el apéndice A.

**Nota.** Las claves de esta tabla son las del **nombre** en `uimaptextureinfo`, no las leídas en
memoria. Las de `0x110`, `0x112`, `0x114` y `0x124` sí se confirmaron en vivo. Para `0x10A` lo
confirmará la línea de arranque del mod (§6.1: `key 27`).

### 1.3 Qué dibuja cada clave [E]

Plantillas de `ui/minimapicon.thtml` (líneas 1937–1952) y estilos de `ui/minimapicon.css`
(líneas 227–236), extraídos en `build/research/arrow/out/ui/`:

| Clave | Componente | `#BaseIcon` | CSS |
|---|---|---|---|
| 27 | `MapIcon_ActorEnemy` | `icon-image-enemy icon-enemy-order` | `cd_icon_map_enemy`, 16×16 px, `#da1f1f`: **punto rojo pequeño** |
| 30 | `MapIcon_ActorEnemyAggro` | igual que el 27 | igual |
| 24 | `MapIcon_ActorEnemyElite` | `icon-image-enemy-elite` | `cd_icon_map_enemy_elite`, 16×16 px, rojo |
| 22 | `MapIcon_ActorEnemyBoss` | `icon-image-enemy-boss icon-enemy-boss-order` | `cd_icon_map_boss`, rojo, sin tamaño fijo: **el cuadrado o rombo de la foto** |
| 28 | `MapIcon_ActorEnemyPolice` | `icon-image-enemy-police` | `cd_icon_map_police`, gris claro |

- Todos los de la familia enemigo tienen además un `#StickyIcon`
  (`icon-image-enemy-aggro-sticky`). Con la clase `cpp-enemy-aggro-target-icon`, es la flecha roja
  de borde `cd_icon_minimap_stiky_enemy` (104×100 px) **[E]**.
- **(hipótesis)** La UI activa esa flecha de borde para los enemigos con aggro hacia el jugador.

---

## 2. Cómo decide el juego el icono de un NPC (tipo 3)

### 2.1 Firma real de los dos cálculos [E]

`0x9E89C0(comp, u16* icon, u16* sub, u8* a, u8* b)`, el gancho de `puppet_minimap.cpp`, llama a:

```
0x9E8D10(comp, jugadorLocal, actorLocal2, icon, &noIcon /*var -0x30*/, a, b, &rel /*var -0x31, init 5*/)
```

- `jugadorLocal` sale de `0x8B4570`; si el actor es él, `icon = cfg+0x126`.
- `actorLocal2` sale de `0x8B44E0`. Se usa para el clan de mercenarios (`+0x110`), la relación
  `+0x120` y el conocimiento (`+0x150`).
- `noIcon` (local del llamador, `[rbp-0x30]`) se pone a 1:
  - en `0x9E90A3` (`Status+0x340`);
  - en `0x9E9FDB`, varias ramas de "sin icono";
  - en `0x9E9FE7` (oculto).
- `*b = 1` **solo** en el camino de oculto (`0x9E9FE7`), que además lanza `HideFlagMinimapActor`.
  Por eso `keep_game_icon()` hace bien en mirar `*b` **[E]**.
- `*a` (`0x9E9103`) = `Status+0x320 > 0 ? 0x1794ED0(Status) : 0`.
- `rel` = `0x2215750(npc, jugadorLocal, 0)` (`0x9E8D97`): la **relación de facción** entre los dos
  actores (§2.4).

### 2.2 `0x9E8D10` para un NPC vivo de tipo 3 [E]

1. Visibilidad, igual que en `mapa-minimapa.md` §2.2:
   - bit `0x4000000` de los datos de la IA;
   - la facción si `Status+0x4A0 & 1`;
   - si está congelado (`Sequencer+0x3DC`), si `Status+0x390|0x394 ≥ 0x80` o si `Status+0x341`,
     va a "oculto" (`*b = 1`).
2. Si `Status+0x340` → `noIcon = 1` y el icono es `0xFFFF`.
3. Si `*a` y `CharacterInfo+0x217 < [exe+0x6CC1958]` y `rel == 0` → `noIcon = 1` (`0x9E93D4`).
   **(hipótesis)** Un hostil "menor" en algún estado especial, que se queda sin punto.
4. Si `Status+0x273` (muerto) → cadáver: `cfg+0xFC`/`0xFE`.
5. Si `CharacterInfo+0x1E8 == 1` y `[comps+0xB0]+0x2F4 == -1` → `noIcon = 1` (`0x9E9700`).
   `comps+0xB0` es el secuenciador. **(hipótesis)** "Solo con elemento de secuenciador", y a las
   copias se lo quitamos (`strip_spawn_identity`).
6. `switch` de tipo, tabla `0x9EA07C`; el tipo 3 va a `0x9E9754`:
   - **`cfg+0x112` (jefe)** si `rel == 0`, el tipo de orden `[[[comps+0x58]+0x50]+0x40] != 3` y
     `CharacterInfo(npc)+0x1E5 ∈ {1, 2}` (`0x9E97DB`);
   - si no, `cfg+0x110` (testigo) si `[[actorLocal2.comps+0x120]+0x18] == 1` y
     `u32 [[npc.comps+0x128]+0x1C] != 0` (`0x9E9842`);
   - si no, `cfg+0x114` (policía) si el icono propio `CharacterInfo+0x1E6` es la clave de policía y
     `0x9E7F90(comp, jugador, actorLocal2)` lo permite (`0x9E988E`);
   - si no, el **icono propio** `CharacterInfo+0x1E6`, según `0x9EA0B0`, el conocimiento
     (`0x213EFA0`) o `CharacterInfo+0x163` (`0x9E9998`). Así salen el perro (126) y otros;
   - si no, `0xFFFF`.

**Un enemigo hostil normal sale de `0x9E8D10` con `0xFFFF`.** El punto lo pone el llamador.

### 2.3 El post-proceso de `0x9E89C0`, que es donde nace el punto rojo [E]

`0x9E8A70`–`0x9E8C4A`, para los tipos 3 y 4 (en el 4, solo si `rel == 0`):

```
orden = [[[actor+0x68]+0x58]+0x50]+0x40
si rel == 0:
    si orden != 3:
        si 0x18004D0(actor, jugadorLocal):            *sub = cfg+0x10E   // Actor_Enemy_Aggro (30)
        si no, si CharacterInfo+0x217 == 4:           *sub = cfg+0x10C   // Actor_Enemy_Elite (24)
        si no:                                        *sub = cfg+0x10A   // Actor_Enemy (27)
si no, si rel == 1:                                   *sub = cfg+0x108   // Actor_Neutral (107)

si noIcon == 0 y *icon == 0xFFFF y *sub ∈ {cfg+0x10A, +0x10C, +0x10E}:
    *icon = (CharacterInfo+0x217 == 4) ? cfg+0x10C : cfg+0x10A        // ← el punto rojo
```

- `0x18004D0(npc, jugador)` recorre una lista del componente `[npc.comps+0x90]`: `+0x108`, con
  `+0x110` entradas de 12 B. Devuelve 1 si alguna tiene en `+8` la clave del jugador y
  `[+0x118] == -1`. **(hipótesis fuerte)** Es la lista de odio o aggro del NPC: "me tiene como
  objetivo".
- `CharacterInfo` = `0x389570(Status+0x30)` (§6.3). `+0x217` es un grado: compara con un umbral
  global en `0x9E93E0`, y el 4 es **élite**.
- Así, un enemigo normal tiene `icon = 27` y `sub = 27`. Si me tiene en su lista de odio,
  `icon = 27` y `sub = 30`.

### 2.4 La relación `0x2215750` [E] + interpretación [I]

- Construye un descriptor de facción de cada actor: `0x179D000` para el NPC y `0x179D1F0` para el
  otro. Toma:
  - un `u16` del dueño (`0x17EF2E0`, a través de `[comps+0x90]`);
  - `Status+0x342`;
  - la última entrada de `Status+0x348`;
  - `[comps+0x1A0]+0x88`;
  - para los tipos 4, 5 y 6, `[comps+0x118]+0x18`.
- Valores devueltos:

  | Valor | Cuándo |
  |---|---|
  | 4 | Mismo grupo (índice 6 de la tabla de 7 listas) |
  | 2 | `0x2215B60` (aliado) |
  | **0** | **Hostil**: `0x2216650` en cualquier sentido, con las facciones distintas |
  | otro | Lo que dé `0x2216A50` con los estados del componente `[comps+0x90]` (`0x2216CD0`, `0x2216EA0`). Ese componente es el mismo de la lista de odio |

- `0x2216A50` devuelve 1 (neutral) si el **tipo de orden** del otro actor es 3. Lo demás depende de
  bits del componente `+0x90` y de `Status+0x248`.
- **(hipótesis)** El enum es 0 hostil, 1 neutral, 2 aliado, 4 mismo grupo y 5 sin relación (el valor
  inicial del llamador).

---

## 3. El histograma del log del invitado

`icons of this world's NPCs: 5x2 44x849 126x811 37x39 27x6302 24x224` **[L]**. El hook cuenta las
**llamadas** (cada recálculo periódico), no actores distintos.

| Clave | Nombre | Qué es |
|---|---|---|
| 5 | `Actor_Player_Damian` | Un personaje jugable cargado como NPC (2 llamadas) |
| 44 | `Actor_Vehicle_Horse_Hired` | Caballo alquilado o propio |
| 126 | `Actor_Npc_AnimalDog` | Perro |
| 37 | `DropItem` | Objeto en el suelo |
| **27** | **`Actor_Enemy`** | **Enemigo hostil: el punto rojo** |
| 24 | `Actor_Enemy_Elite` | Enemigo élite |

Evolución del 27 en ese log (una línea por minuto) **[L]**:

| Hora | Valor |
|---|---|
| 17:49 | 35 |
| 17:50 | 569 |
| 17:50–17:54 | 569 (plano) |
| 17:55 | 3916 |
| 17:56 | 6302 |

El 24 sigue la misma curva: 19 → 107 → 224.

- Sube cuando hay enemigos nativos de G cargados y visibles. Se queda plano cuando no los hay; por
  ejemplo, mientras la supresión de la fase 5 retira los de la zona.
- No es un icono "de combate": el aggro va al **subicono** 30, que el hook no cuenta.
- Ni 22, ni 28, ni 32 aparecen en el histograma: en esa zona no había jefes, policías ni testigos.

---

## 4. Por qué la copia recibe `0xFFFF`

Hecho **[L]**: `asked 16558 times, given our icon 16558 times (the game's last answer 65535)`.
Todas las consultas de copias acabaron en `0xFFFF`, con `*b == 0` y la copia viva (si no,
`keep_game_icon()` no habría puesto el icono).

Por §2.3, para que el post-proceso no ponga el 27, tiene que darse una de estas condiciones:

| # | Condición | Plausibilidad |
|---|---|---|
| a | `rel != 0`: la copia **no es hostil** al jugador local de G | **Alta (hipótesis principal).** La hostilidad pasa por el componente `[comps+0x90]` y por el dueño (`0x17EF2E0` lee el dueño desde ese mismo componente). La copia la controla la red (usuario `0x9010F001`, modo de control 0) y nunca entra en batalla ni tiene aggro (`copia-objetivo-sin-ataques.md` §7.2) |
| b | Tipo de orden `== 3` | Baja: la sonda midió 2 en las copias (`copias-enemigos-faltan-pose-minimapa.md` §3.2) |
| c | `noIcon == 1` por `0x9E93D4` (`*a`, grado bajo y `rel == 0`) o por `0x9E9700` (`CharacterInfo+0x1E8 == 1` sin elemento de secuenciador) | Media para el segundo caso: a las copias de schedule les quitamos el elemento |

**Diagnóstico barato** (solo lectura, dentro del propio hook): guardar también `*sub` y `*a` tal como
los deja el juego para las copias, y añadirlos a la línea de cada minuto:

- `sub ∈ {27, 24, 30}` → `rel == 0` y bloqueó `noIcon` (caso c);
- `sub == 107` → `rel == 1`: el juego ve la copia **neutral** (caso a);
- `sub == 0xFFFF` → `rel` ∉ {0, 1} o tipo de orden 3.

Para el arreglo da igual cuál sea: contestamos nosotros. Pero sirve para saber si algún día merece
la pena hacer que el juego la vea hostil de verdad. Eso daría también el subicono de aggro y, quizá,
el comportamiento de "enemigo cercano" del minimapa.

---

## 5. Qué hace el juego con un icono de la "familia enemigo" (`0x9EA6F0`) [E]

`0x9EA6F0` (aplicar) carga `cfg+0x10A`, `+0x10C` y `+0x10E` (`0x9EAEC9`–`0x9EAEE2`) y comprueba si
el icono y el subicono, viejo y nuevo, son de esa familia (`0x9EB127`, `0x9EB1FA`, `0x9EB4C1`):

- según eso actualiza dos marcas internas;
- con el flag global `byte [exe+0x6CC18E8]`, al salir de la familia lanza **`RemoveDetectEffect`**
  (evento `+0x278`, `0x9EB596`);
- al entrar en ella, calcula un radio a partir del componente `[actor+0x68]+0x50` (`0x20AEA20`,
  `0x20B00A0`) y lanza **`CreateDetectEffect`** (`+0x270`, `0x9EB75F`). Los nombres salen de la
  tabla de eventos (`build/research/arrow/evtable.asm`, `0x7C9A6A`/`0x7C9B19`).

Consecuencias:
- con 22 (jefe), las copias **no** entran en esa lógica; con 27, **sí**, como un enemigo real;
- es código del juego que lee componentes de la copia, que son los de cualquier NPC. Es un riesgo
  bajo, pero nuevo: probarlo primero por loopback;
- **(hipótesis)** `DetectEffect` es el anillo o pulso de detección que se ve con el modo de
  detección (el flag global). Sin él activo no pasa nada.

---

## 6. Propuesta

### 6.1 Paso 1: solo configuración (sin compilar)

En el `cdmp.ini` del **invitado** (Shadow):

```ini
[coop]
enemy_minimap_icon=0x10A
```

- `config.cpp:156` lo lee con `wcstol(…, 0)`, así que acepta `0x10A`.
- `puppet_minimap_set_enemy_offset()` acepta `0xEA ≤ x ≤ 0x16C` par.
- La config se lee una vez al arrancar (`mod.cpp:716`): hay que **reiniciar el juego**.
- La línea de arranque dirá `key 65535` porque la tabla aún no está cargada (pasó igual con
  `0x112`). En la línea de cada minuto conviene añadir `+0x10A` (§6.2) para ver `27`.

Resultado esperado: las copias vivas se ven con el **punto rojo** de 16 px, igual que un enemigo
normal.

### 6.2 Paso 2: cambios en `puppet_minimap.cpp` y `config.h`

1. `config.h:108`: `coop_enemy_minimap_icon = 0x10A;` y comentario "`Actor_Enemy`, el punto rojo".
   Los kits no fijan este valor, así que cambia para todos.
2. En la línea de diagnóstico de `puppet_minimap_tick()`:
   - mostrar `+0x10A`, `+0x10C` y `+0x112` en vez de `+0x110`/`+0x112`/`+0x114`;
   - añadir el `*sub` y el `*a` que dejó el juego para la última copia, con
     `g_copy_answer = icon | sub << 16` y otro atómico para `a`.

   Así se confirma `27` y se resuelve §4.
3. Mantener lo que ya se hace bien:
   - no tocar si `given != 0xFFFF` (el juego decidió algo, por ejemplo el cadáver);
   - no tocar si `keep_game_icon()`: oculto (`*b`), muerto (`+0x273`) o `+0x340`;
   - `*sub = 0xFFFF`, `*b = 0`.
4. `*a`: hoy se pone a 0. El juego lo calculó para la copia y `0x9EA6F0` lo compara con el bit 0 de
   `comp+0x35`. **(hipótesis)** Es más fiel dejarlo como lo dejó el juego, pero el 0 funciona con el
   compañero. Se puede dejar así.

### 6.3 Paso 3 (opcional): élite y jefe como el juego

Para que una copia de élite tenga su icono (24) y la de un jefe el suyo (22), en lugar de un offset
fijo:

```
status = [[actor+0x68]+0x20]                 // ya se lee en keep_game_icon
key    = u16 [status+0x30]                   // CharacterKey
mgr    = [exe+0x6D69A48]
si key < u32 [mgr+8] y (ci = [[mgr+0x58] + key*8]) != 0:     // la caché de 0x389570
    si u8 [ci+0x1E5] ∈ {1,2}  → cfg+0x112   (jefe)
    si no, si u8 [ci+0x217] == 4 → cfg+0x10C (élite)
    si no                     → cfg+0x10A   (normal)
si no → cfg+0x10A
```

- Todo con SEH, como el resto del archivo. Solo lee la caché y **no** llama a `0x389570`, que
  podría deserializar.
- Si la entrada de caché es nula, se usa el normal.
- `0x389570` devuelve `[[mgr+0x58]+key*8]` cuando no es nulo (`0x3895A1` → `0x389682`) **[E]**.
- Riesgo: los offsets `+0x1E5`/`+0x217` son de la estructura en memoria de esta build. Ya los usa el
  propio `0x9E8D10`/`0x9E89C0`.
- `enemy_minimap_icon` podría tener un valor especial (por ejemplo `1` = "automático") para este
  modo, sin romper el offset fijo.

### 6.4 Paso 4 (más adelante): subicono de aggro y flecha de borde

- En un real, si me tiene en su lista de odio, el juego pone `sub = cfg+0x10E` (30).
  **(hipótesis)** Eso activa la flecha roja de borde del `#StickyIcon`.
- Para una copia, el anfitrión sabe a quién apunta el real (`CurrentTargetChangeReq`; `g_engaged`
  en `enemy_relay`). Si se enviara un bit "apunta al invitado" en `EnemyState`, el hook podría poner
  `*sub = cfg+0x10E` mientras dure.
- Hay que probarlo antes con un real y el hook en diagnóstico, para ver que el subicono 30 es lo que
  enciende la flecha.

### 6.5 Riesgos y lo que no hay que tocar

| Riesgo | Mitigación |
|---|---|
| Muertos: el juego da el icono de cadáver (`+0xFC`/`+0xFE`) | Ya se respeta: `given != 0xFFFF` o `Status+0x273` → `keep_game_icon` |
| Ocultos (`*b = 1`, con `HideFlagMinimapActor` ya lanzado) | Ya se respeta con `*b != 0` |
| `Status+0x340` (sin icono a propósito) | Ya se respeta |
| Nuevo: 27 mete la copia en la lógica de `DetectEffect` (§5) | Es lo mismo que con un enemigo real. Probar primero por loopback contra `cdmp_peer` y luego con Shadow |
| La clave de `+0x10A` cambia en otra build | El offset es estable en esta build. Si cambia la build, cambia todo `offsets.h`. La línea de log muestra la clave |
| Copias de NPC no hostiles (si algún día se copian civiles con `0xB01F…`) | Hoy solo se copian hostiles (`is_zone_enemy_locked`). Si cambia, el hook tendría que mirar la facción o un bit del anfitrión |

---

## 7. Pruebas

| # | Dónde | Qué | Qué mirar |
|---|---|---|---|
| T1 | Loopback (PC del anfitrión + `cdmp_peer`) | `enemy_minimap_icon=0x10A` | Captura `CopyFromScreen` del minimapa (2560×1440, recortar la esquina): punto rojo pequeño sobre cada copia. Sin crash ni avisos |
| T2 | Shadow invitado | Lo mismo, con una pelea | Punto rojo y no rombo. Al morir la copia, el icono cambia o desaparece como en un real |
| T3 | Diagnóstico (§6.2.2) | Una sesión con copias | `sub`/`a` de las copias, para decidir §4 (a, b o c) |
| T4 | Opcional (§6.3) | Una copia de élite (24 en el histograma de G) | Su icono es el de élite |

No hace falta copia de `save` para T1–T3: el hook ya existe y solo cambia el valor que devuelve.
Las reglas del proyecto (CONTRIBUTING.md) la piden para las pruebas que escriben en la memoria del juego; este cambio no escribe
nada nuevo.

---

## 8. Referencias rápidas (build 2976)

| Qué | RVA |
|---|---|
| Calcular icono (gancho) / árbol / aplicar | `0x9E89C0` / `0x9E8D10` / `0x9EA6F0` |
| Post-proceso de la familia enemigo | `0x9E8A70`–`0x9E8C4A` |
| Promoción a `+0x10A`/`+0x10C` | `0x9E8C02`–`0x9E8C4A` |
| Rama del tipo 3 | `0x9E9754` (jefe `0x9E97DB`, testigo `0x9E9842`, policía `0x9E988E`) |
| Relación de facción | `0x2215750` (descriptores `0x179D000`/`0x179D1F0`, combinador `0x2216A50`) |
| "Me tiene en su lista de odio" | `0x18004D0` (`[comps+0x90]+0x108`, entradas de 12 B) |
| `CharacterInfo` por clave | `0x389570` (gestor `[exe+0x6D69A48]`, caché `+0x58`, límite `+8`) |
| Registro de nombres del enum de la tabla | `0x15CE070` (enum en `0x6A6AE00`); `cfg+0xA0+2·i` |
| Eventos de detección | `CreateDetectEffect` `+0x270`, `RemoveDetectEffect` `+0x278` (flag `exe+0x6CC18E8`) |
| Datos | `gamedata/uimaptextureinfo` (2027 entradas, `uimt_resolved.txt`), `ui/minimapicon.thtml`/`.css` |

---

## Apéndice A. `cfg+0xEA…0x16C` completo (clave por nombre)

| `cfg+` | Nombre | Clave |
|---|---|---|
| `0xEA` | `Pin_Marker` | 1 |
| `0xEC` | `Dialog_Icon` | 184 |
| `0xEE` | `Dialog_Icon_Interaction` | 185 |
| `0xF0` | `MinimapAlert_Effect` | 537 |
| `0xF2` | `MinimapAlert_Effect_Middle` | 538 |
| `0xF4` | `MinimapAlert_Effect_Middle_Repeat` | 539 |
| `0xF6` | `MinimapAlert_Effect_Short` | 540 |
| `0xF8` | `Detect_Effect` | 31 |
| `0xFA` | `Actor_DeadBody_Default` | 33 |
| `0xFC` | `Actor_DeadBody_Lootable` | 34 |
| `0xFE` | `Actor_DeadBody_Lootable_Sub` | 35 |
| `0x100` | `Actor_Character` | 102 |
| `0x102` | `Actor_Npc_Default` | 103 |
| `0x104` | `Actor_Party_Default` | 104 |
| `0x106` | `Actor_Player_Other` | 105 |
| `0x108` | `Actor_Neutral` | 107 |
| `0x10A` | `Actor_Enemy` | **27** |
| `0x10C` | `Actor_Enemy_Elite` | 24 |
| `0x10E` | `Actor_Enemy_Aggro` | 30 |
| `0x110` | `Actor_Npc_Witness` | 32 |
| `0x112` | `Actor_Enemy_Boss` | 22 |
| `0x114` | `Actor_Enemy_Police` | 28 |
| `0x116` | `Actor_Enemy_Target` | 25 |
| `0x118` | `Actor_Enemy_BountyHunter` | 29 |
| `0x11A` | `Actor_ChallengeTarget` | 8 |
| `0x11C` | `Actor_StageTarget` | 9 |
| `0x11E` | `Actor_QuestTarget` | 12 |
| `0x120` | `MinimapAlert_Effect_Focus` | 20 |
| `0x122` | `MinimapAlert_Target` | 21 |
| `0x124` | `Actor_Focus_InBush` | 23 |
| `0x126` | `Actor_Focus` | 0 |
| `0x128` | `Debug_Quest_Main` | 17 |
| `0x12A` | `Debug_Quest_Sub` | 18 |
| `0x12C` | `Debug_Sequencer` | 19 |
| `0x12E` | `MercenaryNode` | 580 |
| `0x130` | `MercenaryNode_Point` | 581 |
| `0x132` | `Gimmick_MinimapAlert_Effect` | 535 |
| `0x134` | `FactionNode` | 579 |
| `0x136` | `FactionNode_Point` | 573 |
| `0x138` | `FactionNode_SubInner` | 542 |
| `0x13A` | `FactionNode_SubInnerTree` | 574 |
| `0x13C` | `FactionNode_Operation` | 543 |
| `0x13E` | `FactionNode_Flag` | 575 |
| `0x140` | `FactionNode_OperationWork` | 544 |
| `0x142` | `FactionNode_NodeConnect` | 576 |
| `0x144` | `FactionNode_Contribution` | 541 |
| `0x146` | `FactionNode_Research` | 577 |
| `0x148` | `Faction_MainFlag` | 578 |
| `0x14A` | `Faction_DeliveryCharacterActor` | 108 |
| `0x14C` | `Faction_DeliveryCharacterNoActor` | 109 |
| `0x14E` | `Faction_OperationCharacterActor` | 42 |
| `0x150` | `Faction_OperationCharacterNoActor` | 43 |
| `0x152` | `Mission_Area` | 531 |
| `0x154` | `Mission_Fog` | 529 |
| `0x156` | `Mission_Fog_Abyss` | 530 |
| `0x158` | `Mission_Icon` | 532 |
| `0x15A` | `Mission_Icon_Abyss` | 533 |
| `0x15C` | `Skill_LearnSpot` | 520 |
| `0x15E` | `FireArmRange` | 450 |
| `0x160` | `GamePlayTrigger_Fog` | 534 |
| `0x162` | `Actor_Enemy_Target_Exclusive` | 26 |
| `0x164` | `Actor_StageTarget_Exclusive` | 10 |
| `0x166` | `Religion_Box` | 262 |
| `0x168` | `Gimmick_ClawMachine` | 263 |
| `0x16A` | `Gimmick_Pinball` | 264 |
| `0x16C` | `Pin_Marker_Trace` | 2 |

Los valores `i < 37` del enum (`BackGroundTexture` … `Region_Housing`) quedan por debajo de `0xEA`,
en `cfg+0xA0…0xE8`. No los usa el árbol de actores.

---

## 9. Prueba del 7 oct (noche): las copias siguen sin icono con `+0x10A`

Build del mod **228287A2 + cambios locales** (protocolo 9), juego 1.0.0.2976. Log del invitado:
`cdmp.log` del 7 oct, noche (Shadow, 19:16–19:29). Solo
lectura: desensamblado con `cddis.py`/capstone del exe en disco. Scripts y volcados de trabajo, fuera
del repo (`d89c0.txt`, `d8d10.txt`, `dea6f0.txt`, `d9e7c60.txt`,
`d179d000.txt`, `d9e80b0.txt`, `w342.py`, `r342.py`).

### 9.1 Resumen

1. **El icono 27 no es el problema: nuestro gancho no lo ha puesto ni una vez.** El log dice
   `given our icon 0 times` en todas las líneas (8 de 8). La tarde, con `+0x112`, ponía el icono en el
   100 % de las consultas (`16558/16558`) **[L]**.
2. **Causa raíz: `keep_game_icon()` lee `Status+0x340` como `uint32_t`.** Esos 4 bytes incluyen
   `Status+0x342`, que es un **`u16` con la facción del actor**. Para cualquier NPC vale un índice de
   facción o `0xFFFF` ("sin facción"). En ambos casos no es 0, así que `keep_game_icon()` devuelve
   `true` para todas las copias y el gancho sale antes de escribir el icono **[E]**.
3. **La tarde no tenía este fallo.** El build `6B865217` (`6b86521`) es anterior a `aa4ddb5`, que es el
   commit que añadió `keep_game_icon()`. Así que el cambio `+0x112` → `+0x10A` y la regresión llegaron
   juntos, y parecía que el 27 "no se dibuja" **[E]** (historial de git).
4. **Arreglo:** leer `+0x340` como **byte**, igual que el juego (`0x9E909A`, `0x9E91C1`). `+0x341`
   (oculto) ya lo cubre `*b`. No hace falta tocar el offset, el subicono ni nada más (§9.5).
5. **Volver a `+0x112` no arregla nada** con el código actual: el filtro se aplica antes de leer la
   tabla, así que tampoco saldría el rombo.

### 9.2 Lo que dice el log [L]

```
19:16:16 Minimap: the other player's enemy copies get icon table +0x10A (key 65535) when the game gives none
19:22:19 enemy copies asked 4 times,   given our icon 0 times (the game's last answer 65535) | … 27x3536 … | table +0x10A 27 +0x10C 24 +0x110 32 +0x112 22 +0x114 28
19:23:19 asked 70   given 0 … | 27x3596
19:24:19 asked 87   given 0
19:25:19 asked 300  given 0
19:26:19 asked 523  given 0
19:27:19 asked 605  given 0
19:28:19 asked 642  given 0
19:29:19 asked 893  given 0 … | 5x2 44x1249 126x1187 27x3596 37x31 35x73
```

- La tabla se lee bien en vivo: **`+0x10A` = 27 y `+0x10C` = 24**. Así se confirma el §1.2. El
  `key 65535` de la línea de arranque es lo esperado: la tabla aún no está cargada (§6.1).
- **Última respuesta del juego para una copia: siempre `65535`.** El juego no le da icono, igual que la
  tarde. La condición `given != 0xFFFF` no es la que corta.
- **Copias:** 130 copias distintas llegaron a existir (líneas `exists`). Eran osos (`Black Bear`) y
  `Wolf Tracker`. 3 dieron `NOT in the game's actor list` y 10 murieron (`died`). Al cargar, todas
  marcaban `Status +390 0x0 +394 0x0 +340 0 | TS+3DC 0`. **Ojo:** ese `+340` lo lee
  `enemy_relay.cpp:198` como **byte**, y vale 0.
- **Muy pocas consultas:** 893 en 7 minutos para 130 copias, unas 7 por copia en toda su vida. La tarde
  hubo unas 2 700 por minuto. La explicación está en el §9.4.3: es la huella de que las copias **nunca**
  tuvieron icono.
- Histograma de los NPC nativos: 27 (enemigo) se queda en 3596 desde las 19:23, cuando la supresión
  retira los nativos de la zona. `35` = `Actor_DeadBody_Lootable_Sub` (cadáveres nativos). Ni 22, ni 28,
  ni 32.

### 9.3 Por qué no se escribe el icono: los cuatro `return` del gancho

`minimap_icon()` para una copia `0xB01Fxxxx`, tras llamar al original (`puppet_minimap.cpp:117-133`):

| Salida | Condición | ¿Se da? |
|---|---|---|
| 1 | `enemy <= 0` | No: el arranque dice `+0x10A` |
| 2 | `given != 0xFFFF` | No (o no siempre): la última respuesta siempre fue `65535` |
| 3 | `keep_game_icon(comp, b)` | **Sí, siempre** (§9.3.1) |
| 4 | `icon_from_table(enemy) == 0xFFFF` | No: la misma función, desde el tick, lee 27 a la vez. Prueba primero la raíz `0x6D69528` y no depende del hilo |

#### 9.3.1 `Status+0x342` es la facción del actor (u16) [E]

- `0x179D000` y `0x179D1F0` (los descriptores de facción de la relación `0x2215750`, §2.4) hacen
  `movzx ebx, word ptr [Status+0x342]` (`0x179D0A4`, `0x179D0E8`). Lo guardan como `desc+2` y, si la
  pila `Status+0x348` (`+0x350` = cuántas) está vacía, también como `desc+4`, la "facción efectiva".
- `0x9E7F90`, el filtro de policía del propio minimapa (`0x9E80B0`), lee el mismo `u16`
  (o la cima de `+0x348`) y lo compara con **`0xFFFF`**, que es el valor de "sin facción"
  (`0x9E80E6`). Si no es `0xFFFF`, lo busca en la tabla de facciones (`0x259CAD0`).
- Se escribe como `u16` en 10 sitios (`w342.py`). Por ejemplo, `0x17761FD` (en `0x17760B0`) asigna la
  facción a una lista de actores, y `0x214DC19`/`0x214DDC7`/`0x214DFEA` (en `0x214DB90`) guardan la
  anterior antes de cambiarla. En todo el exe hay 49 lecturas `movzx …, word ptr [R+0x342]` y solo 3
  de byte (`r342.py`).
- Consecuencia: `*(uint32_t*)(Status+0x340)` = `b340 | b341<<8 | faccion<<16`. Para un enemigo es
  `faccion<<16`, y `faccion` es un índice válido o `0xFFFF`. Solo sería 0 si la facción fuera
  justamente el índice 0. **(hipótesis, sin importancia)** Un oso o un lobo no tienen el índice 0, y
  aunque una copia lo tuviera, las demás no. El log descarta la excepción: 0 de 893.

#### 9.3.2 Cómo lee el juego esos bytes [E]

En `0x9E8D10`:

```
0x9E9078  cmp byte ptr [Status+0x341], 0 ; jne 0x9E9FE7   → oculto: noIcon=1, *b=1, HideFlagMinimapActor
0x9E909A  cmp byte ptr [Status+0x340], 0 ; je  …          → si no: noIcon=1, *icon=0xFFFF (0x9E90A3)
0x9E91C1  cmp byte ptr [Status+0x340], 0                  (otra rama, también byte)
0x9E940E  cmp byte ptr [Status+0x273], 0                  → cadáver
```

`+0x340` y `+0x341` son dos **banderas de un byte**. La idea de `keep_game_icon()` era "si está
oculto (`+0x341`) o tiene `+0x340`, déjalo". Pero el `uint32_t` se lleva también los dos bytes de la
facción. `+0x341` ya lo cubre `*b`: el camino de `0x9E9FE7` (`0x9E9FEE`) siempre pone `*b = 1`.

#### 9.3.3 Cronología [E] (`git log -- src/mod/net/puppet_minimap.cpp`)

| Commit | Qué | Prueba |
|---|---|---|
| `cac4136` | Gancho de copias: icono fijo si el juego da `0xFFFF`. Sin `keep_game_icon` | — |
| `6b86521` | (sin cambios en el minimapa) | **Tarde: build `6B865217`**, `+0x112`, rombo visible, `given = asked` |
| `aa4ddb5` | **Añade `keep_game_icon()`, con el `uint32_t` en `+0x340`** | — |
| `290a69d` | `+0x10A` por defecto y la línea de log con `+0x10A`/`+0x10C` | — |
| `228287a` (+ cambios locales) | — | **Noche**: `+0x10A`, nada visible, `given = 0` |

### 9.4 Las preguntas abiertas, una a una

#### 9.4.1 ¿Exige el 27 algún estado (hostilidad, "detectado", distancia, capa, sub-icono)? **No** [E]

Quién consume la respuesta: `0x9E7C60` (actualización del componente de icono) llama a `0x9E89C0`
(`0x9E7E3A`) **solo si `comp+0x34 & 6`**. Después pasa el resultado `(icon, sub, a, b)` a
`0x9EA6F0` (`0x9E7E77`). En `0x9EA6F0`:

- `icon == 0xFFFF` → **quita** el icono: borra el bit 1 de `comp+0x34` (`0x9EA940`) y sale
  (`0x9EB76E`).
- Si el icono viejo era `0xFFFF` y el nuevo no (`0x9EAF48`–`0x9EB097`) → pone el bit 1 de `comp+0x34`
  (`0x9EAF77`), llama a `0x4FF1F0` y **crea el icono en la UI** con
  `0x5C2010([[0x6D69190]+0x80]+0x620, icon, actorId, nombre, …)`. Es el mismo camino para cualquier
  clave: no mira la familia enemigo, ni la hostilidad, ni la distancia, ni el subicono.
- Si el icono cambia, lo cambia por el mismo camino (`0x9EC330`, `0x4317A0`).
- La familia enemigo (`cfg+0x10A/0x10C/0x10E`, `0x9EAEC9`–`0x9EB75F`) solo decide **efectos
  añadidos**, `RemoveDetectEffect`/`CreateDetectEffect`, y solo con el flag global
  `byte [exe+0x6CC18E8]`. No bloquea la creación del icono (§5).
- `CharacterInfo+0x1E8`/`0x9E9700` → `0x9E9FDB` solo pone `noIcon = 1`, sin `*b`. Eso impide que el
  **juego** promocione el punto (`0x9E8BF1`), pero no impide que lo pongamos nosotros:
  `keep_game_icon()` no mira `noIcon`.
- El post-proceso `0x9E8A70`–`0x9E8C4A` es del original. Nuestro gancho escribe **después** de él, así
  que lo que decida (o no decida) da igual.

Los NPC nativos reciben exactamente el 27 por este mismo camino (`27x3596`) y se ven como punto rojo.
**(hipótesis fuerte)** La copia con 27 se dibujará igual.

#### 9.4.2 ¿Se ignora el valor del icono principal para la clave 27? ¿Hay que ponerlo como subicono? **No y no** [E]

- `0x9EA6F0` crea el icono a partir del **principal** (`r15w`, `0x9EB08F`). El subicono (`r12w`)
  solo cuenta para:
  - el cambio de icono (`0x9EBF90`, a través de `+0x648`);
  - las marcas de la familia, para el efecto de detección (`0x9EB4C1`).
- Con `+0x112` y `sub = 0xFFFF` se veía el rombo, y el 27 entra por el mismo sitio.
- Un nativo hostil queda con `icon = 27` y `sub = 27` (§2.3). **(hipótesis)** Copiar también
  `sub = 27` solo serviría para el `DetectEffect` y la flecha de borde. No hace falta para ver el punto.
  Recomiendo dejar `sub = 0xFFFF` en el primer arreglo, con menos superficie.

#### 9.4.3 Por qué hubo tan pocas consultas [E]

`0x9E7C60` solo recalcula si `comp+0x34 & 6`. Cuando `0x9EA6F0` recibe `0xFFFF`, borra el bit 1
(`0x9EA940`). Sin icono, la copia deja de recalcularse periódicamente y solo vuelve a consultarse
cuando algo pone el bit 2: el alta, la muerte y otros eventos. **(hipótesis)** Unos 7 por copia.

- La tarde, cada copia tenía el 22 y el bit 1 puesto, así que se consultaba unas 2 veces por segundo,
  como el caballo (`44x…`, unos 110 por minuto).
- **Implicación para el arreglo:** basta con acertar en la **primera** consulta, la del alta. Con eso
  el bit 1 queda puesto y las consultas siguen solas.

### 9.5 Arreglo concreto (para `src/mod/net/puppet_minimap.cpp`; no aplicado)

```cpp
// A copy the game would hide (*b: 0x9E9FE7, which also covers Status+0x341),
// a dead one (+0x273) or one with Status+0x340 set: its icon stays the game's.
// +0x340/+0x341 are byte flags (0x9E909A/0x9E9078); +0x342 is the actor's
// u16 faction (0x179D0A4, 0xFFFF = none), so never read them as one u32.
bool keep_game_icon(const void* comp, const uint8_t* b) {
    __try {
        if (*b != 0) return true;
        const auto actor = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(comp) + 0x08);
        const auto comps = actor ? *reinterpret_cast<const uintptr_t*>(actor + 0x68) : 0;
        const auto status = comps ? *reinterpret_cast<const uintptr_t*>(comps + 0x20) : 0;
        if (!status) return true;
        return *reinterpret_cast<const uint8_t*>(status + 0x273) != 0 ||
               *reinterpret_cast<const uint8_t*>(status + 0x340) != 0;   // was uint32_t
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
```

- Lo demás no cambia: offset `+0x10A`, `*icon = key`, `*sub = 0xFFFF`, `*a = 0`, `*b = 0`.
- **Diagnóstico** (recomendado en el mismo cambio, solo contadores atómicos):
  - contar por qué se conserva el icono del juego: `kept_b`, `kept_dead`, `kept_340` y
    `kept_nostatus`;
  - añadirlo a la línea de cada minuto, por ejemplo
    `kept: hidden 0 dead 12 +340 0 no-status 0`;
  - guardar también `*sub` y `*a` de la última respuesta del juego, para resolver de paso el §4 (a, b
    o c).

  Así, si vuelve a salir `given 0`, el log dirá cuál de las salidas lo corta.
- Riesgo: ninguno nuevo. Es la misma escritura que hacía la tarde, solo que ahora sí se respetan los
  casos de oculto, muerto y `+0x340`. Las lecturas siguen dentro de `__try`.
- Ya no hay que reiniciar con un `.ini` especial: `config.h:109` ya trae `0x10A` por defecto.

### 9.6 Plan B (si con el arreglo sigue sin verse)

Se decide con los contadores del §9.5:

| Lo que diga el log | Interpretación | Qué hacer |
|---|---|---|
| `given ≈ asked` y aun así no hay punto | **(hipótesis improbable)** La UI no dibuja el 27 para la copia (por ejemplo, por la clase `icon-enemy-order` del `.thtml`) | Volver a `enemy_minimap_icon=0x112` en el `.ini` (rombo, ya probado), o usar `+0x10C` (élite, 24, `cd_icon_map_enemy_elite`, rojo de 16 px). Las dos claves entran por el mismo `0x5C2010` |
| `kept: hidden N` alto | El juego da `*b = 1` a las copias en el alta: congelada (`Sequencer+0x3DC`), `+0x341`, `+0x390/+0x394 ≥ 0x80` o sin el bit `0x4000000` de la IA (`0x9E8E37`) | Respetar `*b` **solo** si `Status+0x341 != 0` (oculta de verdad) y si no, poner el icono como hacía la tarde, que se veía. Hay que tener en cuenta que el juego ya habrá lanzado `HideFlagMinimapActor` (`0x3D2460`); la tarde eso no impidió el rombo |
| `kept: +340 N` alto | Las copias tienen la bandera `+0x340` puesta en vivo (en el alta valía 0) | Averiguar quién la pone. Mientras tanto, ignorarla para las copias vivas |
| `asked` sigue muy bajo y `given` = `asked` | Bien: el bit 1 tarda en ponerse en las copias que ya existían | Nada: las copias nuevas reciben el icono en el alta |

### 9.7 Pruebas

| # | Dónde | Qué mirar |
|---|---|---|
| N1 | Loopback (`cdmp_peer` + juego en el PC del anfitrión) | En el log, `given our icon N times` con `N ≈ asked` y `asked` subiendo unas 2 veces por segundo por copia. Captura `CopyFromScreen` del minimapa con un punto rojo pequeño por copia |
| N2 | Shadow como invitado | Lo mismo, con osos o bandidos. Al morir una copia, el icono pasa a cadáver (`given != 0xFFFF`, `kept_dead`) y no queda un punto rojo fijo |
| N3 | Regresión | El compañero sigue con su icono (`other player shown with icon 23`), sin cambios |

No hace falta copia de la partida: no se escribe nada nuevo en la memoria del juego.

### 9.8 Correcciones a las secciones anteriores

- §2.1: "`keep_game_icon()` hace bien en mirar `*b`" sigue siendo cierto. Pero la **lectura de
  `+0x340` como u32** del mismo `keep_game_icon()` es el fallo de esta prueba.
- §6.2.3: "no tocar si `keep_game_icon()`" vale solo con la lectura de un byte del §9.5.
- §1.2: la clave de `+0x10A` (27) y la de `+0x10C` (24) quedan **confirmadas en vivo**.

### 9.9 Resumen corto

1. Con `+0x10A`, la copia no recibió el 27: `given our icon 0 times` (0/893). El icono no "deja de
   dibujarse"; nunca se escribió.
2. Causa: `keep_game_icon()` lee `*(uint32_t*)(Status+0x340)`. `+0x342` es la facción u16 del actor
   (`0x179D0A4`; `0xFFFF` = ninguna), así que el valor nunca es 0 y el gancho siempre deja el `0xFFFF`
   del juego.
3. La tarde (`6b86521`) aún no tenía `keep_game_icon()` (llegó en `aa4ddb5`). Por eso `+0x112` sí se
   veía.
4. El juego lee `+0x340` y `+0x341` como **bytes** (`0x9E909A`, `0x9E9078`). `+0x341` ya va en `*b`.
5. Arreglo: `uint32_t` → `uint8_t` en `+0x340` (`puppet_minimap.cpp:54`). No hace falta nada más.
6. `0x9EA6F0` crea el icono con la clave principal para cualquier valor (`0x5C2010`). El 27 no necesita
   hostilidad, "detectado", distancia ni subicono. La familia enemigo solo añade `DetectEffect` (con el
   flag `0x6CC18E8`).
7. `CharacterInfo+0x1E8`/`0x9E9700` solo pone `noIcon` (impide que el juego promocione) y no afecta al
   gancho.
8. Las pocas consultas (unas 7 por copia) se deben a que `0xFFFF` borra el bit 1 de `comp+0x34`
   (`0x9EA940`) y se detiene el recálculo periódico. Con icono, `0x9EAF77` lo vuelve a poner.
9. `sub = 0xFFFF` basta (el rombo se vio así). `sub = 27` sería solo para la flecha o el efecto de
   aggro, más adelante.
10. Volver a `+0x112` sin el arreglo **tampoco** mostraría nada: el filtro va antes de la tabla.
11. Diagnóstico recomendado: contadores de cada motivo para conservar el icono del juego, y `sub`/`a` en
    la línea de cada minuto.
12. Plan B (si `given ≈ asked` y no se ve): `.ini` `enemy_minimap_icon=0x112` (rombo) o `0x10C` (élite,
    rojo).
13. Plan B (si domina `hidden`): respetar `*b` solo con `Status+0x341`, como la tarde.
